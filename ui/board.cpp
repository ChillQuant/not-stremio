#include "board.hpp"

#include "cards.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sx
{

using hui::Direction;
using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::kFullUv;
using hui::gfx::Rect;
using hui::ui::text;
namespace tween = hui::tween;

namespace
{

// Portrait catalog rows (6 across: 64..1856).
constexpr float kCardW = 268.0f;
constexpr float kCardH = 400.0f;
constexpr float kPitch = 304.0f;
constexpr float kRowH = 584.0f;

// Continue Watching 16:9 landscape row (4 across: 64..1856).
constexpr float kCwCardW = 420.0f;
constexpr float kCwCardH = 236.0f;
constexpr float kCwPitch = 456.0f;
constexpr float kCwRowH = 392.0f;

constexpr float kRowsTop = 816.0f;       // Continue Watching peeks at y=816 under the full-screen Hero
constexpr float kRowsTopSearch = 140.0f; // Search has no Hero: starts right under the top pill bar
constexpr float kPop = 1.06f;            // focused card scale pop

std::string join_meta(const Title &t)
{
    std::string out;
    for (const std::string *part : {&t.year, &t.runtime, &t.genres})
    {
        if (part->empty())
            continue;
        if (!out.empty())
            out += "   \xC2\xB7   ";
        out += *part;
    }
    return out;
}

} // namespace

// Forward declaration: ui/cards.cpp defines this (landscape Continue Watching
// cards); its declaration in ui/cards.hpp lands with the cards worker, so this
// keeps board.cpp compiling either way.
void draw_landscape_card(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, const Env &env, const Title &title,
                         const hui::gfx::Rect &rect, float scale, float enter, bool focused);

// A glass pill button for the hero. Returns its width.
float draw_hero_button(hui::gfx::DrawList &fg, const Env &env, const char *label, float x, float cy, bool focused,
                       Color fill)
{
    const float h = 56.0f;
    const float w = env.fonts.semibold.measure(label, 24.0f) + 48.0f;
    const Rect r{x, cy - h * 0.5f, w, h};
    if (focused)
        fg.glow(r, h * 0.5f, 22.0f, Color::rgb(0xffffff, 0.22f));
    draw_glass_panel(fg, env, r, h * 0.5f, fill, focused ? theme::glass_hair_hi : theme::glass_hair);
    text(fg, env.fonts.semibold, label, r.cx(), cy + 8.0f, 24.0f, theme::ink, Align::center);
    return w;
}
void Board::set_content(const BoardContent *content)
{
    content_ = content;
    const std::size_t rows = content ? content->rows.size() : 0;
    keys_.clear();
    for (std::size_t r = 0; r < rows; ++r)
        keys_.push_back(content->rows[r].key);
    col_.assign(rows, 0);
    across_.assign(rows, {});
    scale_.assign(rows, {});
    for (std::size_t r = 0; r < rows; ++r)
        scale_[r].assign(content->rows[r].items.size(), tween::Spring{1.0f, 0.0f, 1.0f});
    // Instant Board: per-row cursor ids and validation shimmer track the rows.
    focused_ids_.assign(rows, std::string());
    row_shimmer_.assign(rows, {});
    row_ = 0;
    moved_ = false;
    // Instant Board: warm snapshot content is already on screen, so Frame 1
    // renders at full opacity and resting position — no entrance fade. A cold
    // first launch (no rows) keeps age_ at 0 for the calm skeleton fade-in.
    age_ = rows > 0 ? 10.0f : 0.0f;
    hero_btn_ = -1; // fresh rows: the hero buttons let go of the focus
    while (row_ + 1 < static_cast<int>(rows) && content->rows[row_].items.empty())
        ++row_;
    down_.position.snap(0.0f);
    shown_ = before_ = Key{};
    ring_.snap(card_rect(row_, 0, true));
    focus_on_.snap(1.0f);
}

void Board::refresh()
{
    if (!content_)
        return;
    const std::size_t rows = content_->rows.size();
    if (keys_.empty())
    {
        // The first rows have arrived: start on the first one that has cards.
        set_content(content_);
        return;
    }
    std::vector<int> col(rows, 0);
    std::vector<hui::ui::Scroller> across(rows);
    std::vector<std::vector<hui::tween::Spring>> scale(rows);
    // Instant Board: the per-row cursor ids and shimmer springs follow their rows.
    std::vector<std::string> focused_ids(rows);
    std::vector<tween::Spring> shimmer(rows);
    std::string focused_key = row_ < static_cast<int>(keys_.size()) ? keys_[row_] : std::string();
    int new_row = -1;
    for (std::size_t r = 0; r < rows; ++r)
    {
        const Row &row = content_->rows[r];
        const auto it = std::find(keys_.begin(), keys_.end(), row.key);
        if (it != keys_.end())
        {
            const std::size_t old = static_cast<std::size_t>(it - keys_.begin());
            col[r] = col_[old];
            across[r] = across_[old];
            scale[r] = std::move(scale_[old]);
            if (old < focused_ids_.size())
                focused_ids[r] = focused_ids_[old];
            if (old < row_shimmer_.size())
                shimmer[r] = row_shimmer_[old];
            // Instant Board: card insertions must never shift the focus off the
            // selected title — re-locate it by item id, not by column index.
            if (old < focused_ids_.size() && !focused_ids_[old].empty())
            {
                for (std::size_t i = 0; i < row.items.size(); ++i)
                    if (row.items[i].id == focused_ids_[old])
                    {
                        col[r] = static_cast<int>(i);
                        break;
                    }
            }
            if (row.key == focused_key && new_row < 0)
                new_row = static_cast<int>(r);
        }
        scale[r].resize(row.items.size(), tween::Spring{1.0f, 0.0f, 1.0f});
        col[r] = std::min(col[r], std::max(0, static_cast<int>(row.items.size()) - 1));
    }
    keys_.clear();
    for (const Row &row : content_->rows)
        keys_.push_back(row.key);
    col_ = std::move(col);
    across_ = std::move(across);
    scale_ = std::move(scale);
    focused_ids_ = std::move(focused_ids);
    row_shimmer_ = std::move(shimmer);
    const int old_row = row_;
    row_ = new_row >= 0 ? new_row : std::min(row_, std::max(0, static_cast<int>(rows) - 1));
    // Instant Board: rows inserted above the cursor push the focused row down.
    // Nudge the vertical scroller by the inserted rows' height so the viewport
    // stays on the same content instead of jumping.
    if (moved_ && new_row > old_row)
    {
        float shift = 0.0f;
        for (int r = 0; r < new_row - old_row; ++r)
            shift += row_height(r);
        down_.position.value += shift;
        down_.position.target += shift;
    }
    // Rows come in as the addons answer, in any order. Until the player has moved, the
    // first row is where they are, whatever turned up above the one that was first before.
    if (!moved_)
        row_ = 0;
    // The cursor is never left on a row that has nothing in it.
    while (row_ + 1 < static_cast<int>(rows) && content_->rows[row_].items.empty())
        ++row_;
}

void Board::enter()
{
    // Instant Board: the entrance fade only plays on a cold first launch.
    // When snapshot content is already in place, Frame 1 renders at rest.
    age_ = (content_ && !content_->rows.empty()) ? 10.0f : 0.0f;
    rail_.reset(Page::board);
    focus_on_.snap(1.0f);
    hero_btn_ = -1;
}

void Board::set_cursor(int row, int col)
{
    if (!content_ || content_->rows.empty())
        return;
    const int nrows = static_cast<int>(content_->rows.size());
    row_ = std::max(0, std::min(row, nrows - 1));
    const int ncols = static_cast<int>(content_->rows[static_cast<std::size_t>(row_)].items.size());
    if (static_cast<int>(col_.size()) <= row_)
        col_.resize(static_cast<std::size_t>(row_) + 1, 0);
    col_[static_cast<std::size_t>(row_)] = ncols > 0 ? std::max(0, std::min(col, ncols - 1)) : 0;
    moved_ = true;
    hero_btn_ = -1;
    down_.position.snap(scroll_target());

    if (static_cast<int>(across_.size()) > row_)
    {
        const int c = col_[static_cast<std::size_t>(row_)];
        const float pitch = is_landscape_row(row_) ? kCwPitch : kPitch;
        const float cw = is_landscape_row(row_) ? kCwCardW : kCardW;
        across_[static_cast<std::size_t>(row_)].reveal(
            static_cast<float>(c) * pitch, static_cast<float>(c) * pitch + cw,
            theme::kRight - theme::kLeft, 40.0f);
        across_[static_cast<std::size_t>(row_)].position.snap(
            across_[static_cast<std::size_t>(row_)].position.target);
    }
    ring_.snap(card_rect(row_, col_[static_cast<std::size_t>(row_)], true));
}

void Board::set_search(const std::string *query)
{
    query_ = query;
    hero_ = query == nullptr;
}

const Title *Board::focused_title() const
{
    if (!content_ || row_ >= static_cast<int>(content_->rows.size()))
        return nullptr;
    const Row &row = content_->rows[row_];
    if (row.items.empty())
        return nullptr;
    return &row.items[std::min<std::size_t>(col_[row_], row.items.size() - 1)];
}

bool Board::is_landscape_row(int row) const
{
    return hero_ && content_ && row >= 0 && row < static_cast<int>(content_->rows.size()) &&
           content_->rows[static_cast<std::size_t>(row)].key == "continue";
}

float Board::row_height(int row) const
{
    return is_landscape_row(row) ? kCwRowH : kRowH;
}

float Board::row_unscrolled_top(int row) const
{
    float y = hero_ ? kRowsTop : kRowsTopSearch;
    for (int r = 0; r < row; ++r)
        y += row_height(r);
    return y;
}

float Board::scroll_target() const
{
    if (!hero_)
        return row_unscrolled_top(row_) - kRowsTopSearch;
    if (hero_btn_ >= 0 || (!moved_ && row_ == 0))
        return 0.0f;
    if (row_ == 0 && is_landscape_row(0))
        return 136.0f;
    return row_unscrolled_top(row_) - 160.0f;
}

float Board::row_top(int row, bool final_position) const
{
    const float offset = final_position ? down_.position.target : down_.offset();
    return row_unscrolled_top(row) - offset;
}

Rect Board::card_rect(int row, int col, bool final_position) const
{
    float across = 0.0f;
    if (row < static_cast<int>(across_.size()))
        across = final_position ? across_[row].position.target : across_[row].offset();
    const bool cw = is_landscape_row(row);
    const float pitch = cw ? kCwPitch : kPitch;
    const float w = cw ? kCwCardW : kCardW;
    const float h = cw ? kCwCardH : kCardH;
    return {theme::kLeft + static_cast<float>(col) * pitch - across, row_top(row, final_position) + 72.0f, w, h};
}

void Board::update(const hui::InputFrame &input, float dt)
{
    age_ += dt;
    clock_ += dt;
    if (!content_ || content_->rows.empty())
    {
        // Empty board (offline at boot, or "No results" in search): keep the
        // top pill rail usable instead of locking the user out of navigation.
        if (rail_.focused())
        {
            if (input.nav == Direction::left)
                rail_.move(-1);
            else if (input.nav == Direction::right)
                rail_.move(1);
        }
        else if (input.nav == Direction::up || input.nav == Direction::left)
        {
            rail_.focus(true);
        }
        rail_.set_page(Page::board);
        rail_.update(dt);
        return;
    }

    // Instant Board: any navigation marks the board as moved — even rail and
    // hero-button use — so refresh() never yanks the cursor back to row 0
    // after the player has done anything.
    if (input.nav != Direction::none)
        moved_ = true;

    if (rail_.focused())
    {
        if (input.nav == Direction::left)
            rail_.move(-1);
        else if (input.nav == Direction::right)
            rail_.move(1);
        else if (input.nav == Direction::down)
            rail_.focus(false);
    }
    else if (hero_btn_ >= 0)
    {
        switch (input.nav)
        {
        case Direction::up:
            rail_.focus(true);
            hero_btn_ = -1;
            break;
        case Direction::left:
            if (hero_btn_ == 0)
            {
                rail_.focus(true);
                hero_btn_ = -1;
            }
            else
                hero_btn_ = 0;
            break;
        case Direction::right:
            hero_btn_ = 1;
            break;
        case Direction::down:
            hero_btn_ = -1;
            break;
        default:
            break;
        }
    }
    else
    {
        const int rows = static_cast<int>(content_->rows.size());
        int &col = col_[row_];
        const int count = static_cast<int>(content_->rows[row_].items.size());
        switch (input.nav)
        {
        case Direction::left:
            if (col == 0)
            {
                rail_.focus(true);
                hero_btn_ = -1;
            }
            else
                --col;
            break;
        case Direction::right:
            col = std::min(col + 1, std::max(0, count - 1));
            break;
        case Direction::up:
        case Direction::down:
        {
            // Rows with nothing in them are passed over.
            const int step = input.nav == Direction::up ? -1 : 1;
            if (step < 0 && row_ == 0)
            {
                if (hero_)
                    hero_btn_ = 0;
                else
                    rail_.focus(true);
                break;
            }
            for (int r = row_ + step; r >= 0 && r < rows; r += step)
                if (!content_->rows[r].items.empty())
                {
                    row_ = r;
                    break;
                }
            break;
        }
        default:
            break;
        }
        const int n = static_cast<int>(content_->rows[row_].items.size());
        col_[row_] = std::min(col_[row_], std::max(0, n - 1));
    }
    rail_.set_page(Page::board);
    rail_.update(dt);

    // Velocity-adaptive springs: snappy on taps, stiffer on holds.
    fast_scroll_ = input.nav_repeat && input.nav != Direction::none;
    if (fast_scroll_) settle_hold_ = 0.12f;
    else settle_hold_ = std::max(0.0f, settle_hold_ - dt);
    const float scroll_speed = fast_scroll_ ? 36.0f : 22.0f;
    const float ring_speed = fast_scroll_ ? 42.0f : 26.0f;

    // Scroll target:
    // - On Hero buttons (or before moving on Row 0): 0.0f so Hero fills 0..816 and Row 0 peeks at 816..1080.
    // - On Row 0 cards (after moving): 136.0f so the full Row 0 card + title/meta sits at 680..1040.
    // - On Row >= 1: snap the focused catalog row cleanly to y = 124.0f (right below the top pill bar).
    down_.position.target = scroll_target();

    const int c = col_[row_];
    const float pitch = is_landscape_row(row_) ? kCwPitch : kPitch;
    const float cw = is_landscape_row(row_) ? kCwCardW : kCardW;
    across_[row_].reveal(static_cast<float>(c) * pitch, static_cast<float>(c) * pitch + cw,
                         theme::kRight - theme::kLeft, 40.0f);
    for (std::size_t r = 0; r < across_.size(); ++r)
        across_[r].update(dt, scroll_speed);
    down_.update(dt, scroll_speed);

    // The pop of each card, and the ring that glides between them.
    for (std::size_t r = 0; r < scale_.size(); ++r)
        for (std::size_t i = 0; i < scale_[r].size(); ++i)
        {
            const bool focused =
                !rail_.focused() && hero_btn_ < 0 && static_cast<int>(r) == row_ && static_cast<int>(i) == col_[r];
            scale_[r][i].target = focused ? kPop : 1.0f;
            if (!scale_[r][i].settled())
                scale_[r][i].update(dt, ring_speed);
        }
    const Rect target = card_rect(row_, col_[row_], true);
    ring_.target(target);
    ring_.update(dt, ring_speed);
    focus_on_.target = rail_.focused() ? 0.0f : 1.0f;
    focus_on_.update(dt, 16.0f);

    // Ambilight: drift toward the focused title's dominant color.
    {
        const Title *ft = focused_title();
        const std::uint32_t ac = (ft && ft->accent) ? ft->accent : 0x3a2a8f;
        amb_r_.target = ((ac >> 16) & 0xFF) / 255.0f;
        amb_g_.target = ((ac >> 8) & 0xFF) / 255.0f;
        amb_b_.target = (ac & 0xFF) / 255.0f;
        amb_r_.update(dt, 6.0f);
        amb_g_.update(dt, 6.0f);
        amb_b_.update(dt, 6.0f);
    }

    // Instant Board: remember each row's cursor item by id (bounds-checked),
    // so refresh() can re-locate the focused title when card insertions shift
    // indices — and drive each row's validation shimmer toward its lifecycle
    // state (1 while validating, else 0).
    for (std::size_t r = 0; r < content_->rows.size(); ++r)
    {
        if (r < focused_ids_.size())
        {
            const Row &rdata = content_->rows[r];
            const int c = r < col_.size() ? col_[r] : 0;
            focused_ids_[r] = (!rdata.items.empty() && c >= 0 && c < static_cast<int>(rdata.items.size()))
                                  ? rdata.items[static_cast<std::size_t>(c)].id
                                  : std::string();
        }
        if (r < row_shimmer_.size())
        {
            row_shimmer_[r].target = content_->rows[r].state == RowState::validating ? 1.0f : 0.0f;
            row_shimmer_[r].update(dt, 8.0f);
        }
    }

    // The hero follows the focus, crossfading from what it showed.
    // During fast scrolls the swap waits until the cursor settles (120ms),
    // so 15 cards flown past don't trigger 15 redundant crossfades.
    const Key now{row_, col_[row_]};
    if (!(now == shown_) && settle_hold_ <= 0.0f)
    {
        before_ = shown_;
        shown_ = now;
        swap_.start(0.38f);
    }
    swap_.update(dt);
}

void Board::draw_row(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, int row, float top) const
{
    const Row &data = content_->rows[row];
    const bool current = row == row_ && !rail_.focused();
    const float row_in = tween::stagger(age_, row, 0.10f, 0.45f);
    bg.push_opacity(row_in);
    text(bg, env_.fonts.semibold,
         env_.fonts.semibold.font->fit(data.title, theme::kHeading, theme::kRight - theme::kLeft - 260.0f), theme::kLeft,
         top + 36.0f, theme::kHeading, current ? theme::ink : theme::ink_2, Align::left, -0.3f);
    if (data.items.empty())
        text(bg, env_.fonts.regular, "Nothing here yet", theme::kLeft, top + 110.0f, theme::kSubtitle, theme::ink_3);
    // "See All": the Square glyph appears beside the row that has the focus. Continue Watching has none:
    // everything in it can be reached by scrolling.
    const bool continue_row = data.key == "continue";
    // Search results show everything already (no catalog to expand), so "See All" is meaningless there.
    if (!continue_row && query_ == nullptr)
    {
        const Color see_c = current ? theme::ink_2 : theme::ink_3;
        const float see_w = env_.fonts.regular.measure("See All", theme::kLabel);
        text(bg, env_.fonts.regular, "See All", theme::kRight - 22.0f, top + 36.0f, theme::kLabel, see_c, Align::right);
        draw_chevron(bg, theme::kRight - 8.0f, top + 28.0f, 12.0f, 1, see_c);
        if (current)
            hui::ui::draw_button(bg, env_.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::square,
                                 theme::kRight - see_w - 64.0f, top + 26.0f, 30.0f);
    }
    // On Continue Watching, Options takes the card off the row.
    if (current && continue_row && !data.items.empty())
    {
        const float lw = env_.fonts.regular.measure("Options", theme::kLabel);
        const float right = theme::kRight;
        text(bg, env_.fonts.regular, "Options", right, top + 36.0f, theme::kLabel, theme::ink_2, Align::right);
        hui::ui::draw_button(bg, env_.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::options,
                             right - lw - 52.0f, top + 26.0f, 32.0f);
    }
    // Instant Board: the row is re-validating its snapshot content — a
    // breathing dot by the header says so, kept subtle.
    const float val_a = row < static_cast<int>(row_shimmer_.size()) ? row_shimmer_[row].value : 0.0f;
    if (val_a > 0.01f)
    {
        const float breath = 0.5f + 0.5f * std::sin(clock_ * 3.0f);
        bg.circle(theme::kLeft - 20.0f, top + 44.0f, 4.0f, Color::rgb(0xffffff, 0.30f * val_a * breath));
    }
    bg.pop_opacity();

    // The strip is clipped at the left, under the rail's edge, and bleeds off the right.
    // Both layers clip alike: the card's glass overlays must not escape the strip.
    const float rh = row_height(row);
    const hui::gfx::Rect strip{theme::kLeft - 38.0f, top + 36.0f, theme::kScreenW, rh - 28.0f};
    bg.push_clip(strip);
    fg.push_clip(strip);
    const bool landscape = is_landscape_row(row);
    const bool resume = data.title.find("Continue") != std::string::npos;
    for (std::size_t i = 0; i < data.items.size(); ++i)
    {
        const Rect r = card_rect(row, static_cast<int>(i), false);
        if (r.x > theme::kScreenW + 20.0f || r.x + r.w < theme::kLeft - 60.0f)
            continue;
        const float enter = tween::stagger(age_, row * 2 + static_cast<int>(i), 0.035f, 0.45f);
        const bool focused = row == row_ && static_cast<int>(i) == col_[row] && !rail_.focused() && hero_btn_ < 0;
        if (landscape)
            draw_landscape_card(bg, fg, env_, data.items[i], {r.x, top + 72.0f, r.w, r.h}, scale_[row][i].value, enter,
                                focused);
        else
            draw_poster_card(bg, fg, env_, data.items[i], {r.x, top + 72.0f, r.w, r.h}, scale_[row][i].value, enter,
                             focused, resume);
        // Instant Board: while the row validates, a faint 1px rim travels
        // across the cards — white at most 0.07 alpha, scaled by the shimmer.
        if (val_a > 0.01f)
        {
            const float wave = 0.5f + 0.5f * std::sin(clock_ * 2.2f - static_cast<float>(i) * 0.9f);
            const float rim_a = 0.07f * val_a * wave;
            if (rim_a > 0.004f)
                bg.bordered_rect({r.x, top + 72.0f, r.w, r.h}, 12.0f, Color::rgb(0xffffff, 0.0f), 1.0f,
                                 Color::rgb(0xffffff, rim_a));
        }
    }
    bg.pop_clip();
    fg.pop_clip();
}

void Board::draw_hero(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, float offset) const
{
    const Title *now = focused_title();
    if (!now)
        return;
    const float mix = tween::cubic_out(swap_.progress());
    const Title *prev = nullptr;
    if (before_.row >= 0 && before_.row < static_cast<int>(content_->rows.size()))
    {
        const Row &row = content_->rows[before_.row];
        if (before_.col >= 0 && before_.col < static_cast<int>(row.items.size()))
            prev = &row.items[before_.col];
    }

    const float dim = 1.0f - tween::clamp01((offset - 140.0f) / 340.0f);
    if (dim <= 0.01f)
        return;
    const float intro = tween::stagger(age_, 0, 0.0f, 0.7f);
    const Color g = theme::ground;
    const float W = theme::kScreenW, H = theme::kScreenH;
    const Rect full{0.0f, 0.0f, W, H};
    bg.push_opacity(dim * intro);
    if (prev && prev->backdrop != 0 && mix < 1.0f)
        bg.image(prev->backdrop, full, kFullUv, Color::rgb(0xffffff));
    if (now->backdrop != 0)
        bg.image(now->backdrop, full, kFullUv, Color::rgb(0xffffff, prev ? mix : 1.0f));
    // Smooth left vignette for text legibility and bottom vignette into the rows, leaving the right side bright.
    bg.gradient_rect_h({0.0f, 0.0f, 980.0f, H}, 0, g.with_alpha(0.88f), g.with_alpha(0.0f));
    bg.gradient_rect({0.0f, 0.0f, W, 140.0f}, 0, g.with_alpha(0.55f), g.with_alpha(0.0f));
    bg.gradient_rect({0.0f, 520.0f, W, H - 520.0f}, 0, g.with_alpha(0.0f), g.with_alpha(0.98f));
    bg.pop_opacity();
    bg.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -offset * 0.55f);
    const float text_in = tween::smoothstep(mix) * intro * dim;
    const float rise = 10.0f * (1.0f - tween::smoothstep(mix));
    bg.push_opacity(text_in);
    const hui::ui::FontRef &display = env_.fonts.display;
    // 1. Logo or Display Title (anchored above y = 548)
    if (now->logo != 0 && now->logo_w > 0 && now->logo_h > 0)
    {
        float lw = std::min(560.0f, static_cast<float>(now->logo_w));
        float lh = lw * static_cast<float>(now->logo_h) / static_cast<float>(now->logo_w);
        if (lh > 180.0f)
        {
            lh = 180.0f;
            lw = lh * static_cast<float>(now->logo_w) / static_cast<float>(now->logo_h);
        }
        bg.image(now->logo, {theme::kLeft, 548.0f - lh + rise, lw, lh}, kFullUv, Color::rgb(0xffffff));
    }
    else
    {
        float size = 66.0f, leading = 74.0f;
        // Reference the cached wrap directly: no per-frame copy. The 54pt
        // re-wrap may evict the 66pt entry, so its size is consumed first.
        const std::vector<std::string> *wrapped =
            &cached_wrap(now->text_cache_, display.font, now->name, size, 820.0f);
        if (wrapped->size() > 1)
        {
            size = 54.0f;
            leading = 62.0f;
            wrapped = &cached_wrap(now->text_cache_, display.font, now->name, size, 820.0f);
        }
        // The ellipsis clip mutates: cached_fit can evict the wrap entry, so
        // the rewrite goes through a small local copy, never the cached vector.
        std::vector<std::string> clipped;
        const std::vector<std::string> *plines = wrapped;
        if (wrapped->size() > 2)
        {
            clipped.assign(wrapped->begin(), wrapped->begin() + 2);
            clipped[1] = cached_fit(now->text_cache_, display.font, clipped[1] + " \xE2\x80\xA6", size, 820.0f);
            plines = &clipped;
        }
        const std::vector<std::string> &lines = *plines;
        float baseline = 536.0f - (lines.size() > 1 ? leading : 0.0f) + rise;
        for (const std::string &line : lines)
        {
            text(bg, display, cached_fit(now->text_cache_, display.font, line, size, 820.0f), theme::kLeft, baseline,
                 size, theme::ink, Align::left, -1.4f);
            baseline += leading;
        }
    }
    // 2. Synopsis (2 lines, right below the logo)
    cached_paragraph(bg, now->text_cache_, env_.fonts.regular, now->synopsis, theme::kLeft, 588.0f + rise, 22.0f,
                     680.0f, 31.0f, Color::rgb(0xffffff, 0.90f), 2);
    // 3. Metadata row: Year   Runtime   Type/Genres   star IMDb/10
    {
        std::string meta = join_meta(*now);
        const std::string &fitted = cached_fit(now->text_cache_, env_.fonts.semibold.font, meta, 20.0f, 760.0f);
        float mx = theme::kLeft;
        if (!fitted.empty())
            mx += text(bg, env_.fonts.semibold, fitted, mx, 672.0f + rise, 20.0f, theme::ink_2) + 24.0f;
        if (!now->imdb.empty())
        {
            mx += text(bg, env_.fonts.semibold, "★ ", mx, 672.0f + rise, 20.0f, theme::imdb);
            text(bg, env_.fonts.semibold, now->imdb + "/10", mx, 672.0f + rise, 20.0f, theme::ink);
        }
    }
    // 4. Carousel pagination dots centered horizontally
    if (row_ < static_cast<int>(content_->rows.size()))
    {
        const int total = std::min(12, static_cast<int>(content_->rows[row_].items.size()));
        const float dot_fade = 1.0f - tween::clamp01(offset / 90.0f);
        if (total > 1 && dot_fade > 0.01f)
        {
            const int active_dot = std::clamp(col_[row_], 0, total - 1);
            const float dot_gap = 22.0f;
            const float total_w = static_cast<float>(total - 1) * dot_gap;
            float dx = (theme::kScreenW - total_w) * 0.5f;
            for (int i = 0; i < total; ++i)
            {
                const bool act = (i == active_dot);
                bg.circle(dx, 776.0f + rise, act ? 5.5f : 4.5f,
                          Color::rgb(0xffffff, (act ? 0.95f : 0.30f) * dot_fade));
                dx += dot_gap;
            }
        }
    }
    bg.pop_opacity();
    bg.pop_transform();
    // 5. Glass buttons: Watch Now / Resume + More Info
    const bool resume = now->progress >= 0.0f;
    fg.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -offset * 0.55f);
    fg.push_opacity(text_in);
    float bx = theme::kLeft;
    bx += draw_hero_button(fg, env_, resume ? "►  Resume" : "►  Watch Now", bx, 724.0f + rise, hero_btn_ == 0,
                           theme::glass_deep) +
          32.0f;
    bx += draw_hero_button(fg, env_, "More Info", bx, 724.0f + rise, hero_btn_ == 1, theme::glass_fill);
    if (resume)
    {
        char left[48];
        std::snprintf(left, sizeof(left), "%d%% watched", static_cast<int>(now->progress * 100.0f + 0.5f));
        text(fg, env_.fonts.regular, left, bx + 40.0f, 732.0f + rise, 21.0f, theme::ink_2);
    }
    fg.pop_opacity();
    fg.pop_transform();
}

