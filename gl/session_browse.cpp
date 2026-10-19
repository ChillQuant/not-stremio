// The Board, Discover and the Library.

#include "session.hpp"
#include "stream_json.hpp"
#include "util.h" // now_seconds() for the meta dwell gate
#if __has_include("nav_memory.hpp")
#include "nav_memory.hpp" // WORKER 10: grid position save/restore; absent until then
#endif

#include <algorithm>
#include <cstdlib>

namespace sx
{

namespace
{

constexpr std::size_t kMaxRowCards = 50;

std::string row_title(const Catalog &c)
{
    const std::string type = capitalize(c.type);
    return c.name.empty() ? type : c.name + " - " + type;
}

// "tt0903747:2:5" -> "S2 E5"
std::string episode_badge(const std::string &video_id)
{
    const auto parts = split(video_id, ':');
    if (parts.size() >= 3)
    {
        const int s = std::atoi(parts[parts.size() - 2].c_str()), e = std::atoi(parts.back().c_str());
        if (s > 0 || e > 0)
            return "S" + std::to_string(s) + " E" + std::to_string(e);
    }
    return "";
}

// Seconds remaining -> "34 min left" / "2h 5m left", for the per-episode
// resume badge on Continue Watching cards.
std::string time_left(double remaining)
{
    if (remaining <= 0)
        return "";
    const long mins = static_cast<long>((remaining + 30) / 60); // round to the minute
    if (mins < 90)
        return std::to_string(mins) + " min left";
    return std::to_string(mins / 60) + "h " + std::to_string(mins % 60) + "m left";
}

// A stopped-near-the-end item leaves Continue Watching: resuming at 95%+
// is pointless. Grace: something stopped in the last day stays visible in
// case it was an accident and the viewer wants back in.
bool cw_finished(double offset, double duration, std::int64_t last_watched)
{
    if (offset <= 0 || duration <= 0 || offset < duration * 0.95)
        return false;
    return now_epoch_ms() - last_watched > 24LL * 3600 * 1000;
}

// Sort keys for Discover. Both are throw-free and return a negative value
// when the field is missing, so unsorted items sink to the bottom.
double disc_rating_key(const std::string &imdb_rating)
{
    if (imdb_rating.empty())
        return -1.0;
    // "7.8": parse manually; std::stod throws.
    double v = 0.0, frac = 0.0, div = 1.0;
    bool dot = false, any = false;
    for (char c : imdb_rating)
    {
        if (c == '.' && !dot)
        {
            dot = true;
            continue;
        }
        if (c < '0' || c > '9')
            break;
        any = true;
        if (!dot)
            v = v * 10.0 + (c - '0');
        else
        {
            frac = frac * 10.0 + (c - '0');
            div *= 10.0;
        }
    }
    return any ? v + frac / div : -1.0;
}

int disc_year_key(const std::string &release_info)
{
    if (release_info.empty())
        return -1;
    // "2024" or "2024-2025": take the leading year.
    int y = 0, digits = 0;
    for (char c : release_info)
    {
        if (c < '0' || c > '9')
            break;
        y = y * 10 + (c - '0');
        if (++digits > 4)
            break;
    }
    return digits == 4 ? y : -1;
}

const char *disc_sort_label(int sort)
{
    switch (sort)
    {
    case 1:
        return "Rating";
    case 2:
        return "Year";
    default:
        return "Default";
    }
}

std::vector<Item> parse_catalog(const json &j)
{
    std::vector<Item> items;
    const json &metas = jobj(j, "metas");
    if (!metas.is_array())
        return items;
    for (const auto &m : metas)
    {
        Item it = item_from_meta_json(m);
        if (!it.id.empty() && !it.name.empty())
            items.push_back(it);
    }
    return items;
}

std::string catalog_label(const ::Addon *a, const Catalog *c)
{
    return c->name.empty() ? a->name : c->name;
}

#if __has_include("nav_memory.hpp")
// The NavMem page name for a grid page; nullptr when the page carries no
// grid position (addons/settings) or isn't a Page (search).
const char *navmem_name(Page page)
{
    switch (page)
    {
    case Page::board:
        return "board";
    case Page::discover:
        return "discover";
    case Page::library:
        return "library";
    default:
        return nullptr;
    }
}
#endif

} // namespace

Title Session::make_title(const Item &it, bool with_progress) const
{
    Title t;
    t.id = it.id;
    t.type = it.type;
    t.name = it.name;
    t.year = it.release_info;
    t.runtime = it.runtime;
    t.genres = join(it.genres, ", ");
    t.imdb = it.imdb_rating;
    t.synopsis = it.description;
    if (with_progress)
    {
        if (it.type == "series")
        {
            // "S2 E7 · 34 min left": the episode and how much of it is left.
            t.badge = episode_badge(it.video_id);
            if (!t.badge.empty() && it.offset > 0 && it.duration > it.offset)
            {
                const std::string left = time_left(it.duration - it.offset);
                if (!left.empty())
                    t.badge += " \xC2\xB7 " + left;
            }
        }
        if (it.duration > 0 && it.offset > 0)
            t.progress = static_cast<float>(std::max(0.02, std::min(1.0, it.offset / it.duration)));
        t.watched = it.times_watched > 0 && it.offset <= 0;
    }
    t.episode_art = it.video_thumb;
    return t;
}

// ---------------------------------------------------------------------------
// Details the library does not carry

void Session::apply_meta(Item &it) const
{
    const std::string key = it.type + ":" + it.id;
    const auto m = meta_cache_.find(key);
    if (m != meta_cache_.end())
    {
        const Item &c = m->second;
        if (it.background.empty())
            it.background = c.background;
        if (it.logo.empty())
            it.logo = c.logo;
        if (it.poster.empty())
            it.poster = c.poster;
        if (it.description.empty())
            it.description = c.description;
        if (it.release_info.empty())
            it.release_info = c.release_info;
        if (it.runtime.empty())
            it.runtime = c.runtime;
        if (it.imdb_rating.empty())
            it.imdb_rating = c.imdb_rating;
        if (it.genres.empty())
            it.genres = c.genres;
    }
    // The episode's own thumbnail for this video, when want_meta() saw one.
    if (it.video_thumb.empty() && !it.video_id.empty())
    {
        const auto mt = meta_ep_thumbs_.find(key);
        if (mt != meta_ep_thumbs_.end())
        {
            const auto v = mt->second.find(it.video_id);
            if (v != mt->second.end())
                it.video_thumb = v->second;
        }
    }
}

void Session::want_meta(const Item &it)
{
    const std::string key = it.type + ":" + it.id;
    if (meta_cache_.count(key) || meta_asked_.count(key))
        return;
    // Speculative detail preload: the focused card must dwell 300ms before
    // its meta (metadata only, never video bytes) is fetched. refresh_art
    // calls this every frame for the focused card, so a new focus restarts
    // the dwell and cards scrolled past never fire a request. (The effective
    // granularity follows refresh_art's cadence: immediate on a focus move,
    // then its ~1s heartbeat while idle.)
    const double now = now_seconds();
    if (key != meta_dwell_key_)
    {
        meta_dwell_key_ = key;
        meta_dwell_since_ = now;
        return;
    }
    constexpr double kMetaDwellSecs = 0.3;
    if (now - meta_dwell_since_ < kMetaDwellSecs)
        return;
    std::vector<std::string> urls;
    for (const auto &a : addons_)
        if (!addon_disabled(*a) && a->supports("meta", it.type, it.id))
            urls.push_back(a->resource_url("meta", it.type, it.id));
    if (urls.empty())
        return;
    meta_asked_.insert(key);
    struct Res
    {
        bool ok = false;
        ::Meta meta;
    };
    bg<Res>(
        [urls]() {
            Res r;
            for (const auto &u : urls)
            {
                json j;
                std::string err;
                if (!fetch_json(u, j, err))
                    continue;
                const json &m = jobj(j, "meta");
                if (!m.is_object())
                    continue;
                ::Meta parsed = parse_meta(m);
                // Merge across addons: a stub from Local Files / StreamBridge
                // must not block Cinemeta / AIOMetadata from filling the rest.
                if (!r.ok)
                {
                    r.meta = parsed;
                    r.ok = true;
                }
                else
                {
                    if (r.meta.description.empty()) r.meta.description = parsed.description;
                    if (r.meta.background.empty())  r.meta.background = parsed.background;
                    if (r.meta.logo.empty())        r.meta.logo = parsed.logo;
                    if (r.meta.poster.empty())      r.meta.poster = parsed.poster;
                    if (r.meta.release_info.empty()) r.meta.release_info = parsed.release_info;
                    if (r.meta.runtime.empty())     r.meta.runtime = parsed.runtime;
                    if (r.meta.imdb_rating.empty()) r.meta.imdb_rating = parsed.imdb_rating;
                    if (r.meta.genres.empty())      r.meta.genres = parsed.genres;
                    if (r.meta.videos.empty())      r.meta.videos = parsed.videos;
                }
                if (!r.meta.description.empty() && !r.meta.background.empty())
                    break;
            }
            return r;
        },
        [this, key](Res &r) {
            if (!r.ok)
            {
                meta_asked_.erase(key); // allow retry later
                return;
            }
            Item c;
            c.background = r.meta.background;
            c.logo = r.meta.logo;
            c.poster = r.meta.poster;
            c.description = r.meta.description;
            c.release_info = r.meta.release_info;
            c.runtime = r.meta.runtime;
            c.imdb_rating = r.meta.imdb_rating;
            c.genres = r.meta.genres;
            meta_cache_[key] = c;
            if (!r.meta.videos.empty())
                meta_videos_[key] = r.meta.videos;
            // The episodes' thumbnails too: the Continue Watching row shows
            // the episode's own art (Title::episode_art), via apply_meta().
            if (!r.meta.videos.empty())
            {
                auto &thumbs = meta_ep_thumbs_[key];
                thumbs.clear();
                for (const ::Video &v : r.meta.videos)
                    if (!v.id.empty() && !v.thumbnail.empty())
                        thumbs[v.id] = v.thumbnail;
            }
            // Cap the cache like prune_progress(): evict from both maps so a
            // dropped entry can be re-fetched on demand (want_meta checks both).
            static const std::size_t kMetaCacheMax = 200;
            while (meta_cache_.size() > kMetaCacheMax)
            {
                auto evict = meta_cache_.begin();
                meta_asked_.erase(evict->first);
                meta_ep_thumbs_.erase(evict->first);
                meta_videos_.erase(evict->first);
                meta_cache_.erase(evict);
            }
            // Cancel the useless work: if focus moved on while the fetch was
            // in flight, keep the cache (it serves a later revisit) but skip
            // the full board/library rebuild — nothing on screen needs it.
            if (key != meta_dwell_key_)
                return;
            refresh_board();
            refresh_library();
            art_dirty_ = true; // new artwork URLs (background/logo) just landed
        });
}

// ---------------------------------------------------------------------------
// Continue watching

std::vector<Item> Session::continue_watching() const
{
    std::vector<Item> out;
    std::set<std::string> listed; // ids already in: the local + library merge never duplicates
    if (signed_in() && library_loaded_)
    {
        for (const auto &kv : library_)
        {
            const json &li = kv.second;
            const json &st = jobj(li, "state");
            const double offset = jnum(st, "timeOffset") / 1000.0;
            const bool removed = jbool(li, "removed"), temp = jbool(li, "temp");
            if (offset <= 0 || (removed && !temp))
                continue;
            Item it;
            it.id = kv.first;
            it.type = jstr(li, "type");
            it.name = jstr(li, "name");
            it.poster = jstr(li, "poster");
            it.background = jstr(li, "background");
            it.logo = jstr(li, "logo");
            it.video_id = jstr(st, "video_id");
            it.offset = offset;
            it.duration = jnum(st, "duration") / 1000.0;
            it.last_watched = iso8601_to_ms(jstr(st, "lastWatched"));
            if (cw_finished(it.offset, it.duration, it.last_watched))
                continue;
            apply_meta(it);
            listed.insert(it.id);
            out.push_back(it);
        }
        // Local-only entries (watched offline, never synced to the account):
        // merged in, never duplicated with the library rows above. An item
        // the library says is removed stays out even if local data lingers.
        for (const auto &kv : progress_)
        {
            if (kv.second.time <= 0 || listed.count(kv.first))
                continue;
            const auto li = library_.find(kv.first);
            if (li != library_.end() && jbool(li->second, "removed") && !jbool(li->second, "temp"))
                continue;
            Item it;
            it.id = kv.first;
            it.type = kv.second.type;
            it.name = kv.second.name;
            it.poster = kv.second.poster;
            it.video_id = kv.second.video_id;
            it.offset = kv.second.time;
            it.duration = kv.second.duration;
            it.last_watched = kv.second.updated;
            if (cw_finished(it.offset, it.duration, it.last_watched))
                continue;
            apply_meta(it);
            listed.insert(it.id);
            out.push_back(it);
        }
    }
    else
    {
        // Not signed in, or the library hasn't answered yet: the local data
        // load_progress() already read. The row shows instantly on first
        // paint; the branch above merges/enriches when the network lands.
        for (const auto &kv : progress_)
        {
            if (kv.second.time <= 0)
                continue;
            Item it;
            it.id = kv.first;
            it.type = kv.second.type;
            it.name = kv.second.name;
            it.poster = kv.second.poster;
            it.video_id = kv.second.video_id;
            it.offset = kv.second.time;
            it.duration = kv.second.duration;
            it.last_watched = kv.second.updated;
            if (cw_finished(it.offset, it.duration, it.last_watched))
                continue;
            apply_meta(it);
            listed.insert(it.id);
            out.push_back(it);
        }
    }
    // The episode after one that was just finished: nothing of it is watched yet, so there is no
    // timeOffset to find, only what was remembered here.
    for (const auto &kv : progress_)
    {
        if (kv.second.up_next.empty() || listed.count(kv.first))
            continue;
        Item it;
        it.id = kv.first;
        it.type = kv.second.type;
        it.name = kv.second.name;
        it.poster = kv.second.poster;
        const auto li = library_.find(kv.first);
        if (li != library_.end())
        {
            it.poster = jstr(li->second, "poster");
            it.background = jstr(li->second, "background");
            it.logo = jstr(li->second, "logo");
        }
        it.video_id = kv.second.up_next;
        it.up_next = true;
        it.last_watched = kv.second.updated;
        apply_meta(it);
        listed.insert(it.id);
        out.push_back(it);
    }
    // Instant Board: lock the order the snapshot showed. Ids already on
    // screen keep their locked positions (no reshuffle while the library
    // merge revalidates); a genuinely new id watched more recently than
    // everything on screen jumps to the front; any other new id appends
    // after the locked ones, freshest first.
    if (!board_.empty() && board_[0].continue_watching && !board_[0].items.empty())
    {
        struct Lock
        {
            std::size_t idx;
            std::int64_t lw;
        };
        std::map<std::string, Lock> locked;
        for (std::size_t k = 0; k < board_[0].items.size(); ++k)
        {
            const Item &it = board_[0].items[k];
            if (!it.id.empty())
                locked.emplace(it.id, Lock{k, it.last_watched});
        }
        std::int64_t freshest_locked = 0;
        for (const auto &kv : locked)
            freshest_locked = std::max(freshest_locked, kv.second.lw);
        std::stable_sort(out.begin(), out.end(), [&](const Item &a, const Item &b) {
            const auto ia = locked.find(a.id);
            const auto ib = locked.find(b.id);
            const bool ka = ia != locked.end(), kb = ib != locked.end();
            // Rank 0: newcomer fresher than everything on screen (genuinely
            // new, e.g. just watched) — goes first. Rank 1: locked — keeps
            // its snapshot position. Rank 2: other newcomer — appends.
            const int ra = ka ? 1 : (a.last_watched > freshest_locked ? 0 : 2);
            const int rb = kb ? 1 : (b.last_watched > freshest_locked ? 0 : 2);
            if (ra != rb)
                return ra < rb;
            if (ra == 1)
                return ia->second.idx < ib->second.idx;
            return a.last_watched > b.last_watched;
        });
    }
    else
    {
        std::sort(out.begin(), out.end(),
                  [](const Item &a, const Item &b) { return a.last_watched > b.last_watched; });
    }
    if (out.size() > kMaxRowCards)
        out.resize(kMaxRowCards);
    return out;
}

// ---------------------------------------------------------------------------
// Board

void Session::build_board()
{
    const int gen = ++home_gen_;
    board_.clear();
    BoardRow cw;
    cw.key = "continue";
    cw.title = "Continue Watching";
    cw.continue_watching = true;
    cw.loaded = true;
    board_.push_back(cw);

    for (const auto &a : addons_)
    {
        if (addon_disabled(*a))
            continue;
        for (const auto &c : a->catalogs)
        {
            if (c.requires_other_than(""))
                continue; // needs a search or a genre
            BoardRow row;
            row.key = a->transport_url + "|" + c.type + "|" + c.id;
            row.title = row_title(c);
            row.addon_url = a->transport_url;
            row.catalog = c;
            board_.push_back(row);
        }
    }

    for (std::size_t i = 1; i < board_.size(); ++i)
    {
        const ::Addon *a = find_addon(board_[i].addon_url);
        if (!a)
            continue;
        const std::string url = a->resource_url("catalog", board_[i].catalog.type, board_[i].catalog.id);
        struct Res
        {
            std::vector<Item> items;
            std::string error;
        };
        bg<Res>(
            [url]() {
                Res r;
                json j;
                if (fetch_json(url, j, r.error))
                    r.items = parse_catalog(j);
                return r;
            },
            [this, gen, i](Res &r) {
                if (gen != home_gen_ || i >= board_.size())
                    return;
                // Another worker's fields (gl/model.hpp): the row stops
                // validating the moment its catalog answers, either way.
                board_[i].validating = false;
                if (r.error.empty())
                {
                    board_[i].loaded = true;
                    board_[i].from_snapshot = false;
                    board_[i].items = std::move(r.items);
                    board_[i].error.clear();
                    if (board_[i].items.size() > kMaxRowCards)
                        board_[i].items.resize(kMaxRowCards);
                }
                else
                {
                    board_[i].loaded = true;
                    board_[i].error = r.error;
                    // Keep the snapshot items for offline browsing.
                    dlog("catalog %s: %s", board_[i].title.c_str(), r.error.c_str());
                }
                refresh_board();
            });
    }
    refresh_board();
}

void Session::refresh_board()
{
    // Local-first: the Continue Watching row exists from the first paint,
    // built from the local progress data load_progress() already read — no
    // network needed. build_board() / board_snapshot_load() put this row
    // first too; the seed below only covers a refresh_board() that somehow
    // runs before either of them.
    if (board_.empty())
    {
        BoardRow cw;
        cw.key = "continue";
        cw.title = "Continue Watching";
        cw.continue_watching = true;
        cw.loaded = true;
        board_.push_back(cw);
    }
    if (board_[0].continue_watching)
    {
        board_[0].items = continue_watching();
        // Rebuilt from local progress every refresh: never a stale snapshot
        // row and never waiting on a catalog (the validating badge is for
        // catalog rows; the CW special-case in the state priority below
        // covers the library-not-loaded-yet window).
        board_[0].from_snapshot = false;
        board_[0].validating = false;
        // The hero row shows the freshest few; the rest lives on the Library
        // page (Last watched). Capped here — not in continue_watching() —
        // so the resume-offset lookup in play_stream still sees every item.
        constexpr std::size_t kHeroVisible = 12;
        if (board_[0].items.size() > kHeroVisible)
            board_[0].items.resize(kHeroVisible);
    }
    // WORKER 1 (STARTUP): stow the previous rows so this refresh can reuse
    // their GPU texture handles and laid-out text (see the match loop below).
    // Reused once per title; the vectors are gone by the time we return.
    std::vector<Row> prev_rows = std::move(board.rows);
    board.rows.clear();
    board_map_.clear();
    bool any_loading = false;
    int loaded = 0, failed = 0;
    for (std::size_t i = 0; i < board_.size(); ++i)
    {
        const BoardRow &br = board_[i];
        if (!br.loaded)
        {
            any_loading = true;
            continue;
        }
        ++loaded;
        if (!br.error.empty())
        {
            ++failed;
            if (br.items.empty())
                continue;
            // Errored but still has items: a snapshot row surviving offline.
            // It is emitted below with RowState::offline instead of skipped.
        }
        else if (br.items.empty())
            continue;
        Row row;
        row.key = br.key;
        row.title = br.title;
        // Another worker's field (ui/content.hpp): where this row's content
        // came from, so the draw code can badge it.
        if (br.validating || (br.continue_watching && signed_in() && !library_loaded_))
            row.state = RowState::validating;
        else if (!br.error.empty())
            row.state = RowState::offline;
        else if (br.from_snapshot)
            row.state = RowState::cached;
        else
            row.state = RowState::synced;
        for (const Item &it : br.items) {
            Item enriched = it;
            apply_meta(enriched);
            Title nt = make_title(enriched, br.continue_watching);
            // Reuse the previous rows' Title state (GPU texture handles and
            // the text cache) when the same title survives the refresh, so
            // the first repaint doesn't re-upload artwork or re-lay-out
            // text. Matches by row key, then by title id.
            bool matched = false;
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
                        matched = true;
                        break;
                    }
                }
                break;
            }
            if (!matched)
                nt.poster_fade = 0.0f; // a brand-new card: crossfades in over 180ms when its texture arrives
            row.items.push_back(std::move(nt));
        }
        board.rows.push_back(std::move(row));
        board_map_.push_back(static_cast<int>(i));
    }
    board_loading_ = any_loading;
    // Another worker's fields (ui/content.hpp): the offline pill. Failed
    // rows with nothing left loading means the network is down and what is
    // on screen is cached content; while anything still loads, the pill
    // shows the snapshot's age instead.
    board.offline = (failed > 0 && !any_loading);
    board.sync_pill = board.offline ? "Offline · Cached" : (any_loading ? board_snapshot_note_ : "");
    if (!board.rows.empty())
        board.status.clear();
    else if (addons_loading_ || any_loading)
        board.status = "Loading your catalogs...";
    else if (addons_.empty())
        board.status = "No addon could be reached. Check the network connection, then reload the addons in Settings.";
    else if (loaded > 0 && failed == loaded)
        board.status = "Couldn't load catalogs — check your connection.";
    else
        board.status = "None of your addons offer catalogs.";
    changed_ |= kBoard;
    art_dirty_ = true;
    // Persist the board snapshot for the next instant boot. The save keeps
    // the last good content: it needs more than the Continue Watching row
    // plus at least one answered catalog row, and the Continue Watching row
    // is included now (rebuilt from local progress at boot, no network).
    board_snapshot_save();
    // Re-index the fresh board for on-device search (cheap: fingerprint
    // no-op when the sources are unchanged, see session_search.cpp).
    search_index_ensure();
}

