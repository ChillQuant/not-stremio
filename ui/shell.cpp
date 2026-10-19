#include "shell.hpp"

#include "../gl/haptics.hpp"
#include "../gl/hints.hpp"

#include <algorithm>
#include <cmath>

namespace sx
{

using hui::Action;
using hui::Direction;

namespace
{
constexpr int kPageCount = 5;
}

Shell::Shell(const Env &env, Session &session)
    : env_(env), s_(session), board_(env), search_board_(env), discover_(env), library_(env), addons_(env), settings_(env),
      detail_(env), player_ui_(env)
{
    board_.set_content(&s_.board);
    search_board_.set_search(&s_.search_query);
    search_board_.set_content(&s_.search);
    discover_.set_content(&s_.discover);
    library_.set_content(&s_.library);
    addons_.set_content(&s_.addons);
    settings_.set_content(&s_.settings);
    go_page(0);
    // STEP 1 (Instant Board): hydrate the pages before the first frame, so the
    // Board is interactive as soon as it is presented.
    apply_changes();
}

Session::Cursor Shell::cursor() const
{
    Session::Cursor c;
    c.board_row = board_.cursor_row();
    c.board_col = board_.cursor_col();
    c.discover = discover_.cursor_index();
    c.library = library_.cursor_index();
    c.episode = detail_.episode_index();
    c.search_row = search_board_.cursor_row();
    c.search_col = search_board_.cursor_col();
    return c;
}

void Shell::go_page(int page)
{
    page_ = std::clamp(page, 0, kPageCount - 1);
    screen_ = Screen::page;
    switch (page_)
    {
    case 0:
        board_.enter();
        break;
    case 1:
        discover_.enter();
        break;
    case 2:
        library_.enter();
        break;
    case 3:
        addons_.enter();
        break;
    default:
        settings_.enter();
        break;
    }
    s_.page_opened(static_cast<Page>(page_));
}

void Shell::open_title(const Title &title, const std::string &streams_video, bool force_detail)
{
    came_from_ = screen_;
    s_.open_title(title, streams_video, force_detail);
    detail_.set_content(&s_.detail);
    detail_.enter();
    screen_ = Screen::detail;
}

// Back from a title's page: to the page or the search results it was opened from.
void Shell::leave_title()
{
    s_.close_detail();
    if (came_from_ == Screen::search && s_.search_open)
    {
        screen_ = Screen::search;
        search_board_.enter();
    }
    else
    {
        go_page(page_);
    }
}

// What the session changed since the last frame, passed on to the screens that show it.
void Shell::apply_changes()
{
    const unsigned c = s_.take_changed();
    if (c & Session::kBoard)
        board_.refresh();
    if (c & Session::kDiscoverNew)
        discover_.set_content(&s_.discover);
    else if (c & Session::kDiscover)
        discover_.refresh();
    if (c & Session::kLibraryNew)
        library_.set_content(&s_.library);
    else if (c & Session::kLibrary)
        library_.refresh();
    if (c & Session::kAddons)
        addons_.refresh();
    if (c & Session::kSettings)
        settings_.refresh();
    if (c & Session::kDetail)
        detail_.refresh();
    if (c & Session::kSearchNew)
    {
        search_board_.set_content(&s_.search);
        search_board_.enter();
        if (s_.search_open)
            screen_ = Screen::search;
        else if (screen_ == Screen::search)
            screen_ = Screen::page;
    }
    else if (c & Session::kSearch)
    {
        search_board_.refresh();
    }
    Title requested;
    std::string requested_streams;
    if (s_.take_open(&requested, &requested_streams))
        open_title(requested, requested_streams);
    Page page;
    if (s_.take_goto(&page))
    {
        sound_.play(hui::audio::Cue::tab);
        go_page(static_cast<int>(page));
    }
    // NavMem: restore the grid cursor to where the user left it on this page.
    Page rpage;
    int rrow = 0, rcol = 0;
    if (s_.take_restored_grid_pos(&rpage, &rrow, &rcol))
    {
        switch (rpage)
        {
        case Page::board:
            board_.set_cursor(rrow, rcol);
            break;
        case Page::discover:
            discover_.set_cursor(rcol);
            break;
        case Page::library:
            library_.set_cursor(rcol);
            break;
        default:
            break;
        }
    }
}

void Shell::route_detail(const hui::InputFrame &input, float dt)
{
    if (input.pressed != 0 || input.nav != Direction::none)
        s_.cancel_autoplay(); // any button cancels playing the next episode
    const bool streams = detail_.mode() == Detail::Mode::streams;
    const bool series = s_.detail.series;
    // Back leaves the page, except from a series' streams, where it goes back to the episodes.
    const bool leave = input.is_pressed(Action::back) && !(series && streams);
    if (streams)
    {
        if (input.is_pressed(Action::page_prev))
            s_.detail_change_stream_addon(-1);
        else if (input.is_pressed(Action::page_next))
            s_.detail_change_stream_addon(1);
        else if (input.is_pressed(Action::confirm))
            s_.detail_play(detail_.stream_index(), false);
        else if (input.is_pressed(Action::west))
            s_.detail_play(detail_.stream_index(), true);
    }
    else
    {
        if (input.is_pressed(Action::page_prev))
            s_.detail_change_season(-1);
        else if (input.is_pressed(Action::page_next))
            s_.detail_change_season(1);
        else if (input.is_pressed(Action::north))
            s_.detail_toggle_watched(detail_.episode_index());
        else if (input.is_pressed(Action::confirm) && !s_.detail.episodes.empty())
            s_.detail_pick_episode(detail_.episode_index());
        else if (input.is_pressed(Action::west) && !s_.detail.episodes.empty())
        {
            // Square on the episode list: choose the episode, cancel any
            // pending autoplay, and show its streams for a manual pick.
            s_.detail_pick_episode(detail_.episode_index());
            s_.cancel_autoplay();
            detail_.set_mode(Detail::Mode::streams);
        }
    }
    if (input.is_pressed(Action::menu))
        s_.detail_more_actions();
    detail_.update(input, dt);
    // PLAYBACK worker: speculative stream-URL resolution. When the focus
    // lands on a stream row, its playable URL starts resolving on a
    // background thread; the press that follows usually finds it ready.
    // Focus leaving the list abandons the speculation.
    s_.note_stream_focus(streams ? detail_.stream_index() : -1);
    if (leave)
        leave_title();
}

void Shell::route_search(const hui::InputFrame &input, float dt)
{
    search_board_.update(input, dt);
    const bool on_rail = search_board_.rail_focused();
    // L1/R1 leave Search and switch the five main pages (mirrors route_page).
    if (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next))
    {
        const int dir = input.is_pressed(Action::page_next) ? 1 : -1;
        const int next_page = (page_ + dir + kPageCount) % kPageCount;
        s_.close_search();
        sound_.play(hui::audio::Cue::tab);
        go_page(next_page);
        return;
    }
    if (input.is_pressed(Action::back))
    {
        s_.close_search();
        go_page(page_);
        return;
    }
    if (input.is_pressed(Action::north) || input.is_pressed(Action::menu))
    {
        s_.begin_search();
        return;
    }
    if (!input.is_pressed(Action::confirm))
        return;
    if (on_rail)
    {
        const int cursor = search_board_.rail_cursor();
        s_.close_search();
        sound_.play(hui::audio::Cue::tab);
        go_page(cursor);
    }
    else if (const Title *under = search_board_.focused_title())
    {
        open_title(*under);
    }
}

