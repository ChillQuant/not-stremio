// The session's core: setup, the account, addons, the library, settings, the
// pop-ups and the artwork. The Board, Discover and Library are session_browse.cpp,
// a title's page is session_detail.cpp.

#include "session.hpp"

#include "http.h"
#include "hwdec_ps5.h"
#include "netstream.h"
#include "polish.hpp" // WORKER 10: one-line per-frame pump (haptics + hints bar)
#include "torrent/engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sx
{

namespace
{

constexpr std::size_t kTextureBudget = 640u << 20; // bytes of artwork kept on the GPU

enum SettingIndex
{
    // WORKER 7 (SETTINGS): "Recently changed" section, built dynamically at
    // the end of refresh_settings(). Slots stay hidden (empty label) while
    // recent_order_ is empty.
    kHdrRecent,
    kRecent0,
    kRecent1,
    kRecent2,
    kRecent3,
    kRecent4,
    kHdrAccount,
    kAccount,
    kHdrPlayback,
    kTorrents,
    kServer,
    kAutoplay,
    kAutoplayBest,
    kBingeAsk,
    kMatchFrameRate,
    kHdrProbe,
    kAspectMode,
    kSkipMode,
    kMaxResolution,
    kUpscaler,
    kUpscalerSharpness,
    kDeepBlacks,
    kTonemapMode,
    kHdrSubtitles,
    kSubStyle, // WORKER 7: subtitle style preset (Default/Large/High contrast)
    kSubLangs,
    kAutoSubs,
    kSubSize,
    kSubColor,
    kSubBg,
    kSubPos,
    kOsUser,
    kOsPass,
    kHdrAudio,
    kAudioLangs,
    kAudioOutput,
    kVolumeBoost,
    kNightMode,
    kDolbyPassthrough,
    kHdrGeneral,
    kUiSounds,
    kHaptics,
    kAddAddon,
    kReload,
    kExit,
    kHdrStorage,
    kClearCache,
    kRecordLog,
    kSettingCount
};

std::string size_label(const std::string &s)
{
    if (s == "sub-s")
        return "Small";
    if (s == "sub-l")
        return "Large";
    return "Medium";
}

std::string subcolor_label(const std::string &s)
{
    return s == "yellow" ? "Yellow" : "White";
}

std::string format_bytes(std::uint64_t n)
{
    char buf[32];
    if (n >= 1ull << 30)
        std::snprintf(buf, sizeof(buf), "%.1f GB", n / double(1ull << 30));
    else if (n >= 1ull << 20)
        std::snprintf(buf, sizeof(buf), "%.1f MB", n / double(1ull << 20));
    else if (n >= 1ull << 10)
        std::snprintf(buf, sizeof(buf), "%llu KB", static_cast<unsigned long long>((n + 512) >> 10));
    else
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(n));
    return buf;
}

std::string subpos_label(const std::string &s)
{
    return s == "raised" ? "Raised" : "Bottom";
}

std::string binge_label(int v)
{
    return v <= 0 ? "Off" : "After " + std::to_string(v) + " episodes";
}

std::string audioout_label(int v)
{
    return v == 1 ? "Surround (5.1/7.1 PCM)" : "Stereo";
}

std::string skipmode_label(int v)
{
    return v == 2 ? "Auto-skip" : v == 1 ? "Show a Skip button" : "Off";
}

std::string volboost_label(int v)
{
    return std::to_string(v) + "%";
}

std::string maxres_label(const std::string &s)
{
    if (s == "4k")
        return "4K";
    if (s == "1080p")
        return "1080p";
    if (s == "720p")
        return "720p";
    if (s == "480p")
        return "480p";
    return "Auto";
}

std::string upscale_label(int m)
{
    switch (m) {
    case 1: return "FSR1";
    case 2: return "FSR1 + Sharpening";
    case 3: return "Anime4K";
    case 4: return "Cinema HQ (Lanczos + CAS)";
    default: return "Off";
    }
}

// WORKER 7 (SETTINGS): choice tables shared by the dropdowns in
// settings_activate() and by inline Left/Right stepping in
// settings_step_row(). One source of truth so both paths write identical
// pref values.
static const char *kSubSizes[] = {"sub-s", "sub-m", "sub-l"};
static const char *kSubColors[] = {"white", "yellow"};
static const char *kSubPositions[] = {"bottom", "raised"};
static const char *kAspectModes[] = {"fit", "fill"};
static const int kVolumeBoosts[] = {100, 125, 150, 200};
static const int kBingeAsks[] = {0, 3, 5, 10};
static const char *kSubStyleNames[] = {"Default", "Large", "High contrast"};

// The first letter of a name (one UTF-8 character): an addon's mark when it has no logo.
std::string first_letter(const std::string &name)
{
    std::size_t len = name.empty() ? 0 : 1;
    while (len < name.size() && (static_cast<unsigned char>(name[len]) & 0xC0) == 0x80)
        ++len;
    return name.substr(0, len);
}

std::uint32_t colour_for(const std::string &name)
{
    static const std::uint32_t palette[] = {0x7b5bf5, 0x3b82f6, 0x0ea5a4, 0xe0568a, 0xe08a3c, 0x6d6ae0};
    return palette[fnv1a(name) % (sizeof(palette) / sizeof(palette[0]))];
}

// Dynamic per-option help for the settings dropdowns. Each describes the
// option the row is currently showing (or a dropdown candidate); all claims
// are verified against the implementation (upscale.cpp, gpu_convert.cpp,
// src/player.cpp, session_watch.cpp, ui/player.cpp).

const char *torrents_desc(bool builtin) { return builtin ? "Built-in engine: downloads torrents directly on your PS5 using up to 6 GB of rolling cache. No PC or external server needed." : "Streaming server: sends magnet links to your external Stremio server (PC/NAS) for downloading and transcoding."; }
const char *binge_desc(int episodes) { if (episodes <= 0) return "Off: episodes play back-to-back continuously without ever pausing to ask if you are still watching."; if (episodes <= 3) return "After 3 episodes: pauses autoplay after 3 consecutive episodes to check if you are still awake."; if (episodes <= 5) return "After 5 episodes: pauses autoplay after 5 consecutive episodes to check if you are still watching."; return "After 10 episodes: pauses autoplay only after a long 10-episode marathon."; }
const char *aspect_desc(const std::string &mode) { return mode == "fill" ? "Fill: scales the video until it fills the entire 16:9 screen, cropping black bars (and some side edges on ultrawide films)." : "Fit: shows the entire video frame at its original aspect ratio with letterbox bars when needed."; }
const char *skip_desc(int mode) { switch (mode) { case 2: return "Auto-skip: automatically jumps past intros, recaps, and openings as soon as they start (using chapters, IntroDB, or AniSkip)."; case 1: return "Show a Skip button: displays a 'Skip Intro' prompt in the bottom-right corner; press Triangle when you want to skip."; default: return "Off: never prompts or skips intros, recaps, or credits."; } }
const char *maxres_desc(const std::string &res) { if (res == "4k") return "4K (2160p): allows all streams up to 4K UHD. Best for fast connections and 4K TVs."; if (res == "1080p") return "1080p Full HD: filters out 4K streams to save bandwidth and start playback faster. Pair with Upscaler for near-4K sharpness."; if (res == "720p") return "720p HD: limits streams to 720p or lower for slower connections or mobile hotspots."; if (res == "480p") return "480p SD: picks the smallest possible streams to minimize data usage."; return "Auto: ranks the highest-quality streams first with no resolution cap, so 4K streams are included when available."; }
const char *upscale_desc(int mode) { switch (mode) { case 1: return "FSR1 (EASU): AMD FidelityFX spatial upscaler without extra sharpening. Reconstructs clean edges on 720p/1080p video."; case 2: return "FSR1 + Sharpening (EASU + RCAS): AMD edge-aware upscaling followed by Contrast-Adaptive Sharpening. Great all-around crispness."; case 3: return "Anime4K: specialized directional line-reconstruction shader for anime and 2D animation. Thins and sharpens hand-drawn lines without ringing."; case 4: return "Cinema HQ: anti-ringing separable bicubic spatial upscaling (4x4 kernel, min/max de-ringed) followed by RCAS contrast-adaptive sharpening. Natural, halo-free film & series quality."; default: return "Off: standard bilinear GPU scaling with fast 8-bit chroma."; } }
const char *sharpness_desc(int upscaler_mode, double sharp) { const int pct = static_cast<int>(sharp * 100.0 + 0.5); if (upscaler_mode == 3) return pct <= 25 ? "Gentle line reconstruction for clean 1080p anime." : pct <= 50 ? "Balanced line thinning and edge restoration for most anime (recommended)." : "Aggressive line push for soft 720p or heavily compressed anime."; return pct == 0 ? "0%: minimal contrast sharpening over the upscaled picture." : pct <= 50 ? "25%-50%: natural contrast-adaptive detail enhancement without film-grain harshness (recommended)." : "75%-100%: strong edge crispness; best for clean digital sources, may emphasize heavy film grain."; }
const char *deepblacks_desc(int mode) { switch (mode) { case 1: return "Low: gently pulls slightly lifted grey letterbox bars and near-black shadows down to true black without crushing shadow detail."; case 2: return "High: stronger toe curve for washed-out SDR encodes or bright rooms; maximizes perceived contrast on OLED and VA panels."; default: return "Off: preserves the exact mastered shadow levels of the source stream."; } }
const char *tonemap_desc(int mode) { return mode == 1 ? "BT.2390 (Broadcast): ITU broadcast-standard EETF roll-off. Preserves accurate mid-tones and skin brightness, compressing only bright highlights." : "Hable (Filmic): Uncharted 2 filmic S-curve. Gives HDR10, HLG, and Dolby Vision streams rich cinema contrast with smooth highlight shoulders."; }
const char *substyle_desc(int style) { switch (style) { case 1: return "Large preset: sets Subtitle size to Large on white text with a 55% dark box at the bottom."; case 2: return "High contrast preset: sets Large yellow text on an 85% dark background box, raised higher on screen for effortless reading."; default: return "Default preset: Medium white text with a 55% dark box at the bottom of the screen."; } }
const char *audioout_desc(int out) { return out == 1 ? "Surround (5.1/7.1 PCM): decodes surround tracks into discrete 8-channel uncompressed LPCM over HDMI (requires eARC or an AVR)." : "Stereo: folds 5.1/7.1 surround tracks down to 2-channel stereo using a dialogue-boosted Dolby LoRo matrix + LFE sub fold-in."; }
const char *volboost_desc(int vb) { if (vb <= 100) return "100% (Unity): bit-transparent volume with no extra gain."; if (vb <= 125) return "125% (+2 dB): mild lift for slightly quiet movies, protected by a -3 dBFS soft-knee limiter."; if (vb <= 150) return "150% (+3.5 dB): medium boost for quiet surround downmixes; loud explosions are soft-limited so speakers never clip."; return "200% (+6 dB): maximum dialogue lift for very quiet masters, with full tanh soft-clipping protection."; }

} // namespace

// Forward declaration: defined below; used by Session::init().
static bool is_torrent_cache(const std::string &name);

Item item_from_meta_json(const json &j)
{
    Item it;
    it.id = jstr(j, "id", jstr(j, "imdb_id"));
    it.type = jstr(j, "type");
    it.name = jstr(j, "name");
    it.poster = jstr(j, "poster");
    it.background = jstr(j, "background");
    it.logo = jstr(j, "logo");
    it.description = jstr(j, "description");
    it.release_info = jstr(j, "releaseInfo", jstr(j, "year"));
    it.runtime = jstr(j, "runtime");
    it.imdb_rating = jstr(j, "imdbRating");
    const json &g = jobj(j, "genres");
    if (g.is_array())
        for (const auto &v : g)
            if (v.is_string())
                it.genres.push_back(v.get<std::string>());
    return it;
}

// ---------------------------------------------------------------------------
// Setup

bool Session::init(const std::string &base_dir, const std::string &data_dir, hui::gfx::Renderer *renderer)
{
    base_dir_ = base_dir;
    data_dir_ = data_dir;
    renderer_ = renderer;
    make_dirs(data_dir_);
    log_open(data_dir_ + "/log.txt");
    http_init(base_dir_ + "/ca-bundle.crt");
    g_tasks.start(8);
    // The hardware video decoder module used to load here, serially, between
    // boot frames. Nothing touches it until the first playback
    // (HwDecoder::open refuses without it, and the player only opens from
    // watch_start next to the already-lazy GpuYuv::ensure()), so it rides the
    // task pool, next to the addon fetches, instead of stalling the boot.
    g_tasks.run([] { HwDecoder::load_module(); });
    boot_start_ = now_seconds();
    tex_.init(data_dir_ + "/art", renderer_, 6);
    // The app's storage holds the read-ahead: the built-in torrent engine's 6 GB
    // rolling cache, and up to 6 GB for a stream from a server or a direct link.
    bt::Engine::get().configure(data_dir_, 6ll << 30);
    NetStream::set_cache_dir(data_dir_);
    // Boot-only cleanup of leftover torrent cache files (a paused-then-resumed
    // torrent within the same session keeps its cache — this runs at boot only).
    if (DIR *d = opendir(data_dir_.c_str())) {
        while (dirent *e = readdir(d))
            if (is_torrent_cache(e->d_name))
                unlink((data_dir_ + "/" + e->d_name).c_str());
        closedir(d);
    }
    refresh_cache_size_bg(); // the walk is slow; settings reads the cached value

    load_prefs();
    load_progress();
    clock = format_clock();
    idle_since_ = now_seconds(); // the screensaver counts from boot
    board.status = "Loading your catalogs...";
    // Stale-while-revalidate: paint the last good board before any network
    // call. False on first-ever launch: nothing is cached yet.
    addons_cache_load(); // restore last-known addons so the board can build at boot
    boot_done_ = true;   // the snapshot (or an empty board) is already on screen: no boot gate to wait through
    // Snapshot boot: hydrate the snapshot's artwork inside a tight
    // synchronous disk budget so the first paint already has posters; the
    // normal async path takes over the moment the budget lapses.
    tex_.set_sync_disk_budget(0.045);
    board_snapshot_load();
    search_index_ensure(); // start the on-device search index from snapshot/boot content
    refresh_art(last_cursor_);
    tex_.set_sync_disk_budget(0.0);
    refresh_settings();
    refresh_library();
    refresh_addons();
    load_addons();
    if (signed_in())
        load_library();
    return true;
}

