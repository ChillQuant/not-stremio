// Read-only HDR10 diagnostic probe (Task 3.1), modeled on src/videoout_rate.cpp.
//
// Purpose: determine, without touching the picture, what the HDMI output
// reports. Opens its own throwaway VideoOut handle, reads
// sceVideoOutGetOutputStatus (dynamic_range: 0 unknown, 1 SDR, 2 HDR),
// logs the result via dlog(), and closes the handle. NOTHING is submitted,
// retyped, or changed: this probe never calls sceVideoOutSetBufferAttribute2
// or sceVideoOutSubmitChangeBufferAttribute2, so there is nothing to roll
// back and the display cannot be disturbed.
//
// What llvm-nm-18 showed about the EGL driver (2026-10-14, ps5-opengl
// sdk 1.0.0, libps5_opengl_core33.a): the driver imports 14 sceVideoOut
// functions as undefined symbols (sceVideoOutOpen/Close,
// GetOutputStatus, RegisterBuffers2, SetBufferAttribute2, SubmitFlip,
// ...). Notably ABSENT: sceVideoOutSubmitChangeBufferAttribute2 -- the
// call EVO Player uses for its HDR10 retype. The driver only programs
// buffer attributes at registration time; it has no mid-stream retype path.
//
// Linker-wrap analysis (--wrap=sceVideoOutOpen): viable IN PRINCIPLE. The
// driver's references are ordinary undefined symbols resolved at our final
// static link, so --wrap would redirect the driver's internal open call to
// our __wrap_sceVideoOutOpen and let us capture the EGL-owned VideoOut
// handle (PS5 NID dynamic loading happens after the static link, so the
// rewrite applies cleanly). NOT implemented here: capturing the handle is
// only half the problem -- submitting attribute changes on a handle whose
// buffer set is owned by the driver's flip thread races with the driver,
// and EVO's safety rules (GPU idle, never mid-frame) cannot be guaranteed
// against a closed-source driver's internal state. Any future Phase 2 work
// must solve that ownership problem first, on hardware, with the TV's HDR
// indicator as the only accepted proof.
//
// Hard rule, enforced by construction: this file contains no reference to
// SetBufferAttribute2/SubmitChangeBufferAttribute2 at all, so the 8-bit EGL
// swapchain can never be retyped as HDR10 (which would reinterpret pixel
// packing and corrupt the display).
//
// Fails closed: any missing symbol or failed open logs the reason and the
// app continues in SDR. The handle is opened per probe and closed before
// return; no handle is held across frames.
//
// Call once at startup when the user enabled the "HDR10 diagnostic probe"
// setting (default OFF). Never call mid-frame.

#include "hdr_probe.h"

#include <cstdint>

#include <dlfcn.h>

#include "util.h"  // dlog

namespace hdr_probe {
namespace {

// ABI per EVO's evo_agc_runtime.c (evo_vo_output_status).
struct OutputStatus
{
  uint32_t resolution;
  uint32_t dynamic_range;  // 0 unknown, 1 SDR, 2 HDR
  uint64_t refresh_rate;
  uint64_t flags;  // bit 0: HDR output active
  uint64_t reserved[3];
};

using OpenFn = int32_t (*)(int32_t user_id, int32_t type, int32_t index, const void *param);
using CloseFn = int32_t (*)(int32_t handle);
using GetStatusFn = int32_t (*)(int32_t handle, OutputStatus *status);

OpenFn g_open = nullptr;
CloseFn g_close = nullptr;
GetStatusFn g_get_status = nullptr;
bool g_resolved = false;

void resolve()
{
  if (g_resolved)
    return;
  g_resolved = true;
  g_open = reinterpret_cast<OpenFn>(dlsym(RTLD_DEFAULT, "sceVideoOutOpen"));
  g_close = reinterpret_cast<CloseFn>(dlsym(RTLD_DEFAULT, "sceVideoOutClose"));
  g_get_status =
    reinterpret_cast<GetStatusFn>(dlsym(RTLD_DEFAULT, "sceVideoOutGetOutputStatus"));
  dlog("hdr_probe: dlsym open=%d close=%d getStatus=%d", !!g_open, !!g_close,
    !!g_get_status);
}

}  // namespace

void hdr_signal_probe(int width, int height)
{
  static bool done = false;  // one-time by construction; idempotent anyway
  if (done)
    return;
  done = true;

  dlog("hdr_probe: starting read-only HDR10 diagnostic (%dx%d)", width, height);

  resolve();
  if (!g_open || !g_close || !g_get_status)
  {
    dlog("hdr_probe: HDR10 output not available: VideoOut symbols missing - staying SDR");
    return;
  }

  // Throwaway handle (0xff/0/0, the pattern from the PS5 homebrew samples
  // and videoout_rate.cpp): used only for the status query, closed before
  // return on every path. The EGL driver's own handle is untouched.
  const int32_t h = g_open(0xff, 0, 0, nullptr);
  if (h < 0)
  {
    dlog("hdr_probe: HDR10 output not available: sceVideoOutOpen failed (%d) - staying SDR", h);
    return;
  }

  OutputStatus st{};
  const int32_t rc = g_get_status(h, &st);
  if (rc != 0)
  {
    dlog("hdr_probe: HDR10 output not available: GetOutputStatus rc=%d (0x%08x) - staying SDR",
      rc, static_cast<unsigned>(rc));
  }
  else
  {
    dlog("hdr_probe: GetOutputStatus dynamic_range=%u (0 unknown, 1 SDR, 2 HDR) "
         "resolution=%u refresh=%llu flags=0x%llx",
      st.dynamic_range, st.resolution,
      static_cast<unsigned long long>(st.refresh_rate),
      static_cast<unsigned long long>(st.flags));
    if (st.dynamic_range == 2)
      dlog("hdr_probe: TV reports HDR output active (read-only observation; nothing changed)");
    else
      dlog("hdr_probe: TV reports SDR (dynamic_range=%u); nothing changed", st.dynamic_range);
  }

  g_close(h);
  dlog("hdr_probe: done - display untouched, still SDR");
}

}  // namespace hdr_probe
