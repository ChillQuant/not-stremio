// Accessibility settings section (WORKER 8).
//
// Session::a11y_build_rows() appends the Accessibility section at the end of
// the settings list. WORKER 7 (SETTINGS) calls it at the end of
// refresh_settings(); row indices run from settings.rows.size() at entry and
// a11y_row_base_ records the base so a11y_activate() can route Cross presses.
// WORKER 7 also needs one line in settings_activate(), after the header
// early-return:
//     if (a11y_activate(row)) return;

#include "session.hpp"

#include "a11y.hpp"

#include <cmath>

namespace sx
{

namespace
{

// Row offsets within the Accessibility section (0 is the section header).
enum A11yRow
{
    kA11yHdr = 0,
    kA11yTextScale,
    kA11yReduceMotion,
    kA11yHighContrast,
    kA11yFocus,
    kA11ySubSize,
    kA11ySubColor,
    kA11ySubBgBox,
    kA11ySubOutline,
    kA11yRowCount
};

// Choice-aware help: describes the option the row currently shows, mirroring
// the dropdown candidates in a11y_activate(). Thresholds match the
// A11y::*_label functions in a11y.cpp.
const char *a11y_text_scale_desc(float v)
{
    if (v >= 1.4f)
        return "150%: interface text scaled half again larger — easiest to read from the couch. Takes effect after restarting the app.";
    if (v >= 1.12f)
        return "125%: slightly larger interface text. Takes effect after restarting the app.";
    return "100%: default interface text size. Takes effect after restarting the app.";
}

const char *a11y_focus_desc(int s)
{
    switch (s)
    {
    case 1:
        return "Strong: 4px thick white outline plus a soft glow around the focused card — reads at couch distance without relying on color alone.";
    case 2:
        return "Extra: 5px outline with a brighter glow — maximum focus visibility.";
    default:
        return "Standard: the default focus look, no extra outline.";
    }
}

const char *a11y_sub_size_desc(float v)
{
    if (v >= 1.75f)
        return "200%: subtitles twice their regular size.";
    if (v >= 1.4f)
        return "150%: subtitles one and a half times their regular size.";
    if (v >= 1.12f)
        return "125%: slightly larger subtitles on top of the regular size setting.";
    return "100%: no extra enlargement — the regular subtitle size only.";
}

const char *a11y_sub_color_desc(int p)
{
    switch (p)
    {
    case 1:
        return "Yellow on black: warm yellow text on black — highest contrast over bright scenes.";
    case 2:
        return "Cyan on black: bright cyan text on black — an alternative high-contrast scheme.";
    default:
        return "White on black: classic white text on black.";
    }
}

const char *a11y_sub_outline_desc(int px)
{
    if (px >= 6)
        return "Extra (6px): the thickest dark edge — maximum legibility.";
    if (px >= 4)
        return "Thick (4px): a heavy dark edge — text stays readable over any background.";
    if (px >= 2)
        return "Default (2px): the standard readable outline.";
    if (px >= 1)
        return "Thin (1px): a subtle dark edge around each glyph.";
    return "Off: no outline — text may wash out over bright video.";
}

} // namespace

void Session::a11y_build_rows()
{
    using Kind = SettingRow::Kind;
    a11y_row_base_ = static_cast<int>(settings.rows.size());
    const A11y::Settings &a = A11y::get();
    const auto header = [&](const char *label) {
        SettingRow r;
        r.kind = Kind::header;
        r.label = label;
        settings.rows.push_back(r);
    };
    const auto row = [&](Kind kind, const char *label, const std::string &value, const std::string &help,
                         bool on = false) {
        SettingRow r;
        r.kind = kind;
        r.label = label;
        r.value = value;
        r.help = help;
        r.on = on;
        settings.rows.push_back(r);
    };
    header("Accessibility");
    row(Kind::value, "Interface text size", A11y::text_scale_label(a.ui_text_scale),
        a11y_text_scale_desc(a.ui_text_scale));
    row(Kind::toggle, "Reduce motion", "", "Replace interface animations with instant cuts.",
        a.reduce_motion);
    row(Kind::toggle, "High contrast panels", "",
        "Solid black panels behind text instead of frosted glass. Easier to read, and faster to draw.",
        a.high_contrast);
    row(Kind::value, "Focus indicator", A11y::focus_label(a.focus_strength),
        a11y_focus_desc(a.focus_strength));
    row(Kind::value, "Subtitle size boost", A11y::sub_size_label(a.sub_size_scale),
        a11y_sub_size_desc(a.sub_size_scale));
    row(Kind::value, "Subtitle color scheme", A11y::sub_color_label(a.sub_color_preset),
        a11y_sub_color_desc(a.sub_color_preset));
    row(Kind::toggle, "Subtitle background box", "",
        "A solid black box behind every subtitle line. Maximum contrast over bright video.", a.sub_bg_box);
    row(Kind::value, "Subtitle outline", A11y::sub_outline_label(a.sub_outline_min_px),
        a11y_sub_outline_desc(a.sub_outline_min_px));
}

bool Session::a11y_activate(int row)
{
    const int i = row - a11y_row_base_;
    if (a11y_row_base_ < 0 || i <= kA11yHdr || i >= kA11yRowCount)
        return false;
    A11y::Settings &a = A11y::get();
    switch (i)
    {
    case kA11yTextScale:
    {
        static const float vals[] = {1.0f, 1.25f, 1.5f};
        int cur = 0;
        for (int k = 0; k < 3; ++k)
            if (std::abs(vals[k] - a.ui_text_scale) < 0.01f)
                cur = k;
        open_dropdown("Interface text size", {"100%", "125%", "150%"}, cur, [this](int k) {
            A11y::Settings &s = A11y::get();
            s.ui_text_scale = vals[k];
            A11y::validate(s);
            save_prefs();
            cue(hui::audio::Cue::saved);
            say("Interface text size takes effect after restarting the app.", 5.0);
            refresh_settings();
        },
                      {a11y_text_scale_desc(1.0f), a11y_text_scale_desc(1.25f), a11y_text_scale_desc(1.5f)});
        break;
    }
    case kA11yReduceMotion:
        a.reduce_motion = !a.reduce_motion;
        save_prefs();
        cue(hui::audio::Cue::toggle);
        refresh_settings();
        break;
    case kA11yHighContrast:
        a.high_contrast = !a.high_contrast;
        save_prefs();
        cue(hui::audio::Cue::toggle);
        refresh_settings();
        break;
    case kA11yFocus:
        open_dropdown("Focus indicator", {"Standard", "Strong", "Extra"}, a.focus_strength, [this](int k) {
            A11y::Settings &s = A11y::get();
            s.focus_strength = k;
            A11y::validate(s);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        },
                      {a11y_focus_desc(0), a11y_focus_desc(1), a11y_focus_desc(2)});
        break;
    case kA11ySubSize:
    {
        static const float vals[] = {1.0f, 1.25f, 1.5f, 2.0f};
        int cur = 0;
        for (int k = 0; k < 4; ++k)
            if (std::abs(vals[k] - a.sub_size_scale) < 0.01f)
                cur = k;
        open_dropdown("Subtitle size boost", {"100%", "125%", "150%", "200%"}, cur, [this](int k) {
            A11y::Settings &s = A11y::get();
            s.sub_size_scale = vals[k];
            A11y::validate(s);
            save_prefs();
            cue(hui::audio::Cue::saved);
            refresh_settings();
        },
                      {a11y_sub_size_desc(1.0f), a11y_sub_size_desc(1.25f), a11y_sub_size_desc(1.5f),
                       a11y_sub_size_desc(2.0f)});
        break;
    }
    case kA11ySubColor:
        open_dropdown("Subtitle color scheme", {"White on black", "Yellow on black", "Cyan on black"},
                      a.sub_color_preset, [this](int k) {
                          A11y::Settings &s = A11y::get();
                          s.sub_color_preset = k;
                          A11y::validate(s);
                          save_prefs();
                          cue(hui::audio::Cue::saved);
                          refresh_settings();
                      },
                      {a11y_sub_color_desc(0), a11y_sub_color_desc(1), a11y_sub_color_desc(2)});
        break;
    case kA11ySubBgBox:
        a.sub_bg_box = !a.sub_bg_box;
        save_prefs();
        cue(hui::audio::Cue::toggle);
        refresh_settings();
        break;
    case kA11ySubOutline:
    {
        static const int vals[] = {0, 1, 2, 4, 6};
        int cur = 2;
        for (int k = 0; k < 5; ++k)
            if (vals[k] == a.sub_outline_min_px)
                cur = k;
        open_dropdown("Subtitle outline", {"Off", "Thin (1px)", "Default (2px)", "Thick (4px)", "Extra (6px)"}, cur,
                      [this](int k) {
                          A11y::Settings &s = A11y::get();
                          s.sub_outline_min_px = vals[k];
                          A11y::validate(s);
                          save_prefs();
                          cue(hui::audio::Cue::saved);
                          refresh_settings();
                      },
                      {a11y_sub_outline_desc(0), a11y_sub_outline_desc(1), a11y_sub_outline_desc(2),
                       a11y_sub_outline_desc(4), a11y_sub_outline_desc(6)});
        break;
    }
    default:
        return false;
    }
    return true;
}

} // namespace sx
