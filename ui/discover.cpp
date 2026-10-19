#include "discover.hpp"

#include <algorithm>

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

constexpr Rect kPanel{1216.0f, 192.0f, 640.0f, 784.0f};

GridLayout layout()
{
    GridLayout l;
    l.y = 228.0f;
    l.card_w = 252.0f;
    l.card_h = 378.0f;
    l.pitch_x = 282.0f;
    l.pitch_y = 492.0f;
    l.cols = 4;
    l.view_top = 192.0f;
    return l;
}

} // namespace

void Discover::set_content(const DiscoverContent *content)
{
    content_ = content;
    in_filters_ = false;
    filter_ = 0;
    shown_ = -1;
    grid_.set_items(content ? &content->items : nullptr, layout());
}

void Discover::update(const hui::InputFrame &input, float dt)
{
    tick(dt);
    if (!content_)
        return;
    if (!rail_input(input))
    {
        const int filters = static_cast<int>(content_->filters.size());
        if (in_filters_)
        {
            if (input.nav == Direction::up)
                to_rail();
            else if (input.nav == Direction::left)
            {
                if (filter_ == 0)
                    to_rail();
                else
                    --filter_;
            }
            else if (input.nav == Direction::right)
                filter_ = std::min(filter_ + 1, std::max(0, filters - 1));
            else if (input.nav == Direction::down && !content_->items.empty())
                in_filters_ = false;
        }
        else
        {
            const PosterGrid::Edge edge = grid_.move(input.nav);
            if (edge == PosterGrid::Edge::left)
                to_rail();
            else if (edge == PosterGrid::Edge::up)
            {
                if (filters > 0)
                    in_filters_ = true;
                else
                    to_rail();
            }
        }
    }
    grid_.update(dt, !in_filters_ && !rail_.focused());
    if (grid_.index() != shown_)
    {
        shown_ = grid_.index();
        swap_.start(0.32f);
    }
    swap_.update(dt);
}

void Discover::draw_preview(hui::gfx::DrawList &fg, const Title &t) const
{
    const float in = tween::smoothstep(swap_.progress());
    const float rise = 8.0f * (1.0f - in);
    const float panel_in = tween::stagger(age_, 2, 0.05f, 0.5f);
    fg.push_opacity(panel_in);
    fg.shadow({kPanel.x + 10, kPanel.y + 24, kPanel.w - 20, kPanel.h - 20}, 34, 50, Color::rgb(0x000000, 0.4f));
    draw_glass_panel(fg, env_, kPanel, 32.0f, Color::rgb(0xffffff, 0.05f));
    const float x = kPanel.x + 24.0f, w = kPanel.w - 48.0f;
    fg.push_opacity(in);
    // The title's artwork, in a 16:9 window.
    const Rect still{x, kPanel.y + 24.0f, w, w * 9.0f / 16.0f};
    if (t.backdrop != 0)
        fg.image(t.backdrop, still, kFullUv, Color::rgb(0xffffff), 22.0f);
    else
        fg.gradient_rect(still, 22.0f, Color::rgb(0x2a2540), Color::rgb(0x14111f));

    const hui::ui::FontRef &display = env_.fonts.display;
    // Reference the cached wrap directly: no per-frame copy. The ellipsis
    // clip mutates, and cached_fit can evict the wrap entry, so that rewrite
    // goes through a small local copy, never the cached vector.
    const std::vector<std::string> &wrapped = cached_wrap(t.text_cache_, display.font, t.name, 42.0f, w);
    std::vector<std::string> clipped;
    const std::vector<std::string> *plines = &wrapped;
    if (wrapped.size() > 2)
    {
        clipped.assign(wrapped.begin(), wrapped.begin() + 2);
        clipped[1] = cached_fit(t.text_cache_, display.font, clipped[1] + " \xE2\x80\xA6", 42.0f, w);
        plines = &clipped;
    }
    const std::vector<std::string> &lines = *plines;
    float baseline = still.y + still.h + 52.0f + rise;
    for (const std::string &line : lines)
    {
        // A name without spaces cannot wrap, so each line is also held to the panel.
        text(fg, display, cached_fit(t.text_cache_, display.font, line, 42.0f, w), x, baseline, 42.0f, theme::ink,
             Align::left, -0.9f);
        baseline += 50.0f;
    }
    baseline += 2.0f;
    std::string meta = t.year;
    if (!t.runtime.empty())
        meta += "   \xC2\xB7   " + t.runtime;
    const float meta_w = text(fg, env_.fonts.regular,
                              cached_fit(t.text_cache_, env_.fonts.regular.font, meta, 24.0f, w - 150.0f), x,
                              baseline, 24.0f, theme::ink_2);
    draw_imdb(fg, env_, t.imdb, x + meta_w + 26.0f, baseline);
    baseline += 38.0f;
    text(fg, env_.fonts.semibold, cached_fit(t.text_cache_, env_.fonts.semibold.font, t.genres, 22.0f, w), x,
         baseline, 22.0f, theme::accent_soft);
    // The shorter panel only has room for two synopsis lines above the chip.
    cached_paragraph(fg, t.text_cache_, env_.fonts.regular, t.synopsis, x, baseline + 46.0f, 24.0f, w, 35.0f,
                     theme::ink_2, 2);
    draw_chip(fg, env_, hui::ui::Button::cross, "Open", x, kPanel.y + kPanel.h - 62.0f, true);
    fg.pop_opacity();
    fg.pop_opacity();
}

void Discover::draw(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, hui::gfx::BackdropSpec &backdrop) const
{
    apply_page_backdrop(backdrop, clock_);
    if (!content_)
        return;
    const float in = tween::stagger(age_, 0, 0.0f, 0.45f);
    bg.push_opacity(in);
    float x = theme::kLeft;
    // The chips stop short of the preview panel.
    const std::vector<float> caps = filter_value_caps(env_, content_->filters, kPanel.x - 28.0f - theme::kLeft);
    for (std::size_t i = 0; i < content_->filters.size(); ++i)
        x += draw_filter_chip(bg, env_, content_->filters[i], x, 148.0f,
                              in_filters_ && static_cast<int>(i) == filter_ && !rail_.focused(), caps[i]) +
             14.0f;
    bg.pop_opacity();

    grid_.draw(bg, fg, env_, age_, false, !in_filters_ && !rail_.focused());
    if (const Title *t = grid_.focused())
        draw_preview(fg, *t);
    else if (content_->items.empty() && content_->status.rfind("Loading", 0) == 0)
        grid_.draw_skeletons(bg, env_, clock_); // still arriving: placeholders, not blank
    else
        draw_status(bg, env_, content_->status, 400.0f);

    fade_down(bg, theme::kRailWidth, 0.0f, theme::kScreenW, 118.0f, 0.62f);
    fade_up(bg, theme::kRailWidth, 1000.0f, theme::kScreenW, 80.0f, 0.94f);
    draw_topbar(bg, fg, env_, false, age_);
    rail_.draw(fg, env_, age_);
}

} // namespace sx
