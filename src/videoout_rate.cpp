// HDMI refresh-rate control for the 119.88 Hz film mode.
//
// Mechanism (from EVO Player, verified on FW 12.70 hardware): a game-slot
// app may call sceVideoOutConfigureOutput(handle, 0xf, NULL, NULL, 0) to
// switch the HDMI output to 119.88 Hz; every other non-default rate is
// refused with 0x8029001e. At 119.88 Hz, 23.976/24fps film is shown with
// exact 5:5 pulldown instead of 3:2 judder.
//
// Safety design:
//  - Symbols are resolved with dlsym by NAME (EVO: by-name works in the app
//    module; by raw NID fails). If libSceVideoOut is unreachable, every
//    entry point below returns "not available".
//  - Availability is probed with sceVideoOutIsOutputSupported, a pure query
//    that never changes the output mode (no screen blank at probe time).
//  - The switch itself only happens at playback start, never mid-playback,
//    and the default rate is restored when playback stops.
//  - The video-out handle is opened per operation (0xff/0/0, the pattern from
//    the PS5 homebrew samples) and closed immediately after; no handle is
//    held across frames, so EGL's own display state is never disturbed.

#include "videoout_rate.h"

#include <dlfcn.h>

#include "util.h"  // dlog

namespace videoout_rate {
namespace {

// UI-thread-only by construction: every entry point below is called solely
// from Session::watch_start/watch_stop/watch_update, which all run on the UI
// thread (the player's demux/audio/video threads never touch this module).
// Hence the plain globals (g_probe, g_switched, g_resolved, …) need no locks;
// if a non-UI caller is ever added, this assumption must be revisited.
// libSceVideoOut entry points. Signatures per EVO's hardware-verified
// declarations (evo_agc_runtime.c) and the PS5 homebrew samples.
using OpenFn = int (*)(int user_id, int bus_type, int index, const void* param);
using CloseFn = int (*)(int handle);
using IsSupportedFn = int (*)(int handle, unsigned long long mode, const void* options,
                              const void* reserved0, unsigned long long reserved1);
using ConfigureFn = int (*)(int handle, unsigned long long mode, const void* options,
                             const void* reserved0, unsigned long long reserved1);

OpenFn g_open = nullptr;
CloseFn g_close = nullptr;
IsSupportedFn g_is_supported = nullptr;
ConfigureFn g_configure = nullptr;
bool g_resolved = false;

void resolve() {
    if (g_resolved) return;
    g_resolved = true;
    g_open = reinterpret_cast<OpenFn>(dlsym(RTLD_DEFAULT, "sceVideoOutOpen"));
    g_close = reinterpret_cast<CloseFn>(dlsym(RTLD_DEFAULT, "sceVideoOutClose"));
    g_is_supported =
        reinterpret_cast<IsSupportedFn>(dlsym(RTLD_DEFAULT, "sceVideoOutIsOutputSupported"));
    g_configure =
        reinterpret_cast<ConfigureFn>(dlsym(RTLD_DEFAULT, "sceVideoOutConfigureOutput"));
    dlog("videoout: dlsym open=%d close=%d isSupported=%d configure=%d", !!g_open, !!g_close,
         !!g_is_supported, !!g_configure);
}

// Output modes from EVO's verified table: 0x1 default, 0x2 23.976,
// 0x3 24, 0x9 50, 0xf 119.88.
constexpr unsigned long long kModeDefault = 0x1;
constexpr unsigned long long kMode119_88 = 0xf;

int g_probe = -1;  // -1 unknown, 0 unavailable, 1 available
bool g_switched = false;

int open_handle() {
    int h = g_open(0xff, 0, 0, nullptr);
    return h;
}

}  // namespace

bool probe_119_88() {
    if (g_probe >= 0) return g_probe == 1;
    g_probe = 0;
    resolve();
    if (!g_open || !g_close || !g_is_supported) {
        dlog("videoout: 119.88 Hz probe unavailable (symbols missing)");
        return false;
    }
    int h = open_handle();
    if (h < 0) {
        dlog("videoout: sceVideoOutOpen failed");
        return false;
    }
    int r = g_is_supported(h, kMode119_88, nullptr, nullptr, 0);
    g_close(h);
    g_probe = (r == 0) ? 1 : 0;
    dlog("videoout: 119.88 Hz probe %s (rc=%d)", r == 0 ? "supported" : "refused", r);
    return g_probe == 1;
}

bool set_119_88() {
    if (g_switched) return true;
    resolve();
    if (!probe_119_88() || !g_configure) return false;
    int h = open_handle();
    if (h < 0) return false;
    int r = g_configure(h, kMode119_88, nullptr, nullptr, 0);
    g_close(h);
    if (r != 0) {
        g_probe = 0;  // refused at configure time: don't try again this run
        dlog("videoout: switch to 119.88 Hz refused (rc=%d)", r);
        return false;
    }
    g_switched = true;
    dlog("videoout: HDMI output at 119.88 Hz (film mode)");
    return true;
}

void restore_default() {
    if (!g_switched) return;
    g_switched = false;
    resolve();
    if (!g_configure) return;
    int h = open_handle();
    if (h < 0) return;
    int r = g_configure(h, kModeDefault, nullptr, nullptr, 0);
    g_close(h);
    dlog("videoout: HDMI output restored to default (rc=%d)", r);
}

}  // namespace videoout_rate
