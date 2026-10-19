#include "haptics.hpp"

#include "util.h"

#include <cstdint>

#ifdef PLATFORM_PS5
#include "pcm_out.h" // AudioPort::player_wants: never rumble during playback
#endif

namespace sx
{
namespace
{

#ifdef PLATFORM_PS5

// DualSense classic two-motor rumble through libScePad (the vibration mode
// hui::ps5::Pad already selected). Mirrors gl/touchpad.cpp: the handle is
// owned by hui::ps5::Pad, we only borrow it.
struct PadVibration
{
    std::uint8_t large_motor;
    std::uint8_t small_motor;
};

extern "C"
{
    int scePadSetVibration(int handle, const PadVibration *vibration);
    // Live pad handle exported by third_party/hui/src/platform/ps5/pad.cpp.
    extern int g_ps5_pad_handle;
}

void set_motors(float small01, float large01)
{
    const int h = g_ps5_pad_handle;
    if (h < 0)
        return;
    const auto q = [](float v) -> std::uint8_t {
        if (v <= 0.0f)
            return 0;
        if (v >= 1.0f)
            return 255;
        return static_cast<std::uint8_t>(v * 255.0f);
    };
    const PadVibration vibration{q(large01), q(small01)};
    scePadSetVibration(h, &vibration);
}

bool playback_active()
{
    return AudioPort::player_wants.load();
}

#else // not the console: no pad, every effect is a silent no-op

void set_motors(float, float)
{
}

bool playback_active()
{
    return false;
}

#endif

} // namespace

Haptics &Haptics::instance()
{
    static Haptics h;
    return h;
}

void Haptics::set_enabled(bool on)
{
    if (enabled_ == on)
        return;
    enabled_ = on;
    if (!on)
        cancel();
}

void Haptics::fire(float small, float large, float seconds)
{
    if (!enabled_ || playback_active())
        return;
    hold_ = false;
    active_ = true;
    time_left_ = seconds;
    set_motors(small, large);
}

void Haptics::focus_tick()
{
    // Crisp tick on the small motor only: felt, not heard.
    fire(0.35f, 0.0f, 0.010f);
}

void Haptics::select()
{
    // Deeper pulse: the small motor leads, the large one adds weight.
    fire(0.75f, 0.25f, 0.030f);
}

void Haptics::wall_bump()
{
    // Soft bump against the end of a list.
    fire(0.45f, 0.55f, 0.080f);
}

void Haptics::press_hold(float intensity)
{
    if (!enabled_ || playback_active())
        return;
    float v = intensity;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    hold_ = true;
    active_ = true;
    hold_quiet_ = 0.0f;
    time_left_ = 0.0f;
    // Rising intensity: mostly the small motor, the large one joins near the top.
    set_motors(0.15f + 0.65f * v, v > 0.6f ? (v - 0.6f) * 1.5f : 0.0f);
}

void Haptics::press_release()
{
    if (hold_)
        cancel();
}

void Haptics::cancel()
{
    if (!active_)
        return;
    active_ = false;
    hold_ = false;
    time_left_ = 0.0f;
    set_motors(0.0f, 0.0f);
}

void Haptics::tick(float dt)
{
    if (!active_)
        return;
    if (playback_active())
    {
        cancel(); // playback started mid-effect: stop now
        return;
    }
    if (hold_)
    {
        hold_quiet_ += dt;
        if (hold_quiet_ > 0.25f)
            cancel(); // the caller stopped refreshing: never stick on
        return;
    }
    time_left_ -= dt;
    if (time_left_ <= 0.0f)
        cancel();
}

} // namespace sx
