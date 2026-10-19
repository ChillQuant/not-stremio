#include "player.hpp"

#include "cards.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

// Accessibility overrides (WORKER 8, gl/a11y.hpp): read-only here. When the
// header is absent the compiled-in defaults below apply (2px outline at
// 1080p, no forced background box, white color preset).
#if __has_include("a11y.hpp")
#include "a11y.hpp"
#endif
#include "subtitles.h"

namespace sx
{

using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::Rect;
using hui::ui::text;
namespace tween = hui::tween;

namespace
{

std::string clock_text(double seconds)
{
    const long total = static_cast<long>(std::max(0.0, seconds));
    char out[24];
    if (total >= 3600)
        std::snprintf(out, sizeof(out), "%ld:%02ld:%02ld", total / 3600, total / 60 % 60, total % 60);
    else
        std::snprintf(out, sizeof(out), "%ld:%02ld", total / 60, total % 60);
    return out;
}

// Wraps one cue line to the subtitle width: at most two lines, the second
// ellipsized if text remains. Splits on ASCII spaces, which never appear
// inside a UTF-8 sequence; overlong single words are ellipsized by the font.
// Thai (U+0E00-U+0E7F) has no spaces: long Thai lines are wrapped by
// character instead of being truncated.
static std::size_t utf8_char_len(const char *p)
{
    unsigned char c = static_cast<unsigned char>(p[0]);
    if (c < 0x80)
        return 1;
    if (c < 0xE0)
        return 2;
    if (c < 0xF0)
        return 3;
    return 4;
}

static bool is_thai_char(const char *p, std::size_t remaining)
{
    if (remaining < 3)
        return false;
    unsigned char b0 = static_cast<unsigned char>(p[0]);
    unsigned char b1 = static_cast<unsigned char>(p[1]);
    unsigned char b2 = static_cast<unsigned char>(p[2]);
    return b0 == 0xE0 && (b1 == 0xB8 || b1 == 0xB9) && b2 >= 0x80 && b2 <= 0xBF;
}

// Decodes one UTF-8 codepoint. Caller must ensure the bytes are valid.
static std::uint32_t utf8_codepoint(const char *p)
{
    unsigned char c0 = static_cast<unsigned char>(p[0]);
    if (c0 < 0x80)
        return c0;
    if (c0 < 0xE0)
        return (static_cast<std::uint32_t>(c0 & 0x1F) << 6) | (p[1] & 0x3F);
    if (c0 < 0xF0)
        return (static_cast<std::uint32_t>(c0 & 0x0F) << 12) | ((static_cast<std::uint32_t>(p[1] & 0x3F) << 6)) |
               (p[2] & 0x3F);
    return (static_cast<std::uint32_t>(c0 & 0x07) << 18) | ((static_cast<std::uint32_t>(p[1] & 0x3F) << 12)) |
           ((static_cast<std::uint32_t>(p[2] & 0x3F) << 6)) | (p[3] & 0x3F);
}

// Thai combining marks: must not start a line (they anchor to the previous
// base consonant). U+0E31, U+0E34-U+0E3A (vowels), U+0E47-U+0E4E (tone marks).
static bool is_thai_combining_mark(std::uint32_t cp)
{
    return cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E3A) || (cp >= 0x0E47 && cp <= 0x0E4E) ||
           cp == 0x0E30 || cp == 0x0E32 || cp == 0x0E33 || cp == 0x0E45 || cp == 0x0E46;
}

// Thai leading vowels (U+0E40-U+0E44): written before the consonant, must not
// end a line (they would strand without their base).
static bool is_thai_leading_vowel(std::uint32_t cp)
{
    return cp >= 0x0E40 && cp <= 0x0E44;
}

// Splits text into grapheme clusters for Thai: a base character plus its
// combining marks, with leading vowels kept together with their base.
// Non-Thai characters are single-char clusters.
static void split_thai_clusters(const std::string &text, std::vector<std::string> &out)
{
    for (std::size_t i = 0; i < text.size();)
    {
        std::size_t cl = utf8_char_len(text.data() + i);
        if (i + cl > text.size())
            cl = text.size() - i;
        std::uint32_t cp = utf8_codepoint(text.data() + i);
        std::string cluster = text.substr(i, cl);
        i += cl;
        // A leading vowel must stay with the following base character.
        if (cp >= 0x0E00 && cp <= 0x0E7F && is_thai_leading_vowel(cp) && i < text.size())
        {
            cl = utf8_char_len(text.data() + i);
            if (i + cl > text.size())
                cl = text.size() - i;
            cluster += text.substr(i, cl);
            i += cl;
        }
        // Attach following combining marks to the cluster.
        while (i < text.size())
        {
            cl = utf8_char_len(text.data() + i);
            if (i + cl > text.size())
                break;
            std::uint32_t ncp = utf8_codepoint(text.data() + i);
            if (ncp < 0x0E00 || ncp > 0x0E7F || !is_thai_combining_mark(ncp))
                break;
            cluster += text.substr(i, cl);
            i += cl;
        }
        out.push_back(cluster);
    }
}

void wrap_subtitle_line(const hui::gfx::Font *font, const std::string &text, float size, float max_w,
                        std::vector<std::string> &out)
{
    if (text.empty() || font->measure(text, size) <= max_w)
    {
        out.push_back(text);
        return;
    }
    // Thai path: no spaces in Thai, so wrap by grapheme cluster. Breaking
    // between clusters (never before a combining mark, never after a leading
    // vowel) keeps marks anchored to their base consonant.
    bool has_thai = false;
    for (std::size_t i = 0; i < text.size();)
    {
        if (is_thai_char(text.data() + i, text.size() - i))
        {
            has_thai = true;
            break;
        }
        i += utf8_char_len(text.data() + i);
    }
    if (has_thai && text.find(' ') == std::string::npos)
    {
        std::vector<std::string> clusters;
        split_thai_clusters(text, clusters);
        std::size_t ci = 0;
        for (int row = 0; row < 2 && ci < clusters.size(); ++row)
        {
            std::string line;
            while (ci < clusters.size())
            {
                const std::string trial = line + clusters[ci];
                if (!line.empty() && font->measure(trial, size) > max_w)
                    break;
                line = trial;
                ++ci;
            }
            if (row == 1 && ci < clusters.size())
            {
                // Second line overflow: ellipsize line + remainder.
                std::string rest;
                for (; ci < clusters.size(); ++ci)
                    rest += clusters[ci];
                line = font->fit(line + rest, size, max_w);
                ci = clusters.size();
            }
            out.push_back(line);
        }
        return;
    }
    std::vector<std::string> words;
    for (std::size_t s = 0; s < text.size();)
    {
        std::size_t e = text.find(' ', s);
        if (e == std::string::npos)
            e = text.size();
        if (e > s)
            words.push_back(text.substr(s, e - s));
        s = e + 1;
    }
    std::size_t wi = 0;
    for (int row = 0; row < 2 && wi < words.size(); ++row)
    {
        std::string line;
        while (wi < words.size())
        {
            const std::string trial = line.empty() ? words[wi] : line + " " + words[wi];
            if (font->measure(trial, size) > max_w)
                break;
            line = trial;
            ++wi;
        }
        if (line.empty() && wi < words.size())
        {
            // A single word wider than max_w: if it contains Thai, break it
            // by grapheme cluster instead of truncating (mixed Thai/English
            // cues like "สวัสดีครับ John" would otherwise lose text).
            bool word_has_thai = false;
            for (std::size_t k = 0; k < words[wi].size();)
            {
                if (is_thai_char(words[wi].data() + k, words[wi].size() - k))
                {
                    word_has_thai = true;
                    break;
                }
                k += utf8_char_len(words[wi].data() + k);
            }
            if (word_has_thai)
            {
                std::vector<std::string> clusters;
                split_thai_clusters(words[wi], clusters);
                std::string part;
                std::size_t used = 0;
                for (; used < clusters.size(); ++used)
                {
                    const std::string trial = part + clusters[used];
                    if (!part.empty() && font->measure(trial, size) > max_w)
                        break;
                    part = trial;
                }
                if (part.empty())
                {
                    // Even one cluster doesn't fit: fall back to fit().
                    part = font->fit(words[wi], size, max_w);
                    ++wi;
                }
                else if (used < clusters.size())
                {
                    // Push the remainder back as a new word for the next row.
                    std::string rest;
                    for (; used < clusters.size(); ++used)
                        rest += clusters[used];
                    words[wi] = rest;
                    // Don't advance wi; the remainder will be processed next.
                }
                else
                {
                    ++wi;
                }
                line = part;
            }
            else
            {
                line = font->fit(words[wi], size, max_w);
                ++wi;
            }
        }
        if (row == 1 && wi < words.size())
        {
            std::string tail = line;
            for (; wi < words.size(); ++wi)
                tail += " " + words[wi];
            line = font->fit(tail, size, max_w);
        }
        out.push_back(line);
    }
}

// Accessibility reads for subtitle rendering; compiled-in defaults when
// gl/a11y.hpp is absent.
namespace
{
int a11y_sub_outline_min_px()
{
#if __has_include("a11y.hpp")
    return ::A11y::get().sub_outline_min_px;
#else
    return 2;
#endif
}
bool a11y_sub_bg_box()
{
#if __has_include("a11y.hpp")
    return ::A11y::get().sub_bg_box;
#else
    return false;
#endif
}
int a11y_sub_color_preset()
{
#if __has_include("a11y.hpp")
    return ::A11y::get().sub_color_preset;
#else
    return 0; // white
#endif
}

float a11y_sub_size_scale()
{
#if __has_include("a11y.hpp")
    return ::A11y::get().sub_size_scale;
#else
    return 1.0f;
#endif
}

// Transient "Subtitle delay +0.25 s" badge, drawn next to the subtitles.
void draw_delay_badge(hui::gfx::DrawList &list, const hui::ui::FontRef &font, float y, float alpha)
{
    const int ms = Subtitles::delay_ms();
    char badge[64];
    std::snprintf(badge, sizeof(badge), "Subtitle delay %s%d.%02d s", ms < 0 ? "-" : "+", std::abs(ms) / 1000,
                  (std::abs(ms) % 1000) / 10);
    draw_outlined_text(list, font, badge, theme::kScreenW * 0.5f, y, 30.0f, Color::rgb(0xffffff, alpha), Align::center);
}
} // namespace

} // namespace

