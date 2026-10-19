// A grid of posters that scrolls by rows, with the focus ring that glides
// between them. Discover and Library use it.

#pragma once

#include "chrome.hpp"
#include "content.hpp"
#include "core/input.hpp"

#include <vector>

namespace sx
{

struct GridLayout
{
    float x = theme::kLeft;      // left edge of the first column
    float y = 220.0f;            // top of the first row's posters
    float card_w = 268.0f;
    float card_h = 400.0f;
    float pitch_x = 304.0f;
    float pitch_y = 516.0f;
    int cols = 6;
    float view_top = 188.0f;     // posters are clipped above this line
    float view_bottom = 1080.0f;
};

class PosterGrid
{
  public:
    enum class Edge
    {
        none,
        up,   // moved up out of the first row
        left, // moved left out of the first column
    };

    void set_items(const std::vector<Title> *items, const GridLayout &layout);
    // The items changed in place (more were added): keeps the cursor where it is.
    void sync();
    void snap();
    // Restore a saved grid position (nav_memory): clamps to the loaded items.
    void set_index(int idx);
    Edge move(hui::Direction direction);
    void update(float dt, bool has_focus);
    void draw(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, const Env &env, float age, bool show_progress,
              bool has_focus) const;
    // Loading placeholders: shimmering card shapes in the grid's layout, for
    // when the items are still arriving. Subtle, dark-theme, never a spinner.
    void draw_skeletons(hui::gfx::DrawList &bg, const Env &env, float clock) const;

    int index() const
    {
        return index_;
    }
    int cols() const
    {
        return layout_.cols;
    }
    const Title *focused() const;

  private:
    hui::gfx::Rect rect_at(int index, bool final_position) const;

    const std::vector<Title> *items_ = nullptr;
    GridLayout layout_;
    int index_ = 0;
    std::vector<hui::tween::Spring> pop_;
    hui::ui::Scroller scroll_;
    hui::ui::SpringRect ring_;
    hui::tween::Spring focus_on_;
};

} // namespace sx
