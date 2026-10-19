#pragma once

// HDMI refresh-rate control via libSceVideoOut (dlsym, never linked).
// Everything here fails closed: any missing symbol, refused call, or
// unexpected result leaves the display exactly as it was.
//
// UI thread only. EVO Player verified on FW 12.70 that game-slot apps are
// refused every refresh rate except the default and 119.88 Hz (mode 0xf),
// so 23.976/24fps film plays judder-free as 5:5 pulldown at 119.88 Hz.
// 12.4 behaviour is unverified, hence the probe-first design below.
namespace videoout_rate {

// Side-effect-free probe: is 119.88 Hz output available on this console +
// display? Uses sceVideoOutIsOutputSupported (a query, not a mode change),
// so the screen never blanks. The result is cached for the process.
bool probe_119_88();

// Switch the HDMI output to 119.88 Hz. Probes first when needed; returns
// false (leaving the mode untouched) when unavailable or refused.
bool set_119_88();

// Restore the default output rate. No-op unless set_119_88() succeeded.
void restore_default();

}  // namespace videoout_rate