void draw_outlined_text(hui::gfx::DrawList &list, const hui::ui::FontRef &font, const std::string &value, float x,
                        float baseline, float size, Color colour, Align align)
{
    // Readability floor: the outline is drawn in 1080p-equivalent UI units,
    // so the minimum scales with the video/UI height rather than raw pixels.
    // "As authored" keeps the legacy behavior; Readable (the default) and
    // Plain enforce at least ~2px and never a fully-transparent outline,
    // regardless of what the ASS style asked for. The clamp feeds the
    // existing atlas rasterizer — no extra passes.
    // A11y "Outline: Off" (0px) skips the outline entirely.
    const int outline_min = a11y_sub_outline_min_px();
    if (outline_min <= 0 && Subtitles::ass_mode() != AssMode::authored)
    {
        text(list, font, value, x, baseline, size, colour, align);
        return;
    }
    float r = size * 0.055f;
    float outline_alpha = 0.92f * colour.a;
    if (Subtitles::ass_mode() != AssMode::authored)
    {
        r = std::max(r, static_cast<float>(outline_min));
        outline_alpha = std::max(outline_alpha, 0.75f);
    }
    else
    {
        r = std::max(r, 2.0f);
    }
    static const float kDir[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
    for (const auto &d : kDir)
        text(list, font, value, x + d[0] * r, baseline + d[1] * r, size, Color::rgb(0x000000, outline_alpha), align);
    text(list, font, value, x, baseline, size, colour, align);
}

void PlayerUi::snap()
{
    controls_.snap(state.controls ? 1.0f : 0.0f);
    menu_.snap(state.menu ? 1.0f : 0.0f);
    paused_.snap(state.paused ? 1.0f : 0.0f);
    buffering_.snap(state.buffering >= 0 ? 1.0f : 0.0f);
    upnext_.snap(!state.upnext_title.empty() ? 1.0f : 0.0f);
    binge_.snap(state.binge_prompt ? 1.0f : 0.0f);
    skip_.snap(!state.skip_prompt.empty() ? 1.0f : 0.0f);
    skip_y_.snap(state.upnext_title.empty() ? 688.0f : 548.0f);
}

void PlayerUi::update(float dt)
{
    clock_ += dt;
    controls_.target = state.controls || state.paused || state.menu ? 1.0f : 0.0f;
    controls_.update(dt, 16.0f);
    menu_.target = state.menu ? 1.0f : 0.0f;
    menu_.update(dt, 18.0f);
    paused_.target = state.paused ? 1.0f : 0.0f;
    paused_.update(dt, 20.0f);
    buffering_.target = state.buffering >= 0 ? 1.0f : 0.0f;
    buffering_.update(dt, 14.0f);
    upnext_.target = !state.upnext_title.empty() ? 1.0f : 0.0f;
    upnext_.update(dt, 18.0f);
    binge_.target = state.binge_prompt ? 1.0f : 0.0f;
    binge_.update(dt, 18.0f);
    skip_.target = !state.skip_prompt.empty() ? 1.0f : 0.0f;
    skip_.update(dt, 18.0f);
    skip_y_.target = state.upnext_title.empty() ? 688.0f : 548.0f;
    skip_y_.update(dt, 14.0f);
}

void PlayerUi::draw_subtitle(hui::gfx::DrawList &list) const
{
    // One-press sync readout: when the delay is nudged (L2/R2), show a
    // transient badge next to the subtitles — the session's toast sits at the
    // top of the screen, away from where the user is looking while reading.
    // Fired by Subtitles::note_delay_ms, which nudge_sub_delay
    // (session_watch.cpp) calls; the nudge itself is untouched.
    const unsigned dser = Subtitles::delay_serial();
    if (dser != seen_delay_serial_)
    {
        seen_delay_serial_ = dser;
        delay_badge_until_ = clock_ + 2.5f;
    }
    const bool badge_active = clock_ < delay_badge_until_;
    const float base_size = state.sub_size == 0 ? 42.0f : state.sub_size == 2 ? 60.0f : 50.0f;
    const float size = base_size * a11y_sub_size_scale();
    const float leading = size * 1.16f;
    // A whole-cue ASS color outranks the yellow preference; a cue that mixes
    // colors keeps the default rather than guessing wrong (see plain_cue).
    // The accessibility color preset (0 white, 1 yellow, 2 cyan) and the
    // settings' white/yellow choice share presets.
    const int color_preset = a11y_sub_color_preset();
    const int eff_color = state.sub_color != 0 ? state.sub_color : color_preset;
    const Color color = state.sub_fg >= 0 ? Color::rgb(static_cast<std::uint32_t>(state.sub_fg))
#if __has_include("a11y.hpp")
                                          : Color::rgb(::A11y::sub_text_rgb(eff_color));
#else
                                          : (eff_color == 1 ? Color::rgb(0xffe14d) : Color::rgb(0xffffff));
#endif
    const float lift = state.sub_raised ? 120.0f : 0.0f;
    // Subtitles rise out of the way of the controls, and above the bottom-right
    // cards (up-next at y 688; the skip prompt stacks above it at y 548) so
    // dialogue never slides under them.
    float raise = 150.0f * tween::smoothstep(controls_.value);
    float card_top = 1080.0f;
    if (!state.upnext_title.empty())
        card_top = 688.0f;
    if (!state.skip_prompt.empty())
        card_top = state.upnext_title.empty() ? 688.0f : 548.0f; // mirror the card's own y
    if (card_top < 1080.0f)
        raise = std::max(raise, 960.0f - lift - (card_top - 28.0f) + size * 0.3f);
    if (badge_active && state.line.empty() && state.line2.empty())
    {
        // Nudged with no cue on screen: the badge takes the default slot.
        draw_delay_badge(list, env_.fonts.semibold, 960.0f - lift - 124.0f, 1.0f);
        return;
    }
    if (state.line.empty() && state.line2.empty())
        return;
    // Cached: a cue stays on screen 2-5s, so reuse the wrapped lines.
    // The a11y size scale is part of the key: changing it re-wraps.
    const int size_key = state.sub_size * 1000 + int(a11y_sub_size_scale() * 100.0f);
    if (state.line != cached_sub_line_ || size_key != cached_sub_size_)
    {
        // Lookback history: every newly drawn cue is remembered for
        // Subtitles::recent_lines ("what did they just say?").
        if (!state.line.empty() && state.line != cached_sub_line_)
            Subtitles::note_shown(state.line);
        cached_sub_wrapped_.clear();
        for (std::size_t start = 0; start <= state.line.size();)
        {
            std::size_t end = state.line.find('\n', start);
            if (end == std::string::npos)
                end = state.line.size();
            wrap_subtitle_line(env_.fonts.semibold.font, state.line.substr(start, end - start), size,
                               theme::kScreenW - 220.0f, cached_sub_wrapped_);
            start = end + 1;
        }
        cached_sub_line_ = state.line;
        cached_sub_size_ = size_key;
    }
    const std::vector<std::string> &lines = cached_sub_wrapped_;
    // Anchor for the transient delay readout: just above the cue when one is
    // showing, else the default subtitle slot.
    float badge_y = 960.0f - lift - 124.0f;
    if (!state.line.empty() && !lines.empty()) // over-wide all-whitespace cue: nothing to draw
    {
        // ASS \an alignment: 7/8/9 pin to the top (signs), 1/4/7 left, 3/6/9 right.
        const int an = state.sub_an;
        const float sx = an == 1 || an == 4 || an == 7 ? 110.0f : an == 3 || an == 6 || an == 9 ? 1810.0f
                                                                                             : theme::kScreenW * 0.5f;
        const Align salign = an == 1 || an == 4 || an == 7 ? Align::left
                             : an == 3 || an == 6 || an == 9 ? Align::right
                                                            : Align::center;
        float baseline = an >= 7 ? 220.0f : 960.0f - lift - raise - leading * static_cast<float>(lines.size() - 1);
        badge_y = baseline - size - 44.0f; // delay readout sits just above the cue
        // The accessibility background box forces the standard darkening even
        // when the user left the opacity at 0.
        const int bg_opacity = state.sub_bg_opacity > 0 ? state.sub_bg_opacity : (a11y_sub_bg_box() ? 55 : 0);
        if (bg_opacity > 0)
        {
            // One merged background for the whole cue. Per-line boxes are taller
            // (1.52x the font size) than the line spacing (1.16x), so with two
            // lines the boxes overlapped and doubled the darkening between them.
            float wmax = 0.0f;
            for (const std::string &line : lines)
                wmax = std::max(wmax, env_.fonts.semibold.font->measure(line, size));
            const float px = size * 0.35f, pt = size * 0.22f, pb = size * 0.30f;
            const float last = baseline + leading * static_cast<float>(lines.size() - 1);
            const float bx = salign == Align::left ? sx - px : salign == Align::right ? sx - wmax - px : sx - wmax * 0.5f - px;
            list.rounded_rect({bx, baseline - size - pt, wmax + px * 2.0f, last + pb - (baseline - size - pt)},
                              size * 0.16f, Color::rgb(0x000000, bg_opacity / 100.0f));
        }
        for (const std::string &line : lines)
        {
            draw_outlined_text(list, env_.fonts.semibold, line, sx, baseline, size, color, salign);
            baseline += leading;
        }
    }

    // Dual subtitles: the secondary cue lives near the top of the screen,
    // smaller, always centered, clear of the top OSD. Same wrap caching.
    if (!state.line2.empty())
    {
        const float size2 = size * 0.8f;
        const float leading2 = size2 * 1.16f;
        const Color color2 = state.sub2_fg >= 0 ? Color::rgb(static_cast<std::uint32_t>(state.sub2_fg)) : color;
        if (state.line2 != cached_sub2_line_ || size_key != cached_sub2_size_)
        {
            if (!state.line2.empty() && state.line2 != cached_sub2_line_)
                Subtitles::note_shown(state.line2);
            cached_sub2_wrapped_.clear();
            for (std::size_t start = 0; start <= state.line2.size();)
            {
                std::size_t end = state.line2.find('\n', start);
                if (end == std::string::npos)
                    end = state.line2.size();
                wrap_subtitle_line(env_.fonts.semibold.font, state.line2.substr(start, end - start), size2,
                                   theme::kScreenW - 220.0f, cached_sub2_wrapped_);
                start = end + 1;
            }
            cached_sub2_line_ = state.line2;
            cached_sub2_size_ = size_key;
        }
        const std::vector<std::string> &lines2 = cached_sub2_wrapped_;
        float baseline2 = 150.0f;
        for (const std::string &line : lines2)
        {
            draw_outlined_text(list, env_.fonts.semibold, line, theme::kScreenW * 0.5f, baseline2, size2, color2,
                               Align::center);
            baseline2 += leading2;
        }
    }

    if (badge_active)
    {
        const float fade = std::min(1.0f, (delay_badge_until_ - clock_) / 0.5f);
        draw_delay_badge(list, env_.fonts.semibold, badge_y, fade);
    }
}

void PlayerUi::draw_upnext(hui::gfx::DrawList &fg) const
{
    if (state.upnext_title.empty())
        return;
    // Netflix-style card, bottom right: what is coming, the countdown, and the way out.
    // Fades and slides up on entry.
    const float t = upnext_.value;
    fg.push_opacity(t);
    const float cw = 560.0f, ch = 216.0f;
    const float x = theme::kScreenW - cw - 80.0f, y = 688.0f + 24.0f * (1.0f - t);
    draw_glass_panel(fg, env_, {x, y, cw, ch}, 20.0f, Color::rgb(0x000000, 0.72f));
    text(fg, env_.fonts.semibold, "Up next", x + 36.0f, y + 54.0f, 24.0f, theme::accent_soft);
    text(fg, env_.fonts.semibold, env_.fonts.semibold.font->fit(state.upnext_title, 30.0f, cw - 72.0f), x + 36.0f,
         y + 100.0f, 30.0f, theme::ink);
    if (state.upnext_remaining >= 0)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Starting in %d",
                      std::max(1, static_cast<int>(std::ceil(state.upnext_remaining))));
        text(fg, env_.fonts.regular, buf, x + 36.0f, y + 146.0f, 26.0f, theme::ink_2);
    }
    // The fast way out: Cross jumps straight to the next episode.
    const float bw = 168.0f, bh = 44.0f, bx = x + 36.0f, by = y + 160.0f;
    draw_glass_panel(fg, env_, {bx, by, bw, bh}, bh * 0.5f, theme::glass_fill);
    hui::ui::draw_button(fg, env_.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::cross, bx + 24.0f, by + bh * 0.5f,
                         30.0f);
    text(fg, env_.fonts.semibold, "Next", bx + 64.0f, by + 30.0f, 24.0f, theme::ink);
    text(fg, env_.fonts.regular, "Any other button cancels", bx + bw + 18.0f, by + 30.0f, 22.0f, theme::ink_3);
    fg.pop_opacity();
}

