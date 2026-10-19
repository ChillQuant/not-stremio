// Settings: a list of choices and switches, and beside it what the focused one does.

#pragma once

#include "cards.hpp"
#include "page.hpp"

#include <vector>

namespace sx
{

class Settings : public PageBase
{
  public:
    explicit Settings(const Env &env) : PageBase(env, Page::settings)
    {
    }

    void set_content(const SettingsContent *content);
    void refresh(); // a value changed in place: the cursor stays
    void update(const hui::InputFrame &input, float dt);
    void draw(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, hui::gfx::BackdropSpec &backdrop) const;
    // The row under the cursor, or -1 while the rail has the focus.
    int cursor_row() const
    {
        return rail_.focused() ? -1 : index_;
    }
    // True when the cursor sits on a slider row (Left/Right adjusts it).
    bool cursor_on_slider() const
    {
        const int r = cursor_row();
        return r >= 0 && content_ && static_cast<std::size_t>(r) < content_->rows.size() &&
               content_->rows[static_cast<std::size_t>(r)].kind == SettingRow::Kind::slider;
    }
    // True when the cursor sits on a value row whose options Left/Right cycles
    // in place (WORKER 7: inline stepping, no choice list opened).
    bool cursor_can_step() const
    {
        const int r = cursor_row();
        return r >= 0 && content_ && static_cast<std::size_t>(r) < content_->rows.size() &&
               content_->rows[static_cast<std::size_t>(r)].step_inline;
    }
    // L1/R1 section jumping (WORKER 7): move the cursor to the first row of
    // the previous/next section header, wrapping around.
    void section_jump(int dir);

  private:
    hui::gfx::Rect row_rect(int index, bool final_position) const;
    void draw_help(hui::gfx::DrawList &list, const SettingRow &row) const;
    bool is_header(int index) const; // section labels: never focused or activated
    bool is_selectable(int index) const; // headers and hidden (empty-label) rows are skipped
    int first_selectable() const;

    const SettingsContent *content_ = nullptr;
    int index_ = 0;
    hui::ui::Scroller scroll_;
    hui::ui::SpringRect ring_;
    hui::tween::Spring focus_on_;
    std::vector<hui::tween::Spring> switches_; // 0..1 per row, so switches slide
    int shown_ = -1;
    hui::tween::Timer swap_;
};

} // namespace sx
