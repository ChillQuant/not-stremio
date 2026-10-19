// A title's page: its details, the seasons and episodes of a series, and the
// streams the addons offer for a film or an episode.

#include "session.hpp"
#include "nav_memory.hpp" // WORKER 10 (polish): remember the grid position when leaving
#include "netstream.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace sx
{

namespace
{

// ---- what an addon puts in a stream's name and description -------------------

// Torrentio and others decorate descriptions with emoji and symbols the fonts do
// not have; they carry the numbers (seeds, size) and are read before being dropped.
std::string plain(const std::string &s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (i + len > s.size())
            break;
        const bool emoji = len == 4 || (len == 3 && c == 0xE2 && (static_cast<unsigned char>(s[i + 1]) == 0x9A || static_cast<unsigned char>(s[i + 1]) == 0x9C)) ||
                           (len == 3 && c == 0xEF && static_cast<unsigned char>(s[i + 1]) == 0xB8);
        if (!emoji)
            out.append(s, i, len);
        i += len;
    }
    return trim(out);
}

std::string first_line(const std::string &s)
{
    const std::size_t nl = s.find('\n');
    return plain(nl == std::string::npos ? s : s.substr(0, nl));
}

// ---- trailers (Piped / Invidious, public instances, no API key) ------------

// All tried with short timeouts; the first playable direct URL wins. These
// are community instances and come and go: failure is normal and quiet.
const char *const kTrailerPiped[] = {
    "https://pipedapi.kavin.rocks",
    "https://pipedapi.adminforge.de",
    "https://pipedapi.reallyaweso.me",
};
const char *const kTrailerInvidious[] = {
    "https://inv.nadeko.net",
    "https://invidious.nerdvpn.de",
    "https://iv.melmac.space",
};
constexpr int kTrailerInstances = 3; // max instances tried per backend

// Piped: /search?q=..&filter=videos -> items[0].url ("/watch?v=ID"),
// then /streams/ID -> videoStreams[].url (direct mp4).
std::string trailer_via_piped(const std::string &query)
{
    const std::string q = url_encode(query);
    for (int i = 0; i < kTrailerInstances; ++i)
    {
        const std::string base = kTrailerPiped[i];
        json sj;
        std::string err;
        if (!fetch_json(base + "/search?q=" + q + "&filter=videos", sj, err, 5))
            continue;
        std::string watch;
        if (sj.contains("items") && sj["items"].is_array() && !sj["items"].empty())
            watch = sj["items"][0].value("url", "");
        const std::size_t pos = watch.find("v=");
        if (pos == std::string::npos)
            continue;
        const std::string vid = watch.substr(pos + 2);
        json vj;
        if (!fetch_json(base + "/streams/" + url_encode(vid), vj, err, 5))
            continue;
        if (vj.contains("videoStreams") && vj["videoStreams"].is_array())
        {
            for (const json &vs : vj["videoStreams"])
            {
                const std::string u = vs.value("url", "");
                if (u.compare(0, 4, "http") == 0)
                    return u;
            }
        }
    }
    return "";
}

// Invidious: /api/v1/search?q=..&type=video -> [0].videoId,
// then /api/v1/videos/ID -> formatStreams[].url (fallback adaptiveFormats).
std::string trailer_via_invidious(const std::string &query)
{
    const std::string q = url_encode(query);
    for (int i = 0; i < kTrailerInstances; ++i)
    {
        const std::string base = kTrailerInvidious[i];
        json sj;
        std::string err;
        if (!fetch_json(base + "/api/v1/search?q=" + q + "&type=video", sj, err, 5))
            continue;
        std::string vid;
        if (sj.is_array() && !sj.empty())
            vid = sj[0].value("videoId", "");
        if (vid.empty())
            continue;
        json vj;
        if (!fetch_json(base + "/api/v1/videos/" + url_encode(vid), vj, err, 5))
            continue;
        for (const char *key : {"formatStreams", "adaptiveFormats"})
        {
            if (!vj.contains(key) || !vj[key].is_array())
                continue;
            for (const json &f : vj[key])
            {
                const std::string u = f.value("url", "");
                if (u.compare(0, 4, "http") == 0)
                    return u;
            }
        }
    }
    return "";
}

std::string resolve_trailer_url(const std::string &query)
{
    std::string u = trailer_via_piped(query);
    if (!u.empty())
        return u;
    return trailer_via_invidious(query);
}

const char *const kResolutionOrder[] = {"4K", "1440p", "1080p", "720p", "SD", "Other"};

std::string resolution_of(const ::Stream &st)
{
    const std::string s = lower(st.name + " " + st.description + " " + st.filename);
    const auto has = [&](const char *w) { return s.find(w) != std::string::npos; };
    if (has("2160p") || has("4k") || has("uhd"))
        return "4K";
    if (has("1440p") || has("2k"))
        return "1440p";
    if (has("1080p") || has("1080i") || has("fhd"))
        return "1080p";
    if (has("720p"))
        return "720p";
    if (has("576p") || has("480p") || has("360p") || has("dvdrip") || has("dvdscr"))
        return "SD";
    return "Other";
}

int resolution_rank(const std::string &r)
{
    for (int i = 0; i < 6; ++i)
        if (r == kResolutionOrder[i])
            return i;
    return 5;
}

// "👤 142" in a description.
int seeds_of(const std::string &description)
{
    const std::size_t p = description.find("\xF0\x9F\x91\xA4");
    if (p == std::string::npos)
        return -1;
    return std::atoi(description.c_str() + p + 4);
}

// "💾 18.4 GB" in a description, else any number followed by a size unit.
std::string size_of(const std::string &description)
{
    std::size_t p = description.find("\xF0\x9F\x92\xBE");
    if (p != std::string::npos)
    {
        p += 4;
        while (p < description.size() && description[p] == ' ')
            ++p;
        std::string size;
        while (p < description.size() && description[p] != '\n' && description[p] != ' ' && static_cast<unsigned char>(description[p]) < 0x80)
            size += description[p++];
        if (p < description.size() && description[p] == ' ')
        {
            ++p;
            std::string unit;
            while (p < description.size() && std::isalpha(static_cast<unsigned char>(description[p])))
                unit += description[p++];
            if (!unit.empty())
                size += " " + unit;
        }
        return size;
    }
    const std::string l = lower(description);
    for (const char *unit : {" gb", " mb", " tb", " gib", " mib"})
    {
        const std::size_t u = l.find(unit);
        if (u == std::string::npos || u == 0)
            continue;
        std::size_t s = u;
        while (s > 0 && (std::isdigit(static_cast<unsigned char>(l[s - 1])) || l[s - 1] == '.' || l[s - 1] == ','))
            --s;
        if (s < u)
        {
            std::string out = description.substr(s, u - s) + " ";
            std::string up = std::string(unit + 1);
            for (char &c : up)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return out + up;
        }
    }
    return "";
}

std::string tags_of(const ::Stream &st)
{
    const std::string s = lower(st.name + " " + st.description + " " + st.filename);
    static const struct
    {
        const char *needle;
        const char *label;
    } kTags[] = {{"dolby vision", "Dolby Vision"}, {" dv", "Dolby Vision"}, {"hdr10+", "HDR10+"}, {"hdr10", "HDR10"}, {"hdr", "HDR"},
                 {"atmos", "Dolby Atmos"},          {"truehd", "TrueHD"},     {"dts-hd", "DTS-HD"}, {"dts", "DTS"},
                 {"7.1", "7.1"},                    {"5.1", "5.1"},           {"remux", "Remux"},   {"bluray", "BluRay"},
                 {"web-dl", "WEB-DL"},              {"webrip", "WEBRip"},     {"hevc", "HEVC"},     {"x265", "HEVC"},
                 {"h265", "HEVC"}};
    std::vector<std::string> out;
    for (const auto &t : kTags)
        if (s.find(t.needle) != std::string::npos && std::find(out.begin(), out.end(), t.label) == out.end())
            out.push_back(t.label);
    if (out.size() > 4)
        out.resize(4);
    return join(out, "  \xC2\xB7  ");
}

Stream view_of(const ::Stream &st)
{
    Stream v;
    v.resolution = resolution_of(st);
    const std::string sname = first_line(st.name);
    if (st.addon.empty())
        v.name = sname;
    else if (!sname.empty() && sname != st.addon)
        v.name = st.addon + "  \xC2\xB7  " + sname;
    else
        v.name = st.addon;
    // The release name: the file's own, else the first line of the description.
    v.detail = !st.filename.empty() ? st.filename : first_line(st.description);
    if (v.detail.empty())
        v.detail = first_line(st.name);
    v.tags = tags_of(st);
    v.size = size_of(st.description);
    const int seeds = seeds_of(st.description);
    if (seeds >= 0)
        v.seeds = std::to_string(seeds) + (seeds == 1 ? " seed" : " seeds");
    else if (!st.info_hash.empty())
        v.seeds = "torrent";
    return v;
}

// "2010-09-14T00:00:00.000Z" -> "Sep 14, 2010"
std::string date_label(const std::string &released)
{
    if (released.size() < 10 || released[4] != '-' || released[7] != '-')
        return released;
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int m = std::atoi(released.substr(5, 2).c_str());
    if (m < 1 || m > 12)
        return released.substr(0, 10);
    return std::string(months[m - 1]) + " " + std::to_string(std::atoi(released.substr(8, 2).c_str())) + ", " +
           released.substr(0, 4);
}

std::string episode_title(const ::Video &v)
{
    return v.title.empty() ? "Episode " + std::to_string(v.episode) : v.title;
}

} // namespace