void Session::shutdown()
{
    if (shutdown_)
        return;
    shutdown_ = true;
    if (watching)
        watch_stop(false);
    player_.close();
    // Flush the board (cursor + Continue Watching) so the next boot's
    // snapshot opens exactly where this session left off.
    board_snapshot_save();
    bt::Engine::get().shutdown();
    g_tasks.stop();
    tex_.shutdown();
    save_progress();
}

// ---------------------------------------------------------------------------
// Persistence

void Session::load_prefs()
{
    const std::string path = data_dir_ + "/settings.json";
    json j;
    std::string err;
    if (load_json(path, j, &err))
    {
        prefs_.server_url = jstr(j, "server_url");
        prefs_.builtin_torrents = jbool(j, "builtin_torrents", prefs_.builtin_torrents);
        prefs_.subtitle_langs = jstr(j, "subtitle_languages", prefs_.subtitle_langs);
        prefs_.auto_subtitles = jbool(j, "auto_subtitles", prefs_.auto_subtitles);
        prefs_.sub_size = jstr(j, "subtitle_size", prefs_.sub_size);
        prefs_.audio_langs = jstr(j, "audio_languages");
        prefs_.audio_output = (int)jnum(j, "audio_output", prefs_.audio_output);
        if (prefs_.audio_output < 0 || prefs_.audio_output > 1)
            prefs_.audio_output = 0;
        {
            int vb = (int)jnum(j, "volume_boost", prefs_.volume_boost);
            prefs_.volume_boost = (vb == 125 || vb == 150 || vb == 200) ? vb : 100;
        }
        {
            int v = (int)jnum(j, "volume", prefs_.volume);
            prefs_.volume = v < 0 ? 0 : v > 200 ? 200 : v;
        }
        prefs_.night_mode = jbool(j, "night_mode", prefs_.night_mode);
        prefs_.dolby_passthrough = jbool(j, "dolby_passthrough", prefs_.dolby_passthrough);
        prefs_.autoplay_next = jbool(j, "autoplay_next", prefs_.autoplay_next);
        prefs_.autoplay_best = jbool(j, "autoplay_best", prefs_.autoplay_best);
        prefs_.binge_ask = std::clamp(static_cast<int>(jnum(j, "binge_ask", prefs_.binge_ask)), 0, 10);
        {
            const std::string mr = jstr(j, "max_resolution", prefs_.max_resolution);
            prefs_.max_resolution = (mr == "4k" || mr == "1080p" || mr == "720p" || mr == "480p") ? mr : "auto";
        }
        prefs_.os_user = jstr(j, "os_user", prefs_.os_user);
        prefs_.os_pass = jstr(j, "os_pass", prefs_.os_pass);
        // ---- SEARCH UX (worker 5): recent searches with timestamps ----
        // Exactly 5 kept, auto-expire after 30 days. The legacy plain-string
        // "search_history" is migrated once (stamped with now) and then dropped.
        {
            prefs_.recent_searches.clear();
            const double now = static_cast<double>(now_epoch_ms()) / 1000.0;
            constexpr double kRecentTtlS = 30.0 * 24.0 * 3600.0;
            constexpr std::size_t kRecentMax = 5;
            auto push_recent = [&](const std::string &q, double at) {
                if (q.empty() || prefs_.recent_searches.size() >= kRecentMax)
                    return;
                if (at <= 0 || now - at > kRecentTtlS)
                    return; // expired
                for (const auto &e : prefs_.recent_searches)
                    if (e.query == q)
                        return; // deduped
                prefs_.recent_searches.push_back({q, at});
            };
            const json &rs = jobj(j, "recent_searches");
            if (rs.is_array())
            {
                for (const auto &v : rs)
                    push_recent(jstr(v, "q"), jnum(v, "t", now));
            }
            else
            {
                const json &sh = jobj(j, "search_history");
                if (sh.is_array())
                    for (const auto &v : sh)
                        if (v.is_string())
                            push_recent(v.get<std::string>(), now);
            }
        }
        prefs_.match_frame_rate = jbool(j, "match_frame_rate", prefs_.match_frame_rate);
        prefs_.hdr_probe = jbool(j, "hdr_probe", prefs_.hdr_probe);
        prefs_.sub_color = jstr(j, "subtitle_color", prefs_.sub_color);
        // The old "subtitle_bg" was "none" | "box": migrate once, then the
        // new integer key takes over and the old one is dropped on save.
        if (j.contains("subtitle_bg"))
            prefs_.sub_bg_opacity = jstr(j, "subtitle_bg", "") == "box" ? 55 : 0;
        else
            prefs_.sub_bg_opacity = static_cast<int>(jnum(j, "subtitle_bg_opacity", prefs_.sub_bg_opacity));
        if (prefs_.sub_bg_opacity < 0 || prefs_.sub_bg_opacity > 100)
            prefs_.sub_bg_opacity = 55;
        prefs_.sub_pos = jstr(j, "subtitle_pos", prefs_.sub_pos);
        prefs_.aspect_mode = jstr(j, "aspect_mode", prefs_.aspect_mode);
        if (prefs_.aspect_mode != "fit" && prefs_.aspect_mode != "fill")
            prefs_.aspect_mode = "fit";
        prefs_.skip_mode = static_cast<int>(jnum(j, "skip_mode", prefs_.skip_mode));
        if (prefs_.skip_mode < 0 || prefs_.skip_mode > 2)
            prefs_.skip_mode = 1;
        prefs_.disc_sort = std::clamp(static_cast<int>(jnum(j, "disc_sort", prefs_.disc_sort)), 0, 2);
        prefs_.audio_delay = jnum(j, "audio_delay", 0.0);
        if (!std::isfinite(prefs_.audio_delay) || prefs_.audio_delay < -0.5 || prefs_.audio_delay > 0.5)
            prefs_.audio_delay = 0.0;
        prefs_.upscaler = std::clamp(static_cast<int>(jnum(j, "upscaler", prefs_.upscaler)), 0, 4);
        prefs_.deep_blacks = std::clamp(static_cast<int>(jnum(j, "deep_blacks", prefs_.deep_blacks)), 0, 2);
        prefs_.tonemap_mode = std::clamp(static_cast<int>(jnum(j, "tonemap_mode", prefs_.tonemap_mode)), 0, 1);
        prefs_.upscaler_sharpness = jnum(j, "upscaler_sharpness", 0.5);
        if (!std::isfinite(prefs_.upscaler_sharpness) || prefs_.upscaler_sharpness < 0.0 || prefs_.upscaler_sharpness > 1.0)
            prefs_.upscaler_sharpness = 0.5;
        const json &am = jobj(j, "aniskip_mal");
        if (am.is_object())
            for (auto it = am.begin(); it != am.end(); ++it)
            {
                // jnum needs an object; the value here is a bare number.
                const int id = it.value().is_number() ? static_cast<int>(it.value().get<double>()) : 0;
                if (id > 0 && prefs_.aniskip_mal.size() < 500)
                    prefs_.aniskip_mal[it.key()] = id;
            }
        const json &sc = jobj(j, "skipdb_cache");
        if (sc.is_object())
        {
            // TTLs: 30 days for positive entries, 7 for negative ones (an
            // empty segment list means the title definitively has no data).
            const double now = static_cast<double>(std::time(nullptr));
            for (auto it = sc.begin(); it != sc.end(); ++it)
            {
                const json &e = it.value();
                if (!e.is_object())
                    continue;
                const double at = jnum(e, "at", 0);
                if (at <= 0 || at > now)
                    continue;
                std::vector<aniskip::SkipSegment> segs;
                const json &arr = jobj(e, "segs");
                if (arr.is_array())
                    for (const auto &s : arr)
                    {
                        if (!s.is_object())
                            continue;
                        const double st = jnum(s, "s", -1), en = jnum(s, "e", -1);
                        const std::string k = jstr(s, "k");
                        if (!(st >= 0) || !(en > st) || !std::isfinite(st) || !std::isfinite(en))
                            continue;
                        if (k != "intro" && k != "op" && k != "recap" && k != "credits" && k != "ed" &&
                            k != "preview")
                            continue;
                        aniskip::SkipSegment sg;
                        sg.start = st;
                        sg.end = en;
                        sg.kind = k;
                        segs.push_back(sg);
                    }
                const double ttl = segs.empty() ? 86400.0 : 30 * 86400.0;
                if (now - at < ttl && prefs_.skipdb_cache.size() < 500)
                {
                    Prefs::SkipDbEntry entry;
                    entry.fetched_at = at;
                    entry.segs = std::move(segs);
                    prefs_.skipdb_cache[it.key()] = std::move(entry);
                }
            }
        }
        prefs_.ui_sounds = jbool(j, "ui_sounds", prefs_.ui_sounds);
        // ---- WORKER 10 (polish): haptics toggle ----
        prefs_.haptics = jbool(j, "haptics", prefs_.haptics);
        // ---- end WORKER 10 ----
        prefs_.auth_key = jstr(j, "auth_key");
        prefs_.user_email = jstr(j, "user_email");
        // Multi-account profiles.
        prefs_.profiles.clear();
        prefs_.active_profile = -1;
        const json &pr = jobj(j, "profiles");
        if (pr.is_array())
        {
            int idx = 0;
            for (const auto &v : pr)
            {
                if (!v.is_object() || idx >= Prefs::kMaxProfiles)
                    break;
                Prefs::Profile p;
                p.id = jstr(v, "id", "p" + std::to_string(idx + 1));
                p.name = jstr(v, "name");
                p.auth_key = jstr(v, "auth_key");
                p.user_email = jstr(v, "user_email");
                if (p.name.empty())
                    p.name = p.user_email.empty() ? ("Profile " + std::to_string(idx + 1)) : p.user_email;
                prefs_.profiles.push_back(std::move(p));
                ++idx;
            }
            prefs_.active_profile = static_cast<int>(jnum(j, "active_profile", 0));
            if (prefs_.active_profile < 0 || prefs_.active_profile >= static_cast<int>(prefs_.profiles.size()))
                prefs_.active_profile = prefs_.profiles.empty() ? -1 : 0;
        }
        if (prefs_.profiles.empty() && !prefs_.auth_key.empty())
        {
            // First run with profiles: migrate the single account into profile #1.
            Prefs::Profile p;
            p.id = "p1";
            p.auth_key = prefs_.auth_key;
            p.user_email = prefs_.user_email;
            p.name = p.user_email.empty() ? "Profile 1" : p.user_email;
            prefs_.profiles.push_back(std::move(p));
            prefs_.active_profile = 0;
        }
        profile_sync_active();
        const json &ex = jobj(j, "extra_addons");
        if (ex.is_array())
            for (const auto &v : ex)
                if (v.is_string())
                    prefs_.extra_addons.push_back(v.get<std::string>());
        const json &da = jobj(j, "disabled_addons");
        if (da.is_array())
            for (const auto &v : da)
                if (v.is_string())
                    prefs_.disabled_addons.insert(v.get<std::string>());
        const json &ao = jobj(j, "addon_order");
        if (ao.is_array())
            for (const auto &v : ao)
                if (v.is_string())
                    prefs_.addon_order.push_back(v.get<std::string>());
        const json &tm = jobj(j, "track_memory");
        if (tm.is_object())
            for (auto it = tm.begin(); it != tm.end(); ++it)
                if (it.value().is_object())
                {
                    Prefs::TrackMemory m;
                    m.audio_lang = jstr(it.value(), "a");
                    m.sub_lang = jstr(it.value(), "s");
                    m.sub_internal = jbool(it.value(), "si", false);
                    m.sub_off = jbool(it.value(), "so", false);
                    m.sub_delay = std::clamp(jnum(it.value(), "sd", 0.0), -60.0, 60.0);
                    prefs_.track_memory[it.key()] = m;
                }
        // ---- A11Y (worker 8): accessibility settings; owned by gl/a11y.cpp ----
        {
            A11y::Settings &a = A11y::get();
            a.reduce_motion = jbool(j, "a11y_reduce_motion", a.reduce_motion);
            a.ui_text_scale = static_cast<float>(jnum(j, "a11y_ui_text_scale", a.ui_text_scale));
            a.high_contrast = jbool(j, "a11y_high_contrast", a.high_contrast);
            a.sub_outline_min_px = static_cast<int>(jnum(j, "a11y_sub_outline_min_px", a.sub_outline_min_px));
            a.sub_bg_box = jbool(j, "a11y_sub_bg_box", a.sub_bg_box);
            a.sub_color_preset = static_cast<int>(jnum(j, "a11y_sub_color_preset", a.sub_color_preset));
            a.sub_size_scale = static_cast<float>(jnum(j, "a11y_sub_size_scale", a.sub_size_scale));
            a.focus_strength = static_cast<int>(jnum(j, "a11y_focus_strength", a.focus_strength));
            A11y::validate(a);
        }
        // ---- WORKER 7 (SETTINGS) prefs block ----
        {
            // Per-title audio delay: the "ad" key rides on the track_memory
            // entries loaded above (presence of the key means it was stored).
            const json &tm7 = jobj(j, "track_memory");
            if (tm7.is_object())
                for (auto it = tm7.begin(); it != tm7.end(); ++it)
                    if (it.value().is_object() && it.value().contains("ad"))
                    {
                        const auto mit = prefs_.track_memory.find(it.key());
                        if (mit == prefs_.track_memory.end())
                            continue;
                        mit->second.audio_delay = std::clamp(jnum(it.value(), "ad", 0.0), -0.5, 0.5);
                        mit->second.has_audio_delay = true;
                    }
            // "Recently changed" order, newest first; ids are validated
            // against the current row indices (stale ones are dropped).
            prefs_.recent_settings.clear();
            recent_order_.clear();
            const json &rs = jobj(j, "recent_settings");
            if (rs.is_array())
                for (const auto &v : rs)
                {
                    if (!v.is_number() || prefs_.recent_settings.size() >= 5)
                        break;
                    const int id = static_cast<int>(v.get<double>());
                    if (id <= kRecent4 || id >= kSettingCount)
                        continue;
                    prefs_.recent_settings.push_back(id);
                    recent_order_.push_back(id);
                }
        }
    }
    else if (err == "invalid JSON")
    {
        // The file exists but cannot be parsed: preserve it before falling
        // back to defaults, or the next save would destroy any chance of
        // recovering the sign-in and addons. ("not found" just means first run.)
        const std::string bak = path + ".corrupt";
        std::string data;
        if (read_file(path, data) && write_file(bak, data))
            dlog("settings: damaged %s preserved as %s", path.c_str(), bak.c_str());
        else
            dlog("settings: %s is damaged and could not be preserved", path.c_str());
        say("Your settings file was damaged and has been reset to defaults.", 6.0, true);
    }
}

