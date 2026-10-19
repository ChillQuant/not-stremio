// Dolby Digital (AC-3) HDMI passthrough, v2.
//
// Bitstreams AC-3, E-AC-3 (Dolby Digital Plus / Atmos JOC), DTS Core, and
// AAC straight to the receiver over HDMI using the NID-resolved
// sceAudioOutEx* bitstream port (EVO Player v0.12.0's proven mechanism):
// IEC 61937 bursts (see iec61937.h) on codec-specific carrier ports.
//
// Carrier modes (from sceAudioOutExOpen's tables, FW 12.70):
//   AC-3:   mode 0, 256-frame grains, 48 kHz
//   DTS:    mode 2, 256-frame grains, 48 kHz
//   E-AC-3: mode 3, 1024-frame grains, 192 kHz
//   AAC:    mode 1, 256-frame grains, 48 kHz
//
// Everything fails closed to PCM:
//  - the Ex NIDs are resolved at runtime with dlsym, never statically
//    imported, so a firmware without them just yields PCM;
//  - any open/configure failure tears the port down and reports false, and
//    the player falls back to its normal PCM path;
//  - only the four codecs above are attempted (TrueHD stays on PCM).
//
// NOTE: EVO also probes the sink's EDID (sceAudioOutSysGetHdmiMonitorInfo)
// for codec support. That call's struct layout is not verifiable from any
// public source, so calling it with a guessed layout risks memory corruption;
// it is deliberately omitted. Safety instead comes from (1) the explicit
// opt-in setting, default off, (2) fail-closed NID resolution, and (3) the
// ExConfigureOutput return-code check.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <vector>

#include "iec61937.h"
#include "util.h"  // dlog

class Passthrough {
public:
    // Mirrors iec61937::Codec; None = passthrough unavailable.
    enum class PtCodec : int {
        None = -1,
        Ac3 = 0,
        Eac3 = 1,
        Dts = 2,
        Aac = 3,
    };
    static iec61937::Codec to_iec(PtCodec c);

    Passthrough() = default;
    Passthrough(const Passthrough&) = delete;
    Passthrough& operator=(const Passthrough&) = delete;
    ~Passthrough() { close(); }

    // Resolve the Ex NIDs for this firmware. False = not present -> PCM.
    bool resolve();

    // Open the bitstream port for `codec`, switch HDMI to bitstream,
    // send the null-burst lead-in, start the output thread.
    // False = fall back to PCM (port torn down, HDMI restored).
    bool open(PtCodec codec);
    // Lead-out null bursts, close the port, restore HDMI PCM audio,
    // stop the output thread. Safe to call when not active.
    void close();
    bool active() const { return active_; }
    PtCodec codec() const { return codec_; }

    // Seek: drop packer state and queued grains (the port stays up).
    void reset();

    // Pack one raw packet of the open codec; complete frames become grains
    // for the output thread. Returns audio samples queued.
    // For raw AAC (no ADTS headers), pass the AudioSpecificConfig via
    // set_aac_extradata() before the first push_packet().
    int push_packet(const uint8_t* data, size_t size);
    void set_aac_extradata(const uint8_t* ed, size_t ed_size);
    // End of stream: nothing pending for fixed-size frames (no-op).
    void flush() {}

    void set_paused(bool p) { paused_ = p; }

    size_t queued_bytes() const;
    // Seconds of audio queued ahead of the playhead.
    double queued_seconds() const;

private:
    void output_loop();
    static void* thread_main(void* self);
    void send_null_bursts(int ms);

    // NID-resolved entry points (null until resolve() succeeds).
    using ExOpenFn = int (*)(int user, int mode);
    using ExCloseFn = int (*)(int handle);
    using ExConfigFn = int (*)(int zero, unsigned flags, int mode, int target, uint64_t opt);
    ExOpenFn ex_open_ = nullptr;
    ExCloseFn ex_close_ = nullptr;
    ExConfigFn ex_config_ = nullptr;
    bool resolved_ = false;

    std::atomic<bool> active_{false};
    PtCodec codec_ = PtCodec::None;
    int ex_mode_ = -1;
    int grain_bytes_ = 1024;  // bytes per sceAudioOutOutput call
    int port_rate_ = 48000;
    int handle_ = -1;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{true};
    pthread_t thread_{};
    bool thread_live_ = false;

    mutable std::mutex m_;
    std::vector<uint8_t> stream_;   // unparsed packet bytes
    std::vector<uint8_t> ring_;     // contiguous grain bytes (flat ring)
    size_t head_ = 0;               // read offset into ring_
    // E-AC-3 block accumulation: one burst = 6 independent blocks.
    std::vector<uint8_t> eac3_pending_;
    int eac3_blocks_ = 0;
    // E-AC-3 media-clock state: dependent (7.1 extension) substream frames
    // ride along in the payload but must not advance the clock a second
    // time, and non-48 kHz content is normalized to the 48 kHz reference
    // the player's audio clock divides by.
    int eac3_rate_ = 48000;
    double eac3_clk_acc_ = 0.0;
    // One-shot logs for unusual-but-safe conditions.
    bool hd_logged_ = false;    // DTS-HD MA core extraction in effect
    bool dts14_logged_ = false; // 14-bit DTS seen (not carried)
    // Raw AAC: synthesized ADTS headers from the ASC extradata.
    std::vector<uint8_t> aac_asc_;
};
