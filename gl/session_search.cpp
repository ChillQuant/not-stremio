// Typing with the console's keyboard, and searching with it.
//
// SEARCH UX (worker C, zero-wait): the search page opens with content
// (trending from the Discover cache) on frame 1, a submitted query shows
// on-device matches instantly from a background-built prefix index (with a
// synchronous title-scan fallback when the index is not ready yet), and the
// network fan-out launches immediately with the previous in-flight request
// truly cancelled. No spinners anywhere: stale/local results stay on screen
// until fresh ones arrive.

#include "session.hpp"

#include "search_index.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace sx
{

namespace
{

constexpr std::size_t kMaxRowCards = 50;
constexpr std::size_t kMaxLocalCards = 6; // instant on-device matches per keystroke/submit
constexpr std::size_t kTrendingCards = 12;
// No network debounce: every start_search() caller passes a fully-completed
// query (IME submit/done, a recent-search pick, or a detail-page person
// name), so the network fan-out launches immediately. (Zero-wait, worker C.)
constexpr double kRecentTtlS = 30.0 * 24.0 * 3600.0; // recent searches expire after 30 days
constexpr std::size_t kRecentMax = 5; // exactly 5 recent searches kept

std::string row_title(const Catalog &c)
{
    const std::string type = capitalize(c.type);
    return c.name.empty() ? type : c.name + " - " + type;
}

std::string item_key(const Item &it)
{
    std::string k = it.type;
    k.push_back(':');
    k += it.id;
    return k;
}

double rating_of(const Item &it)
{
    if (it.imdb_rating.empty())
        return -1.0;
    char *end = nullptr;
    const double v = std::strtod(it.imdb_rating.c_str(), &end);
    return (end != it.imdb_rating.c_str()) ? v : -1.0;
}

// True when any word of `lowered` starts with the lowercased query word `qw`.
// UTF-8 safe: bytes >= 0x80 stay glued to their word.
bool any_word_prefix(const std::string &lowered, const std::string &qw)
{
    std::size_t i = 0;
    const std::size_t n = lowered.size();
    while (i < n)
    {
        const unsigned char c = static_cast<unsigned char>(lowered[i]);
        if (c < 0x80 && !std::isalnum(c))
        {
            ++i;
            continue;
        }
        std::size_t e = i;
        while (e < n)
        {
            const unsigned char d = static_cast<unsigned char>(lowered[e]);
            if (d < 0x80 && !std::isalnum(d))
                break;
            ++e;
        }
        if (starts_with(lowered.substr(i, e - i), qw))
            return true;
        i = e;
    }
    return false;
}

} // namespace

void Session::ask_text(const std::function<void(const std::string &)> &done)
{
    note_input();
    text_done_ = done;
    // Type from the phone: start the LAN listener alongside the OS keyboard.
    // The OS dialog stays the primary input and the fallback if the listener
    // won't start (no LAN, ports busy).
    phone_ime_.start();
    if (!ime_.open())
    {
        text_done_ = nullptr;
        phone_ime_.stop();
        say("The keyboard could not be opened.", 4.0, true);
    }
}

void Session::poll_typing()
{
    // Phone text first: it arrives without the OS dialog.
    if (text_done_)
    {
        std::string ptext;
        if (phone_ime_.poll(&ptext))
        {
            const auto fn = text_done_;
            text_done_ = nullptr;
            phone_ime_.stop();
            ime_.close();
            if (fn)
                fn(ptext);
            return;
        }
    }
    if (!ime_.active())
        return;
    std::string text;
    const Ime::State s = ime_.poll(&text);
    if (s == Ime::State::done)
    {
        const auto fn = text_done_;
        text_done_ = nullptr;
        phone_ime_.stop();
        if (fn)
            fn(text);
    }
    else if (s == Ime::State::cancelled)
    {
        text_done_ = nullptr;
        phone_ime_.stop();
    }
}

std::string Session::phone_ime_url() const
{
    return phone_ime_.running() ? phone_ime_.url() : std::string();
}

std::uint32_t Session::qr_texture(const std::string &url) const
{
    return tex_.get(url, ArtKind::qr);
}

// ---- recent searches: exactly 5, 30-day expiry, persisted ----

std::vector<std::string> Session::recent_search_queries()
{
    auto &rs = prefs_.recent_searches;
    const double now = static_cast<double>(now_epoch_ms()) / 1000.0;
    bool pruned = false;
    rs.erase(std::remove_if(rs.begin(), rs.end(),
                            [&](const Prefs::RecentSearch &e) {
                                const bool old = now - e.at > kRecentTtlS;
                                pruned = pruned || old;
                                return old;
                            }),
             rs.end());
    if (pruned)
        save_prefs();
    std::vector<std::string> out;
    out.reserve(rs.size());
    for (const Prefs::RecentSearch &e : rs)
        out.push_back(e.query);
    return out;
}

void Session::recent_search_add(const std::string &q)
{
    auto &rs = prefs_.recent_searches;
    rs.erase(std::remove_if(rs.begin(), rs.end(), [&](const Prefs::RecentSearch &e) { return e.query == q; }),
             rs.end());
    const double now = static_cast<double>(now_epoch_ms()) / 1000.0;
    rs.insert(rs.begin(), Prefs::RecentSearch{q, now});
    rs.erase(std::remove_if(rs.begin(), rs.end(), [&](const Prefs::RecentSearch &e) { return now - e.at > kRecentTtlS; }),
             rs.end());
    if (rs.size() > kRecentMax)
        rs.resize(kRecentMax);
    save_prefs();
}

void Session::recent_search_clear()
{
    prefs_.recent_searches.clear();
    save_prefs();
}

// ---- local prefix index: built on a worker, swapped in on the UI thread ----

void Session::search_index_ensure()
{
    // Fingerprint the on-device sources; rebuild on the pool when they change.
    // Reading the sizes is O(1); the snapshot copies below are a few hundred
    // small structs at most, never on the critical path of a keystroke.
    std::uint64_t fp = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) { fp ^= v + 0x9e3779b97f4a7c15ull + (fp << 6) + (fp >> 2); };
    mix(static_cast<std::uint64_t>(lib_items_.size()));
    mix(static_cast<std::uint64_t>(disc_items_.size()));
    mix(static_cast<std::uint64_t>(meta_cache_.size()));
    mix(static_cast<std::uint64_t>(progress_.size()));
    if (fp == search_index_fp_ || search_index_building_)
        return;
    search_index_building_ = true;
    SearchIndex::Snapshot snap;
    snap.library = lib_items_;
    snap.continue_watching = continue_watching();
    snap.discover = disc_items_;
    snap.meta.reserve(meta_cache_.size());
    for (const auto &kv : meta_cache_)
        snap.meta.push_back(kv.second);
    for (const auto &kv : progress_)
    {
        const Progress &p = kv.second;
        if (p.name.empty())
            continue;
        Item it;
        it.id = kv.first;
        it.type = p.type;
        it.name = p.name;
        it.poster = p.poster;
        snap.recent.push_back(std::move(it));
    }
    bg_urgent<std::shared_ptr<SearchIndex>>(
        [snap = std::move(snap)]() mutable {
            auto idx = std::make_shared<SearchIndex>();
            idx->build(std::move(snap));
            return idx;
        },
        [this, fp](std::shared_ptr<SearchIndex> &idx) {
            search_index_building_ = false;
            search_index_ = idx;
            search_index_fp_ = fp;
            dlog("search index: %zu titles, %zu postings, ~%zu KB", idx->titles(), idx->postings(),
                 idx->bytes() / 1024);
        });
}