void Shell::route_page(const hui::InputFrame &input, float dt)
{
    const bool confirm = input.is_pressed(Action::confirm);
    bool on_rail = false;
    int cursor = 0;
    const Title *under = nullptr;
    // L1/R1 switch the five main pages (Board, Discover, Library, Addons,
    // Settings) from anywhere on a page. Handled here, before any
    // page-specific routing, so no page keeps its own L1/R1 binding.
    if (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next))
    {
        const int dir = input.is_pressed(Action::page_next) ? 1 : -1;
        const int next_page = (page_ + dir + kPageCount) % kPageCount;
        sound_.play(hui::audio::Cue::tab);
        go_page(next_page);
        return;
    }
    switch (page_)
    {
    case 0:
        board_.update(input, dt);
        on_rail = board_.rail_focused();
        cursor = board_.rail_cursor();
        under = board_.focused_title();
        if (!on_rail && input.is_pressed(Action::west))
            s_.board_square(board_.cursor_row(), board_.cursor_col());
        // Options on a Continue Watching card: remove it (after asking). Elsewhere it is search.
        if (!on_rail && input.is_pressed(Action::menu) && s_.board_options(board_.cursor_row(), board_.cursor_col()))
            return;
        break;
    case 1:
        discover_.update(input, dt);
        on_rail = discover_.rail_focused();
        cursor = discover_.rail_cursor();
        under = discover_.focused_title();
        if (confirm && discover_.filter_cursor() >= 0)
        {
            s_.filter_activate(Page::discover, discover_.filter_cursor());
            return;
        }
        // Infinite scroll: fetch the next page when the cursor reaches the
        // last row. discover_load_more() no-ops while loading or at the end.
        if (discover_.at_end())
            s_.discover_load_more();
        break;
    case 2:
        library_.update(input, dt);
        on_rail = library_.rail_focused();
        cursor = library_.rail_cursor();
        under = library_.focused_title();
        if (confirm && library_.filter_cursor() >= 0)
        {
            s_.filter_activate(Page::library, library_.filter_cursor());
            return;
        }
        break;
    case 3:
        addons_.update(input, dt);
        on_rail = addons_.rail_focused();
        cursor = addons_.rail_cursor();
        if (!on_rail && confirm)
            s_.addon_activate(addons_.cursor_index());
        if (!on_rail && input.is_pressed(Action::west))
            s_.addons_reload();
        break;
    default:
    {
        // Slider rows: Left/Right adjusts the value in place instead of
        // moving focus (Left would otherwise jump to the rail).
        hui::InputFrame in = input;
        if (settings_.cursor_on_slider() &&
            (in.nav == Direction::left || in.nav == Direction::right))
        {
            s_.settings_nudge_slider(in.nav == Direction::right ? 1 : -1);
            in.nav = Direction::none;
            nav_eaten_ = true; // consumed: not a failed focus move (no error buzz)
        }
        // WORKER 7: steppable value rows (<= 4 options): Left/Right cycles the
        // value directly instead of opening a choice list (1 press vs 3).
        else if (settings_.cursor_can_step() &&
                 (in.nav == Direction::left || in.nav == Direction::right))
        {
            s_.settings_step_row(settings_.cursor_row(), in.nav == Direction::right ? 1 : -1);
            in.nav = Direction::none;
            nav_eaten_ = true; // consumed: not a failed focus move (no error buzz)
        }
        // WORKER 7: L2/R2 jump the cursor between section headers. Unbound on
        // this page before; the rail keeps its own behavior when focused.
        if (!settings_.rail_focused() &&
            (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next)))
        {
            settings_.section_jump(input.is_pressed(Action::jump_next) ? 1 : -1);
            sound_.play(hui::audio::Cue::tab);
            return;
        }
        settings_.update(in, dt);
        on_rail = settings_.rail_focused();
        cursor = settings_.rail_cursor();
        if (confirm && settings_.cursor_row() >= 0)
        {
            s_.settings_activate(settings_.cursor_row());
            return;
        }
        break;
    }
    }
    // Circle from any page but the Board goes to the Board (and from the Board it does nothing: the end of
    // the way back).
    if (page_ != 0 && input.is_pressed(Action::back))
    {
        go_page(0);
        return;
    }
    // Triangle opens Search from anywhere (even when the top rail is focused).
    // Options opens Search when not on the rail.
    if (input.is_pressed(Action::north) || (!on_rail && input.is_pressed(Action::menu)))
    {
        s_.begin_search();
        return;
    }
    if (!confirm)
        return;
    if (on_rail)
    {
        if (cursor != page_)
        {
            sound_.play(hui::audio::Cue::tab);
            go_page(cursor);
        }
    }
    else if (under)
    {
        // Glass Phase 1: the hero's buttons. Watch Now does what confirming
        // the focused card does; More Info opens the title's page.
        const int hero_btn = page_ == 0 ? board_.hero_button() : -1;
        if (hero_btn == 1)
            open_title(*under, std::string(), true);
        // A card of Continue Watching asks what to do (or goes to the next episode's streams); any other opens.
        else if (page_ == 0 && s_.board_is_continue(board_.cursor_row()))
            s_.continue_press(board_.cursor_row(), board_.cursor_col());
        else
            open_title(*under);
    }
}

