// Stremio on PS5: the design tokens every screen draws with.
//
// The identity is Stremio's: near-black ground with violet light, the violet
// accent, white focus. Everything is laid out on the toolkit's 1920 x 1080
// virtual canvas (third_party/hui), which it scales to 1080p, 1440p or 4K.

#pragma once

#include "gfx/draw_list.hpp"

namespace sx::theme
{

using hui::gfx::Color;

inline const Color ground = Color::rgb(0x0c0b11);       // the page
inline const Color accent = Color::rgb(0x7b5bf5);       // Stremio violet
inline const Color accent_soft = Color::rgb(0xa58bff);  // violet text on dark
inline const Color accent_deep = Color::rgb(0x3a2a8f);  // the glow behind the page
inline const Color ink = Color::rgb(0xffffff, 0.94f);   // titles
inline const Color ink_2 = Color::rgb(0xffffff, 0.72f); // body text
inline const Color ink_3 = Color::rgb(0xffffff, 0.52f); // labels, 5.5:1 on the ground
inline const Color surface = Color::rgb(0xffffff, 0.07f);
inline const Color surface_hi = Color::rgb(0xffffff, 0.13f);
inline const Color hairline = Color::rgb(0xffffff, 0.10f);
inline const Color watched = Color::rgb(0x22b365);
inline const Color imdb = Color::rgb(0xf5c518);

// Glass Phase 1: frosted-glass surfaces. The visual language (translucent
// dark fill + 1px inner rim light + soft drop shadow over a blurred backdrop)
// is inspired by Fxy6969's "liquid-glass" Stremio theme; the blur itself comes
// from the toolkit's DrawList::glass over renderer.glass_texture().
inline const Color glass_fill = Color::rgb(0x464646, 0.22f);    // dark glass pill fill
inline const Color glass_deep = Color::rgb(0x000000, 0.41f);    // Watch-button glass
inline const Color glass_hair = Color::rgb(0xffffff, 0.10f);    // 1px rim
inline const Color glass_hair_hi = Color::rgb(0xffffff, 0.20f);// active/focus rim
// accent stays violet: Stremio's identity, unlike the web theme's white flip.

// Layout (virtual pixels).
constexpr float kRailWidth = 0.0f;    // top horizontal pill nav (no left sidebar)
constexpr float kLeft = 64.0f;        // where page content starts
constexpr float kRight = 1856.0f;     // where it ends (1920 - 64)
constexpr float kScreenW = 1920.0f;
constexpr float kScreenH = 1080.0f;

// Type sizes: nothing a person has to read is under 20.
constexpr float kLabel = 20.0f;
constexpr float kBody = 24.0f;
constexpr float kSubtitle = 28.0f;
constexpr float kHeading = 32.0f;

} // namespace sx::theme
