// HDMI bitstream passthrough (AC-3, E-AC-3, DTS, AAC). See pt_out.h.
// IEC 61937 framing ported from EVO Player v0.12.0 (GPL-3.0-or-later).

#include "pt_out.h"

#include <chrono>
#include <cstring>
#include <pthread.h>
#include <thread>

#include "iec61937.h"
#include "pcm_out.h"

extern "C" int sceAudioOutOutput(int handle, const void* samples);

#ifdef PLATFORM_PS5_NATIVE
#include <dlfcn.h>
extern "C" {
int sceAudioOutInit(void);
}
#endif

namespace {

constexpr int kTargetHdmi = 1;

// NIDs verified against the public aerolib symbol list (FW 12.70 dump);
// resolved at runtime so a firmware without them fails closed to PCM.
const char* kNidExOpen = "6X6dp+07h4U";    // sceAudioOutExOpen
const char* kNidExClose = "0TfjSulCV2A";   // sceAudioOutExClose
const char* kNidExConfig = "VcE+gXSwFXI";  // sceAudioOutExConfigureOutput

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// EVO's HDMI-reset settle: at least 1 s between the last restore and a new open.
int64_t g_last_reset_ms = 0;

}  // namespace

iec61937::Codec Passthrough::to_iec(PtCodec c) {
    switch (c) {
        case PtCodec::Ac3: return iec61937::Codec::Ac3;
        case PtCodec::Eac3: return iec61937::Codec::Eac3;
        case PtCodec::Dts: return iec61937::Codec::Dts;
        case PtCodec::Aac: return iec61937::Codec::Aac;
        default: return iec61937::Codec::Ac3;
    }
}

bool Passthrough::resolve() {
    if (resolved_) return true;
#ifdef PLATFORM_PS5_NATIVE
    ex_open_ = reinterpret_cast<ExOpenFn>(dlsym(RTLD_DEFAULT, kNidExOpen));
    ex_close_ = reinterpret_cast<ExCloseFn>(dlsym(RTLD_DEFAULT, kNidExClose));
    ex_config_ = reinterpret_cast<ExConfigFn>(dlsym(RTLD_DEFAULT, kNidExConfig));
    if (!ex_open_) ex_open_ = reinterpret_cast<ExOpenFn>(dlsym(RTLD_DEFAULT, "sceAudioOutExOpen"));
    if (!ex_close_) ex_close_ = reinterpret_cast<ExCloseFn>(dlsym(RTLD_DEFAULT, "sceAudioOutExClose"));
    if (!ex_config_) ex_config_ = reinterpret_cast<ExConfigFn>(dlsym(RTLD_DEFAULT, "sceAudioOutExConfigureOutput"));
#endif
    if (!ex_open_ || !ex_close_ || !ex_config_) {
        ex_open_ = nullptr;
        ex_close_ = nullptr;
        ex_config_ = nullptr;
        dlog("pt: Ex NIDs not on this firmware, passthrough unavailable");
        return false;
    }
    resolved_ = true;
    return true;
}

void Passthrough::send_null_bursts(int ms) {
    if (handle_ < 1) return;
    std::vector<uint8_t> grain(grain_bytes_, 0);
    grain[0] = 0x72;  // Pa 0xF872, little-endian
    grain[1] = 0xF8;
    grain[2] = 0x1F;  // Pb 0x4E1F, little-endian
    grain[3] = 0x4E;
    // Pc = 0 (null), Pd = 0, payload all zero.
    const int grain_frames = grain_bytes_ / 4;
    int grains = ms * port_rate_ / 1000 / grain_frames;
    for (int i = 0; i < grains; ++i) {
#ifdef PLATFORM_PS5_NATIVE
        sceAudioOutOutput(handle_, grain.data());
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(ms / (grains > 0 ? grains : 1)));
#endif
    }
}

