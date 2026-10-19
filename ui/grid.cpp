#include "grid.hpp"

#include "cards.hpp"

#include <algorithm>
#include <cmath>

namespace sx
{

using hui::Direction;
using hui::gfx::Color;
using hui::gfx::Rect;
namespace tween = hui::tween;

namespace
{
constexpr float kPop = 1.07f;
} // namespace

void PosterGrid::set_items(const std::vector<Title> *items, const GridLayout &layout)
{
    items_ = items;
    layout_ = layout;
    index_ = 0;
    pop_.assign(items ? items->size() : 0, tween::Spring{1.0f, 0.0f, 1.0f});
    scroll_.position.snap(0.0f);
    snap();
}

void PosterGrid::sync()
{
    const std::size_t count = items_ ? items_->size() : 0;
    const bool was_empty = pop_.empty();
    pop_.resize(count, tween::Spring{1.0f, 0.0f, 1.0f});
    index_ = std::min(index_, std::max(0, static_cast<int>(count) - 1));
    if (was_empty && count > 0)
        snap();
}

void PosterGrid::snap()
{
    ring_.snap(rect_at(index_, true));
    focus_on_.snap(1.0f);
}

void PosterGrid::set_index(int idx)
{
    if (!items_ || items_->empty())
    {
        index_ = 0;
        return;
    }
    index_ = std::max(0, std::min(idx, static_cast<int>(items_->size()) - 1));
    snap();
}

const Title *PosterGrid::focused() const
{
    if (!items_ || items_->empty())
        return nullptr;
    return &(*items_)[std::min<std::size_t>(index_, items_->size() - 1)];
}

Rect PosterGrid::rect_at(int index, bool final_position) const
{
    const int row = index / layout_.cols;
    const int col = index % layout_.cols;
    const float scroll = final_position ? scroll_.position.target : scroll_.offset();
    return {layout_.x + static_cast<float>(col) * layout_.pitch_x,
            layout_.y + static_cast<float>(row) * layout_.pitch_y - scroll, layout_.card_w, layout_.card_h};
}

PosterGrid::Edge PosterGrid::move(Direction direction)
{
    // Nothing to move over: left still reaches the rail and up the filters.
    if (!items_ || items_->empty())
        return direction == Direction::left ? Edge::left : direction == Direction::up ? Edge::up : Edge::none;
    const int count = static_cast<int>(items_->size());
    const int row = index_ / layout_.cols;
    const int col = index_ % layout_.cols;
    switch (direction)
    {
    case Direction::left:
        if (col == 0)
            return Edge::left;
        --index_;
        break;
    case Direction::right:
        if (col + 1 < layout_.cols && index_ + 1 < count)
            ++index_;
        break;
    case Direction::up:
        if (row == 0)
            return Edge::up;
        index_ -= layout_.cols;
        break;
    case Direction::down:
        if (index_ + layout_.cols < count)
            index_ += layout_.cols;
        else if ((count - 1) / layout_.cols > row)
            index_ = count - 1; // a short last row
        break;
    default:
        break;
    }
    return Edge::none;
}

void PosterGrid::update(float dt, bool has_focus)
{
    if (!items_)
        return;
    const int row = index_ / layout_.cols;
    const float top = static_cast<float>(row) * layout_.pitch_y;
    scroll_.reveal(top, top + layout_.card_h + 96.0f, layout_.view_bottom - layout_.y - 40.0f, 10.0f);
    scroll_.update(dt, 22.0f);
    for (std::size_t i = 0; i < pop_.size(); ++i)
    {
        pop_[i].target = has_focus && static_cast<int>(i) == index_ ? kPop : 1.0f;
        if (!pop_[i].settled())
            pop_[i].update(dt, 20.0f);
    }
    ring_.target(rect_at(index_, true));
    ring_.update(dt, 26.0f);
    focus_on_.target = has_focus ? 1.0f : 0.0f;
    focus_on_.update(dt, 16.0f);
}

void PosterGrid::draw(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, const Env &env, float age, bool show_progress,
                      bool has_focus) const
{
    // With nothing in it there is no card to ring: the screen's own message shows instead.
    if (!items_ || items_->empty())
        return;
    const hui::gfx::Rect clip{layout_.x - 40.0f, layout_.view_top, theme::kScreenW,
                              layout_.view_bottom - layout_.view_top};
    bg.push_clip(clip);
    fg.push_clip(clip);
    const Rect ring = ring_.value();
    const float pad_x = layout_.card_w * (kPop - 1.0f) * 0.5f + 3.0f;
    const float pad_y = layout_.card_h * (kPop - 1.0f) * 0.5f + 3.0f;
    const Rect outer{ring.x - pad_x, ring.y - pad_y, ring.w + 2.0f * pad_x, ring.h + 2.0f * pad_y};
    if (focus_on_.value > 0.01f)
    {
        bg.push_opacity(focus_on_.value);
        bg.glow(outer, 16.0f, 34.0f, theme::accent.with_alpha(0.42f));
        bg.pop_opacity();
    }
    for (int i = 0; i < static_cast<int>(items_->size()); ++i)
    {
        const Rect r = rect_at(i, false);
        if (r.y > layout_.view_bottom + 10.0f || r.y + layout_.card_h + 96.0f < layout_.view_top)
            continue;
        const int row = i / layout_.cols;
        const int col = i % layout_.cols;
        const float enter = tween::stagger(age, row * 2 + col, 0.03f, 0.45f);
        draw_poster_card(bg, fg, env, (*items_)[i], r, pop_[i].value, enter, has_focus && i == index_, show_progress);
    }
    if (focus_on_.value > 0.01f)
    {
        bg.push_opacity(focus_on_.value);
        bg.bordered_rect(outer, 16.0f, Color::rgb(0xffffff, 0.0f), 4.0f, Color::rgb(0xffffff));
        bg.pop_opacity();
    }
    bg.pop_clip();
    fg.pop_clip();
}

void PosterGrid::draw_skeletons(hui::gfx::DrawList &bg, const Env &env, float clock) const
{
    // Loading placeholders in the grid's own layout: two rows of card-shaped
    // rects with a gentle traveling shimmer. Dark-theme subtle, never a ring.
    // The caller only shows these while the status says "Loading...": cached
    // content appears instantly, with no skeleton flash.
    (void)env;
    const int count = layout_.cols * 2;
    const hui::gfx::Rect clip{layout_.x - 40.0f, layout_.view_top, theme::kScreenW,
                              layout_.view_bottom - layout_.view_top};
    bg.push_clip(clip);
    for (int i = 0; i < count; ++i)
    {
        const int row = i / layout_.cols;
        const int col = i % layout_.cols;
        const Rect r{layout_.x + static_cast<float>(col) * layout_.pitch_x,
                     layout_.y + static_cast<float>(row) * layout_.pitch_y, layout_.card_w, layout_.card_h};
        if (r.y > layout_.view_bottom)
            break;
        // A soft wave travels across the cards: each card breathes between
        // 8% and 15% white, phase-offset so the shimmer moves left to right.
        const float wave = 0.5f + 0.5f * std::sin(clock * 2.2f - static_cast<float>(i) * 0.55f);
        const float alpha = 0.08f + 0.07f * wave;
        bg.rounded_rect(r, 12.0f, Color::rgb(0xffffff, alpha));
        // A hint of the title line below the poster shape.
        const Rect line{r.x + 8.0f, r.y + layout_.card_h + 14.0f, layout_.card_w * 0.7f, 14.0f};
        bg.rounded_rect(line, 7.0f, Color::rgb(0xffffff, alpha * 0.7f));
    }
    bg.pop_clip();
}

} // namespace sx