// ---- the search page opens with content: never an empty page ----

void Session::search_show_trending()
{
    search.rows.clear();
    search_map_.clear();
    search_local_have_ = false;
    search_local_items_.clear();
    search.status.clear();
    if (!disc_items_.empty())
    {
        // Trending/popular from the Discover cache, best-rated first.
        // Artwork resolves through the existing texture cache.
        std::vector<const Item *> top;
        top.reserve(disc_items_.size());
        for (const Item &it : disc_items_)
            if (!it.id.empty() && !it.name.empty())
                top.push_back(&it);
        std::stable_sort(top.begin(), top.end(),
                         [](const Item *a, const Item *b) { return rating_of(*a) > rating_of(*b); });
        if (!top.empty())
        {
            Row row;
            row.key = "search:trending";
            row.title = "Trending";
            for (const Item *it : top)
            {
                row.items.push_back(make_title(*it, false));
                search_local_items_.push_back(*it);
                if (row.items.size() >= kTrendingCards)
                    break;
            }
            search.rows.push_back(std::move(row));
        }
    }
    if (search.rows.empty() && !lib_items_.empty())
    {
        // Discover never opened: fall back to the library so the page still
        // shows something with artwork.
        Row row;
        row.key = "search:library";
        row.title = "From your library";
        for (const Item &it : lib_items_)
        {
            if (it.id.empty() || it.name.empty())
                continue;
            row.items.push_back(make_title(it, true));
            search_local_items_.push_back(it);
            if (row.items.size() >= kTrendingCards)
                break;
        }
        if (!row.items.empty())
            search.rows.push_back(std::move(row));
    }
    search_local_have_ = !search_local_items_.empty();
    if (search.rows.empty())
        search.status = "Type to search across your addons.";
    art_dirty_ = true;
}