void PlayerUi::draw_binge(hui::gfx::DrawList &fg) const
{
    if (!state.binge_prompt)
        return;
    // Centered card: the binge guard is asking whether anyone is still there.
    // Fades and scales up on entry.
    const float t = binge_.value;
    fg.push_opacity(t);
    const float cw = 660.0f, ch = 300.0f;
    const float x = (theme::kScreenW - cw) * 0.5f, y = (theme::kScreenH - ch) * 0.5f;
    const float cx = x + cw * 0.5f;
    fg.push_transform(0.96f + 0.04f * t, cx, y + ch * 0.5f, 0.0f, 0.0f);
    draw_glass_panel(fg, env_, {x, y, cw, ch}, 24.0f, Color::rgb(0x000000, 0.85f));
    text(fg, env_.fonts.semibold, "Still watching?", cx, y + 92.0f, 44.0f, theme::ink, Align::center);
    const float bw = 240.0f, bh = 56.0f, bx = cx - bw * 0.5f, by = y + 140.0f;
    draw_glass_panel(fg, env_, {bx, by, bw, bh}, bh * 0.5f, Color::rgb(0xffffff, 0.08f));
    hui::ui::draw_button(fg, env_.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::cross, bx + 36.0f,
                         by + bh * 0.5f, 32.0f);
    text(fg, env_.fonts.semibold, "Continue", bx + 84.0f, by + 38.0f, 26.0f, theme::ink);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Stopping in %d",
                  std::max(1, static_cast<int>(std::ceil(state.binge_left))));
    text(fg, env_.fonts.regular, buf, cx, y + 258.0f, 24.0f, theme::ink_3, Align::center);
    fg.pop_transform();
    fg.pop_opacity();
}