void Board::draw_skeleton_rows(hui::gfx::DrawList &bg) const
{
    // Instant Board, cold start: the Stremio mark as a faint watermark, plus
    // three shimmering Hero placeholder bars where the hero will land.
    const std::uint32_t mark = env_.icons.logo_xl != 0 ? env_.icons.logo_xl : env_.icons.logo;
    if (mark != 0)
        bg.image(mark, {theme::kLeft, 210.0f, 84.0f, 84.0f}, kFullUv, Color::rgb(0xffffff, 0.12f));
    const float bar_y[3] = {330.0f, 410.0f, 460.0f};
    const float bar_w[3] = {520.0f, 680.0f, 380.0f};
    for (int i = 0; i < 3; ++i)
    {
        const float wave = 0.5f + 0.5f * std::sin(clock_ * 2.2f - static_cast<float>(i) * 1.2f);
        bg.rounded_rect({theme::kLeft, bar_y[i], bar_w[i], 22.0f}, 11.0f,
                        Color::rgb(0xffffff, 0.07f + 0.05f * wave));
    }
    // Two placeholder rows while the catalogs load: a header bar and a strip
    // of card shapes, with the same gentle traveling shimmer as the grids.
    for (int row = 0; row < 2; ++row)
    {
        const float top = row_unscrolled_top(row);
        if (top > theme::kScreenH)
            break;
        const float header_wave = 0.5f + 0.5f * std::sin(clock_ * 2.2f - static_cast<float>(row) * 1.2f);
        bg.rounded_rect({theme::kLeft, top + 28.0f, 280.0f, 22.0f}, 11.0f,
                        hui::gfx::Color::rgb(0xffffff, 0.07f + 0.05f * header_wave));
        for (int col = 0; col < 6; ++col)
        {
            const float x = theme::kLeft + static_cast<float>(col) * kPitch;
            if (x > theme::kScreenW)
                break;
            const int i = row * 6 + col;
            const float wave = 0.5f + 0.5f * std::sin(clock_ * 2.2f - static_cast<float>(i) * 0.55f);
            const float alpha = 0.08f + 0.07f * wave;
            bg.rounded_rect({x, top + 72.0f, kCardW, kCardH}, 12.0f, hui::gfx::Color::rgb(0xffffff, alpha));
        }
    }
}