void Session::begin_search()
{
    search_index_ensure();
    // Open the search page immediately with content already visible; the
    // dropdown (history) or keyboard opens over it.
    search_open = true;
    search_query.clear();
    ++search_gen_;
    if (search_cancel_)
        search_cancel_->store(true);
    search_cancel_.reset();
    ++search_debounce_gen_;
    search_.clear();
    search_map_.clear();
    search_show_trending();
    changed_ |= kSearch | kSearchNew;
    const std::vector<std::string> recents = recent_search_queries();
    if (recents.empty())
    {
        ask_text([this](const std::string &q) { start_search(q); });
        return;
    }
    std::vector<std::string> opts;
    opts.push_back("New search...");
    for (const std::string &q : recents)
        opts.push_back(q);
    opts.push_back("Clear recent searches");
    open_dropdown("Search", opts, 0, [this, opts](int i) {
        if (i == 0)
        {
            ask_text([this](const std::string &q) { start_search(q); });
        }
        else if (i == static_cast<int>(opts.size()) - 1)
        {
            recent_search_clear();
            say("Recent searches cleared");
            search_show_trending(); // the page is already open; refresh it
            changed_ |= kSearch;
        }
        else if (i > 0)
        {
            const std::vector<std::string> r = recent_search_queries();
            if (i - 1 < static_cast<int>(r.size()))
                start_search(r[static_cast<std::size_t>(i - 1)]);
        }
    });
}

void Session::close_search()
{
    ++search_gen_;
    ++search_debounce_gen_; // (legacy counter; no debounce path remains)
    if (search_cancel_)
        search_cancel_->store(true); // truly cancel the in-flight HTTP
    search_cancel_.reset();
    search_open = false;
    search.rows.clear();
    search.status.clear();
    search_.clear();
    search_map_.clear();
    search_local_have_ = false;
    changed_ |= kSearch | kSearchNew;
}

// ---- instant on-device matches, no network ----

void Session::search_local_results(const std::string &q)
{
    search_local_have_ = false;
    search_local_items_.clear();
    std::vector<Item> hits;
    const std::shared_ptr<const SearchIndex> idx = search_index_;
    if (idx && !idx->empty())
    {
        const std::vector<std::size_t> hs = idx->lookup(q, kMaxLocalCards);
        hits.reserve(hs.size());
        for (std::size_t h : hs)
            hits.push_back(idx->at(h).item);
    }
    else
    {
        // Zero-wait fallback: the index is not built yet (it builds on the
        // urgent lane via search_index_ensure()). Scan the on-device sources
        // directly for case-insensitive substring title matches so the
        // "On this device" row still appears in 0ms. Title field is Item::name.
        const std::string ql = lower(q);
        std::unordered_set<std::string> seen;
        auto consider = [&](const Item &it) {
            if (hits.size() >= kMaxLocalCards || it.id.empty() || it.name.empty())
                return;
            if (!seen.insert(item_key(it)).second)
                return;
            if (lower(it.name).find(ql) == std::string::npos)
                return;
            hits.push_back(it);
        };
        for (const Item &it : lib_items_)
            consider(it);
        for (const Item &it : disc_items_)
            consider(it);
        for (const BoardRow &br : board_)
            for (const Item &it : br.items)
                consider(it);
    }
    if (hits.empty())
        return;
    Row row;
    row.key = "search:local";
    row.title = "On this device";
    for (const Item &hit : hits)
    {
        search_local_items_.push_back(hit);
        row.items.push_back(make_title(hit, true));
    }
    search_local_row_ = std::move(row);
    search_local_have_ = true;
    search.rows.clear();
    search.rows.push_back(search_local_row_);
    search.status.clear();
    art_dirty_ = true;
}

// ---- smart ranking: exact/prefix title, then owned, then rest ----

