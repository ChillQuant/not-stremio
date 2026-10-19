// Local search-as-you-type index. See search_index.hpp.

#include "search_index.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace sx
{

namespace
{

// UTF-8 safe word scan: bytes >= 0x80 stay glued to their word (Thai, CJK,
// etc.), ASCII splits on non-alphanumerics.
bool word_char(unsigned char c)
{
    return (c & 0x80) != 0 || std::isalnum(c) != 0;
}

std::vector<std::string> words_of(const std::string &lowered)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < lowered.size())
    {
        while (i < lowered.size() && !word_char(static_cast<unsigned char>(lowered[i])))
            ++i;
        std::size_t e = i;
        while (e < lowered.size() && word_char(static_cast<unsigned char>(lowered[e])))
            ++e;
        if (e > i)
            out.push_back(lowered.substr(i, e - i));
        i = e;
    }
    return out;
}

std::string item_uid(const Item &it)
{
    std::string k = it.type;
    k.push_back('\x01');
    k += it.id;
    return k;
}

int hit_score(const SearchHit &h, const std::string &ql)
{
    int s;
    if (h.name_lc == ql)
        s = 300;
    else if (starts_with(h.name_lc, ql))
        s = 200;
    else
    {
        bool wp = false;
        for (const std::string &w : h.words)
            if (starts_with(w, ql))
            {
                wp = true;
                break;
            }
        s = wp ? 100 : 0;
    }
    if (h.in_library || h.in_continue)
        s += 50;
    // Shorter names first among equals: the exact title usually wins.
    s -= static_cast<int>(std::min(h.name_lc.size(), std::size_t(40)));
    return s;
}

} // namespace

void SearchIndex::build(Snapshot snap)
{
    hits_.clear();
    postings_.clear();
    hits_.reserve(512);
    postings_.reserve(8192);

    std::vector<std::pair<std::vector<Item> *, int>> sources;
    sources.emplace_back(&snap.library, 0);
    sources.emplace_back(&snap.continue_watching, 1);
    sources.emplace_back(&snap.discover, 2);
    sources.emplace_back(&snap.meta, 3);
    sources.emplace_back(&snap.recent, 4);

    std::set<std::string> seen; // (type,id) already indexed: first (highest-priority) source wins
    for (const auto &src : sources)
    {
        for (const Item &it : *src.first)
        {
            if (hits_.size() >= kMaxTitles)
                break;
            if (it.id.empty() || it.name.empty())
                continue;
            if (!seen.insert(item_uid(it)).second)
                continue;
            SearchHit h;
            h.item = it;
            h.name_lc = lower(it.name);
            h.words = words_of(h.name_lc);
            h.in_library = (src.second == 0);
            h.in_continue = (src.second == 1);
            hits_.push_back(std::move(h));
        }
    }

    for (std::uint32_t i = 0; i < hits_.size() && postings_.size() < kMaxPostings; ++i)
    {
        for (const std::string &w : hits_[i].words)
        {
            // Prefixes from length 2: single letters match too much to be useful.
            const std::size_t top = std::min(w.size(), std::size_t(16));
            for (std::size_t len = 2; len <= top && postings_.size() < kMaxPostings; ++len)
                postings_.emplace_back(w.substr(0, len), i);
        }
    }
    std::sort(postings_.begin(), postings_.end(),
              [](const auto &a, const auto &b) { return a.first < b.first || (a.first == b.first && a.second < b.second); });
    postings_.erase(std::unique(postings_.begin(), postings_.end()), postings_.end());
    // Drop the build-time over-reserve: the index lives for the whole session.
    hits_.shrink_to_fit();
    postings_.shrink_to_fit();
}

std::vector<std::size_t> SearchIndex::lookup(const std::string &query, std::size_t limit) const
{
    std::vector<std::size_t> out;
    if (hits_.empty() || limit == 0)
        return out;
    const std::string ql = lower(trim(query));
    if (ql.empty())
        return out;

    if (ql.size() < 2)
    {
        // First keystroke: a linear scan over titles is still microseconds.
        for (std::size_t i = 0; i < hits_.size() && out.size() < limit; ++i)
            if (starts_with(hits_[i].name_lc, ql))
                out.push_back(i);
        return out;
    }

    const std::vector<std::string> qwords = words_of(ql);
    if (qwords.empty())
        return out;
    // As-you-type: the last word is the partial prefix being typed; earlier
    // words must each prefix-match some title word.
    const std::string &tail = qwords.back();
    std::vector<std::size_t> cand;
    for (auto it = std::lower_bound(postings_.begin(), postings_.end(), tail,
                                    [](const std::pair<std::string, std::uint32_t> &p,
                                       const std::string &v) { return p.first < v; });
         it != postings_.end() && it->first == tail; ++it)
        cand.push_back(it->second);
    if (cand.empty())
        return out;
    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());

    struct Scored
    {
        int score;
        std::size_t idx;
    };
    std::vector<Scored> scored;
    scored.reserve(cand.size());
    for (std::size_t i : cand)
    {
        const SearchHit &h = hits_[i];
        bool ok = true;
        for (std::size_t w = 0; w + 1 < qwords.size(); ++w)
        {
            bool any = false;
            for (const std::string &t : h.words)
                if (starts_with(t, qwords[w]))
                {
                    any = true;
                    break;
                }
            if (!any)
            {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;
        scored.push_back({hit_score(h, ql), i});
    }
    std::sort(scored.begin(), scored.end(),
              [](const Scored &a, const Scored &b) { return a.score > b.score; });
    for (const Scored &s : scored)
    {
        out.push_back(s.idx);
        if (out.size() >= limit)
            break;
    }
    return out;
}

std::size_t SearchIndex::bytes() const
{
    std::size_t n = sizeof(*this);
    n += hits_.capacity() * sizeof(SearchHit);
    for (const SearchHit &h : hits_)
    {
        const Item &it = h.item;
        n += it.id.capacity() + it.type.capacity() + it.name.capacity() + it.poster.capacity() +
             it.background.capacity() + it.logo.capacity() + it.description.capacity() +
             it.release_info.capacity() + it.runtime.capacity() + it.imdb_rating.capacity() +
             it.video_id.capacity();
        for (const std::string &g : it.genres)
            n += g.capacity();
        n += h.name_lc.capacity();
        n += h.words.capacity() * sizeof(std::string);
        for (const std::string &w : h.words)
            n += w.capacity();
    }
    n += postings_.capacity() * sizeof(postings_[0]);
    for (const auto &p : postings_)
        n += p.first.capacity();
    return n;
}

} // namespace sx