void PlayerUi::draw_skip_prompt(hui::gfx::DrawList &fg) const
{
    if (state.skip_prompt.empty())
        return;
    // Bottom-right card, mirroring the up-next card. Stacked above it when
    // both are visible (an outro in the last 20 s of the episode).
    // Fades in on entry; the y glides instead of teleporting when up-next appears.
    const float t = skip_.value;
    fg.push_opacity(t);
    const float cw = 420.0f, ch = 120.0f;
    const float x = theme::kScreenW - cw - 80.0f;
    const float y = skip_y_.value;
    draw_glass_panel(fg, env_, {x, y, cw, ch}, 20.0f, Color::rgb(0x000000, 0.72f));
    hui::ui::draw_button(fg, env_.fonts, hui::ui::GlyphStyle::dark(), hui::ui::Button::triangle, x + 36.0f,
                         y + ch * 0.5f, 44.0f);
    text(fg, env_.fonts.semibold, state.skip_prompt, x + 100.0f, y + 50.0f, 30.0f, theme::ink);
    text(fg, env_.fonts.regular, "Skip", x + 100.0f, y + 86.0f, 22.0f, theme::ink_2);
    fg.pop_opacity();
}

void PlayerUi::draw_volume_osd(hui::gfx::DrawList &fg) const
{
    if (state.volume_osd < 0)
        return;
    const float pw = 520.0f, ph = 96.0f;
    const float x = (theme::kScreenW - pw) * 0.5f;
    const float y = 120.0f;
    draw_glass_panel(fg, env_, {x, y, pw, ph}, 24.0f, Color::rgb(0x000000, 0.72f));
    text(fg, env_.fonts.semibold, "Volume", x + 36.0f, y + 57.5f, 28.0f, theme::ink);
    const float bx = x + 176.0f, bw = 224.0f, bh = 12.0f;
    const float by = y + (ph - bh) * 0.5f;
    fg.rounded_rect({bx, by, bw, bh}, 6.0f, Color::rgb(0xffffff, 0.25f));
    const float fill = bw * std::clamp(state.volume_osd / 100.0f, 0.0f, 1.0f);
    const Color bar_col = state.volume_osd > 100 ? theme::accent_soft : Color::rgb(0xffffff, 0.9f);
    if (fill > 0.5f)
        fg.rounded_rect({bx, by, fill, bh}, 6.0f, bar_col);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", state.volume_osd);
    text(fg, env_.fonts.regular, buf, x + pw - 36.0f, y + 57.5f, 28.0f, theme::ink, Align::right);
}

