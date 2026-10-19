#include "cards.hpp"

#include "../gl/a11y.hpp" // A11Y (worker 8): focus indicator strength
#include <algorithm>
#include <cmath>

namespace sx
{

using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::kFullUv;
using hui::gfx::Rect;
using hui::ui::text;

namespace
{

// The poster's corner radius (Glass Phase 1: 12px, the theme's radius).
constexpr float kCardR = 12.0f;
// The focus pop both pages animate to (mirrors board.cpp / grid.cpp).
constexpr float kPop = 1.07f;

} // namespace

void draw_poster_card(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, const Env &env, const Title &t, const Rect &r,
                      float scale, float enter, bool focused, bool show_progress)
{
    bg.push_opacity(enter);
    // Cards arrive from a little below.
    const float lift = 18.0f * (1.0f - enter);
    bg.push_transform(scale, r.cx(), r.cy(), 0.0f, lift);
    bg.shadow({r.x + 8, r.y + 18, r.w - 16, r.h - 14}, 22, 36, Color::rgb(0x000000, focused ? 0.55f : 0.38f));
    if (t.poster != 0)
    {
        if (t.poster_fade < 0.999f) {
            // Fresh upload: dark placeholder underneath, poster fades in over 180ms.
            bg.gradient_rect(r, kCardR, Color::rgb(0x2a2540), Color::rgb(0x14111f));
            bg.image(t.poster, r, kFullUv, Color::rgb(0xffffff, t.poster_fade), kCardR);
        } else {
            bg.image(t.poster, r, kFullUv, Color::rgb(0xffffff), kCardR);
        }
    }
    else
    {
        bg.gradient_rect(r, kCardR, Color::rgb(0x2a2540), Color::rgb(0x14111f));
        hui::ui::paragraph(bg, env.fonts.semibold, t.name, r.x + 20, r.y + 60, 26.0f, r.w - 40, 34.0f, theme::ink_2,
                           5);
    }
    // A strip at the foot of the poster of whatever can be resumed.
    if (show_progress && t.progress >= 0.0f)
    {
        bg.gradient_rect({r.x, r.y + r.h - 70.0f, r.w, 70.0f}, 0.0f, Color::rgb(0x000000, 0.0f),
                         Color::rgb(0x000000, 0.62f));
        const Rect track{r.x + 18.0f, r.y + r.h - 24.0f, r.w - 36.0f, 7.0f};
        bg.rounded_rect(track, 3.5f, Color::rgb(0xffffff, 0.10f));
        const float fill_w = track.w * std::clamp(t.progress, 0.0f, 1.0f);
        bg.glow({track.x, track.y, fill_w, track.h}, 3.5f, 8.0f, theme::accent_soft.with_alpha(0.5f));
        bg.rounded_rect({track.x, track.y, fill_w, track.h}, 3.5f, theme::accent_soft);
    }
    if (!t.badge.empty())
    {
        const float bw = env.fonts.semibold.measure(t.badge, 20.0f) + 22.0f;
        bg.rounded_rect({r.x + 12.0f, r.y + 12.0f, bw, 34.0f}, 17.0f, Color::rgb(0x0c0b11, 0.78f));
        text(bg, env.fonts.semibold, t.badge, r.x + 12.0f + bw * 0.5f, r.y + 36.0f, 20.0f, theme::ink, Align::center);
    }
    // A11Y (worker 8): unmissable focus. The stock focus language (scale pop,
    // shadow, sheen) is untouched; opting into a stronger indicator adds a
    // thick bright outline plus a soft glow — thickness and brightness carry
    // it, never color alone — so the focused card reads at 3m couch distance.
    const int a11y_focus = A11y::get().focus_strength;
    if (focused && a11y_focus > 0)
    {
        const float fw = a11y_focus >= 2 ? 5.0f : 4.0f;
        const Rect fring{r.x - 4.0f, r.y - 4.0f, r.w + 8.0f, r.h + 8.0f};
        bg.glow(fring, kCardR + 4.0f, 16.0f, Color::rgb(0xffffff, a11y_focus >= 2 ? 0.50f : 0.32f));
        bg.bordered_rect(fring, kCardR + 4.0f, Color::rgb(0x000000, 0.0f), fw, Color::rgb(0xffffff, 0.95f));
    }
    bg.pop_transform();
    const float pop_y = (scale - 1.0f) * r.h * 0.5f;
    const float foot_y = r.y + r.h + pop_y;
    // Line 1: Title
    const hui::ui::FontRef &face = env.fonts.semibold;
    text(bg, face, cached_fit(t.text_cache_, face.font, t.name, 22.0f, r.w), r.x, foot_y + 38.0f, 22.0f,
         focused ? theme::ink : Color::rgb(0xffffff, 0.90f));
    // Line 2: star rating  ·  year
    if (!t.imdb.empty() || !t.year.empty())
    {
        std::string sub;
        if (!t.imdb.empty())
            sub = "★ " + t.imdb;
        if (!t.year.empty())
            sub += (sub.empty() ? "" : "   \xC2\xB7   ") + t.year;
        text(bg, env.fonts.regular,
             cached_fit(t.text_cache_, env.fonts.regular.font, sub, 19.0f, r.w),
             r.x, foot_y + 64.0f, 19.0f, theme::ink_2);
    }
    // Line 3: Genres
    if (!t.genres.empty())
    {
        text(bg, env.fonts.regular,
             cached_fit(t.text_cache_, env.fonts.regular.font, t.genres, 18.0f, r.w),
             r.x, foot_y + 88.0f, 18.0f, theme::ink_3);
    }
    bg.pop_opacity();

    // Foreground: the frosted-glass overlays, drawn after renderer.glass()
    // blurs the poster behind them. Same transform/entrance as the poster so
    // they ride the pop.
    if (t.watched)
    {
        // A yellow-tinted glass pill with a check, top right.
        const float pw = 56.0f, ph = 36.0f;
        const Rect pr{r.x + r.w - 12.0f - pw, r.y + 12.0f, pw, ph};
        fg.push_opacity(enter);
        fg.push_transform(scale, r.cx(), r.cy(), 0.0f, lift);
        draw_glass_panel(fg, env, pr, ph * 0.5f, Color::rgb(0xffdc64, 0.35f), Color::rgb(0xffc850, 0.4f));
        const float cx = pr.cx(), cy = pr.cy();
        fg.line(cx - 7.0f, cy + 1.0f, cx - 2.0f, cy + 6.5f, 3.4f, Color::rgb(0xffffff));
        fg.line(cx - 2.0f, cy + 6.5f, cx + 8.0f, cy - 6.0f, 3.4f, Color::rgb(0xffffff));
        fg.pop_transform();
        fg.pop_opacity();
    }
    if (focused)
    {
        // Dim the poster, then a frosted-glass play disc, fading in with the pop.
        const float amount = std::clamp((scale - 1.0f) / (kPop - 1.0f), 0.0f, 1.0f);
        if (amount > 0.01f)
        {
            fg.push_opacity(enter * amount);
            fg.push_transform(scale, r.cx(), r.cy(), 0.0f, lift);
            // Diagonal foil sheen: top-left to bottom-right specular sweep.
            bg.gradient_rect(r, kCardR, Color::rgb(0xffffff, 0.10f * amount), Color::rgb(0xffffff, 0.0f));
            fg.rounded_rect(r, kCardR, Color::rgb(0x000000, 0.4f));
            const float dcx = r.cx(), dcy = r.cy(), drad = 44.0f;
            draw_glass_panel(fg, env, {dcx - drad, dcy - drad, drad * 2.0f, drad * 2.0f}, drad,
                             Color::rgb(0xffffff, 0.45f), Color::rgb(0xffffff, 0.5f));
            const float s = 30.0f;
            const float xy[6] = {dcx - s * 0.4f, dcy - s * 0.55f, dcx - s * 0.4f, dcy + s * 0.55f, dcx + s * 0.5f, dcy};
            fg.polygon(xy, 3, Color::rgb(0xffffff, 0.95f));
            fg.pop_transform();
            fg.pop_opacity();
        }
    }
}

void draw_landscape_card(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, const Env &env, const Title &t, const Rect &r,
                         float scale, float enter, bool focused)
{
    bg.push_opacity(enter);
    const float lift = 18.0f * (1.0f - enter);
    bg.push_transform(scale, r.cx(), r.cy(), 0.0f, lift);
    bg.shadow({r.x + 8.0f, r.y + 16.0f, r.w - 16.0f, r.h - 12.0f}, 18.0f, 34.0f,
              Color::rgb(0x000000, focused ? 0.58f : 0.40f));

    if (t.backdrop != 0)
    {
        bg.image(t.backdrop, r, kFullUv, Color::rgb(0xffffff), 16.0f);
    }
    else if (t.poster != 0)
    {
        bg.image(t.poster, r, {0.0f, 0.20f, 1.0f, 0.375f}, Color::rgb(0xffffff), 16.0f);
    }
    else
    {
        bg.gradient_rect(r, 16.0f, Color::rgb(0x2a2540), Color::rgb(0x14111f));
        hui::ui::paragraph(bg, env.fonts.semibold, t.name, r.x + 24.0f, r.y + 54.0f, 26.0f, r.w - 48.0f, 34.0f,
                           theme::ink_2, 3);
    }

    // Subtle dark vignette for legibility of the play disc, badge, and progress bar.
    bg.gradient_rect({r.x, r.y + r.h - 76.0f, r.w, 76.0f}, 16.0f, Color::rgb(0x000000, 0.0f),
                     Color::rgb(0x000000, 0.72f));

    if (t.progress >= 0.0f)
    {
        const Rect track{r.x + 18.0f, r.y + r.h - 20.0f, r.w - 36.0f, 6.0f};
        bg.rounded_rect(track, 3.0f, Color::rgb(0xffffff, 0.16f));
        const float fill_w = std::max(6.0f, track.w * std::clamp(t.progress, 0.0f, 1.0f));
        bg.glow({track.x, track.y, fill_w, track.h}, 3.0f, 8.0f, theme::accent_soft.with_alpha(0.5f));
        bg.rounded_rect({track.x, track.y, fill_w, track.h}, 3.0f, theme::accent_soft);
    }
    if (!t.badge.empty())
    {
        const float bw = env.fonts.semibold.measure(t.badge, 19.0f) + 22.0f;
        bg.rounded_rect({r.x + 14.0f, r.y + 14.0f, bw, 32.0f}, 16.0f, Color::rgb(0x0c0b11, 0.78f));
        text(bg, env.fonts.semibold, t.badge, r.x + 14.0f + bw * 0.5f, r.y + 36.5f, 19.0f, theme::ink, Align::center);
    }

    const int a11y_focus = A11y::get().focus_strength;
    if (focused && a11y_focus > 0)
    {
        const float fw = a11y_focus >= 2 ? 5.0f : 4.0f;
        const Rect fring{r.x - 4.0f, r.y - 4.0f, r.w + 8.0f, r.h + 8.0f};
        bg.glow(fring, 20.0f, 16.0f, Color::rgb(0xffffff, a11y_focus >= 2 ? 0.50f : 0.32f));
        bg.bordered_rect(fring, 20.0f, Color::rgb(0x000000, 0.0f), fw, Color::rgb(0xffffff, 0.95f));
    }
    bg.pop_transform();
    const float pop_y = (scale - 1.0f) * r.h * 0.5f;
    const float foot_y = r.y + r.h + pop_y;

    // Footer under the 16:9 card.
    text(bg, env.fonts.semibold, cached_fit(t.text_cache_, env.fonts.semibold.font, t.name, 22.0f, r.w), r.x,
         foot_y + 38.0f, 22.0f, focused ? theme::ink : Color::rgb(0xffffff, 0.90f));
    if (!t.imdb.empty() || !t.year.empty() || !t.genres.empty())
    {
        std::string sub;
        if (!t.imdb.empty())
            sub = "★ " + t.imdb;
        if (!t.year.empty())
            sub += (sub.empty() ? "" : "   \xC2\xB7   ") + t.year;
        if (!t.genres.empty())
            sub += (sub.empty() ? "" : "   \xC2\xB7   ") + t.genres;
        text(bg, env.fonts.regular, cached_fit(t.text_cache_, env.fonts.regular.font, sub, 18.0f, r.w), r.x,
             foot_y + 64.0f, 18.0f, theme::ink_2);
    }
    bg.pop_opacity();

    // Centered frosted-glass play disc on Continue Watching cards (always visible, brighter on focus).
    fg.push_opacity(enter);
    fg.push_transform(scale, r.cx(), r.cy(), 0.0f, lift);
    const float dcx = r.cx(), dcy = r.cy(), drad = 32.0f;
    draw_glass_panel(fg, env, {dcx - drad, dcy - drad, drad * 2.0f, drad * 2.0f}, drad,
                     Color::rgb(0xffffff, focused ? 0.42f : 0.24f),
                     Color::rgb(0xffffff, focused ? 0.65f : 0.35f));
    const float s = 22.0f;
    const float xy[6] = {dcx - s * 0.35f, dcy - s * 0.52f, dcx - s * 0.35f, dcy + s * 0.52f, dcx + s * 0.55f, dcy};
    fg.polygon(xy, 3, Color::rgb(0xffffff, 0.95f));
    fg.pop_transform();
    fg.pop_opacity();
}

void draw_chevron(hui::gfx::DrawList &list, float cx, float cy, float size, int direction, Color colour)
{
    // Two strokes meeting at a point: direction 0 down, 1 right, 2 left.
    const float h = size * 0.5f;
    float ax, ay, bx, by, px, py;
    switch (direction)
    {
    case 1:
        ax = cx - h * 0.6f, ay = cy - h, px = cx + h * 0.6f, py = cy, bx = cx - h * 0.6f, by = cy + h;
        break;
    case 2:
        ax = cx + h * 0.6f, ay = cy - h, px = cx - h * 0.6f, py = cy, bx = cx + h * 0.6f, by = cy + h;
        break;
    default:
        ax = cx - h, ay = cy - h * 0.6f, px = cx, py = cy + h * 0.6f, bx = cx + h, by = cy - h * 0.6f;
        break;
    }
    list.line(ax, ay, px, py, 3.2f, colour);
    list.line(px, py, bx, by, 3.2f, colour);
}

namespace
{
constexpr float kChipPad = 26.0f + 14.0f + 16.0f + 18.0f + 24.0f; // everything but the two texts
}

std::vector<float> filter_value_caps(const Env &env, const std::vector<Filter> &filters, float avail)
{
    std::vector<float> natural, caps(filters.size(), 0.0f);
    float fixed = 16.0f * static_cast<float>(filters.size() > 0 ? filters.size() - 1 : 0), total = 0.0f;
    for (const Filter &f : filters)
    {
        natural.push_back(env.fonts.semibold.measure(f.value, 24.0f));
        fixed += kChipPad + env.fonts.regular.measure(f.label, 22.0f);
        total += natural.back();
    }
    const float budget = avail - fixed;
    if (total <= budget)
        return caps;
    float lo = 40.0f, hi = 0.0f;
    for (float v : natural)
        hi = std::max(hi, v);
    for (int i = 0; i < 24; ++i)
    {
        const float mid = (lo + hi) * 0.5f;
        float sum = 0.0f;
        for (float v : natural)
            sum += std::min(v, mid);
        (sum > budget ? hi : lo) = mid;
    }
    for (std::size_t i = 0; i < caps.size(); ++i)
        if (natural[i] > lo)
            caps[i] = lo;
    return caps;
}

float draw_filter_chip(hui::gfx::DrawList &list, const Env &env, const Filter &f, float x, float cy, bool focused,
                       float max_value)
{
    const float h = 58.0f;
    const float label_w = env.fonts.regular.measure(f.label, 22.0f);
    const std::string value = max_value > 0.0f ? env.fonts.semibold.font->fit(f.value, 24.0f, max_value) : f.value;
    const float value_w = env.fonts.semibold.measure(value, 24.0f);
    const float w = 26.0f + label_w + 14.0f + value_w + 16.0f + 18.0f + 24.0f;
    const Rect r{x, cy - h * 0.5f, w, h};
    if (focused)
    {
        list.glow(r, h * 0.5f, 22.0f, theme::accent.with_alpha(0.42f));
        list.bordered_rect(r, h * 0.5f, theme::surface_hi, 3.0f, Color::rgb(0xffffff));
    }
    else
    {
        list.rounded_rect(r, h * 0.5f, theme::surface);
    }
    text(list, env.fonts.regular, f.label, x + 26.0f, cy + 8.0f, 22.0f, theme::ink_3);
    text(list, env.fonts.semibold, value, x + 26.0f + label_w + 14.0f, cy + 8.5f, 24.0f, theme::ink);
    draw_chevron(list, x + w - 30.0f, cy, 14.0f, 0, focused ? theme::ink : theme::ink_2);
    return w;
}

void draw_status(hui::gfx::DrawList &list, const Env &env, const std::string &message, float baseline)
{
    if (message.empty())
        return;
    hui::ui::paragraph(list, env.fonts.regular, message, theme::kLeft, baseline, 32.0f, 1000.0f, 46.0f, theme::ink_2, 3);
}

float draw_tag(hui::gfx::DrawList &list, const Env &env, const std::string &label, float x, float cy, Color fill,
               Color ink)
{
    const float w = env.fonts.semibold.measure(label, 20.0f) + 28.0f;
    list.rounded_rect({x, cy - 17.0f, w, 34.0f}, 17.0f, fill);
    text(list, env.fonts.semibold, label, x + w * 0.5f, cy + 7.0f, 20.0f, ink, Align::center);
    return w;
}

void draw_switch(hui::gfx::DrawList &list, float x, float cy, float on)
{
    const Rect track{x - 34.0f, cy - 19.0f, 68.0f, 38.0f};
    list.rounded_rect(track, 19.0f, hui::gfx::mix(Color::rgb(0xffffff, 0.18f), theme::accent, on));
    const float knob = track.x + 19.0f + 30.0f * on;
    list.shadow({knob - 14.0f, cy - 10.0f, 28.0f, 28.0f}, 14.0f, 8.0f, Color::rgb(0x000000, 0.35f));
    list.circle(knob, cy, 15.0f, Color::rgb(0xffffff));
}

} // namespace sx
