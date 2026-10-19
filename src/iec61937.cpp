// IEC 61937 burst packing. See iec61937.h for the layout.
// E-AC-3/DTS/AAC ported from EVO Player v0.12.0 (GPL-3.0-or-later).

#include "iec61937.h"

namespace iec61937 {

namespace {

// AC-3 frame size table: words (16-bit) per frame, indexed by
// [frmsizecod][fscod]. fscod: 0 = 48 kHz, 1 = 44.1 kHz, 2 = 32 kHz.
const int kAc3Words[38][3] = {
    {64, 69, 96},   {64, 70, 96},   {80, 87, 120},   {80, 88, 120},
    {96, 104, 144}, {96, 105, 144}, {112, 121, 168}, {112, 122, 168},
    {128, 139, 192},{128, 140, 192},{160, 174, 240}, {160, 175, 240},
    {192, 208, 288},{192, 209, 288},{224, 243, 336}, {224, 244, 336},
    {256, 278, 384},{256, 279, 384},{320, 348, 480}, {320, 349, 480},
    {384, 417, 576},{384, 418, 576},{448, 487, 672}, {448, 488, 672},
    {512, 557, 768},{512, 558, 768},{640, 696, 960}, {640, 697, 960},
    {768, 835, 1152},{768, 836, 1152},{896, 975, 1344},{896, 976, 1344},
    {1024, 1114, 1536},{1024, 1115, 1536},{1152, 1253, 1728},{1152, 1254, 1728},
    {1280, 1393, 1920},{1280, 1394, 1920},
};

constexpr int kEac3Blocks[4] = {1, 2, 3, 6};

constexpr uint16_t kPa = 0xF872;
constexpr uint16_t kPb = 0x4E1F;
constexpr uint16_t kTypeAc3 = 0x01;
constexpr uint16_t kTypeDts1 = 0x0B;  // 512 samples
constexpr uint16_t kTypeDts2 = 0x0C;  // 1024 samples
constexpr uint16_t kTypeDts3 = 0x0D;  // 2048 samples
constexpr uint16_t kTypeEac3 = 0x15;
constexpr uint16_t kTypeAac = 0x07;

// Write one burst: Pa Pb Pc Pd header, payload as big-endian words,
// zero-padded to burst_bytes.
void write_burst(uint16_t data_type, uint16_t length_code,
                 const uint8_t* payload, size_t payload_size,
                 size_t burst_bytes, std::vector<uint8_t>* out) {
    const size_t start = out->size();
    out->resize(start + burst_bytes, 0);
    uint8_t* b = out->data() + start;
    auto put = [&](size_t word, uint16_t v) {
        b[word * 2] = uint8_t(v & 0xFF);
        b[word * 2 + 1] = uint8_t(v >> 8);
    };
    put(0, kPa);
    put(1, kPb);
    put(2, data_type);
    put(3, length_code);
    for (size_t i = 0; i < payload_size; i += 2) {
        const uint8_t hi = payload[i];
        const uint8_t lo = i + 1 < payload_size ? payload[i + 1] : 0;
        put(4 + i / 2, uint16_t((hi << 8) | lo));
    }
}

}  // namespace

const char* codec_name(Codec codec) {
    switch (codec) {
        case Codec::Ac3: return "Dolby Digital (AC-3)";
        case Codec::Eac3: return "Dolby Digital Plus (E-AC-3)";
        case Codec::Dts: return "DTS";
        case Codec::Aac: return "AAC (ADTS)";
    }
    return "Unknown";
}

Carrier carrier_for(Codec codec) {
    // From sceAudioOutExOpen's own tables in libSceAudioOut (FW 12.70),
    // via EVO Player v0.12.0.
    switch (codec) {
        case Codec::Ac3: return {0, 256, 48000};
        case Codec::Dts: return {2, 256, 48000};
        case Codec::Eac3: return {3, 1024, 192000};
        case Codec::Aac: return {1, 256, 48000};
    }
    return {};
}

// --- AC-3 (unchanged) ---

bool is_ac3_sync(const uint8_t* data, size_t size, size_t at) {
    return at + 2 <= size && data[at] == 0x0B && data[at + 1] == 0x77;
}

size_t ac3_frame_size(const uint8_t* data, size_t size, size_t at, int* samples,
                      int* bsmod) {
    if (!is_ac3_sync(data, size, at) || at + 6 > size) return 0;
    int fscod = data[at + 4] >> 6;
    int frmsizecod = data[at + 4] & 0x3F;
    int bsid = data[at + 5] >> 3;
    if (fscod > 2 || frmsizecod > 37 || bsid > 10) return 0;
    size_t frame_size = size_t(kAc3Words[frmsizecod][fscod]) * 2;
    if (at + frame_size > size) return 0;
    if (samples) *samples = kAc3SamplesPerFrame;
    if (bsmod) *bsmod = data[at + 5] & 0x07;
    return frame_size;
}

bool write_ac3_burst(const uint8_t* data, size_t frame_size, int bsmod,
                      std::vector<uint8_t>* out) {
    if (!data || !out || frame_size == 0 || frame_size + 8 > kBurstBytes ||
        !is_ac3_sync(data, frame_size, 0))
        return false;
    write_burst(uint16_t(kTypeAc3 | ((bsmod & 0x07) << 8)),
                uint16_t(frame_size * 8), data, frame_size, kBurstBytes, out);
    return true;
}

// --- E-AC-3 ---

bool is_eac3_sync(const uint8_t* data, size_t size, size_t at) {
    return at + 6 <= size && data[at] == 0x0B && data[at + 1] == 0x77 &&
           (data[at + 5] >> 3) > 10;
}

size_t eac3_frame_size(const uint8_t* data, size_t size, size_t at,
                       int* blocks, int* samples, bool* dependent,
                       int* sample_rate) {
    if (!is_ac3_sync(data, size, at) || at + 6 > size) return 0;
    const int bsid = data[at + 5] >> 3;
    if (bsid <= 10 || bsid > 16) return 0;
    const int strmtyp = data[at + 2] >> 6;
    const int frmsiz = ((data[at + 2] & 7) << 8) | data[at + 3];
    const int fscod = data[at + 4] >> 6;
    const int numblkscod = (data[at + 4] >> 4) & 3;
    const size_t frame_size = size_t(frmsiz + 1) * 2;
    if (at + frame_size > size) return 0;
    const int nblocks = fscod == 3 ? 6 : kEac3Blocks[numblkscod];
    if (blocks) *blocks = nblocks;
    if (samples) *samples = nblocks * 256;
    if (dependent) *dependent = (strmtyp == 1);
    if (sample_rate) {
        static constexpr int kRates[3] = {48000, 44100, 32000};
        *sample_rate = fscod < 3 ? kRates[fscod] : 48000;
    }
    return frame_size;
}

bool write_eac3_burst(const uint8_t* data, size_t len,
                      std::vector<uint8_t>* out) {
    // One burst = 6 audio blocks (1536 samples). The port runs at 192 kHz,
    // so the burst lasts 6144 stereo frames: 24576 bytes.
    constexpr size_t kEac3Burst = 6144 * 4;
    if (!data || !out || len == 0 || len + 8 > kEac3Burst) return false;
    write_burst(kTypeEac3, uint16_t(len), data, len, kEac3Burst, out);
    return true;
}

// --- DTS ---

DtsForm dts_sync_form(const uint8_t* data, size_t size, size_t at) {
    if (at + 4 > size) return DtsForm::None;
    const uint32_t w = (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) |
                       (uint32_t(data[at + 2]) << 8) | uint32_t(data[at + 3]);
    switch (w) {
        case 0x7FFE8001u: return DtsForm::Be16;  // 16-bit big-endian
        case 0xFE7F0180u: return DtsForm::Le16;  // 16-bit little-endian
        case 0x1FFFE800u: return DtsForm::Be14;  // 14-bit big-endian
        case 0xFF1F00E8u: return DtsForm::Le14;  // 14-bit little-endian
        default: return DtsForm::None;
    }
}

bool is_dts_sync(const uint8_t* data, size_t size, size_t at) {
    return dts_sync_form(data, size, at) == DtsForm::Be16;
}

bool is_dtshd_sync(const uint8_t* data, size_t size, size_t at) {
    return at + 4 <= size && data[at] == 0x64 && data[at + 1] == 0x58 &&
           data[at + 2] == 0x20 && data[at + 3] == 0x25;
}

namespace {
// Shared core-header parse: 10 bytes in big-endian 16-bit layout.
size_t dts_core_header_size(const uint8_t h[10], int* samples) {
    const int nblks = ((h[4] & 1) << 6) | (h[5] >> 2);
    const int fsize = (((h[5] & 3) << 12) | (h[6] << 4) | (h[7] >> 4)) + 1;
    if (fsize <= 0) return 0;
    const int nsamples = (nblks + 1) * 32;
    if (nsamples != 512 && nsamples != 1024 && nsamples != 2048) return 0;
    if (samples) *samples = nsamples;
    return size_t(fsize);
}
}  // namespace

size_t dts_header_frame_size(const uint8_t hdr10[10], int* samples) {
    if (!hdr10) return 0;
    return dts_core_header_size(hdr10, samples);
}

size_t dts_frame_size(const uint8_t* data, size_t size, size_t at, int* samples) {
    if (dts_sync_form(data, size, at) != DtsForm::Be16) return 0;
    if (at + 10 > size) return 0;
    const size_t fsize = dts_core_header_size(data + at, samples);
    if (fsize == 0 || at + fsize > size) return 0;
    return fsize;
}

bool write_dts_burst(const uint8_t* data, size_t frame_size, int samples,
                      std::vector<uint8_t>* out) {
    uint16_t type;
    switch (samples) {
        case 512: type = kTypeDts1; break;
        case 1024: type = kTypeDts2; break;
        case 2048: type = kTypeDts3; break;
        default: return false;
    }
    const size_t burst = size_t(samples) * 4;
    if (!data || !out || frame_size == 0 || frame_size + 8 > burst ||
        !is_dts_sync(data, frame_size, 0))
        return false;
    write_burst(type, uint16_t(frame_size * 8), data, frame_size, burst, out);
    return true;
}

// --- AAC (ADTS) ---

bool is_aac_sync(const uint8_t* data, size_t size, size_t at) {
    return at + 7 <= size && data[at] == 0xFF && (data[at + 1] & 0xF6) == 0xF0;
}

size_t aac_frame_size(const uint8_t* data, size_t size, size_t at) {
    if (!is_aac_sync(data, size, at)) return 0;
    const size_t len = size_t(((data[at + 3] & 3) << 11) | (data[at + 4] << 3) |
                              (data[at + 5] >> 5));
    if (len < 7 || at + len > size) return 0;
    // Only single-block ADTS (1024 samples) is carried.
    const int blocks = (data[at + 6] & 3) + 1;
    if (blocks != 1) return 0;
    return len;
}

bool write_aac_burst(const uint8_t* data, size_t frame_size,
                      std::vector<uint8_t>* out) {
    // One ADTS frame per 4096-byte burst (1024 samples).
    constexpr size_t kAacBurst = 1024 * 4;
    if (!data || !out || frame_size == 0 || frame_size + 8 > kAacBurst ||
        !is_aac_sync(data, frame_size, 0))
        return false;
    // Pd: payload bits, padded up to a whole 16-bit word so the last byte
    // is never dropped (EVO v0.12.0 fix).
    const size_t padded = (frame_size + 1) & ~size_t{1};
    write_burst(kTypeAac, uint16_t(padded * 8), data, frame_size, kAacBurst, out);
    return true;
}

bool make_adts_header(const uint8_t* extradata, size_t extradata_size,
                      size_t raw_frame_size, uint8_t out_header[7]) {
    // AudioSpecificConfig: 5 bits object type, 4 bits sample rate index,
    // 4 bits channel config.
    if (!extradata || extradata_size < 2 || !out_header) return false;
    const int obj_type = (extradata[0] >> 3) & 0x1F;
    const int sr_idx = ((extradata[0] & 0x07) << 1) | ((extradata[1] >> 7) & 0x01);
    const int ch_cfg = (extradata[1] >> 3) & 0x0F;
    if (obj_type < 1 || obj_type > 4 || sr_idx > 12 || ch_cfg == 0 || ch_cfg > 7)
        return false;
    const int profile = obj_type - 1;  // ADTS profile = ASC object type - 1
    const size_t total = raw_frame_size + 7;
    if (total > 0x1FFF) return false;
    out_header[0] = 0xFF;
    out_header[1] = 0xF1;  // sync + MPEG-4 + layer 0 + protection absent
    out_header[2] = uint8_t((profile << 6) | (sr_idx << 2) | ((ch_cfg >> 2) & 0x01));
    out_header[3] = uint8_t(((ch_cfg & 0x03) << 6) | ((total >> 11) & 0x03));
    out_header[4] = uint8_t((total >> 3) & 0xFF);
    out_header[5] = uint8_t(((total & 0x07) << 5) | 0x1F);
    out_header[6] = 0xFC;  // 1 raw data block, rest padding
    return true;
}

}  // namespace iec61937