void Session::save_prefs()
{
    json j;
    j["server_url"] = prefs_.server_url;
    j["builtin_torrents"] = prefs_.builtin_torrents;
    j["subtitle_languages"] = prefs_.subtitle_langs;
    j["auto_subtitles"] = prefs_.auto_subtitles;
    j["subtitle_size"] = prefs_.sub_size;
    j["audio_languages"] = prefs_.audio_langs;
    j["audio_output"] = prefs_.audio_output;
    j["volume_boost"] = prefs_.volume_boost;
    j["volume"] = prefs_.volume;
    j["night_mode"] = prefs_.night_mode;
    j["dolby_passthrough"] = prefs_.dolby_passthrough;
    j["autoplay_next"] = prefs_.autoplay_next;
    j["autoplay_best"] = prefs_.autoplay_best;
    j["binge_ask"] = prefs_.binge_ask;
    j["max_resolution"] = prefs_.max_resolution;
    j["os_user"] = prefs_.os_user;
    j["os_pass"] = prefs_.os_pass;
    // ---- SEARCH UX (worker 5): recent searches with timestamps ----
    {
        json rs = json::array();
        for (const auto &e : prefs_.recent_searches)
            rs.push_back({{"q", e.query}, {"t", e.at}});
        j["recent_searches"] = rs;
    }
    j["match_frame_rate"] = prefs_.match_frame_rate;
    j["hdr_probe"] = prefs_.hdr_probe;
    j["subtitle_color"] = prefs_.sub_color;
    j["subtitle_bg_opacity"] = prefs_.sub_bg_opacity;
    j["subtitle_pos"] = prefs_.sub_pos;
    j["aspect_mode"] = prefs_.aspect_mode;
    j["skip_mode"] = prefs_.skip_mode;
    j["disc_sort"] = prefs_.disc_sort;
    j["audio_delay"] = prefs_.audio_delay;
    j["upscaler"] = prefs_.upscaler;
    j["deep_blacks"] = prefs_.deep_blacks;
    j["tonemap_mode"] = prefs_.tonemap_mode;
    j["upscaler_sharpness"] = prefs_.upscaler_sharpness;
    {
        json am = json::object();
        std::size_t n = 0;
        for (const auto &kv : prefs_.aniskip_mal)
        {
            if (++n > 500)
                break; // bound the file size
            am[kv.first] = kv.second;
        }
        j["aniskip_mal"] = am;
    }
    {
        json sc = json::object();
        std::size_t n = 0;
        for (const auto &kv : prefs_.skipdb_cache)
        {
            if (++n > 500)
                break; // bound the file size
            json e;
            e["at"] = kv.second.fetched_at;
            json arr = json::array();
            for (const auto &s : kv.second.segs)
                arr.push_back({{"s", s.start}, {"e", s.end}, {"k", s.kind}});
            e["segs"] = arr;
            sc[kv.first] = e;
        }
        j["skipdb_cache"] = sc;
    }
    j["ui_sounds"] = prefs_.ui_sounds;
    // ---- WORKER 10 (polish): haptics toggle ----
    j["haptics"] = prefs_.haptics;
    // ---- end WORKER 10 ----
    j["extra_addons"] = prefs_.extra_addons;
    j["disabled_addons"] =
        std::vector<std::string>(prefs_.disabled_addons.begin(), prefs_.disabled_addons.end());
    j["addon_order"] = prefs_.addon_order;
    j["auth_key"] = prefs_.auth_key;
    j["user_email"] = prefs_.user_email;
    {
        json pa = json::array();
        for (const auto &p : prefs_.profiles)
        {
            json o = json::object();
            o["id"] = p.id;
            o["name"] = p.name;
            o["auth_key"] = p.auth_key;
            o["user_email"] = p.user_email;
            pa.push_back(std::move(o));
        }
        j["profiles"] = std::move(pa);
        j["active_profile"] = prefs_.active_profile;
    }
    {
        json tm = json::object();
        std::size_t n = 0;
        for (const auto &kv : prefs_.track_memory)
        {
            if (++n > 200)
                break; // bound the file size
            json m;
            m["a"] = kv.second.audio_lang;
            m["s"] = kv.second.sub_lang;
            m["si"] = kv.second.sub_internal;
            m["so"] = kv.second.sub_off;
            m["sd"] = kv.second.sub_delay;
            tm[kv.first] = m;
        }
        j["track_memory"] = tm;
    }
    // ---- A11Y (worker 8): accessibility settings; owned by gl/a11y.cpp ----
    {
        const A11y::Settings &a = A11y::get();
        j["a11y_reduce_motion"] = a.reduce_motion;
        j["a11y_ui_text_scale"] = a.ui_text_scale;
        j["a11y_high_contrast"] = a.high_contrast;
        j["a11y_sub_outline_min_px"] = a.sub_outline_min_px;
        j["a11y_sub_bg_box"] = a.sub_bg_box;
        j["a11y_sub_color_preset"] = a.sub_color_preset;
        j["a11y_sub_size_scale"] = a.sub_size_scale;
        j["a11y_focus_strength"] = a.focus_strength;
    }
    // ---- WORKER 7 (SETTINGS) prefs block ----
    {
        // Per-title audio delay, annotated onto the track_memory entries
        // written above (only entries that were actually stored).
        json &tm7 = j["track_memory"];
        if (tm7.is_object())
            for (auto it = tm7.begin(); it != tm7.end(); ++it)
            {
                const auto mit = prefs_.track_memory.find(it.key());
                if (mit != prefs_.track_memory.end() && mit->second.has_audio_delay)
                    it.value()["ad"] = mit->second.audio_delay;
            }
        // "Recently changed" order, newest first.
        prefs_.recent_settings.assign(recent_order_.begin(), recent_order_.end());
        json rs = json::array();
        for (int id : prefs_.recent_settings)
            rs.push_back(id);
        j["recent_settings"] = std::move(rs);
    }
    if (!save_json(data_dir_ + "/settings.json", j))
        say("Could not save settings to " + data_dir_, 5.0, true);
}

void Session::load_progress()
{
    json j;
    if (!load_json(data_dir_ + "/progress.json", j))
        return;
    const json &items = jobj(j, "items");
    if (items.is_object())
        for (auto it = items.begin(); it != items.end(); ++it)
        {
            Progress p;
            p.type = jstr(it.value(), "type");
            p.name = jstr(it.value(), "name");
            p.poster = jstr(it.value(), "poster");
            p.video_id = jstr(it.value(), "video_id");
            p.time = jnum(it.value(), "time");
            p.duration = jnum(it.value(), "duration");
            p.updated = static_cast<std::int64_t>(jnum(it.value(), "updated"));
            p.stream_json = jstr(it.value(), "stream");
            p.stream_video_id = jstr(it.value(), "stream_video");
            p.up_next = jstr(it.value(), "up_next");
            progress_[it.key()] = p;
        }
    const json &w = jobj(j, "watched");
    if (w.is_array())
        for (const auto &v : w)
            if (v.is_string())
                watched_.insert(v.get<std::string>());
}

// JSON for the progress file, from a snapshot. Pure: safe on any thread.
static json progress_to_json(const std::map<std::string, Progress> &progress, const std::set<std::string> &watched)
{
    json j;
    json items = json::object();
    for (const auto &kv : progress)
        items[kv.first] = json{{"type", kv.second.type},         {"name", kv.second.name},
                               {"poster", kv.second.poster},     {"video_id", kv.second.video_id},
                               {"time", kv.second.time},         {"duration", kv.second.duration},
                               {"updated", kv.second.updated},   {"stream", kv.second.stream_json},
                               {"stream_video", kv.second.stream_video_id}, {"up_next", kv.second.up_next}};
    j["items"] = items;
    j["watched"] = json(std::vector<std::string>(watched.begin(), watched.end()));
    return j;
}

void Session::prune_progress()
{
    static const std::size_t kMax = 200; // like track_memory: the save below must stay bounded
    while (progress_.size() > kMax)
    {
        // Evict the least-recently-updated entry (the map is keyed by id, not time).
        auto oldest = progress_.begin();
        for (auto it = progress_.begin(); it != progress_.end(); ++it)
            if (it->second.updated < oldest->second.updated)
                oldest = it;
        progress_.erase(oldest);
    }
}

void Session::save_progress()
{
    prune_progress();
    const std::string path = data_dir_ + "/progress.json";
    if (shutdown_)
    {
        // The worker pool is already stopped: write synchronously so the
        // final position is never lost. Takes save_m_ so an in-flight
        // background save can't interleave on the shared tmp path.
        std::lock_guard<std::mutex> lock(save_m_);
        if (!save_json(path, progress_to_json(progress_, watched_)))
            say("Could not save watch progress to " + data_dir_, 5.0, true);
        return;
    }
    // Snapshot on the UI thread (no worker touches progress_), then serialize
    // and write on a worker: the render thread never blocks on JSON or disk.
    // save_m_ keeps concurrent writers off the shared tmp path; `this`
    // outlives the pool (g_tasks.stop() joins workers before teardown).
    g_tasks.run([this, snap = progress_, watched = watched_, path]() {
        std::lock_guard<std::mutex> lock(save_m_);
        if (!save_json(path, progress_to_json(snap, watched)))
            dlog("progress: could not save to %s", path.c_str());
    });
}

// ---------------------------------------------------------------------------
// Addons and the library

const ::Addon *Session::find_addon(const std::string &url) const
{
    for (const auto &a : addons_)
        if (a->transport_url == url)
            return a.get();
    return nullptr;
}

bool Session::addon_disabled(const ::Addon &a) const
{
    return prefs_.disabled_addons.count(a.transport_url) > 0;
}

void Session::apply_addon_order()
{
    // The user's saved order wins; addons not in it (newly installed) keep
    // their load order at the end. Stable, so equal keys never swap.
    std::map<std::string, std::size_t> pos;
    for (std::size_t i = 0; i < prefs_.addon_order.size(); ++i)
        pos[prefs_.addon_order[i]] = i;
    const std::size_t last = prefs_.addon_order.size();
    std::stable_sort(addons_.begin(), addons_.end(), [&](const std::unique_ptr<::Addon> &a,
                                                          const std::unique_ptr<::Addon> &b) {
        const auto ia = pos.find(a->transport_url);
        const auto ib = pos.find(b->transport_url);
        const std::size_t pa = ia == pos.end() ? last : ia->second;
        const std::size_t pb = ib == pos.end() ? last : ib->second;
        return pa < pb;
    });
}

bool Session::addons_cache_load()
{
    json j;
    if (!load_json(data_dir_ + "/addons_cache.json", j) || !j.is_array())
        return false;
    addons_.clear();
    for (const auto &e : j)
    {
        auto a = std::make_unique<::Addon>();
        const std::string url = jstr(e, "transportUrl");
        if (!url.empty() && parse_addon(jobj(e, "manifest"), url, *a))
        {
            a->from_account = jbool(e, "fromAccount", false);
            addons_.push_back(std::move(a));
        }
    }
    if (addons_.empty())
        return false;
    apply_addon_order();
    return true;
}

void Session::addons_cache_save() const
{
    if (addons_.empty())
        return;
    json arr = json::array();
    for (const auto &a : addons_)
        arr.push_back({{"transportUrl", a->transport_url},
                       {"fromAccount", a->from_account},
                       {"manifest", a->manifest}});
    save_json(data_dir_ + "/addons_cache.json", arr);
}

void Session::load_addons()
{
    struct Result
    {
        std::vector<std::shared_ptr<::Addon>> addons;
        std::vector<std::string> errors;
        bool auth_failed = false;
    };
    const std::string key = prefs_.auth_key;
    const std::vector<std::string> extra = prefs_.extra_addons;
    const int gen = ++addons_gen_;
    addons_loading_ = true;
    bg<Result>(
        [key, extra]() {
            Result r;
            std::set<std::string> seen;
            if (!key.empty())
            {
                const ApiResult ar = api_addon_collection(key);
                if (ar.ok)
                {
                    const json &list = jobj(ar.result, "addons");
                    if (list.is_array())
                        for (const auto &e : list)
                        {
                            auto a = std::make_shared<::Addon>();
                            const std::string url = jstr(e, "transportUrl");
                            if (parse_addon(jobj(e, "manifest"), url, *a))
                            {
                                a->from_account = true;
                                if (seen.insert(url).second)
                                    r.addons.push_back(a);
                            }
                        }
                }
                else
                {
                    r.errors.push_back("Could not load your addons (" + ar.error + ")");
                    const std::string l = lower(ar.error);
                    if (l.find("session") != std::string::npos || l.find("auth") != std::string::npos)
                        r.auth_failed = true;
                }
            }
            std::vector<std::string> urls;
            if (r.addons.empty())
                urls = {kCinemetaUrl, kOpenSubtitlesUrl};
            for (const auto &u : extra)
                urls.push_back(u);
            for (const auto &u : urls)
            {
                if (seen.count(u))
                    continue;
                auto a = std::make_shared<::Addon>();
                std::string err;
                if (fetch_addon(u, *a, err))
                {
                    seen.insert(u);
                    r.addons.push_back(a);
                }
                else
                {
                    r.errors.push_back("Could not reach " + u + " (" + err + ")");
                }
            }
            return r;
        },
        [this, gen](Result &r) {
            if (gen != addons_gen_)
                return;
            addons_loading_ = false;
            if (r.addons.empty() && !addons_.empty())
            {
                // Offline: the fetch failed, but the disk cache restored in
                // init() still holds the last-known addons — keep them so
                // the board can build instead of going blank.
                dlog("addons: fetch failed, keeping %zu cached addons", addons_.size());
            }
            else
            {
                addons_.clear();
                for (const auto &a : r.addons)
                    addons_.push_back(std::make_unique<::Addon>(*a));
                apply_addon_order();
                addons_cache_save(); // refresh the disk cache for the next boot
            }
            // Only toast when there is nothing on screen to contradict it:
            // with snapshot rows up, a transient error toast is noise.
            if (!r.errors.empty() && board.rows.empty())
                say(r.errors.front(), 6.0, true);
            if (r.auth_failed)
                say("Your Stremio session expired. Sign in again in Settings.", 8.0, true);
            dlog("addons: %zu loaded", addons_.size());
            on_addons_loaded();
        });
}

