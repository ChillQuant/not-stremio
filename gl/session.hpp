// The app's state and everything that goes out to the network for it: the
// account, the addons, the catalogs, the library, a title's details and
// streams. It turns all of that into the plain content the screens draw
// (ui/content.hpp) and changes it in place as answers arrive; the shell reads
// what changed and tells the screens.
//
// One thread owns it (the one that draws). Blocking work runs on the task
// pool and its answers are delivered back to that thread in update().

#pragma once

#include "a11y.hpp"
#include "art.hpp"
#include "aniskip.h"
#include "content.hpp"
#include "gpu_convert.hpp"
#include "upscale.hpp"
#include "ime.hpp"
#include "phone_ime.hpp"
#include "model.hpp"
#include "search_index.hpp" // SEARCH UX (worker 5): the local prefix index
#include "overlays.hpp"
#include "player.h"
#include "touchpad.hpp"
#include "subtitles.h"
#include "tasks.h"
#include "audio/cues.hpp"
#include "core/input.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace sx
{

// How a stream from an addon is listed: resolution badge, size, seeds, tags.
Stream stream_view(const ::Stream &stream);

class Session
{
  public:
    // Where the cursor is on each screen, so the artwork around it is fetched first.
    struct Cursor
    {
        int board_row = 0, board_col = 0;
        int discover = 0;
        int library = 0;
        int episode = 0;
        int search_row = 0, search_col = 0;
    };

    // What changed since take_changed() last said. kDiscoverNew / kLibraryNew mean
    // the list is a different one (a filter changed): the screen starts at the top.
    enum Changed : unsigned
    {
        kBoard = 1,
        kDiscover = 2,
        kLibrary = 4,
        kAddons = 8,
        kSettings = 16,
        kDetail = 32,
        kDiscoverNew = 64,
        kLibraryNew = 128,
        kSearch = 256,
        kSearchNew = 512,
    };

    bool init(const std::string &base_dir, const std::string &data_dir, hui::gfx::Renderer *renderer);
    void shutdown();
    void update(float dt, const Cursor &cursor);
    // The display's real size (main.cpp): 4K video output needs a 2160p mode.
    void set_display_size(int w, int h);

    unsigned take_changed()
    {
        const unsigned c = changed_;
        changed_ = 0;
        return c;
    }
    // A page the app wants shown (after "See all" on the Board).
    bool take_goto(Page *page)
    {
        if (!goto_)
            return false;
        *page = *goto_;
        goto_.reset();
        return true;
    }
    // Grid-restore handoff for the shell (nav_memory.hpp): page_opened()
    // stashes the entered page's restored grid position; the shell takes it
    // once and moves the grid cursor. For flat grids (discover/library) row
    // is 0 and col is the flat item index; the shell converts with its
    // column count and clamps to the loaded items.
    bool take_restored_grid_pos(Page *page, int *row, int *col)
    {
        if (!has_restored_)
            return false;
        has_restored_ = false;
        *page = restored_page_;
        *row = restored_row_;
        *col = restored_col_;
        return true;
    }
    // The sounds the screens should make, in the order things happened (the shell plays them).
    void cue(hui::audio::Cue c)
    {
        if (cues_.size() < 16)
            cues_.push_back(c);
    }
    std::vector<hui::audio::Cue> take_cues()
    {
        std::vector<hui::audio::Cue> v;
        v.swap(cues_);
        return v;
    }
    bool ui_sounds() const
    {
        return prefs_.ui_sounds;
    }
    bool hdr_probe_enabled() const
    {
        return prefs_.hdr_probe;
    }
    bool wants_exit() const
    {
        return exit_;
    }
    // ---- OLED screensaver ----
    // Any user input: reset the idle timer. Called from the action entry
    // points below and on cursor moves in update().
    void note_input();
    // 0 = off, 1 = dimming (120s idle), 2 = pure-black drifting logo (300s).
    int screensaver_phase() const
    {
        return saver_phase_;
    }
    // 0..1 black overlay alpha for phase 1 (1.0 in phase 2).
    float screensaver_dim() const;
    // Drifting logo position, 0..1 across the screen (phase 2).
    float screensaver_logo_x() const;
    float screensaver_logo_y() const;
    // True while the first wake input must be consumed, not routed to the UI.
    // The shell checks this before routing input; on true it calls
    // screensaver_wake() and drops the input.
    bool screensaver_wake_armed() const
    {
        return saver_wake_armed_;
    }
    void screensaver_wake();

    // ---- what the screens draw ----
    BoardContent board;
    DiscoverContent discover;
    LibraryContent library;
    AddonsContent addons;
    SettingsContent settings;
    // WORKER 7 (SETTINGS): most-recently-changed SettingIndex values, newest
    // first, capped at 5; persisted in prefs. recent_shown_ maps each visible
    // Recent slot back to its source row for activation.
    std::deque<int> recent_order_;
    std::vector<int> recent_shown_;
    DetailContent detail;
    BoardContent search; // the results of a search, rows like the Board's
    std::string search_query;
    bool search_open = false; // the results are on show
    std::string clock; // "21:45"

    // ---- playing ----
    // The launch screen while a stream starts, then the player over the video.
    bool launch_open = false;
    LaunchState launch;
    bool watching = false; // the player is up (also during the launch screen)
    PlayerState playing;
    std::uint32_t video_texture = 0; // the current picture, 0 until the first one
    int video_w = 0, video_h = 0;
    uint64_t video_gen_ = 0;  // last uploaded picture gen; skip re-uploads
    VideoUpscaler upscaler_; // FSR1/Anime4K upscaling for sub-display-res video
    bool upscaler_tried_ = false, upscaler_ok_ = false;
    // Cache: gate the upscaler on new video frames (it only needs to run once
    // per source frame or param change, not on every 60-120Hz UI frame).
    mutable std::uint32_t upscaled_tex_ = 0;
    mutable std::uint32_t upscaled_src_tex_ = 0; // source video_texture the cached upscale was made from
    mutable uint64_t upscaled_gen_ = 0;
    mutable int upscaled_src_w_ = 0, upscaled_src_h_ = 0;
    mutable int upscaled_dst_w_ = 0, upscaled_dst_h_ = 0;
    mutable int upscaled_uw_ = 0, upscaled_uh_ = 0;
    mutable UpscaleMode upscaled_mode_ = UpscaleMode::Off;
    mutable float upscaled_sharp_ = -1.0f;
    std::vector<std::uint8_t> cpu_rgba_; // upload_video's CPU fallback buffer: reused across frames
    GpuYuv gpu_yuv_;  // YUV->RGB on the GPU; falls back to the CPU path
    bool gpu_yuv_tried_ = false, gpu_yuv_ok_ = false;
    bool film_mode_ = false;  // the display is at 119.88 Hz from film mode; restored on genuine stops,
                            // kept across episode transitions (the next playback re-decides once fps is known)
    bool film_mode_checked_ = false; // per-playback: the one-time film-mode fps decision ran (watch_update)
    int display_w_ = 1920, display_h_ = 1080; // real display size, from main.cpp
    bool typing() const
    {
        return ime_.active();
    }

    // ---- pop-ups ----
    bool dropdown_open = false;
    DropdownState dropdown;
    bool sign_in_open = false;
    SignInState sign_in;
    bool toast_open = false;
    ToastState toast;

    // ---- what the screens ask for ----
    void page_opened(Page page);
    // streams_video: for a series, the episode whose streams are shown at once (Continue Watching).
    void open_title(const Title &title, const std::string &streams_video = std::string(), bool force_detail = false);
    void close_detail();
    void detail_pick_episode(int index);
    void detail_change_season(int delta);
    void detail_toggle_watched(int index);
    void detail_more_actions(); // Options-button menu: mark series watched, remove from library
    void detail_mark_series_watched();
    void detail_confirm_remove_library();
    void detail_remove_library();
    void detail_play(int index, bool with_server);
    void detail_change_stream_addon(int delta); // L1/R1: switch stream source addon tab
    // Play-button focus: the detail page's focused stream row, for
    // speculative stream-URL resolution (session_watch.cpp). Called from
    // the shell's detail routing; -1 when the focus leaves the list.
    void note_stream_focus(int index);
    void cancel_autoplay()
    {
        autoplay_pending_ = false;
        autobest_pending_ = false;
    }
    void filter_activate(Page page, int filter);
    void settings_activate(int row);
    void settings_nudge_slider(int dir); // Left/Right on a Kind::slider row: ±5
    // WORKER 7 (SETTINGS): Left/Right on a step_inline value row cycles the
    // value directly (no choice list). True when the row stepped.
    bool settings_step_row(int row, int dir);
    // WORKER 7: records a changed setting for the "Recent" section (newest
    // first, no duplicates, capped).
    void settings_touch(int row);
    // WORKER 7: subtitle style preset (Default/Large/High contrast), derived
    // from the existing sub_size/sub_color/sub_bg_opacity/sub_pos prefs.
    int sub_style_current() const;
    void settings_apply_sub_style(int i);
    // WORKER 7: cross-worker helper for the playback Up-menu (WORKER 3 owns
    // gl/session_watch.cpp and calls this from watch_build_menu()).
    struct QuickSettingRow
    {
        std::string label; // "Subtitle size"
        std::string value; // "Large" (or "On"/"Off" for toggles)
        int setting_row;   // SettingIndex: pass to settings_activate()/settings_step_row()
        bool step_inline;  // Left/Right on this row cycles the value (1 press)
    };
    std::vector<QuickSettingRow> quick_settings_rows();
    // WORKER 7: per-title audio delay with fallback to the global pref.
    // WORKER 3 adopts these in session_watch.cpp (nudge_audio_delay and the
    // player_.set_audio_delay call in watch_start).
    double audio_delay_for(const std::string &key) const;
    void set_audio_delay_for(const std::string &key, double v);
    // WORKER 7: cross-worker: defined by WORKER 8 (ACCESSIBILITY) in
    // gl/a11y_settings.cpp; called at the end of refresh_settings().
    void a11y_build_rows();
    void addons_reload();
    // Cross on an addon: options menu (Enable/Disable, Move up/down, Uninstall for local ones).
    void addon_activate(int index);
    // Whether the user disabled this addon on the Addons page (skipped for streams/meta/catalogs).
    bool addon_disabled(const ::Addon &a) const;
    // Re-sort addons_ into the user's saved priority order.
    void apply_addon_order();
    // Sub-1s boot: restore addons from disk (addons_cache.json) so the board
    // can build before the network addons answer; written after each load.
    bool addons_cache_load();
    void addons_cache_save() const;
    // Square on the Board: "See all" (a catalog opens in Discover, Continue Watching in the Library).
    void board_square(int row, int col);
    // Whether a row of the Board is Continue Watching.
    bool board_is_continue(int row) const;
    // Cross on a card of Continue Watching: what was playing plays again (after asking), or the
    // streams of the next episode are shown.
    void continue_press(int row, int col);
    // Continue Watching asks for a title's page to be opened (the shell does it).
    bool take_open(Title *title, std::string *streams_video)
    {
        if (!open_req_)
            return false;
        *title = open_req_->first;
        *streams_video = open_req_->second;
        open_req_.reset();
        return true;
    }
    // Options on a card of Continue Watching: a menu, then a question, before it is removed.
    // False when the card has no options (the caller then treats the button as search).
    bool board_options(int row, int col);
    // True once the first screen has its catalogs and the posters in view (or has waited long enough).
    bool boot_ready() const
    {
        return boot_done_;
    }
    // ---- board snapshot (stale-while-revalidate, gl/board_cache.cpp) ----
    // Loads the last successfully loaded board rows from disk and paints them
    // immediately, before any network call. False on first-ever launch (or a
    // corrupt/stale file): nothing is cached yet.
    bool board_snapshot_load();
    // Writes the current board rows to disk. Called at the end of
    // refresh_board() on the UI thread; a no-op unless the board is fully
    // loaded, so a partial load is never persisted.
    void board_snapshot_save();
    // "Updated 3h ago" while the snapshot is what's on screen; empty once
    // fresh catalogs have answered.
    const std::string &board_snapshot_note() const
    {
        return board_snapshot_note_;
    }
    void dropdown_confirm();
    void dropdown_cancel();
    void sign_in_cancel();
    // The buttons while a stream starts or plays.
    void watch_input(const hui::InputFrame &input);
    // Asks for a search with the console's keyboard.
    void begin_search();
    void start_search(const std::string &query); // searches for a text already typed
    void close_search();
    void say(const std::string &message, double seconds = 4.0, bool error = false);
    // Video zoom for the shell's quad: "fit" (whole picture) or "fill" (crop to screen).
    const std::string &aspect_mode() const
    {
        return prefs_.aspect_mode;
    }
    void discover_load_more(); // next page, if there is one and none is loading (called from UI)
    // ---- multi-account profiles ----
    // Up to 10 stored accounts. Switching clears and reloads account-scoped
    // state (library, addons). Cross on a profile switches to it; the
    // "add"/"remove" entries manage the list.
    int profile_count() const;
    std::string profile_name(int i) const; // display name for settings/dropdown
    int active_profile() const;
    void profile_add();    // start sign-in flow that adds (not replaces)
    void profile_switch(int i);
    void profile_remove(int i); // two-step confirm, like sign out
    void profile_dropdown();    // account row: switch/add/remove via dropdown
    // ---- type from phone ----
    // URL of the phone typing page while a text field is open, else empty.
    std::string phone_ime_url() const;
    std::uint32_t qr_texture(const std::string &url) const;
    // Video upscaler settings for the shell's draw path.
    UpscaleMode upscaler_mode() const
    {
        return static_cast<UpscaleMode>(prefs_.upscaler);
    }
    float upscaler_sharpness() const
    {
        return static_cast<float>(prefs_.upscaler_sharpness);
    }
    int display_width() const { return display_w_; }
    int display_height() const { return display_h_; }

  private:
    template <typename T> void bg(std::function<T()> work, std::function<void(T &)> done);
    template <typename T> void bg_urgent(std::function<T()> work, std::function<void(T &)> done);

    // persistence and account
    void load_prefs();
    void save_prefs();
    void load_progress();
    void save_progress();
    bool signed_in() const
    {
        return !prefs_.auth_key.empty();
    }
    void load_addons();
    void on_addons_loaded();
    void load_library();
    const ::Addon *find_addon(const std::string &url) const;
    std::vector<Item> continue_watching() const;

    // board
    void build_board();
    void refresh_board();
    void hero_meta_check(const Cursor &cursor); // fetch full meta for the focused Board title
    void continue_remove(const Item &item);
    void resume_play(const Title &title, const Item &item);
    void start_remembered_stream();
    bool saved_stream(const std::string &item_id, const std::string &video_id, ::Stream *out, bool allow_any_episode = false) const;
    std::string next_episode_id() const;
    bool boot_check(double seconds) const;
    // discover
    void enter_discover();
    void navmem_save_page(Page page);    // NavMem::save_grid_pos for the page being left
    void navmem_restore_page(Page page); // NavMem::restore_grid_pos -> stash for the shell
    void discover_build_chips();
    void discover_load(bool more);
    void discover_sort_items(); // re-sort loaded items by prefs_.disc_sort
    // library
    void refresh_library();
    // addons, settings
    void refresh_addons();
    void refresh_settings();
    // Accessibility activation (worker 8, gl/a11y_settings.cpp): handles Cross
    // on the rows a11y_build_rows() appends (worker 7 hooks it from
    // settings_activate()).
    bool a11y_activate(int row);
    int a11y_row_base_ = -1; // first appended accessibility row, -1 before the first build
    // Cache accounting for the Storage section: art/ + stream buffers + log.
    std::uint64_t cache_size() const;
    void clear_cache();
    // The directory walk is slow: it runs on a worker, refresh_settings()
    // only reads the last value.
    void refresh_cache_size_bg();
    std::atomic<std::uint64_t> cache_size_cached_{0};
    std::atomic<bool> cache_size_updated_{false}; // bg walk finished: refresh the row
    std::atomic<std::uint64_t> cache_walk_seq_{0}; // newer walk discards older results
    void open_dropdown(const std::string &title, const std::vector<std::string> &options, int active,
                       std::function<void(int)> chosen, std::vector<std::string> descriptions = {});
    void login_start();
    void login_poll();
    void login_done(const std::string &auth_key);
    void sign_out();
    // detail
    void detail_apply_meta();
    void detail_set_season(int index);
    void detail_load_streams(const std::string &video_id, const std::string &heading);
    void detail_refresh_streams();
    // Instant Season-Pack Autoplay: remember the playing stream's fingerprint,
    // then pick the matching stream for the next episode.
    void arm_autoplay_from(const ::Stream &s);
    int pick_autoplay_stream(const std::vector<::Stream> &ranked, bool all_answered) const;
    void detail_preresolve_top_stream(); // Worker 9 (network): page-open stream-resolution trigger
    void detail_fill_title();
    bool watched(const std::string &video_id) const;
    // artwork
    void refresh_art(const Cursor &cursor);

    // search and typing
    void ask_text(const std::function<void(const std::string &)> &done);
    void refresh_search();
    void poll_typing();

    // playing (session_watch.cpp)
    struct WatchSub
    {
        std::string label, lang;
        int embedded = -1;      // stream index in the file, or
        std::string url;        // an addon's subtitle file
        std::vector<Cue> cues;  // once loaded
        bool loading = false, failed = false;
    };
    void play_stream(const ::Stream &stream, bool transcode, bool is_transition = false);
    // A stream's playable URL, resolved off the UI thread (torrent engine /
    // server /create for torrents, direct for HTTP). Shared by the play
    // path, Play-focus speculation and the next-episode pre-resolve.
    struct StreamRes
    {
        std::string url, error;
        std::vector<std::string> headers;
        int file_idx = -1;
    };
    StreamRes resolve_stream(const ::Stream &st, const std::string &srv, bool torrent, bool builtin, bool transcode,
                             int season, int episode, std::atomic<bool> *cancel);
    static std::string resolve_key(const ::Stream &st, bool transcode, bool builtin);
    // Speculative resolution: Play-button focus (and the 80% pre-resolve)
    // resolve here; play_stream takes the result instead of resolving again.
    void spec_resolve_begin(const ::Stream &st, int season, int episode, double ttl);
    bool spec_resolve_take(const ::Stream &st, bool transcode, bool builtin, std::atomic<bool> *cancel,
                           StreamRes &out);
    void spec_resolve_cancel();
    void watch_preresolve_next();
    void watch_start(const std::string &url, const std::vector<std::string> &headers, double start,
                     const std::string &title, const std::string &subtitle, bool direct);
    void watch_update();
    void watch_stop(bool ended, bool keep_frame = false);
    void watch_save_progress(bool final, bool force_finished = false);
    void watch_show_info();
    void nudge_sub_delay(double delta); // ±0.1 s steps, clamped to ±60 s, OSD feedback
    void nudge_audio_delay(double delta); // ±0.05 s steps, clamped to ±0.5 s, persisted, OSD feedback
    void watch_seek_by(double delta); // D-pad/L1/R1 seek with a target-time preview bubble
    void watch_build_menu();
    void watch_select_sub(int index);
    void watch_select_sub2(int index); // secondary subtitle (dual subs)
    void watch_ensure_sub_cues(int index); // lazy-load a WatchSub's cues (USB or download)
    void watch_scan_usb_subs(); // list /mnt/usb0 + /mnt/usb1 for subtitle files
    void watch_load_addon_subtitles();
    void watch_auto_subtitles();
    static int sub_auto_rank(const WatchSub &s, const std::string &lang);
    // Per-show track memory: the id whose picks are remembered (series/movie),
    // saving manual choices and applying them to later episodes.
    std::string watch_memory_key() const;
    void watch_remember_audio(const std::string &lang);
    void watch_remember_sub();
    // Enforce the 200-entry cap at insert time (the save loop only caps
    // what it writes, which let the in-memory map grow without bound).
    void watch_prune_track_memory();
    void prune_progress(); // cap progress_ like track_memory: the save below must stay bounded
    std::mutex save_m_;    // serializes progress.json writes: the bg worker and
                           // the synchronous shutdown write share one tmp path
    void watch_apply_remembered_audio();
    // Intro/outro skip: one lookup per episode (worker thread), the
    // per-frame position check, and the segment under a position.
    //
    // SkipRequest is the single point where a playback is resolved to a
    // backend query. It consolidates series-ID resolution (item → video →
    // meta), season/episode resolution (meta → video-ID parse), backend
    // selection, and cache-key construction. One builder, one result.
    struct SkipRequest
    {
        bool valid = false;      // resolution succeeded; safe to dispatch
        std::string diag;        // why not (nerd-stats diagnostic)
        std::string backend;     // "tidb" or "aniskip"
        std::string series_id;   // canonical ID for the backend
        bool is_movie = false;
        int season = 0;          // tidb series only
        int episode = 0;         // tidb series (>=1) or aniskip (>=1)
        std::string key;         // cache key: backend:id[:season:episode]
        bool defer = false;      // metadata not ready; retry when it arrives
    };
    SkipRequest watch_build_skip_request();
    void watch_start_skip_lookup();
    void watch_dispatch_skip(const SkipRequest &req, bool bypass_cache);
    void watch_refresh_skip(); // manual refresh: bypass cache, refetch now
    void watch_check_skip(double pos, double dur);
    int watch_skip_at(double pos) const;
    // Triangle on a visible Skip prompt: jump past the segment. True when
    // the press was consumed (prompt visible and position inside a segment).
    bool watch_try_skip_prompt();
    void watch_next_episode();
    // True when watch_next_episode() will take the pre-resolved fast path:
    // the next episode's winning stream is already scored and cached, so the
    // transition can stay inside the player (no launch modal, frame held).
    bool watch_next_fast_ready() const;
    // The up-next countdown hit zero, or the Next button was pressed: roll
    // into the next episode through one shared transition.
    void watch_upnext_fire(bool manual = false);
    // An automatic advance is about to happen: the binge guard intercepts it
    // with the "Still watching?" prompt once the threshold is reached.
    // Returns true when it intercepted (no advance happened).
    bool watch_binge_check();
    const ::Video *next_episode() const; // the episode after the current one, or nullptr
    static std::string episode_heading(const ::Video &v);
    bool upnext_active() const { return !w_upnext_heading_.empty() || w_upnext_until_ > 0; }
    void upload_video();
    void torrent_stats_start(const std::string &url, const std::string &info_hash);
    void torrent_stats_poll();
    std::vector<std::string> pref_langs(const std::string &list) const;
    std::string server() const;

    Title make_title(const Item &item, bool with_progress) const;
    // The library's items carry no backdrop or description: both are asked of the
    // addons for the title under the cursor, kept here and filled in everywhere.
    void apply_meta(Item &item) const;
    void want_meta(const Item &item);

    std::string base_dir_, data_dir_;
    hui::gfx::Renderer *renderer_ = nullptr;
    mutable TexCache tex_; // mutable: QR/phone textures are fetched from const draw paths
    Prefs prefs_;
    std::map<std::string, Progress> progress_;
    std::set<std::string> watched_;
    unsigned changed_ = 0;
    Page page_ = Page::board; // the page on show
    std::optional<Page> goto_;
    bool exit_ = false;
    bool shutdown_ = false; // shutdown() ran: guards against a double call
    std::vector<hui::audio::Cue> cues_;
    std::optional<std::pair<Title, std::string>> open_req_;
    std::string want_streams_video_; // open_title: show these streams as soon as the details are in
    bool resume_play_ = false;       // open_title: play the remembered stream as soon as the details are in
    bool force_detail_ = false;      // open_title: suppress auto-play-best, always show the Detail page
    bool resuming_ = false;          // the launch screen is up before play_stream has been called
    bool resumed_from_board_ = false; // the title's page is open behind the Board only for playing
    std::string resume_video_;
    double clock_ = 0;
    Cursor last_cursor_;
    // Scroll direction for asymmetric artwork prefetch: +1 when the last
    // move went down/right, -1 when up/left (refresh_art() biases ahead).
    int scroll_dir_col_ = 1;
    int scroll_dir_row_ = 1;
    double last_art_ = 0;
    bool art_dirty_ = true;
    double toast_until_ = 0;

    // addons and library
    std::vector<std::unique_ptr<::Addon>> addons_;
    bool addons_loading_ = false;
    bool board_loading_ = false; // some catalog of the Board has not answered yet
    bool boot_done_ = false;
    double boot_start_ = 0.0;
    int addons_gen_ = 0;
    std::map<std::string, json> library_; // library items by _id
    bool library_loaded_ = false;

    // details of titles the library listed without them, by "type:id"
    std::map<std::string, Item> meta_cache_;
    std::set<std::string> meta_asked_;
    // Episode thumbnails, by "type:id" then video id: the Continue Watching
    // row shows the episode's own art (Title::episode_art), not just the
    // show poster. Filled by want_meta() next to meta_cache_; evicted with it.
    std::map<std::string, std::map<std::string, std::string>> meta_ep_thumbs_;
    // Video (episode) lists per meta key, by "type:id": filled by want_meta()
    // / hero_meta_check() next to meta_cache_; evicted with it.
    std::map<std::string, std::vector<::Video>> meta_videos_;
    // Speculative detail preload (want_meta): the focused card must dwell
    // 300ms before its meta is fetched; a new focus restarts the dwell.
    std::string meta_dwell_key_;
    double meta_dwell_since_ = 0;
    // Grid-restore handoff (nav_memory.hpp): page_opened() saves the page
    // being left and stashes the entered page's restored position for
    // take_restored_grid_pos().
    Page restored_page_ = Page::board;
    int restored_row_ = -1, restored_col_ = -1;
    bool has_restored_ = false;

    // board
    std::vector<BoardRow> board_;
    std::vector<int> board_map_; // content row -> board_ index
    int home_gen_ = 0;
    std::string board_snapshot_note_; // "Updated 3h ago" while the snapshot is on screen
    // Hero meta: the focused Board title's full details (description, background, logo).
    // Catalogs only carry name+poster; the hero fetches meta in the background.
    std::string hero_meta_key_; // type:id currently fetched (or empty)
    int hero_meta_gen_ = 0;
    // Hero-meta dwell gate: holding D-pad must rest 200ms on one card before
    // its meta is fetched; a new focus restarts the dwell.
    std::string hero_dwell_key_;
    double hero_dwell_since_ = 0.0;

    // discover
    struct DiscCatalog
    {
        const ::Addon *addon;
        const Catalog *catalog;
    };
    std::vector<std::string> disc_types_;
    std::string disc_type_;
    std::vector<DiscCatalog> disc_catalogs_;
    int disc_catalog_ = 0;
    std::string disc_genre_;
    std::vector<Item> disc_items_;
    std::unordered_set<std::string> disc_ids_; // O(1) dedupe for the infinite-scroll append
    bool disc_loading_ = false, disc_end_ = false;
    int disc_gen_ = 0;

    // library
    std::vector<Item> lib_items_;
    std::string lib_type_;
    int lib_sort_ = 0; // 0 recent, 1 A-Z, 2 most watched

    // dropdown
    std::function<void(int)> dropdown_chosen_;

    // sign-in
    std::string login_code_raw_, login_qr_url_;
    double login_next_poll_ = 0, login_expires_ = 0;
    bool login_polling_ = false;
    int login_gen_ = 0;
    bool confirm_sign_out_ = false;
    bool confirm_clear_cache_ = false;

    // detail
    Item d_item_;
    ::Meta d_meta_;
    bool d_meta_loaded_ = false;
    std::vector<int> d_seasons_;
    int d_season_idx_ = 0;
    std::vector<const ::Video *> d_season_videos_;
    std::string d_video_id_;
    std::string d_stream_heading_;
    std::vector<::Stream> d_streams_; // in the order the screen shows them (after addon-tab filter)
    std::vector<::Stream> d_all_streams_; // full ranked list, before addon-tab filter
    std::vector<::Stream> d_found_;   // as the addons sent them
    int d_stream_addon_ = 0; // active stream addon tab (0 = All)
    int d_gen_ = 0, d_pending_ = 0, d_answered_ = 0, d_meta_gen_ = 0;
    bool d_open_ = false;
    std::string prewarmed_key_; // url/info_hash of the stream already pre-warmed
    int w_stream_idx_ = -1;       // index in d_streams_ currently being tried (-1 if not from d_streams_)
    int w_failover_tries_ = 0;    // automatic failovers run for this play request
    static constexpr int kMaxFailoverTries = 2; // up to 3 streams total (initial + 2 failovers)
    bool try_failover_stream(const std::string &why);
    // Speculative stream-URL resolution (Play-button focus, 80% pre-resolve):
    // one in-flight or freshly finished operation, keyed by stream identity.
    // Play takes a fresh-enough match instead of resolving again; a focus
    // change or a new playback cancels/supersedes it. Only the worker
    // threads touch the op; the UI thread only swaps the shared_ptr.
    struct SpecOp
    {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        StreamRes res;
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    };
    std::mutex spec_m_;
    std::string spec_key_;
    std::shared_ptr<SpecOp> spec_op_;
    double spec_at_ = 0;      // wall time the result landed
    double spec_ttl_ = 30.0;  // how long the result stays usable
    std::atomic<int> spec_gen_{0}; // bumped to abandon an in-flight op (UI thread writes, workers read)
    int spec_focus_idx_ = -1; // stream row the focus speculation is for, -1 = none
    std::string spec_focus_id_; // that row's stream identity (the list can re-sort under the cursor)
    // Speculative stream-URL prefetch: the focused stream row dwells 250ms
    // (update() checks), then spec_prefetch_stream() hands it to the watch
    // prefetch. The stub forwards to spec_prefetch_stream_impl(), whose real
    // logic lives in session_watch.cpp (Worker C).
    int spec_dwell_idx_ = -1;  // stream index in d_streams_ awaiting dwell, -1 = none
    double spec_dwell_since_ = 0; // wall time the dwell started
    void spec_prefetch_stream(int idx);      // entry point (stub in session.cpp)
    void spec_prefetch_stream_impl(int idx); // real prefetch (session_watch.cpp)

    // search
    Ime ime_;
    std::function<void(const std::string &)> text_done_;
    std::vector<BoardRow> search_;
    std::vector<int> search_map_;
    int search_gen_ = 0, search_pending_ = 0;
    // ---- SEARCH UX (worker 5): local prefix index + debounced network search ----
    void search_index_ensure(); // bg (re)build when the on-device data changed
    void search_show_trending(); // instant content when the search page opens
    void search_local_results(const std::string &q); // instant on-device row, no network
    void search_launch_network(const std::string &q, int gen,
                               const std::shared_ptr<std::atomic<bool>> &cancel);
    static int search_rank_score(const Item &it, const std::string &ql,
                                 const std::unordered_set<std::string> &owned);
    std::vector<std::string> recent_search_queries(); // pruned, most recent first (cap 5)
    void recent_search_add(const std::string &q); // dedupe, cap 5, 30-day expiry, persists
    void recent_search_clear();
    std::shared_ptr<const SearchIndex> search_index_; // swapped on the UI thread only
    std::uint64_t search_index_fp_ = 0; // fingerprint of the indexed sources
    bool search_index_building_ = false;
    std::shared_ptr<std::atomic<bool>> search_cancel_; // true cancels the in-flight network search
    int search_debounce_gen_ = 0; // a newer search drops a pending debounced launch
    Row search_local_row_; // the "On this device" row, kept above network rows
    bool search_local_have_ = false;
    std::vector<Item> search_local_items_; // source Items for the local row (trending or on-device),
                                           // index-aligned with search_local_row_.items for poster lookup
    std::unordered_set<std::string> search_owned_; // "type:id" in library/continue watching, for ranking

    // playing
    Player player_;
    Item w_item_;
    std::string w_video_id_;
    ::Stream w_stream_;
    std::vector<WatchSub> w_subs_;
    int w_sub_active_ = -1; // index in w_subs_, -1 off
    int w_sub2_active_ = -1; // secondary subtitle (dual subs), index in w_subs_, -1 off
    double w_sub_delay_ = 0;
    double w_seek_target_ = -1; // seek-preview bubble: target media time, -1 = hidden
    double w_seek_until_ = 0;   // ... hidden after this wall time even if the seek stalls
    bool w_nerd_ = false;         // R3: stats-for-nerds overlay
    bool w_was_buffering_ = false; // edge detector for the stall toast
    double w_info_until_ = 0, w_last_save_ = 0, w_last_saved_pos_ = -1;
    // Up-next countdown: offered in the last seconds of a series episode.
    std::string w_upnext_heading_;
    double w_upnext_until_ = 0; // wall-clock deadline for the unknown-duration case, 0 = inactive
    bool w_upnext_cancelled_ = false;
    // "Still watching?" binge guard: counts automatic advances since the
    // last user input; asks instead of advancing past the threshold.
    static constexpr double kBingeTimeoutSecs = 60.0; // no-input grace before giving up
    int w_binge_count_ = 0;
    bool w_binge_asking_ = false;
    bool w_binge_auto_ = false; // the pending play_stream is an automatic advance
    double w_binge_left_ = 0; // seconds until the prompt gives up
    double w_binge_prev_ = 0; // last watch_update time, for the countdown
    bool w_started_ = false, w_menu_built_ = false, w_auto_sub_done_ = false, w_heavy_warned_ = false;
    bool w_hw_warned_ = false; // the specific "why software decoding" toast fired for this stream
    bool w_hw_mid_warned_ = false; // the mid-stream hw-failure toast fired for this stream
    bool w_sub_auto_ = false; // the active subtitle was chosen by auto-select (safe to re-pick)
    bool w_menu_dirty_ = false;
    double w_stats_at_ = 0, w_stats_logged_ = 0;
    int w_gen_ = 0;
    std::shared_ptr<std::atomic<bool>> launch_cancel_;
    // Launch-screen watchdogs: the backdrop stays up until the first real
    // frame reaches the video surface (no black flash), and a resolved URL
    // that yields no data fails over instead of stalling silently.
    double w_launch_since_ = 0;      // wall time the launch screen came up
    double w_launch_prog_ = 0;       // last observed launch data progress
    double w_launch_stall_since_ = 0; // wall time that progress last advanced
    // Seamless Play-Next: while true the player holds the previous episode's
    // last frame (video_texture) instead of showing the launch modal, until
    // the next episode's first frame arrives. w_seamless_gen_ snapshots
    // video_gen_ at transition start so watch_update() can spot that frame.
    bool w_seamless_transition_ = false;
    int w_seamless_gen_ = 0;
    static constexpr double kLaunchFirstFrameWait = 5.0; // then give up waiting for a frame
    static constexpr double kLaunchStallFailover = 10.0; // no-data stall that fails over
    std::string w_preresolve_id_; // next-episode video id already pre-resolved, "" = none
    // Instant Season-Pack Autoplay: the 80% pre-resolve caches the winning
    // stream so the transition plays it immediately (no detail-page round trip).
    std::string w_next_preresolved_vid_;
    ::Stream w_next_stream_;
    bool w_next_stream_ready_ = false;
    // The pre-resolved next-episode stream's socket was re-warmed while the
    // up-next card was up (once per playback): the 80% pre-resolve's warm
    // socket can go cold before the episode ends.
    bool w_upnext_prewarmed_ = false;
    // Intro/outro skip (AniSkip): segments for the playing episode, the
    // episode they were fetched for, and which segment was already skipped.
    std::vector<aniskip::SkipSegment> w_skip_;
    std::string w_skip_key_; // series id + episode, so one lookup runs per episode
    int w_skip_done_ = -1;   // segment index already skipped this playback
    int w_skip_prompted_ = -1; // segment index the prompt was surfaced for (Prompt mode)
    bool w_skip_deferred_ = false; // skip lookup deferred: metadata wasn't ready at play time; retry in watch_update once d_meta_ arrives
    std::string w_skip_diag_; // nerd-stats diagnostic: why skip lookup failed ("no meta", "no data", etc.)
    std::shared_ptr<std::atomic<bool>> w_skip_cancel_;
    // DualSense touchpad swipe volume: reader + gesture state. The reader
    // opens its own scePad handle; if unavailable, swipes stay disabled.
    TouchpadReader w_touchpad_;
    bool w_touch_active_ = false; // finger currently on the pad
    int w_touch_last_y_ = 0;      // Y at the last poll (for delta)
    double w_vol_osd_until_ = 0; // volume indicator visible until (monotonic s)
    std::string subtitle_shown_;
    int subtitle_shown_an_ = 2;
    std::string subtitle2_shown_; // secondary cue change detection (dual subs)
    int subtitle2_shown_an_ = 2;
    // the torrent's statistics while it starts or buffers
    std::string t_stats_url_;
    int t_gen_ = 0;
    double t_next_poll_ = 0;
    bool t_polling_ = false, t_visible_ = false;
    int t_peers_ = 0, t_known_ = -1;
    double t_speed_ = 0, t_progress_ = -1;
    bool t_has_meta_ = false;
    // the next episode, picked once its streams are in
    bool autoplay_pending_ = false;
    std::string autoplay_binge_, autoplay_addon_;
    std::string autoplay_hash_, autoplay_res_, autoplay_name_; // season-pack fingerprint of the playing stream
    // auto-play best stream: play the best stream once its streams are in
    bool autobest_pending_ = false;

    // "See all" on the Board: which catalog Discover should open on.
    std::string see_all_addon_, see_all_catalog_, see_all_type_;
    std::vector<std::string> addon_logos_;
    // ---- OLED screensaver: idle tracking ----
    double idle_since_ = 0; // monotonic seconds of the last user input
    int saver_phase_ = 0;   // 0 off, 1 dimming (120s), 2 drifting logo (300s)
    double saver_t_ = 0;    // seconds spent in phase 2 (drives the drift path)
    double saver_dim_t_ = 0; // seconds spent in phase 1 (drives the dim ramp)
    bool saver_wake_armed_ = false;
    void touch_idle();
    void update_saver(float dt, double now);
    // ---- multi-account profiles ----
    bool login_as_new_profile_ = false; // login_start() is adding, not replacing
    bool confirm_remove_profile_ = false;
    int pending_profile_action_ = -1; // dropdown choice being confirmed
    void profile_sync_active();       // prefs_.auth_key/user_email <-> active profile
    void profile_reload_account_state(); // clear + reload library/addons after switch
    // ---- type from phone ----
    PhoneIme phone_ime_;
};

template <typename T> void Session::bg(std::function<T()> work, std::function<void(T &)> done)
{
    g_tasks.run<T>(std::move(work), std::move(done));
}

template <typename T> void Session::bg_urgent(std::function<T()> work, std::function<void(T &)> done)
{
    g_tasks.run_urgent<T>(std::move(work), std::move(done));
}

} // namespace sx