Stream stream_view(const ::Stream &st)
{
    return view_of(st);
}

bool Session::watched(const std::string &video_id) const
{
    return watched_.count(video_id) > 0;
}

void Session::detail_toggle_watched(int index)
{
    if (index < 0 || index >= static_cast<int>(d_season_videos_.size()) ||
        index >= static_cast<int>(detail.episodes.size()))
        return;
    const std::string &id = d_season_videos_[static_cast<std::size_t>(index)]->id;
    if (watched_.count(id))
    {
        watched_.erase(id);
        say("Marked as not watched");
    }
    else
    {
        watched_.insert(id);
        say("Marked as watched");
    }
    save_progress();
    detail.episodes[static_cast<std::size_t>(index)].watched = watched(id);
    changed_ |= kDetail;
}

void Session::detail_more_actions()
{
    if (!d_open_)
        return;
    std::vector<std::string> opts;
    int idx_watched = -1, idx_remove = -1, idx_trailer = -1;
    if (detail.series && d_meta_loaded_)
    {
        bool all = true;
        for (const ::Video &v : d_meta_.videos)
            if (!watched_.count(v.id))
            {
                all = false;
                break;
            }
        opts.push_back(all ? "Mark series as unwatched" : "Mark series as watched");
        idx_watched = static_cast<int>(opts.size()) - 1;
    }
    if (signed_in() && !d_item_.id.empty())
    {
        const auto li = library_.find(d_item_.id);
        if (li != library_.end() && !jbool(li->second, "removed"))
        {
            opts.push_back("Remove from library");
            idx_remove = static_cast<int>(opts.size()) - 1;
        }
    }
    if (!detail.title.name.empty())
    {
        opts.push_back("Watch trailer");
        idx_trailer = static_cast<int>(opts.size()) - 1;
    }
    opts.push_back("Cancel");
    open_dropdown("More actions", opts, -1, [this, idx_watched, idx_remove, idx_trailer](int i) {
        if (i == idx_watched && idx_watched >= 0)
            detail_mark_series_watched();
        else if (i == idx_remove && idx_remove >= 0)
            detail_confirm_remove_library();
        else if (i == idx_trailer && idx_trailer >= 0)
        {
            // Trailer lookup: Piped then Invidious, resolved off the UI
            // thread. Plays through the normal play_stream path but "as" an
            // empty item, so the trailer can never pollute progress,
            // Continue Watching, or watched state.
            if (watching)
                return;
            const std::string name =
                d_meta_loaded_ && !d_meta_.name.empty() ? d_meta_.name : d_item_.name;
            if (name.empty())
            {
                say("No trailer found", 2.5, false);
                return;
            }
            std::string q = name;
            const std::string year = d_meta_loaded_ ? d_meta_.release_info : d_item_.release_info;
            if (!year.empty())
            {
                q += " ";
                q += year;
            }
            q += " trailer";
            const int gen = d_gen_;
            say("Finding trailer...", 2.0, false);
            bg<std::string>(
                [q]() { return resolve_trailer_url(q); },
                [this, gen](std::string &url) {
                    if (gen != d_gen_ || !d_open_)
                        return; // the page moved on while resolving
                    if (url.empty())
                    {
                        say("No trailer found", 2.5, false);
                        return;
                    }
                    if (watching)
                        return;
                    ::Stream tr;
                    tr.url = url;
                    tr.name = "Trailer";
                    tr.addon = "Trailer";
                    // Play "as" an empty item: watch_save_progress()
                    // early-returns on empty w_item_.id and the watched insert
                    // is skipped on empty w_video_id_. d_item_ is restored
                    // before returning; only the synchronous prefix of
                    // play_stream ever sees the empty ids.
                    const std::string saved_id = d_item_.id;
                    const std::string saved_video = d_video_id_;
                    d_item_.id.clear();
                    d_video_id_.clear();
                    play_stream(tr, false);
                    d_item_.id = saved_id;
                    d_video_id_ = saved_video;
                    progress_.erase("");
                    save_progress();
                });
        }
    });
}