void Session::on_addons_loaded()
{
    // Stale-while-revalidate: keep the rows on screen (the boot snapshot, or
    // the previous live ones on a manual reload) so the board never flashes
    // to empty while the catalogs re-answer. build_board() clears board_, so
    // the stashed rows are re-attached to the matching fresh rows below.
    std::vector<BoardRow> stale;
    stale.reserve(board_.size());
    for (const BoardRow &r : board_)
        if (r.key != "continue" && r.loaded && !r.items.empty())
            stale.push_back(r);
    build_board();
    if (board_.size() > 1 && !stale.empty())
    {
        for (std::size_t i = 1; i < board_.size(); ++i)
        {
            if (board_[i].loaded)
                continue; // answered between build_board's refresh and here
            for (const BoardRow &s : stale)
            {
                if (s.key == board_[i].key)
                {
                    board_[i].items = s.items;
                    board_[i].loaded = true;
                    // Another worker's fields (gl/model.hpp): this row is a
                    // stale snapshot row until its catalog re-answers.
                    board_[i].from_snapshot = true;
                    board_[i].validating = true;
                    break;
                }
            }
        }
        refresh_board(); // repaint with the pre-filled rows
        // Fresh rows are on their way: the snapshot is no longer the story.
        board_snapshot_note_.clear();
    }
    else if (board_.size() <= 1 && !stale.empty())
    {
        // No catalog rows at all (offline?): keep showing the stashed ones.
        // The note stays: these rows are still the stale snapshot.
        for (const BoardRow &s : stale)
            board_.push_back(s);
        refresh_board();
    }
    else
    {
        board_snapshot_note_.clear();
    }
    // Discover starts again from the new catalogs the next time it opens.
    disc_items_.clear();
    discover.items.clear();
    disc_types_.clear();
    discover.status.clear();
    changed_ |= kDiscover | kDiscoverNew;
    refresh_addons();
    refresh_settings();
    if (page_ == Page::discover)
        enter_discover();
}

void Session::load_library()
{
    const std::string key = prefs_.auth_key;
    if (key.empty())
        return;
    bg<ApiResult>([key]() { return api_library_get(key); },
                  [this](ApiResult &r) {
                      if (!r.ok)
                      {
                          // Only toast when the Board is empty: with snapshot
                          // rows on screen the failure is invisible anyway.
                          if (board.rows.empty())
                              say("Could not load your library: " + r.error, 5.0, true);
                          return;
                      }
                      library_.clear();
                      if (r.result.is_array())
                          for (const auto &it : r.result)
                          {
                              const std::string id = jstr(it, "_id");
                              if (!id.empty())
                                  library_[id] = it;
                          }
                      library_loaded_ = true;
                      dlog("library: %zu items", library_.size());
                      refresh_board();
                      refresh_library();
                  });
}

void Session::addons_reload()
{
    note_input(); // user action: reset the screensaver idle timer
    say("Reloading addons...");
    load_addons();
    if (signed_in())
        load_library();
}

void Session::addon_activate(int index)
{
    note_input(); // user action: reset the screensaver idle timer
    if (index < 0 || index >= static_cast<int>(addons_.size()))
        return;
    const ::Addon &a = *addons_[static_cast<std::size_t>(index)];
    const std::string url = a.transport_url;
    const std::string name = a.name;
    const auto it = std::find(prefs_.extra_addons.begin(), prefs_.extra_addons.end(), url);
    const bool is_local = it != prefs_.extra_addons.end();
    const bool dis = addon_disabled(a);
    std::vector<std::string> opts;
    opts.push_back(dis ? "Enable" : "Disable");
    opts.push_back("Move up");
    opts.push_back("Move down");
    if (is_local)
        opts.push_back("Uninstall");
    else
        opts.push_back("About removing");
    opts.push_back("Cancel");
    const int cancel_at = static_cast<int>(opts.size()) - 1;
    open_dropdown(name, opts, cancel_at, [this, url, name, is_local, opts](int i) {
        if (i < 0 || i >= static_cast<int>(opts.size()))
            return;
        const std::string &sel = opts[static_cast<std::size_t>(i)];
        if (sel == "Cancel")
            return;
        if (sel == "Enable" || sel == "Disable")
        {
            const bool disable = sel == "Disable";
            if (disable)
                prefs_.disabled_addons.insert(url);
            else
                prefs_.disabled_addons.erase(url);
            save_prefs();
            say(disable ? ("\"" + name + "\" disabled.") : ("\"" + name + "\" enabled."));
            on_addons_loaded(); // rebuild board/discover without refetching manifests
            return;
        }
        if (sel == "Move up" || sel == "Move down")
        {
            const int dir = sel == "Move up" ? -1 : 1;
            int at = -1;
            for (int k = 0; k < static_cast<int>(addons_.size()); ++k)
                if (addons_[static_cast<std::size_t>(k)]->transport_url == url)
                    at = k;
            const int to = at + dir;
            if (at < 0 || to < 0 || to >= static_cast<int>(addons_.size()))
                return;
            std::swap(addons_[static_cast<std::size_t>(at)], addons_[static_cast<std::size_t>(to)]);
            prefs_.addon_order.clear();
            for (const auto &x : addons_)
                prefs_.addon_order.push_back(x->transport_url);
            save_prefs();
            on_addons_loaded();
            return;
        }
        if (sel == "About removing")
        {
            say("Install and remove addons in the Stremio app on your phone or computer; they sync here. "
                "Press Square to reload them now.",
                7.0);
            return;
        }
        if (sel == "Uninstall" && is_local)
        {
            // Nested two-press confirm; the outer dropdown is already closed.
            open_dropdown("Remove \"" + name + "\"?", {"Remove", "Cancel"}, 1, [this, url, name](int j) {
                if (j != 0)
                    return;
                auto &ex = prefs_.extra_addons;
                ex.erase(std::remove(ex.begin(), ex.end(), url), ex.end());
                prefs_.disabled_addons.erase(url);
                prefs_.addon_order.erase(
                    std::remove(prefs_.addon_order.begin(), prefs_.addon_order.end(), url),
                    prefs_.addon_order.end());
                save_prefs();
                say("\"" + name + "\" removed.");
                load_addons();
            });
            return;
        }
    });
}

// ---------------------------------------------------------------------------
// Addons and settings screens

void Session::refresh_addons()
{
    addons.items.clear();
    addon_logos_.clear();
    for (const auto &a : addons_)
    {
        sx::Addon row;
        row.name = a->name;
        row.version = a->version.empty() ? "" : "v" + a->version;
        row.description = a->description;
        std::vector<std::string> types;
        for (const auto &t : a->types)
            types.push_back(t);
        row.types = join(types, " ");
        row.local = !a->from_account;
        row.disabled = addon_disabled(*a);
        row.initial = first_letter(a->name);
        row.colour = colour_for(a->name);
        addons.items.push_back(row);
        addon_logos_.push_back(jstr(a->manifest, "logo"));
    }
    changed_ |= kAddons;
    art_dirty_ = true;
}

// The cache the Clear-cache row reports and deletes: the artwork files in
// art/ (re-downloaded on demand), the rotating stream buffers, the torrent
// caches (up to ~6 GB), and the log. settings.json / progress.json are never
// touched.
static bool is_torrent_cache(const std::string &name)
{
    return name.compare(0, 14, "torrent-cache-") == 0 && name.size() > 18 &&
           name.compare(name.size() - 4, 4, ".bin") == 0;
}

std::uint64_t Session::cache_size() const
{
    std::uint64_t total = 0;
    const std::string art = data_dir_ + "/art";
    if (DIR *d = opendir(art.c_str()))
    {
        while (dirent *e = readdir(d))
        {
            if (e->d_name[0] == '.')
                continue;
            struct stat st;
            const std::string p = art + "/" + e->d_name;
            if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                total += static_cast<std::uint64_t>(st.st_size);
        }
        closedir(d);
    }
    if (DIR *d = opendir(data_dir_.c_str()))
    {
        while (dirent *e = readdir(d))
        {
            if (!is_torrent_cache(e->d_name))
                continue;
            struct stat st;
            const std::string p = data_dir_ + "/" + e->d_name;
            if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                total += static_cast<std::uint64_t>(st.st_size);
        }
        closedir(d);
    }
    for (int i = 0; i < 16; ++i)
    {
        struct stat st;
        if (stat((data_dir_ + "/stream-cache-" + std::to_string(i) + ".bin").c_str(), &st) == 0 &&
            S_ISREG(st.st_mode))
            total += static_cast<std::uint64_t>(st.st_size);
    }
    {
        struct stat st;
        if (stat((data_dir_ + "/log.txt").c_str(), &st) == 0 && S_ISREG(st.st_mode))
            total += static_cast<std::uint64_t>(st.st_size);
    }
    return total;
}

void Session::refresh_cache_size_bg()
{
    // A newer walk (e.g. after Clear-cache) discards an older in-flight
    // result, so a stale pre-clear size can never win the race.
    const std::uint64_t seq = ++cache_walk_seq_;
    g_tasks.run([this, seq] {
        const std::uint64_t sz = cache_size();
        if (seq == cache_walk_seq_.load())
        {
            cache_size_cached_.store(sz);
            cache_size_updated_.store(true);
        }
    });
}

void Session::clear_cache()
{
    std::uint64_t freed = 0;
    const std::string art = data_dir_ + "/art";
    if (DIR *d = opendir(art.c_str()))
    {
        while (dirent *e = readdir(d))
        {
            if (e->d_name[0] == '.')
                continue;
            const std::string p = art + "/" + e->d_name;
            struct stat st;
            if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            freed += static_cast<std::uint64_t>(st.st_size);
            if (unlink(p.c_str()) != 0)
                dlog("settings: clear cache: could not delete %s", p.c_str());
        }
        closedir(d);
    }
    for (int i = 0; i < 16; ++i)
    {
        const std::string p = data_dir_ + "/stream-cache-" + std::to_string(i) + ".bin";
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            freed += static_cast<std::uint64_t>(st.st_size);
        unlink(p.c_str()); // missing files are fine
    }
    // The torrent caches: up to ~6 GB, and the single largest cache in the
    // app. Unlinking a file another (stopped) torrent still has open is safe:
    // its FILE* keeps the inode until fclose.
    if (DIR *d = opendir(data_dir_.c_str()))
    {
        while (dirent *e = readdir(d))
        {
            if (!is_torrent_cache(e->d_name))
                continue;
            const std::string p = data_dir_ + "/" + e->d_name;
            struct stat st;
            if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            freed += static_cast<std::uint64_t>(st.st_size);
            if (unlink(p.c_str()) != 0)
                dlog("settings: clear cache: could not delete %s", p.c_str());
        }
        closedir(d);
    }
    {
        const std::string p = data_dir_ + "/torrent-cache.bin"; // legacy leftover
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            freed += static_cast<std::uint64_t>(st.st_size);
        unlink(p.c_str()); // missing files are fine
    }
    {
        const std::string p = data_dir_ + "/log.txt";
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            freed += static_cast<std::uint64_t>(st.st_size);
        if (FILE *f = std::fopen(p.c_str(), "w"))
            std::fclose(f);
    }
    dlog("settings: cache cleared, freed %llu bytes", static_cast<unsigned long long>(freed));
    // Also drop the in-memory skip caches: a poisoned negative entry or a
    // wrong MAL mapping is only recoverable by clearing them.
    prefs_.skipdb_cache.clear();
    prefs_.aniskip_mal.clear();
    save_prefs();
    refresh_cache_size_bg();
}