unsigned Shell::view_key() const
{
    return static_cast<unsigned>(screen_) * 64u + static_cast<unsigned>(page_) + (s_.dropdown_open ? 1000u : 0u);
}

unsigned Shell::focus_signature()
{
    const Session::Cursor c = cursor();
    unsigned h = 17u;
    const auto mix = [&h](int v) { h = h * 131u + static_cast<unsigned>(v + 7); };
    mix(c.board_row);
    mix(c.board_col);
    mix(c.discover);
    mix(c.library);
    mix(c.episode);
    mix(c.search_row);
    mix(c.search_col);
    mix(board_.rail_focused());
    mix(board_.rail_cursor());
    mix(discover_.rail_focused());
    mix(discover_.rail_cursor());
    mix(discover_.filter_cursor());
    mix(library_.rail_focused());
    mix(library_.rail_cursor());
    mix(library_.filter_cursor());
    mix(addons_.rail_focused());
    mix(addons_.rail_cursor());
    mix(addons_.cursor_index());
    mix(settings_.rail_focused());
    mix(settings_.rail_cursor());
    mix(settings_.cursor_row());
    mix(search_board_.rail_focused());
    mix(search_board_.rail_cursor());
    mix(detail_.stream_index());
    mix(detail_.episode_index());
    mix(s_.dropdown_open ? s_.dropdown.selected : -1);
    return h;
}
void Shell::update(hui::InputFrame input, float dt)
{
    clock_ += dt;
    // Screensaver wake: the first input is consumed, never routed to the UI.
    if (s_.screensaver_wake_armed() &&
        (input.pressed != 0 || input.nav != Direction::none))
    {
        s_.screensaver_wake();
        input = hui::InputFrame{};
        input.connected = true;
    }
    else if (input.pressed != 0 || input.nav != Direction::none)
    {
        s_.note_input();
    }
    // What arrived since the last frame, before any screen looks at its content.
    apply_changes();
    nav_eaten_ = false; // reset per frame; route_page sets it when it eats a D-pad press
    // The sounds: what the session wants played, and the cursor's move before and after the buttons.
    sound_.set_enabled(s_.ui_sounds());
    for (const hui::audio::Cue cue : s_.take_cues())
        sound_.play(cue);
    // STEP 1 (Instant Board): the old boot_out_ >= 0.35f gate is dropped along
    // with the boot wait; sounds are live from the first frame.
    const bool sounds_ok = !s_.watching && !s_.sign_in_open && typing_hold_ <= 0.0f;
    const unsigned sig_before = sounds_ok ? focus_signature() : 0u;
    const unsigned view_before = view_key();
    if (sounds_ok && input.is_pressed(Action::back) && (s_.dropdown_open || screen_ != Screen::page || page_ != 0))
        sound_.play(hui::audio::Cue::back);
    if (sounds_ok && input.is_pressed(Action::confirm))
        Haptics::instance().select();
    const hui::InputFrame idle = [] {
        hui::InputFrame f;
        f.connected = true;
        return f;
    }();

    if (s_.dropdown_open)
    {
        DropdownState &d = s_.dropdown;
        const int count = static_cast<int>(d.options.size());
        if (input.nav == Direction::up)
            d.selected = std::max(0, d.selected - 1);
        else if (input.nav == Direction::down)
            d.selected = std::min(std::max(0, count - 1), d.selected + 1);
        if (input.is_pressed(Action::confirm))
            s_.dropdown_confirm();
        else if (input.is_pressed(Action::back))
            s_.dropdown_cancel();
    }
    else if (s_.sign_in_open)
    {
        if (input.is_pressed(Action::back))
            s_.sign_in_cancel();
    }
    // The keyboard dialog has the controller; its last button presses must not reach the screens.
    typing_hold_ = s_.typing() ? 0.35f : std::max(0.0f, typing_hold_ - dt);
    // The pop-ups hold the controller while they are up; the screens underneath keep moving.
    // STEP 1 (Instant Board): no boot wait — the Board is interactive from the
    // first frame. (The old welcome cue and board re-enter on boot_ready went
    // with the boot animation.)
    const bool booting = false;
    const bool held = s_.dropdown_open || s_.sign_in_open || dropdown_t_.value > 0.3f || typing_hold_ > 0.0f || booting;
    const hui::InputFrame &in = held ? idle : input;

    if (s_.watching)
    {
        if (!was_watching_)
            watch_t_ = 0.0f;
        watch_t_ += dt;
        s_.watch_input(in);
        player_ui_.state = s_.playing;
        player_ui_.update(dt);
    }
    else if (screen_ == Screen::detail)
    {
        route_detail(in, dt);
    }
    else if (screen_ == Screen::search)
    {
        route_search(in, dt);
    }
    else
    {
        route_page(in, dt);
    }
    if (was_watching_ && !s_.watching)
        sound_.play_when_back(hui::audio::Cue::resume);
    if (sounds_ok && !nav_eaten_ && input.nav != Direction::none && view_key() == view_before)
    {
        // A direction that moved the focus makes a soft sound; one that could not (the end of a list) a
        // short one that says so. Held directions stay silent at the end of a list: the initial press
        // already said it, and repeats would buzz several times a second.
        if (focus_signature() != sig_before)
        {
            sound_.play(hui::audio::Cue::focus);
            Haptics::instance().focus_tick();
        }
        else if (!input.nav_repeat)
        {
            sound_.play(hui::audio::Cue::error);
            Haptics::instance().wall_bump();
        }
    }
    sound_.update(dt);
    was_watching_ = s_.watching;

    dropdown_t_.target = s_.dropdown_open ? 1.0f : 0.0f;
    dropdown_t_.update(dt, 22.0f);
    sign_in_t_.target = s_.sign_in_open ? 1.0f : 0.0f;
    sign_in_t_.update(dt, 18.0f);
    toast_t_.target = s_.toast_open ? 1.0f : 0.0f;
    toast_t_.update(dt, 18.0f);
    // A new screen (page, detail, search, player) restarts the fade-in. It is
    // visual only: input is never blocked, and a fast second navigation just
    // restarts it instead of queuing.
    const unsigned key = static_cast<unsigned>(screen_) * 64u + static_cast<unsigned>(page_) +
                         (s_.watching ? 4096u : 0u);
    if (key != screen_key_)
    {
        screen_key_ = key;
        screen_t_.start(kScreenFade);
    }
    screen_t_.update(dt);
}

