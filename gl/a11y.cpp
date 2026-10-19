// Accessibility settings: persistence and validation (WORKER 8).

#include "a11y.hpp"

#include "../src/util.h" // json, load_json, save_json, jbool, jnum

#include <algorithm>
#include <cmath>

namespace A11y
{

namespace
{

// main.cpp's kData; the prefs file lives here. Duplicated (rather than
// plumbed through) so the settings are readable before Session::init() hands
// out the data dir — fonts are built before prefs load, and the text scale
// must be known at font-init time.
constexpr const char *kPrefsPath = "/download0/stremio/settings.json";

Settings &store()
{
    static Settings s;
    return s;
}

bool &loaded_flag()
{
    static bool f = false;
    return f;
}

void read_keys(const json &j, Settings &s)
{
    s.reduce_motion = jbool(j, "a11y_reduce_motion", s.reduce_motion);
    s.ui_text_scale = static_cast<float>(jnum(j, "a11y_ui_text_scale", s.ui_text_scale));
    s.high_contrast = jbool(j, "a11y_high_contrast", s.high_contrast);
    s.sub_outline_min_px = static_cast<int>(jnum(j, "a11y_sub_outline_min_px", s.sub_outline_min_px));
    s.sub_bg_box = jbool(j, "a11y_sub_bg_box", s.sub_bg_box);
    s.sub_color_preset = static_cast<int>(jnum(j, "a11y_sub_color_preset", s.sub_color_preset));
    s.sub_size_scale = static_cast<float>(jnum(j, "a11y_sub_size_scale", s.sub_size_scale));
    s.focus_strength = static_cast<int>(jnum(j, "a11y_focus_strength", s.focus_strength));
}

void write_keys(json &j, const Settings &s)
{
    j["a11y_reduce_motion"] = s.reduce_motion;
    j["a11y_ui_text_scale"] = s.ui_text_scale;
    j["a11y_high_contrast"] = s.high_contrast;
    j["a11y_sub_outline_min_px"] = s.sub_outline_min_px;
    j["a11y_sub_bg_box"] = s.sub_bg_box;
    j["a11y_sub_color_preset"] = s.sub_color_preset;
    j["a11y_sub_size_scale"] = s.sub_size_scale;
    j["a11y_focus_strength"] = s.focus_strength;
}

} // namespace

Settings &get()
{
    if (!loaded_flag())
    {
        loaded_flag() = true;
        load();
    }
    return store();
}

void load()
{
    Settings &s = store();
    json j;
    std::string err;
    if (!load_json(kPrefsPath, j, &err))
        return; // first run (or unreadable): keep the defaults
    read_keys(j, s);
    validate(s);
}

void save()
{
    Settings s = store();
    validate(s);
    json j;
    std::string err;
    load_json(kPrefsPath, j, &err);
    if (err == "invalid JSON")
        return; // never clobber a corrupt file the session is about to preserve
    write_keys(j, s);
    save_json(kPrefsPath, j);
}

void validate(Settings &s)
{
    // Stepped choices: snap corrupt/hand-edited values to the nearest step.
    static const float text_steps[] = {1.0f, 1.25f, 1.5f};
    float best = text_steps[0];
    for (float st : text_steps)
        if (std::fabs(s.ui_text_scale - st) < std::fabs(s.ui_text_scale - best))
            best = st;
    s.ui_text_scale = std::isfinite(s.ui_text_scale) ? best : 1.0f;
    static const float sub_steps[] = {1.0f, 1.25f, 1.5f, 2.0f};
    best = sub_steps[0];
    for (float st : sub_steps)
        if (std::fabs(s.sub_size_scale - st) < std::fabs(s.sub_size_scale - best))
            best = st;
    s.sub_size_scale = std::isfinite(s.sub_size_scale) ? best : 1.0f;
    s.sub_outline_min_px = std::clamp(s.sub_outline_min_px, 0, 8);
    s.sub_color_preset = std::clamp(s.sub_color_preset, 0, 2);
    s.focus_strength = std::clamp(s.focus_strength, 0, 2);
}

std::uint32_t sub_text_rgb(int preset)
{
    switch (preset)
    {
    case 1:
        return 0xffe135; // warm yellow on black
    case 2:
        return 0x35d8ff; // cyan on black
    default:
        return 0xffffff; // white on black
    }
}

const char *text_scale_label(float v)
{
    if (v >= 1.4f)
        return "150%";
    if (v >= 1.12f)
        return "125%";
    return "100%";
}

const char *focus_label(int v)
{
    if (v >= 2)
        return "Extra";
    if (v >= 1)
        return "Strong";
    return "Standard";
}

const char *sub_size_label(float v)
{
    if (v >= 1.75f)
        return "200%";
    if (v >= 1.4f)
        return "150%";
    if (v >= 1.12f)
        return "125%";
    return "100%";
}

const char *sub_color_label(int preset)
{
    switch (preset)
    {
    case 1:
        return "Yellow on black";
    case 2:
        return "Cyan on black";
    default:
        return "White on black";
    }
}

const char *sub_outline_label(int px)
{
    if (px >= 6)
        return "Extra (6px)";
    if (px >= 4)
        return "Thick (4px)";
    if (px >= 2)
        return "Default (2px)";
    if (px >= 1)
        return "Thin (1px)";
    return "Off";
}

} // namespace A11y