void Session::refresh_settings()
{
    settings.account = prefs_.user_email;
    settings.rows.assign(kSettingCount, SettingRow());
    const auto set = [&](int i, SettingRow::Kind kind, const char *label, const std::string &value, const std::string &help,
                         bool on = false) {
        SettingRow &r = settings.rows[static_cast<std::size_t>(i)];
        r.kind = kind;
        r.label = label;
        r.value = value;
        r.help = help;
        r.on = on;
    };
    using Kind = SettingRow::Kind;
    const auto head = [&](int i, const char *label) {
        SettingRow &r = settings.rows[static_cast<std::size_t>(i)];
        r.kind = Kind::header;
        r.label = label;
    };
    head(kHdrAccount, "Account");
    {
        // Multi-account: the row shows the active profile; Cross opens the
        // account dropdown (switch / add / remove / sign out).
        const int n = static_cast<int>(prefs_.profiles.size());
        std::string value, help;
        if (signed_in())
        {
            value = prefs_.user_email.empty() ? profile_name(prefs_.active_profile) : prefs_.user_email;
            if (n > 1)
                value += " (" + std::to_string(n) + " accounts)";
            help = "Press Cross to switch or manage accounts.";
        }
        else if (n > 0)
        {
            value = profile_name(prefs_.active_profile) + " (signed out)";
            help = "Press Cross to sign in or manage accounts.";
        }
        else
        {
            value = "Not signed in";
            help = "Sign in to sync your addons, library and Continue Watching. Press Cross.";
        }
        set(kAccount, Kind::value, "Stremio account", value, help);
    }
    head(kHdrPlayback, "Playback");
    set(kTorrents, Kind::value, "Play torrents with", prefs_.builtin_torrents ? "This app" : "Streaming server",
        torrents_desc(prefs_.builtin_torrents));
    set(kServer, Kind::value, "Streaming server", prefs_.server_url.empty() ? "Not set" : prefs_.server_url,
        "Optional: for server transcoding. Your Stremio server, for example http://192.168.1.20:11470.");
    set(kAutoplay, Kind::toggle, "Play next episode automatically", "", "Start the next episode when one ends.",
        prefs_.autoplay_next);
    set(kAutoplayBest, Kind::toggle, "Auto-play best stream", "",
        "Play the best stream automatically instead of showing the stream list.", prefs_.autoplay_best);
    set(kBingeAsk, Kind::value, "Ask when binge-watching", binge_label(prefs_.binge_ask),
        std::string(binge_desc(prefs_.binge_ask)) +
            " Only applies when 'Play next episode automatically' is on.");
    set(kMatchFrameRate, Kind::toggle, "Match frame rate (119.88 Hz)", "",
        "Experimental: switch the TV to 119.88 Hz when playing ~24fps films, so motion is judder-free "
        "(5:5 pulldown). Only works if your TV accepts it; harmless otherwise.",
        prefs_.match_frame_rate);
    set(kHdrProbe, Kind::toggle, "HDR10 diagnostic probe", "",
        "Diagnostic only: at the next startup, query the HDMI output state (read-only) and log whether the TV "
        "reports HDR. Never changes the picture. Off by default.",
        prefs_.hdr_probe);
    set(kAspectMode, Kind::value, "Zoom", prefs_.aspect_mode == "fill" ? "Fill" : "Fit",
        aspect_desc(prefs_.aspect_mode));
    set(kSkipMode, Kind::value, "Skip intros/outros", skipmode_label(prefs_.skip_mode),
        skip_desc(prefs_.skip_mode));
    set(kMaxResolution, Kind::value, "Max resolution", maxres_label(prefs_.max_resolution),
        maxres_desc(prefs_.max_resolution));
    set(kUpscaler, Kind::value, "Upscaler", upscale_label(prefs_.upscaler),
        upscale_desc(prefs_.upscaler));
    if (prefs_.upscaler >= 2)
        set(kUpscalerSharpness, Kind::value, "Sharpening strength",
            std::to_string(static_cast<int>(prefs_.upscaler_sharpness * 100)) + "%",
            sharpness_desc(prefs_.upscaler, prefs_.upscaler_sharpness));
    set(kDeepBlacks, Kind::value, "Deep blacks",
        prefs_.deep_blacks == 2 ? "High" : prefs_.deep_blacks == 1 ? "Low" : "Off",
        deepblacks_desc(prefs_.deep_blacks));
    set(kTonemapMode, Kind::value, "HDR tone mapping",
        prefs_.tonemap_mode == 1 ? "BT.2390 (Broadcast)" : "Hable (Filmic)",
        tonemap_desc(prefs_.tonemap_mode));
    head(kHdrSubtitles, "Subtitles");
    set(kSubStyle, Kind::value, "Subtitle style", kSubStyleNames[sub_style_current()],
        substyle_desc(sub_style_current()));
    set(kSubLangs, Kind::value, "Subtitle languages", prefs_.subtitle_langs.empty() ? "None" : prefs_.subtitle_langs,
        "Preferred order, for example \"gre, eng\".");
    set(kAutoSubs, Kind::toggle, "Subtitles on automatically", "", "Turn subtitles on automatically in your preferred language.",
        prefs_.auto_subtitles);
    set(kSubSize, Kind::value, "Subtitle size", size_label(prefs_.sub_size), "How large subtitles are drawn over the video.");
    set(kSubColor, Kind::value, "Subtitle color", subcolor_label(prefs_.sub_color), "The color of subtitle text.");
    set(kSubBg, Kind::slider, "Subtitle background", std::to_string(prefs_.sub_bg_opacity) + "%",
        "How dark the box behind subtitles is. Left/Right to adjust.");
    settings.rows[static_cast<std::size_t>(kSubBg)].slider_pct = prefs_.sub_bg_opacity;
    set(kSubPos, Kind::value, "Subtitle position", subpos_label(prefs_.sub_pos),
        "Raise subtitles above the bottom edge of the screen.");
    set(kOsUser, Kind::value, "OpenSubtitles username", prefs_.os_user.empty() ? "Not set" : prefs_.os_user,
        "Your OpenSubtitles.com account, for subtitle addons that support sign-in. Stored only on this console. Type - to clear.");
    set(kOsPass, Kind::value, "OpenSubtitles password", prefs_.os_pass.empty() ? "Not set" : "Set",
        "Your OpenSubtitles.com password. Stored only on this console, never synced. Type - to clear.");
    head(kHdrAudio, "Audio");
    set(kAudioLangs, Kind::value, "Audio languages", prefs_.audio_langs.empty() ? "Default" : prefs_.audio_langs,
        "Preferred audio track language, for example \"eng\". Leave empty for the default.");
    set(kAudioOutput, Kind::value, "Audio output", audioout_label(prefs_.audio_output),
        audioout_desc(prefs_.audio_output));
    set(kVolumeBoost, Kind::value, "Volume boost", volboost_label(prefs_.volume_boost),
        volboost_desc(prefs_.volume_boost));
    set(kNightMode, Kind::toggle, "Night mode", "", "Tame loud peaks and lift quiet dialogue for late-night watching.", prefs_.night_mode);
    set(kDolbyPassthrough, Kind::toggle, "HDMI audio passthrough", "", "Bitstream Dolby Digital (AC-3), Dolby Digital Plus / Atmos (E-AC-3), DTS, and AAC directly to your receiver or soundbar over HDMI. Only enable if your audio system decodes these formats.", prefs_.dolby_passthrough);
    head(kHdrGeneral, "General");
    set(kUiSounds, Kind::toggle, "Interface sounds", "", "A soft sound when you move around, go back or switch page.", prefs_.ui_sounds);
    set(kHaptics, Kind::toggle, "Controller vibration", "", "A light tick when you move, a pulse when you confirm.", prefs_.haptics);
    set(kAddAddon, Kind::value, "Add addon by URL", "",
        prefs_.extra_addons.empty() ? "An addon's manifest URL. Remove added addons from the Addons page."
                                    : std::to_string(prefs_.extra_addons.size()) + " added here.");
    set(kReload, Kind::action, "Reload addons and catalogs", "", "Fetch every addon and catalog again.");
    set(kExit, Kind::action, "Exit Stremio", "", "Close the app and go back to the home screen.");
    head(kHdrStorage, "Storage");
    set(kClearCache, Kind::action, "Clear cache",
        confirm_clear_cache_ ? "Press again to confirm" : format_bytes(cache_size_cached_.load()),
        "Delete downloaded artwork, stream and torrent buffers, and the log. Your sign-in, settings and Continue Watching are "
        "kept. Safe to use any time, even during playback.");
    set(kRecordLog, Kind::action, "Record diagnostic log",
        log_record_active() ? "Recording — press to stop" : "Off",
        "Capture everything the app logs into a timestamped file in /data/stremio "
        "(visible in PS5Upload). Start it before reproducing a bug, stop it after — "
        "the full path shows on screen so you can find the file.");
    // WORKER 7: inline stepping. Value rows with <= 4 options cycle directly
    // on Left/Right instead of opening a choice list; the shell checks
    // step_inline before intercepting the D-pad.
    {
        const int steppables[] = {kTorrents, kAspectMode, kSkipMode, kUpscaler, kUpscalerSharpness, kDeepBlacks,
                                  kTonemapMode, kSubStyle, kSubSize, kSubColor, kSubPos,
                                  kAudioOutput, kVolumeBoost, kBingeAsk};
        for (int id : steppables)
        {
            SettingRow &r = settings.rows[static_cast<std::size_t>(id)];
            r.step_inline = true;
            if (r.help.find("Left/Right") == std::string::npos)
                r.help += " Left/Right changes it directly.";
        }
    }
    // WORKER 7: "Recently changed". The visible Recent slots (kRecent0..kRecent4)
    // were removed (Task 4); recent_order_ still tracks last-touched ids.
    // Clear the mapping each refresh so Settings always starts cleanly at
    // Account/Playback: rows.assign() leaves the kHdrRecent header and slots
    // as empty-label value rows, which the list skips (never focused, never
    // drawn).
    recent_shown_.clear();
    // cross-worker: accessibility section
    a11y_build_rows();
    changed_ |= kSettings;
}

void Session::settings_activate(int row)
{
    note_input(); // user action: reset the screensaver idle timer
    // WORKER 7: the "Recently changed" slots activate their source rows.
    if (row >= kRecent0 && row <= kRecent4)
    {
        const std::size_t slot = static_cast<std::size_t>(row - kRecent0);
        if (slot < recent_shown_.size())
            row = recent_shown_[slot];
        else
            return; // an empty slot is never focused, but stay safe
    }
    if (row >= 0 && static_cast<std::size_t>(row) < settings.rows.size() &&
        settings.rows[static_cast<std::size_t>(row)].kind == SettingRow::Kind::header)
        return; // section labels are not activatable
    // cross-worker: accessibility rows appended by WORKER 8 (gl/a11y_settings.cpp)
    if (a11y_activate(row))
        return;
    if (row != kAccount && confirm_sign_out_)
        confirm_sign_out_ = false;
    if (row != kClearCache && confirm_clear_cache_)
        confirm_clear_cache_ = false;
    switch (row)
    {
    case kAccount:
        if (!signed_in() && prefs_.profiles.empty())
            login_start();
        else
            profile_dropdown(); // switch / add / remove / sign out
        break;
    case kTorrents:
        prefs_.builtin_torrents = !prefs_.builtin_torrents;
        settings_touch(kTorrents);
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kServer:
        ask_text([this](const std::string &v) {
            // The keyboard starts empty: typing nothing keeps the address.
            std::string u = trim(v);
            if (u.empty())
                return;
            if (u.find("://") == std::string::npos)
                u = "http://" + u;
            prefs_.server_url = u;
            settings_touch(kServer);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        });
        break;
    case kSubStyle:
    {
        // One preset sets size + color + background + position together; the
        // individual rows below stay for fine-tuning.
        open_dropdown("Subtitle style", {kSubStyleNames[0], kSubStyleNames[1], kSubStyleNames[2]},
                      sub_style_current(), [this](int i) {
                          settings_apply_sub_style(i);
                          settings_touch(kSubStyle);
                          save_prefs();
                          cue(hui::audio::Cue::saved);
                          refresh_settings();
                      },
                      {substyle_desc(0), substyle_desc(1), substyle_desc(2)});
        break;
    }
    case kSubLangs:
        ask_text([this](const std::string &v) {
            if (trim(v).empty())
                return;
            prefs_.subtitle_langs = trim(v);
            settings_touch(kSubLangs);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        });
        break;
    case kAudioLangs:
        ask_text([this](const std::string &v) {
            if (trim(v).empty())
                return;
            prefs_.audio_langs = trim(v) == "-" ? "" : trim(v);
            settings_touch(kAudioLangs);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        });
        break;
    case kAudioOutput:
    {
        open_dropdown("Audio output", {"Stereo", "Surround (5.1/7.1 PCM)"}, prefs_.audio_output == 1 ? 1 : 0, [this](int i) {
            prefs_.audio_output = i;
            settings_touch(kAudioOutput);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        },
                      {audioout_desc(0), audioout_desc(1)});
        break;
    }
    case kVolumeBoost:
    {
        int cur = 0;
        for (int i = 0; i < 4; ++i)
            if (prefs_.volume_boost == kVolumeBoosts[i])
                cur = i;
        open_dropdown("Volume boost", {"100%", "125%", "150%", "200%"}, cur, [this](int i) {
            prefs_.volume_boost = kVolumeBoosts[i];
            settings_touch(kVolumeBoost);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        },
                      {volboost_desc(100), volboost_desc(125), volboost_desc(150), volboost_desc(200)});
        break;
    }
    case kNightMode:
        prefs_.night_mode = !prefs_.night_mode;
        settings_touch(kNightMode);
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kDolbyPassthrough:
        prefs_.dolby_passthrough = !prefs_.dolby_passthrough;
        settings_touch(kDolbyPassthrough);
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kAddAddon:
        ask_text([this](const std::string &v) {
            const std::string url = normalize_addon_url(v);
            if (trim(v).empty() || trim(v) == "https://")
                return;
            say("Loading addon...");
            bg<std::string>(
                [url]() {
                    ::Addon a;
                    std::string err;
                    if (!fetch_addon(url, a, err))
                        return "Could not add the addon: " + err;
                    return std::string();
                },
                [this, url](std::string &err) {
                    if (!err.empty())
                    {
                        say(err, 6.0, true);
                        return;
                    }
                    if (std::find(prefs_.extra_addons.begin(), prefs_.extra_addons.end(), url) == prefs_.extra_addons.end())
                        prefs_.extra_addons.push_back(url);
                    settings_touch(kAddAddon);
                    save_prefs();
                    say("Addon added");
                    load_addons();
                    refresh_settings();
                });
        });
        break;
    case kAutoSubs:
        prefs_.auto_subtitles = !prefs_.auto_subtitles;
        settings_touch(kAutoSubs);
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kSubSize:
    {
        const int cur = prefs_.sub_size == "sub-s" ? 0 : prefs_.sub_size == "sub-l" ? 2 : 1;
        open_dropdown("Subtitle size", {"Small", "Medium", "Large"}, cur, [this](int i) {
            prefs_.sub_size = kSubSizes[i];
            settings_touch(kSubSize);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        },
                      {"Small: compact subtitle text that stays out of the way of the picture.",
                       "Medium: balanced default for couch viewing.",
                       "Large: easy-to-read dialogue from across the room."});
        break;
    }
    case kAspectMode:
    {
        const int cur = prefs_.aspect_mode == "fill" ? 1 : 0;
        open_dropdown("Zoom", {"Fit", "Fill"}, cur, [this](int i) {
            prefs_.aspect_mode = i == 1 ? "fill" : "fit";
            settings_touch(kAspectMode);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        },
                      {aspect_desc("fit"), aspect_desc("fill")});
        break;
    }
    case kSkipMode:
    {
        open_dropdown("Skip intros/outros", {"Off", "Show a Skip button", "Auto-skip"}, prefs_.skip_mode,
                      [this](int i) {
                          prefs_.skip_mode = i;
                          settings_touch(kSkipMode);
                          save_prefs();
                          cue(hui::audio::Cue::saved);
                          refresh_settings();
                      },
                      {skip_desc(0), skip_desc(1), skip_desc(2)});
        break;
    }
    case kMaxResolution:
    {
        static const char *vals[] = {"auto", "4k", "1080p", "720p", "480p"};
        int cur = 0;
        for (int i = 0; i < 5; ++i)
            if (vals[i] == prefs_.max_resolution)
                cur = i;
        open_dropdown("Max resolution", {"Auto", "4K", "1080p", "720p", "480p"}, cur, [this](int i) {
            prefs_.max_resolution = vals[i];
            settings_touch(kMaxResolution);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        },
                      {maxres_desc("auto"), maxres_desc("4k"), maxres_desc("1080p"), maxres_desc("720p"),
                       maxres_desc("480p")});
        break;
    }
    case kUpscaler:
    {
        open_dropdown("Upscaler", {"Off", "FSR1", "FSR1 + Sharpening", "Anime4K", "Cinema HQ (Lanczos + CAS)"}, prefs_.upscaler,
                      [this](int i) {
                          prefs_.upscaler = i;
                          settings_touch(kUpscaler);
                          save_prefs();
                          cue(hui::audio::Cue::saved);
                          refresh_settings();
                      },
                      {upscale_desc(0), upscale_desc(1), upscale_desc(2), upscale_desc(3), upscale_desc(4)});
        break;
    }
    case kUpscalerSharpness:
    {
        if (prefs_.upscaler < 2)
            break; // hidden row: not selectable, not activatable
        // 0%, 25%, 50%, 75%, 100%
        static const double vals[] = {0.0, 0.25, 0.5, 0.75, 1.0};
        int cur = 2;
        for (int i = 0; i < 5; ++i)
            if (std::abs(vals[i] - prefs_.upscaler_sharpness) < 0.01)
                cur = i;
        open_dropdown("Sharpening strength", {"0%", "25%", "50%", "75%", "100%"}, cur, [this](int i) {
            prefs_.upscaler_sharpness = vals[i];
            settings_touch(kUpscalerSharpness);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        },
                      {sharpness_desc(prefs_.upscaler, 0.0), sharpness_desc(prefs_.upscaler, 0.25),
                       sharpness_desc(prefs_.upscaler, 0.5), sharpness_desc(prefs_.upscaler, 0.75),
                       sharpness_desc(prefs_.upscaler, 1.0)});
        break;
    }
    case kDeepBlacks:
    {
        open_dropdown("Deep blacks", {"Off", "Low", "High"}, prefs_.deep_blacks,
                      [this](int i) {
                          prefs_.deep_blacks = i;
                          settings_touch(kDeepBlacks);
                          save_prefs();
                          cue(hui::audio::Cue::saved);
                          refresh_settings();
                      },
                      {deepblacks_desc(0), deepblacks_desc(1), deepblacks_desc(2)});
        break;
    }
    case kTonemapMode:
    {
        open_dropdown("HDR tone mapping", {"Hable (Filmic)", "BT.2390 (Broadcast)"},
                      prefs_.tonemap_mode, [this](int i) {
                          prefs_.tonemap_mode = i;
                          settings_touch(kTonemapMode);
                          save_prefs();
                          cue(hui::audio::Cue::saved);
                          refresh_settings();
                      },
                      {tonemap_desc(0), tonemap_desc(1)});
        break;
    }
    case kSubColor:
    {
        const int cur = prefs_.sub_color == "yellow" ? 1 : 0;
        open_dropdown("Subtitle color", {"White", "Yellow"}, cur, [this](int i) {
            prefs_.sub_color = kSubColors[i];
            settings_touch(kSubColor);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        },
                      {"Clean white text with a dark outline.",
                       "Classic cinema yellow — high contrast against bright snow or daylight scenes."});
        break;
    }
    case kSubPos:
    {
        const int cur = prefs_.sub_pos == "raised" ? 1 : 0;
        open_dropdown("Subtitle position", {"Bottom", "Raised"}, cur, [this](int i) {
            prefs_.sub_pos = kSubPositions[i];
            settings_touch(kSubPos);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        },
                      {"Standard position near the bottom edge.",
                       "Lifted 120px higher so subtitles sit inside the frame on ultrawide movies."});
        break;
    }
    case kOsUser:
        ask_text([this](const std::string &v) {
            // The keyboard starts empty: typing nothing keeps the saved value.
            // A single "-" clears it.
            if (v == "-")
            {
                prefs_.os_user.clear();
            }
            else
            {
                const std::string u = trim(v);
                if (u.empty())
                    return;
                prefs_.os_user = u;
            }
            settings_touch(kOsUser);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        });
        break;
    case kOsPass:
        ask_text([this](const std::string &v) {
            if (v == "-")
            {
                prefs_.os_pass.clear();
            }
            else
            {
                const std::string u = trim(v);
                if (u.empty())
                    return;
                prefs_.os_pass = u;
            }
            settings_touch(kOsPass);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        });
        break;
    case kAutoplay:
        prefs_.autoplay_next = !prefs_.autoplay_next;
        settings_touch(kAutoplay);
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kAutoplayBest:
        prefs_.autoplay_best = !prefs_.autoplay_best;
        settings_touch(kAutoplayBest);
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kBingeAsk:
    {
        int cur = 1;
        for (int i = 0; i < 4; ++i)
            if (kBingeAsks[i] == prefs_.binge_ask)
                cur = i;
        open_dropdown("Ask when binge-watching", {"Off", "After 3 episodes", "After 5 episodes", "After 10 episodes"},
                      cur, [this](int i) {
                          prefs_.binge_ask = kBingeAsks[i];
                          settings_touch(kBingeAsk);
                          save_prefs();
                          cue(hui::audio::Cue::saved);
                          refresh_settings();
                      },
                      {binge_desc(0), binge_desc(3), binge_desc(5), binge_desc(10)});
        break;
    }
    case kMatchFrameRate:
        prefs_.match_frame_rate = !prefs_.match_frame_rate;
        settings_touch(kMatchFrameRate);
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kHdrProbe:
        prefs_.hdr_probe = !prefs_.hdr_probe;
        settings_touch(kHdrProbe);
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kUiSounds:
        prefs_.ui_sounds = !prefs_.ui_sounds;
        settings_touch(kUiSounds);
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kHaptics:
        prefs_.haptics = !prefs_.haptics;
        settings_touch(kHaptics);
        save_prefs();
        cue(hui::audio::Cue::toggle);
        break;
    case kReload:
        addons_reload();
        break;
    case kClearCache:
        if (!confirm_clear_cache_)
        {
            confirm_clear_cache_ = true;
            cue(hui::audio::Cue::toggle);
        }
        else
        {
            confirm_clear_cache_ = false;
            clear_cache();
            say("Cache cleared");
        }
        break;
    case kRecordLog:
        if (log_record_active())
        {
            std::string path = log_record_stop();
            dlog("diagnostic recording stopped: %s", path.c_str());
            say(path.empty() ? "Recording stopped" : "Saved: " + path);
        }
        else
        {
            std::string path = log_record_start(data_dir_);
            if (path.empty())
                say("Couldn't start recording");
            else
            {
                dlog("diagnostic recording started: %s", path.c_str());
                say("Recording to: " + path);
            }
        }
        break;
    case kExit:
        exit_ = true;
        break;
    case kSubBg:
        // ✕ on the slider resets to the default, matching the "✕ Adjust" chip.
        prefs_.sub_bg_opacity = 55;
        settings_touch(kSubBg);
        save_prefs();
        cue(hui::audio::Cue::slider);
        break;
    default:
        break;
    }
    refresh_settings();
}