void PlayerUi::draw_controls(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, float amount) const
{
    bg.push_opacity(amount);
    fg.push_opacity(amount);
    bg.gradient_rect({0, 0, theme::kScreenW, 240.0f}, 0, Color::rgb(0x000000, 0.72f), Color::rgb(0x000000, 0.0f));
    bg.gradient_rect({0, 640.0f, theme::kScreenW, 440.0f}, 0, Color::rgb(0x000000, 0.0f), Color::rgb(0x000000, 0.86f));

    // The stats-for-nerds panel sits top-right (x 1260-1880): keep a long
    // title from running under it.
    const float title_w = state.nerd_overlay ? 1100.0f : 1500.0f;
    text(bg, env_.fonts.semibold, env_.fonts.semibold.font->fit(state.title, 42.0f, title_w), 110.0f, 108.0f, 42.0f,
         theme::ink, Align::left, -0.4f);
    if (!state.subtitle.empty())
        text(bg, env_.fonts.regular, env_.fonts.regular.font->fit(state.subtitle, 28.0f, title_w), 110.0f, 152.0f,
             28.0f, theme::ink_2);
    // Hardware/format badges from the session's signal analysis
    // (state.badges: {"4K", "HDR10", "HW Decode", ...}).
    {
        float bx = 110.0f;
        const float by = 192.0f;
        for (const std::string &badge : state.badges)
        {
            Color fill = Color::rgb(0xffffff, 0.09f), ink = theme::ink_2;
            if (badge == "4K" || badge == "Dolby Vision" || badge == "HDR10" || badge == "HDR10+" ||
                badge == "HLG")
            {
                fill = Color::rgb(0xf5c518, 0.22f);
                ink = Color::rgb(0xffdf6b);
            }
            else if (badge == "HW Decode")
            {
                fill = theme::accent.with_alpha(0.24f);
                ink = theme::accent_soft;
            }
            bx += draw_tag(bg, env_, badge, bx, by, fill, ink) + 10.0f;
        }
    }

    // The seek bar.
    const float x = 110.0f, w = 1700.0f, y = 862.0f;
    const double duration = std::max(1.0, state.duration);
    const float played = static_cast<float>(std::clamp(state.position / duration, 0.0, 1.0));
    const float ready = static_cast<float>(std::clamp((state.position + state.buffered) / duration, 0.0, 1.0));
    bg.rounded_rect({x, y, w, 8.0f}, 4.0f, Color::rgb(0xffffff, 0.62f));
    bg.rounded_rect({x, y, w * std::max(ready, played), 8.0f}, 4.0f, Color::rgb(0xffffff, 0.80f));
    // Intro/outro bands: over the track and the buffered fill, under the played progress,
    // so they stay visible in buffered regions.
    for (const auto &sg : state.skip_segments)
    {
        const float x0 = x + w * static_cast<float>(std::clamp(sg.start / duration, 0.0, 1.0));
        const float x1 = x + w * static_cast<float>(std::clamp(sg.end / duration, 0.0, 1.0));
        if (x1 > x0 + 1.0f)
            bg.rounded_rect({x0, y, x1 - x0, 8.0f}, 4.0f, Color::rgb(0xffb347, 0.85f));
    }
    bg.glow({x, y, w * played, 8.0f}, 4.0f, 12.0f, theme::accent.with_alpha(0.55f));
    bg.rounded_rect({x, y, w * played, 8.0f}, 4.0f, theme::accent_soft);
    bg.circle(x + w * played, y + 4.0f, 14.0f, Color::rgb(0xffffff));
    // Seek-preview bubble: while a D-pad/L1/R1 seek is in flight, show the
    // target time above the bar at the target position. (A real thumbnail
    // would need a parallel decoder; the player decodes sequentially, so a
    // time label is the honest version.)
    if (state.seek_preview >= 0)
    {
        const float tx = x + w * static_cast<float>(std::clamp(state.seek_preview / duration, 0.0, 1.0));
        std::string label = clock_text(state.seek_preview);
        const double diff = state.seek_preview - state.position;
        label += diff >= 0 ? "  (+" + clock_text(diff) + ")" : "  (-" + clock_text(-diff) + ")";
        const float tw = env_.fonts.semibold.font->measure(label, 26.0f) + 36.0f;
        const float bx = std::clamp(tx - tw / 2.0f, x, x + w - tw);
        bg.rounded_rect({bx, y - 58.0f, tw, 44.0f}, 22.0f, Color::rgb(0x000000, 0.72f));
        text(bg, env_.fonts.semibold, label, bx + tw / 2.0f, y - 48.0f, 26.0f, theme::ink, Align::center);
    }
    text(bg, env_.fonts.semibold, clock_text(state.position), x, y + 56.0f, 26.0f, theme::ink);
    // "Ends at HH:MM" wall-clock finish time next to the remaining duration.
    if (!state.ends_at.empty())
        text(bg, env_.fonts.regular, state.ends_at + "   ·   -" + clock_text(state.duration - state.position), x + w,
             y + 56.0f, 26.0f, theme::ink_2, Align::right);
    else
        text(bg, env_.fonts.regular, "-" + clock_text(state.duration - state.position), x + w, y + 56.0f, 26.0f,
             theme::ink_2, Align::right);

    // X-Ray: when paused, a frosted "In this title" strip above the seekbar
    // with the director and cast.
    if (paused_.value > 0.05f && !state.xray_cast.empty() && !state.menu)
    {
        bg.push_opacity(paused_.value);
        text(bg, env_.fonts.regular,
             "In this title:  " + env_.fonts.regular.font->fit(state.xray_cast, 22.0f, 1200.0f), 110.0f, 834.0f,
             22.0f, theme::ink_3);
        bg.pop_opacity();
    }

    // Tracks on the left, hints on the right. The hints are measured first and
    // the chips get whatever room is left. The row only fits if the hint
    // labels stay short: keep them terse, or the chip budget below goes
    // negative and the chips collapse (see the floor there).
    hui::ui::HintLayout layout;
    layout.size = 36.0f;
    layout.text_size = 24.0f;
    layout.cy = 1010.0f;
    layout.item_gap = 40.0f;
    layout.font = &env_.fonts.regular;
    // While the up-next card is up, Cross belongs to it ("Next"), not to play/pause.
    // With the track menu open Cross selects a track instead, so the hint must
    // not claim "Next" there.
    const bool upnext_next = !state.upnext_title.empty() && !state.menu;
    // In the Tracks menu the buttons do menu things: Cross selects a track,
    // Circle closes, L1/R1 nudge audio delay, L2/R2 nudge subtitle delay.
    const hui::ui::Hint menu_hints[4] = {{hui::ui::Button::cross, "Select"},
                                         {hui::ui::Button::circle, "Close"},
                                         {hui::ui::Button::l1, "Audio delay", hui::ui::Button::r1},
                                         {hui::ui::Button::l2, "Sub delay", hui::ui::Button::r2}};
    // One d-pad hint, not three: three labels made the row wider than the
    // screen and the chips slid under the hints (PS5 2026-10-13).
    const hui::ui::Hint hints[7] = {{hui::ui::Button::cross, upnext_next ? "Next" : (state.paused ? "Play" : "Pause")},
                                    {hui::ui::Button::dpad, "Seek · Zoom"},
                                    {hui::ui::Button::l1, "1 min", hui::ui::Button::r1},
                                    {hui::ui::Button::l2, "Sub sync", hui::ui::Button::r2},
                                    {hui::ui::Button::right_stick, "Stats"},
                                    {hui::ui::Button::circle, "Stop"},
                                    {hui::ui::Button::triangle, "Skip"}};
    // The Skip hint only appears while its prompt is up (Triangle is the
    // audio-track cycle the rest of the time).
    const hui::ui::Hint *active_hints = state.menu ? menu_hints : hints;
    const int hint_count = state.menu ? 4 : (state.skip_prompt.empty() ? 6 : 7);
    const float hints_w = hui::ui::measure_hints(env_.fonts, active_hints, hint_count, layout);

    // Long track names share what room the hints leave: the shorter one
    // keeps its full name, the longer one gives way. The Tracks chip is
    // fixed-width: 12 + glyph(36) + 12 + text + 22 (see draw_chip), and the
    // audio/subs chips each carry the same 82px of chrome plus a 14px gap.
    // The floor below is 1px, not 200: a 200px floor on the *text* budget
    // ignores the ~364px of fixed chrome, so when the hints get wide the
    // chips blew past the hints' start and the glyphs drew over the chip
    // text (PS5 2026-10-13). With a 1px floor the chips collapse to empty
    // pills instead of ever overlapping; the hint labels above are kept
    // short so the budget stays positive in practice.
    const std::string audio = "Audio  " + state.audio, subs = "Subtitles  " + state.subs;
    const float tracks_w = 82.0f + env_.fonts.semibold.measure("Tracks", theme::kBody);
    const float chip_chrome = 12.0f + 36.0f + 12.0f + 22.0f; // draw_chip's fixed part
    const float budget =
        std::max(1.0f, x + w - hints_w - 28.0f - x - tracks_w - 2 * chip_chrome - 2 * 14.0f);
    const float na = env_.fonts.semibold.measure(audio.c_str(), theme::kBody);
    const float ns = env_.fonts.semibold.measure(subs.c_str(), theme::kBody);
    float ma = na, ms = ns;
    if (na + ns > budget)
    {
        if (na <= budget * 0.5f)
            ms = budget - na;
        else if (ns <= budget * 0.5f)
            ma = budget - ns;
        else
            ma = ms = budget * 0.5f;
    }
    float cx = x;
    // One glass pill behind the whole row: chips on the left, hints on the
    // right, grouped the way the theme groups its control cluster.
    draw_glass_panel(fg, env_, {x - 18.0f, 1010.0f - 32.0f, w + 36.0f, 64.0f}, 20.0f, theme::glass_fill);
    cx += draw_chip(fg, env_, hui::ui::Button::triangle, audio.c_str(), cx, 1010.0f, false, ma) + 14.0f;
    cx += draw_chip(fg, env_, hui::ui::Button::square, subs.c_str(), cx, 1010.0f, false, ms) + 14.0f;
    draw_chip(fg, env_, hui::ui::Button::options, "Tracks", cx, 1010.0f);
    hui::ui::draw_hints(fg, env_.fonts, hui::ui::GlyphStyle::dark(), active_hints, hint_count, x + w, true, layout);
    bg.pop_opacity();
    fg.pop_opacity();
}