void Session::detail_mark_series_watched()
{
    if (!d_meta_loaded_ || d_meta_.videos.empty())
        return;
    bool all = true;
    for (const ::Video &v : d_meta_.videos)
        if (!watched_.count(v.id))
        {
            all = false;
            break;
        }
    for (const ::Video &v : d_meta_.videos)
    {
        if (all)
            watched_.erase(v.id);
        else
            watched_.insert(v.id);
    }
    save_progress();
    for (std::size_t i = 0; i < d_season_videos_.size() && i < detail.episodes.size(); ++i)
        detail.episodes[i].watched = watched(d_season_videos_[i]->id);
    changed_ |= kDetail;
    say(all ? "Marked series as unwatched" : "Marked series as watched");
}

void Session::detail_confirm_remove_library()
{
    open_dropdown("Remove \"" + d_item_.name + "\" from your library?", {"Remove", "Cancel"}, 1,
                  [this](int i) {
                      if (i == 0)
                          detail_remove_library();
                  });
}

void Session::detail_remove_library()
{
    const std::string id = d_item_.id;
    if (id.empty())
        return;
    library_.erase(id);
    progress_.erase(id);
    save_progress();
    if (signed_in())
    {
        const std::string key = prefs_.auth_key;
        const json changes =
            json::array({{{"_id", id}, {"removed", true}, {"_mtime", iso8601_now()}}});
        bg<ApiResult>([key, changes]() { return api_library_put(key, changes); },
                      [this](ApiResult &r) {
                          if (!r.ok)
                              say("Could not update your library: " + r.error, 5.0, true);
                      });
    }
    say("Removed from library");
    refresh_library();
    refresh_board();
}

// ---------------------------------------------------------------------------

