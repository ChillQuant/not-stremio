#include "chrome.hpp"

#include "../gl/a11y.hpp" // A11Y (worker 8): high-contrast panels
#include "core/tween.hpp"

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

constexpr const char *kPageNames[5] = {"Board", "Discover", "Library", "Addons", "Settings"};
constexpr float kPillY = 36.0f;
constexpr float kPillH = 48.0f;
constexpr float kPillX0 = 132.0f;
constexpr float kPillW[5] = {118.0f, 142.0f, 128.0f, 132.0f, 138.0f};
constexpr float kPillGap = 14.0f;

} // namespace

Rail::Rail()
{
    pill_.snap(item_rect(0));
}

Rect Rail::item_rect(int index)
{
    const int idx = std::clamp(index, 0, 4);
    float x = kPillX0;
    for (int i = 0; i < idx; ++i)
        x += kPillW[i] + kPillGap;
    return {x, kPillY, kPillW[idx], kPillH};
}

void Rail::set_page(Page page)
{
    page_ = static_cast<int>(page);
    if (!focused_)
        cursor_ = page_;
}

void Rail::focus(bool on)
{
    focused_ = on;
    if (on)
        cursor_ = page_;
}

void Rail::move(int delta)
{
    cursor_ = std::clamp(cursor_ + delta, 0, 4);
}

void Rail::reset(Page page)
{
    page_ = static_cast<int>(page);
    focused_ = false;
    cursor_ = page_;
    pill_.snap(item_rect(cursor_));
    glow_.snap(0.0f);
}

void Rail::update(float dt)
{
    pill_.target(item_rect(cursor_));
    pill_.update(dt, 22.0f);
    glow_.target = focused_ ? 1.0f : 0.0f;
    glow_.update(dt, 16.0f);
}

void draw_glass_panel(hui::gfx::DrawList &fg, const Env &env, const hui::gfx::Rect &r, float radius, Color tint,
                      Color rim)
{
    // A11Y (worker 8): high-contrast mode. A solid neutral-black panel
    // replaces the frosted-glass blur: maximum text legibility at distance,
    // and the backdrop-blur sampling (5-15 ms of GPU per frame) is skipped
    // entirely — a net perf win. Opt-in; the stock glass look is untouched.
    if (A11y::get().high_contrast)
    {
        fg.bordered_rect(r, radius, Color::rgb(0x000000, 0.94f), 1.5f, Color::rgb(0xffffff, 0.38f));
        return;
    }
    if (env.glass_tex != 0)
    {
        fg.glass(env.glass_tex, r, radius, tint);
        // The 1px inner rim light: the signature of the glass look.
        fg.bordered_rect(r, radius, Color::rgb(0x000000, 0.0f), 1.0f, rim);
    }
    else
    {
        fg.bordered_rect(r, radius, tint, 1.0f, rim);
    }
    // Specular top-edge catchlight: makes every frosted glass pill and panel
    // look like physical top-lit glass at a cost of 1 tiny rect.
    const float inset = std::min(radius * 0.85f, r.w * 0.25f);
    if (r.w > inset * 2.0f + 4.0f)
    {
        fg.rounded_rect({r.x + inset, r.y + 0.5f, r.w - inset * 2.0f, 1.5f}, 0.75f,
                        Color::rgb(0xffffff, std::min(0.32f, rim.a * 1.8f)));
    }
}

void Rail::draw(hui::gfx::DrawList &fg, const Env &env, float age) const
{
    const float in = hui::tween::stagger(age, 0, 0.0f, 0.30f);
    fg.push_opacity(in);
    const Rect first = item_rect(0), last = item_rect(4);
    const Rect bar{first.x - 6.0f, kPillY - 4.0f, (last.x + last.w) - first.x + 12.0f, kPillH + 8.0f};
    draw_glass_panel(fg, env, bar, bar.h * 0.5f, Color::rgb(0xffffff, 0.06f), Color::rgb(0xffffff, 0.14f));

    const Rect active_r = item_rect(page_);
    if (!focused_ || cursor_ == page_)
    {
        fg.rounded_rect(active_r, kPillH * 0.5f, Color::rgb(0xffffff, 0.16f));
    }
    if (glow_.value > 0.01f)
    {
        const Rect p = pill_.value();
        fg.push_opacity(glow_.value);
        fg.glow(p, kPillH * 0.5f, 18.0f, Color::rgb(0xffffff, 0.22f));
        fg.bordered_rect(p, kPillH * 0.5f, Color::rgb(0xffffff, 0.20f), 2.0f, Color::rgb(0xffffff, 0.95f));
        fg.pop_opacity();
    }
    for (int i = 0; i < 5; ++i)
    {
        const Rect r = item_rect(i);
        const bool lit = (i == page_ && !focused_) || (focused_ && i == cursor_);
        const Color tint = lit ? Color::rgb(0xffffff) : theme::ink_2;
        const hui::ui::FontRef &face = lit ? env.fonts.semibold : env.fonts.regular;
        text(fg, face, kPageNames[i], r.cx(), r.cy() + 7.5f, 21.0f, tint, Align::center);
    }
    fg.pop_opacity();
}