void PlayerUi::draw_buffering(hui::gfx::DrawList &list, float amount) const
{
    list.push_opacity(amount);
    list.rounded_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0, Color::rgb(0x000000, 0.38f));
    const float cx = theme::kScreenW * 0.5f, cy = 470.0f;
    // No ring: the loading progress lives in artwork, never a spinner ring.
    // A breathing dot marks the wait instead.
    const float breathe = 0.45f + 0.55f * (0.5f + 0.5f * std::sin(clock_ * 3.0f));
    list.circle(cx, cy, 20.0f, theme::accent_soft.with_alpha(0.85f * breathe));
    char label[48];
    std::snprintf(label, sizeof(label), "Buffering  %d%%", state.buffering);
    text(list, env_.fonts.semibold, label, cx, cy + 128.0f, 36.0f, theme::ink, Align::center);
    if (!state.net.empty())
        text(list, env_.fonts.regular, state.net, cx, cy + 172.0f, 26.0f, theme::ink_2, Align::center);
    list.pop_opacity();
}

void PlayerUi::draw_nerd(hui::gfx::DrawList &fg) const
{
    if (!state.nerd_overlay || state.nerd_lines.empty())
        return;
    // Compact panel, top-right, clear of the title block.
    const float w = 620.0f;
    const float h = 56.0f + 30.0f * static_cast<float>(state.nerd_lines.size());
    const float x = theme::kScreenW - w - 40.0f, y = 40.0f;
    draw_glass_panel(fg, env_, {x, y, w, h}, 12.0f, Color::rgb(0x000000, 0.72f));
    text(fg, env_.fonts.semibold, "Stats", x + 20.0f, y + 36.0f, 24.0f, theme::ink);
    float yy = y + 70.0f;
    for (const auto &line : state.nerd_lines)
    {
        text(fg, env_.fonts.regular, env_.fonts.regular.font->fit(line, 22.0f, w - 40.0f), x + 20.0f, yy, 22.0f,
             theme::ink_2);
        yy += 30.0f;
    }
}

