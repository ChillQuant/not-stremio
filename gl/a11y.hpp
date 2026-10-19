// Accessibility settings (WORKER 8).
//
// The single place the app's low-vision / low-hearing / motor accessibility
// options live. Everything is opt-in: every default keeps the stock look,
// and every feature is zero-or-negative performance cost when off (a single
// branch at the application point).
//
// ---- Cross-worker contract: exact names and types, do not rename ----
//   - WORKER 4 (SUBTITLES) reads sub_outline_min_px, sub_bg_box and
//     sub_color_preset (plus sub_size_scale, documented below) via
//     `#if __has_include("a11y.hpp")`, with compiled-in fallbacks.
//   - WORKER 2 (BROWSING) and WORKER 10 (POLISH) read reduce_motion the
//     same way: when true, animations become instant cuts.
// This header lives in the global namespace so `A11y::get()` resolves from
// any scope (sx:: included).

#pragma once

#include <cstdint>

namespace A11y
{

struct Settings
{
    // ---- contract fields (exact names/types) ----
    bool reduce_motion = false; // animations become instant cuts
    float ui_text_scale = 1.0f; // interface text multiplier: 1.0 / 1.25 / 1.5
    bool high_contrast = false; // solid black panels instead of frosted glass
    int sub_outline_min_px = 2; // subtitle outline thickness, px (0 = off)
    bool sub_bg_box = false;    // solid black box behind each subtitle line
    int sub_color_preset = 0;   // 0 white-on-black, 1 yellow-on-black, 2 cyan-on-black
    // ---- worker-8 extensions (documented for worker 4 to honor too) ----
    // Multiplier the subtitle renderer applies on top of the base subtitle
    // size: 1.0 / 1.25 / 1.5 / 2.0. Lets low-vision users reach ~2x subtitles.
    float sub_size_scale = 1.0f;
    // Card focus indicator: 0 standard, 1 strong (4px outline + glow),
    // 2 extra (5px outline + stronger glow).
    int focus_strength = 0;
};

Settings &get();

// (Re)read the a11y_* keys from the prefs file. get() calls this lazily on
// first use so the persisted text scale is visible before Session::init()
// (fonts are built before prefs load); Session::load_prefs() re-applies the
// same keys through its marked A11Y block once the session is up.
void load();
// Persist the a11y_* keys (read-modify-write; every other key untouched).
// The session's own save_prefs() marked A11Y block is the normal path; this
// exists for use without a session.
void save();
// Clamp every field to its valid range (hand-edited or corrupt JSON).
void validate(Settings &s);

// Subtitle color schemes: 0xRRGGBB text color for a sub_color_preset value.
std::uint32_t sub_text_rgb(int preset);
// Settings-screen labels for the current values.
const char *text_scale_label(float v);
const char *focus_label(int v);
const char *sub_size_label(float v);
const char *sub_color_label(int preset);
const char *sub_outline_label(int px);

} // namespace A11y