// ---------------------------------------------------------------------------
// Hero meta: the Board's focused title needs full details (description,
// background, logo) that catalogs don't carry. Fetched in the background.

void Session::hero_meta_check(const Cursor &cursor)
{
    if (cursor.board_row < 0 || cursor.board_row >= static_cast<int>(board_map_.size()))
        return;
    const int bi = board_map_[static_cast<std::size_t>(cursor.board_row)];
    if (bi < 0 || bi >= static_cast<int>(board_.size()))
        return;
    BoardRow &br = board_[static_cast<std::size_t>(bi)];
    if (cursor.board_col < 0 || cursor.board_col >= static_cast<int>(br.items.size()))
        return;
    Item &it = br.items[static_cast<std::size_t>(cursor.board_col)];
    if (it.id.empty() || it.type.empty())
        return;
    const std::string key = it.type + ":" + it.id;

    // If meta_cache_ already has it, apply immediately — but only rebuild
    // the board when apply_meta() actually changed something, otherwise the
    // cached-but-empty case rebuilds the whole board (and saves the board
    // snapshot) every frame while hero_meta_check() is called.
    if (meta_cache_.count(key))
    {
        if (it.description.empty() || it.background.empty())
        {
            const std::string before_desc = it.description;
            const std::string before_bg = it.background;
            apply_meta(it);
            if (it.description != before_desc || it.background != before_bg)
                refresh_board();
        }
        return;
    }
    if (!it.description.empty() && !it.background.empty())
        return;
    if (key == hero_meta_key_)
        return;

    // Dwell gate: holding D-pad must rest 200ms on one card before a new
    // meta fetch is launched; a new focus restarts the dwell.
    if (!hero_meta_key_.empty())
    {
        const double now = now_seconds();
        if (key != hero_dwell_key_)
        {
            hero_dwell_key_ = key;
            hero_dwell_since_ = now;
            return;
        }
        if (now - hero_dwell_since_ < 0.20)
            return;
    }

    std::vector<std::string> urls;
    for (const auto &a : addons_)
        if (!addon_disabled(*a) && a->supports("meta", it.type, it.id))
            urls.push_back(a->resource_url("meta", it.type, it.id));
    // Do NOT set hero_meta_key_ if addons haven't loaded yet, so we retry as soon as they do.
    if (urls.empty())
        return;

    hero_meta_key_ = key;
    const int gen = ++hero_meta_gen_;
    const std::string type = it.type, id = it.id;
    struct Res
    {
        bool ok = false;
        ::Meta meta;
    };
    bg<Res>(
        [urls]() {
            Res r;
            for (const auto &u : urls)
            {
                json j;
                std::string err;
                if (!fetch_json(u, j, err))
                    continue;
                const json &m = jobj(j, "meta");
                if (!m.is_object())
                    continue;
                ::Meta parsed = parse_meta(m);
                // If an earlier addon returned a partial stub without synopsis/backdrop,
                // keep looking so Cinemeta / AIOMetadata can fill the rest.
                if (!r.ok)
                {
                    r.meta = parsed;
                    r.ok = true;
                }
                else
                {
                    if (r.meta.description.empty()) r.meta.description = parsed.description;
                    if (r.meta.background.empty())  r.meta.background = parsed.background;
                    if (r.meta.logo.empty())        r.meta.logo = parsed.logo;
                    if (r.meta.poster.empty())      r.meta.poster = parsed.poster;
                    if (r.meta.release_info.empty()) r.meta.release_info = parsed.release_info;
                    if (r.meta.runtime.empty())     r.meta.runtime = parsed.runtime;
                    if (r.meta.imdb_rating.empty()) r.meta.imdb_rating = parsed.imdb_rating;
                    if (r.meta.genres.empty())      r.meta.genres = parsed.genres;
                    if (r.meta.videos.empty())      r.meta.videos = parsed.videos;
                }
                if (!r.meta.description.empty() && !r.meta.background.empty())
                    break;
            }
            return r;
        },
        [this, gen, key, type, id](Res &r) {
            if (!r.ok)
            {
                if (gen == hero_meta_gen_)
                    hero_meta_key_.clear(); // allow retry later
                return;
            }
            // Store in meta_cache_ FIRST so continue_watching() / apply_meta()
            // inside refresh_board() preserves the fetched metadata on row 0!
            Item c;
            c.background = r.meta.background;
            c.logo = r.meta.logo;
            c.poster = r.meta.poster;
            c.description = r.meta.description;
            c.release_info = r.meta.release_info;
            c.runtime = r.meta.runtime;
            c.imdb_rating = r.meta.imdb_rating;
            c.genres = r.meta.genres;
            meta_cache_[key] = c;
            meta_asked_.insert(key);
            if (!r.meta.videos.empty())
                meta_videos_[key] = r.meta.videos;
            if (!r.meta.videos.empty())
            {
                auto &thumbs = meta_ep_thumbs_[key];
                thumbs.clear();
                for (const ::Video &v : r.meta.videos)
                    if (!v.id.empty() && !v.thumbnail.empty())
                        thumbs[v.id] = v.thumbnail;
            }
            for (auto &b : board_)
            {
                for (auto &i : b.items)
                {
                    if (i.type == type && i.id == id)
                        apply_meta(i);
                }
            }
            if (gen != hero_meta_gen_)
                return;
            refresh_board();
            refresh_library();
            art_dirty_ = true;
        });
}

