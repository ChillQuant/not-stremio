#pragma once

// Per-item cache for laid-out text. wrap()/fit() shape through HarfBuzz, so
// calling them every frame for the same string is pure waste; these return
// identical results and only re-layout when the (font, text, size, width)
// key changes. Refs stay valid: entries live in a fixed array.

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "gfx/font.hpp"
#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"

namespace sx
{

struct CachedText
{
    struct Entry
    {
        const void *font = nullptr;
        std::string text;
        float size = 0.0f, width = 0.0f;
        bool wrap = false; // true: lines is valid; false: fitted is valid
        bool used = false;
        bool done = false; // the value was laid out for the current key
        std::string fitted;
        std::vector<std::string> lines;
    };
    // Twelve slots: the hero lays out ~7 distinct keys per frame, the detail
    // page ~10 (a smaller cache would re-lay-out every frame there).
    std::array<Entry, 12> entries;
    uint8_t next_evict = 0;
};

// font->fit() with the result cached.
const std::string &cached_fit(CachedText &cache, const hui::gfx::Font *font, std::string_view text, float size,
                              float width);
// font->wrap() with the result cached.
const std::vector<std::string> &cached_wrap(CachedText &cache, const hui::gfx::Font *font, std::string_view text,
                                            float size, float width);
// hui::ui::paragraph() with the wrap/fit cached: pixel-identical output.
float cached_paragraph(hui::gfx::DrawList &list, CachedText &cache, const hui::ui::FontRef &font,
                       std::string_view value, float x, float y, float size, float width, float line_height,
                       hui::gfx::Color color, int max_lines = 99, hui::gfx::Align align = hui::gfx::Align::left);

} // namespace sx