void Session::open_title(const Title &t, const std::string &streams_video, bool force_detail)
{
    const Item *found = nullptr;
    const auto look = [&](const std::vector<Item> &items) {
        if (found)
            return;
        for (const Item &it : items)
            if (it.id == t.id && (t.type.empty() || it.type == t.type))
            {
                found = &it;
                return;
            }
    };
    for (const BoardRow &br : board_)
        look(br.items);
    for (const BoardRow &br : search_)
        look(br.items);
    look(disc_items_);
    look(lib_items_);
    Item it;
    if (found)
    {
        it = *found;
    }
    else
    {
        it.id = t.id;
        it.type = t.type;
        it.name = t.name;
    }

    d_item_ = it;
    d_meta_ = ::Meta();
    d_meta_loaded_ = false;
    d_season_videos_.clear();
    d_seasons_.clear();
    d_streams_.clear();
    d_all_streams_.clear();
    d_found_.clear();
    d_stream_addon_ = 0;
    d_video_id_.clear();
    ++d_gen_;
    ++d_meta_gen_;
    d_open_ = true;
    want_streams_video_ = streams_video;
    resume_play_ = false;
    force_detail_ = force_detail;
    autobest_pending_ = false;

    detail = DetailContent();
    // The library/catalog item may carry no backdrop or description: fill the
    // gaps from the speculative meta cache before painting.
    apply_meta(d_item_);
    detail_fill_title();
    detail.series = it.type == "series";
    detail.episodes_status = "Loading episodes...";
    detail.streams_status = "";
    // Cached episode list (meta_videos_, keyed "type:id" like meta_cache_):
    // the episodes are known before any addon answers, so paint them now.
    if (const auto vit = meta_videos_.find(d_item_.type + ":" + d_item_.id);
        vit != meta_videos_.end() && !vit->second.empty())
    {
        d_meta_.videos = vit->second;
        d_meta_loaded_ = true;
        detail_apply_meta();
    }
    changed_ |= kDetail;
    art_dirty_ = true;

    // The details come from the first addon that has them.
    std::vector<std::string> urls;
    for (const auto &a : addons_)
        if (!addon_disabled(*a) && a->supports("meta", it.type, it.id))
            urls.push_back(a->resource_url("meta", it.type, it.id));
    const int gen = d_meta_gen_;
    struct Res
    {
        bool ok = false;
        ::Meta meta;
        std::string error;
    };
    bg<Res>(
        [urls]() {
            Res r;
            // Merge across addons: each parsed meta fills whichever of the
            // needed fields are still empty; stop early once everything is
            // filled (or all addons are done). One addon rarely has it all
            // (e.g. videos from one, description from another).
            const auto filled = [&]() {
                return !r.meta.description.empty() && !r.meta.background.empty() &&
                       !r.meta.logo.empty() && !r.meta.videos.empty() &&
                       !r.meta.cast.empty() && !r.meta.directors.empty();
            };
            for (const auto &u : urls)
            {
                json j;
                std::string err;
                if (!fetch_json(u, j, err))
                {
                    if (r.error.empty())
                        r.error = err;
                    continue;
                }
                const json &m = jobj(j, "meta");
                if (!m.is_object())
                    continue;
                const ::Meta pm = parse_meta(m);
                // Identity fields: the first addon that has them wins.
                if (r.meta.id.empty())
                {
                    r.meta.id = pm.id;
                    r.meta.type = pm.type;
                    r.meta.name = pm.name;
                    r.meta.poster = pm.poster;
                    r.meta.release_info = pm.release_info;
                    r.meta.runtime = pm.runtime;
                    r.meta.imdb_rating = pm.imdb_rating;
                    r.meta.genres = pm.genres;
                    r.meta.writers = pm.writers;
                }
                if (r.meta.description.empty())
                    r.meta.description = pm.description;
                if (r.meta.background.empty())
                    r.meta.background = pm.background;
                if (r.meta.logo.empty())
                    r.meta.logo = pm.logo;
                if (r.meta.videos.empty() && !pm.videos.empty())
                    r.meta.videos = pm.videos;
                if (r.meta.cast.empty() && !pm.cast.empty())
                    r.meta.cast = pm.cast;
                if (r.meta.directors.empty() && !pm.directors.empty())
                    r.meta.directors = pm.directors;
                r.ok = true;
                if (filled())
                    break;
            }
            if (urls.empty())
                r.error = "No addon has details for this";
            return r;
        },
        [this, gen](Res &r) {
            if (gen != d_meta_gen_ || !d_open_)
                return;
            if (!r.ok)
            {
                detail.episodes_status = "Could not load the details: " + r.error;
                // Without details it can still be played as one video.
                detail.series = false;
                detail_load_streams(d_item_.id, "Streams");
                // Instant Board: the meta fetch failed (most likely offline) but
                // the cached title stays on screen — detail.title is left
                // intact above. detail_load_streams resets streams_status, so
                // the offline note goes after it; it is replaced as soon as
                // the stream addons actually answer.
                detail.streams_status = "Offline — cached details shown. Connect to the internet to load streams.";
                if (prefs_.autoplay_best && !resume_play_ && !force_detail_ && want_streams_video_.empty())
                    autobest_pending_ = true;
                changed_ |= kDetail;
                if (resume_play_)
                    start_remembered_stream();
                return;
            }
            d_meta_ = r.meta;
            if (d_meta_.id.empty())
                d_meta_.id = d_item_.id;
            if (d_meta_.type.empty())
                d_meta_.type = d_item_.type;
            d_meta_loaded_ = true;
            detail_apply_meta();
            if (resume_play_)
                start_remembered_stream();
        });

    // A film: its streams right away. Skipped when the cache-hit path above
    // already loaded them (d_video_id_ set by detail_load_streams), so the
    // addons are not asked twice for the same film.
    if (!detail.series && d_video_id_.empty())
        detail_load_streams(it.id, "Streams");
}

// Continue Watching, "Play now": the details are in, so the stream that was playing is played again.
void Session::start_remembered_stream()
{
    resume_play_ = false;
    ::Stream s;
    // A series' stream belongs to one episode; a film has one video, whatever it is called.
    const bool series = d_item_.type == "series";
    if (saved_stream(d_item_.id, series ? resume_video_ : std::string(), &s))
    {
        const auto p = progress_.find(d_item_.id);
        d_video_id_ = series || p == progress_.end() || p->second.stream_video_id.empty() ? resume_video_
                                                                                          : p->second.stream_video_id;
        play_stream(s, false);
        return;
    }
    // Nothing usable remembered after all: leave the launch screen, the page shows the streams.
    resuming_ = false;
    resumed_from_board_ = false;
    watch_stop(false);
}
void Session::close_detail()
{
    // WORKER 10 (polish): remember where the grid was so coming back lands on
    // the same card (the grid scrolls the focused card into view, which also
    // restores the scroll position). The cursor behind the detail does not
    // move while it is open, so the current cursor is the position to keep.
    // Saved for every grid the detail can open from; restore_grid_pos is
    // called by the browse grid (WORKER 2).
    if (d_open_)
    {
        const Cursor &c = last_cursor_;
        NavMem::save_grid_pos("board", c.board_row, c.board_col);
        NavMem::save_grid_pos("discover", c.discover / 4, c.discover % 4);
        NavMem::save_grid_pos("library", c.library / 6, c.library % 6);
        NavMem::save_grid_pos("search", c.search_row, c.search_col);
    }
    ++d_gen_;
    ++d_meta_gen_;
    d_open_ = false;
}