// Left/Right on the subtitle-background slider: step the opacity, save, and
// refresh the row. The slider cue is a soft tick, not the saved chime.
void Session::settings_nudge_slider(int dir)
{
    note_input(); // user action: reset the screensaver idle timer
    prefs_.sub_bg_opacity = std::clamp(prefs_.sub_bg_opacity + dir * 5, 0, 100);
    settings_touch(kSubBg);
    save_prefs();
    cue(hui::audio::Cue::slider);
    refresh_settings();
}

// "Recently changed" bookkeeping: newest first, no duplicates, capped at 5.
// Recent slots themselves are never tracked.
void Session::settings_touch(int row)
{
    if (row <= kRecent4 || row >= kSettingCount)
        return;
    recent_order_.erase(std::remove(recent_order_.begin(), recent_order_.end(), row), recent_order_.end());
    recent_order_.push_front(row);
    while (recent_order_.size() > 5)
        recent_order_.pop_back();
}

// Which subtitle style preset the current prefs most closely match.
int Session::sub_style_current() const
{
    if (prefs_.sub_size == "sub-l" && prefs_.sub_color == "yellow" && prefs_.sub_bg_opacity >= 85)
        return 2; // High contrast
    if (prefs_.sub_size == "sub-l")
        return 1; // Large
    return 0;     // Default
}

// One preset writes the same four pref keys the individual rows use
// (WORKER 4/8's keys): size, color, background opacity, position.
void Session::settings_apply_sub_style(int i)
{
    switch (i)
    {
    case 1: // Large
        prefs_.sub_size = "sub-l";
        prefs_.sub_color = "white";
        prefs_.sub_bg_opacity = 55;
        prefs_.sub_pos = "bottom";
        break;
    case 2: // High contrast
        prefs_.sub_size = "sub-l";
        prefs_.sub_color = "yellow";
        prefs_.sub_bg_opacity = 85;
        prefs_.sub_pos = "raised";
        break;
    default: // Default
        prefs_.sub_size = "sub-m";
        prefs_.sub_color = "white";
        prefs_.sub_bg_opacity = 55;
        prefs_.sub_pos = "bottom";
        break;
    }
}

// Inline Left/Right stepping: for value rows with <= 4 options the value
// cycles directly instead of opening a choice list (1 press vs 3). Writes
// exactly the pref values the dropdowns would (shared tables above); the
// shell only calls this when the focused row has step_inline set. True when
// the row stepped.
bool Session::settings_step_row(int row, int dir)
{
    note_input(); // user action: reset the screensaver idle timer
    // Recent slots step their source row (same mapping as settings_activate).
    if (row >= kRecent0 && row <= kRecent4)
    {
        const std::size_t slot = static_cast<std::size_t>(row - kRecent0);
        if (slot >= recent_shown_.size())
            return false;
        row = recent_shown_[slot];
    }
    const int d = dir >= 0 ? 1 : -1;
    const auto cycle_str = [&](const char *const *vals, int n, std::string &pref) {
        int cur = 0;
        for (int i = 0; i < n; ++i)
            if (pref == vals[i])
            {
                cur = i;
                break;
            }
        pref = vals[static_cast<std::size_t>((cur + d + n) % n)];
    };
    const auto cycle_int = [&](int lo, int hi, int &pref) {
        const int n = hi - lo + 1;
        pref = lo + (((pref - lo + d) % n + n) % n);
    };
    const auto cycle_choice = [&](const int *vals, int n, int &pref) {
        int cur = 0;
        for (int i = 0; i < n; ++i)
            if (pref == vals[i])
            {
                cur = i;
                break;
            }
        pref = vals[(cur + d + n) % n];
    };
    bool changed = false;
    switch (row)
    {
    case kTorrents:
        prefs_.builtin_torrents = !prefs_.builtin_torrents;
        changed = true;
        break;
    case kAspectMode:
        cycle_str(kAspectModes, 2, prefs_.aspect_mode);
        changed = true;
        break;
    case kSkipMode:
        cycle_int(0, 2, prefs_.skip_mode);
        changed = true;
        break;
    case kUpscaler:
        cycle_int(0, 4, prefs_.upscaler);
        changed = true;
        break;
    case kUpscalerSharpness:
        prefs_.upscaler_sharpness = std::clamp(prefs_.upscaler_sharpness + d * 0.25, 0.0, 1.0);
        changed = true;
        break;
    case kDeepBlacks:
        cycle_int(0, 2, prefs_.deep_blacks);
        changed = true;
        break;
    case kTonemapMode:
        cycle_int(0, 1, prefs_.tonemap_mode);
        changed = true;
        break;
    case kSubStyle:
        settings_apply_sub_style((sub_style_current() + d + 3) % 3);
        changed = true;
        break;
    case kSubSize:
        cycle_str(kSubSizes, 3, prefs_.sub_size);
        changed = true;
        break;
    case kSubColor:
        cycle_str(kSubColors, 2, prefs_.sub_color);
        changed = true;
        break;
    case kSubPos:
        cycle_str(kSubPositions, 2, prefs_.sub_pos);
        changed = true;
        break;
    case kAudioOutput:
        cycle_int(0, 1, prefs_.audio_output);
        changed = true;
        break;
    case kVolumeBoost:
        cycle_choice(kVolumeBoosts, 4, prefs_.volume_boost);
        changed = true;
        break;
    case kBingeAsk:
        cycle_choice(kBingeAsks, 4, prefs_.binge_ask);
        changed = true;
        break;
    default:
        break;
    }
    if (!changed)
        return false;
    settings_touch(row);
    save_prefs();
    cue(hui::audio::Cue::slider); // a soft tick: the value moved one step
    refresh_settings();
    return true;
}

// Cross-worker handoff (WORKER 3 owns gl/session_watch.cpp): the quick
// settings strip for the playback Up-menu. WORKER 3 calls this from
// watch_build_menu(); Cross on a returned row routes to
// settings_activate(row.setting_row), Left/Right to
// settings_step_row(row.setting_row, dir) when row.step_inline.
std::vector<Session::QuickSettingRow> Session::quick_settings_rows()
{
    refresh_settings(); // values are always current
    std::vector<QuickSettingRow> out;
    const int ids[] = {kSubSize, kUpscaler, kTonemapMode, kVolumeBoost, kNightMode};
    for (int id : ids)
    {
        const SettingRow &s = settings.rows[static_cast<std::size_t>(id)];
        QuickSettingRow q;
        q.label = s.label;
        q.value = s.kind == SettingRow::Kind::toggle ? (s.on ? "On" : "Off") : s.value;
        q.setting_row = id;
        q.step_inline = s.step_inline;
        out.push_back(q);
    }
    return out;
}

// Per-title audio delay, mirroring the per-title subtitle-delay mechanism:
// the remembered value wins, otherwise the global lip-sync pref applies.
double Session::audio_delay_for(const std::string &key) const
{
    if (!key.empty())
    {
        const auto it = prefs_.track_memory.find(key);
        if (it != prefs_.track_memory.end() && it->second.has_audio_delay)
            return it->second.audio_delay;
    }
    return prefs_.audio_delay;
}

void Session::set_audio_delay_for(const std::string &key, double v)
{
    // Same range and 50 ms snapping as the global path.
    v = std::lround(std::clamp(v, -0.5, 0.5) / 0.05) * 0.05;
    if (key.empty())
    {
        prefs_.audio_delay = v;
    }
    else
    {
        Prefs::TrackMemory &m = prefs_.track_memory[key];
        m.audio_delay = v;
        m.has_audio_delay = true;
        watch_prune_track_memory(); // keep the map bounded, like the other writers
    }
    save_prefs();
}

// ---------------------------------------------------------------------------
// Pop-ups

void Session::say(const std::string &message, double seconds, bool error)
{
    toast.message = message;
    toast.error = error;
    toast_open = true;
    cue(hui::audio::Cue::notify);
    toast_until_ = now_seconds() + seconds;
}

void Session::open_dropdown(const std::string &title, const std::vector<std::string> &options, int active,
                            std::function<void(int)> chosen, std::vector<std::string> descriptions)
{
    dropdown.title = title;
    dropdown.message.clear();
    dropdown.descriptions = std::move(descriptions);
    dropdown.danger = -1;
    dropdown.dialog = false;
    dropdown.poster = 0;
    dropdown.progress = -1.0f;
    dropdown.options = options;
    dropdown.active = active;
    dropdown.selected = std::max(0, std::min(active, static_cast<int>(options.size()) - 1));
    dropdown_chosen_ = std::move(chosen);
    dropdown_open = true;
}

