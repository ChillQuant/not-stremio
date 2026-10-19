// DualSense haptics for the interface: short fire-and-forget effects.
//
// The wrapper drives libScePad directly on the pad handle hui owns (the same
// handle gl/touchpad.cpp borrows) and stops every effect on its own, so
// callers never manage lifetimes: Haptics::instance().focus_tick() and done.
// Every call is null-safe when no pad is present, and every effect is dropped
// while video plays. A Settings toggle (Prefs::haptics) turns it all off.
//
// Integration (for the shell/input owner):
//   - Haptics::instance().tick(dt) once per frame (via the polish pump).
//   - focus_tick() when the highlight moves, select() on confirm,
//     wall_bump() when a direction could not move (end of a list).
//   - press_hold(0..1) every frame a button is held for rising intensity,
//     press_release() when it lets go.

#pragma once

namespace sx
{

class Haptics
{
  public:
    static Haptics &instance();

    void set_enabled(bool on);
    bool enabled() const
    {
        return enabled_;
    }

    void focus_tick(); // light 10ms tick: the highlight moved
    void select();     // deeper 30ms pulse: confirmed
    void wall_bump();  // soft bump: pushed past the end of a list
    // Rising intensity while a button is held: call every frame with 0..1.
    // Stops on its own shortly after the calls stop (or press_release()).
    void press_hold(float intensity);
    void press_release();

    // Once a frame: stops finished effects. Auto-cancels when playback starts.
    void tick(float dt);
    void cancel();

  private:
    Haptics() = default;
    void fire(float small, float large, float seconds);

    bool enabled_ = true;
    bool active_ = false;
    bool hold_ = false;
    float time_left_ = 0.0f;
    float hold_quiet_ = 0.0f; // seconds since the last press_hold refresh
};

} // namespace sx