void Session::detail_fill_title()
{
    Title &t = detail.title;
    const ::Meta &m = d_meta_;
    t.id = d_item_.id;
    t.type = d_item_.type;
    t.name = d_meta_loaded_ && !m.name.empty() ? m.name : d_item_.name;
    t.year = d_meta_loaded_ ? m.release_info : d_item_.release_info;
    t.runtime = d_meta_loaded_ ? m.runtime : d_item_.runtime;
    t.genres = join(d_meta_loaded_ ? m.genres : d_item_.genres, ", ");
    t.imdb = d_meta_loaded_ ? m.imdb_rating : d_item_.imdb_rating;
    t.synopsis = d_meta_loaded_ ? m.description : d_item_.description;
    if (d_meta_loaded_)
    {
        std::vector<std::string> cast = m.cast;
        if (cast.size() > 6)
            cast.resize(6);
        detail.cast = join(cast, ", ");
        detail.cast_names = cast;
        detail.directors = join(m.directors, ", ");
        detail.director_names = m.directors;
        detail.writers = join(m.writers, ", ");
    }
    else
    {
        detail.director_names.clear();
        detail.cast_names.clear();
    }
    // Cast/director search: confirming a focused name on the detail page
    // opens Search for that person. start_search() flips search_open and the
    // shell picks it up on the next sync.
    detail.on_person_search = [this](const std::string &name) {
        if (!name.empty() && d_open_)
            start_search(name);
    };
}

void Session::detail_apply_meta()
{
    const ::Meta &m = d_meta_;
    if (d_item_.poster.empty())
        d_item_.poster = m.poster;
    if (d_item_.background.empty())
        d_item_.background = m.background;
    detail_fill_title();

    // Series-like when there are several videos (series, channels, anime).
    bool series = m.type != "movie" && m.videos.size() > 1;
    if (m.type == "series" && !m.videos.empty())
        series = true;
    if (series)
    {
        detail.series = true;
        d_seasons_.clear();
        for (const auto &v : m.videos)
            if (std::find(d_seasons_.begin(), d_seasons_.end(), v.season) == d_seasons_.end())
                d_seasons_.push_back(v.season);
        // Specials (season 0) last.
        std::stable_sort(d_seasons_.begin(), d_seasons_.end(), [](int a, int b) {
            if (a == 0)
                return false;
            if (b == 0)
                return true;
            return a < b;
        });
        detail.seasons.clear();
        for (int s : d_seasons_)
            detail.seasons.push_back(s == 0 ? "Specials" : "Season " + std::to_string(s));
        // Start on the season being watched, else the first.
        int season_idx = 0;
        std::string resume_video;
        for (const Item &cw : continue_watching())
            if (cw.id == d_item_.id)
                resume_video = cw.video_id;
        if (resume_video.empty() && progress_.count(d_item_.id))
            resume_video = progress_[d_item_.id].video_id;
        int want_episode = -1;
        for (const auto &v : m.videos)
            if (v.id == resume_video)
            {
                for (std::size_t i = 0; i < d_seasons_.size(); ++i)
                    if (d_seasons_[i] == v.season)
                        season_idx = static_cast<int>(i);
                want_episode = v.episode;
            }
        detail_set_season(season_idx);
        if (want_episode >= 0)
            for (std::size_t i = 0; i < d_season_videos_.size(); ++i)
                if (d_season_videos_[i]->episode == want_episode)
                {
                    detail.episode_hint = static_cast<int>(i);
                    ++detail.hint_serial;
                }
        // Asked for from Continue Watching: that episode's streams, without the list of episodes.
        if (!want_streams_video_.empty() && !resume_play_)
        {
            for (const auto &v : m.videos)
                if (v.id == want_streams_video_)
                {
                    std::string heading = "S" + std::to_string(v.season) + " E" + std::to_string(v.episode);
                    if (!v.title.empty())
                        heading += "  \xC2\xB7  " + v.title;
                    detail_load_streams(v.id, heading);
                    ++detail.streams_serial;
                    break;
                }
        }
        want_streams_video_.clear();
    }
    else
    {
        const bool was_series = detail.series;
        detail.series = false;
        detail.episodes.clear();
        const std::string vid = m.videos.size() == 1 ? m.videos[0].id : m.id;
        if (was_series || vid != d_video_id_)
            detail_load_streams(vid, "Streams");
        if (prefs_.autoplay_best && !resume_play_ && !force_detail_ && want_streams_video_.empty())
            autobest_pending_ = true;
        // The streams may already be fully loaded (meta arrived after the
        // addons answered); the arm above would then never fire because no
        // further stream event arrives. Nudge the publish path once so a
        // completed load auto-plays immediately. detail_refresh_streams()
        // is idempotent: republishing the final order changes nothing visible.
        if (autobest_pending_ && d_answered_ >= d_pending_)
            detail_refresh_streams();
    }
    changed_ |= kDetail;
    art_dirty_ = true;
}