void Session::dropdown_confirm()
{
    note_input(); // user action: reset the screensaver idle timer
    dropdown_open = false;
    const auto fn = dropdown_chosen_;
    dropdown_chosen_ = nullptr;
    if (fn && dropdown.selected >= 0 && dropdown.selected < static_cast<int>(dropdown.options.size()))
        fn(dropdown.selected);
}

void Session::dropdown_cancel()
{
    note_input(); // user action: reset the screensaver idle timer
    dropdown_open = false;
    dropdown_chosen_ = nullptr;
}

// ---------------------------------------------------------------------------
// Signing in with a code approved on another device

void Session::login_start()
{
    note_input(); // user action: reset the screensaver idle timer
    const int gen = ++login_gen_;
    sign_in_open = true;
    sign_in = SignInState();
    sign_in.link = "link.stremio.com";
    sign_in.status = "Loading...";
    login_code_raw_.clear();
    login_qr_url_.clear();
    login_polling_ = false;
    bg<LinkCode>([]() { return link_create(); },
                 [this, gen](LinkCode &lc) {
                     if (gen != login_gen_ || !sign_in_open)
                         return;
                     if (!lc.ok)
                     {
                         sign_in.status = "Could not get a code: " + lc.error;
                         return;
                     }
                     login_code_raw_ = lc.code;
                     sign_in.code = lc.code;
                     std::string shown = lc.link;
                     if (starts_with(shown, "https://"))
                         shown = shown.substr(8);
                     sign_in.link = shown;
                     login_qr_url_ = "https://link.stremio.com/qr?data=" + url_encode(lc.link);
                     sign_in.status = "Waiting for you to approve the sign-in on your phone or computer...";
                     login_next_poll_ = now_seconds() + 3;
                     login_expires_ = now_seconds() + 10 * 60;
                 });
}

void Session::login_poll()
{
    sign_in.qr = login_qr_url_.empty() ? 0 : tex_.get(login_qr_url_, ArtKind::qr);
    if (login_code_raw_.empty() || login_polling_ || now_seconds() < login_next_poll_)
        return;
    if (now_seconds() > login_expires_)
    {
        sign_in.status = "The sign-in code expired. Try again from Settings.";
        login_code_raw_.clear();
        return;
    }
    login_polling_ = true;
    const int gen = login_gen_;
    const std::string code = login_code_raw_;
    bg<ApiResult>([code]() { return link_read(code); },
                  [this, gen](ApiResult &r) {
                      login_polling_ = false;
                      login_next_poll_ = now_seconds() + 3;
                      if (gen != login_gen_ || !sign_in_open)
                          return;
                      if (r.ok)
                          login_done(jstr(r.result, "authKey"));
                  });
}

void Session::sign_in_cancel()
{
    note_input(); // user action: reset the screensaver idle timer
    ++login_gen_;
    sign_in_open = false;
    login_code_raw_.clear();
    login_as_new_profile_ = false; // backing out of "add account" cancels it
}

void Session::login_done(const std::string &auth_key)
{
    sign_in_cancel();
    if (login_as_new_profile_)
    {
        // "Add account": the new login becomes a new profile, not a replacement.
        login_as_new_profile_ = false;
        if (static_cast<int>(prefs_.profiles.size()) < Prefs::kMaxProfiles)
        {
            Prefs::Profile p;
            p.id = "p" + std::to_string(prefs_.profiles.size() + 1);
            p.auth_key = auth_key;
            p.name = "Profile " + std::to_string(prefs_.profiles.size() + 1);
            prefs_.profiles.push_back(std::move(p));
            prefs_.active_profile = static_cast<int>(prefs_.profiles.size()) - 1;
        }
    }
    prefs_.auth_key = auth_key;
    save_prefs();
    say("Signed in");
    bg<ApiResult>([auth_key]() { return api_get_user(auth_key); },
                  [this](ApiResult &r) {
                      if (!r.ok)
                          return;
                      prefs_.user_email = jstr(r.result, "email");
                      // Keep the active profile in sync with the live credentials.
                      if (prefs_.active_profile >= 0 &&
                          prefs_.active_profile < static_cast<int>(prefs_.profiles.size()))
                      {
                          auto &pr = prefs_.profiles[static_cast<std::size_t>(prefs_.active_profile)];
                          pr.auth_key = prefs_.auth_key;
                          pr.user_email = prefs_.user_email;
                          if (!pr.user_email.empty())
                              pr.name = pr.user_email;
                      }
                      else if (!prefs_.auth_key.empty())
                      {
                          // No profile slot (should not happen after migration):
                          // create one so the account is kept.
                          Prefs::Profile p;
                          p.id = "p1";
                          p.auth_key = prefs_.auth_key;
                          p.user_email = prefs_.user_email;
                          p.name = p.user_email.empty() ? "Profile 1" : p.user_email;
                          prefs_.profiles.push_back(std::move(p));
                          prefs_.active_profile = 0;
                      }
                      save_prefs();
                      refresh_settings();
                      say("Signed in as " + prefs_.user_email);
                  });
    library_.clear();
    library_loaded_ = false;
    load_addons();
    load_library();
    refresh_settings();
}

void Session::sign_out()
{
    const std::string key = prefs_.auth_key;
    g_tasks.run([key]() { api_logout(key); });
    prefs_.auth_key.clear();
    prefs_.user_email.clear();
    if (prefs_.active_profile >= 0 && prefs_.active_profile < static_cast<int>(prefs_.profiles.size()))
    {
        // Keep the profile slot; only its credentials are cleared.
        auto &pr = prefs_.profiles[static_cast<std::size_t>(prefs_.active_profile)];
        pr.auth_key.clear();
        pr.user_email.clear();
    }
    library_.clear();
    library_loaded_ = false;
    save_prefs();
    say("Signed out");
    load_addons();
    refresh_library();
    refresh_settings();
}

// ---------------------------------------------------------------------------
// Multi-account profiles (up to 10). prefs_.auth_key/user_email always mirror
// the active profile so the rest of the code is unchanged.

void Session::profile_sync_active()
{
    if (prefs_.active_profile >= 0 && prefs_.active_profile < static_cast<int>(prefs_.profiles.size()))
    {
        const auto &pr = prefs_.profiles[static_cast<std::size_t>(prefs_.active_profile)];
        prefs_.auth_key = pr.auth_key;
        prefs_.user_email = pr.user_email;
    }
    else
    {
        prefs_.auth_key.clear();
        prefs_.user_email.clear();
    }
}

void Session::profile_reload_account_state()
{
    library_.clear();
    library_loaded_ = false;
    load_addons();
    load_library();
    refresh_library();
    refresh_settings();
}

int Session::profile_count() const
{
    return static_cast<int>(prefs_.profiles.size());
}

std::string Session::profile_name(int i) const
{
    if (i < 0 || i >= static_cast<int>(prefs_.profiles.size()))
        return {};
    return prefs_.profiles[static_cast<std::size_t>(i)].name;
}

int Session::active_profile() const
{
    return prefs_.active_profile;
}

void Session::profile_add()
{
    note_input();
    confirm_remove_profile_ = false; // a different action cancels a pending remove
    pending_profile_action_ = -1;
    if (static_cast<int>(prefs_.profiles.size()) >= Prefs::kMaxProfiles)
    {
        say("Maximum of 10 accounts reached.", 4.0, true);
        return;
    }
    login_as_new_profile_ = true;
    login_start();
}

void Session::profile_switch(int i)
{
    note_input();
    confirm_remove_profile_ = false; // a different action cancels a pending remove
    pending_profile_action_ = -1;
    if (i < 0 || i >= static_cast<int>(prefs_.profiles.size()) || i == prefs_.active_profile)
        return;
    // Stash the live credentials into the outgoing profile first.
    if (prefs_.active_profile >= 0 && prefs_.active_profile < static_cast<int>(prefs_.profiles.size()))
    {
        auto &old = prefs_.profiles[static_cast<std::size_t>(prefs_.active_profile)];
        old.auth_key = prefs_.auth_key;
        old.user_email = prefs_.user_email;
    }
    prefs_.active_profile = i;
    profile_sync_active();
    save_prefs();
    say("Switched to " + prefs_.profiles[static_cast<std::size_t>(i)].name);
    profile_reload_account_state();
}

void Session::profile_remove(int i)
{
    note_input();
    if (i < 0 || i >= static_cast<int>(prefs_.profiles.size()))
        return;
    const std::string name = prefs_.profiles[static_cast<std::size_t>(i)].name;
    if (!confirm_remove_profile_ || pending_profile_action_ != i)
    {
        confirm_remove_profile_ = true;
        pending_profile_action_ = i;
        say("Press again to remove " + name + ".", 4.0, false);
        return;
    }
    confirm_remove_profile_ = false;
    pending_profile_action_ = -1;
    const std::string key = prefs_.profiles[static_cast<std::size_t>(i)].auth_key;
    if (!key.empty())
        g_tasks.run([key]() { api_logout(key); });
    prefs_.profiles.erase(prefs_.profiles.begin() + i);
    if (prefs_.active_profile == i)
    {
        prefs_.active_profile = prefs_.profiles.empty() ? -1 : 0;
        profile_sync_active();
        profile_reload_account_state();
    }
    else if (prefs_.active_profile > i)
        --prefs_.active_profile;
    save_prefs();
    refresh_settings();
    say("Removed " + name);
}

void Session::profile_dropdown()
{
    note_input();
    // NOTE: confirm_remove_profile_ is intentionally NOT reset here: the
    // two-step remove survives reopening the dropdown for the second press.
    const int n = static_cast<int>(prefs_.profiles.size());
    std::vector<std::string> options;
    for (int i = 0; i < n; ++i)
    {
        const auto &p = prefs_.profiles[static_cast<std::size_t>(i)];
        std::string label = (i == prefs_.active_profile ? "[active] " : "") + p.name;
        if (p.auth_key.empty())
            label += " (signed out)";
        options.push_back(std::move(label));
    }
    const int kAdd = n;
    const int kRemove = n + 1;
    const int kSignOut = n + 2;
    (void)kAdd; (void)kRemove; (void)kSignOut; // indices derived inline below
    if (n < Prefs::kMaxProfiles)
        options.push_back("Add account");
    if (prefs_.active_profile >= 0)
        options.push_back("Remove this account");
    options.push_back(signed_in() ? "Sign out" : "Close");
    // Map the pressed index back even when some entries were skipped.
    open_dropdown("Accounts", options, prefs_.active_profile >= 0 ? prefs_.active_profile : 0,
                  [this](int choice) {
                      if (choice < 0)
                          return;
                      const int nn = static_cast<int>(prefs_.profiles.size());
                      int idx = choice;
                      bool is_add = false, is_remove = false, is_signout = false;
                      if (idx < nn)
                      {
                          // a profile row
                      }
                      else
                      {
                          idx -= nn;
                          // order of the trailing entries depends on what was shown
                          if (nn < Prefs::kMaxProfiles)
                          {
                              if (idx == 0) { is_add = true; }
                              else { idx -= 1; }
                          }
                          if (!is_add && prefs_.active_profile >= 0)
                          {
                              if (idx == 0) { is_remove = true; }
                              else { idx -= 1; }
                          }
                          if (!is_add && !is_remove)
                              is_signout = true;
                      }
                      if (is_add)
                          profile_add();
                      else if (is_remove)
                          profile_remove(prefs_.active_profile);
                      else if (is_signout)
                      {
                          if (signed_in())
                              sign_out();
                      }
                      else if (choice < nn)
                          profile_switch(choice);
                  });
}

// ---------------------------------------------------------------------------
// OLED screensaver: 120 s idle -> dim to near-black, 300 s -> pure-black
// drifting logo. Video playback and typing disable the timer entirely.
// The shell draws the overlay from screensaver_phase()/screensaver_dim()/
// screensaver_logo_*(); on wake it consumes the first input via
// screensaver_wake_armed()/screensaver_wake().

void Session::touch_idle()
{
    idle_since_ = now_seconds();
    if (saver_phase_ == 1)
    {
        saver_phase_ = 0;
        saver_dim_t_ = 0;
    }
    // Phase 2 is exited only through screensaver_wake(), so the armed first
    // input is never silently swallowed by a stray note_input().
}

void Session::note_input()
{
    touch_idle();
}

void Session::screensaver_wake()
{
    saver_wake_armed_ = false;
    saver_phase_ = 0;
    saver_t_ = 0;
    saver_dim_t_ = 0;
    idle_since_ = now_seconds();
}

void Session::update_saver(float dt, double now)
{
    // Active video playback and typing disable the idle timer entirely.
    if (watching || typing())
    {
        idle_since_ = now;
        if (saver_phase_ != 0)
        {
            saver_phase_ = 0;
            saver_dim_t_ = 0;
            saver_t_ = 0;
            saver_wake_armed_ = false;
        }
        return;
    }
    const double idle = now - idle_since_;
    if (idle >= 300.0)
    {
        if (saver_phase_ != 2)
        {
            saver_phase_ = 2;
            saver_t_ = 0;
            saver_wake_armed_ = true; // the first wake input is consumed
        }
        saver_t_ += dt;
    }
    else if (idle >= 120.0)
    {
        if (saver_phase_ != 1)
        {
            saver_phase_ = 1;
            saver_dim_t_ = 0;
        }
        saver_dim_t_ += dt;
        saver_wake_armed_ = false;
    }
    else if (saver_phase_ != 0)
    {
        saver_phase_ = 0;
        saver_dim_t_ = 0;
        saver_t_ = 0;
        saver_wake_armed_ = false;
    }
}

float Session::screensaver_dim() const
{
    if (saver_phase_ == 2)
        return 1.0f;
    if (saver_phase_ == 1)
        return std::min(0.85f, static_cast<float>(saver_dim_t_ / 8.0) * 0.85f);
    return 0.0f;
}

float Session::screensaver_logo_x() const
{
    return 0.5f + 0.35f * std::sin(static_cast<float>(saver_t_ * 0.11));
}

float Session::screensaver_logo_y() const
{
    return 0.5f + 0.30f * std::sin(static_cast<float>(saver_t_ * 0.073 + 1.3));
}