bool Passthrough::open(PtCodec codec) {
    if (active_) return codec_ == codec;
    if (codec == PtCodec::None) return false;
    if (!resolved_ && !resolve()) return false;

    const iec61937::Carrier carrier = iec61937::carrier_for(to_iec(codec));
    if (carrier.mode < 0) return false;

    // The PS5 allows one audio port per process. Signal that the player
    // wants the port and wait for UI sounds to release it, mirroring
    // PcmOut::open(). Without this, ex_open_ fails with port-busy when
    // UI sounds are enabled.
    AudioPort::player_wants = true;
    for (int i = 0; i < 300 && AudioPort::ui_holds.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    int64_t since = now_ms() - g_last_reset_ms;
    if (since < 500) sleep_ms(int(500 - since));

#ifdef PLATFORM_PS5_NATIVE
    // Without this, the Ex* port opens fail when UI sounds are disabled and
    // nothing else has initialized the audio library. Accept 0 or
    // already-initialized, mirroring PcmOut::open().
    int init = sceAudioOutInit();
    if (init != 0 && init != int(0x8026000e)) {
        dlog("pt: sceAudioOutInit 0x%08x, falling back to PCM", unsigned(init));
        AudioPort::player_wants = false;
        return false;
    }
#endif

    int h = ex_open_(0xFF, carrier.mode);
    if (h < 1) {
        dlog("pt: ExOpen(mode %d) failed (%d), falling back to PCM", carrier.mode, h);
        AudioPort::player_wants = false;
        return false;
    }
    int rc = ex_config_(0, 0, carrier.mode, kTargetHdmi, 0);
    if (rc < 0) {
        dlog("pt: ExConfigureOutput(mode %d) failed (0x%x), falling back to PCM",
             carrier.mode, rc);
        ex_close_(h);
        ex_config_(0, 0, 0xFF, 0xFF, 0);
        g_last_reset_ms = now_ms();
        AudioPort::player_wants = false;
        return false;
    }
    handle_ = h;
    codec_ = codec;
    ex_mode_ = carrier.mode;
    grain_bytes_ = carrier.grain_frames * 4;
    port_rate_ = carrier.sample_rate;
    eac3_rate_ = 48000;
    eac3_clk_acc_ = 0.0;
    hd_logged_ = false;
    dts14_logged_ = false;

    send_null_bursts(200);  // lead-in so the receiver locks before audio

    stop_ = false;
    paused_ = true;  // apply_run_state() unpauses once playback runs
    if (pthread_create(&thread_, nullptr, thread_main, this) != 0) {
        dlog("pt: output thread failed, falling back to PCM");
        sceAudioOutOutput(handle_, nullptr);
        ex_close_(handle_);
        ex_config_(0, 0, 0xFF, 0xFF, 0);
        g_last_reset_ms = now_ms();
        handle_ = -1;
        codec_ = PtCodec::None;
        AudioPort::player_wants = false;
        return false;
    }
    thread_live_ = true;
    active_ = true;
    dlog("pt: %s passthrough open (mode %d, %d Hz, %d-byte grains)",
         iec61937::codec_name(to_iec(codec)), carrier.mode, carrier.sample_rate,
         grain_bytes_);
    return true;
}

void Passthrough::close() {
    if (!active_) return;
    active_ = false;

    stop_ = true;
    if (thread_live_) {
        pthread_join(thread_, nullptr);
        thread_live_ = false;
    }

    send_null_bursts(250);  // lead-out
#ifdef PLATFORM_PS5_NATIVE
    sceAudioOutOutput(handle_, nullptr);
    ex_close_(handle_);
    sleep_ms(125);
    ex_config_(0, 0, 0xFF, 0xFF, 0);
#else
    sleep_ms(125);
#endif
    g_last_reset_ms = now_ms();
    sleep_ms(200);

    handle_ = -1;
    codec_ = PtCodec::None;
    AudioPort::player_wants = false;
    std::lock_guard<std::mutex> lock(m_);
    ring_.clear();
    head_ = 0;
    stream_.clear();
    eac3_pending_.clear();
    eac3_blocks_ = 0;
}

void Passthrough::reset() {
    std::lock_guard<std::mutex> lock(m_);
    stream_.clear();
    ring_.clear();
    head_ = 0;
    eac3_pending_.clear();
    eac3_blocks_ = 0;
    eac3_rate_ = 48000;
    eac3_clk_acc_ = 0.0;
    hd_logged_ = false;
    dts14_logged_ = false;
}

void Passthrough::set_aac_extradata(const uint8_t* ed, size_t ed_size) {
    std::lock_guard<std::mutex> lock(m_);
    aac_asc_.assign(ed, ed + ed_size);
}

int Passthrough::push_packet(const uint8_t* data, size_t size) {
    if (!active_ || !data || size == 0) return 0;
    std::lock_guard<std::mutex> lock(m_);
    stream_.insert(stream_.end(), data, data + size);

    // Compact the ring if the head has advanced past 1 MB (like PcmOut).
    auto compact_ring = [&]() {
        if (head_ > (1u << 20) && head_ * 2 > ring_.size()) {
            ring_.erase(ring_.begin(), ring_.begin() + ptrdiff_t(head_));
            head_ = 0;
        }
    };
    auto emit_burst = [&](const std::vector<uint8_t>& burst, int samples) -> int {
        compact_ring();
        ring_.insert(ring_.end(), burst.begin(), burst.end());
        return samples;
    };

    int samples = 0;
    size_t cursor = 0;
    const uint8_t* s = stream_.data();
    const size_t n = stream_.size();

    if (codec_ == PtCodec::Ac3) {
        while (cursor < n) {
            while (cursor + 2 <= n && !iec61937::is_ac3_sync(s, n, cursor))
                ++cursor;
            int frame_samples = 0, bsmod = 0;
            size_t fsize = iec61937::ac3_frame_size(s, n, cursor, &frame_samples, &bsmod);
            if (fsize == 0) {
                if (cursor + 6 <= n) ++cursor;
                else break;
                continue;
            }
            std::vector<uint8_t> burst;
            if (!iec61937::write_ac3_burst(s + cursor, fsize, bsmod, &burst)) {
                ++cursor;
                continue;
            }
            cursor += fsize;
            samples += emit_burst(burst, frame_samples);
        }
    } else if (codec_ == PtCodec::Eac3) {
        // Accumulate independent frames until 6 audio blocks, then emit one
        // burst. Dependent (7.1 extension) frames ride along in the payload
        // but don't count toward the 6 and don't advance the media clock:
        // they extend the same audio blocks, not additional media time.
        while (cursor < n) {
            while (cursor + 2 <= n && !iec61937::is_ac3_sync(s, n, cursor))
                ++cursor;
            int blocks = 0, frame_samples = 0, rate = 0;
            bool dependent = false;
            size_t fsize = iec61937::eac3_frame_size(s, n, cursor, &blocks,
                                                     &frame_samples, &dependent,
                                                     &rate);
            if (fsize == 0) {
                if (cursor + 6 <= n) ++cursor;
                else break;
                continue;
            }
            if (rate > 0) eac3_rate_ = rate;
            eac3_pending_.insert(eac3_pending_.end(), s + cursor, s + cursor + fsize);
            if (eac3_pending_.size() > 49152) {
                // Malformed stream (never reaches 6 independent blocks):
                // drop the accumulation instead of growing forever.
                dlog("pt: E-AC-3 accumulation overrun, resyncing");
                eac3_pending_.clear();
                eac3_blocks_ = 0;
            }
            if (!dependent) {
                eac3_blocks_ += blocks;
                // Media time at the player's 48 kHz reference: dependent
                // frames must not advance the clock a second time, and
                // 44.1/32 kHz content is scaled so the clock stays true.
                eac3_clk_acc_ += double(frame_samples) * 48000.0 / double(eac3_rate_);
                const int s48 = int(eac3_clk_acc_);
                eac3_clk_acc_ -= s48;
                samples += s48;
            }
            cursor += fsize;
            if (eac3_blocks_ >= 6) {
                std::vector<uint8_t> burst;
                if (iec61937::write_eac3_burst(eac3_pending_.data(),
                                               eac3_pending_.size(), &burst)) {
                    emit_burst(burst, 0);  // samples already counted above
                }
                eac3_pending_.clear();
                eac3_blocks_ = 0;
            }
        }
    } else if (codec_ == PtCodec::Dts) {
        while (cursor < n) {
            iec61937::DtsForm form = iec61937::DtsForm::None;
            while (cursor + 4 <= n &&
                   (form = iec61937::dts_sync_form(s, n, cursor)) ==
                       iec61937::DtsForm::None) {
                // DTS-HD MA frames start with the HD sync; the DTS core is
                // embedded further in. Note it once, then keep scanning:
                // the core sync below is what actually gets carried.
                if (!hd_logged_ && iec61937::is_dtshd_sync(s, n, cursor)) {
                    hd_logged_ = true;
                    dlog("pt: DTS-HD MA detected, extracting the DTS core");
                }
                ++cursor;
            }
            if (form == iec61937::DtsForm::None) break;
            if (form == iec61937::DtsForm::Be14 ||
                form == iec61937::DtsForm::Le14) {
                // 14-bit DTS is a museum format and IEC 61937 payloads are
                // 16-bit words; carrying it would need unverified bit
                // surgery, so it is skipped loudly instead of misframed.
                if (!dts14_logged_) {
                    dts14_logged_ = true;
                    dlog("pt: 14-bit DTS is not carried over IEC 61937, skipping");
                }
                ++cursor;
                continue;
            }
            // Normalize to big-endian 16-bit: the parser and the burst
            // packer both expect it.
            int frame_samples = 0;
            size_t fsize = 0;
            std::vector<uint8_t> norm;
            const uint8_t* frame = s + cursor;
            if (cursor + 10 > n) break;  // wait for the core header
            if (form == iec61937::DtsForm::Le16) {
                // Byte-swap the 10-byte header to parse the frame size,
                // then swap the whole frame: the packer wants BE16.
                uint8_t hdr[10];
                for (int i = 0; i < 10; i += 2) {
                    hdr[i] = s[cursor + i + 1];
                    hdr[i + 1] = s[cursor + i];
                }
                fsize = iec61937::dts_header_frame_size(hdr, &frame_samples);
            } else {
                fsize = iec61937::dts_header_frame_size(s + cursor, &frame_samples);
            }
            if (fsize == 0) {
                ++cursor;  // invalid header: skip this sync candidate
                continue;
            }
            if (cursor + fsize > n) break;  // incomplete frame: wait for more
            if (form == iec61937::DtsForm::Le16) {
                norm.resize(fsize);
                for (size_t i = 0; i + 1 < fsize; i += 2) {
                    norm[i] = s[cursor + i + 1];
                    norm[i + 1] = s[cursor + i];
                }
                if (fsize & 1) norm[fsize - 1] = s[cursor + fsize - 1];
                frame = norm.data();
            }
            std::vector<uint8_t> burst;
            if (!iec61937::write_dts_burst(frame, fsize, frame_samples, &burst)) {
                ++cursor;
                continue;
            }
            cursor += fsize;
            samples += emit_burst(burst, frame_samples);
        }
    } else if (codec_ == PtCodec::Aac) {
        while (cursor < n) {
            // Raw AAC (no ADTS): synthesize a header from the ASC extradata.
            if (!aac_asc_.empty() && !iec61937::is_aac_sync(s, n, cursor)) {
                uint8_t hdr[7];
                // Treat the rest of the packet as one raw frame.
                const size_t raw_len = n - cursor;
                if (iec61937::make_adts_header(aac_asc_.data(), aac_asc_.size(),
                                               raw_len, hdr)) {
                    std::vector<uint8_t> frame(hdr, hdr + 7);
                    frame.insert(frame.end(), s + cursor, s + n);
                    std::vector<uint8_t> burst;
                    if (iec61937::write_aac_burst(frame.data(), frame.size(), &burst))
                        samples += emit_burst(burst, 1024);
                }
                cursor = n;
                break;
            }
            while (cursor + 2 <= n && !iec61937::is_aac_sync(s, n, cursor))
                ++cursor;
            size_t fsize = iec61937::aac_frame_size(s, n, cursor);
            if (fsize == 0) {
                if (cursor + 7 <= n) ++cursor;
                else break;
                continue;
            }
            std::vector<uint8_t> burst;
            if (!iec61937::write_aac_burst(s + cursor, fsize, &burst)) {
                ++cursor;
                continue;
            }
            cursor += fsize;
            samples += emit_burst(burst, 1024);
        }
    }

    if (cursor > 0) stream_.erase(stream_.begin(), stream_.begin() + cursor);
    if (stream_.size() > 65536) stream_.clear();
    return samples;
}

size_t Passthrough::queued_bytes() const {
    std::lock_guard<std::mutex> lock(m_);
    return ring_.size() - head_;
}

double Passthrough::queued_seconds() const {
    // Stereo S16 grains: 4 bytes per port frame.
    return double(queued_bytes()) / double(port_rate_ > 0 ? port_rate_ : 48000) / 4.0;
}

void* Passthrough::thread_main(void* self) {
    static_cast<Passthrough*>(self)->output_loop();
    return nullptr;
}

void Passthrough::output_loop() {
    // Never pthread_setname_np here: it hangs on the console (see pcm_out).
    const int gb = grain_bytes_ > 0 ? grain_bytes_ : 1024;
    std::vector<uint8_t> grain(gb, 0);
    const std::vector<uint8_t> silence(gb, 0);
    const int grain_frames = gb / 4;
    while (!stop_) {
        bool got = false;
        if (!paused_) {
            std::lock_guard<std::mutex> lock(m_);
            if (ring_.size() - head_ >= size_t(gb)) {
                std::memcpy(grain.data(), ring_.data() + head_, gb);
                head_ += gb;
                if (head_ == ring_.size()) {
                    ring_.clear();
                    head_ = 0;
                }
                got = true;
            }
        }
        if (got) {
#ifdef PLATFORM_PS5_NATIVE
            if (sceAudioOutOutput(handle_, grain.data()) < 0) sleep_ms(5);
#else
            std::this_thread::sleep_for(
                std::chrono::microseconds(int64_t(grain_frames) * 1000000 / port_rate_));
#endif
        } else if (paused_) {
            sleep_ms(2);
        } else {
#ifdef PLATFORM_PS5_NATIVE
            sceAudioOutOutput(handle_, silence.data());
#else
            std::this_thread::sleep_for(
                std::chrono::microseconds(int64_t(grain_frames) * 1000000 / port_rate_));
#endif
        }
    }
}
