#include "text_cache.hpp"

namespace sx
{

namespace
{

CachedText::Entry &entry_for(CachedText &cache, const hui::gfx::Font *font, std::string_view text, float size,
                             float width, bool wrap)
{
    for (CachedText::Entry &e : cache.entries)
    {
        if (e.used && e.font == font && e.wrap == wrap && e.size == size && e.width == width && e.text == text)
            return e;
    }
    // Miss: reuse the first free slot, else the oldest (slot 0).
    for (CachedText::Entry &e : cache.entries)
    {
        if (!e.used)
        {
            e.used = true;
            e.done = false;
            e.font = font;
            e.text = std::string(text);
            e.size = size;
            e.width = width;
            e.wrap = wrap;
            return e;
        }
    }
    CachedText::Entry &e = cache.entries[cache.next_evict % cache.entries.size()];
    cache.next_evict = (cache.next_evict + 1) % cache.entries.size();
    e.done = false;
    e.font = font;
    e.text = std::string(text);
    e.size = size;
    e.width = width;
    e.wrap = wrap;
    e.fitted.clear();
    e.lines.clear();
    return e;
}

} // namespace

const std::string &cached_fit(CachedText &cache, const hui::gfx::Font *font, std::string_view text, float size,
                              float width)
{
    CachedText::Entry &e = entry_for(cache, font, text, size, width, false);
    if (!e.done)
    {
        e.fitted = text.empty() ? std::string() : font->fit(text, size, width);
        e.done = true;
    }
    return e.fitted;
}

const std::vector<std::string> &cached_wrap(CachedText &cache, const hui::gfx::Font *font, std::string_view text,
                                            float size, float width)
{
    CachedText::Entry &e = entry_for(cache, font, text, size, width, true);
    if (!e.done)
    {
        e.lines = text.empty() ? std::vector<std::string>() : font->wrap(text, size, width);
        e.done = true;
    }
    return e.lines;
}

// Same as hui::ui::paragraph: word-wrapped from the first baseline at y, at
// most max_lines (the last one ends in an ellipsis if text remains). Only the
// wrap/fit go through the cache.
float cached_paragraph(hui::gfx::DrawList &list, CachedText &cache, const hui::ui::FontRef &font,
                       std::string_view value, float x, float y, float size, float width, float line_height,
                       hui::gfx::Color color, int max_lines, hui::gfx::Align align)
{
    // Copy: the fit below can evict the wrap entry this reference points at.
    const std::vector<std::string> lines = cached_wrap(cache, font.font, value, size, width);
    int drawn = 0;
    for (const std::string &line : lines)
    {
        const bool last =
            drawn + 1 == max_lines && lines.size() > static_cast<std::size_t>(max_lines);
        if (last)
        {
            std::string cut = line;
            const std::size_t sp = cut.find_last_of(' ');
            if (sp != std::string::npos && sp > 0)
                cut.erase(sp);
            hui::ui::text(list, font, cached_fit(cache, font.font, cut + " \xE2\x80\xA6", size, width), x, y,
                          size, color, align);
        }
        else
            hui::ui::text(list, font, line, x, y, size, color, align);
        y += line_height;
        if (++drawn >= max_lines)
            break;
    }
    return y;
}

} // namespace sx