int Session::search_rank_score(const Item &it, const std::string &ql,
                               const std::unordered_set<std::string> &owned)
{
    const std::string nl = lower(it.name);
    int s;
    if (nl == ql)
        s = 100;
    else if (starts_with(nl, ql))
        s = 80;
    else
    {
        // Any query word prefix-matching any title word (UTF-8 safe scan).
        bool wp = false;
        std::size_t qi = 0;
        while (qi < ql.size() && !wp)
        {
            const unsigned char c = static_cast<unsigned char>(ql[qi]);
            if (c < 0x80 && !std::isalnum(c))
            {
                ++qi;
                continue;
            }
            std::size_t qe = qi;
            while (qe < ql.size())
            {
                const unsigned char d = static_cast<unsigned char>(ql[qe]);
                if (d < 0x80 && !std::isalnum(d))
                    break;
                ++qe;
            }
            if (qe > qi && any_word_prefix(nl, ql.substr(qi, qe - qi)))
                wp = true;
            qi = qe;
        }
        if (wp)
            s = 60;
        else if (nl.find(ql) != std::string::npos)
            s = 40;
        else
            s = 10;
    }
    // (2) in the Library or Continue Watching: hash-set lookups, O(1).
    if (owned.find(item_key(it)) != owned.end())
        s += 25;
    return s;
}

void Session::start_search(const std::string &text)
{
    const std::string q = trim(text);
    if (q.empty())
        return;
    recent_search_add(q);
    search_index_ensure();
    const int gen = ++search_gen_;
    ++search_debounce_gen_;
    // Cancel the previous in-flight network search: the HTTP layer aborts,
    // so a fast typist costs one fan-out, not one per keystroke.
    if (search_cancel_)
        search_cancel_->store(true);
    const auto cancel = std::make_shared<std::atomic<bool>>(false);
    search_cancel_ = cancel;
    search_query = q;
    search_open = true;
    search_.clear();
    search_map_.clear();
    search_local_have_ = false;
    search.rows.clear();
    for (const auto &a : addons_)
    {
        if (addon_disabled(*a))
            continue;
        for (const auto &c : a->catalogs)
        {
            if (!c.has_extra("search") || c.requires_other_than("search"))
                continue;
            BoardRow row;
            row.key = "search|" + a->transport_url + "|" + c.type + "|" + c.id;
            row.title = row_title(c);
            row.addon_url = a->transport_url;
            row.catalog = c;
            search_.push_back(row);
        }
    }
    // Instant: on-device matches from the local prefix index, no network.
    search_local_results(q);
    changed_ |= kSearch | kSearchNew;
    if (search_.empty())
    {
        search.status = addons_loading_ ? "Addons are still loading..." : "None of your addons can search.";
        changed_ |= kSearch;
        return;
    }
    // Zero-wait: every caller passes a fully-completed query (IME submit/done,
    // a recent-search pick, or a detail-page person name), so the network
    // fan-out launches immediately. The previous in-flight request was
    // already cancelled above; gen guards against stale arrivals.
    search_launch_network(q, gen, cancel);
}

void Session::search_launch_network(const std::string &q, int gen,
                                     const std::shared_ptr<std::atomic<bool>> &cancel)
{
    search_pending_ = static_cast<int>(search_.size());
    // Never a spinner: local/stale results stay on screen and swap when ready.
    if (search.rows.empty())
        search.status = "Searching...";
    changed_ |= kSearch;
    // Owned-set for ranking, built once per search: library + continue watching.
    search_owned_.clear();
    for (const Item &it : lib_items_)
        search_owned_.insert(item_key(it));
    for (const Item &it : continue_watching())
        search_owned_.insert(item_key(it));
    for (std::size_t i = 0; i < search_.size(); ++i)
    {
        const ::Addon *a = find_addon(search_[i].addon_url);
        const std::string url =
            a ? a->resource_url("catalog", search_[i].catalog.type, search_[i].catalog.id, "search=" + url_encode(q)) : "";
        struct Res
        {
            std::vector<Item> items;
            std::string error;
        };
        bg_urgent<Res>(
            [url, cancel]() {
                Res r;
                json j;
                if (!cancel->load() && !url.empty() && fetch_json(url, j, r.error, 5, cancel.get()))
                {
                    const json &metas = jobj(j, "metas");
                    if (metas.is_array())
                        for (const auto &m : metas)
                        {
                            if (cancel->load())
                                break;
                            Item it = item_from_meta_json(m);
                            if (!it.id.empty() && !it.name.empty())
                                r.items.push_back(it);
                        }
                }
                return r;
            },
            [this, gen, i, cancel](Res &r) {
                if (gen != search_gen_ || cancel->load() || i >= search_.size())
                    return;
                search_[i].loaded = true;
                search_[i].items = std::move(r.items);
                if (search_[i].items.size() > kMaxRowCards)
                    search_[i].items.resize(kMaxRowCards);
                --search_pending_;
                refresh_search();
            });
    }
}

