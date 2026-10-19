// What the app knows about, apart from how it is drawn: something that can be
// opened, what was watched, the user's settings.

#pragma once

#include "stremio.h"
#include "util.h"

#include "aniskip.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sx
{

// Something that can be opened: a catalog entry, a library item.
struct Item
{
    std::string id, type, name, poster, background, logo;
    std::string description, release_info, runtime, imdb_rating;
    std::vector<std::string> genres;
    // Continue watching
    std::string video_id;
    std::string video_thumb; // the episode's own thumbnail URL, when the meta fetch knew it
    double offset = 0, duration = 0; // seconds
    std::int64_t last_watched = 0;   // epoch ms
    int times_watched = 0;
    bool up_next = false; // video_id is the episode after one that was finished (nothing watched of it yet)
};

Item item_from_meta_json(const json &j);

// Where a title was left, for people who are not signed in.
struct Progress
{
    std::string type, name, poster, video_id;
    double time = 0, duration = 0;
    std::int64_t updated = 0;
    // The stream (as JSON) that was playing and the episode it was for: Continue Watching plays it again.
    std::string stream_json, stream_video_id;
    // The episode after the one that was just finished, until something of it is watched.
    std::string up_next;
};

struct Prefs
{
    // Remembered audio/subtitle picks, per series or movie id: choosing a
    // track once applies it to every later episode of the same show.
    struct TrackMemory
    {
        std::string audio_lang; // "" = no remembered audio pick
        std::string sub_lang;   // "" = no remembered subtitle pick
        bool sub_internal = false; // the remembered sub was in-file (not addon)
        bool sub_off = false;      // remembered "subtitles off"
        double sub_delay = 0;      // remembered subtitle delay in seconds
        // ---- WORKER 7 (SETTINGS): per-title audio delay ----
        double audio_delay = 0;       // remembered audio lip-sync offset, seconds
        bool has_audio_delay = false; // an explicit per-title delay was stored
    };
    std::string server_url;
    bool builtin_torrents = true; // the app's own torrent engine, no server needed
    std::string subtitle_langs = "eng";
    bool auto_subtitles = true;
    std::string sub_size = "sub-m";
    std::string audio_langs;
    int audio_output = 0;   // 0 = stereo downmix, 1 = discrete 5.1/7.1 PCM over HDMI
    int volume_boost = 100; // percent: 100, 125, 150, 200
    int volume = 100;       // master volume 0-100 (touchpad swipe); persisted
    bool night_mode = false;       // DRC: tame loud peaks, lift quiet dialogue
    bool dolby_passthrough = false; // bitstream AC-3 to the receiver (opt-in)
    bool match_frame_rate = false;  // experimental: switch HDMI to 119.88 Hz for ~24fps film
    bool hdr_probe = false;        // HDR10 diagnostic probe: read-only VideoOut query at startup (default off)
    bool autoplay_next = true;
    bool autoplay_best = true; // play the best stream without showing the picker
    int binge_ask = 3;         // "Still watching?" after this many automatic episodes; 0 = off
    std::string sub_color = "white"; // subtitle text color: "white" | "yellow"
    int sub_bg_opacity = 55;        // subtitle background darkness, 0-100 (0 = no box)
    std::string sub_pos = "bottom";  // subtitle position: "bottom" | "raised"
    std::string aspect_mode = "fit";   // video zoom: "fit" (whole picture) | "fill" (crop to screen)
    bool ui_sounds = true; // the interface's soft sounds
    // ---- WORKER 10 (polish) ----
    bool haptics = true; // DualSense UI haptics: tick on move, pulse on select
    // ---- end WORKER 10 ----
    int skip_mode = 1;     // intro/outro skip: 0 off, 1 prompt, 2 auto
    int disc_sort = 0;     // discover sort: 0 default (catalog order), 1 rating, 2 year (newest)
    std::string max_resolution = "auto"; // stream cap: "auto" | "4k" | "1080p" | "720p" | "480p"
    std::string os_user; // OpenSubtitles.com username (for a supporting subtitle addon)
    std::string os_pass; // OpenSubtitles.com password (stored locally, never synced)
    // ---- SEARCH UX (worker 5): recent searches with timestamps ----
    // Exactly 5 kept, auto-expire after 30 days. Replaces the old plain
    // search_history strings (migrated once on load).
    struct RecentSearch
    {
        std::string query;
        double at = 0; // unix epoch seconds, when the search ran
    };
    std::vector<RecentSearch> recent_searches;
    double audio_delay = 0.0; // audio lip-sync offset in seconds, -0.5..+0.5 (+ = audio later)
    int upscaler = 1; // video upscaler: 0 off, 1 FSR1, 2 FSR1+sharpening (3 Anime4K reserved)
    int deep_blacks = 0;   // 0 = Off, 1 = Low, 2 = High
    int tonemap_mode = 0;  // 0 = Hable (Filmic), 1 = BT.2390 (Broadcast)
    double upscaler_sharpness = 0.5; // RCAS strength 0.0-1.0, for upscaler == 2
    std::map<std::string, int> aniskip_mal; // series id -> MyAnimeList id, for intro/outro lookup
    // TheIntroDB skip-segment cache: "tidb:<imdbid>" (movie) or
    // "tidb:<imdbid>:<season>:<episode>" -> fetch time + segments. An empty
    // segment list is a negative entry: the title definitively has no data.
    struct SkipDbEntry
    {
        double fetched_at = 0; // unix epoch seconds
        std::vector<aniskip::SkipSegment> segs;
    };
    std::map<std::string, SkipDbEntry> skipdb_cache;
    std::vector<std::string> extra_addons;
    std::set<std::string> disabled_addons; // transport_urls the user turned off on the Addons page
    std::vector<std::string> addon_order;  // transport_urls in the user's priority order
    std::string auth_key, user_email;
    // Multi-account profiles: up to 10 stored accounts. auth_key/user_email
    // above mirror the active profile's credentials for the rest of the code.
    struct Profile
    {
        std::string id;         // "p1", "p2", ...: stable across renames
        std::string name;       // display name (email when known)
        std::string auth_key;
        std::string user_email;
    };
    static constexpr int kMaxProfiles = 10;
    std::vector<Profile> profiles;
    int active_profile = -1; // index into profiles, -1 = none
    std::map<std::string, TrackMemory> track_memory; // series/movie id -> picks
    // ---- WORKER 7 (SETTINGS) prefs ----
    std::vector<int> recent_settings; // SettingIndex values, newest first (cap 5)
};

// One catalog of one addon, shown as a row on the Board.
struct BoardRow
{
    std::string key; // addon url | type | catalog id
    std::string title;
    std::string addon_url;
    Catalog catalog;
    std::vector<Item> items;
    bool continue_watching = false;
    bool loaded = false;
    std::string error; // non-empty when the catalog fetch failed
    // Instant Board: the snapshot lifecycle of this row. The session keeps
    // these in the model and projects them onto the UI's Row::state.
    bool validating = false;  // snapshot content is on screen while addons re-fetch
    bool from_snapshot = false; // this row's items came from the disk snapshot
};

} // namespace sx
