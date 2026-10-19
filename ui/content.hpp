// What the screens show: plain data, no graphics code. The console fills it
// from the user's addons; the preview tool fills it with invented samples.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "aniskip.h"
#include "text_cache.hpp"

namespace sx
{

struct Title
{
    std::string id;
    std::string type;      // "movie", "series": what the addons call it
    std::string name;
    std::string year;
    std::string runtime;   // "2h 08m"
    std::string genres;    // "Drama, Mystery"
    std::string imdb;      // "7.8", empty when unknown
    std::string synopsis;
    std::string badge;     // "S2 E5" on a series being watched
    std::string episode_art; // episode thumbnail URL for a continue-watching card, "" = show poster
    float progress = -1.0f; // 0..1 when it can be resumed, negative when not
    bool watched = false;   // finished: a green check on its poster
    std::uint32_t poster = 0;   // GL texture, 0 until the artwork has arrived
    std::uint32_t backdrop = 0; // GL texture, 0 until the artwork has arrived
    std::uint32_t logo = 0;     // title art, drawn in the hero instead of the name
    int logo_w = 0;             // its pixel size, so the hero keeps its aspect
    int logo_h = 0;
    float poster_fade = 1.0f;         // 0..1 fade-in over 180ms when first uploaded
    std::uint32_t accent = 0x3a2a8f;  // dominant artwork color for ambient bleed
    // Laid-out text (card name, hero title/meta/synopsis): same strings the
    // draw code would shape every frame, cached by (font, text, size, width).
    mutable CachedText text_cache_;
};

// Instant Board: the lifecycle of a row's content between the disk snapshot
// and the live addon answers.
enum class RowState
{
    cached,     // served from the disk snapshot, not yet re-validated
    validating, // snapshot content is on screen while the addons re-fetch it
    synced,     // matches the latest addon answers (the steady state)
    offline,    // snapshot content served with no network to refresh it
};

struct Row
{
    std::string title; // "Continue Watching", "Popular - Movie"
    std::vector<Title> items;
    std::string key;   // which catalog it is, so a row keeps the cursor when others appear above it
    // Instant Board: where this row's content came from and what is happening
    // to it. Snapshot rows start cached/validating and move to synced as the
    // addons confirm them; offline marks a row served entirely from disk.
    RowState state = RowState::synced;
};

struct BoardContent
{
    std::vector<Row> rows;
    std::string status; // what to say while there are no rows: "Loading your catalogs..."
    // Instant Board: the glass sync-status pill in the top bar. Empty = hidden.
    std::string sync_pill; // "Synced just now", "Syncing...", "Offline — cached"
    bool offline = false;  // the whole board was served from the snapshot without network
};

// A drop-down choice at the top of Discover and Library: "Type  Movie".
struct Filter
{
    std::string label; // "Type", "Catalog", "Genre"
    std::string value; // "Movie", "Popular", "All genres"
};

struct DiscoverContent
{
    std::vector<Filter> filters;
    std::vector<Title> items;
    std::string status; // while there are no items: "Loading...", "Nothing here."
};

struct LibraryContent
{
    std::vector<Filter> filters;
    std::vector<Title> items;
    std::string count; // "128 titles"
    std::string status; // while there are no items
};

struct Addon
{
    std::string name;
    std::string version;     // "v2.4.1"
    std::string types;       // "movie  series  tv"
    std::string description;
    std::string initial;     // shown when the addon has no logo
    std::uint32_t logo = 0;  // GL texture, 0 until it has arrived
    std::uint32_t colour = 0x7b5bf5; // the tile's colour while there is no logo
    bool local = false;      // runs on this console, not on a server
    bool disabled = false;   // turned off by the user; skipped for streams/meta/catalogs
    mutable CachedText text_cache_;
};

struct AddonsContent
{
    std::vector<Addon> items;
};

struct SettingRow
{
    enum class Kind
    {
        value,  // opens a list of choices
        toggle, // on or off
        slider, // Left/Right adjusts a numeric value in place
        action, // does something: sign out, reload
        header, // a section label: drawn dimmer, never focused or activated
    };
    Kind kind = Kind::value;
    std::string label;
    std::string value; // shown on the right
    bool on = false;   // for toggles
    int slider_pct = -1; // for sliders: 0-100 fill of the bar
    // WORKER 7 (SETTINGS): value rows with <= 4 options cycle the value
    // directly on Left/Right instead of opening a choice list.
    bool step_inline = false;
    std::string help;  // what it does, shown beside the list
    mutable CachedText text_cache_;
};

struct SettingsContent
{
    std::string account; // "alex@example.com"
    std::vector<SettingRow> rows;
};

struct Episode
{
    int number = 0;
    std::string title;
    std::string info;         // "Oct 4  ·  52m"
    std::string overview;     // the episode's own synopsis, may be empty
    bool watched = false;
    float progress = -1.0f;   // 0..1 when it can be resumed
    std::uint32_t thumb = 0;  // GL texture, 0 until it has arrived
    mutable CachedText text_cache_; // the fitted title line
};

// One way to watch: a file from an addon.
struct Stream
{
    std::string resolution; // "4K", "1080p", "720p", "SD"
    std::string name;       // the addon or release: "Stream Finder"
    std::string detail;     // the file name
    std::string tags;       // "HDR10  ·  Dolby Atmos"
    std::string size;       // "18.4 GB"
    std::string seeds;      // "142 seeds", empty for direct links
    mutable CachedText text_cache_; // the fitted name/detail/tags lines
};

struct DetailContent
{
    Title title;
    std::string directors;
    std::string writers;
    std::string cast;
    // Individual names behind the comma-joined lines above, for the focusable
    // person search on the detail page (populated by Session::detail_fill_title).
    std::vector<std::string> director_names;
    std::vector<std::string> cast_names;
    // Wired by the session to open Search for a person; invoked by the Detail
    // UI when a focused cast/director name is confirmed.
    std::function<void(const std::string &)> on_person_search;
    bool series = false;
    std::vector<std::string> seasons; // "Season 1", ...
    int season = 0;
    std::vector<Episode> episodes;
    std::vector<Stream> streams;
    std::string stream_heading; // "Streams", or the episode they are for
    // Addon tabs for the stream list: {"All", "Torrentio", ...} when >= 2 addons
    // have streams. L1/R1 switches tabs.
    std::vector<std::string> stream_addons;
    int stream_addon = 0; // active tab index into stream_addons (0 = All)
    // What the lists say while they are empty: "Loading streams...", "No streams found".
    std::string episodes_status;
    std::string streams_status;
    // Asks the page to put its episode cursor on `episode_hint` when this changes.
    int episode_hint = 0;
    int hint_serial = 0;
    // Asks the page to show the streams of the episode that was just loaded, when this changes.
    int streams_serial = 0;
};

struct PlayerState
{
    std::string title;
    std::string subtitle;   // "S2 E5  ·  The Long Night"
    double position = 0.0;  // seconds
    double duration = 0.0;
    double buffered = 0.0;  // seconds, how far ahead is ready
    bool paused = false;
    int buffering = -1;     // percent while stalled, negative when playing
    std::string net;        // "8.4 MB/s  ·  32 peers"
    std::string audio;      // "English  5.1"
    std::string subs;       // "Greek", "Off"
    std::string line;       // the subtitle on screen
    int sub_an = 2;         // ASS \an alignment (numpad 1-9), 2 = bottom-center
    int sub_fg = -1;        // whole-cue subtitle color 0xRRGGBB, -1 = default
    std::string line2;      // secondary subtitle cue on screen (dual subs, top)
    int sub2_an = 2;        // ASS \an alignment for the secondary cue
    int sub2_fg = -1;       // whole-cue color for secondary, -1 = default
    bool controls = true;   // shown on a button press, hidden after a few seconds
    bool menu = false;      // the track menu
    std::vector<std::string> audio_tracks;
    std::vector<std::string> subtitle_tracks;
    int audio_active = 0;
    int subtitle_active = 0;
    int subtitle2_active = 0; // secondary pick, index into subtitle_tracks (0 = Off)
    int menu_column = 0;
    int menu_row = 0;
    std::vector<std::string> speed_options; // "0.5x".."2x" in the track menu
    int speed_active = 1;                   // index into speed_options
    std::vector<std::string> skip_options;  // "Refresh skip timings" in the track menu
    int skip_active = -1;                   // no persistent selection (action row)
    std::string stats;           // "H264 1920x800 (hardware) · 24 fps · buffered 12.0s"
    bool nerd_overlay = false; // R3: toggleable stats-for-nerds panel
    std::vector<std::string> nerd_lines; // Player::nerd_stats(), when the overlay is on
    std::string upnext_title;    // "S2 E6  ·  The Long Night": empty = no up-next card
    double upnext_remaining = -1; // seconds on the up-next countdown, < 0 = no countdown number
    bool binge_prompt = false;   // "Still watching?" centered card
    double binge_left = 0;       // seconds until it gives up and stops
    int sub_size = 1;            // 0 small, 1 medium, 2 large
    int sub_color = 0;           // 0 white, 1 yellow
    int sub_bg_opacity = 0;        // dark box behind each subtitle line, 0-100 (0 = none)
    bool sub_raised = false;     // subtitles sit higher above the bottom edge
    std::string subtitle_delay;  // "+0.25 s"
    std::string audio_delay;     // "+0.05 s" (lip-sync)
    double seek_preview = -1;    // seek-target bubble time, -1 = hidden
    double seek_shimmer = 0.0;   // directional shimmer over the held frame while a seek
                                // catches up: -1 seeking back, +1 seeking forward, 0 none
    std::string ends_at;         // "Ends at 23:42", empty when duration unknown
    std::string xray_cast;       // "Christopher Nolan  ·  Cillian Murphy, Emily Blunt..."
    std::vector<std::string> badges; // active signal badges: {"4K", "HDR10", "HW Decode", ...}
    std::vector<aniskip::SkipSegment> skip_segments; // intro/outro bands on the seekbar
    std::string skip_prompt;     // "Skip intro" / "Skip outro", empty = no prompt
    int volume_osd = -1;         // 0-100 volume indicator, -1 = hidden
};

// Starting a stream: what is being started, how far it has got, and what the
// torrent engine is doing.
struct LaunchState
{
    std::string title;
    std::string subtitle; // "S2 E5  Â·  The Long Night" for an episode
    std::string source;   // the addon: "Stream Finder"
    std::string quality;  // "4K  Â·  HDR10  Â·  21.3 GB"
    std::vector<std::string> stages; // "Finding sources", "Connecting to peers", ...
    int stage = 0;                   // the one in progress; earlier ones are done
    float progress = -1.0f;          // how full the buffer is, negative when not known yet
    bool torrent = true;
    std::string peers; // "32"
    std::string speed; // "8.4 MB/s"
    std::string done;  // "12%"
    std::string hash;  // the torrent's info hash
    std::uint32_t backdrop = 0;
    std::uint32_t poster = 0;
    std::uint32_t logo = 0; // the title's logo art (wide transparent PNG); 0 = none
    int logo_w = 0, logo_h = 0;
};
struct SearchContent
{
    std::string query;
    BoardContent results; // rows: "Movies", "Series"
};

// The five pages of the left rail, in Stremio's order.
enum class Page
{
    board,
    discover,
    library,
    addons,
    settings,
};

} // namespace sx