void Shell::draw(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, hui::gfx::BackdropSpec &backdrop) const
{
    if (s_.watching)
    {
        if (s_.launch_open)
        {
            backdrop.mode = hui::gfx::BackdropMode::none;
            draw_launch(bg, fg, env_, s_.launch, watch_t_, std::min(1.0f, watch_t_ * 3.0f));
        }
        else
        {
            backdrop.mode = hui::gfx::BackdropMode::none;
            bg.rounded_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0, hui::gfx::Color::rgb(0x000000));
            if (s_.video_texture != 0)
            {
                float w = static_cast<float>(s_.video_w), h = static_cast<float>(s_.video_h);
                std::uint32_t draw_tex = s_.video_texture;
                // FSR1/Anime4K upscaling: upscalers also double as a
                // 1:1 pass-through with sharpening (except Fsr1, which needs
                // a real upscale), so the run gate is: below display res
                // (both dims strictly smaller), or at-or-below display res
                // for the sharpening modes. Widescreen sources that match
                // the display in one dim bypass the upscaler entirely.
                // The upscaler does raw GL here (before any bg.* calls for
                // this frame's video); it saves/restores the viewport and
                // FBO binding (hui rebinds its program/VAO/textures before
                // drawing). Fail-closed: any issue draws the source directly.
                if (!s_.upscaler_tried_)
                {
                    s_.upscaler_tried_ = true;
                    s_.upscaler_ok_ = s_.upscaler_.ensure();
                }
                UpscaleMode um = s_.upscaler_mode();
                const int dw = s_.display_width(), dh = s_.display_height();
                const bool needs_upscale = (s_.video_w < dw && s_.video_h < dh);
                const bool fits_display = (s_.video_w <= dw && s_.video_h <= dh);
                const bool allow_native = fits_display && (um != UpscaleMode::Fsr1);
                if (s_.upscaler_ok_ && um != UpscaleMode::Off &&
                    (needs_upscale || allow_native) &&
                    s_.video_w > 0 && s_.video_h > 0)
                {
                    // Aspect-fit the upscale target into the display. EASU assumes
                    // 1:1 vUV mapping, so a non-16:9 source upscaled to the full
                    // 16:9 display would stretch. Fit by aspect instead.
                    const double scale = std::min(double(dw) / s_.video_w, double(dh) / s_.video_h);
                    const int dst_w = std::max(1, int(s_.video_w * scale + 0.5));
                    const int dst_h = std::max(1, int(s_.video_h * scale + 0.5));
                    // Gate the upscaler on new video frames: upscale once per
                    // source frame / param change, then reuse the cached result.
                    const float sharp = s_.upscaler_sharpness();
                    if (s_.upscaled_tex_ == 0 || s_.upscaled_src_tex_ != s_.video_texture ||
                        s_.upscaled_gen_ != s_.video_gen_ ||
                        s_.upscaled_src_w_ != s_.video_w || s_.upscaled_src_h_ != s_.video_h ||
                        s_.upscaled_dst_w_ != dst_w || s_.upscaled_dst_h_ != dst_h ||
                        s_.upscaled_mode_ != um || s_.upscaled_sharp_ != sharp)
                    {
                        s_.upscaled_tex_ = s_.upscaler_.upscale(
                            s_.video_texture, s_.video_w, s_.video_h,
                            dst_w, dst_h, um, sharp,
                            s_.upscaled_uw_, s_.upscaled_uh_);
                        s_.upscaled_src_tex_ = s_.video_texture;
                        s_.upscaled_gen_ = s_.video_gen_;
                        s_.upscaled_src_w_ = s_.video_w;
                        s_.upscaled_src_h_ = s_.video_h;
                        s_.upscaled_dst_w_ = dst_w;
                        s_.upscaled_dst_h_ = dst_h;
                        s_.upscaled_mode_ = um;
                        s_.upscaled_sharp_ = sharp;
                    }
                    if (s_.upscaled_tex_ != 0)
                    {
                        draw_tex = s_.upscaled_tex_;
                        w = static_cast<float>(s_.upscaled_uw_);
                        h = static_cast<float>(s_.upscaled_uh_);
                    }
                }
                else
                {
                    s_.upscaled_tex_ = 0;
                }
                if (w > 0.0f && h > 0.0f)
                {
                    // Fit: whole picture, scale-down-only (Tier-4b). Fill: crop
                    // the edges to cover the screen; the framebuffer clips the
                    // overdrawn quad, UVs stay correct, so no scissor is needed.
                    const bool fill = s_.aspect_mode() == "fill";
                    const float scale = fill ? std::max(theme::kScreenW / w, theme::kScreenH / h)
                                             : std::min({1.0f, theme::kScreenW / w, theme::kScreenH / h});
                    w *= scale;
                    h *= scale;
                }
                bg.image(draw_tex, {(theme::kScreenW - w) * 0.5f, (theme::kScreenH - h) * 0.5f, w, h},
                         hui::gfx::kFullUv, hui::gfx::Color::rgb(0xffffff));
                // PLAYBACK worker: directional shimmer while a seek catches
                // up. The held frame stays on screen; a soft neutral-white
                // band sweeps across it in the seek direction (forward:
                // left-to-right). One clipped quad pair, only while seeking.
                const double shimmer = s_.playing.seek_shimmer;
                if (shimmer != 0.0 && w > 0.0f && h > 0.0f)
                {
                    const float vx = (theme::kScreenW - w) * 0.5f;
                    const float vy = (theme::kScreenH - h) * 0.5f;
                    const float band = w * 0.22f;
                    float t = std::fmod(watch_t_ / 0.9f, 1.0f); // one sweep every ~0.9 s
                    if (shimmer < 0.0)
                        t = 1.0f - t;
                    const float bx = vx - band + t * (w + 2.0f * band);
                    const float mid = bx + band * 0.5f;
                    bg.push_clip({vx, vy, w, h});
                    bg.gradient_rect_h({bx, vy, band * 0.5f, h}, 0.0f, hui::gfx::Color::rgb(0xffffff, 0.0f),
                                       hui::gfx::Color::rgb(0xffffff, 0.08f));
                    bg.gradient_rect_h({mid, vy, band * 0.5f, h}, 0.0f, hui::gfx::Color::rgb(0xffffff, 0.08f),
                                       hui::gfx::Color::rgb(0xffffff, 0.0f));
                    bg.pop_clip();
                }
            }
            player_ui_.draw(bg, fg);
        }
    }
    else if (screen_ == Screen::detail)
    {
        // A soft fade-in on screen changes: 0.7 -> 1.0 over ~180ms. Subtle
        // enough to feel instant, smooth enough to hide the hard cut.
        const float fade = screen_fade();
        bg.push_opacity(fade);
        fg.push_opacity(fade);
        detail_.draw(bg, fg, backdrop);
        fg.pop_opacity();
        bg.pop_opacity();
    }
    else if (screen_ == Screen::search)
    {
        const float fade = screen_fade();
        bg.push_opacity(fade);
        fg.push_opacity(fade);
        search_board_.draw(bg, fg, backdrop);
        fg.pop_opacity();
        bg.pop_opacity();
    }
    else
    {
        const float fade = screen_fade();
        bg.push_opacity(fade);
        fg.push_opacity(fade);
        switch (page_)
        {
        case 0:
            board_.draw(bg, fg, backdrop);
            break;
        case 1:
            discover_.draw(bg, fg, backdrop);
            break;
        case 2:
            library_.draw(bg, fg, backdrop);
            break;
        case 3:
            addons_.draw(bg, fg, backdrop);
            break;
        default:
            settings_.draw(bg, fg, backdrop);
            break;
        }
        fg.pop_opacity();
        bg.pop_opacity();
    }
    if (dropdown_t_.value > 0.01f)
        draw_dropdown(bg, fg, env_, s_.dropdown, dropdown_t_.value);
    if (sign_in_t_.value > 0.01f)
        draw_sign_in(bg, fg, env_, s_.sign_in, clock_, sign_in_t_.value);
    if (toast_t_.value > 0.01f)
    {
        // During playback the bottom-center toast would sit on the subtitles
        // (baseline ~960) and the controls row (y 978-1042): lift it to the
        // top, clear of the title block (ends ~y 108) and the cards below.
        const float toast_y = (s_.watching && !s_.launch_open) ? 140.0f : 936.0f;
        draw_toast(fg, env_, s_.toast, toast_t_.value, toast_y);
    }
    // The floating bottom-center hints pill ("Options: Mark watched") is
    // intentionally not drawn: every screen already renders its own properly
    // aligned button glyphs (top bar, row headers, detail footer, player
    // controls), and the pill overlapped card titles while being factually
    // wrong on the Board (Options = "Remove from Continue Watching" / Search).
    // Screensaver overlay, drawn last so it covers everything.
    {
        const int phase = s_.screensaver_phase();
        if (phase == 1)
        {
            bg.rounded_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0,
                            hui::gfx::Color::rgb(0x000000, s_.screensaver_dim()));
        }
        else if (phase == 2)
        {
            bg.rounded_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0,
                            hui::gfx::Color::rgb(0x000000, 1.0f));
            // Drifting logo/clock on pure black (OLED pixels off).
            const float lx = s_.screensaver_logo_x() * theme::kScreenW;
            const float ly = s_.screensaver_logo_y() * theme::kScreenH;
            text(bg, env_.fonts.semibold, "stremio", lx, ly, 28.0f,
                 theme::ink_3.with_alpha(0.35f), hui::gfx::Align::center);
        }
    }
    // Phone typing QR card while the IME listener is up.
    {
        const std::string purl = s_.phone_ime_url();
        if (!purl.empty())
        {
            const std::uint32_t qr = s_.qr_texture(purl);
            if (qr != 0)
            {
                const float qs = 220.0f;
                bg.image(qr, {theme::kScreenW - qs - 40.0f, 40.0f, qs, qs},
                         hui::gfx::kFullUv, hui::gfx::Color::rgb(0xffffff));
            }
            text(bg, env_.fonts.regular, "Type from your phone:",
                 theme::kScreenW - 260.0f, 290.0f, 20.0f, theme::ink_2);
            text(bg, env_.fonts.regular, purl,
                 theme::kScreenW - 260.0f, 318.0f, 18.0f, theme::ink_3);
        }
    }
}

} // namespace sx