void Session::detail_set_season(int index)
{
    if (d_seasons_.empty())
        return;
    index = std::max(0, std::min(index, static_cast<int>(d_seasons_.size()) - 1));
    d_season_idx_ = index;
    detail.season = index;
    const int season = d_seasons_[static_cast<std::size_t>(index)];
    d_season_videos_.clear();
    for (const auto &v : d_meta_.videos)
        if (v.season == season)
            d_season_videos_.push_back(&v);
    // What was left half-watched, shown on its episode.
    std::string resume_video;
    float resume_progress = -1.0f;
    for (const Item &cw : continue_watching())
        if (cw.id == d_item_.id && cw.duration > 0)
        {
            resume_video = cw.video_id;
            resume_progress = static_cast<float>(std::min(1.0, cw.offset / cw.duration));
        }
    detail.episodes.clear();
    int first_unwatched = -1;
    for (std::size_t i = 0; i < d_season_videos_.size(); ++i)
    {
        const ::Video *v = d_season_videos_[i];
        Episode e;
        e.number = v->episode;
        e.title = episode_title(*v);
        e.info = date_label(v->released);
        // Unaired future episodes: badge them so users don't wonder why
        // there are no streams.
        const std::int64_t rel_ms = iso8601_to_ms(v->released);
        if (rel_ms > now_epoch_ms() + 86400000LL)
            e.info = e.info.empty() ? "Upcoming" : ("Upcoming  \xC2\xB7  " + e.info);
        e.overview = v->overview;
        e.watched = watched(v->id);
        if (v->id == resume_video)
            e.progress = resume_progress;
        detail.episodes.push_back(e);
        if (!e.watched && first_unwatched < 0)
            first_unwatched = static_cast<int>(i);
    }
    detail.episode_hint = first_unwatched >= 0 ? first_unwatched : 0;
    ++detail.hint_serial;
    detail.episodes_status = detail.episodes.empty() ? "No episodes in this season." : "";
    changed_ |= kDetail;
    art_dirty_ = true;
}

void Session::detail_change_season(int delta)
{
    if (d_seasons_.size() <= 1)
        return;
    // Wrap around: L1 on the first season goes to the last, R1 on the last
    // goes to the first.
    const int n = static_cast<int>(d_seasons_.size());
    const int next = (d_season_idx_ + (delta % n) + n) % n;
    detail_set_season(next);
}

void Session::detail_pick_episode(int index)
{
    if (index < 0 || index >= static_cast<int>(d_season_videos_.size()))
        return;
    const ::Video *v = d_season_videos_[static_cast<std::size_t>(index)];
    std::string heading = "S" + std::to_string(v->season) + " E" + std::to_string(v->episode);
    if (!v->title.empty())
        heading += "  \xC2\xB7  " + v->title;
    detail_load_streams(v->id, heading);
    if (prefs_.autoplay_best && !resume_play_ && want_streams_video_.empty())
        autobest_pending_ = true;
}

void Session::detail_load_streams(const std::string &video_id, const std::string &heading)
{
    const int gen = ++d_gen_;
    d_video_id_ = video_id;
    d_stream_heading_ = heading;
    d_found_.clear();
    d_streams_.clear();
    d_all_streams_.clear();
    d_stream_addon_ = 0;
    detail.streams.clear();
    detail.stream_addons.clear();
    detail.stream_addon = 0;
    detail.stream_heading = heading;
    d_answered_ = 0;
    prewarmed_key_.clear();

    std::string type = d_meta_loaded_ ? d_meta_.type : d_item_.type;
    if (type.empty())
        type = d_item_.type;
    std::vector<std::pair<std::string, std::string>> sources; // addon name, url
    for (const auto &a : addons_)
        if (!addon_disabled(*a) && a->supports("stream", type, video_id))
            sources.push_back({a->name, a->resource_url("stream", type, video_id)});
    d_pending_ = static_cast<int>(sources.size());
    if (sources.empty())
    {
        detail.streams_status =
            addons_loading_ ? "Addons are still loading."
            : !prefs_.disabled_addons.empty()
                ? "None of your enabled addons provide streams for this. Enable a stream addon on the Addons page, "
                  "or install one (for example Torrentio) in your Stremio account."
                : "None of your addons provide streams for this. Install a stream addon "
                  "(for example Torrentio) in your Stremio account.";
        changed_ |= kDetail;
        return;
    }
    detail.streams_status = "Loading streams... (0 of " + std::to_string(sources.size()) + " addons answered)";
    for (const auto &s : sources)
    {
        const std::string name = s.first, url = s.second;
        struct Res
        {
            std::vector<::Stream> streams;
            std::string error;
        };
        bg<Res>(
            [name, url]() {
                Res r;
                json j;
                if (fetch_json(url, j, r.error, 30))
                    r.streams = parse_streams(j, name);
                return r;
            },
            [this, gen, name](Res &r) {
                if (gen != d_gen_)
                    return;
                ++d_answered_;
                if (!r.error.empty())
                    dlog("streams from %s: %s", name.c_str(), r.error.c_str());
                for (auto &st : r.streams)
                    d_found_.push_back(st);
                detail_refresh_streams();
            });
    }
    changed_ |= kDetail;
}

// Instant Season-Pack Autoplay: fingerprint the playing stream so the next
// episode's streams can be matched to it (same release pack, else same
// addon + resolution + name).
void Session::arm_autoplay_from(const ::Stream &s)
{
    autoplay_pending_ = true;
    autoplay_binge_ = s.binge_group;
    autoplay_addon_ = s.addon;
    autoplay_hash_ = s.info_hash;
    autoplay_res_ = resolution_of(s);
    autoplay_name_ = first_line(s.name);
}