void PlayerUi::draw_menu(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, float amount) const
{
    bg.push_opacity(amount);
    fg.push_opacity(amount);
    bg.rounded_rect({0, 0, theme::kScreenW, theme::kScreenH}, 0, Color::rgb(0x080808, 0.55f));
    const Rect panel{260.0f, 150.0f, 1400.0f, 780.0f};
    fg.push_transform(0.97f + 0.03f * amount, panel.cx(), panel.cy(), 0.0f, 0.0f);
    fg.shadow({panel.x + 10, panel.y + 30, panel.w - 20, panel.h - 20}, 38, 60, Color::rgb(0x000000, 0.55f));
    draw_glass_panel(fg, env_, panel, 38.0f, Color::rgb(0x101012, 0.86f));
    struct Column
    {
        const char *title;
        const std::vector<std::string> *tracks;
        int active;
        float x;
        float w;
    };
    // Four columns share the panel: audio + subtitles keep room for names,
    // speed and skip are narrow action columns.
    const Column columns[4] = {{"Audio", &state.audio_tracks, state.audio_active, panel.x + 50.0f, 400.0f},
                               {"Subtitles", &state.subtitle_tracks, state.subtitle_active, panel.x + 480.0f, 400.0f},
                               {"Speed", &state.speed_options, state.speed_active, panel.x + 910.0f, 220.0f},
                               {"Skip", &state.skip_options, state.skip_active, panel.x + 1160.0f, 240.0f}};
    for (int c = 0; c < 4; ++c)
    {
        const bool focused = state.menu_column == c;
        const float x = columns[c].x;
        fg.push_opacity(focused ? 1.0f : 0.55f);
        text(fg, env_.fonts.semibold, columns[c].title, x, panel.y + 88.0f, 34.0f, theme::ink);
        // Long lists scroll: the window follows the cursor, or the ticked track.
        const int total = static_cast<int>(columns[c].tracks->size());
        const int anchor = focused ? state.menu_row : columns[c].active;
        const int first = std::max(0, std::min(anchor - 3, total - 7));
        for (int k = 0; k < 7 && first + k < total; ++k)
        {
            const int i = first + k;
            const Rect r{x, panel.y + 124.0f + static_cast<float>(k) * 76.0f, columns[c].w, 66.0f};
            const bool selected = focused && state.menu_row == i;
            if (selected)
            {
                fg.glow(r, 24.0f, 22.0f, theme::accent.with_alpha(0.32f));
                fg.bordered_rect(r, 24.0f, theme::surface_hi, 3.0f, Color::rgb(0xffffff));
            }
            else
            {
                fg.rounded_rect(r, 24.0f, Color::rgb(0xffffff, 0.05f));
            }
            const bool active = i == columns[c].active;
            if (active)
            {
                fg.line(r.x + 26.0f, r.cy() + 1.0f, r.x + 34.0f, r.cy() + 10.0f, 3.6f, theme::accent_soft);
                fg.line(r.x + 34.0f, r.cy() + 10.0f, r.x + 50.0f, r.cy() - 9.0f, 3.6f, theme::accent_soft);
            }
            const hui::ui::FontRef &face = active ? env_.fonts.semibold : env_.fonts.regular;
            text(fg, face, face.font->fit((*columns[c].tracks)[i], 27.0f, r.w - 110.0f), r.x + 70.0f, r.cy() + 9.0f, 27.0f,
                 active || selected ? theme::ink : theme::ink_2);
            // Dual subtitles: mark the secondary pick with a "2" chip.
            if (c == 1 && i == state.subtitle2_active && state.subtitle2_active > 0 && i != columns[c].active)
                text(fg, env_.fonts.semibold, "2", r.x + r.w - 34.0f, r.cy() + 9.0f, 24.0f, theme::ink_3,
                     Align::center);
        }
        fg.pop_opacity();
    }
    text(fg, env_.fonts.regular, "Subtitle delay  " + (state.subtitle_delay.empty() ? std::string("0.00 s") : state.subtitle_delay) + "   (L2 / R2)",
         panel.x + 710.0f, panel.y + panel.h - 48.0f, 24.0f, theme::ink_3);
    text(fg, env_.fonts.regular, "Audio delay  " + (state.audio_delay.empty() ? std::string("0.00 s") : state.audio_delay) + "   (L1 / R1)",
         panel.x + 710.0f, panel.y + panel.h - 80.0f, 24.0f, theme::ink_3);
    text(fg, env_.fonts.regular, "Cross: primary subtitle   ·   Square: secondary subtitle (top)",
         panel.x + 710.0f, panel.y + panel.h - 112.0f, 24.0f, theme::ink_3);
    draw_chip(fg, env_, hui::ui::Button::circle, "Close", panel.x + panel.w - 200.0f, panel.y + panel.h - 56.0f);
    fg.pop_transform();
    fg.pop_opacity();
    bg.pop_opacity();
}

