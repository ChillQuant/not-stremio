#pragma once

// Read-only HDR10 diagnostic probe via libSceVideoOut (dlsym, never linked).
//
// Opens a throwaway VideoOut handle, reads sceVideoOutGetOutputStatus, logs
// what the HDMI output reports (dynamic_range: 0 unknown, 1 SDR, 2 HDR),
// and closes the handle. Probe-only: no Settings-driven rendering changes,
// and HDR is never enabled -- this file does not even reference the buffer
// attribute functions, so the 8-bit EGL swapchain cannot be retyped.
//
// Fails closed: any missing symbol or failed open logs
// "HDR10 output not available: <reason>" and the app continues in SDR.
//
// Runs only when the user enabled the "HDR10 diagnostic probe" setting
// (default OFF). Call once at startup; never call mid-frame. Holds no
// VideoOut handle across frames.
namespace hdr_probe {

// width/height are the display dimensions, logged for context.
void hdr_signal_probe(int width, int height);

}  // namespace hdr_probe
