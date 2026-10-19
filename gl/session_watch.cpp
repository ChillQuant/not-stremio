// Playing: starting a stream (the app's own torrent engine, a streaming server or a
// direct link), the player's state for the screens, tracks, subtitles, and what
// was watched.

#include "session.hpp"
#include "stream_json.hpp"
#include "system_status.hpp"
#include "videoout_rate.h"
#include "yuv_convert.h"

#include "http.h"
#include "netstream.h"
#include "skipdb.h"
#include "torrent/engine.h"
#include "torrent/torrent_stream.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <thread>

namespace sx
{

namespace
{

constexpr double kInfoSeconds = 5.0;
constexpr double kSaveEvery = 30.0;

// The hardware decoder's refusal reason in plain words, or "" when it doesn't
// map cleanly: raw decoder jargon must never reach the screen.
std::string plain_hw_reason(const std::string &note)
{
    if (note == "HEVC coded in tiles") return note;
    if (note == "10-bit H.264" || note == "10-bit VP9" || note == "12-bit HEVC") return note;
    if (note == "decoder busy") return "the hardware decoder was busy";
    if (note == "no decoder module") return "the hardware decoder wasn't available";
    if (note == "decoder set-up failed") return "the hardware decoder failed to start";
    if (note == "pixel format isn't 4:2:0") return "its pixel format isn't supported";
    if (note == "bigger than 4K") return "it's larger than the hardware decoder supports";
    if (note.find("profile") != std::string::npos) return "its encoding profile isn't supported";
    const std::string tail = "has no hardware decoder";
    const std::size_t p = note.find(tail);
    if (p != std::string::npos)
    {
        const std::string codec = trim(note.substr(0, p));
        if (!codec.empty()) return "the PS5 has no " + codec + " hardware decoder";
    }
    return "";
}

std::string random_id()
{
    static const char *hex = "0123456789abcdef";
    std::string s;
    std::uint64_t x = static_cast<std::uint64_t>(now_epoch_ms()) * 6364136223846793005ULL + static_cast<std::uint64_t>(std::rand());
    for (int i = 0; i < 16; ++i)
    {
        s += hex[x & 15];
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    return s;
}

// A cue's markup (<em>, <strong>, <br/>, entities) as the plain lines the player draws.
std::string plain_cue(const std::string &rml, int *fg = nullptr)
{
    // Strips RML to plain text. When `fg` is given it receives the cue's color
    // as 0xRRGGBB when the whole cue uses one <span style="color:..."> color,
    // -1 when the cue is uncolored or mixes colors (the OSD draws one color
    // per cue, so a mixed cue keeps the default rather than guessing wrong).
    std::string out;
    int color = -1;
    bool mixed = false;
    for (std::size_t i = 0; i < rml.size();)
    {
        if (rml[i] == '<')
        {
            const std::size_t end = rml.find('>', i);
            if (end == std::string::npos)
                break;
            if (rml.compare(i, 3, "<br") == 0)
                out += '\n';
            else if (!mixed)
            {
                const std::string tag = rml.substr(i, end - i + 1);
                const std::string key = "color:#";
                const std::size_t kp = tag.find(key);
                if (kp != std::string::npos && kp + key.size() + 6 <= tag.size())
                {
                    unsigned v = 0;
                    if (std::sscanf(tag.c_str() + kp + key.size(), "%6x", &v) == 1)
                    {
                        if (color < 0)
                            color = static_cast<int>(v);
                        else if (color != static_cast<int>(v))
                            mixed = true;
                    }
                }
            }
            i = end + 1;
        }
        else if (rml[i] == '&')
        {
            const std::size_t end = rml.find(';', i);
            const std::string entity = end == std::string::npos ? "" : rml.substr(i, end - i + 1);
            if (entity == "&amp;")
                out += '&';
            else if (entity == "&lt;")
                out += '<';
            else if (entity == "&gt;")
                out += '>';
            else if (entity == "&quot;")
                out += '"';
            else if (entity == "&apos;" || entity == "&#39;")
                out += '\'';
            else if (entity == "&#123;")
                out += '{';
            else if (entity == "&#125;")
                out += '}';
            else
            {
                out += rml[i];
                ++i;
                continue;
            }
            i = end + 1;
        }
        else
        {
            out += rml[i++];
        }
    }
    if (fg)
        *fg = mixed ? -1 : color;
    return out;
}

} // namespace

std::string Session::server() const
{
    std::string s = trim(prefs_.server_url);
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    if (!s.empty() && !starts_with(s, "http://") && !starts_with(s, "https://"))
        s = "http://" + s;
    return s;
}

std::vector<std::string> Session::pref_langs(const std::string &list) const
{
    std::vector<std::string> out;
    for (const auto &part : split(list, ','))
    {
        const std::string p = trim(part);
        if (p.empty() || p == "-")
            continue;
        out.push_back(language_to_iso639_2(p));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Starting a stream

void Session::detail_play(int index, bool with_server)
{
    if (index < 0 || index >= static_cast<int>(d_streams_.size()))
        return;
    autoplay_pending_ = false;
    autobest_pending_ = false;
    w_binge_count_ = 0; // the user picked this stream themselves
    w_binge_auto_ = false;
    // Arm automatic failover: this play request came from d_streams_[index].
    w_stream_idx_ = with_server ? -1 : index;
    w_failover_tries_ = 0;
    play_stream(d_streams_[static_cast<std::size_t>(index)], with_server);
}

// Automatic stream failover: when a stream dies during the Launch screen,
// silently try the next candidate instead of dumping the user to the menu.
// Only eligible while the movie hasn't started (launch phase).
bool Session::try_failover_stream(const std::string &why)
{
    if (w_started_)
        return false; // mid-movie failure: the user handles it
    if (w_stream_idx_ < 0 || w_failover_tries_ >= kMaxFailoverTries)
        return false;
    if (!d_open_ || w_item_.id != d_item_.id)
        return false; // the detail page moved on: stale failure
    const int next_idx = w_stream_idx_ + 1;
    if (next_idx >= static_cast<int>(d_streams_.size()))
        return false; // nothing left to try
    ++w_failover_tries_;
    w_stream_idx_ = next_idx;
    const ::Stream next_st = d_streams_[static_cast<std::size_t>(next_idx)];
    dlog("watch: stream #%d failed (%s); auto-failing over to stream #%d (%s)", next_idx - 1,
         why.c_str(), next_idx, next_st.name.c_str());
    // Cleanly stop the failed attempt without leaving watching/detail state.
    watching = false;
    launch_open = false;
    player_.close();
    // Drop the failed attempt's frame: the next attempt starts from the
    // title art, never from a dead stream's last picture.
    // Drop the failed attempt's frame: the next attempt starts from the
    // title art, never from a dead stream's last picture.
    if (video_texture != 0)
    {
        const GLuint old = video_texture;
        glDeleteTextures(1, &old);
        video_texture = 0;
        video_w = video_h = 0;
    }
    say("Source unavailable — trying stream " + std::to_string(w_failover_tries_ + 1) + " of " +
            std::to_string(std::min<int>(kMaxFailoverTries + 1, static_cast<int>(d_streams_.size()))) + "...",
        3.5, false);
    play_stream(next_st, false);
    return true;
}

void Session::play_stream(const ::Stream &st, bool transcode, bool is_transition)
{
    const bool was_resuming = resuming_;
    const auto refuse = [&](const char *message) {
        if (was_resuming)
        {
            resuming_ = false;
            resumed_from_board_ = false;
            watch_stop(false);
        }
        say(message, 6.0, true);
    };
    const std::string srv = server();
    const bool torrent = st.url.empty() && !st.info_hash.empty();
    // Torrents play with the app's own engine, unless Settings says to use the
    // server; transcoding is always the server's.
    const bool builtin = torrent && !transcode && (prefs_.builtin_torrents || srv.empty());
    if (torrent && !builtin && srv.empty())
    {
        refuse("Torrent streams need your Stremio streaming server: set it in Settings.");
        return;
    }
    if (transcode && srv.empty())
    {
        refuse("Transcoding needs your Stremio streaming server: set it in Settings.");
        return;
    }
    if (watching && !resuming_)
        return;
    resuming_ = false;

    // What is being played, for progress and the player's titles.
    w_item_ = d_item_;
    if (d_meta_loaded_)
    {
        w_item_.name = d_meta_.name.empty() ? d_item_.name : d_meta_.name;
        w_item_.type = d_meta_.type.empty() ? d_item_.type : d_meta_.type;
        if (!d_meta_.poster.empty())
            w_item_.poster = d_meta_.poster;
        if (!d_meta_.background.empty())
            w_item_.background = d_meta_.background;
        if (!d_meta_.logo.empty())
            w_item_.logo = d_meta_.logo;
    }
    w_video_id_ = d_video_id_;
    w_stream_ = st;
    // Remembered, so that Continue Watching can play this same stream again.
    {
        Progress &p = progress_[w_item_.id];
        p.type = w_item_.type;
        p.name = w_item_.name;
        p.poster = w_item_.poster;
        p.stream_json = stream_to_json(st).dump();
        p.stream_video_id = w_video_id_;
        save_progress();
    }

    std::string title = w_item_.name, subtitle;
    int season = -1, episode = -1;
    if (detail.series)
    {
        for (const auto &v : d_meta_.videos)
            if (v.id == w_video_id_)
            {
                season = v.season;
                episode = v.episode;
                subtitle = "S" + std::to_string(v.season) + " E" + std::to_string(v.episode) +
                           (v.title.empty() ? "" : "  \xC2\xB7  " + v.title);
            }
    }
    else
    {
        subtitle = detail.title.year;
    }

    // Resume where it was left.
    double start = 0;
    for (const Item &cw : continue_watching())
    {
        if (cw.id != w_item_.id)
            continue;
        const bool same_video = cw.video_id.empty() || cw.video_id == w_video_id_ || !detail.series;
        if (same_video && cw.offset > 5 && (cw.duration <= 0 || cw.offset < cw.duration * 0.95))
            start = cw.offset;
    }

    const int gen = ++w_gen_;
    launch = LaunchState();
    launch.title = title;
    launch.subtitle = subtitle;
    launch.source = st.addon;
    {
        const Stream v = stream_view(st);
        launch.quality = v.resolution;
        if (!v.tags.empty())
            launch.quality += "  \xC2\xB7  " + v.tags;
        if (!v.size.empty())
            launch.quality += "  \xC2\xB7  " + v.size;
    }
    // Torrent and direct streams narrate the wait differently; always exactly
    // four stages — draw_launch indexes 0..3.
    launch.stages = torrent ? std::vector<std::string>{"Finding sources", "Connecting to peers", "Filling the buffer",
                                                       "Starting"}
                            : std::vector<std::string>{"Resolving stream", "Connecting to server", "Filling the buffer",
                                                       "Starting"};
    launch.stage = 0;
    launch.progress = -1.0f;
    launch.torrent = torrent;
    launch.hash = st.info_hash;
    // The launch-screen watchdogs: the title art stays up until the first
    // real frame reaches the video surface, and a resolved URL that yields
    // no data fails over instead of stalling silently. w_launch_since_ also
    // arms the seamless-transition fallback in watch_update().
    w_launch_since_ = now_seconds();
    w_launch_stall_since_ = w_launch_since_;
    w_launch_prog_ = 0;
    if (is_transition)
    {
        // Seamless Play-Next: stay inside the player. Episode N's last frame
        // stays on the GPU (video_texture is preserved by the keep_frame
        // watch_stop) until the next episode's first frame arrives — no
        // launch modal, no launch sound. If the new stream stalls, the
        // fallback in watch_update() raises the launch modal.
        w_seamless_transition_ = true;
        w_seamless_gen_ = video_gen_;
        launch_open = false;
    }
    else
    {
        w_seamless_transition_ = false;
        launch_open = true;
        cue(hui::audio::Cue::launch);
    }
    watching = true;
    playing = PlayerState();
    playing.title = title;
    playing.subtitle = subtitle;
    playing.controls = true;
    if (is_transition)
        w_info_until_ = now_seconds() + 4.0; // the new episode's info bar shows briefly
    art_dirty_ = true;

    if (builtin)
        torrent_stats_start(TorrentStream::make_url(st.info_hash, -1), st.info_hash);
    else if (torrent)
        torrent_stats_start(srv + "/" + st.info_hash + "/stats.json", st.info_hash);
    else
        t_visible_ = false;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    launch_cancel_ = cancel;
    const ::Stream s = st;
    bg<StreamRes>(
        [this, s, srv, torrent, builtin, transcode, season, episode, cancel]() {
            StreamRes r;
            // A fresh-enough speculative resolution (Play-button focus, the
            // next-episode pre-resolve) wins: no duplicate torrent start or
            // server /create for the same stream.
            if (spec_resolve_take(s, transcode, builtin, cancel.get(), r))
                return r;
            return resolve_stream(s, srv, torrent, builtin, transcode, season, episode, cancel.get());
        },
        [this, gen, start, title, subtitle, srv, s, builtin](StreamRes &r) {
            // A seamless transition never raises the launch modal, so
            // launch_open is false for it by design — not a stale request.
            if (gen != w_gen_ || (!launch_open && !w_seamless_transition_))
                return;
            if (!r.error.empty())
            {
                if (r.error != "cancelled" && try_failover_stream(r.error))
                    return;
                watch_stop(false);
                if (r.error != "cancelled")
                    say(r.error, 8.0, true);
                return;
            }
            if (builtin)
                t_stats_url_ = r.url;
            else if (r.file_idx >= 0)
                t_stats_url_ = srv + "/" + s.info_hash + "/" + std::to_string(r.file_idx) + "/stats.json";
            // Several connections and the long read-ahead for big files: direct links
            // and the streaming server's torrent streams. Not for transcoding, which is
            // a playlist of pieces.
            const bool par = r.url.find("/hlsv2/") == std::string::npos;
            watch_start(r.url, r.headers, start, title, subtitle, par);
        });
}

// ---------------------------------------------------------------------------
// Speculative stream-URL resolution (PLAYBACK worker)
//
// What the play path, the Play-button focus speculation and the next-episode
// pre-resolve all funnel through: the torrent engine / server /create dance
// for torrents, the URL itself for direct links. Runs on a worker thread;
// `cancel` aborts the long waits. Pure function of its arguments (srv,
// torrent and builtin are computed on the UI thread, where prefs_ lives).
Session::StreamRes Session::resolve_stream(const ::Stream &st, const std::string &srv, bool torrent, bool builtin,
                                           bool transcode, int season, int episode, std::atomic<bool> *cancel)
{
    StreamRes r;
    std::string media = st.url;
    if (!st.url.empty())
        r.headers = st.request_headers;
    if (builtin)
    {
        // The app's own engine: start it, wait for the file list (the peers
        // send it when the addon gave only the hash), pick the file and play
        // it from the engine.
        bt::Engine &eng = bt::Engine::get();
        eng.start(st.info_hash, st.sources);
        std::vector<bt::FileInfo> files;
        std::string err;
        if (!eng.wait_metadata(st.info_hash, files, cancel, 900, &err))
        {
            if (err == "cancelled")
                r.error = "cancelled";
            else if (err == "timed out")
                r.error = "No peers sent this torrent's file list in 15 minutes. Try another stream.";
            else
                r.error = "The torrent couldn't start (" + err + ").";
            return r;
        }
        int idx = st.file_idx;
        if (idx < 0 || static_cast<std::size_t>(idx) >= files.size())
            idx = bt::Engine::guess_file(files, season, episode);
        if (idx < 0)
        {
            r.error = "This torrent has no video file.";
            return r;
        }
        dlog("torrent %s: playing file %d, %s", st.info_hash.c_str(), idx, files[static_cast<std::size_t>(idx)].path.c_str());
        eng.select_file(st.info_hash, idx);
        r.url = TorrentStream::make_url(st.info_hash, idx);
        r.file_idx = idx;
        return r;
    }
    if (torrent)
    {
        // As Stremio's own player does: with no trackers from the addon and a
        // known file, play the file URL straight away and let the server find
        // peers its own way. Only otherwise ask it to /create the torrent first.
        int idx = st.file_idx;
        std::vector<std::string> sources;
        for (const auto &x : st.sources)
        {
            const std::string src = starts_with(x, "tracker:") || starts_with(x, "dht:") ? x : "tracker:" + x;
            if (std::find(sources.begin(), sources.end(), src) == sources.end())
                sources.push_back(src);
        }
        if (!sources.empty() || idx < 0)
        {
            json body;
            body["torrent"] = {{"infoHash", st.info_hash}};
            if (!sources.empty())
            {
                std::vector<std::string> all = {"dht:" + st.info_hash};
                for (const auto &x : sources)
                    if (x != all[0])
                        all.push_back(x);
                sources = all;
                body["peerSearch"] = {{"sources", sources}, {"min", 40}, {"max", 200}};
            }
            if (idx < 0)
            {
                json guess = json::object();
                if (season >= 0)
                    guess["season"] = season;
                if (episode >= 0)
                    guess["episode"] = episode;
                body["guessFileIdx"] = guess;
            }
            else
            {
                body["guessFileIdx"] = false;
            }
            // With few seeders the server may need minutes to get the torrent's
            // metadata: wait 15 minutes (Circle cancels). A proxy in front of the
            // server answers 502/503/504 when it tires of waiting: that means
            // "not yet", so ask again.
            const double deadline = now_seconds() + 900;
            HttpResponse h;
            for (;;)
            {
                const long left = static_cast<long>(deadline - now_seconds());
                h = http_post_json(srv + "/" + st.info_hash + "/create", body.dump(), std::max(left, 1L), cancel);
                const bool not_yet = h.error.empty() && h.status >= 502 && h.status <= 504;
                if (!not_yet || cancel->load() || now_seconds() + 3 >= deadline)
                    break;
                dlog("torrent: server answered HTTP %ld to /create, asking again", h.status);
                for (int i = 0; i < 20 && !cancel->load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (cancel->load())
                h.error = "cancelled";
            if (!h.error.empty() || !h.ok())
            {
                if (h.error == "cancelled")
                    r.error = "cancelled";
                else if (h.error.find("timed out") != std::string::npos)
                    r.error = "Your streaming server found no peers for this torrent in 15 minutes. Try another stream.";
                else
                    r.error = "Your streaming server: " + h.describe();
                return r;
            }
            if (idx < 0)
            {
                const json j = json::parse(h.body, nullptr, false);
                idx = static_cast<int>(jnum(j, "guessedFileIdx", 0));
            }
        }
        media = srv + "/" + st.info_hash + "/" + std::to_string(idx);
        std::string query;
        for (const auto &x : sources)
            query += (query.empty() ? "?tr=" : "&tr=") + url_encode(x);
        media += query;
        r.file_idx = idx;
        r.headers.clear();
    }
    if (transcode)
    {
        r.url = srv + "/hlsv2/" + random_id() + "/master.m3u8?mediaURL=" + url_encode(media) +
                "&videoCodecs=h264&audioCodecs=aac&audioCodecs=mp3&maxAudioChannels=2";
        r.headers.clear();
    }
    else
    {
        r.url = media;
    }
    return r;
}

std::string Session::resolve_key(const ::Stream &st, bool transcode, bool builtin)
{
    // Identity of "the same stream" for the speculation cache. Torrents are
    // identified by hash + file; direct links by URL. The engine/server mode
    // matters (builtin resolves to the local engine's URL, server to
    // /create's), as does transcoding (a different URL shape).
    if (!st.url.empty())
        return std::string("u:") + (transcode ? "tr:" : "") + st.url;
    if (!st.info_hash.empty())
        return std::string("t:") + (builtin ? "b:" : "s:") + st.info_hash + ":" + std::to_string(st.file_idx);
    return "";
}

// A focus (or the 80% pre-resolve) starts resolving this stream's playable
// URL now; the play press takes the result instead of resolving again. Only
// meaningful for torrents: direct links resolve instantly anyway.
void Session::spec_resolve_begin(const ::Stream &st, int season, int episode, double ttl)
{
    const std::string srv = server();
    const bool torrent = st.url.empty() && !st.info_hash.empty();
    const bool builtin = torrent && (prefs_.builtin_torrents || srv.empty());
    const std::string key = resolve_key(st, false, builtin);
    if (key.empty())
        return;
    auto op = std::make_shared<SpecOp>();
    int gen;
    {
        std::lock_guard<std::mutex> lock(spec_m_);
        if (key == spec_key_ && spec_op_)
        {
            bool fresh = false;
            {
                std::lock_guard<std::mutex> olock(spec_op_->m);
                fresh = !spec_op_->done || (now_seconds() - spec_at_ <= spec_ttl_);
            }
            if (fresh)
                return; // already in flight, or fresh enough
            spec_op_->cancel->store(true); // stale: supersede it
        }
        // Only installing a new op abandons the previous generation: an
        // early return above must not invalidate the in-flight result.
        gen = ++spec_gen_;
        spec_key_ = key;
        spec_op_ = op;
        spec_ttl_ = ttl;
        spec_at_ = 0; // not ready yet
    }
    const ::Stream s = st;
    g_tasks.run([this, s, srv, torrent, builtin, season, episode, op, gen] {
        StreamRes r = resolve_stream(s, srv, torrent, builtin, false, season, episode, op->cancel.get());
        std::lock_guard<std::mutex> lock(spec_m_);
        if (gen != spec_gen_.load())
            return; // superseded or cancelled: drop it
        op->res = std::move(r);
        {
            std::lock_guard<std::mutex> olock(op->m);
            op->done = true;
        }
        spec_at_ = now_seconds();
        op->cv.notify_all();
        dlog("watch: speculative stream resolution finished (%s)", op->res.error.empty() ? "ok" : op->res.error.c_str());
    });
}

// Play's worker calls this first: a fresh-enough speculation for the same
// stream is joined (no duplicate engine start / /create). True when the
// caller should use `out` and skip its own resolution.
bool Session::spec_resolve_take(const ::Stream &st, bool transcode, bool builtin, std::atomic<bool> *cancel,
                               StreamRes &out)
{
    std::shared_ptr<SpecOp> op;
    double ttl, at;
    {
        std::lock_guard<std::mutex> lock(spec_m_);
        if (spec_key_ != resolve_key(st, transcode, builtin) || !spec_op_)
            return false;
        op = spec_op_;
        ttl = spec_ttl_;
        at = spec_at_;
    }
    {
        // Join the in-flight resolution in slices so a cancelled play
        // doesn't hang on it.
        std::unique_lock<std::mutex> lock(op->m);
        while (!op->done && !(cancel && cancel->load()))
            op->cv.wait_for(lock, std::chrono::milliseconds(200));
        if (!op->done)
            return false;
    }
    // A stale or abandoned resolution is a miss: the caller resolves the
    // normal way. (Stale torrent URLs may reference an evicted engine
    // torrent or a forgotten server-side one.) The stale op is left in
    // place: the next begin() supersedes it, which keeps the focus index
    // in sync without extra bookkeeping.
    if (now_seconds() - at > ttl || op->res.error == "cancelled")
        return false;
    out = op->res;
    return true;
}

void Session::spec_resolve_cancel()
{
    ++spec_gen_;
    std::lock_guard<std::mutex> lock(spec_m_);
    if (spec_op_)
        spec_op_->cancel->store(true);
    spec_key_.clear();
    spec_op_.reset();
    spec_focus_idx_ = -1;
    spec_focus_id_.clear();
}

// The detail page's Play target under the cursor. Called from the shell's
// detail routing (every frame while the streams list is up): focusing a
// torrent row starts resolving its playable URL in the background so the
// press that follows usually finds it ready. Focus leaving the list (or
// the page) abandons the speculation.
void Session::note_stream_focus(int index)
{
    // Dwell arming for Worker B's update() hook: when the focused stream row
    // settles for 250ms, update() hands it to spec_prefetch_stream() ->
    // spec_prefetch_stream_impl(). An invalid focus disarms, so a stale index
    // can never fire on a later page.
    const bool valid = d_open_ && !watching && index >= 0 &&
                       index < static_cast<int>(d_streams_.size()) && !d_video_id_.empty();
    const int armed = valid ? index : -1;
    if (armed != spec_dwell_idx_)
    {
        spec_dwell_idx_ = armed;
        spec_dwell_since_ = now_seconds();
    }
    if (!valid)
    {
        spec_resolve_cancel();
        return;
    }
    const ::Stream &st = d_streams_[static_cast<std::size_t>(index)];
    // The row's stream identity: the list re-sorts once all addons answer,
    // which can move a different stream under a stationary cursor.
    const std::string id =
        st.info_hash.empty() ? st.url : st.info_hash + ":" + std::to_string(st.file_idx);
    if (index == spec_focus_idx_ && id == spec_focus_id_)
        return;
    spec_focus_idx_ = index;
    spec_focus_id_ = id;
    if (st.url.empty() && st.info_hash.empty())
        return;
    if (!st.url.empty())
        return; // direct links resolve instantly on press; nothing to speculate
    int season = -1, episode = -1;
    for (const auto &v : d_meta_.videos)
        if (v.id == d_video_id_)
        {
            season = v.season;
            episode = v.episode;
        }
    spec_resolve_begin(st, season, episode, 30.0);
}

// Focus-dwell prefetch (Worker C): the stream under the focused row gets its
// connection warmed and first bytes prefetched (HTTP), or its playable URL
// resolved (torrent), so the Cross press that follows usually finds it
// ready. Driven by Worker B's update() dwell hook through
// spec_prefetch_stream(). The prewarmed_key_ guard skips repeats for the
// same stream but allows a newly focused one (NetStream's prefetch is one
// entry at a time: a new prefetch supersedes the old).
void Session::spec_prefetch_stream_impl(int idx)
{
    if (!d_open_ || watching)
        return;
    if (idx < 0 || idx >= static_cast<int>(d_streams_.size()) || d_video_id_.empty())
        return;
    const ::Stream &st = d_streams_[static_cast<std::size_t>(idx)];
    if (!st.url.empty())
    {
        if (st.url == prewarmed_key_)
            return;
        prewarmed_key_ = st.url;
        g_tasks.run([u = st.url, h = st.request_headers] {
            NetStream::prewarm(u, h);
        });
        return;
    }
    if (st.info_hash.empty())
        return;
    int season = -1, episode = -1;
    for (const auto &v : d_meta_.videos)
        if (v.id == d_video_id_)
        {
            season = v.season;
            episode = v.episode;
        }
    // spec_resolve_begin dedupes internally (same key + fresh = no-op).
    spec_resolve_begin(st, season, episode, 60.0);
}

void Session::set_display_size(int w, int h)
{
    display_w_ = w;
    display_h_ = h;
}

void Session::watch_start(const std::string &url, const std::vector<std::string> &headers, double start,
                          const std::string &title, const std::string &subtitle, bool direct)
{
    Player::Options o;
    o.url = url;
    o.parallel = direct;
    o.headers = headers;
    o.start = start;
    o.audio_langs = pref_langs(prefs_.audio_langs);
    o.audio_output = prefs_.audio_output;
    o.volume_boost = prefs_.volume_boost;
    o.volume = prefs_.volume;
    o.night_mode = prefs_.night_mode;
    o.dolby_passthrough = prefs_.dolby_passthrough;
    // The GPU YUV->RGB path: compile the shaders once, here on the UI thread
    // where the GL context is current. Any failure keeps the CPU path for
    // the whole stream (the player re-decides per stream anyway).
    if (!gpu_yuv_tried_)
    {
        gpu_yuv_tried_ = true;
        gpu_yuv_ok_ = gpu_yuv_.ensure();
        dlog("watch: GPU YUV conversion %s", gpu_yuv_ok_ ? "on" : "off (CPU path)");
    }
    o.allow_gpu_convert = gpu_yuv_ok_;
    // 4K output only when the GPU path is active and the display is 2160p;
    // otherwise frames are emitted into the 1080p box exactly as before.
    if (gpu_yuv_ok_ && display_w_ >= 3840 && display_h_ >= 2160)
    {
        o.max_out_w = 3840;
        o.max_out_h = 2160;
    }
    player_.open(o);
    player_.set_audio_delay(prefs_.audio_delay); // global lip-sync pref

    // 119.88 Hz film mode is decided in watch_update(), once per playback,
    // when player_.fps() becomes known. Reading it here would always see 0:
    // Player::open() (above) resets fps_ and demux_run() sets it later.
    // film_mode_ is display state, so it intentionally survives across an
    // episode transition (watch_stop skips the restore there); the next
    // playback's one-time check re-decides.
    film_mode_checked_ = false;
    // The upscaler cache belongs to the previous stream: reset so a stale
    // upscaled frame/badge can never linger into the new playback. The draw
    // path re-gates on the new video frames and repopulates it.
    upscaled_tex_ = 0;
    upscaled_gen_ = 0;

    playing.title = title;
    playing.subtitle = subtitle;
    playing.position = start;
    playing.duration = 0;
    playing.paused = false;
    playing.buffering = -1;
    playing.sub_size = prefs_.sub_size == "sub-s" ? 0 : prefs_.sub_size == "sub-l" ? 2 : 1;
    playing.sub_color = prefs_.sub_color == "yellow" ? 1 : 0;
    playing.sub_bg_opacity = prefs_.sub_bg_opacity;
    playing.sub_raised = prefs_.sub_pos == "raised";
    w_started_ = false;
    w_heavy_warned_ = false;
    w_hw_warned_ = false;
    w_hw_mid_warned_ = false;
    w_menu_built_ = false;
    w_auto_sub_done_ = false;
    w_sub_auto_ = false;
    w_subs_.clear();
    w_sub_active_ = -1;
    w_sub2_active_ = -1;
    playing.line2.clear();
    playing.subtitle2_active = 0;
    subtitle2_shown_.clear();
    subtitle2_shown_an_ = 2;
    watch_scan_usb_subs(); // USB subtitle files become selectable tracks
    w_sub_delay_ = 0;
    {
        const std::string mkey = watch_memory_key();
        if (!mkey.empty())
        {
            const auto mit = prefs_.track_memory.find(mkey);
            if (mit != prefs_.track_memory.end())
                w_sub_delay_ = std::clamp(mit->second.sub_delay, -60.0, 60.0);
        }
    }
    subtitle_shown_.clear();
    subtitle_shown_an_ = 2;
    w_last_save_ = now_seconds();
    w_last_saved_pos_ = start;
    w_info_until_ = now_seconds() + kInfoSeconds;
    w_upnext_heading_.clear();
    w_upnext_until_ = 0;
    w_upnext_cancelled_ = false;
    w_upnext_prewarmed_ = false; // a new playback runs its own socket re-warm
    w_preresolve_id_.clear(); // a new playback runs its own 80% pre-resolve
    w_next_preresolved_vid_.clear(); // a new playback runs its own season-pack pre-resolve
    w_next_stream_ready_ = false;
    // A fresh playback restarts the binge count — unless it's the automatic
    // advance, which is exactly what the count is measuring.
    if (w_binge_auto_)
        w_binge_auto_ = false;
    else
        w_binge_count_ = 0;
    w_binge_asking_ = false;
    launch.stage = 1;
    watch_load_addon_subtitles();
}

// ---------------------------------------------------------------------------
// The torrent's statistics (peers, speed, how much of the file is there)

void Session::torrent_stats_start(const std::string &url, const std::string &info_hash)
{
    ++t_gen_;
    t_stats_url_ = url;
    t_peers_ = 0;
    t_known_ = -1;
    t_speed_ = 0;
    t_progress_ = -1;
    t_has_meta_ = false;
    t_visible_ = true;
    t_next_poll_ = 0;
    t_polling_ = false;
    launch.hash = info_hash;
}

void Session::torrent_stats_poll()
{
    if (t_polling_ || now_seconds() < t_next_poll_)
        return;
    t_polling_ = true;
    const int gen = t_gen_;
    const std::string url = t_stats_url_;
    struct Stats
    {
        bool ok = false;
        double peers = 0, speed = 0, progress = -1;
        int known = -1; // the app's own engine only
        bool has_meta = false;
    };
    bg<Stats>(
        [url]() {
            Stats s;
            if (TorrentStream::is_url(url))
            { // the app's own engine
                const bt::Stats es = bt::Engine::get().stats(url.substr(10, 40));
                s.ok = es.found;
                s.peers = es.peers;
                s.speed = es.download_rate;
                s.progress = es.file_progress;
                s.known = es.known_peers;
                s.has_meta = es.has_metadata;
                return s;
            }
            const HttpResponse h = http_get(url, 5);
            if (!h.ok())
                return s;
            const json j = json::parse(h.body, nullptr, false);
            if (!j.is_object())
                return s;
            s.ok = true;
            s.peers = jnum(j, "peers");
            s.speed = jnum(j, "downloadSpeed");
            if (j.contains("streamProgress"))
                s.progress = jnum(j, "streamProgress");
            return s;
        },
        [this, gen](Stats &s) {
            t_polling_ = false;
            t_next_poll_ = now_seconds() + 1;
            if (gen != t_gen_ || !s.ok)
                return;
            t_peers_ = static_cast<int>(s.peers);
            t_speed_ = s.speed;
            if (s.progress >= 0)
                t_progress_ = s.progress;
            t_known_ = s.known;
            t_has_meta_ = s.has_meta;
        });
}

// ---------------------------------------------------------------------------
// While it plays

// Smarter failure messages: our own code produces stable, matchable error
// strings (Player::open, NetStream, TorrentStream), so the display layer can
// say *why* a stream died instead of echoing internals. Prefix-matches on our
// Classified failure message. `torrent_specific` tells the caller the text
// already covers the torrent case, so the generic seeder backstop is skipped.
// Matches on our own strings only — never on FFmpeg's text.
struct FailureDesc
{
    std::string text;
    bool torrent_specific = false;
};
static FailureDesc describe_failure(const std::string &err)
{
    // Mid-playback stall: classify the wrapped reason the same way.
    static const std::string stopped_prefix = "The stream stopped (";
    if (err.size() > stopped_prefix.size() + 1 &&
        err.compare(0, stopped_prefix.size(), stopped_prefix) == 0 && err.back() == ')')
        return describe_failure(
            err.substr(stopped_prefix.size(), err.size() - stopped_prefix.size() - 1));

    // Torrent readiness strings, wrapped in "Could not open the stream (...)"
    // or on their own: show the detail, not the wrapper.
    if (err.find("no metadata yet") != std::string::npos ||
        err.find("the torrent isn't running") != std::string::npos ||
        err.find("no such file in the torrent") != std::string::npos ||
        err.find("bad torrent address") != std::string::npos)
    {
        static const std::string open_prefix = "Could not open the stream (";
        std::string detail = err;
        if (detail.size() > open_prefix.size() + 1 &&
            detail.compare(0, open_prefix.size(), open_prefix) == 0 && detail.back() == ')')
            detail = detail.substr(open_prefix.size(), detail.size() - open_prefix.size() - 1);
        return {"This torrent isn't ready yet (" + detail + "). Try another stream.", true};
    }

    static const std::string open_prefix = "Could not open the stream (";
    if (err.size() > open_prefix.size() + 1 &&
        err.compare(0, open_prefix.size(), open_prefix) == 0 && err.back() == ')')
    {
        const std::string inner =
            err.substr(open_prefix.size(), err.size() - open_prefix.size() - 1);
        if (inner.compare(0, 6, "HTTP 4") == 0)
        {
            const std::string code = inner.size() >= 8 ? inner.substr(5, 3) : inner;
            return {"This link is dead (" + code + "). Try another stream.", false};
        }
        if (inner.compare(0, 6, "HTTP 5") == 0)
            return {"The server isn't ready yet. Wait a bit or try another stream.", false};
        // Anything else wrapped here is either our own curl errbuf/strerror
        // text or an FFmpeg error string — shown honestly, no claim about
        // which one it is.
        return {"Couldn't open this stream (" + inner + "). Try another stream.", false};
    }

    return {"Playback failed: " + err, false};
}

void Session::watch_show_info()
{
    w_info_until_ = now_seconds() + kInfoSeconds;
}

void Session::nudge_sub_delay(double delta)
{
    // Manual nudge shares EVO's ±60 s range; the DualSense has no swipe
    // gesture in our input layer, so L2/R2 serve the nudge role.
    w_sub_delay_ = std::clamp(w_sub_delay_ + delta, -60.0, 60.0);
    say("Subtitle delay " + std::to_string(static_cast<int>(std::lround(w_sub_delay_ * 1000))) + " ms");
    // Remember per show, alongside the subtitle track pick.
    const std::string key = watch_memory_key();
    if (!key.empty())
    {
        prefs_.track_memory[key].sub_delay = w_sub_delay_;
        watch_prune_track_memory();
        save_prefs();
    }
}

void Session::nudge_audio_delay(double delta)
{
    // Lip-sync for soundbars/AVRs: ±500 ms in 50 ms steps, persisted globally.
    // L1/R1 serve the nudge role while the track menu is open (±60 s seek
    // owns them during plain playback).
    prefs_.audio_delay = std::clamp(prefs_.audio_delay + delta, -0.5, 0.5);
    // Snap to 50 ms steps to avoid float drift from repeated nudges.
    prefs_.audio_delay = std::lround(prefs_.audio_delay / 0.05) * 0.05;
    player_.set_audio_delay(prefs_.audio_delay);
    save_prefs();
    w_menu_dirty_ = true;
    int ms = static_cast<int>(std::lround(prefs_.audio_delay * 1000));
    say("Audio delay " + std::to_string(ms) + " ms" + (ms < 0 ? " (video waits for late audio)" : (ms > 0 ? " (audio waits for late video)" : "")));
}

void Session::watch_seek_by(double delta)
{
    // D-pad / L1/R1 seek with a preview bubble: the player seeks async, so
    // the bar's position lags the intent. The bubble shows the target until
    // the playhead converges or 3 s pass (stalled seek). The player holds
    // the pre-seek frame on screen meanwhile (its hold snapshot + the
    // upload_video() generation guard: no re-upload, no tear); the shimmer
    // direction tells the eye which way the seek is going.
    const double target = std::max(0.0, player_.position() + delta);
    w_seek_target_ = target;
    w_seek_until_ = now_seconds() + 3.0;
    playing.seek_shimmer = delta < 0 ? -1.0 : 1.0;
    player_.seek(target);
}

void Session::watch_update()
{
    const double now = now_seconds();
    const Player::State st = player_.state();
    player_.log_stats();
    // The PlayStation's menu is over the app (the PS button), or the app is in the background: pause.
    // It stays paused until Cross.
    {
        const SystemStatus sys = read_system_status();
        if (sys.valid && (sys.overlaid || sys.background) && w_started_ && !player_.paused())
        {
            player_.set_paused(true);
            dlog("playback paused: the system menu is over the app");
        }
    }

    // Deferred skip lookup: the episode metadata wasn't ready when playback
    // started (e.g. launched from Continue Watching before the detail fetch
    // landed). Retry now that d_meta_ has arrived. The w_skip_key_ guard in
    // watch_start_skip_lookup() prevents duplicate fetches, and the lookup
    // itself decides whether the metadata is usable (video-ID fallback can
    // succeed even with an empty videos list).
    if (w_skip_deferred_ && d_meta_loaded_)
    {
        w_skip_deferred_ = false;
        watch_start_skip_lookup();
    }

    // DualSense touchpad swipe volume: swipe up to raise, down to lower.
    // Continuous: the volume follows the finger like a slider. A full
    // top-to-bottom swipe spans ~60% volume; range is 0-200%.
    {
        if (!w_touchpad_.is_open())
            w_touchpad_.open(); // lazy; inert if the open fails
        TouchPoint tp;
        if (w_touchpad_.poll(tp))
        {
            if (!w_touch_active_)
            {
                // Finger just landed: anchor the gesture.
                w_touch_active_ = true;
                w_touch_last_y_ = tp.y;
            }
            else
            {
                const int dy = tp.y - w_touch_last_y_;
                // Y grows downward: swipe up (dy negative) = louder.
                // 18 pad units = 1% volume; the accumulator only moves on
                // whole-percent deltas, so sub-threshold jitter is ignored.
                const int kUnitsPerPct = 15; // 15 pad Y-units = 1% volume (~70% per full swipe)
                const int delta_pct = -dy / kUnitsPerPct;
                if (delta_pct != 0)
                {
                    w_touch_last_y_ -= delta_pct * kUnitsPerPct;
                    int vol = std::clamp(prefs_.volume + delta_pct, 0, 200);
                    if (vol != prefs_.volume)
                    {
                        prefs_.volume = vol;
                        player_.set_volume(vol);
                        save_prefs();
                        w_vol_osd_until_ = now + 1.5;
                        playing.volume_osd = vol;
                    }
                }
            }
        }
        else
        {
            w_touch_active_ = false;
        }
        // Hide the volume indicator after the timeout.
        if (playing.volume_osd >= 0 && now >= w_vol_osd_until_)
            playing.volume_osd = -1;
    }

    if (st == Player::State::Failed)
    {
        const std::string err = player_.error();
        if (!w_started_ && try_failover_stream(err))
            return;
        watch_stop(false);
        const FailureDesc desc = describe_failure(err);
        // The seeder backstop only applies when the message doesn't already
        // cover the torrent case.
        const bool seeder_hint =
            !w_stream_.info_hash.empty() && !desc.torrent_specific;
        say(desc.text + (seeder_hint ? " The torrent may have no seeders; try another stream." : ""), 8.0,
            true);
        return;
    }
    // "Still watching?" countdown: frozen while a modal is up, so it never
    // gives up while the user is choosing something.
    if (w_binge_asking_)
    {
        const double dt = w_binge_prev_ > 0 ? now - w_binge_prev_ : 0;
        w_binge_prev_ = now;
        if (!playing.menu && !dropdown_open)
        {
            w_binge_left_ -= std::min(dt, 0.5);
            if (w_binge_left_ <= 0)
            {
                w_binge_asking_ = false;
                w_binge_count_ = 0;
                playing.binge_prompt = false;
                watch_stop(false);
                say("Stopped: you weren't watching.", 4.0);
                return;
            }
        }
        playing.binge_prompt = true;
        playing.binge_left = std::max(0.0, w_binge_left_);
    }
    if (st == Player::State::Ended)
    {
        // A countdown running over the ended frame (unknown duration): tick it on the wall clock.
        if (w_upnext_until_ > 0)
        {
            const double left = w_upnext_until_ - now;
            playing.upnext_title = w_upnext_heading_;
            playing.upnext_remaining = left;
            if (left <= 0 && !playing.menu)
            {
                watch_upnext_fire();
            }
            else if (left <= 0)
            {
                // The track menu is open: hold instead of yanking the user
                // mid-selection; it fires on menu close.
                playing.upnext_remaining = 1.0;
            }
            return;
        }
        watch_save_progress(true, true); // it played to the end
        const bool next = prefs_.autoplay_next && detail.series && !w_upnext_cancelled_ && !w_binge_asking_;
        if (next && player_.duration() <= 0)
        {
            // Duration unknown: no 20 s heads-up was possible; offer the countdown over the ended frame.
            if (const ::Video *nx = next_episode())
            {
                w_upnext_heading_ = episode_heading(*nx);
                w_upnext_until_ = now + 10.0;
                playing.upnext_title = w_upnext_heading_;
                playing.upnext_remaining = 10.0;
                return; // don't stop yet; the countdown owns the transition
            }
        }
        // The transition (fast or slow) now stays in-player: keep Episode N's
        // last frame on the GPU whenever the next episode will roll, so the
        // slow path's held frame survives too. (A binge-check intercept keeps
        // the ended frame behind the "Still watching?" prompt; its confirm
        // path re-enters through watch_upnext_fire().)
        const bool rolling = next && player_.duration() > 0;
        watch_stop(true, rolling);
        if (rolling && !watch_binge_check())
            watch_next_episode();
        return;
    }

    // 119.88 Hz film mode (experimental setting): decided once per playback,
    // here on the UI thread, when the stream's frame rate is known.
    // player_.fps() is 0 until demux_run() opens the video stream, so the old
    // synchronous check in watch_start could never see it. ~24fps content
    // gets exact 5:5 pulldown instead of 3:2 judder. The switch blanks
    // briefly; it usually lands behind the launch screen. Anything
    // unavailable or refused leaves the display exactly as it was.
    if (watching && !film_mode_checked_)
    {
        const double fps = player_.fps();
        if (fps > 0)
        {
            film_mode_checked_ = true;
            if (prefs_.match_frame_rate && fps > 23.9 && fps < 24.1)
            {
                if (videoout_rate::set_119_88())
                {
                    film_mode_ = true;
                    // The 119.88 Hz switch is a refresh-rate change on the
                    // current resolution (EVO's verified mode table: 0xf is a
                    // rate, not a resolution). ps5::Display caches the
                    // open-time size and there is no verified by-name query
                    // for the live HDMI resolution, so display_w_/h_ from
                    // startup stay authoritative and the 4K output gate is
                    // unaffected. Device triage: with film mode on, confirm
                    // 4K output still engages on a 2160p TV.
                    const bool gate4k = display_w_ >= 3840 && display_h_ >= 2160;
                    dlog("watch: film mode on, 119.88 Hz (%.3g fps); display assumed %dx%d, 4K gate %s",
                         fps, display_w_, display_h_, gate4k ? "engaged" : "off");
                }
                else
                    dlog("watch: film mode unavailable (%.3g fps)", fps);
            }
            else
            {
                if (film_mode_)
                {
                    // Carried over from the previous episode's film mode, but
                    // this one isn't ~24fps: back to the default rate.
                    videoout_rate::restore_default();
                    film_mode_ = false;
                }
                dlog("watch: film mode off (pref %s, %.3g fps)", prefs_.match_frame_rate ? "on" : "off", fps);
            }
        }
    }

    int pct = 0;
    const bool buffering = player_.buffering(&pct);
    if (t_visible_ && (launch_open || buffering))
        torrent_stats_poll();

    if (launch_open)
    {
        // Say what is going on. Before the file is requested the torrent engine
        // knows how far it has got; then the player opens it and fills its buffer.
        if (st == Player::State::Idle)
        {
            launch.stage = t_known_ <= 0 ? 0 : (t_has_meta_ ? 2 : 1);
        }
        else if (st == Player::State::Opening)
        {
            launch.stage = 1;
        }
        else if (buffering)
        {
            launch.stage = 2;
            launch.progress = static_cast<float>(pct) / 100.0f;
        }
        else
        {
            launch.stage = 3;
            // The buffer is full but the first frame may not be on the video
            // surface yet: keep the title art up until it is, so the launch
            // never cuts to a black frame. The watchdog below bounds the
            // wait for streams that never produce a picture.
            if (video_texture != 0 || now - w_launch_since_ > kLaunchFirstFrameWait)
            {
                launch_open = false;
                w_started_ = true;
                w_seamless_transition_ = false; // a raised-then-cleared modal ends the seamless window too
            }
        }
        // Dead-link watchdog: the URL resolved but yields no data. 404s and
        // 0-byte bodies fail the player outright (the Failed branch above
        // fails those over); a silent stall trips this timer instead.
        const bool fetching = st == Player::State::Opening || buffering;
        double prog = 0;
        if (t_visible_ && t_progress_ >= 0)
            prog = t_progress_ * 100.0;
        else if (buffering)
            prog = static_cast<double>(pct);
        else if (player_.buffered_seconds() > 0.05)
            prog = 100.0;
        if (!fetching)
        {
            w_launch_stall_since_ = now; // resolution phase: its own timeouts apply
            w_launch_prog_ = 0;
        }
        else if (prog > w_launch_prog_ + 0.5)
        {
            w_launch_prog_ = prog;
            w_launch_stall_since_ = now;
        }
        else if (now - w_launch_stall_since_ > kLaunchStallFailover)
        {
            if (try_failover_stream("no data for 10 seconds"))
                return;
            w_launch_stall_since_ = now; // nothing left to try: don't spin
        }
    }
    // Seamless Play-Next: the next episode's first frame ends the hold — its
    // arrival also marks the new playback started (watch_start() cleared
    // w_started_, and the launch block above never runs for transitions, so
    // nothing else would set it). video_gen_ advances in upload_video() when
    // the new stream's first picture lands.
    if (w_seamless_transition_ && video_gen_ != w_seamless_gen_)
    {
        w_seamless_transition_ = false;
        w_started_ = true;
    }
    // Safety fallback: while the next episode's first frame hasn't arrived,
    // a slow-but-healthy open shows an in-player buffering indicator over the
    // held frame — not the launch modal. Only after 8 s with no first frame
    // does the visible launch modal take over, so the stall-failover
    // watchdog still runs instead of holding a dead frame forever.
    if (w_seamless_transition_ && video_gen_ == w_seamless_gen_)
    {
        if (now - w_launch_since_ > 0.35)
            playing.buffering = 0; // in-player indicator; launch_open stays false
        if (now - w_launch_since_ > 8.0)
        {
            launch_open = true;
            w_seamless_transition_ = false;
        }
    }
    launch.peers = std::to_string(t_peers_);
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f MB/s", t_speed_ / (1024.0 * 1024.0));
        launch.speed = buf;
        if (t_progress_ >= 0)
        {
            std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(t_progress_ * 100.0 + 0.5));
            launch.done = buf;
        }
    }

    if (st == Player::State::Playing && !w_menu_built_)
    {
        // The file is open: its own subtitle tracks go first.
        w_menu_built_ = true;
        std::vector<WatchSub> embedded;
        for (const auto &t : player_.subtitle_tracks())
        {
            WatchSub s;
            s.label = format_sub_track_label(t.lang, t.label, "") + " (in file)";
            s.lang = t.lang;
            s.embedded = t.stream;
            embedded.push_back(s);
        }
        w_subs_.insert(w_subs_.begin(), embedded.begin(), embedded.end());
        if (w_sub_active_ >= 0)
            w_sub_active_ += static_cast<int>(embedded.size());
        if (w_sub2_active_ >= 0)
            w_sub2_active_ += static_cast<int>(embedded.size());
        // Embedded tracks enumerate only once the file is playing, usually
        // after the addon subtitles already triggered an auto-pick. Re-run
        // the pick with the full list so an internal track outranks the
        // external one — unless the user chose manually (w_sub_auto_ false).
        if (w_sub_auto_ || w_sub_active_ < 0)
        {
            w_sub_active_ = -1;
            w_auto_sub_done_ = false;
            w_sub_auto_ = false;
        }
        watch_auto_subtitles();
        // Audio tracks are known now too: apply the remembered pick for this
        // show, if any (runs once per playback, alongside the menu build).
        watch_apply_remembered_audio();
        // Intro/outro skip: one AniSkip lookup per episode, on a worker thread.
        watch_start_skip_lookup();
        w_menu_dirty_ = true;
    }

    const double pos = player_.position();
    const double dur = player_.duration();

    // The subtitle on screen.
    std::string sub;
    int sub_an = 2, sub_fg = -1;
    if (w_sub_active_ >= 0 && w_sub_active_ < static_cast<int>(w_subs_.size()))
    {
        WatchSub &ws = w_subs_[static_cast<std::size_t>(w_sub_active_)];
        const double t = pos - w_sub_delay_;
        if (ws.embedded >= 0)
            sub = player_.embedded_subtitle(ws.embedded, t, &sub_an);
        else if (!ws.cues.empty())
            sub = cues_at(ws.cues, t, &sub_an);
    }
    if (sub != subtitle_shown_ || sub_an != subtitle_shown_an_)
    {
        subtitle_shown_ = sub;
        subtitle_shown_an_ = sub_an;
        playing.line = plain_cue(sub, &sub_fg);
        playing.sub_an = sub_an;
        playing.sub_fg = sub_fg;
    }

    // The secondary subtitle on screen (dual subs). Skipped when it is the
    // same track as the primary, so the cue is never drawn twice.
    std::string sub2;
    int sub2_an = 2, sub2_fg = -1;
    if (w_sub2_active_ >= 0 && w_sub2_active_ < static_cast<int>(w_subs_.size()) && w_sub2_active_ != w_sub_active_)
    {
        WatchSub &ws2 = w_subs_[static_cast<std::size_t>(w_sub2_active_)];
        const double t = pos - w_sub_delay_;
        if (ws2.embedded >= 0)
            sub2 = player_.embedded_subtitle(ws2.embedded, t, &sub2_an);
        else if (!ws2.cues.empty())
            sub2 = cues_at(ws2.cues, t, &sub2_an);
    }
    if (sub2 != subtitle2_shown_ || sub2_an != subtitle2_shown_an_)
    {
        subtitle2_shown_ = sub2;
        subtitle2_shown_an_ = sub2_an;
        playing.line2 = plain_cue(sub2, &sub2_fg);
        playing.sub2_an = sub2_an;
        playing.sub2_fg = sub2_fg;
    }

    playing.position = pos;
    playing.duration = dur;
    // Seek-preview bubble: visible until the playhead converges on the target
    // (within 2 s — the player lands frame-accurately) or the 3 s watchdog
    // fires. Repeated seeks just move the target.
    if (w_seek_target_ >= 0)
    {
        if (std::fabs(pos - w_seek_target_) < 2.0 || now_seconds() > w_seek_until_)
        {
            w_seek_target_ = -1;
            playing.seek_shimmer = 0.0; // the real frame is on show (or the seek gave up)
        }
    }
    playing.seek_preview = w_seek_target_;
    playing.buffered = player_.buffered_seconds();
    playing.paused = player_.paused();
    // During a seamless hold the in-player "waiting for first frame" indicator
    // (playing.buffering = 0, set above) must survive: player_ is closed, so
    // the plain assignment below would clobber it to -1 every frame.
    if (!(w_seamless_transition_ && video_gen_ == w_seamless_gen_ && playing.buffering >= 0))
        playing.buffering = (buffering && w_started_) ? pct : -1;
    // Stall toast: one "connection lost" per stall episode, "stream back" on resume.
    const bool stalled = buffering && w_started_ && !launch_open;
    if (stalled && !w_was_buffering_)
        say("Connection lost \xE2\x80\x94 retrying\xE2\x80\xA6");
    else if (!stalled && w_was_buffering_ && st == Player::State::Playing && w_started_)
        say("Stream back", 2.0);
    w_was_buffering_ = stalled;
    // Stats-for-nerds overlay (R3): refreshed with the other per-frame state.
    playing.nerd_overlay = w_nerd_;
    if (w_nerd_)
    {
        playing.nerd_lines = player_.nerd_stats();
        // Show the active upscaler when it's actually upscaling: mode, the
        // source->output resolution, and the RCAS sharpness percentage.
        if (upscaled_tex_ != 0 && prefs_.upscaler > 0)
        {
            const char *uname = prefs_.upscaler == 4 ? "Cinema HQ"
                              : prefs_.upscaler == 3 ? "Anime4K"
                              : prefs_.upscaler == 2 ? "FSR1+CAS"
                                                     : "FSR1";
            char ubuf[128];
            std::snprintf(ubuf, sizeof(ubuf), "upscaler: %s (%dx%d -> %dx%d, sharp %d%%)",
                          uname, upscaled_src_w_, upscaled_src_h_, upscaled_uw_, upscaled_uh_,
                          int(prefs_.upscaler_sharpness * 100.0 + 0.5));
            playing.nerd_lines.push_back(ubuf);
        }
        // Skip-lookup diagnostic: shows why the Skip button is missing.
        if (!w_skip_diag_.empty() && w_skip_.empty())
            playing.nerd_lines.push_back(w_skip_diag_);
    }
    else
        playing.nerd_lines.clear();
    {
        char buf[96];
        if (t_visible_)
        {
            std::snprintf(buf, sizeof(buf), "%.1f MB/s  \xC2\xB7  %d peers", t_speed_ / (1024.0 * 1024.0), t_peers_);
            playing.net = buf;
        }
        else
        {
            playing.net.clear();
        }
    }
    std::string audio_label;
    for (const auto &t : player_.audio_tracks())
        if (t.stream == player_.audio_stream())
            audio_label = t.label;
    playing.audio = audio_label.empty() ? "-" : audio_label;
    playing.subs = (w_sub_active_ >= 0 && w_sub_active_ < static_cast<int>(w_subs_.size()))
                       ? w_subs_[static_cast<std::size_t>(w_sub_active_)].label
                       : "Off";
    // The decoder, frame rate and buffer: on the controls, and in the log every 10 seconds.
    if (now - w_stats_at_ > 0.5)
    {
        w_stats_at_ = now;
        playing.stats = player_.stats();
        // Active signal badges for the OSD / X-Ray strip, rebuilt with stats.
        playing.badges.clear();
        const int vw = player_.video_w();
        if (vw >= 3000)
            playing.badges.push_back("4K");
        else if (vw >= 2000)
            playing.badges.push_back("1440p");
        else if (vw >= 1700)
            playing.badges.push_back("1080p");
        else if (vw >= 1200)
            playing.badges.push_back("720p");
        if (playing.stats.find("Dolby Vision") != std::string::npos)
            playing.badges.push_back("Dolby Vision");
        else if (playing.stats.find("HDR10") != std::string::npos)
            playing.badges.push_back("HDR10");
        else if (playing.stats.find("HLG") != std::string::npos)
            playing.badges.push_back("HLG");
        playing.badges.push_back(player_.using_hw() ? "HW Decode" : "SW Decode");
        // Upscaler badge: shown only while the upscaler actually produced a
        // cached frame (upscaled_tex_ != 0) and the upscaler is enabled.
        if (upscaled_tex_ != 0 && prefs_.upscaler > 0)
        {
            const char *ub = prefs_.upscaler == 4 ? "Cinema HQ"
                           : prefs_.upscaler == 3 ? "Anime4K"
                           : prefs_.upscaler == 2 ? "FSR1+CAS"
                                                  : "FSR1";
            playing.badges.push_back(ub);
        }
        if (film_mode_)
            playing.badges.push_back("119.88 Hz");
    }
    // "Ends at HH:MM" wall-clock finish time, tracking playback speed.
    if (dur > 0 && pos >= 0 && dur > pos)
    {
        const double spd = player_.speed() > 0.1 ? player_.speed() : 1.0;
        const std::time_t finish = std::time(nullptr) + static_cast<std::time_t>((dur - pos) / spd);
        struct tm tm_fin{};
        localtime_r(&finish, &tm_fin);
        char ebuf[32];
        std::snprintf(ebuf, sizeof(ebuf), "Ends at %02d:%02d", tm_fin.tm_hour, tm_fin.tm_min);
        playing.ends_at = ebuf;
    }
    else
    {
        playing.ends_at.clear();
    }
    // X-Ray cast line from the loaded detail metadata.
    if (d_meta_loaded_ && w_item_.id == d_item_.id)
    {
        std::string xr;
        if (!detail.directors.empty())
            xr = detail.directors;
        if (!detail.cast.empty())
            xr += (xr.empty() ? "" : "  ·  ") + detail.cast;
        playing.xray_cast = xr;
    }
    else
    {
        playing.xray_cast.clear();
    }
    // DualSense lightbar cinema mode: dark while playing, soft violet when
    // paused or on the launch screen. set_lightbar() only calls scePad on
    // actual color changes.
    if (w_started_ && !player_.paused())
        w_touchpad_.set_lightbar(0, 0, 0);
    else if (watching)
        w_touchpad_.set_lightbar(45, 32, 95);
    if (now - w_stats_logged_ > 10.0 && st == Player::State::Playing)
    {
        w_stats_logged_ = now;
        dlog("player: %s", playing.stats.c_str());
    }
    // Up next: in the last 20 s of a series episode, offer the next one with a
    // countdown that tracks the episode position, not the clock — so pausing
    // or seeking behaves, and the number on the card always matches reality.
    if (st == Player::State::Playing && detail.series && prefs_.autoplay_next && !w_upnext_cancelled_ && !w_binge_asking_ && dur > 0 && pos >= 0)
    {
        const double remain = dur - pos;
        // 80% in: the next episode's stream URL starts resolving now, so the
        // transition usually finds it ready (torrent starts and server
        // /creates are what make episode changes feel slow).
        if (remain <= dur * 0.2)
            watch_preresolve_next();
        if (remain <= 20.0)
        {
            if (w_upnext_heading_.empty())
            {
                if (const ::Video *nx = next_episode())
                    w_upnext_heading_ = episode_heading(*nx);
                else
                    w_upnext_cancelled_ = true; // no next episode: don't ask again
            }
            if (!w_upnext_cancelled_)
            {
                playing.upnext_title = w_upnext_heading_;
                // No clamp: the card appears 20 s out, so the countdown starts
                // at 20 and ticks down with the episode.
                playing.upnext_remaining = std::max(0.0, remain);
                // The card is up but no pre-resolve ran yet (seeked straight
                // into the last 25 s): resolve now so the transition still
                // has a stream ready. watch_preresolve_next() is idempotent
                // (guarded on w_next_preresolved_vid_), so repeats are free.
                if (w_preresolve_id_.empty())
                    watch_preresolve_next();
                // Re-warm the pre-resolved stream's connection, once per
                // playback: the socket opened at 80% can go cold before the
                // episode ends, which is what put the loading screen back up.
                if (w_next_stream_ready_ && !w_upnext_prewarmed_ && !w_next_stream_.url.empty())
                {
                    w_upnext_prewarmed_ = true;
                    const std::string url = w_next_stream_.url;
                    const std::vector<std::string> hdrs = w_next_stream_.request_headers;
                    g_tasks.run_urgent([url, hdrs] { NetStream::prewarm(url, hdrs); });
                }
                if (remain <= 0.5 && !playing.menu)
                {
                    // The countdown ran out exactly at the end: roll on.
                    watch_upnext_fire();
                    return;
                }
                if (remain <= 0.5)
                {
                    // The track menu is open: hold at the last second instead of
                    // yanking the user mid-selection; it fires on menu close.
                    playing.upnext_remaining = 1.0;
                }
            }
        }
        else if (remain > 25.0 && !w_upnext_heading_.empty())
        {
            // Sought back away from the end: stand down.
            w_upnext_heading_.clear();
            playing.upnext_title.clear();
            playing.upnext_remaining = -1;
        }
    }
    playing.controls = now < w_info_until_ || playing.paused || playing.menu;
    // Intro/outro skip: seekbar bands, prompt or auto-skip. Runs after
    // playing.controls is set: the prompt must only exist while it is
    // actually on screen (see the gate inside watch_check_skip).
    watch_check_skip(pos, dur);
    if (playing.menu && w_menu_dirty_)
        watch_build_menu();

    if (w_started_ && now - w_last_save_ > kSaveEvery)
    {
        w_last_save_ = now;
        watch_save_progress(false);
    }
    // Specific decode warnings, once per stream: when the hardware decoder
    // isn't on the job, say why up front instead of letting the user discover
    // it through dropped frames. SD/HD software decode is fine, so only 4K
    // earns a toast; the generic "too heavy" backstop below covers the rest.
    if (!w_hw_warned_ && !player_.using_hw() && player_.video_w() > 0)
    {
        w_hw_warned_ = true;
        const int vw = player_.video_w();
        const std::string codec = lower(player_.video_codec());
        const std::string note = lower(player_.hw_note());
        const bool is4k = vw >= 3000;
        if (is4k && (codec == "av1" || note.find("av1") != std::string::npos))
            say("This stream is AV1, and the PS5 has no AV1 hardware decoder, so 4K can't play smoothly in software. "
                "Pick an HEVC or H.264 stream, or press Square on the stream to transcode it on the server.",
                10.0);
        else if (is4k)
        {
            std::string msg = "Software decoding can't keep up with 4K smoothly";
            const std::string reason = plain_hw_reason(player_.hw_note());
            if (!reason.empty()) msg += " (" + reason + ")";
            msg += ". Pick a 1080p stream, or press Square on the stream to transcode it on the server.";
            say(msg, 10.0);
        }
    }
    // The hardware decoder gave up mid-stream: warn once when the fallback is
    // 4K software decode. (Separate flag: the start-of-stream check above has
    // already run by the time this can happen.)
    if (!w_hw_mid_warned_ && player_.hw_failed_midstream() && player_.video_w() >= 3000)
    {
        w_hw_mid_warned_ = true;
        say("The hardware decoder gave up on this stream, and software decoding can't keep up with 4K. "
            "Pick a 1080p stream, or press Square on the stream to transcode it on the server.",
            10.0);
    }
    if (player_.too_heavy() && !w_heavy_warned_)
    {
        w_heavy_warned_ = true;
        say("This video is too heavy to decode smoothly. Pick a 1080p stream, or press Square on a stream to transcode it "
            "on the server.",
            10.0);
    }
}

void Session::upload_video()
{
    const Player::Picture *pic = player_.present();
    if (!pic)
        return;
    // Skip re-upload when the picture hasn't changed (e.g., seek hold frame
    // served on every UI frame until the decoder catches up).
    if (pic->gen == video_gen_ && video_texture != 0)
        return;
    video_gen_ = pic->gen;
    int w = pic->w, h = pic->h;
    const std::uint8_t *rgba = pic->rgba;
    if (pic->yuv)
    {
        // The GPU path: the planes go up as textures and the shader converts
        // (and scales). Any GL failure converts the same planes on the CPU
        // instead — the exact old behaviour, never a broken frame.
        std::uint32_t target = video_texture;
        int tw = video_w, th = video_h;
        gpu_yuv_.set_quality(prefs_.deep_blacks, prefs_.tonemap_mode, prefs_.upscaler > 0);
        if (gpu_yuv_.render(*pic, target, tw, th))
        {
            video_texture = target;
            video_w = tw;
            video_h = th;
            return;
        }
        // Fail closed at the picture's own size (scale 1 always applies). The
        // buffer is reused across frames (was: a ~33 MB malloc+free per frame
        // for 4K) and the tone-mapping rows run on the shared row pool.
        const std::size_t need = std::size_t(pic->src_w) * pic->src_h * 4;
        if (cpu_rgba_.size() != need)
            cpu_rgba_.resize(need);
        Player::parallel_yuv_to_rgba(pic->planes, pic->colors, 1, cpu_rgba_.data(), pic->src_w, pic->src_h);
        w = pic->src_w;
        h = pic->src_h;
        rgba = cpu_rgba_.data();
    }
    if (video_texture == 0 || w != video_w || h != video_h)
    {
        if (video_texture != 0)
        {
            const GLuint old = video_texture;
            glDeleteTextures(1, &old);
        }
        video_texture = renderer_->batch().create_texture(w, h, rgba);
        video_w = w;
        video_h = h;
    }
    else
    {
        renderer_->batch().update_texture(video_texture, w, h, rgba);
    }
}

void Session::watch_stop(bool ended, bool keep_frame)
{
    resume_play_ = false;
    resuming_ = false;
    w_seamless_transition_ = false; // a real stop ends any in-flight seamless transition
    // Leaving the player: return the DualSense lightbar to system default.
    w_touchpad_.reset_lightbar();
    if (w_started_ && !ended)
        watch_save_progress(true);
    if (ended && !w_video_id_.empty())
    {
        watched_.insert(w_video_id_);
        save_progress();
    }
    ++w_gen_;
    if (launch_cancel_)
        *launch_cancel_ = true;
    if (w_skip_cancel_)
        *w_skip_cancel_ = true;
    w_skip_.clear();
    w_skip_key_.clear();
    w_skip_done_ = -1;
    w_skip_prompted_ = -1;
    w_skip_deferred_ = false;
    w_skip_diag_.clear();
    w_binge_asking_ = false;
    // Keep the pre-resolved next-episode stream across the auto-advance
    // chain: watch_upnext_fire() calls watch_stop(true) immediately before
    // watch_next_episode(), and clearing here would make that fast path dead.
    // Genuine stops (manual exit, last episode, autoplay off) clear it; the
    // next playback's watch_start() clears it regardless.
    const bool keep_preresolve = ended && prefs_.autoplay_next && !w_upnext_cancelled_ && detail.series &&
                                 next_episode() != nullptr;
    if (!keep_preresolve)
    {
        w_next_preresolved_vid_.clear();
        w_next_stream_ready_ = false;
    }
    // The upscaler cache belongs to this stream: drop it so the badge and
    // nerd stats can't reference an upscaled frame that no longer exists.
    // A seamless Play-Next keeps it: the held frame renders exactly as it
    // did during playback until the new stream's frames arrive (the draw
    // path re-gates on video_gen_, and watch_start() resets it anyway).
    if (!keep_frame)
    {
        upscaled_tex_ = 0;
        upscaled_gen_ = 0;
    }
    if (film_mode_)
    {
        // Skip the restore when the next episode autoplays immediately:
        // restoring then re-setting 119.88 Hz would blank the screen twice.
        // film_mode_ (display state) survives the transition; the next
        // playback's one-time fps check restores the default rate if that
        // episode isn't ~24fps. Genuine stops always restore.
        const bool autoplay_chain = ended && prefs_.match_frame_rate && prefs_.autoplay_next &&
                                    !w_upnext_cancelled_ && detail.series && next_episode() != nullptr;
        if (autoplay_chain)
            dlog("watch: keeping 119.88 Hz across the episode transition");
        else
        {
            film_mode_ = false;
            videoout_rate::restore_default();
            dlog("watch: film mode off, display rate restored");
        }
    }
    player_.close();
    watching = false;
    launch_open = false;
    t_visible_ = false;
    playing = PlayerState();
    subtitle_shown_.clear();
    subtitle_shown_an_ = 2;
    // A seamless Play-Next keeps the texture: Episode N's last frame stays on
    // the GPU until the next episode's first frame replaces it in
    // upload_video().
    if (video_texture != 0 && !keep_frame)
    {
        const GLuint old = video_texture;
        glDeleteTextures(1, &old);
        video_texture = 0;
        video_w = video_h = 0;
    }
    // Back on the title's page: what was watched shows there.
    if (d_open_)
        for (std::size_t i = 0; i < detail.episodes.size() && i < d_season_videos_.size(); ++i)
            detail.episodes[i].watched = watched(d_season_videos_[i]->id);
    changed_ |= kDetail;
    refresh_board();
    refresh_library();
}

void Session::watch_save_progress(bool final, bool force_finished)
{
    const double pos = player_.position(), dur = player_.duration();
    if (dur <= 0 || w_item_.id.empty())
        return; // live streams
    const bool finished = force_finished || pos > dur * 0.9;
    if (!final && pos < 1)
        return;

    Progress &p = progress_[w_item_.id];
    p.type = w_item_.type;
    p.name = w_item_.name;
    p.poster = w_item_.poster;
    p.video_id = w_video_id_;
    p.time = finished ? 0 : pos;
    // Finished: Continue Watching goes on to the next episode (and shows its streams); else it is the one being watched.
    p.up_next = finished ? next_episode_id() : std::string();
    p.duration = dur;
    p.updated = now_epoch_ms();
    if (finished)
        watched_.insert(w_video_id_);
    save_progress();

    const double delta = w_last_saved_pos_ >= 0 ? std::max(0.0, std::min(pos - w_last_saved_pos_, kSaveEvery * 2)) : 0;
    w_last_saved_pos_ = pos;

    if (!signed_in())
        return;
    const std::string now = iso8601_now();
    json item;
    const auto it = library_.find(w_item_.id);
    if (it != library_.end())
    {
        item = it->second;
    }
    else
    {
        item = json{{"_id", w_item_.id},
                    {"name", w_item_.name},
                    {"type", w_item_.type},
                    {"poster", w_item_.poster},
                    {"posterShape", "poster"},
                    {"removed", true},
                    {"temp", true},
                    {"_ctime", now},
                    {"state", json::object()},
                    {"behaviorHints", {{"defaultVideoId", nullptr}, {"featuredVideoId", nullptr}, {"hasScheduledVideos", false}}}};
    }
    json &s = item["state"];
    if (!s.is_object())
        s = json::object();
    s["lastWatched"] = now;
    s["timeOffset"] = static_cast<std::int64_t>(finished ? 0 : pos * 1000);
    s["duration"] = static_cast<std::int64_t>(dur * 1000);
    s["video_id"] = w_video_id_;
    s["timeWatched"] = static_cast<std::int64_t>(jnum(s, "timeWatched") + delta * 1000);
    s["overallTimeWatched"] = static_cast<std::int64_t>(jnum(s, "overallTimeWatched") + delta * 1000);
    if (!s.contains("timesWatched"))
        s["timesWatched"] = 0;
    if (!s.contains("flaggedWatched"))
        s["flaggedWatched"] = 0;
    if (!s.contains("noNotif"))
        s["noNotif"] = false;
    if (!s.contains("watched"))
        s["watched"] = nullptr;
    if (finished && final)
    {
        s["timesWatched"] = static_cast<int>(jnum(s, "timesWatched")) + 1;
        if (w_item_.type == "movie")
            s["flaggedWatched"] = 1;
    }
    item["_mtime"] = now;
    library_[w_item_.id] = item;
    library_loaded_ = true;

    const std::string key = prefs_.auth_key;
    const json changes = json::array({item});
    bg<ApiResult>([key, changes]() { return api_library_put(key, changes); },
                  [](ApiResult &r) {
                      if (!r.ok)
                          dlog("library put: %s", r.error.c_str());
                  });
}

// ---------------------------------------------------------------------------
// The buttons

void Session::watch_input(const hui::InputFrame &in)
{
    using hui::Action;
    using hui::Direction;
    if (!watching)
        return;
    if (launch_open)
    {
        // The stream is still starting: only a way out.
        if (in.is_pressed(Action::back))
            watch_stop(false);
        return;
    }
    // The "Still watching?" card owns input while it's up: Cross continues
    // to the next episode; any other press dismisses it without advancing
    // (the viewer is present, just doing something else). While the track
    // menu is open it keeps its own meaning for every button.
    if (w_binge_asking_ && !playing.menu)
    {
        if (in.is_pressed(Action::confirm))
        {
            w_binge_asking_ = false;
            watch_upnext_fire(true); // manual continue: the binge count restarts
            return;
        }
        if (in.pressed != 0 || in.nav != Direction::none)
        {
            w_binge_asking_ = false;
            w_binge_count_ = 0;
            playing.binge_prompt = false;
        }
        else
            return;
    }
    // Any user input during playback restarts the binge count.
    if (in.pressed != 0 || in.nav != Direction::none)
        w_binge_count_ = 0;
    // A Triangle press on a visible Skip prompt jumps past the segment
    // WITHOUT cancelling an in-flight up-next countdown: skipping the
    // credits is how you reach the next episode faster, the opposite of
    // cancelling autoplay. (review 2026-10-13)
    if (in.is_pressed(Action::north) && watch_try_skip_prompt())
        return;
    // Up next: Cross jumps straight to the next episode instead of waiting
    // out the countdown — unless the track menu is open, where Cross keeps
    // its menu meaning. Any other press keeps the behavior below: it
    // cancels autoplay, then still performs its normal action.
    if (upnext_active() && !playing.menu && in.is_pressed(Action::confirm))
    {
        watch_upnext_fire(true); // manual Next: the binge count restarts
        return;
    }
    // Up-next countdown: any button cancels it, and the cancel is shown —
    // a silent cancel would leave the user wondering why nothing happened.
    // The press is NOT swallowed: it still performs its normal action
    // (pause, zoom, stats...), otherwise every button would need two
    // presses during the last 20 s of an episode. (audit 2026-10-13)
    if (upnext_active() && (in.pressed != 0 || in.nav != Direction::none))
    {
        w_upnext_heading_.clear();
        w_upnext_until_ = 0;
        w_upnext_cancelled_ = true;
        playing.upnext_title.clear();
        playing.upnext_remaining = -1;
        say("Up next cancelled", 2.5);
    }
    const bool any = in.pressed != 0 || in.nav != Direction::none;
    if (any)
        watch_show_info();

    if (playing.menu)
    {
        const std::size_t audio_count = playing.audio_tracks.size(), sub_count = playing.subtitle_tracks.size(),
                          speed_count = playing.speed_options.size(), skip_count = playing.skip_options.size();
        if (in.nav == Direction::left)
            playing.menu_column = std::max(0, playing.menu_column - 1);
        else if (in.nav == Direction::right)
            playing.menu_column = std::min(3, playing.menu_column + 1);
        else if (in.nav == Direction::up)
            playing.menu_row = std::max(0, playing.menu_row - 1);
        else if (in.nav == Direction::down)
            playing.menu_row += 1;
        const int count = static_cast<int>(playing.menu_column == 0   ? audio_count
                                           : playing.menu_column == 1 ? sub_count
                                           : playing.menu_column == 2 ? speed_count
                                                                      : skip_count);
        playing.menu_row = std::clamp(playing.menu_row, 0, std::max(0, count - 1));
        // Tracks menu: Cross picks the primary track, Square on the subtitle
        // column picks the secondary (dual subtitles). Square elsewhere does
        // nothing here; outside the menu it still cycles subtitles as before.
        if (in.is_pressed(Action::confirm) || in.is_pressed(Action::west))
        {
            const bool secondary = in.is_pressed(Action::west);
            if (playing.menu_column == 0)
            {
                if (!secondary)
                {
                    const auto tracks = player_.audio_tracks();
                    if (playing.menu_row < static_cast<int>(tracks.size()))
                    {
                        const auto &t = tracks[static_cast<std::size_t>(playing.menu_row)];
                        player_.select_audio(t.stream);
                        watch_remember_audio(t.lang);
                    }
                }
            }
            else if (playing.menu_column == 1)
            {
                if (secondary)
                {
                    watch_select_sub2(playing.menu_row - 1);
                }
                else
                {
                    watch_select_sub(playing.menu_row - 1);
                    w_sub_auto_ = false; // manual choice: auto-select must not override it
                    watch_remember_sub();
                }
            }
            else if (playing.menu_column == 2)
            {
                if (!secondary)
                {
                    static const double speeds[] = {0.5, 1.0, 1.5, 2.0};
                    const int i = std::clamp(playing.menu_row, 0, 3);
                    player_.set_speed(speeds[i]);
                    say("Speed: " + playing.speed_options[std::size_t(i)]);
                }
            }
            else
            {
                if (!secondary)
                {
                    // Skip column: manual refresh bypasses the cache.
                    watch_refresh_skip();
                }
            }
            w_menu_dirty_ = true;
        }
        if (in.is_pressed(Action::jump_prev))
            nudge_sub_delay(-0.1);
        if (in.is_pressed(Action::jump_next))
            nudge_sub_delay(0.1);
        if (in.is_pressed(Action::page_prev))
            nudge_audio_delay(-0.05);
        if (in.is_pressed(Action::page_next))
            nudge_audio_delay(0.05);
        if (in.is_pressed(Action::back) || in.is_pressed(Action::menu) || in.is_pressed(Action::touch))
        {
            playing.menu = false;
            watch_show_info();
        }
        if (playing.menu)
            watch_build_menu();
        return;
    }

    if (in.is_pressed(Action::confirm) || in.is_pressed(Action::touch))
    {
        player_.set_paused(!player_.paused());
    }
    else if (in.nav == Direction::up || in.is_pressed(Action::menu))
    {
        playing.menu = true;
        watch_build_menu();
        const auto tracks = player_.audio_tracks();
        playing.menu_column = tracks.size() > 1 ? 0 : 1;
        playing.menu_row = playing.menu_column == 0 ? playing.audio_active : playing.subtitle_active;
    }
    else if (in.nav == Direction::down && !in.nav_repeat)
    {
        // Toggle the video zoom. Toast only: the hint row is already full.
        // (Repeat-gated: holding down must not strobe fit/fill.)
        const bool fill = prefs_.aspect_mode != "fill";
        prefs_.aspect_mode = fill ? "fill" : "fit";
        save_prefs();
        say(fill ? "Zoom: Fill" : "Zoom: Fit");
    }
    else if (in.is_pressed(Action::l3))
    {
        // Restart the episode. Toasted: without feedback an accidental
        // stick-click silently throws away the watch position.
        player_.seek(0);
        say("Restarted from beginning");
    }
    else if (in.nav == Direction::left)
    {
        watch_seek_by(-10);
    }
    else if (in.nav == Direction::right)
    {
        watch_seek_by(10);
    }
    else if (in.is_pressed(Action::page_prev))
    {
        watch_seek_by(-60);
    }
    else if (in.is_pressed(Action::page_next))
    {
        watch_seek_by(60);
    }
    else if (in.is_pressed(Action::west))
    {
        // Cycle: off, each subtitle, off.
        const int n = static_cast<int>(w_subs_.size());
        if (n == 0)
        {
            say("No subtitles for this video");
        }
        else
        {
            int next = w_sub_active_ + 1;
            if (next >= n)
                next = -1;
            watch_select_sub(next);
            w_sub_auto_ = false; // manual choice: auto-select must not override it
            watch_remember_sub();
        }
    }
    else if (in.is_pressed(Action::north))
    {
        // A visible Skip prompt takes over Triangle for one press.
        if (!watch_try_skip_prompt())
        {
            const auto tracks = player_.audio_tracks();
            if (tracks.size() < 2)
            {
                say(tracks.empty() ? "No audio" : "Only one audio track");
            }
            else
            {
                std::size_t cur = 0;
                for (std::size_t i = 0; i < tracks.size(); ++i)
                    if (tracks[i].stream == player_.audio_stream())
                        cur = i;
                const auto &t = tracks[(cur + 1) % tracks.size()];
                player_.select_audio(t.stream);
                watch_remember_audio(t.lang);
                say("Audio: " + t.label);
            }
        }
    }
    else if (in.is_pressed(Action::jump_prev))
    {
        nudge_sub_delay(-0.1);
    }
    else if (in.is_pressed(Action::jump_next))
    {
        nudge_sub_delay(0.1);
    }
    else if (in.is_pressed(Action::r3))
    {
        w_nerd_ = !w_nerd_;
        say(w_nerd_ ? "Stats overlay on" : "Stats overlay off");
    }
    else if (in.is_pressed(Action::back))
    {
        watch_stop(false);
    }
}

void Session::watch_build_menu()
{
    w_menu_dirty_ = false;
    playing.audio_tracks.clear();
    playing.audio_active = 0;
    const auto tracks = player_.audio_tracks();
    for (std::size_t i = 0; i < tracks.size(); ++i)
    {
        playing.audio_tracks.push_back(tracks[i].label);
        if (tracks[i].stream == player_.audio_stream())
            playing.audio_active = static_cast<int>(i);
    }
    playing.subtitle_tracks.clear();
    playing.subtitle_tracks.push_back("Off");
    for (const WatchSub &s : w_subs_)
    {
        std::string label = s.label;
        if (s.loading)
            label += "  (loading)";
        if (s.failed)
            label += "  (failed)";
        playing.subtitle_tracks.push_back(label);
    }
    playing.subtitle_active = w_sub_active_ + 1;
    playing.subtitle2_active = w_sub2_active_ + 1;
    playing.speed_options = {"0.5x", "1x", "1.5x", "2x"};
    const double sp = player_.speed();
    playing.speed_active = sp < 0.75 ? 0 : sp < 1.25 ? 1 : sp < 1.75 ? 2 : 3;
    playing.skip_options = {"Refresh skip timings"};
    playing.skip_active = -1;
    playing.menu_row = std::min(playing.menu_row,
                                std::max(0, static_cast<int>(playing.menu_column == 0 ? playing.audio_tracks.size()
                                                                                       : playing.menu_column == 1 ? playing.subtitle_tracks.size()
                                                                                       : playing.menu_column == 2 ? playing.speed_options.size()
                                                                                                                  : playing.skip_options.size()) -
                                                1));
    char delay[48];
    std::snprintf(delay, sizeof(delay), "%+.2f s", w_sub_delay_);
    playing.subtitle_delay = delay;
    std::snprintf(delay, sizeof(delay), "%+.2f s", prefs_.audio_delay);
    playing.audio_delay = delay;
}

// ---------------------------------------------------------------------------
// Subtitles

void Session::watch_select_sub(int index)
{
    if (index < 0 || index >= static_cast<int>(w_subs_.size()))
    {
        w_sub_active_ = -1;
        player_.select_embedded_subtitle(-1);
        say("Subtitles off");
        w_menu_dirty_ = true;
        return;
    }
    w_sub_active_ = index;
    WatchSub &ws = w_subs_[static_cast<std::size_t>(index)];
    // Tell the player which embedded stream is active so it doesn't decode
    // all 25-40 ASS tracks simultaneously. External URLs use -1 (decode all).
    player_.select_embedded_subtitle(ws.embedded);
    // Suppress the toast on automatic initial selection so it never overwrites
    // the hardware-decoder refusal warning.
    if (!w_sub_auto_)
        say("Subtitles: " + ws.label);
    watch_ensure_sub_cues(index);
    w_menu_dirty_ = true;
}

// Secondary subtitle (dual subs): Square on a subtitle row in the Tracks menu.
void Session::watch_select_sub2(int index)
{
    if (index < 0 || index >= static_cast<int>(w_subs_.size()))
    {
        w_sub2_active_ = -1;
        playing.subtitle2_active = 0;
        subtitle2_shown_.clear();
        playing.line2.clear();
        say("Secondary subtitles off");
        w_menu_dirty_ = true;
        return;
    }
    if (index == w_sub_active_)
    {
        say("Already the primary subtitle — pick another track", 2.5);
        return;
    }
    w_sub2_active_ = index;
    WatchSub &ws = w_subs_[static_cast<std::size_t>(index)];
    playing.subtitle2_active = index + 1;
    say("Secondary subtitles: " + ws.label);
    watch_ensure_sub_cues(index);
    w_menu_dirty_ = true;
}

// Lazy-load a WatchSub's cues: USB files (url under /mnt/) are read from
// disk, addon subtitles are downloaded. Matches by url so retries are safe.
void Session::watch_ensure_sub_cues(int index)
{
    if (index < 0 || index >= static_cast<int>(w_subs_.size()))
        return;
    WatchSub &ws = w_subs_[static_cast<std::size_t>(index)];
    if (ws.embedded >= 0 || !ws.cues.empty() || ws.loading)
        return;
    ws.loading = true;
    ws.failed = false;
    const std::string url = ws.url, lang = ws.lang;
    const int gen = w_gen_;
    struct Res
    {
        std::vector<Cue> cues;
        std::string error;
    };
    bg<Res>(
        [url, lang]() {
            Res r;
            if (url.rfind("/mnt/", 0) == 0)
            {
                // USB subtitle file: read from disk (capped at 2 MB).
                FILE *f = std::fopen(url.c_str(), "rb");
                if (!f)
                {
                    r.error = "Could not open the USB subtitle file";
                    return r;
                }
                std::fseek(f, 0, SEEK_END);
                const long sz = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                if (sz <= 0 || sz > 2 * 1024 * 1024)
                {
                    std::fclose(f);
                    r.error = "That subtitle file is empty or unreadable";
                    return r;
                }
                std::string body(static_cast<std::size_t>(sz), '\0');
                if (std::fread(&body[0], 1, body.size(), f) != body.size())
                {
                    std::fclose(f);
                    r.error = "That subtitle file is empty or unreadable";
                    return r;
                }
                std::fclose(f);
                r.cues = parse_subtitle_file(to_utf8(body, lang));
            }
            else
            {
                const HttpResponse h = http_get(url, 30);
                if (!h.ok())
                {
                    r.error = h.describe();
                    return r;
                }
                r.cues = parse_subtitle_file(to_utf8(h.body, lang));
            }
            if (r.cues.empty())
                r.error = "That subtitle file is empty or unreadable";
            return r;
        },
        [this, gen, url](Res &r) {
            if (gen != w_gen_)
                return;
            const bool usb = url.rfind("/mnt/", 0) == 0;
            for (WatchSub &s : w_subs_)
            {
                if (s.url != url)
                    continue;
                s.loading = false;
                s.cues = std::move(r.cues);
                s.failed = !r.error.empty();
            }
            if (!r.error.empty())
                say(usb ? "Could not load the USB subtitles: " + r.error
                        : "Could not download the subtitles: " + r.error,
                    5.0, true);
            w_menu_dirty_ = true;
        });
}

// USB subtitles: scan /mnt/usb0 and /mnt/usb1 for subtitle files at playback
// start. Unmounted sticks fail silently — no toast, no error.
void Session::watch_scan_usb_subs()
{
    static const char *const mounts[] = {"/mnt/usb0/", "/mnt/usb1/"};
    static const char *const exts[] = {".srt", ".ass", ".ssa", ".vtt"};
    int added = 0;
    for (const char *mnt : mounts)
    {
        DIR *d = opendir(mnt);
        if (!d)
            continue; // not mounted: silent
        while (added < 50)
        {
            struct dirent *e = readdir(d);
            if (!e)
                break;
            std::string name = e->d_name;
            if (name.empty() || name[0] == '.')
                continue;
            std::string lower = name;
            for (char &c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool ok = false;
            for (const char *x : exts)
            {
                const std::size_t xl = std::strlen(x);
                if (lower.size() > xl && lower.compare(lower.size() - xl, xl, x) == 0)
                {
                    ok = true;
                    break;
                }
            }
            if (!ok)
                continue;
            WatchSub s;
            s.url = std::string(mnt) + name; // local path: read from disk on select
            s.label = format_sub_track_label("", name, s.url) + " (USB)";
            w_subs_.push_back(std::move(s));
            ++added;
        }
        closedir(d);
    }
    if (added > 0)
        w_menu_dirty_ = true;
}

void Session::watch_load_addon_subtitles()
{
    const std::string type = w_item_.type, vid = w_video_id_;
    // Preferred languages, captured for the worker (prefs_ is UI-thread-only).
    const std::vector<std::string> want = pref_langs(prefs_.subtitle_langs);
    std::vector<std::pair<std::string, std::string>> sources;
    for (const auto &a : addons_)
        if (!addon_disabled(*a) && a->supports("subtitles", type, vid))
            sources.push_back({a->name, a->resource_url("subtitles", type, vid)});
    const int gen = w_gen_;
    struct SubRes
    {
        std::vector<SubtitleTrack> tracks;
        std::string pref_url;       // url of the pre-parsed track, "" when none
        std::vector<Cue> pref_cues; // its cues, fetched and parsed on the worker
    };
    for (const auto &src : sources)
    {
        const std::string name = src.first, url = src.second;
        bg<SubRes>(
            [name, url, want]() {
                SubRes r;
                json j;
                std::string err;
                if (!fetch_json(url, j, err, 25))
                    return r;
                r.tracks = parse_subtitles(j, name);
                // Pre-parse the most-preferred track's cues right here on the
                // worker, so the UI thread gets WatchSub::cues ready and the
                // auto-select sees the real cue count immediately instead of
                // lazy-loading it after the pick.
                for (const std::string &pl : want)
                {
                    const SubtitleTrack *hit = nullptr;
                    for (const SubtitleTrack &t : r.tracks)
                        if (!t.url.empty() && language_to_iso639_2(t.lang) == pl)
                        {
                            hit = &t;
                            break;
                        }
                    if (!hit)
                        continue;
                    const HttpResponse h = http_get(hit->url, 30);
                    if (!h.ok())
                        break;
                    r.pref_cues = parse_subtitle_file(to_utf8(h.body, language_to_iso639_2(hit->lang)));
                    if (!r.pref_cues.empty())
                        r.pref_url = hit->url;
                    break;
                }
                return r;
            },
            [this, gen](SubRes &r) {
                if (gen != w_gen_)
                    return;
                std::vector<SubtitleTrack> &tracks = r.tracks;
                // Preferred languages first, then the rest, at most 80.
                const auto prefs = pref_langs(prefs_.subtitle_langs);
                const auto rank = [&](const std::string &lang) {
                    const std::string l = language_to_iso639_2(lang);
                    for (std::size_t i = 0; i < prefs.size(); ++i)
                        if (prefs[i] == l)
                            return static_cast<int>(i);
                    return static_cast<int>(prefs.size());
                };
                std::stable_sort(tracks.begin(), tracks.end(),
                                 [&](const SubtitleTrack &a, const SubtitleTrack &b) { return rank(a.lang) < rank(b.lang); });
                for (const auto &t : tracks)
                {
                    if (w_subs_.size() >= 80)
                        break;
                    WatchSub s;
                    s.lang = language_to_iso639_2(t.lang);
                    s.url = t.url;
                    // "English (ASS, SDH)  ·  OpenSubtitles" instead of
                    // "English 1  ·  OpenSubtitles".
                    s.label = format_sub_track_label(s.lang, t.label, t.url) + "  \xC2\xB7  " + t.addon;
                    if (!r.pref_url.empty() && s.url == r.pref_url)
                        s.cues = std::move(r.pref_cues);
                    w_subs_.push_back(s);
                }
                watch_auto_subtitles();
                w_menu_dirty_ = true;
            });
    }
}

// Auto-select rank for one subtitle track in a preferred language: internal
// (embedded) tracks first, junk labels and tiny cue counts deprioritized.
// Returns 100 when the track is not a candidate for this language.
int Session::sub_auto_rank(const WatchSub &s, const std::string &lang)
{
    if (s.lang != lang)
        return 100;
    int r = (s.embedded >= 0) ? 0 : 2; // internal first
    const std::string l = lower(s.label);
    if (l.find("sign") != std::string::npos || l.find("song") != std::string::npos ||
        l.find("karaoke") != std::string::npos || l.find("commentary") != std::string::npos)
        r += 1;
    if (!s.cues.empty() && s.cues.size() < 10)
        r += 1;
    return r;
}

std::string Session::watch_memory_key() const
{
    if (w_item_.type != "series" && w_item_.type != "movie")
        return std::string();
    return w_item_.id;
}

void Session::watch_prune_track_memory()
{
    static const std::size_t kMax = 200; // matches the cap in the save loop
    while (prefs_.track_memory.size() > kMax)
        prefs_.track_memory.erase(prefs_.track_memory.begin());
}

void Session::watch_remember_audio(const std::string &lang)
{
    const std::string key = watch_memory_key();
    if (key.empty() || lang.empty())
        return;
    prefs_.track_memory[key].audio_lang = lang;
    watch_prune_track_memory();
    save_prefs();
}

void Session::watch_remember_sub()
{
    const std::string key = watch_memory_key();
    if (key.empty())
        return;
    Prefs::TrackMemory &m = prefs_.track_memory[key];
    if (w_sub_active_ < 0 || w_sub_active_ >= static_cast<int>(w_subs_.size()))
    {
        m.sub_off = true;
        m.sub_lang.clear();
    }
    else
    {
        const WatchSub &ws = w_subs_[static_cast<std::size_t>(w_sub_active_)];
        m.sub_off = false;
        m.sub_lang = ws.lang;
        m.sub_internal = ws.embedded >= 0;
    }
    watch_prune_track_memory();
    save_prefs();
}

void Session::watch_apply_remembered_audio()
{
    const std::string key = watch_memory_key();
    if (key.empty())
        return;
    const auto it = prefs_.track_memory.find(key);
    if (it == prefs_.track_memory.end() || it->second.audio_lang.empty())
        return;
    const std::string &lang = it->second.audio_lang;
    const auto tracks = player_.audio_tracks();
    const int cur = player_.audio_stream();
    for (const auto &t : tracks)
        if (t.stream == cur && t.lang == lang)
            return; // already on the remembered track
    for (const auto &t : tracks)
        if (t.lang == lang)
        {
            player_.select_audio(t.stream);
            say("Audio: " + t.label);
            return;
        }
}

// ============================================================================
// Skip-intro/outro lookup: clean architecture (Tier 35 refactor).
//
// A single SkipRequest builder resolves everything once:
//   1. Series identity -> backend ("tidb" for IMDb IDs, "aniskip" for kitsu:)
//      Item ID first, then video-ID prefix, then loaded metadata ID.
//   2. Episode numbers -> season/episode from metadata, else video-ID parse.
//   3. Cache key -> "tidb:<id>[:s:e]" or "aniskip:<id>:<ep>".
//
// watch_start_skip_lookup() and watch_refresh_skip() both build a request
// and hand it to watch_dispatch_skip(). No duplicated resolution logic.
// ============================================================================

Session::SkipRequest Session::watch_build_skip_request()
{
    SkipRequest req;
    if (prefs_.skip_mode == 0)
    {
        req.diag = "skip: off in settings";
        return req; // skip disabled: invalid, but the diagnostic says why
    }
    const std::string item_id = w_item_.id;
    req.is_movie = (w_item_.type == "movie");
    const bool is_series = (w_item_.type == "series");
    if (!req.is_movie && !is_series)
        return req; // not playable media: invalid, no diagnostic

    // --- Step 1: resolve the series ID and backend ---
    // Priority: item ID -> video-ID prefix -> metadata ID. First valid wins.
    std::string tidb_id;   // valid IMDb "tt" ID, if any
    std::string kitsu_id;  // "kitsu:..." ID, if any

    auto consider = [&](const std::string &id) {
        if (tidb_id.empty() && skipdb::valid_imdb_id(id))
            tidb_id = id;
        else if (kitsu_id.empty() && id.compare(0, 6, "kitsu:") == 0)
            kitsu_id = id;
    };
    consider(item_id);
    // Video IDs are often "ttXXXX:season:episode" even when the addon uses
    // a non-IMDb series ID. The prefix before the first colon is the series.
    if (!w_video_id_.empty())
    {
        const std::size_t c = w_video_id_.find(':');
        consider(c == std::string::npos ? w_video_id_ : w_video_id_.substr(0, c));
    }
    if (d_meta_loaded_)
        consider(d_meta_.id);

    if (!tidb_id.empty())
    {
        req.backend = "tidb";
        req.series_id = tidb_id;
    }
    else if (is_series && !kitsu_id.empty())
    {
        req.backend = "aniskip";
        req.series_id = kitsu_id;
    }
    else
    {
        // Unrecognized ID format: neither IMDb (TheIntroDB) nor kitsu
        // (AniSkip). Include the ID so the format is visible in nerd stats.
        req.diag = "skip: unsupported ID (" + item_id.substr(0, 24) + ")";
        return req;
    }

    // --- Step 2: resolve season/episode ---
    if (req.backend == "tidb" && !req.is_movie)
    {
        // Metadata match first, then "ttXXXX:season:episode" video-ID parse.
        for (const auto &v : d_meta_.videos)
            if (v.id == w_video_id_)
            {
                req.season = v.season;
                req.episode = v.episode;
            }
        if ((req.season < 1 || req.episode < 1) && !w_video_id_.empty())
        {
            const std::string &vid = w_video_id_;
            const std::size_t c1 = vid.find(':');
            if (c1 != std::string::npos)
            {
                const std::size_t c2 = vid.find(':', c1 + 1);
                if (c2 != std::string::npos)
                {
                    try {
                        int s = std::stoi(vid.substr(c1 + 1, c2 - c1 - 1));
                        int e = std::stoi(vid.substr(c2 + 1));
                        if (s >= 1 && e >= 1) { req.season = s; req.episode = e; }
                    } catch (...) { /* malformed: fall through */ }
                }
            }
        }
        if (req.season < 1 || req.episode < 1)
        {
            // Metadata not ready yet (e.g. playing from Continue Watching
            // before the detail fetch lands): defer and retry in
            // watch_update() once d_meta_ arrives, instead of silently
            // giving up. If metadata IS loaded but the episode isn't in it,
            // that's permanent — don't retry.
            req.diag = "skip: no meta";
            req.defer = !d_meta_loaded_;
            dlog("skip: no season/episode for video '%s' (meta %s)", w_video_id_.c_str(),
                 d_meta_loaded_ ? "loaded" : "not loaded");
            return req;
        }
        char keybuf[160];
        std::snprintf(keybuf, sizeof(keybuf), "tidb:%s:%d:%d",
                      req.series_id.c_str(), req.season, req.episode);
        req.key = keybuf;
    }
    else if (req.backend == "aniskip")
    {
        // Metadata match first, then trailing ":N" parse. Require a second
        // colon so a bare "kitsu:12345" isn't misparsed as episode 12345.
        for (const auto &v : d_meta_.videos)
            if (v.id == w_video_id_)
            {
                req.episode = v.episode;
                req.season = v.season;
            }
        if (req.episode < 1 && !w_video_id_.empty())
        {
            const std::size_t first = w_video_id_.find(':');
            const std::size_t c = w_video_id_.rfind(':');
            if (c != std::string::npos && c != first && c + 1 < w_video_id_.size())
            {
                try {
                    int e = std::stoi(w_video_id_.substr(c + 1));
                    if (e >= 1)
                        req.episode = e;
                } catch (...) { /* malformed: fall through */ }
            }
        }
        if (req.episode < 1)
        {
            req.diag = "skip: no meta";
            req.defer = !d_meta_loaded_;
            dlog("skip: no episode for kitsu video '%s' (meta %s)", w_video_id_.c_str(),
                 d_meta_loaded_ ? "loaded" : "not loaded");
            return req;
        }
        req.key = "aniskip:" + req.series_id + ":" + std::to_string(req.episode);
    }
    else // tidb movie: no season/episode needed
    {
        char keybuf[160];
        std::snprintf(keybuf, sizeof(keybuf), "tidb:%s", req.series_id.c_str());
        req.key = keybuf;
    }

    req.valid = true;
    return req;
}

void Session::watch_start_skip_lookup()
{
    w_skip_.clear();
    w_skip_done_ = -1;
    w_skip_prompted_ = -1;
    w_skip_deferred_ = false;
    w_skip_diag_.clear();
    // Seed from embedded MKV/MP4 chapter markers first: free, instant, offline.
    // The network lookup below only overwrites these when it returns non-empty
    // segments (community databases are more precise when they have data).
    w_skip_ = player_.chapter_skip_segments();
    if (!w_skip_.empty())
        dlog("skip: %zu segment(s) from embedded chapters", w_skip_.size());
    SkipRequest req = watch_build_skip_request();
    if (!req.valid)
    {
        if (!req.diag.empty())
            w_skip_diag_ = req.diag;
        if (req.defer)
            w_skip_deferred_ = true;
        return;
    }
    watch_dispatch_skip(req, false);
}

void Session::watch_dispatch_skip(const SkipRequest &req, bool bypass_cache)
{
    if (req.key == w_skip_key_ && !bypass_cache)
        return; // already looked up (or in flight) for this episode
    w_skip_key_ = req.key;

    const int gen = w_gen_;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    w_skip_cancel_ = cancel;

    if (req.backend == "tidb")
    {
        // The prefs map doubles as the in-memory cache: 30-day TTL for
        // positive entries, 24-hour for negative ones (an empty list means
        // the title definitively has no data). TheIntroDB is community-driven
        // and gains new submissions daily, so negative answers expire quickly.
        if (!bypass_cache)
        {
            const double now = static_cast<double>(std::time(nullptr));
            const auto found = prefs_.skipdb_cache.find(req.key);
            if (found != prefs_.skipdb_cache.end())
            {
                const double age = now - found->second.fetched_at;
                const double ttl = found->second.segs.empty() ? 86400.0 : 30 * 86400.0;
                if (found->second.fetched_at > 0 && age >= 0 && age < ttl)
                {
                    w_skip_ = found->second.segs;
                    if (w_skip_.empty())
                        w_skip_diag_ = "skip: no data";
                    return;
                }
                prefs_.skipdb_cache.erase(found); // stale: refetch below
            }
        }
        const std::string series_id = req.series_id;
        const int season = req.season, episode = req.episode;
        const bool is_movie = req.is_movie;
        const std::string key = req.key;
        // AniSkip fallback context, captured on the GL thread: anime is
        // routinely cataloged under IMDb tt... ids (Cinemeta/Torrentio), where
        // TheIntroDB has sparse coverage but AniSkip has nearly everything.
        const std::string title = w_item_.name;
        const double duration = player_.duration();
        const bool meta_ready = d_meta_loaded_;
        const std::string mal_key = series_id + ":S" + std::to_string(season > 0 ? season : 1);
        int mal_cached = 0;
        {
            const auto cached = prefs_.aniskip_mal.find(mal_key);
            if (cached != prefs_.aniskip_mal.end())
                mal_cached = cached->second;
        }
        struct Res
        {
            std::vector<aniskip::SkipSegment> segs;
            bool answered = false; // definitive answer: safe to (negative-)cache
            std::string diag;      // nerd-stats diagnostic when lookup fails
            int mal_id = 0;        // AniSkip fallback's MAL id (0 when unused)
            bool mal_via_jikan = false; // Jikan-derived: never persisted
            std::string mal_key;   // season-aware cache key for the MAL id
        };
        bg<Res>(
            [series_id, season, episode, is_movie, title, duration, meta_ready, mal_key, mal_cached, cancel]() {
                Res r;
                const bool tidb_answered = skipdb::fetch_tidb_segments(series_id, season, episode, is_movie,
                                                                      cancel.get(), r.segs);
                if (!r.segs.empty())
                {
                    r.answered = tidb_answered;
                    if (!r.answered)
                        r.diag = "skip: network error";
                    return r;
                }
                // TheIntroDB came back empty for a series episode: fall back
                // to AniSkip in the same worker. MAL treats each anime season
                // as its own entry (episodes start at 1), so the id cache is
                // keyed by series + season.
                if (!is_movie && !title.empty() && !(cancel && cancel->load()))
                {
                    const int s = season > 0 ? season : 1;
                    int mal = mal_cached;
                    bool via_jikan = false;
                    bool aniskip_answered = (mal > 0);
                    if (mal <= 0)
                        aniskip_answered =
                            aniskip::resolve_mal_id(series_id, title, s, cancel.get(), mal, &via_jikan);
                    r.mal_id = mal;
                    r.mal_via_jikan = via_jikan;
                    r.mal_key = mal_key;
                    if (mal > 0 && !(cancel && cancel->load()))
                    {
                        bool transport_failed = false;
                        aniskip::fetch_skip_times(mal, episode, cancel.get(), r.segs, &transport_failed,
                                                  duration);
                        // fetch_skip_times returning empty is a definitive
                        // "no data" unless the transport itself died.
                        aniskip_answered = !transport_failed;
                    }
                    // Only a definitive answer from BOTH backends may be
                    // negative-cached; a dead AniSkip transport must not
                    // poison the cache.
                    r.answered = tidb_answered && aniskip_answered;
                }
                else
                {
                    // No title to resolve the fallback with (or a movie): the
                    // tidb answer stands. But when metadata never loaded and
                    // the item name is empty, the title wasn't known yet, so
                    // don't negative-cache — the deferred retry may learn it.
                    r.answered = tidb_answered && (meta_ready || !title.empty());
                }
                if (!r.answered && r.segs.empty())
                    r.diag = "skip: network error";
                return r;
            },
            [this, gen, key](Res &r) {
                if (gen != w_gen_ || key != w_skip_key_)
                    return;
                // Persist a trustworthy fallback MAL id (never a
                // Jikan-derived guess) so later episodes skip the resolve.
                if (!r.mal_key.empty() && r.mal_id > 0 && !r.mal_via_jikan &&
                    prefs_.aniskip_mal[r.mal_key] != r.mal_id)
                {
                    prefs_.aniskip_mal[r.mal_key] = r.mal_id;
                    if (prefs_.aniskip_mal.size() > 500)
                        prefs_.aniskip_mal.erase(prefs_.aniskip_mal.begin());
                    save_prefs();
                }
                if (r.answered)
                {
                    // Cache definitive answers, positive and negative alike.
                    // Transport failures are never cached.
                    Prefs::SkipDbEntry e;
                    e.fetched_at = static_cast<double>(std::time(nullptr));
                    e.segs = r.segs;
                    prefs_.skipdb_cache[key] = std::move(e);
                    while (prefs_.skipdb_cache.size() > 500)
                    {
                        auto oldest = prefs_.skipdb_cache.begin();
                        for (auto it = prefs_.skipdb_cache.begin(); it != prefs_.skipdb_cache.end(); ++it)
                            if (it->second.fetched_at < oldest->second.fetched_at)
                                oldest = it;
                        prefs_.skipdb_cache.erase(oldest);
                    }
                    save_prefs();
                    if (r.segs.empty() && w_skip_.empty())
                        w_skip_diag_ = "skip: no data";
                }
                else if (!r.diag.empty())
                {
                    w_skip_diag_ = r.diag;
                }
                // Only overwrite chapter-derived segments when the network
                // actually found something: the databases are more precise,
                // but an empty network result must not clobber chapters.
                if (!r.segs.empty())
                    w_skip_ = std::move(r.segs);
                else if (w_skip_.empty() && r.answered)
                    w_skip_diag_ = "skip: no data";
            });
    }
    else // aniskip
    {
        const std::string series_id = req.series_id;
        const std::string title = w_item_.name;
        const int episode = req.episode;
        const int season = req.season > 0 ? req.season : 1;
        const double duration = player_.duration();
        const std::string key = req.key;
        int mal_id = 0;
        const auto cached = prefs_.aniskip_mal.find(series_id);
        if (cached != prefs_.aniskip_mal.end())
            mal_id = cached->second;
        struct Res
        {
            std::vector<aniskip::SkipSegment> segs;
            int mal_id = 0;
            bool mal_via_jikan = false; // Jikan-derived: don't persist (may be wrong)
            std::string diag; // nerd-stats diagnostic when lookup fails
        };
        bg<Res>(
            [series_id, title, episode, season, duration, mal_id, cancel]() {
                Res r;
                int mal = mal_id;
                bool via_jikan = false;
                if (mal <= 0 && !aniskip::resolve_mal_id(series_id, title, season, cancel.get(), mal, &via_jikan))
                {
                    r.diag = "skip: network error";
                    return r;
                }
                r.mal_id = mal;
                r.mal_via_jikan = via_jikan;
                if (cancel && cancel->load())
                    return r;
                bool transport_failed = false;
                aniskip::fetch_skip_times(mal, episode, cancel.get(), r.segs, &transport_failed, duration);
                if (r.segs.empty() && transport_failed)
                    r.diag = "skip: network error";
                return r;
            },
            [this, gen, key, series_id](Res &r) {
                if (gen != w_gen_ || key != w_skip_key_)
                    return;
                if (r.mal_id > 0 && prefs_.aniskip_mal[series_id] != r.mal_id)
                {
                    // Never persist a Jikan-derived id: the title search can
                    // attach the wrong show, and a cached wrong id poisons every
                    // future episode with no recovery path. Use it for this
                    // playback only; next time it re-resolves (Kitsu may be back).
                    if (!r.mal_via_jikan)
                    {
                        prefs_.aniskip_mal[series_id] = r.mal_id;
                        if (prefs_.aniskip_mal.size() > 500)
                            prefs_.aniskip_mal.erase(prefs_.aniskip_mal.begin());
                        save_prefs();
                    }
                }
                if (r.segs.empty() && w_skip_.empty())
                    w_skip_diag_ = r.diag.empty() ? "skip: no data" : r.diag;
                // Don't clobber chapter-derived segments with an empty result.
                if (!r.segs.empty())
                    w_skip_ = std::move(r.segs);
            });
    }
}

void Session::watch_refresh_skip()
{
    if (prefs_.skip_mode == 0)
    {
        say("Skip is disabled in Settings");
        return;
    }
    // Clear the deferred flag: the refresh is an explicit re-run, so a
    // stale deferred retry must not fire afterwards and duplicate it.
    w_skip_deferred_ = false;
    // Build the request once to get the canonical cache key, then clear it.
    // This uses the same resolution as the lookup, so the refresh actually
    // bypasses the cache even when metadata isn't loaded.
    SkipRequest req = watch_build_skip_request();
    if (req.valid && req.backend == "tidb")
        prefs_.skipdb_cache.erase(req.key);
    // Clear a possibly-poisoned AniSkip MAL-id mapping for this series so a
    // wrong Jikan-derived id gets re-resolved instead of reused forever.
    // (The fallback path keys by series + season, so clear that too.)
    if (req.valid)
    {
        prefs_.aniskip_mal.erase(req.series_id);
        if (req.backend == "tidb" && req.season > 0)
            prefs_.aniskip_mal.erase(req.series_id + ":S" + std::to_string(req.season));
    }
    else
        prefs_.aniskip_mal.erase(w_item_.id);
    save_prefs();
    // Clear the in-flight guard so the lookup actually re-runs. Signal the
    // old cancel first: without this, a slow in-flight fetch and the new one
    // run concurrently and last-writer-wins can clobber a fresh success
    // with a stale failure.
    if (w_skip_cancel_)
        w_skip_cancel_->store(true);
    w_skip_key_.clear();
    w_skip_.clear();
    w_skip_diag_.clear();
    w_skip_done_ = -1;
    w_skip_prompted_ = -1;
    say("Refreshing skip timings...");
    if (req.valid)
        watch_dispatch_skip(req, true); // bypass_cache: force refetch
    else if (!req.diag.empty())
        w_skip_diag_ = req.diag;
    w_menu_dirty_ = true;
}

int Session::watch_skip_at(double pos) const
{
    for (std::size_t i = 0; i < w_skip_.size(); ++i)
    {
        const auto &s = w_skip_[i];
        if (pos >= s.start && pos < s.end)
            return static_cast<int>(i);
    }
    return -1;
}

bool Session::watch_try_skip_prompt()
{
    if (playing.skip_prompt.empty())
        return false;
    const int idx = watch_skip_at(player_.position());
    if (idx < 0)
        return false;
    const auto &sg = w_skip_[static_cast<std::size_t>(idx)];
    const double dur = player_.duration();
    w_skip_done_ = idx;
    if (dur > 0)
        player_.seek(std::max(0.0, std::min(sg.end, dur - 1.0)));
    playing.skip_prompt.clear();
    say(aniskip::kind_label(sg.kind) + " skipped");
    return true;
}

void Session::watch_check_skip(double pos, double dur)
{
    playing.skip_prompt.clear();
    if (prefs_.skip_mode == 0 || w_skip_.empty() || dur <= 0)
    {
        playing.skip_segments.clear();
        w_skip_prompted_ = -1;
        return;
    }
    // Segment-end pre-warm: 3 s before a skip segment's end, warm that
    // timestamp in the player so an auto or Triangle skip lands on ready
    // data instead of rebuffering. Once per segment per episode.
    // Function-statics (not members): one Session exists, and this keeps
    // Worker B's session.hpp untouched.
    static double s_prewarm_end = -1.0;
    static std::string s_prewarm_key;
    if (s_prewarm_key != w_skip_key_)
    {
        s_prewarm_key = w_skip_key_;
        s_prewarm_end = -1.0;
    }
    for (const aniskip::SkipSegment &s : w_skip_)
    {
        if (pos >= s.start - 3.0 && pos < s.end && s.end != s_prewarm_end)
        {
            s_prewarm_end = s.end;
            player_.prewarm_time(s.end);
            break;
        }
    }
    playing.skip_segments = w_skip_;
    const int idx = watch_skip_at(pos);
    if (idx < 0)
    {
        w_skip_prompted_ = -1;
        return;
    }
    const aniskip::SkipSegment &sg = w_skip_[static_cast<std::size_t>(idx)];
    // Never seek past the end: an outro running to the credits clamps to
    // just before it. Skip when there is nothing worth jumping to.
    const double target = std::max(0.0, std::min(sg.end, dur - 1.0));
    if (target - pos < 1.0)
        return;
    // Auto-skip covers intros and recaps (and anime openings/endings).
    // Credits and previews are prompt-only even in Auto mode: they often
    // hide post-credit scenes.
    const bool auto_kind = sg.kind == "op" || sg.kind == "ed" ||
                           sg.kind == "intro" || sg.kind == "recap";
    if (prefs_.skip_mode == 2 && auto_kind && !playing.menu && idx != w_skip_done_)
    {
        // Auto-skip. A segment the user sought back into stays skipped-over
        // (w_skip_done_): seeking back is deliberate.
        w_skip_done_ = idx;
        player_.seek(target);
    }
    else if (prefs_.skip_mode == 1 ||
             (prefs_.skip_mode == 2 && !auto_kind && !playing.menu && idx != w_skip_done_))
    {
        // The Triangle-skip input is gated on the prompt being visible, so
        // the prompt must only exist when it is actually on screen: with the
        // OSD hidden the user would otherwise lose Triangle's audio-track
        // cycle to an invisible skip. (review 2026-10-13)
        if (playing.controls)
            playing.skip_prompt = "Skip " + aniskip::kind_label(sg.kind);
        else if (w_skip_prompted_ != idx)
        {
            // Prompt mode with the OSD hidden (the passive-viewer case): surface
            // the OSD once so the prompt is actually seen. The prompt still only
            // exists while the OSD is visible, preserving the Triangle invariant.
            w_skip_prompted_ = idx;
            watch_show_info();
            playing.skip_prompt = "Skip " + aniskip::kind_label(sg.kind);
        }
    }
}

void Session::watch_auto_subtitles()
{
    if (!prefs_.auto_subtitles || w_auto_sub_done_ || w_sub_active_ >= 0)
        return;
    // A remembered pick for this show outranks the generic auto-select.
    const Prefs::TrackMemory *mem = nullptr;
    {
        const std::string key = watch_memory_key();
        if (!key.empty())
        {
            const auto it = prefs_.track_memory.find(key);
            if (it != prefs_.track_memory.end())
                mem = &it->second;
        }
    }
    if (mem && mem->sub_off)
    {
        w_auto_sub_done_ = true; // remembered "off": stay silent
        return;
    }
    if (mem && !mem->sub_lang.empty())
    {
        int best = -1, best_rank = 100;
        for (std::size_t i = 0; i < w_subs_.size(); ++i)
        {
            const WatchSub &s = w_subs_[i];
            if (s.lang != mem->sub_lang || (s.embedded >= 0) != mem->sub_internal)
                continue;
            const int r = sub_auto_rank(s, mem->sub_lang);
            if (r < best_rank)
            {
                best_rank = r;
                best = static_cast<int>(i);
            }
        }
        if (best >= 0)
        {
            w_auto_sub_done_ = true;
            w_sub_auto_ = true;
            watch_select_sub(best);
            return;
        }
        // No match yet — the right tracks may still be arriving; fall through
        // to the generic pick. If it locks in, the late-arriving embedded
        // tracks still trigger a re-pick above, which retries this memory.
    }
    for (const auto &lang : pref_langs(prefs_.subtitle_langs))
    {
        // Do not pick subtitles in the language being heard.
        bool heard = false;
        for (const auto &t : player_.audio_tracks())
            if (t.stream == player_.audio_stream() && t.lang == lang)
                heard = true;
        if (heard)
        {
            w_auto_sub_done_ = true;
            return;
        }
        // Pick the best track: internal (embedded) subtitles before external
        // ones, and skip junk tracks (signs/songs-only) unless nothing better
        // exists. Cue counts are only parsed once a track is used, so at
        // auto-select time the label heuristic does most of the work (EVO's
        // MIN_USEFUL_CUES rule); the cue gate applies where counts are known.
        int best = -1, best_rank = 100;
        for (std::size_t i = 0; i < w_subs_.size(); ++i)
        {
            // SDH tracks are never auto-picked: the user asks for them
            // explicitly (a remembered manual pick above still honors them).
            if (sub_track_is_sdh(w_subs_[i].label))
                continue;
            const int r = sub_auto_rank(w_subs_[i], lang);
            if (r < best_rank)
            {
                best_rank = r;
                best = static_cast<int>(i);
            }
        }
        if (best >= 0)
        {
            w_auto_sub_done_ = true;
            w_sub_auto_ = true;
            watch_select_sub(best);
            return;
        }
    }
    // Last resort: when nothing carried a language tag, a single untagged
    // embedded track is probably the right one.
    w_auto_sub_done_ = true;
    int untagged = -1, untagged_count = 0;
    for (std::size_t i = 0; i < w_subs_.size(); ++i)
        if (w_subs_[i].embedded >= 0 && w_subs_[i].lang.empty())
        {
            ++untagged_count;
            untagged = static_cast<int>(i);
        }
    if (untagged_count == 1)
    {
        w_sub_auto_ = true;
        watch_select_sub(untagged);
    }
}

// ---------------------------------------------------------------------------
// The next episode

namespace
{

int preresolve_res_rank(const std::string &r)
{
    // Mirrors detail_refresh_streams()' kResolutionOrder ("4K" best first).
    static const char *const order[] = {"4K", "1440p", "1080p", "720p", "SD", "Other"};
    for (int i = 0; i < 6; ++i)
        if (r == order[i])
            return i;
    return 5;
}

// Pick the stream the automatic advance would most likely pick, without
// touching the detail page's stream list: best resolution inside the
// max-resolution cap, then the same binge group as the playing episode,
// else the same addon, else the first. A miss only costs the speculation:
// play_stream resolves the normal way.
int pick_preresolve_stream(std::vector<::Stream> &streams, const std::string &binge_group, const std::string &addon,
                           const std::string &maxres)
{
    if (streams.empty())
        return -1;
    std::vector<std::size_t> order(streams.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::vector<std::string> res(streams.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        res[i] = stream_view(streams[i]).resolution;
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return preresolve_res_rank(res[a]) < preresolve_res_rank(res[b]); });
    // The max-resolution cap, as in detail_refresh_streams().
    int min_rank = 5;
    if (maxres == "1080p")
        min_rank = 2;
    else if (maxres == "720p")
        min_rank = 3;
    else if (maxres != "auto" && maxres != "4k")
        min_rank = 4;
    std::vector<std::size_t> capped;
    for (std::size_t i : order)
        if (preresolve_res_rank(res[i]) >= min_rank)
            capped.push_back(i);
    const std::vector<std::size_t> &cand = capped.empty() ? order : capped;
    int pick = -1;
    if (!binge_group.empty())
        for (std::size_t i : cand)
            if (streams[i].binge_group == binge_group)
            {
                pick = static_cast<int>(i);
                break;
            }
    if (pick < 0)
        for (std::size_t i : cand)
            if (streams[i].addon == addon)
            {
                pick = static_cast<int>(i);
                break;
            }
    if (pick < 0 && !cand.empty())
        pick = static_cast<int>(cand[0]);
    return pick;
}

} // namespace

// At ~80% of a series episode, fetch the next episode's streams and start
// resolving the likely pick's playable URL in the background. The
// transition's play_stream takes the cached result when it matches, so the
// torrent-engine start / server /create that makes episode changes feel
// slow usually happens while the current episode is still playing. The
// winning stream is picked exactly the way the transition's autoplay would
// pick it (season-pack fingerprint) and cached in w_next_stream_, so the
// transition plays instantly without a detail-page round trip. Runs once per
// episode (w_next_preresolved_vid_); the detail page is never touched.
void Session::watch_preresolve_next()
{
    const ::Video *ne = next_episode();
    if (!ne || ne->id == w_next_preresolved_vid_)
        return;
    w_next_preresolved_vid_ = ne->id;
    w_next_stream_ready_ = false;
    const std::string video_id = ne->id;
    const int season = ne->season, episode = ne->episode;
    std::string type = d_meta_loaded_ ? d_meta_.type : d_item_.type;
    if (type.empty())
        type = d_item_.type;
    // Addons serving streams for the next episode, the playing stream's addon
    // first (it usually answers fastest for the same source).
    const std::string cur_addon = w_stream_.addon;
    std::vector<std::pair<std::string, std::string>> sources; // addon name, stream URL
    for (const auto &a : addons_)
        if (!cur_addon.empty() && a->name == cur_addon && !addon_disabled(*a) &&
            a->supports("stream", type, video_id))
            sources.push_back({a->name, a->resource_url("stream", type, video_id)});
    for (const auto &a : addons_)
        if ((cur_addon.empty() || a->name != cur_addon) && !addon_disabled(*a) &&
            a->supports("stream", type, video_id))
            sources.push_back({a->name, a->resource_url("stream", type, video_id)});
    if (sources.empty())
        return;
    struct Got
    {
        std::vector<::Stream> streams;
    };
    bg_urgent<Got>(
        [sources]() {
            Got g;
            for (const auto &s : sources)
            {
                json j;
                std::string err;
                if (fetch_json(s.second, j, err, 30))
                {
                    std::vector<::Stream> v = parse_streams(j, s.first);
                    g.streams.insert(g.streams.end(), v.begin(), v.end());
                }
            }
            return g;
        },
        [this, video_id, season, episode](Got &g) {
            if (video_id != w_next_preresolved_vid_ || !watching)
                return; // moved on (stopped, or a newer pre-resolve won)
            // Score exactly the way the transition's autoplay would: arm the
            // fingerprint from the playing stream and pick, then restore the
            // pre-existing autoplay state (a genuine in-flight autoplay must
            // not be disturbed by this background scoring).
            const bool was_pending = autoplay_pending_;
            const std::string sb = autoplay_binge_, sa = autoplay_addon_, sh = autoplay_hash_,
                              sr = autoplay_res_, sn = autoplay_name_;
            arm_autoplay_from(w_stream_);
            const int pick = pick_autoplay_stream(g.streams, true);
            autoplay_pending_ = was_pending;
            autoplay_binge_ = sb;
            autoplay_addon_ = sa;
            autoplay_hash_ = sh;
            autoplay_res_ = sr;
            autoplay_name_ = sn;
            if (pick < 0 || static_cast<std::size_t>(pick) >= g.streams.size())
                return;
            const ::Stream &win = g.streams[static_cast<std::size_t>(pick)];
            w_next_stream_ = win;
            w_next_stream_ready_ = true;
            dlog("watch: pre-resolved the next episode's stream (%s)", win.name.c_str());
            // Play-Next freeze fix: prewarm the connection pool + resolve the CDN
            // redirect instead of prefetching the fragile 2MB head buffer
            // (prefetch_take() can consume g_prefetch mid-perform).
            if (!win.url.empty())
            {
                g_tasks.run([u = win.url, h = win.request_headers] {
                    NetStream::prewarm(u, h);
                });
            }
            else if (!win.info_hash.empty())
                spec_resolve_begin(win, season, episode, 300.0);
        });
}

const ::Video *Session::next_episode() const
{
    const auto &vids = d_meta_.videos;
    std::size_t cur = vids.size();
    for (std::size_t i = 0; i < vids.size(); ++i)
        if (vids[i].id == w_video_id_)
            cur = i;
    if (cur + 1 >= vids.size())
        return nullptr;
    const ::Video &next = vids[cur + 1];
    if (next.season == 0 && vids[cur].season != 0)
        return nullptr; // do not roll into the specials
    return &next;
}

std::string Session::episode_heading(const ::Video &v)
{
    std::string heading = "S" + std::to_string(v.season) + " E" + std::to_string(v.episode);
    if (!v.title.empty())
        heading += "  \xC2\xB7  " + v.title;
    return heading;
}

void Session::watch_upnext_fire(bool manual)
{
    if (manual)
        w_binge_count_ = 0;
    else if (watch_binge_check())
        return; // "Still watching?" is up instead of advancing
    // The union of what the two countdown-zero paths did: clear the card,
    // mark the episode finished (idempotent — the Ended path already saved),
    // and roll into the next episode.
    w_upnext_until_ = 0;
    w_upnext_heading_.clear();
    playing.upnext_title.clear();
    playing.upnext_remaining = -1;
    watch_save_progress(true, true);
    // Seamless Play-Next: keep Episode N's last frame on the GPU across the
    // stop for both the fast and the slow path (the slow path stays in-player
    // now); play_stream(..., true) takes it from there.
    watch_stop(true, true);
    watch_next_episode();
}

// An automatic advance is about to happen: the binge guard counts it and,
// once the threshold is reached, puts up the "Still watching?" prompt
// instead. Returns true when it intercepted (no advance happened).
bool Session::watch_binge_check()
{
    const int threshold = prefs_.binge_ask;
    if (threshold <= 0 || !prefs_.autoplay_next)
        return false;
    ++w_binge_count_;
    if (w_binge_count_ < threshold)
        return false;
    w_binge_asking_ = true;
    w_binge_left_ = kBingeTimeoutSecs;
    w_binge_prev_ = 0;
    w_upnext_heading_.clear();
    w_upnext_until_ = 0;
    playing.upnext_title.clear();
    playing.upnext_remaining = -1;
    playing.binge_prompt = true;
    playing.binge_left = w_binge_left_;
    return true;
}

bool Session::watch_next_fast_ready() const
{
    if (!w_next_stream_ready_)
        return false;
    const ::Video *next = next_episode();
    return next && w_next_preresolved_vid_ == next->id;
}

void Session::watch_next_episode()
{
    const ::Video *next = next_episode();
    if (!next)
        return;

    for (std::size_t i = 0; i < d_seasons_.size(); ++i)
        if (d_seasons_[i] == next->season && static_cast<int>(i) != d_season_idx_)
            detail_set_season(static_cast<int>(i));
    for (std::size_t i = 0; i < d_season_videos_.size(); ++i)
        if (d_season_videos_[i]->id == next->id)
        {
            detail.episode_hint = static_cast<int>(i);
            ++detail.hint_serial;
        }

    // Fast path: the 80% pre-resolve already fetched and scored this episode's
    // streams — play the winning stream directly, no detail-page round trip.
    // play_stream reads d_video_id_ into w_video_id_, so set it first.
    if (w_next_stream_ready_ && w_next_preresolved_vid_ == next->id)
    {
        w_next_stream_ready_ = false; // consume the cached pick
        d_video_id_ = next->id;
        d_stream_heading_ = episode_heading(*next);
        w_binge_auto_ = true; // the play_stream this triggers must not reset the binge count
        w_stream_idx_ = -1;   // the cached stream is not from d_streams_
        w_failover_tries_ = 0;
        play_stream(w_next_stream_, false, true); // seamless: hold the last frame in-player
        // Refresh the picker list in the background so it shows this episode's
        // streams, not the previous episode's, if the user opens it mid-playback.
        detail_load_streams(next->id, d_stream_heading_);
        say("Next: " + d_stream_heading_);
        return;
    }

    // Slow path: the next episode's stream wasn't pre-resolved. Stay
    // in-player instead of dropping to the launch modal: the callers kept
    // Episode N's last frame on the GPU, and it stays up while the streams
    // resolve in the background. (watch_stop was already called by the
    // caller — re-calling it here would undo the film-mode carry-over — so
    // the in-player state is set directly.) The eventual autoplay
    // play_stream(..., watching=true) takes the seamless path: no launch
    // modal, no destroyed texture.
    autoplay_pending_ = true;
    autobest_pending_ = false; // the binge pick owns stream selection from here
    arm_autoplay_from(w_stream_);
    w_binge_auto_ = true; // the play_stream this triggers must not reset the binge count
    std::string heading = episode_heading(*next);
    watching = true;
    w_seamless_transition_ = true;
    w_seamless_gen_ = static_cast<int>(video_gen_); // first-frame detect needs a fresh snapshot
    launch_open = false;
    playing.buffering = 0;
    playing.title = w_item_.name;
    playing.subtitle = heading;
    playing.controls = true;
    w_info_until_ = now_seconds() + 4.0;
    w_launch_since_ = now_seconds();
    detail_load_streams(next->id, heading);
    say("Next: " + heading);
}

} // namespace sx