void PlayerUi::draw(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg) const
{
    if (controls_.value > 0.01f)
        draw_controls(bg, fg, controls_.value);
    draw_upnext(fg);
    draw_binge(fg);
    draw_skip_prompt(fg);
    draw_volume_osd(fg);
    draw_nerd(fg);
    if (buffering_.value < 0.5f && menu_.value < 0.5f)
        draw_subtitle(bg);
    if (paused_.value > 0.01f && menu_.value < 0.5f)
    {
        bg.push_opacity(paused_.value * (1.0f - menu_.value));
        const float cx = theme::kScreenW * 0.5f, cy = 470.0f;
        bg.shadow({cx - 90, cy - 70, 180, 180}, 90, 40, Color::rgb(0x000000, 0.4f));
        bg.circle(cx, cy, 90.0f, Color::rgb(0x000000, 0.55f));
        bg.ring(cx, cy, 89.0f, 2.0f, Color::rgb(0xffffff, 0.22f));
        bg.rounded_rect({cx - 34.0f, cy - 36.0f, 20.0f, 72.0f}, 7.0f, Color::rgb(0xffffff, 0.96f));
        bg.rounded_rect({cx + 14.0f, cy - 36.0f, 20.0f, 72.0f}, 7.0f, Color::rgb(0xffffff, 0.96f));
        bg.pop_opacity();
    }
    if (buffering_.value > 0.01f)
        draw_buffering(bg, buffering_.value);
    if (menu_.value > 0.01f)
        draw_menu(bg, fg, menu_.value);
}

} // namespace sx