void Session::continue_remove(const Item &it)
{
    const auto p = progress_.find(it.id);
    if (p != progress_.end())
    {
        p->second.time = 0;
        p->second.up_next.clear();
        save_progress();
    }
    const auto li = library_.find(it.id);
    if (signed_in() && li != library_.end())
    {
        json &item = li->second;
        item["state"]["timeOffset"] = 0;
        item["_mtime"] = iso8601_now();
        const std::string key = prefs_.auth_key;
        const json changes = json::array({item});
        bg<ApiResult>([key, changes]() { return api_library_put(key, changes); },
                      [this](ApiResult &r) {
                          if (!r.ok)
                              say("Could not update your library: " + r.error, 5.0, true);
                      });
    }
    say("Removed from Continue Watching");
    refresh_board();
    refresh_library();
}

bool Session::board_options(int row, int col)
{
    if (row < 0 || row >= static_cast<int>(board_map_.size()))
        return false;
    const BoardRow &br = board_[static_cast<std::size_t>(board_map_[static_cast<std::size_t>(row)])];
    if (!br.continue_watching || col < 0 || col >= static_cast<int>(br.items.size()))
        return false;
    const Item it = br.items[static_cast<std::size_t>(col)];
    // The poster and how far it is watched, in the dialog.
    std::uint32_t poster = 0;
    float progress = -1.0f;
    if (row < static_cast<int>(board.rows.size()) && col < static_cast<int>(board.rows[static_cast<std::size_t>(row)].items.size()))
    {
        const Title &t = board.rows[static_cast<std::size_t>(row)].items[static_cast<std::size_t>(col)];
        poster = t.poster;
        progress = t.progress;
    }
    const std::string where = it.type == "series" ? episode_badge(it.video_id) : std::string();
    open_dropdown(it.name, {"Remove from Continue Watching", "Cancel"}, -1, [this, it, poster, progress, where](int i) {
        if (i != 0)
            return;
        // Taking something away asks once more, and the cursor starts on the safe answer.
        open_dropdown(it.name, {"Remove", "Keep"}, -1, [this, it](int j) {
            if (j == 0)
                continue_remove(it);
        });
        dropdown.message = "Remove it from Continue Watching?";
        dropdown.danger = 0;
        dropdown.selected = 1;
        dropdown.dialog = true;
        dropdown.poster = poster;
        dropdown.progress = progress;
    });
    dropdown.message = where;
    dropdown.selected = 0;
    dropdown.dialog = true;
    dropdown.poster = poster;
    dropdown.progress = progress;
    return true;
}