void draw_logo(hui::gfx::DrawList &list, const Env &env)
{
    if (env.icons.logo != 0)
        list.image(env.icons.logo, {theme::kLeft, 36.0f, 48.0f, 48.0f}, kFullUv,
                   Color::rgb(0xffffff, 0.94f));
}

void draw_topbar(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, const Env &env, bool search_focused, float age,
                 const std::string *query, float clock)
{
    const float in = hui::tween::stagger(age, 0, 0.0f, 0.4f);
    bg.push_opacity(in);
    draw_logo(bg, env);
    bg.pop_opacity();

    fg.push_opacity(in);
    const bool has_query = query != nullptr && !query->empty();
    const float pw = has_query ? 420.0f : 210.0f;
    const Rect pill{theme::kRight - 116.0f - pw, 36.0f, pw, 48.0f};
    if (search_focused)
    {
        fg.glow(pill, 24.0f, 20.0f, theme::accent.with_alpha(0.4f));
        draw_glass_panel(fg, env, pill, 24.0f, theme::surface_hi);
        fg.bordered_rect(pill, 24.0f, Color::rgb(0xffffff, 0.0f), 2.5f, Color::rgb(0xffffff));
    }
    else
    {
        draw_glass_panel(fg, env, pill, 24.0f, Color::rgb(0xffffff, 0.06f), Color::rgb(0xffffff, 0.14f));
    }
    if (env.icons.search != 0)
        fg.image(env.icons.search, {pill.x + 18.0f, pill.cy() - 12.0f, 24.0f, 24.0f}, kFullUv,
                 search_focused ? Color::rgb(0xffffff) : theme::ink_2);
    if (has_query)
    {
        const float w = text(fg, env.fonts.semibold, env.fonts.semibold.font->fit(*query, 21.0f, pw - 106.0f),
                             pill.x + 52.0f, pill.cy() + 7.5f, 21.0f, theme::ink);
        if (std::fmod(clock, 1.0f) < 0.6f)
            fg.rounded_rect({pill.x + 56.0f + w, pill.cy() - 12.0f, 2.5f, 24.0f}, 1.25f, theme::accent_soft);
    }
    else
    {
        text(fg, env.fonts.regular, "Search", pill.x + 50.0f, pill.cy() + 7.5f, 20.0f,
             search_focused ? theme::ink : theme::ink_2);
    }
    hui::ui::draw_button(fg, env.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::triangle,
                         pill.x + pill.w - 38.0f, pill.cy(), 26.0f);
    fg.pop_opacity();

    bg.push_opacity(in);
    text(bg, env.fonts.semibold, env.clock, theme::kRight, 69.0f, 26.0f, theme::ink_2, Align::right);
    bg.pop_opacity();
}

float draw_chip(hui::gfx::DrawList &list, const Env &env, hui::ui::Button button, const char *label, float x,
                float cy, bool strong, float max_text)
{
    const float h = 52.0f;
    const float glyph = 36.0f;
    std::string shown = label;
    if (max_text > 0.0f)
        shown = env.fonts.semibold.font->fit(shown, theme::kBody, max_text);
    label = shown.c_str();
    const float text_w = env.fonts.semibold.measure(label, theme::kBody);
    const float w = 12.0f + glyph + 12.0f + text_w + 22.0f;
    const Rect r{x, cy - h * 0.5f, w, h};
    if (strong)
        list.rounded_rect(r, h * 0.5f, Color::rgb(0xffffff, 0.94f));
    else
        draw_glass_panel(list, env, r, h * 0.5f, theme::glass_fill);
    hui::ui::draw_button(list, env.fonts, hui::ui::GlyphStyle::dark(), button, x + 12.0f, cy, glyph);
    text(list, env.fonts.semibold, label, x + 12.0f + glyph + 12.0f, cy + 8.0f, theme::kBody,
         strong ? Color::rgb(0x0c0b11) : theme::ink);
    return w;
}

float draw_imdb(hui::gfx::DrawList &list, const Env &env, const std::string &rating, float x, float baseline)
{
    if (rating.empty())
        return 0.0f;
    const Rect badge{x, baseline - 25.0f, 62.0f, 32.0f};
    list.rounded_rect(badge, 7, theme::imdb);
    text(list, env.fonts.semibold, "IMDb", badge.cx(), baseline - 1.0f, 19.0f, Color::rgb(0x000000), Align::center);
    const float w = text(list, env.fonts.semibold, rating, x + 74.0f, baseline, theme::kBody + 2.0f, theme::ink);
    return 74.0f + w;
}

void fade_down(hui::gfx::DrawList &list, float x, float y, float w, float h, float strength, float solid)
{
    if (solid > 0.5f)
        list.rounded_rect({x, y, w, solid}, 0, theme::ground.with_alpha(strength));
    list.gradient_rect({x, y + solid, w, h - solid}, 0, theme::ground.with_alpha(strength),
                       theme::ground.with_alpha(0.0f));
}

void fade_up(hui::gfx::DrawList &list, float x, float y, float w, float h, float strength)
{
    list.gradient_rect({x, y, w, h}, 0, theme::ground.with_alpha(0.0f), theme::ground.with_alpha(strength));
}

} // namespace sx