// Pick the next episode's stream matching the armed fingerprint. ranked is
// sorted best-first. Tiers:
//  1. exact info_hash or binge_group match (fires even before all addons answer),
//  2. same addon + same resolution + same first-line name,
//  3. same addon + same resolution,
//  4. same addon, else the first seed-healthy stream, else the first stream.
// Tiers 2-4 skip only streams positively known dead (seeds_of explicitly
// parsed "0 seeds"); seeds_of returns -1 for HTTP/Debrid streams with no
// seed count, and those stay eligible.
int Session::pick_autoplay_stream(const std::vector<::Stream> &ranked, bool all_answered) const
{
    if (ranked.empty())
        return -1; // no "first stream" exists; the caller clears the pending state
    const auto seed_healthy = [](const ::Stream &st) { return seeds_of(st.description) != 0; };
    // Tier 1: the exact pack / release continues.
    for (std::size_t i = 0; i < ranked.size(); ++i)
    {
        if (!autoplay_hash_.empty() && ranked[i].info_hash == autoplay_hash_)
            return static_cast<int>(i);
        if (!autoplay_binge_.empty() && ranked[i].binge_group == autoplay_binge_)
            return static_cast<int>(i);
    }
    // Tier 2: same addon + resolution + first-line name.
    for (std::size_t i = 0; i < ranked.size(); ++i)
        if (ranked[i].addon == autoplay_addon_ && resolution_of(ranked[i]) == autoplay_res_ &&
            first_line(ranked[i].name) == autoplay_name_ && seed_healthy(ranked[i]))
            return static_cast<int>(i);
    // Tier 3: same addon + resolution.
    for (std::size_t i = 0; i < ranked.size(); ++i)
        if (ranked[i].addon == autoplay_addon_ && resolution_of(ranked[i]) == autoplay_res_ &&
            seed_healthy(ranked[i]))
            return static_cast<int>(i);
    if (!all_answered)
        return -1; // wait for the remaining addons before falling back
    // Tier 4: same addon; else the first seed-healthy stream; else the first stream.
    for (std::size_t i = 0; i < ranked.size(); ++i)
        if (ranked[i].addon == autoplay_addon_ && seed_healthy(ranked[i]))
            return static_cast<int>(i);
    for (std::size_t i = 0; i < ranked.size(); ++i)
        if (seed_healthy(ranked[i]))
            return static_cast<int>(i);
    return 0;
}