// ---------------------------------------------------------------------------
// Frame

void Session::update(float dt, const Cursor &cursor)
{
    (void)dt;
    g_tasks.drain();
    const double now = now_seconds();
    update_saver(dt, now);
    // While the first screen is being got ready, more pictures go up to the GPU each frame.
    const bool arrived = tex_.pump(boot_done_ ? 3 : 12);

    if (now - clock_ > 1.0)
    {
        clock_ = now;
        clock = format_clock();
    }
    if (!boot_done_ && boot_check(now - boot_start_))
    {
        boot_done_ = true;
        dlog("boot: ready after %.1f s", now - boot_start_);
    }
    if (toast_open && now > toast_until_)
        toast_open = false;
    if (cache_size_updated_.exchange(false))
        refresh_settings();
    // Played from Continue Watching on the Board: the title's page was open behind it only to play.
    // When the video is over (and no next episode is lined up), it closes.
    if (resumed_from_board_ && !watching && !resuming_ && !autoplay_pending_)
    {
        resumed_from_board_ = false;
        close_detail();
    }
    if (sign_in_open)
        login_poll();
    poll_typing();
    if (watching)
    {
        watch_update();
        upload_video();
    }

    // More of Discover when the cursor nears the end.
    constexpr int kDiscoverCols = 4;
    if (!disc_end_ && !disc_loading_ && !disc_items_.empty() &&
        cursor.discover + 2 * kDiscoverCols >= static_cast<int>(disc_items_.size()))
        discover_load(true);
    // The Board hero needs full meta (description, background, logo) for the
    // focused title; catalogs only carry name+poster.
    hero_meta_check(cursor);

    const bool moved = cursor.board_row != last_cursor_.board_row || cursor.board_col != last_cursor_.board_col ||
                       cursor.discover != last_cursor_.discover || cursor.library != last_cursor_.library ||
                       cursor.episode != last_cursor_.episode || cursor.search_row != last_cursor_.search_row ||
                       cursor.search_col != last_cursor_.search_col;
    // Track the scroll direction so refresh_art() can prefetch ahead of the
    // cursor asymmetrically.
    if (cursor.board_col != last_cursor_.board_col)
        scroll_dir_col_ = (cursor.board_col > last_cursor_.board_col) ? 1 : -1;
    if (cursor.board_row != last_cursor_.board_row)
        scroll_dir_row_ = (cursor.board_row > last_cursor_.board_row) ? 1 : -1;
    last_cursor_ = cursor;
    if (moved)
    {
        // The per-move tex_.bump_epoch() is gone (it was the only caller):
        // the priority queue re-prioritizes the new window's jobs on every
        // refresh_art() below, so stale queued jobs no longer need an epoch
        // kill. See the report for why cancel_outside() is not called here.
        touch_idle();      // cursor navigation resets the screensaver timer
    }
    // 300ms dwell: the Discover focus has settled on a title whose meta has
    // not been asked for and is not cached yet. Refreshing art here lets the
    // focused title's preview picture (and its full meta) arrive without
    // waiting for the 1s periodic refresh.
    if (!meta_dwell_key_.empty() && now - meta_dwell_since_ >= 0.3 &&
        !meta_asked_.count(meta_dwell_key_) && !meta_cache_.count(meta_dwell_key_))
        art_dirty_ = true;
    // 250ms speculative dwell: the focused stream row on the title's page
    // settled (note_stream_focus() armed spec_dwell_idx_), so hand it to the
    // watch prefetch — Play then resolves (near-)instantly. Fires once per
    // dwell; torrent info_hash entries resolve inside the impl.
    if (d_open_ && !watching && spec_dwell_idx_ >= 0 && now - spec_dwell_since_ >= 0.25)
    {
        const int idx = spec_dwell_idx_;
        spec_dwell_idx_ = -1;
        spec_prefetch_stream(idx);
    }
    if (arrived || moved || art_dirty_ || now - last_art_ > 1.0)
    {
        art_dirty_ = false;
        last_art_ = now;
        refresh_art(cursor);
    }
    if (arrived && tex_.trim(kTextureBudget))
        refresh_art(cursor);
    // WORKER 10 (polish): the one-line per-frame pump — haptics timers, the haptics
    // pref, and the hints-bar state machine. Never draws; the shell draws the bar.
    sx::polish_frame(dt, prefs_.haptics, sx::hints_context_for(watching, d_open_, search_open), now - idle_since_);
}

// Speculative stream prefetch: the real logic is
// Session::spec_prefetch_stream_impl() in session_watch.cpp (Worker C); this
// stub is the update() entry point so the two never collide at link.
void Session::spec_prefetch_stream(int idx)
{
    spec_prefetch_stream_impl(idx);
}

// The app is ready to be shown as soon as the first row's pictures are on
// the GPU (sub-1s boot): the mark gets a brief reveal, the boot never waits
// for slow catalogs, and pictures that have not come by 2.5s come when they come.
// Instant Board: the snapshot (or an empty board) is painted in init(), so
// there is no boot gate left to wait through. Supersedes the old timed gates.
bool Session::boot_check(double) const
{
    return true;
}

// ---------------------------------------------------------------------------
// Artwork: the textures for what is near the cursor; the rest only if they are
// already on the GPU.

void Session::refresh_art(const Cursor &cursor)
{
    const double now = now_seconds();
    const auto fetch = [&](const std::string &url, ArtKind kind, bool near) {
        return near ? tex_.get(url, kind) : tex_.peek(url, kind);
    };

    // Board: cards around the cursor, and the hero's picture for the focused title and its neighbours.
    // Asymmetric column window: prefetch further ahead in the scroll
    // direction; priority ranks nearer cards (and same-direction cards)
    // first. tex_.get() with priority is Worker A's API; off-window cards
    // only peek() so they never queue a download.
    const int col_lo = (scroll_dir_col_ >= 0) ? (cursor.board_col - 4) : (cursor.board_col - 16);
    const int col_hi = (scroll_dir_col_ >= 0) ? (cursor.board_col + 16) : (cursor.board_col + 4);
    for (std::size_t r = 0; r < board.rows.size() && r < board_map_.size(); ++r)
    {
        const std::vector<Item> &items = board_[static_cast<std::size_t>(board_map_[r])].items;
        const bool row_near = std::abs(static_cast<int>(r) - cursor.board_row) <= 2;
        const int row_dist = std::abs(static_cast<int>(r) - cursor.board_row);
        const bool is_cw = board_[static_cast<std::size_t>(board_map_[r])].continue_watching;
        for (std::size_t c = 0; c < board.rows[r].items.size() && c < items.size(); ++c)
        {
            Title &t = board.rows[r].items[c];
            const int col = static_cast<int>(c);
            const int col_dist = col - cursor.board_col;
            const bool card_near = row_near && col > col_lo && col < col_hi;
            if (card_near)
            {
                // Row distance dominates; columns ahead of the scroll
                // direction beat columns behind it 3:1.
                const bool same_dir = (scroll_dir_col_ >= 0) == (col_dist >= 0);
                const float prio = static_cast<float>(
                    row_dist * 10 + (same_dir ? std::abs(col_dist) : std::abs(col_dist) * 3));
                t.poster = tex_.get(items[c].poster, ArtKind::poster, prio);
            }
            else
            {
                t.poster = tex_.peek(items[c].poster, ArtKind::poster);
            }
            const bool hero = static_cast<int>(r) == cursor.board_row && std::abs(col - cursor.board_col) <= 1;
            std::string bg_url = items[c].background;
            if (bg_url.empty() && is_cw && !items[c].video_thumb.empty())
                bg_url = items[c].video_thumb;
            if (bg_url.empty() && items[c].id.compare(0, 2, "tt") == 0)
                bg_url = "https://images.metahub.space/background/medium/" + items[c].id + "/img";
            t.backdrop = fetch(bg_url, ArtKind::backdrop, hero || (is_cw && card_near));
            std::string logo_url = items[c].logo;
            if (logo_url.empty() && items[c].id.compare(0, 2, "tt") == 0)
                logo_url = "https://images.metahub.space/logo/medium/" + items[c].id + "/img";
            t.logo = fetch(logo_url, ArtKind::logo, hero);
            if (t.logo != 0)
                tex_.dims(logo_url, ArtKind::logo, &t.logo_w, &t.logo_h);
            // Ambient color + fade: accent from the backdrop (poster fallback),
            // fade from the poster's GPU upload age.
            t.accent = tex_.accent_of(items[c].background, ArtKind::backdrop);
            if (t.accent == 0x3a2a8f && !items[c].poster.empty())
                t.accent = tex_.accent_of(items[c].poster, ArtKind::poster);
            t.poster_fade = std::clamp(tex_.age(items[c].poster, ArtKind::poster, now) / 0.18f, 0.0f, 1.0f);
            if (t.poster != 0 && t.poster_fade < 1.0f) art_dirty_ = true;
            // The library lists a title without its backdrop or story: ask the addons.
            if (hero && (items[c].background.empty() || items[c].description.empty()))
                want_meta(items[c]);
        }
    }

    // Search results: the same, without a hero. Row 0 may be the local
    // (trending/on-device) row, which lives in search_local_items_ rather
    // than search_map_; the map is offset by one while it is present.
    for (std::size_t r = 0; r < search.rows.size(); ++r)
    {
        const bool is_local = search_local_have_ && r == 0;
        const std::size_t net_r = search_local_have_ ? r - 1 : r;
        if (!is_local && net_r >= search_map_.size())
            break;
        const std::vector<Item> &items = is_local
            ? search_local_items_
            : search_[static_cast<std::size_t>(search_map_[net_r])].items;
        const bool row_near = std::abs(static_cast<int>(r) - cursor.search_row) <= 2;
        for (std::size_t c = 0; c < search.rows[r].items.size() && c < items.size(); ++c)
        {
            const int scol = static_cast<int>(c);
            search.rows[r].items[c].poster = fetch(
                items[c].poster, ArtKind::poster,
                row_near && scol > cursor.search_col - 12 && scol < cursor.search_col + 12);
        }
    }

    // Starting a stream: the title's picture behind the ring and in it.
    if (launch_open)
    {
        launch.backdrop = fetch(w_item_.background, ArtKind::backdrop, true);
        launch.poster = fetch(w_item_.poster, ArtKind::poster, true);
        launch.logo = fetch(w_item_.logo, ArtKind::logo, true);
        if (launch.logo != 0)
            tex_.dims(w_item_.logo, ArtKind::logo, &launch.logo_w, &launch.logo_h);
    }

    // Discover: posters in a window, and the preview picture of the focused title.
    for (std::size_t i = 0; i < discover.items.size() && i < disc_items_.size(); ++i)
    {
        Title &t = discover.items[i];
        const int d = static_cast<int>(i) - cursor.discover;
        if (d == 0)
        {
            apply_meta(disc_items_[i]);
            if (t.synopsis.empty() && !disc_items_[i].description.empty())
                t.synopsis = disc_items_[i].description;
            if (t.runtime.empty() && !disc_items_[i].runtime.empty())
                t.runtime = disc_items_[i].runtime;
            if (t.year.empty() && !disc_items_[i].release_info.empty())
                t.year = disc_items_[i].release_info;
            if (t.imdb.empty() && !disc_items_[i].imdb_rating.empty())
                t.imdb = disc_items_[i].imdb_rating;
            if (t.genres.empty() && !disc_items_[i].genres.empty())
                t.genres = join(disc_items_[i].genres, ", ");
            if (disc_items_[i].background.empty() || disc_items_[i].description.empty())
                want_meta(disc_items_[i]);
        }
        t.poster = fetch(disc_items_[i].poster, ArtKind::poster, d > -12 && d < 24);
        t.poster_fade = std::clamp(tex_.age(disc_items_[i].poster, ArtKind::poster, now) / 0.18f, 0.0f, 1.0f);
        if (t.poster != 0 && t.poster_fade < 1.0f) art_dirty_ = true;
        std::string bg_url = disc_items_[i].background;
        if (bg_url.empty() && disc_items_[i].id.compare(0, 2, "tt") == 0)
            bg_url = "https://images.metahub.space/background/medium/" + disc_items_[i].id + "/img";
        t.backdrop = fetch(bg_url, ArtKind::backdrop, d == 0);
    }

    // Library
    for (std::size_t i = 0; i < library.items.size() && i < lib_items_.size(); ++i)
    {
        Title &t = library.items[i];
        const int d = static_cast<int>(i) - cursor.library;
        t.poster = fetch(lib_items_[i].poster, ArtKind::poster, d > -14 && d < 28);
        t.poster_fade = std::clamp(tex_.age(lib_items_[i].poster, ArtKind::poster, now) / 0.18f, 0.0f, 1.0f);
        if (t.poster != 0 && t.poster_fade < 1.0f) art_dirty_ = true;
    }

    // Addons
    for (std::size_t i = 0; i < addons.items.size() && i < addon_logos_.size(); ++i)
        addons.items[i].logo = fetch(addon_logos_[i], ArtKind::icon, true);

    // A title's page
    if (d_open_)
    {
        std::string background = d_meta_loaded_ && !d_meta_.background.empty() ? d_meta_.background : d_item_.background;
        if (background.empty() && d_item_.id.compare(0, 2, "tt") == 0)
            background = "https://images.metahub.space/background/medium/" + d_item_.id + "/img";
        detail.title.backdrop = fetch(background, ArtKind::backdrop, true);

        std::string logo_url = d_meta_loaded_ && !d_meta_.logo.empty() ? d_meta_.logo : d_item_.logo;
        if (logo_url.empty() && d_item_.id.compare(0, 2, "tt") == 0)
            logo_url = "https://images.metahub.space/logo/medium/" + d_item_.id + "/img";
        detail.title.logo = fetch(logo_url, ArtKind::logo, true);
        if (detail.title.logo != 0)
            tex_.dims(logo_url, ArtKind::logo, &detail.title.logo_w, &detail.title.logo_h);

        detail.title.accent = tex_.accent_of(background, ArtKind::backdrop);
        if (detail.title.accent == 0x3a2a8f && !d_item_.poster.empty())
            detail.title.accent = tex_.accent_of(d_item_.poster, ArtKind::poster);

        for (std::size_t i = 0; i < detail.episodes.size() && i < d_season_videos_.size(); ++i)
            detail.episodes[i].thumb = fetch(d_season_videos_[i]->thumbnail, ArtKind::thumb,
                                             std::abs(static_cast<int>(i) - cursor.episode) < 10);
    }
}

} // namespace sx
