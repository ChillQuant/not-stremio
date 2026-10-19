#include "settings.hpp"

#include <algorithm>
#include <vector>

namespace sx
{

using hui::Direction;
using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::Rect;
using hui::ui::text;
namespace tween = hui::tween;

namespace
{

constexpr float kListX = theme::kLeft;
constexpr float kListW = 1140.0f;
constexpr float kRowH = 90.0f;
constexpr float kPitch = 100.0f;
constexpr float kTop = 196.0f;
constexpr Rect kHelp{1256.0f, 228.0f, 600.0f, 520.0f};

} // namespace

Rect Settings::row_rect(int index, bool final_position) const
{
    const float scroll = final_position ? scroll_.position.target : scroll_.offset();
    // Hidden rows (empty label) collapse: count only visible rows up to this index.
    int visible = 0;
    if (content_)
    {
        for (int i = 0; i < index && i < static_cast<int>(content_->rows.size()); ++i)
        {
            const auto &r = content_->rows[static_cast<std::size_t>(i)];
            if (!r.label.empty() || r.kind == SettingRow::Kind::header)
                ++visible;
        }
    }
    else
    {
        visible = index;
    }
    return {kListX, kTop + static_cast<float>(visible) * kPitch - scroll, kListW, kRowH};
}

bool Settings::is_header(int index) const
{
    return content_ && index >= 0 && index < static_cast<int>(content_->rows.size()) &&
           content_->rows[static_cast<std::size_t>(index)].kind == SettingRow::Kind::header;
}

// Rows with an empty label are hidden (e.g. "Sharpening strength" when the
// upscaler doesn't use it): never focused, never drawn.
bool Settings::is_selectable(int index) const
{
    return content_ && index >= 0 && index < static_cast<int>(content_->rows.size()) && !is_header(index) &&
           !content_->rows[static_cast<std::size_t>(index)].label.empty();
}

int Settings::first_selectable() const
{
    if (!content_)
        return 0;
    for (int i = 0; i < static_cast<int>(content_->rows.size()); ++i)
        if (is_selectable(i))
            return i;
    return 0;
}

void Settings::set_content(const SettingsContent *content)
{
    content_ = content;
    index_ = first_selectable();
    shown_ = -1;
    switches_.assign(content ? content->rows.size() : 0, {});
    if (content)
        for (std::size_t i = 0; i < content->rows.size(); ++i)
            switches_[i].snap(content->rows[i].on ? 1.0f : 0.0f);
    scroll_.position.snap(0.0f);
    ring_.snap(row_rect(index_, true));
    focus_on_.snap(1.0f);
}

void Settings::refresh()
{
    if (!content_)
        return;
    const std::size_t rows = content_->rows.size();
    const std::size_t before = switches_.size();
    switches_.resize(rows);
    for (std::size_t i = before; i < rows; ++i)
        switches_[i].snap(content_->rows[i].on ? 1.0f : 0.0f);
    index_ = std::min(index_, std::max(0, static_cast<int>(rows) - 1));
    if (!is_selectable(index_))
        index_ = first_selectable();
}

void Settings::update(const hui::InputFrame &input, float dt)
{
    tick(dt);
    if (!content_ || content_->rows.empty())
        return;
    const int count = static_cast<int>(content_->rows.size());
    if (!rail_input(input))
    {
        if (input.nav == Direction::up)
        {
            // Section headers are labels, hidden rows are skipped: skip over both.
            bool moved = false;
            for (int i = index_ - 1; i >= 0; --i)
                if (is_selectable(i))
                {
                    index_ = i;
                    moved = true;
                    break;
                }
            if (!moved)
                to_rail();
        }
        else if (input.nav == Direction::down)
        {
            for (int i = index_ + 1; i < count; ++i)
                if (is_selectable(i))
                {
                    index_ = i;
                    break;
                }
        }
        else if (input.nav == Direction::left)
            to_rail();
    }
    // The reveal must use the visible position, not the raw index: hidden rows
    // (empty labels) collapse in row_rect(), so index_ * kPitch would scroll
    // past the actual rows.
    const float top = (row_rect(index_, true).y + scroll_.position.target) - kTop;
    scroll_.reveal(top <= kPitch ? 0.0f : top, top + kRowH, 1080.0f - kTop - 60.0f, 14.0f);
    scroll_.update(dt, 18.0f);
    ring_.target(row_rect(index_, true));
    ring_.update(dt, 22.0f);
    focus_on_.target = rail_.focused() ? 0.0f : 1.0f;
    focus_on_.update(dt, 16.0f);
    for (std::size_t i = 0; i < switches_.size(); ++i)
    {
        switches_[i].target = content_->rows[i].on ? 1.0f : 0.0f;
        switches_[i].update(dt, 22.0f);
    }
    if (index_ != shown_)
    {
        shown_ = index_;
        swap_.start(0.28f);
    }
    // A row hidden since the last refresh (empty label) must not keep the cursor.
    if (!is_selectable(index_))
        index_ = first_selectable();
    swap_.update(dt);
}

// L1/R1 section jumping (WORKER 7): land on the first selectable row of the
// previous/next section header, wrapping at the ends. Hidden sections (the
// empty "Recent" header) are skipped.
void Settings::section_jump(int dir)
{
    if (!content_ || content_->rows.empty())
        return;
    const int count = static_cast<int>(content_->rows.size());
    std::vector<int> heads;
    for (int i = 0; i < count; ++i)
        if (content_->rows[static_cast<std::size_t>(i)].kind == SettingRow::Kind::header &&
            !content_->rows[static_cast<std::size_t>(i)].label.empty())
            heads.push_back(i);
    if (heads.empty())
        return;
    const int n = static_cast<int>(heads.size());
    int cur = 0;
    for (int h = 0; h < n; ++h)
        if (heads[static_cast<std::size_t>(h)] <= index_)
            cur = h;
    const int next = (cur + (dir >= 0 ? 1 : -1) + n) % n;
    for (int i = heads[static_cast<std::size_t>(next)] + 1; i < count; ++i)
        if (is_selectable(i))
        {
            index_ = i;
            // Scroll to the jumped-to row right away: when landing on the
            // first visible section, also bring its header into view.
            const float jtop = (row_rect(index_, true).y + scroll_.position.target) - kTop;
            const float reveal_top = (next == 0 && jtop <= kPitch) ? 0.0f : jtop;
            scroll_.reveal(reveal_top, jtop + kRowH, 1080.0f - kTop - 60.0f, 14.0f);
            return;
        }
    // A section with no selectable rows: stay put rather than strand the
    // cursor on a header (update() would yank it back to the top).
}

void Settings::draw_help(hui::gfx::DrawList &fg, const SettingRow &row) const
{
    const float in = tween::smoothstep(swap_.progress());
    const float panel_in = tween::stagger(age_, 2, 0.05f, 0.5f);
    fg.push_opacity(panel_in);
    draw_glass_panel(fg, env_, kHelp, 36.0f, Color::rgb(0xffffff, 0.05f));
    const float x = kHelp.x + 38.0f;
    const float w = kHelp.w - 76.0f;
    fg.push_opacity(in);
    const float rise = 8.0f * (1.0f - in);
    const hui::ui::FontRef &display = env_.fonts.display;
    const std::vector<std::string> &wl = cached_wrap(row.text_cache_, display.font, row.label, 36.0f, w);
    float baseline = kHelp.y + 76.0f + rise;
    for (std::size_t i = 0; i < wl.size() && i < 2; ++i)
    {
        text(fg, display, wl[i], x, baseline, 36.0f, theme::ink, Align::left, -0.8f);
        baseline += 44.0f;
    }
    // The current value, in the page's violet.
    std::string value = row.value;
    if (row.kind == SettingRow::Kind::toggle)
        value = row.on ? "On" : "Off";
    text(fg, env_.fonts.semibold, cached_fit(row.text_cache_, env_.fonts.semibold.font, value, 26.0f, w), x, baseline + 4.0f, 26.0f,
         theme::accent_soft);
    cached_paragraph(fg, row.text_cache_, env_.fonts.regular, row.help, x, baseline + 54.0f, 23.0f, w, 34.0f, theme::ink_2, 6);
    const char *verb = row.kind == SettingRow::Kind::toggle ? "Switch"
                       : row.kind == SettingRow::Kind::action  ? "Open"
                       : row.kind == SettingRow::Kind::slider ? "Adjust"
                                                              : "Change";
    draw_chip(fg, env_, hui::ui::Button::cross, verb, x, kHelp.y + kHelp.h - 56.0f, true);
    fg.pop_opacity();
    fg.pop_opacity();
}

void Settings::draw(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, hui::gfx::BackdropSpec &backdrop) const
{
    apply_page_backdrop(backdrop, clock_);
    if (!content_)
        return;
    const float in = tween::stagger(age_, 0, 0.0f, 0.45f);
    bg.push_opacity(in);
    text(bg, env_.fonts.semibold, "Settings", theme::kLeft, 158.0f, theme::kHeading, theme::ink_2);
    bg.pop_opacity();

    bg.push_clip({theme::kLeft - 44.0f, 188.0f, 1228.0f, 1080.0f - 188.0f});
    fg.push_clip({theme::kLeft - 44.0f, 188.0f, 1228.0f, 1080.0f - 188.0f});
    const Rect ring = ring_.value();
    if (focus_on_.value > 0.01f)
    {
        fg.push_opacity(focus_on_.value);
        fg.glow(ring, 24.0f, 26.0f, theme::accent.with_alpha(0.34f));
        fg.pop_opacity();
    }
    for (int i = 0; i < static_cast<int>(content_->rows.size()); ++i)
    {
        const Rect r = row_rect(i, false);
        if (r.y > 1080.0f || r.y + r.h < 180.0f)
            continue;
        const SettingRow &row = content_->rows[i];
        if (row.label.empty() && row.kind != SettingRow::Kind::header)
            continue; // hidden row (e.g. sharpening when upscaler != FSR1+Sharpening)
        const bool focused = i == index_ && !rail_.focused();
        const float enter = tween::stagger(age_, i, 0.04f, 0.4f);
        // The focused row is a glass panel: it and its contents draw in the
        // foreground layer so they sample the blur; the rest stays put.
        hui::gfx::DrawList &rl = (focused && row.kind != SettingRow::Kind::header) ? fg : bg;
        rl.push_opacity(enter);
        if (row.kind == SettingRow::Kind::header)
        {
            // A section label: dimmer, no card, no chevron, never focused.
            text(rl, env_.fonts.semibold, row.label, r.x + 34.0f, r.cy() + 8.0f, 24.0f, theme::ink_3);
        }
        else
        {
            rl.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 14.0f * (1.0f - enter));
            if (focused)
                draw_glass_panel(rl, env_, r, 24.0f, theme::glass_fill);
            else
                rl.rounded_rect(r, 24.0f, theme::surface);
            text(rl, env_.fonts.semibold, row.label, r.x + 34.0f, r.cy() + 10.0f, 28.0f, focused ? theme::ink : theme::ink_2);
            const float right = r.x + r.w - 34.0f;
            if (row.kind == SettingRow::Kind::toggle)
            {
                draw_switch(rl, right - 34.0f, r.cy(), switches_[i].value);
            }
            else if (row.kind == SettingRow::Kind::slider)
            {
                // A horizontal bar showing the fill, then the % value. No
                // chevron: Left/Right adjusts it in place.
                const float pct = std::clamp(row.slider_pct, 0, 100) / 100.0f;
                const float bw = 220.0f, bh = 8.0f;
                const float bx = right - 80.0f - bw;
                const float by = r.cy() - bh * 0.5f;
                rl.rounded_rect({bx, by, bw, bh}, bh * 0.5f, Color::rgb(0xffffff, 0.14f));
                if (pct > 0.0f)
                    rl.rounded_rect({bx, by, bw * pct, bh}, bh * 0.5f,
                                    focused ? theme::accent : theme::accent_soft);
                text(rl, env_.fonts.regular, row.value, right, r.cy() + 9.0f, 26.0f,
                     focused ? theme::accent_soft : theme::ink_3, Align::right);
            }
            else
            {
                draw_chevron(rl, right - 8.0f, r.cy(), 16.0f, 1, focused ? theme::ink : theme::ink_3);
                text(rl, env_.fonts.regular, cached_fit(row.text_cache_, env_.fonts.regular.font, row.value, 26.0f, 420.0f), right - 40.0f, r.cy() + 9.0f,
                     26.0f, focused ? theme::accent_soft : theme::ink_3, Align::right);
            }
            rl.pop_transform();
        }
        rl.pop_opacity();
    }
    if (focus_on_.value > 0.01f)
    {
        fg.push_opacity(focus_on_.value);
        fg.bordered_rect(ring, 24.0f, Color::rgb(0xffffff, 0.0f), 4.0f, Color::rgb(0xffffff));
        fg.pop_opacity();
    }
    bg.pop_clip();
    fg.pop_clip();

    if (index_ >= 0 && index_ < static_cast<int>(content_->rows.size()))
        draw_help(fg, content_->rows[index_]);

    fade_down(bg, theme::kRailWidth, 0.0f, theme::kScreenW, 118.0f, 0.62f);
    fade_up(bg, theme::kRailWidth, 1000.0f, theme::kScreenW, 80.0f, 0.94f);
    draw_topbar(bg, fg, env_, false, age_);
    rail_.draw(fg, env_, age_);
}

} // namespace sx