bool Session::board_is_continue(int row) const
{
    return row >= 0 && row < static_cast<int>(board_map_.size()) &&
           board_[static_cast<std::size_t>(board_map_[static_cast<std::size_t>(row)])].continue_watching;
}

bool Session::saved_stream(const std::string &item_id, const std::string &video_id, ::Stream *out,
                           bool allow_any_episode) const
{
    const auto p = progress_.find(item_id);
    if (p == progress_.end() || p->second.stream_json.empty())
        return false;
    // The stream is for one episode: it is no use for another, unless the
    // caller opted in to accept any remembered episode's stream.
    if (!allow_any_episode && !video_id.empty() && p->second.stream_video_id != video_id)
        return false;
    try
    {
        const ::Stream s = stream_from_json(json::parse(p->second.stream_json));
        if (!s.playable())
            return false;
        *out = s;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void Session::continue_press(int row, int col)
{
    if (!board_is_continue(row))
        return;
    const BoardRow &br = board_[static_cast<std::size_t>(board_map_[static_cast<std::size_t>(row)])];
    if (col < 0 || col >= static_cast<int>(br.items.size()) || row >= static_cast<int>(board.rows.size()) ||
        col >= static_cast<int>(board.rows[static_cast<std::size_t>(row)].items.size()))
        return;
    const Item it = br.items[static_cast<std::size_t>(col)];
    const Title title = board.rows[static_cast<std::size_t>(row)].items[static_cast<std::size_t>(col)];
    ::Stream saved;
    // One tap resumes: the remembered stream plays at once, skipping the
    // detail page. Otherwise (the next episode of one that was finished, or
    // nothing remembered) the streams are shown at once, as before — and the
    // detail page stays reachable there, on the hero's More Info button, and
    // on the Options menu.
    // Instant Season-Pack Autoplay (Worker C): an up-next tile (the episode
    // after one the viewer finished) has no remembered stream of its own,
    // but any remembered stream for this item primes autoplay once the
    // episode's details load.
    if (it.up_next && saved_stream(it.id, it.video_id, &saved, true))
    {
        open_title(title, it.video_id);
        arm_autoplay_from(saved);
        resumed_from_board_ = true;
        return;
    }
    const bool can_play_now =
        !it.up_next && it.offset > 0 && saved_stream(it.id, it.type == "series" ? it.video_id : std::string(), &saved);
    if (!can_play_now)
    {
        open_req_ = std::make_pair(title, it.video_id);
        return;
    }
    resume_play(title, it);
}

void Session::resume_play(const Title &title, const Item &it)
{
    if (watching)
        return;
    // Start ~5 s before the saved point: the viewer re-enters with context.
    // The remembered offset itself is moved (both stores), so the launch
    // path in play_stream() — which reads it back via continue_watching() —
    // and every later resume agree. Only past 10 s (play_stream() starts at
    // 0 for offsets <= 5, so a smaller rewind would restart the video) and
    // never for a finished item. save_progress() writes via tmp+rename, so
    // a crash mid-write cannot corrupt progress.json.
    if (it.offset > 10.0 && (it.duration <= 0 || it.offset < it.duration * 0.95))
    {
        const double back = it.offset - 5.0;
        const auto p = progress_.find(it.id);
        if (p != progress_.end())
        {
            p->second.time = back;
            p->second.updated = now_epoch_ms();
        }
        if (signed_in())
        {
            const auto li = library_.find(it.id);
            if (li != library_.end())
                li->second["state"]["timeOffset"] = static_cast<std::int64_t>(back * 1000);
        }
        save_progress();
    }
    open_title(title); // loads the details (the film's streams too: they are not used)
    resume_play_ = true;
    resumed_from_board_ = true;
    resume_video_ = it.video_id.empty() ? it.id : it.video_id;
    // The launch screen is up at once, while the details come in.
    w_item_ = it;
    launch = LaunchState();
    launch.title = it.name;
    launch.subtitle = it.type == "series" ? episode_badge(it.video_id) : std::string();
    // The remembered stream may be a direct HTTP/Debrid link rather than a
    // torrent — show direct-connection stages for those (always exactly 4,
    // like the torrent set) instead of "Connecting to peers".
    ::Stream saved;
    if (saved_stream(it.id, it.type == "series" ? it.video_id : std::string(), &saved) && !saved.url.empty())
    {
        launch.stages = {"Resolving stream", "Connecting to server", "Filling the buffer", "Starting"};
        launch.torrent = false;
    }
    else
        launch.stages = {"Finding sources", "Connecting to peers", "Filling the buffer", "Starting"};
    launch.stage = 0;
    launch.progress = -1.0f;
    launch_open = true;
    watching = true;
    resuming_ = true;
    playing = PlayerState();
    playing.title = it.name;
    playing.subtitle = launch.subtitle;
    playing.controls = true;
    art_dirty_ = true;
}

std::string Session::next_episode_id() const
{
    if (!d_open_ || !d_meta_loaded_ || !detail.series || w_item_.id != d_item_.id)
        return std::string();
    const auto &vids = d_meta_.videos;
    std::size_t cur = vids.size();
    for (std::size_t i = 0; i < vids.size(); ++i)
        if (vids[i].id == w_video_id_)
            cur = i;
    if (cur + 1 >= vids.size())
        return std::string();
    if (vids[cur + 1].season == 0 && vids[cur].season != 0)
        return std::string(); // do not roll into the specials
    return vids[cur + 1].id;
}
void Session::board_square(int row, int col)
{
    (void)col;
    if (row < 0 || row >= static_cast<int>(board_map_.size()))
        return;
    const int bi = board_map_[static_cast<std::size_t>(row)];
    BoardRow &br = board_[static_cast<std::size_t>(bi)];
    if (br.continue_watching)
        return; // everything in it can be reached by scrolling: it has no "See all"    // See all: Discover opens on this catalog.
    see_all_addon_ = br.addon_url;
    see_all_catalog_ = br.catalog.id;
    see_all_type_ = br.catalog.type;
    disc_items_.clear();
    disc_ids_.clear();
    discover.items.clear();
    changed_ |= kDiscover | kDiscoverNew;
    goto_ = Page::discover;
}

// ---------------------------------------------------------------------------
// Discover

void Session::page_opened(Page page)
{
    // Leaving a page: its grid position is remembered (from the latest
    // cursor update()) so back-navigation can reinstate the focus.
    if (page != page_)
        navmem_save_page(page_);
    page_ = page;
    if (page == Page::discover)
        enter_discover();
    else if (page == Page::library && signed_in() && !library_loaded_)
        load_library();
    else if (page == Page::settings)
        refresh_cache_size_bg(); // refresh the Settings → Clear cache number
    // Entering a page: reinstate the remembered focus. Stashed for the
    // shell, which takes it once via take_restored_grid_pos().
    navmem_restore_page(page);
}

#if __has_include("nav_memory.hpp")
void Session::navmem_save_page(Page page)
{
    const char *name = navmem_name(page);
    if (!name)
        return;
    int row = 0, col = 0;
    switch (page)
    {
    case Page::board:
        row = last_cursor_.board_row;
        col = last_cursor_.board_col;
        break;
    case Page::discover:
    case Page::library:
        // Flat grids: col carries the item index; the shell converts with
        // its column count and clamps to the loaded items.
        col = page == Page::discover ? last_cursor_.discover : last_cursor_.library;
        break;
    default:
        return;
    }
    NavMem::save_grid_pos(name, row, col);
}

void Session::navmem_restore_page(Page page)
{
    const char *name = navmem_name(page);
    if (!name)
        return;
    int row = 0, col = 0;
    if (!NavMem::restore_grid_pos(name, row, col))
        return;
    restored_page_ = page;
    restored_row_ = row;
    restored_col_ = col;
    has_restored_ = true;
}
#else
void Session::navmem_save_page(Page page)
{
    (void)page;
}
void Session::navmem_restore_page(Page page)
{
    (void)page;
}
#endif

void Session::enter_discover()
{
    // Types offered by catalogs that can be browsed without a search.
    disc_types_.clear();
    for (const auto &a : addons_)
    {
        if (addon_disabled(*a))
            continue;
        for (const auto &c : a->catalogs)
        {
            if (c.requires_other_than("genre"))
                continue;
            if (std::find(disc_types_.begin(), disc_types_.end(), c.type) == disc_types_.end())
                disc_types_.push_back(c.type);
        }
    }
    const auto rank = [](const std::string &t) { return t == "movie" ? 0 : t == "series" ? 1 : 2; };
    std::stable_sort(disc_types_.begin(), disc_types_.end(),
                     [&](const std::string &a, const std::string &b) { return rank(a) < rank(b); });
    if (disc_types_.empty())
    {
        discover.filters.clear();
        discover.items.clear();
        disc_items_.clear();
    disc_ids_.clear();
        discover.status = addons_loading_ ? "Addons are still loading..." : "None of your addons have catalogs to browse.";
        changed_ |= kDiscover;
        return;
    }
    bool restart = false;
    if (!see_all_type_.empty())
    {
        disc_type_ = see_all_type_;
        disc_genre_.clear();
        disc_catalog_ = 0;
        restart = true;
    }
    else if (std::find(disc_types_.begin(), disc_types_.end(), disc_type_) == disc_types_.end())
    {
        disc_type_ = disc_types_[0];
        disc_catalog_ = 0;
        disc_genre_.clear();
        restart = true;
    }
    discover_build_chips();
    if (!see_all_type_.empty())
    {
        for (std::size_t i = 0; i < disc_catalogs_.size(); ++i)
            if (disc_catalogs_[i].addon->transport_url == see_all_addon_ && disc_catalogs_[i].catalog->id == see_all_catalog_)
                disc_catalog_ = static_cast<int>(i);
        discover_build_chips();
        see_all_type_.clear();
    }
    if (restart || disc_items_.empty())
        discover_load(false);
    else
        changed_ |= kDiscover;
}

void Session::discover_build_chips()
{
    disc_catalogs_.clear();
    for (const auto &a : addons_)
    {
        if (addon_disabled(*a))
            continue;
        for (const auto &c : a->catalogs)
            if (c.type == disc_type_ && !c.requires_other_than("genre"))
                disc_catalogs_.push_back({a.get(), &c});
    }
    if (disc_catalog_ >= static_cast<int>(disc_catalogs_.size()))
        disc_catalog_ = 0;

    discover.filters.clear();
    discover.filters.push_back({"Type", capitalize(disc_type_)});
    if (!disc_catalogs_.empty())
    {
        const DiscCatalog &dc = disc_catalogs_[static_cast<std::size_t>(disc_catalog_)];
        discover.filters.push_back({"Catalog", catalog_label(dc.addon, dc.catalog)});
        const CatalogExtra *g = dc.catalog->extra("genre");
        if (g && !g->options.empty())
        {
            if (g->required && disc_genre_.empty())
                disc_genre_ = g->options[0];
            discover.filters.push_back({"Genre", disc_genre_.empty() ? "All genres" : disc_genre_});
        }
        else
        {
            disc_genre_.clear();
        }
    }
    discover.filters.push_back({"Sort", disc_sort_label(prefs_.disc_sort)});
    changed_ |= kDiscover;
}

void Session::discover_load(bool more)
{
    if (disc_catalogs_.empty())
        return;
    if (!more)
    {
        ++disc_gen_;
        disc_items_.clear();
    disc_ids_.clear();
        discover.items.clear();
        disc_end_ = false;
        // A new list: a stashed grid restore from an earlier page enter no
        // longer applies (the shell resets to the top on kDiscoverNew).
        if (has_restored_ && restored_page_ == Page::discover)
            has_restored_ = false;
        changed_ |= kDiscover | kDiscoverNew;
    }
    if (disc_loading_ && more)
        return;
    const DiscCatalog &dc = disc_catalogs_[static_cast<std::size_t>(disc_catalog_)];
    std::vector<std::string> extra;
    if (!disc_genre_.empty())
        extra.push_back("genre=" + url_encode(disc_genre_));
    if (more && !disc_items_.empty())
        extra.push_back("skip=" + std::to_string(disc_items_.size()));
    const std::string url = dc.addon->resource_url("catalog", dc.catalog->type, dc.catalog->id, join(extra, "&"));
    const int gen = disc_gen_;
    disc_loading_ = true;
    if (disc_items_.empty())
        discover.status = "Loading...";
    struct Res
    {
        std::vector<Item> items;
        std::string error;
    };
    bg<Res>(
        [url]() {
            Res r;
            json j;
            if (fetch_json(url, j, r.error))
                r.items = parse_catalog(j);
            return r;
        },
        [this, gen](Res &r) {
            if (gen != disc_gen_)
                return;
            disc_loading_ = false;
            // Skip duplicates (some addons ignore skip and send page one again).
            // O(1) via the id set; the vector stays capped so infinite scroll
            // can't grow memory without bound.
            constexpr std::size_t kDiscoverCap = 500;
            std::size_t added = 0;
            for (const Item &it : r.items)
            {
                if (disc_items_.size() >= kDiscoverCap)
                {
                    disc_end_ = true;
                    break;
                }
                if (!disc_ids_.insert(it.id).second)
                    continue;
                disc_items_.push_back(it);
                discover.items.push_back(make_title(it, false));
                ++added;
            }
            if (added == 0)
                disc_end_ = true;
            if (!r.error.empty() && disc_items_.empty())
                discover.status = "Could not load this catalog (" + r.error + ")";
            else if (disc_items_.empty())
                discover.status = "Nothing here.";
            else
                discover.status.clear();
            if (added > 0)
                discover_sort_items(); // keep the sort across pages
            changed_ |= kDiscover;
            art_dirty_ = true;
        });
}

void Session::discover_load_more()
{
    if (disc_loading_ || disc_end_ || disc_items_.empty())
        return;
    discover_load(true);
}

void Session::discover_sort_items()
{
    const int sort = prefs_.disc_sort;
    if (sort <= 0 || disc_items_.size() < 2)
        return;
    // Sort indices, then apply the same permutation to both vectors so the
    // Title objects (with their artwork texture ids) stay with their Items.
    std::vector<std::size_t> idx(disc_items_.size());
    for (std::size_t i = 0; i < idx.size(); ++i)
        idx[i] = i;
    if (sort == 1) // rating, highest first
    {
        std::stable_sort(idx.begin(), idx.end(), [this](std::size_t a, std::size_t b) {
            return disc_rating_key(disc_items_[a].imdb_rating) > disc_rating_key(disc_items_[b].imdb_rating);
        });
    }
    else if (sort == 2) // year, newest first
    {
        std::stable_sort(idx.begin(), idx.end(), [this](std::size_t a, std::size_t b) {
            return disc_year_key(disc_items_[a].release_info) > disc_year_key(disc_items_[b].release_info);
        });
    }
    else
    {
        return;
    }
    std::vector<Item> sorted_items;
    std::vector<Title> sorted_titles;
    sorted_items.reserve(idx.size());
    sorted_titles.reserve(idx.size());
    for (std::size_t i : idx)
    {
        sorted_items.push_back(std::move(disc_items_[i]));
        sorted_titles.push_back(std::move(discover.items[i]));
    }
    disc_items_ = std::move(sorted_items);
    discover.items = std::move(sorted_titles);
    changed_ |= kDiscover;
}

// ---------------------------------------------------------------------------
// The two filter rows

void Session::filter_activate(Page page, int filter)
{
    if (page == Page::discover)
    {
        // The Sort chip is always last, but its index varies (the Genre chip
        // is optional), so check it by label before the index-based branches.
        if (filter >= 0 && filter < static_cast<int>(discover.filters.size()) &&
            discover.filters[static_cast<std::size_t>(filter)].label == "Sort")
        {
            open_dropdown("Sort", {"Default", "Rating", "Year (newest)"}, prefs_.disc_sort, [this](int i) {
                if (i == prefs_.disc_sort)
                    return;
                prefs_.disc_sort = i;
                save_prefs();
                discover_build_chips(); // refreshes the chip label
                // Re-sort the loaded items in place: no reload, artwork stays
                // attached to its Title. Switching back to Default reloads
                // page one, since the original catalog order isn't retained.
                if (i == 0)
                    discover_load(false);
                else
                    discover_sort_items();
            });
        }
        else if (filter == 0)
        {
            std::vector<std::string> opts;
            int active = 0;
            for (std::size_t i = 0; i < disc_types_.size(); ++i)
            {
                opts.push_back(capitalize(disc_types_[i]));
                if (disc_types_[i] == disc_type_)
                    active = static_cast<int>(i);
            }
            open_dropdown("Type", opts, active, [this](int i) {
                if (disc_types_[static_cast<std::size_t>(i)] == disc_type_)
                    return;
                disc_type_ = disc_types_[static_cast<std::size_t>(i)];
                disc_catalog_ = 0;
                disc_genre_.clear();
                discover_build_chips();
                discover_load(false);
            });
        }
        else if (filter == 1)
        {
            std::vector<std::string> opts;
            for (const auto &dc : disc_catalogs_)
            {
                std::string label = catalog_label(dc.addon, dc.catalog);
                if (label != dc.addon->name)
                    label += " (" + dc.addon->name + ")";
                opts.push_back(label);
            }
            open_dropdown("Catalog", opts, disc_catalog_, [this](int i) {
                if (i == disc_catalog_)
                    return;
                disc_catalog_ = i;
                disc_genre_.clear();
                discover_build_chips();
                discover_load(false);
            });
        }
        else if (filter == 2 && !disc_catalogs_.empty())
        {
            const CatalogExtra *g = disc_catalogs_[static_cast<std::size_t>(disc_catalog_)].catalog->extra("genre");
            if (!g)
                return;
            std::vector<std::string> opts;
            int active = 0;
            if (!g->required)
                opts.push_back("All genres");
            for (const auto &o : g->options)
            {
                if (o == disc_genre_)
                    active = static_cast<int>(opts.size());
                opts.push_back(o);
            }
            const bool required = g->required;
            open_dropdown("Genre", opts, active, [this, opts, required](int i) {
                const std::string genre = (!required && i == 0) ? "" : opts[static_cast<std::size_t>(i)];
                if (genre == disc_genre_)
                    return;
                disc_genre_ = genre;
                discover_build_chips();
                discover_load(false);
            });
        }
    }
    else if (page == Page::library)
    {
        if (filter == 0)
        {
            std::vector<std::string> types;
            for (const auto &kv : library_)
            {
                const std::string t = jstr(kv.second, "type");
                if (!t.empty() && std::find(types.begin(), types.end(), t) == types.end())
                    types.push_back(t);
            }
            for (const auto &kv : progress_)
                if (!kv.second.type.empty() && std::find(types.begin(), types.end(), kv.second.type) == types.end())
                    types.push_back(kv.second.type);
            std::sort(types.begin(), types.end());
            std::vector<std::string> opts = {"All types"};
            int active = 0;
            for (const auto &t : types)
            {
                if (t == lib_type_)
                    active = static_cast<int>(opts.size());
                opts.push_back(capitalize(t));
            }
            open_dropdown("Type", opts, active, [this, types](int i) {
                lib_type_ = i == 0 ? "" : types[static_cast<std::size_t>(i - 1)];
                refresh_library();
                changed_ |= kLibraryNew;
            });
        }
        else
        {
            open_dropdown("Sort by", {"Last watched", "A-Z", "Most watched"}, lib_sort_, [this](int i) {
                lib_sort_ = i;
                refresh_library();
                changed_ |= kLibraryNew;
            });
        }
    }
}

// ---------------------------------------------------------------------------
// Library

void Session::refresh_library()
{
    std::vector<Item> all;
    if (signed_in())
    {
        for (const auto &kv : library_)
        {
            const json &li = kv.second;
            if (jbool(li, "removed") || jbool(li, "temp"))
                continue;
            const json &st = jobj(li, "state");
            Item it;
            it.id = kv.first;
            it.type = jstr(li, "type");
            it.name = jstr(li, "name");
            it.poster = jstr(li, "poster");
            it.background = jstr(li, "background");
            it.logo = jstr(li, "logo");
            it.video_id = jstr(st, "video_id");
            it.offset = jnum(st, "timeOffset") / 1000.0;
            it.duration = jnum(st, "duration") / 1000.0;
            it.times_watched = static_cast<int>(jnum(st, "timesWatched"));
            it.last_watched = iso8601_to_ms(jstr(st, "lastWatched"));
            if (!it.last_watched)
                it.last_watched = iso8601_to_ms(jstr(li, "_mtime"));
            apply_meta(it);
            all.push_back(it);
        }
    }
    else
    {
        for (const auto &kv : progress_)
        {
            Item it;
            it.id = kv.first;
            it.type = kv.second.type;
            it.name = kv.second.name;
            it.poster = kv.second.poster;
            it.video_id = kv.second.video_id;
            it.offset = kv.second.time;
            it.duration = kv.second.duration;
            it.last_watched = kv.second.updated;
            all.push_back(it);
        }
    }

    std::vector<std::string> types;
    for (const Item &it : all)
        if (!it.type.empty() && std::find(types.begin(), types.end(), it.type) == types.end())
            types.push_back(it.type);
    if (!lib_type_.empty() && std::find(types.begin(), types.end(), lib_type_) == types.end())
        lib_type_.clear();

    static const char *sorts[] = {"Last watched", "A-Z", "Most watched"};
    library.filters.clear();
    library.filters.push_back({"Show", lib_type_.empty() ? "All types" : capitalize(lib_type_)});
    library.filters.push_back({"Sort", sorts[lib_sort_]});

    lib_items_.clear();
    for (const Item &it : all)
        if (lib_type_.empty() || it.type == lib_type_)
            lib_items_.push_back(it);
    if (lib_sort_ == 0)
        std::sort(lib_items_.begin(), lib_items_.end(), [](const Item &a, const Item &b) { return a.last_watched > b.last_watched; });
    else if (lib_sort_ == 1)
        std::sort(lib_items_.begin(), lib_items_.end(), [](const Item &a, const Item &b) { return lower(a.name) < lower(b.name); });
    else
        std::sort(lib_items_.begin(), lib_items_.end(), [](const Item &a, const Item &b) { return a.times_watched > b.times_watched; });

    library.items.clear();
    for (const Item &it : lib_items_)
        library.items.push_back(make_title(it, true));
    const std::size_t n = library.items.size();
    library.count = std::to_string(n) + (n == 1 ? " title" : " titles");

    if (n > 0)
        library.status.clear();
    else if (signed_in() && !library_loaded_)
        library.status = "Loading your library...";
    else if (!signed_in() && all.empty())
        library.status = "Sign in to your Stremio account in Settings to see your library here.";
    else
        library.status = "Nothing in your library matches this filter.";
    changed_ |= kLibrary;
    art_dirty_ = true;
}

} // namespace sx
