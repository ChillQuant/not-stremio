#include "touchpad.hpp"

#include "util.h"

#include <cstdint>
#include <cstring>

namespace sx
{
namespace
{

// DualSense lightbar color parameter (libScePad).
struct ScePadLightBarParam
{
    std::uint8_t r, g, b, reserve;
};

extern "C"
{
    int scePadSetLightBar(int handle, const ScePadLightBarParam *param);
    int scePadResetLightBar(int handle);
    // Live pad state exported by third_party/hui/src/platform/ps5/pad.cpp.
    // Pad::read() already receives the full 120-byte ScePadData every frame;
    // the touchpad region (0x34..0x4B) is copied here instead of discarded.
    extern std::uint8_t g_ps5_latest_pad_raw[120];
    extern bool g_ps5_latest_pad_valid;
    extern int g_ps5_pad_handle;
}

// True PS5 libScePad ScePadTouch layout (the OS driver already unpacks the
// USB HID wire format into 16-bit coordinates):
//   0x34: touch_count (uint8_t: 0, 1, or 2 active fingers)
//   0x3C..0x43: touch[0] { uint16_t x (0..1919 LE); uint16_t y (0..1079 LE);
//                           uint8_t id; uint8_t reserve[3]; }
//   0x44..0x4B: touch[1] (same layout)
struct ScePadTouchPoint
{
    std::uint16_t x;
    std::uint16_t y;
    std::uint8_t id;
    std::uint8_t reserved[3];
};
static_assert(sizeof(ScePadTouchPoint) == 8, "ScePadTouch is 8 bytes");

bool parse_touch_point(const ScePadTouchPoint &t, TouchPoint &out)
{
    out.active = true;
    out.x = t.x > 1919 ? 1919 : t.x;
    out.y = t.y > 1079 ? 1079 : t.y;
    return true;
}

} // namespace

TouchpadReader::~TouchpadReader()
{
    close();
}

bool TouchpadReader::open()
{
    // Piggybacks on hui::ps5::Pad's handle; no second scePadOpen needed.
    handle_ = g_ps5_pad_handle;
    return handle_ >= 0;
}

void TouchpadReader::close()
{
    // Do NOT call scePadClose(handle_): hui::ps5::Pad owns the handle lifetime.
    handle_ = -1;
    has_lightbar_ = false;
}

void TouchpadReader::set_lightbar(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    const int h = g_ps5_pad_handle;
    if (h < 0)
        return;
    // Only talk to the controller when the color actually changed.
    if (has_lightbar_ && light_r_ == r && light_g_ == g && light_b_ == b)
        return;
    const ScePadLightBarParam param{r, g, b, 0};
    scePadSetLightBar(h, &param);
    light_r_ = r;
    light_g_ = g;
    light_b_ = b;
    has_lightbar_ = true;
}

void TouchpadReader::reset_lightbar()
{
    const int h = g_ps5_pad_handle;
    if (h < 0)
        return;
    scePadResetLightBar(h);
    has_lightbar_ = false;
}

bool TouchpadReader::poll(TouchPoint &point)
{
    point.active = false;
    if (!g_ps5_latest_pad_valid)
        return false;
    const std::uint8_t *raw = g_ps5_latest_pad_raw;
    const std::uint8_t touch_count = raw[0x34];
    if (touch_count == 0 || touch_count > 2)
        return false;
    // Use the first active finger. Touch structs start at 0x3C, 8 bytes each.
    const auto *t0 = reinterpret_cast<const ScePadTouchPoint *>(raw + 0x3C);
    const auto *t1 = reinterpret_cast<const ScePadTouchPoint *>(raw + 0x44);
    const ScePadTouchPoint &t = (touch_count > 1) ? *t1 : *t0;
    if (parse_touch_point(t, point))
    {
        static bool logged_first = false;
        if (!logged_first)
        {
            logged_first = true;
            dlog("touchpad: first touch at x=%d y=%d (touchNum=%u, id=%u)",
                 point.x, point.y, unsigned(touch_count), unsigned(t.id));
        }
        return true;
    }
    return false;
}

} // namespace sx
