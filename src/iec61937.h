// IEC 61937 burst packing for HDMI bitstream passthrough.
//
// Supports AC-3 (Dolby Digital), E-AC-3 (Dolby Digital Plus / Atmos JOC),
// DTS Core, and AAC (ADTS). Ported from EVO Player v0.12.0's iec61937.cpp
// (GPL-3.0-or-later, same as this project).
//
// Burst layout (all codecs):
//   word[0] = 0xF872 (Pa)   word[1] = 0x4E1F (Pb)
//   word[2] = Pc (data type) word[3] = Pd (payload length descriptor)
//   then the payload, each byte pair (a, b) as one word 0xaabb,
//   zero-padded to the burst size.
//
// Carrier modes (sceAudioOutExOpen / ExConfigureOutput, from FW 12.70 tables):
//   AC-3:  mode 0, 256-frame grains, 48 kHz port
//   DTS:   mode 2, 256-frame grains, 48 kHz port
//   E-AC-3: mode 3, 1024-frame grains, 192 kHz port
//   AAC:   mode 1, 256-frame grains, 48 kHz port

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace iec61937 {

// One packed burst is 6144 bytes (1536 samples * 4 bytes) for AC-3.
inline constexpr size_t kBurstBytes = 6144;
inline constexpr int kAc3SamplesPerFrame = 1536;

enum class Codec : uint8_t {
    Ac3,
    Eac3,
    Dts,
    Aac,
};

const char* codec_name(Codec codec);

// What the console needs to open the right bitstream port.
struct Carrier {
    int mode = -1;         // sceAudioOutExOpen / ExConfigureOutput mode
    int grain_frames = 0;  // stereo frames per sceAudioOutOutput call
    int sample_rate = 0;   // the port's rate
};
Carrier carrier_for(Codec codec);

// --- AC-3 (existing, unchanged) ---

// True when data[at..at+1] is the AC-3 sync word 0x0B77.
bool is_ac3_sync(const uint8_t* data, size_t size, size_t at);

// Parse the AC-3 frame header at `at` (must start with the sync word).
// Returns the frame size in bytes, or 0 when the header is invalid or the
// frame is incomplete. On success sets *samples (always 1536) and *bsmod.
size_t ac3_frame_size(const uint8_t* data, size_t size, size_t at, int* samples,
                      int* bsmod);

// Append one 6144-byte burst for the AC-3 frame [data, data + frame_size)
// to *out. Returns false on bad input (out left untouched).
bool write_ac3_burst(const uint8_t* data, size_t frame_size, int bsmod,
                      std::vector<uint8_t>* out);

// --- E-AC-3 ---

// E-AC-3 also starts with 0x0B77 but has bsid > 10 (11..16).
bool is_eac3_sync(const uint8_t* data, size_t size, size_t at);

// Parse one E-AC-3 frame. Sets *blocks (1/2/3/6 audio blocks), *samples
// (blocks * 256), *dependent (true for 7.1 extension substreams), and
// *sample_rate (32000/44100/48000 from fscod) when the pointers are set.
// Returns frame size in bytes, or 0 if invalid/incomplete.
size_t eac3_frame_size(const uint8_t* data, size_t size, size_t at,
                       int* blocks, int* samples, bool* dependent,
                       int* sample_rate = nullptr);

// Append one E-AC-3 burst (6 audio blocks = 1536 samples, 24576 bytes)
// built from the byte range [data, data + len). The caller accumulates
// frames until 6 independent blocks are collected; dependent substream
// frames are included in the payload but don't count toward the 6.
bool write_eac3_burst(const uint8_t* data, size_t len,
                      std::vector<uint8_t>* out);

// --- DTS ---

// The four DTS core sync-word forms, as 32-bit big-endian values:
// 16-bit BE 0x7FFE8001, 16-bit LE 0xFE7F0180, 14-bit BE 0x1FFFE800,
// 14-bit LE 0xFF1F00E8. Only the 16-bit forms are carried over IEC 61937
// (its payload is 16-bit words); the 14-bit forms are recognized so the
// packer can skip them explicitly instead of misframing.
enum class DtsForm : uint8_t {
    None,
    Be16,
    Le16,
    Be14,
    Le14,
};

// Which sync form (if any) starts at `at`. Needs 4 bytes.
DtsForm dts_sync_form(const uint8_t* data, size_t size, size_t at);

// DTS core sync word 0x7FFE8001 (big-endian, as stored in containers).
bool is_dts_sync(const uint8_t* data, size_t size, size_t at);

// DTS-HD Master Audio frame sync 0x64582025 ("dX%"). A DTS-HD frame
// embeds a backward-compatible DTS core; the core sync is found by
// scanning forward from here.
bool is_dtshd_sync(const uint8_t* data, size_t size, size_t at);

// Parse one DTS core frame header (10 bytes, big-endian 16-bit layout).
// Sets *samples (512, 1024, or 2048). Returns frame size in bytes, or 0
// if invalid. No bounds check against a larger buffer: the caller must
// ensure the full frame is present.
size_t dts_header_frame_size(const uint8_t hdr10[10], int* samples);

// Parse one DTS core frame. Sets *samples (512, 1024, or 2048).
// Returns frame size in bytes, or 0 if invalid/incomplete.
size_t dts_frame_size(const uint8_t* data, size_t size, size_t at, int* samples);

// Append one DTS burst (samples * 4 bytes) for the frame.
// The Pc data type (0x0B/0x0C/0x0D) is derived from the sample count.
bool write_dts_burst(const uint8_t* data, size_t frame_size, int samples,
                      std::vector<uint8_t>* out);

// --- AAC (ADTS) ---

// ADTS sync word 0xFFF (12 bits).
bool is_aac_sync(const uint8_t* data, size_t size, size_t at);

// Parse one ADTS frame. Returns frame size in bytes, or 0 if
// invalid/incomplete. ADTS frames always decode to 1024 samples.
size_t aac_frame_size(const uint8_t* data, size_t size, size_t at);

// Append one 4096-byte AAC burst (1024 samples). The payload is padded
// to a whole 16-bit word so the last byte is never dropped (EVO v0.12.0).
bool write_aac_burst(const uint8_t* data, size_t frame_size,
                      std::vector<uint8_t>* out);

// Synthesize a 7-byte ADTS header for raw AAC (from AudioSpecificConfig
// extradata) so it can go through the ADTS path. Returns false if the
// extradata isn't a valid ASC.
bool make_adts_header(const uint8_t* extradata, size_t extradata_size,
                      size_t raw_frame_size, uint8_t out_header[7]);

}  // namespace iec61937