void Session::detail_refresh_streams()
{
    // Best picture first, then the better seeded. This ranking is the final
    // order; while addons are still answering, the visible list is frozen
    // instead (new arrivals go to the bottom in arrival order) so rows never
    // move under the cursor mid-browse. The single final sort happens below.
    std::vector<std::size_t> order(d_found_.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    // The user's addon order breaks quality/seed ties: an earlier addon wins.
    std::map<std::string, std::size_t> addon_prio;
    for (std::size_t i = 0; i < addons_.size(); ++i)
        addon_prio[addons_[i]->name] = i;
    const std::size_t prio_last = addons_.size();
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const int ra = resolution_rank(resolution_of(d_found_[a])), rb = resolution_rank(resolution_of(d_found_[b]));
        if (ra != rb)
            return ra < rb;
        const int sa = seeds_of(d_found_[a].description), sb = seeds_of(d_found_[b].description);
        if (sa != sb)
        {
            // seeds_of returns -1 for HTTP streams (no seed count) and 0 for
            // dead torrents. A working HTTP stream outranks a 0-seed torrent,
            // but any seeded torrent outranks HTTP.
            if (sa < 0 && sb == 0)
                return true;
            if (sb < 0 && sa == 0)
                return false;
            return sa > sb;
        }
        const auto ia = addon_prio.find(d_found_[a].addon);
        const auto ib = addon_prio.find(d_found_[b].addon);
        const std::size_t pa = ia == addon_prio.end() ? prio_last : ia->second;
        const std::size_t pb = ib == addon_prio.end() ? prio_last : ib->second;
        return pa < pb;
    });
    std::vector<::Stream> ranked;
    ranked.reserve(order.size());
    for (std::size_t i : order)
        ranked.push_back(d_found_[i]);
    // Max-resolution cap: drop streams sharper than the cap so the picker and
    // auto-play prefer smaller files. "Other" (unknown) always passes; if the
    // cap would leave nothing, fall back to the full list rather than showing
    // "No streams found".
    if (prefs_.max_resolution != "auto" && prefs_.max_resolution != "4k")
    {
        const int min_rank = prefs_.max_resolution == "1080p" ? 2 : prefs_.max_resolution == "720p" ? 3 : 4;
        std::vector<::Stream> capped;
        capped.reserve(ranked.size());
        for (const ::Stream &st : ranked)
            if (resolution_rank(resolution_of(st)) >= min_rank)
                capped.push_back(st);
        if (!capped.empty())
            ranked = std::move(capped);
    }
    const bool all = d_answered_ >= d_pending_;
    if (all)
    {
        // Loading finished: publish the final sorted order, once.
        d_all_streams_ = ranked;
        // Build addon tabs from distinct addon names (first-seen order = user priority).
        std::vector<std::string> addons;
        for (const ::Stream &st : ranked)
        {
            if (st.addon.empty())
                continue;
            bool seen = false;
            for (const auto &a : addons)
                if (a == st.addon) { seen = true; break; }
            if (!seen)
                addons.push_back(st.addon);
        }
        if (addons.size() >= 2)
        {
            detail.stream_addons.clear();
            detail.stream_addons.push_back("All");
            for (const auto &a : addons)
                detail.stream_addons.push_back(a);
            if (d_stream_addon_ >= static_cast<int>(detail.stream_addons.size()))
                d_stream_addon_ = 0;
        }
        else
        {
            detail.stream_addons.clear();
            d_stream_addon_ = 0;
        }
        detail.stream_addon = d_stream_addon_;
        // Apply the addon tab filter.
        d_streams_.clear();
        detail.streams.clear();
        const std::string &want = (d_stream_addon_ > 0 && d_stream_addon_ < static_cast<int>(detail.stream_addons.size()))
                                      ? detail.stream_addons[static_cast<std::size_t>(d_stream_addon_)]
                                      : std::string();
        for (const ::Stream &st : ranked)
        {
            if (!want.empty() && st.addon != want)
                continue;
            d_streams_.push_back(st);
            detail.streams.push_back(view_of(st));
        }
        // Worker 9 (network): page-open stream resolution — the top-ranked
        // stream's playable URL is resolved now, in the background, and
        // cached for the session, so pressing Play reuses it instead of
        // waiting (torrent metadata can take minutes on few seeders).
        detail_preresolve_top_stream();
    }
    else
    {
        // Still loading: append only the arrivals since the last publish, in
        // arrival order. d_streams_ holds exactly the first d_streams_.size()
        // entries of d_found_ here (detail_load_streams clears all three, and
        // this branch only ever appends), so no published row ever moves.
        for (std::size_t i = d_streams_.size(); i < d_found_.size(); ++i)
        {
            d_streams_.push_back(d_found_[i]);
            detail.streams.push_back(view_of(d_found_[i]));
        }
    }
    // Instant pre-warm + prefetch: as soon as the first addon returns a
    // playable HTTP/Debrid stream (even before all addons answer), warm its
    // connection and prefetch its first bytes on a worker thread, so pressing
    // Cross starts playback in <1s. HTTP-only and side-effect-free; torrents
    // are resolved by detail_preresolve_top_stream() instead. Once per detail
    // session: prewarmed_key_ is cleared by detail_load_streams(), and a
    // torrent-only first arrival does not consume the guard.
    if (!ranked.empty() && !watching)
    {
        const ::Stream &top = ranked.front();
        if (!top.url.empty() && top.url != prewarmed_key_)
        {
            prewarmed_key_ = top.url;
            g_tasks.run([u = top.url, h = top.request_headers] {
                NetStream::prewarm(u, h);
            });
        }
    }
    // The next episode: a stream like the last one once the streams are in
    // (exact info_hash / binge group first, then same addon + resolution +
    // name, then same addon + resolution, then same addon, else the first).
    if (autoplay_pending_)
    {
        const int pick = pick_autoplay_stream(ranked, all);
        if (pick >= 0)
        {
            autoplay_pending_ = false;
            autobest_pending_ = false;
            w_stream_idx_ = pick;
            w_failover_tries_ = 0;
            play_stream(ranked[static_cast<std::size_t>(pick)], false, watching);
        }
        else if (all)
        {
            autoplay_pending_ = false;
            w_binge_auto_ = false;
        }
    }
    // Auto-play best stream: the user pressed play (not "choose another
    // stream"), so once loading completes the best stream plays instead of
    // the picker. ranked is sorted best-first; explicitly dead (zero-seed)
    // torrents are skipped unless nothing else exists.
    if (autobest_pending_ && !autoplay_pending_ && all)
    {
        autobest_pending_ = false;
        int pick = -1;
        for (std::size_t i = 0; i < ranked.size() && pick < 0; ++i)
            if (seeds_of(ranked[i].description) != 0)
                pick = static_cast<int>(i);
        if (pick < 0 && !ranked.empty())
            pick = 0;
        if (pick >= 0)
        {
            w_stream_idx_ = pick;
            w_failover_tries_ = 0;
            play_stream(ranked[static_cast<std::size_t>(pick)], false);
        }
    }
    if (d_answered_ < d_pending_)
        detail.streams_status = "Loading streams... (" + std::to_string(d_answered_) + " of " + std::to_string(d_pending_) +
                                " addons answered)";
    else if (detail.streams.empty())
        detail.streams_status = "No streams found. Install a stream addon (for example Torrentio) in your Stremio account.";
    else
        detail.streams_status.clear();
    changed_ |= kDetail;
}

void Session::detail_change_stream_addon(int delta)
{
    if (detail.stream_addons.size() <= 1)
        return;
    const int n = static_cast<int>(detail.stream_addons.size());
    d_stream_addon_ = (d_stream_addon_ + (delta % n) + n) % n;
    detail_refresh_streams();
    cue(hui::audio::Cue::tab);
}

// Worker 9 (network): the detail-page-OPEN stream-resolution trigger. The
// expensive part of pressing Play on a torrent is the metadata wait (peers
// may take minutes) or the server's /create; do it now, while the user
// browses. Shares Worker 3's speculation cache (spec_resolve_begin): a
// Play-button-focus or 80% pre-resolve for the same stream is reused, never
// double-resolved, and the Play press takes the result via
// spec_resolve_take. Only the top-ranked stream — never speculative
// season-wide fetching — and never while something plays. HTTP streams need
// nothing (their connection is already pre-warmed above).
void Session::detail_preresolve_top_stream()
{
    if (d_streams_.empty() || watching)
        return;
    const ::Stream &top = d_streams_.front();
    if (!top.url.empty())
        return; // direct link: resolves instantly; the pre-warm above did the work
    if (top.info_hash.empty())
        return;
    int season = -1, episode = -1;
    for (const auto &v : d_meta_.videos)
        if (v.id == d_video_id_)
        {
            season = v.season;
            episode = v.episode;
        }
    // A long TTL: the user may browse the page for minutes before pressing
    // Play, and the warmed torrent/server state stays valid past the press.
    spec_resolve_begin(top, season, episode, 600.0);
}

} // namespace sx