void Board::draw(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, hui::gfx::BackdropSpec &backdrop) const
{
    // The page: near-black with Stremio's violet light from the top right.
    backdrop.mode = hui::gfx::BackdropMode::gradient;
    backdrop.colors[0] = theme::ground;
    backdrop.colors[1] = theme::ground;
    backdrop.colors[2] = Color{amb_r_.value, amb_g_.value, amb_b_.value, 1.0f}; // ambilight
    backdrop.params[0] = 0.92f;
    backdrop.params[1] = 0.0f;
    backdrop.params[2] = 0.55f;
    backdrop.time = clock_;

    if (!content_ || content_->rows.empty())
    {
        // Still loading: skeleton rows instead of a blank page. Cached content
        // never hits this branch, so there is no skeleton flash.
        if (content_ && content_->status.rfind("Loading", 0) == 0)
            draw_skeleton_rows(bg);
        else if (content_ && !content_->status.empty())
        {
            draw_status(bg, env_, content_->status, 330.0f);
        }
        else if (!hero_ && query_ != nullptr)
        {
            const float in = tween::stagger(age_, 0, 0.0f, 0.5f);
            bg.push_opacity(in);
            text(bg, env_.fonts.semibold, "No results for \xE2\x80\x9C" + *query_ + "\xE2\x80\x9D", theme::kLeft, 330.0f,
                 44.0f, theme::ink, Align::left, -0.6f);
            text(bg, env_.fonts.regular, "Check the spelling, or try a shorter name.", theme::kLeft, 392.0f,
                 theme::kSubtitle, theme::ink_2);
            bg.pop_opacity();
        }
        draw_topbar(bg, fg, env_, false, age_, query_, clock_);
        rail_.draw(fg, env_, age_);
        return;
    }

    const float offset = down_.offset();
    if (hero_)
        draw_hero(bg, fg, offset);

    const Rect ring = ring_.value();
    const float pad_x = ring.w * (kPop - 1.0f) * 0.5f + 3.0f;
    const float pad_y = ring.h * (kPop - 1.0f) * 0.5f + 3.0f;
    const Rect outer{ring.x - pad_x, ring.y - pad_y, ring.w + 2.0f * pad_x, ring.h + 2.0f * pad_y};
    // While the hero buttons hold the focus the card ring stands down.
    const bool has_focus = focused_title() != nullptr && hero_btn_ < 0;

    const float clip_top = offset > 10.0f ? 98.0f : 0.0f;
    if (clip_top > 0.0f)
    {
        bg.push_clip({0.0f, clip_top, theme::kScreenW, theme::kScreenH - clip_top});
        fg.push_clip({0.0f, clip_top, theme::kScreenW, theme::kScreenH - clip_top});
    }
    if (focus_on_.value > 0.01f && has_focus)
    {
        const Color amb = Color{amb_r_.value, amb_g_.value, amb_b_.value, 1.0f};
        bg.push_opacity(focus_on_.value);
        bg.glow(outer, 16.0f, 32.0f, hui::gfx::mix(theme::accent, amb, 0.5f).with_alpha(0.42f));
        bg.pop_opacity();
    }
    for (int r = 0; r < static_cast<int>(content_->rows.size()); ++r)
    {
        const float top = row_top(r, false);
        const float rh = row_height(r);
        if (top > theme::kScreenH || top + rh < clip_top)
            continue;
        draw_row(bg, fg, r, top);
    }
    if (focus_on_.value > 0.01f && has_focus)
    {
        bg.push_opacity(focus_on_.value);
        bg.bordered_rect(outer, 16.0f, Color::rgb(0xffffff, 0.0f), 3.5f, Color::rgb(0xffffff));
        bg.pop_opacity();
    }
    if (clip_top > 0.0f)
    {
        bg.pop_clip();
        fg.pop_clip();
    }

    // Chrome over the content: it scrolls under soft edges, not hard lines.
    // At the top of the page the backdrop shows through; once rows scroll under
    // the top bar the fade closes up so nothing collides with the search pill.
    const float closed = tween::clamp01(offset / 200.0f);
    // (Over the wallpaper they reach the screen's left edge: starting at the rail's edge they would
    // leave a step there.)
    const float edge = hero_ ? 0.0f : theme::kRailWidth;
    fade_down(bg, edge, 0.0f, theme::kScreenW, tween::lerp(150.0f, 220.0f, closed),
              tween::lerp(0.55f, 0.98f, closed), tween::lerp(0.0f, 108.0f, closed));
    fade_up(bg, edge, 990.0f, theme::kScreenW, 90.0f, 0.94f);
    draw_topbar(bg, fg, env_, false, age_, query_, clock_);
    // Instant Board: the glass sync-status pill, just left of the search pill.
    // Empty sync_pill (or a search view) hides it.
    if (content_ && !content_->sync_pill.empty() && query_ == nullptr)
    {
        const float pw = env_.fonts.regular.measure(content_->sync_pill, 19.0f) + 34.0f;
        const float search_left = theme::kRight - 116.0f - 210.0f;
        const Rect spill{search_left - 14.0f - pw, 36.0f, pw, 48.0f};
        draw_glass_panel(fg, env_, spill, 24.0f, Color::rgb(0xffffff, 0.04f),
                         content_->offline ? Color::rgb(0xf5c518, 0.25f) : Color::rgb(0xffffff, 0.10f));
        text(fg, env_.fonts.regular, content_->sync_pill, spill.cx(), spill.cy() + 7.0f, 19.0f,
             content_->offline ? Color::rgb(0xf5c518, 0.85f) : theme::ink_3, Align::center);
    }
    rail_.draw(fg, env_, age_);
}

} // namespace sx
