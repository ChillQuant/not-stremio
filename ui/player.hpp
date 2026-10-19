// What is drawn over the video: the controls, the paused and buffering
// states, subtitles and the track menu. The picture itself is the player's.

#pragma once

#include "chrome.hpp"
#include "content.hpp"

namespace sx
{

class PlayerUi
{
  public:
    explicit PlayerUi(const Env &env) : env_(env)
    {
        skip_y_.snap(688.0f);
    }

    PlayerState state; // the app keeps this up to date

    void snap();       // jump every animation to where the state wants it
    void update(float dt);
    void draw(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg) const;

  private:
    void draw_controls(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, float amount) const;
    void draw_menu(hui::gfx::DrawList &bg, hui::gfx::DrawList &fg, float amount) const;
    void draw_buffering(hui::gfx::DrawList &list, float amount) const;
    void draw_subtitle(hui::gfx::DrawList &list) const;
    void draw_upnext(hui::gfx::DrawList &list) const;
    void draw_binge(hui::gfx::DrawList &list) const;
    void draw_skip_prompt(hui::gfx::DrawList &list) const;
    void draw_volume_osd(hui::gfx::DrawList &list) const;
    void draw_nerd(hui::gfx::DrawList &list) const;

    const Env &env_;
    hui::tween::Spring controls_;
    hui::tween::Spring menu_;
    hui::tween::Spring paused_;
    hui::tween::Spring buffering_;
    hui::tween::Spring upnext_;
    hui::tween::Spring binge_;
    hui::tween::Spring skip_;
    hui::tween::Spring skip_y_;
    float clock_ = 0.0f;
    // Wrapped-subtitle cache: a cue stays on screen 2-5s (120-300 frames),
    // so reusing the wrapped lines avoids per-frame Thai-cluster splitting.
    mutable std::string cached_sub_line_;
    mutable int cached_sub_size_ = -1;
    mutable std::vector<std::string> cached_sub_wrapped_;
    // Secondary (dual subtitle) wrap cache: same lifetime logic as primary.
    mutable std::string cached_sub2_line_;
    mutable int cached_sub2_size_ = -1;
    mutable std::vector<std::string> cached_sub2_wrapped_;
    // One-press sync readout: the last delay-nudge serial seen, and the time
    // until which the transient "Subtitle delay" badge stays visible.
    // Fired by Subtitles::note_delay_ms (session_watch's nudge calls it).
    mutable unsigned seen_delay_serial_ = 0;
    mutable float delay_badge_until_ = -1.0f;
};

// Text with a dark outline, for subtitles over any picture.
void draw_outlined_text(hui::gfx::DrawList &list, const hui::ui::FontRef &font, const std::string &value, float x,
                        float baseline, float size, hui::gfx::Color colour, hui::gfx::Align align);

} // namespace sx