void Session::refresh_search()
{
    // Stow the previous rows so this refresh can reuse their GPU texture
    // handles and laid-out text (see the match loop below), mirroring
    // refresh_board()'s use-once-move pattern. Reused once per title; the
    // vector is gone by the time we return.
    std::vector<Row> prev_rows = std::move(search.rows);
    search.rows.clear();
    search_map_.clear();
    if (search_local_have_)
        search.rows.push_back(search_local_row_);
    const std::string ql = lower(search_query);
    // Dedupe against the local row: it already showed these titles.
    std::unordered_set<std::string> seen;
    if (search_local_have_)
        for (const Title &t : search_local_row_.items)
            seen.insert(t.type + ":" + t.id);
    for (std::size_t i = 0; i < search_.size(); ++i)
    {
        BoardRow &br = search_[i];
        if (!br.loaded || br.items.empty())
            continue;
        // The index this row will occupy once pushed: the cursor-stability
        // pin below needs it before the push happens.
        const int target_row = static_cast<int>(search.rows.size());
        // Cursor-stability head: the previous rendering's first
        // (focus col + 4) title ids for this row, in their existing order.
        // Captured BEFORE the reuse loop below consumes (clears) pt.id.
        std::vector<std::string> pin_head;
        if (target_row == last_cursor_.search_row && last_cursor_.search_col > 0)
        {
            const int want_pin = last_cursor_.search_col + 4;
            for (Row &pr : prev_rows)
            {
                if (pr.key != br.key)
                    continue;
                for (const Title &pt : pr.items)
                {
                    if (!pt.id.empty() && static_cast<int>(pin_head.size()) < want_pin)
                        pin_head.push_back(pt.id);
                    if (static_cast<int>(pin_head.size()) >= want_pin)
                        break;
                }
                break;
            }
        }
        // Microsecond sort over tens of items: exact/prefix title match,
        // then library/continue-watching, then the rest; rating tiebreak.
        std::stable_sort(br.items.begin(), br.items.end(), [&](const Item &a, const Item &b) {
            const int sa = search_rank_score(a, ql, search_owned_);
            const int sb = search_rank_score(b, ql, search_owned_);
            if (sa != sb)
                return sa > sb;
            return rating_of(a) > rating_of(b);
        });
        Row row;
        row.key = br.key;
        row.title = br.title;
        for (const Item &it : br.items)
        {
            if (!seen.insert(item_key(it)).second)
                continue;
            Title nt = make_title(it, false);
            // Reuse the previous rows' Title state (GPU texture handles and
            // the text cache) when the same title survives the refresh, so
            // the first repaint doesn't re-upload artwork or re-lay-out
            // text. Matches by row key, then by title id.
            for (Row &pr : prev_rows)
            {
                if (pr.key != br.key)
                    continue;
                for (Title &pt : pr.items)
                {
                    if (pt.id == nt.id && !pt.id.empty())
                    {
                        nt.poster = pt.poster;
                        nt.backdrop = pt.backdrop;
                        nt.logo = pt.logo;
                        nt.logo_w = pt.logo_w;
                        nt.logo_h = pt.logo_h;
                        nt.poster_fade = pt.poster_fade;
                        nt.accent = pt.accent;
                        nt.text_cache_ = std::move(pt.text_cache_);
                        pt.id.clear(); // consume once: prevents any use-after-move on duplicate catalog IDs
                        break;
                    }
                }
                break;
            }
            row.items.push_back(std::move(nt));
            if (row.items.size() >= kMaxRowCards)
                break;
        }
        if (row.items.empty())
            continue;
        // Cursor stability: newly arriving addon rows must not shift the card
        // under the cursor. When this is the focused row, pin the previous
        // rendering's first (focus col + 4) titles at the head of the new
        // row, in their existing order; arrivals still append after them.
        int pinned = 0;
        for (const std::string &pid : pin_head)
        {
            auto it2 = std::find_if(row.items.begin() + pinned, row.items.end(),
                                    [&](const Title &t) { return t.id == pid; });
            if (it2 == row.items.end())
                continue;
            Title pinned_t = std::move(*it2);
            row.items.erase(it2);
            row.items.insert(row.items.begin() + pinned, std::move(pinned_t));
            ++pinned;
        }
        search.rows.push_back(std::move(row));
        search_map_.push_back(static_cast<int>(i));
    }
    // "No results" is the screen's own message once the search is over; while
    // rows are showing, never replace them with a loading state.
    search.status = search_pending_ > 0 && search.rows.empty() ? "Searching..." : "";
    changed_ |= kSearch;
    art_dirty_ = true;
}

} // namespace sx
