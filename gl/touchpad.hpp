#pragma once
// DualSense touchpad swipe reader for volume control.
//
// Parses the touch data from scePadRead's 120-byte ScePadData records.
// Layout (from PS5 homebrew research):
//   0x34: touch count (1 byte)
//   0x3C: touch point 0 (8 bytes)
//   0x44: touch point 1 (8 bytes)
//
// Each 8-byte touch point:
//   byte 0: contact ID, bit 7 set = finger lifted/inactive
//   bytes 1-2: X coordinate (12-bit LE, 0-1919)
//   bytes 3-4: Y coordinate (12-bit LE, 0-1079)
//   bytes 5-7: reserved
//
// This opens its own scePad handle (read-only). If the open fails
// (e.g. hui already holds the controller exclusively), the reader
// stays inert and swipe volume is silently unavailable.

#include <cstdint>

namespace sx
{

struct TouchPoint
{
    bool active = false; // finger currently touching
    int x = 0;           // 0-1919
    int y = 0;           // 0-1079, 0 = top of pad
};

class TouchpadReader
{
  public:
    TouchpadReader() = default;
    TouchpadReader(const TouchpadReader &) = delete;
    TouchpadReader &operator=(const TouchpadReader &) = delete;
    ~TouchpadReader();

    // Open the controller. Returns true on success.
    bool open();
    void close();
    bool is_open() const { return handle_ >= 0; }

    // Poll for touch state. Call once per frame. Returns true if a
    // finger is currently touching (point filled in).
    bool poll(TouchPoint &point);

    // DualSense lightbar cinema mode. set_lightbar() only calls into
    // scePad when the color actually changes; reset_lightbar() returns
    // the controller to its system default. Both no-op when closed.
    void set_lightbar(std::uint8_t r, std::uint8_t g, std::uint8_t b);
    void reset_lightbar();

  private:
    int handle_ = -1;
    // Last lightbar color sent; has_lightbar_ tracks whether one was set
    // so the first set_lightbar() after open/reset always goes through.
    std::uint8_t light_r_ = 0, light_g_ = 0, light_b_ = 0;
    bool has_lightbar_ = false;
};

} // namespace sx
