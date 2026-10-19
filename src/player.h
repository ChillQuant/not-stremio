#pragma once

#include "aniskip.h"
#include "pcm_out.h"
#include "pt_out.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hwdec_ps5.h"
#include "netstream.h"
#include "subtitles.h"
#include "yuv_convert.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVCodecParameters;
struct AVPacket;
struct AVFrame;
struct SwsContext;
struct SwrContext;

// Plays one URL with FFmpeg; video on the PS5's hardware decoder when it
// takes the stream (hwdec_ps5.h), else FFmpeg's software decoder. Threads:
// demux, video decode (+ scale to screen size), audio decode (+ resample
// into the audio output's queue). The audio queue is the master clock; without
// audio, the wall clock. The UI thread calls present() every frame and shows
// the picture it returns.
class Player {
public:
	struct Track {
		int stream = -1;
		std::string label, lang;
	};
	struct Options {
		std::string url;
		std::vector<std::string> headers;
		double start = 0;
		std::vector<std::string> audio_langs;  // preferred, ISO 639-2
		// A direct link (not the streaming server): a big file may be
		// downloaded over several connections (netstream.h).
		bool parallel = false;
		// GPU YUV->RGB conversion (gl/gpu_convert.hpp): the session sets
		// this when its shaders compiled. The player still decides per
		// stream (only YUV 4:2:0, 8/10-bit, goes through the GPU path).
		bool allow_gpu_convert = false;
		// Output cap for emitted frames: 1920x1080 normally, 3840x2160 when
		// the GPU path runs on a 2160p display.
		int max_out_w = 1920, max_out_h = 1080;
		// Audio output: 0 = stereo downmix, 1 = discrete 5.1/7.1 PCM over
		// HDMI (needs eARC or a receiver; falls back to stereo if the
		// console refuses the multichannel port).
		int audio_output = 0;
		// Volume boost in percent (100, 125, 150, 200): makeup gain for
		// quiet masters, with soft-clipping so peaks don't distort.
		int volume_boost = 100;
		// Master volume in percent (0-100, default 100): swipe-controlled
		// via the DualSense touchpad. Multiplied with volume_boost.
		int volume = 100;
		// Night mode: dynamic-range compressor for late-night watching
		// (tames loud peaks, lifts quiet dialogue). Runs in the DSP loop.
		bool night_mode = false;
		// Bitstream AC-3 to the receiver over HDMI (passthrough, v1).
		// Needs the Ex NIDs; fails closed to PCM when unavailable.
		bool dolby_passthrough = false;
	};
	enum class State { Idle, Opening, Playing, Ended, Failed };

	~Player();

	void open(const Options& opts);
	void close();

	State state() const { return state_.load(); }
	std::string error() const;

	bool paused() const { return paused_; }
	void set_paused(bool p);
	bool buffering(int* percent = nullptr) const;
	// Seconds of demuxed media queued ahead of the playhead; 0 when not
	// playing or unknown. Feeds PlayerState.buffered for the seek bar.
	double buffered_seconds() const;

	double position();  // seconds; the seek target while a seek is pending
	double duration() const { return duration_.load(); }
	void seek(double t);
	// Intro/recap/credits segments from embedded container chapters (MKV/MP4).
	// Zero-cost (parsed by FFmpeg at open), works offline. The session seeds
	// its skip list from this before the network lookup runs.
	std::vector<aniskip::SkipSegment> chapter_skip_segments() const;
	// Frame rate of the video stream (0 when unknown), from av_guess_frame_rate
	// during open(). Used by the 119.88 Hz film-mode switch: ~24fps content
	// gets exact 5:5 pulldown instead of 3:2 judder. Atomic: written by the
	// demux thread, read by the UI thread (the film-mode decision runs in
	// watch_update once fps is known).
	double fps() const { return fps_.load(); }

	std::vector<Track> audio_tracks() const;
	int audio_stream() const { return audio_stream_.load(); }
	void select_audio(int stream);

	// Text subtitles inside the file (SRT/ASS/WebVTT/mov_text).
	std::vector<Track> subtitle_tracks() const;
	std::string embedded_subtitle(int stream, double t, int* an = nullptr);

	// A picture to show: tightly packed RGBA at its display size (the CPU
	// path), or the decoded YUV planes (the GPU path, converted to RGBA on
	// the GPU by the session). Valid until the next present().
	struct Picture {
		const uint8_t* rgba = nullptr;  // CPU path; null when yuv is set
		int w = 0, h = 0;               // display size
		bool yuv = false;               // the GPU path: planes below, not rgba
		YuvPicture planes;              // into the frame's storage
		YuvColors colors = YuvColors::Bt709;
		int src_w = 0, src_h = 0;  // decoded size (for the CPU fallback)
		uint64_t gen = 0;          // incremented on each new frame or seek-hold
	};
	// UI thread, every frame: picks the frame due now. Returns it when it is a new
	// one, nullptr when the one already shown is still the right one.
	const Picture* present();

	std::string stats();
	std::string codec_summary() const;
	// Bytes downloaded since the first byte arrived, over the elapsed time:
	// the session's Auto-quality decision in the first seconds of buffering.
	// -1 until a second of data (torrents: the engine's download rate).
	double initial_throughput_bps() const;
	// The open stream's bitrate in bit/s, -1 when unknown.
	double stream_bitrate_bps() const;
	// Seconds since the last byte arrived on the connection (-1 when unknown;
	// torrents don't track it). For the session's no-data stall detection.
	double seconds_since_data() const;
	// Hint the parallel downloader to fetch the chunk at playback time sec
	// now (next-episode / chapter-skip prewarming). No-op without a network
	// stream or a known duration/size.
	void prewarm_time(double sec);
	// Throughput-based initial quality (Worker 9): the picked stream needs
	// more than ~70% of the measured link throughput — it won't sustain.
	// The session fails over to the next candidate on this (launch phase
	// only, never mid-playback; a manual pick always wins).
	static bool too_heavy_for_link(double stream_bitrate_bps, double measured_bps) {
		return stream_bitrate_bps > 0 && measured_bps > 0 && stream_bitrate_bps > 0.7 * measured_bps;
	}
	// Toggleable "stats for nerds" overlay lines: codec, decode path, fps,
	// drops, buffer, audio, bitrate. Empty when not playing.
	std::vector<std::string> nerd_stats() const;
	// Every 30 s while playing: frame rate shown, drops, buffer, decoder (to the log).
	void log_stats();
	bool too_heavy() const { return too_heavy_; }
	// The hardware decode path, for the session's start-of-playback warning.
	bool using_hw() const { return hw_active_.load(); }
	// True once the hardware decoder has given up mid-stream (software took over).
	bool hw_failed_midstream() const { return hw_failed_midstream_.load(); }
	// Active embedded subtitle stream (-1 = decode all). Set by the session
	// when the user picks a subtitle track, so anime MKVs with 25-40 ASS tracks
	// don't decode them all simultaneously.
	void select_embedded_subtitle(int stream) { active_sub_stream_.store(stream); }
	// Why the hardware decoder refused this stream ("" when it didn't). Plain
	// words from the decoder ("av1 has no hardware decoder", "decoder busy");
	// the session maps it to user-facing text.
	std::string hw_note() const;
	int video_w() const;             // coded width, 0 when unknown
	int video_h() const;             // coded height, 0 when unknown
	std::string video_codec() const; // e.g. "av1", "" when unknown
	// Threaded yuv_to_rgba over [0, rows): the row pool fast_convert uses,
	// exposed so the session's CPU fallback doesn't need its own threads.
	static void parallel_yuv_to_rgba(const YuvPicture& pic, YuvColors colors, int scale, uint8_t* out, int out_w,
	                                 int rows);
	// Playback speed. Snaps to 0.5/1/1.5/2; takes effect immediately without
	// a restart. Per-stream only (resets to 1x on open). Thread-safe.
	void set_speed(double s);
	double speed() const { return speed_.load(); }
	// Audio lip-sync offset in seconds (-0.5..+0.5; + = audio plays later).
	// Shifts the audio-driven clock; video follows. Takes effect immediately.
	// Thread-safe.
	void set_audio_delay(double d);
	double audio_delay() const { return audio_delay_.load(); }
	// Master volume 0-100. Takes effect immediately. Thread-safe.
	void set_volume(int v);
	int volume() const { return volume_.load(); }

private:
	class PacketQueue {
	public:
		~PacketQueue();
		void put(AVPacket* pkt, double pts_s, double dur_s);
		// 1: packet, 0: timed out/empty, -1: end of input reached and empty
		int get(AVPacket** pkt, int* serial, int timeout_ms);
		// Sleeps until the queue is consumed from, flushed, eof'd, or aborted
		// (all signal the queue's CV). Lets the demux thread rest instead of
		// polling when the queues are full.
		void wait_for_room(int timeout_ms);
		void flush();
		void set_eof(bool eof);
		void abort();
		size_t count() const;
		size_t bytes() const;
		double duration() const;
		int serial() const;
		void reset();
		// Wake every thread sleeping on the queue's condition variable (used
		// on seeks to jolt the demux/decoder threads out of wait_for_room).
		void notify() { cv_.notify_all(); }

	private:
		struct Item {
			AVPacket* pkt;
			double pts, dur;
		};
		mutable std::mutex m_;
		std::condition_variable cv_;
		std::deque<Item> q_;
		size_t bytes_ = 0;
		double last_pts_ = -1, dur_sum_ = 0;
		int serial_ = 0;
		bool eof_ = false, abort_ = false;
	};
	struct Frame {
		std::vector<uint8_t> pixels;  // RGBA (CPU path)
		std::vector<uint8_t> yuv;     // packed Y, U, V planes (GPU path)
		bool is_yuv = false;
		YuvPicture planes;  // into yuv above
		YuvColors colors = YuvColors::Bt709;
		int w = 0, h = 0;
		double pts = 0;
		int serial = 0;
	};

	static int interrupt_cb(void* opaque);
	void demux_thread();
	void demux_run();
	void video_thread();
	bool video_thread_hw();  // false: the hardware decoder gave up, carry on in software
	bool emit_frame(double pts, int serial, const std::function<void(Frame&)>& fill, int src_w, int src_h,
	                double sar);
	void hw_to_rgba(const HwDecoder::Picture& pic, Frame& f);
	// HDR fallback for pictures swscale would otherwise convert raw: through
	// a YUV420P10LE intermediate, then the normal tone-mapping path.
	bool hdr_fallback_convert(const AVFrame* frame, Frame& f);
	YuvPicture hw_picture(const HwDecoder::Picture& pic);
	bool fast_convert(const YuvPicture& pic, Frame& f);
	// GPU path: pack the decoded planes tightly into the frame (no colour
	// conversion; the session's shader does that). False when the picture
	// isn't YUV 4:2:0 8/10-bit (the caller converts on the CPU instead).
	bool store_yuv(const YuvPicture& pic, Frame& f);
	YuvColors colors_ = YuvColors::Bt709;  // of the video being played
	// GPU YUV->RGB path, decided once per stream (see demux_run): the
	// session allowed it AND the codec gives YUV 4:2:0 8/10-bit. When false
	// everything below behaves exactly as before this change.
	std::atomic<bool> use_gpu_convert_{false};
	int out_cap_w_ = 1920, out_cap_h_ = 1080;  // from Options; the emit box
	int max_frames_ = 8;  // frame-queue depth; 4 for >1080p sources (frame bytes scale)
	void audio_thread();
	const AVCodecParameters* video_par() const; // raw pointer into fmt_: immediate-deref only
	void fail(const std::string& msg);
	bool open_stream_codec(int stream, AVCodecContext** out);
	void handle_subtitle_packet(AVPacket* pkt);
	void update_buffering();
	double clock_locked();
	void set_clock_running(bool running);
	std::string track_label(int stream) const;

	Options opts_;
	std::atomic<State> state_{State::Idle};
	mutable std::mutex err_m_;
	std::string error_;
	std::string hw_note_;  // guarded by err_m_: why hw decoding refused this stream

	std::thread demux_, video_, audio_;
	// Non-blocking close: close() hands the demux thread to the reaper
	// instead of joining it on the calling thread. lifecycle_m_ guards the
	// reaper_ handle only (join/spawn serialization on the calling
	// threads); the teardown itself is ordered by the existing
	// per-structure locks, and the reaper thread never takes lifecycle_m_
	// (see the lock-order audit at Player::close in player.cpp).
	std::mutex lifecycle_m_;
	std::thread reaper_;
	std::atomic<bool> abort_{false};
	double open_started_ = 0;

	AVFormatContext* fmt_ = nullptr;
	mutable std::recursive_mutex fmt_m_;  // guards fmt_ lifetime: UI-thread
	                                      // accessors vs demux-thread teardown
	AVIOContext* main_pb_ = nullptr;  // our network input (NetStream)
	std::atomic<bool> quick_{false};  // opened without the index: seeking reopens
	NetStream::IoHooks io_hooks_;
	std::atomic<int> video_stream_{-1}, audio_stream_{-1};
	AVCodecContext* vctx_ = nullptr;
	std::unique_ptr<HwDecoder> hw_;      // the hardware decoder, while it decodes
	std::atomic<bool> hw_active_{false};  // for the stats line
	int hw_shift_ = -1;                   // 10-bit samples: shift to 8 bits (2 or 8, found on the first picture)
	SwsContext* hw_sws_ = nullptr;        // hardware pictures at sizes the fast path doesn't do
	SwsContext* hdr_sws_ = nullptr;       // HDR fallback: exotic pictures via a 4:2:0 10-bit intermediate
	AVFrame* hdr_tmp_ = nullptr;          // HDR fallback: reused 10-bit intermediate (was a per-frame alloc)
	int hdr_tmp_w_ = 0, hdr_tmp_h_ = 0;   // its size; reallocated only when the output size changes
	bool logged_hdr_sws_ = false;        // one dlog per stream when an HDR frame hits raw swscale
	AVCodecContext* actx_ = nullptr;
	std::mutex actx_m_;
	std::map<int, AVCodecContext*> sctx_;
	// Active embedded subtitle stream: when >4 subtitle tracks exist (common
	// in anime MKVs with 25-40 ASS tracks), only decode this one.
	// -2 = not initialized (decode all until menu built), -1 = off/external (skip all), >=0 = specific stream.
	std::atomic<int> active_sub_stream_{-2};
	std::atomic<double> duration_{0};

	PacketQueue vq_, aq_;
	std::atomic<bool> eof_{false};

	// Seeking
	std::mutex seek_m_;
	bool seek_req_ = false;
	double seek_target_ = 0;
	// Accurate seeking: each decoder discards output before its target.
	std::atomic<double> vdrop_{-1}, adrop_{-1};
	std::atomic<int> audio_switch_{-1};
	// Speed change asking the demux thread to re-decide PT vs PCM (passthrough
	// can't time-stretch): set by set_speed() on the UI thread, consumed in
	// the demux loop next to audio_switch_.
	std::atomic<bool> audio_output_recheck_{false};
	std::atomic<double> start_offset_{0};  // container start time; all times shown are relative to it
	double to_sec(int64_t ts, double tb) const;
	void do_seek(double target);
	void apply_run_state();
	// Passthrough: true when this AC-3 stream may use the bitstream port.
	// setup_audio_output() (re-)decides PT vs PCM; the caller must hold
	// actx_m_ when the audio thread is already running (track switch).
	bool decide_passthrough(int audio_stream);
	void setup_audio_output();
	double queued_audio_seconds() const;

	// Video frames
	std::mutex fq_m_;
	std::condition_variable fq_cv_;
	std::deque<Frame> frames_;
	std::vector<Frame> pool_;
	std::atomic<bool> video_drained_{false};
	Frame current_;  // the picture on show
	// Seek hold-frame (EVO's hold_buf pattern): the last on-show picture,
	// snapshotted in seek() under fq_m_ and served by present() until the
	// decoder catches up, so a seek freezes instead of tearing. hold_keep_ is
	// a one-shot: open() clears a stale hold for a new stream, but the
	// quick-reopen path in seek() needs its snapshot to survive.
	Frame hold_;
	bool hold_valid_ = false;
	bool hold_keep_ = false;
	Picture pic_;
	bool have_picture_ = false;
	uint64_t pic_gen_ = 0;  // incremented on each new frame or seek-hold
	SwsContext* sws_ = nullptr;

	// Audio
	PcmOut pcm_;
	Passthrough pt_;                      // AC-3 bitstream port (opt-in)
	std::atomic<bool> pt_active_{false};  // set once the Ex port is up
	SwrContext* swr_ = nullptr;
	float nm_env_[8] = {};  // night-mode compressor envelope, audio thread only
	std::mutex clock_m_;
	double audio_end_pts_ = -1;  // pts at the end of what's queued for output
	double last_audio_feed_ = 0;  // wall time of the last audio packet fed (starvation watchdog)
	std::atomic<bool> audio_drained_{false};
	// Playback speed (0.5/1/1.5/2). The audio thread resamples by 1/speed_
	// and advances audio_end_pts_ in media time; the wall branch of
	// clock_locked() scales elapsed real time. Atomic: UI thread writes,
	// audio/demux threads read.
	std::atomic<double> speed_{1.0};
	// Audio lip-sync offset: added to the audio-driven clock so video shifts
	// against audio. Atomic: UI thread writes, any thread reads via clock.
	std::atomic<double> audio_delay_{0.0};
	// Master volume 0-100. Atomic: UI thread writes (touchpad swipes),
	// audio thread reads in the DSP loop.
	std::atomic<int> volume_{100};
	// True when PT was left because of a non-1x speed (UI thread only): going
	// back to 1x restores passthrough.
	bool pt_before_speed_ = false;

	// Wall clock (no audio)
	double wall_base_pts_ = 0, wall_base_time_ = 0;
	bool wall_running_ = false;

	std::atomic<bool> paused_{false};
	std::atomic<bool> buffering_{true};
	std::atomic<int> buffer_percent_{0};
	int stalls_ = 0;       // index into kBufferTargets: grows each time the stream falls behind
	int stall_count_ = 0;  // for the log
	double stalls_cool_start_ = -1;  // monotonic start of the current stall-free stretch (-1: stalled/unknown)
	double start_target_ = 0;  // rebuffer floor, from the file's bitrate
	// Worker 9 (network): the fast start.
	static constexpr double kStartGate = 0.25;     // fast initial start once first keyframe is decoded
	static constexpr double kSeekGate = 0.25;      // sub-0.5s seek start gate
	static constexpr double kAudioEarlyLead = 1.5;  // audio-first: max audio lead over the video gate
	static constexpr double kEarlyGuardSecs = 10.0;  // re-buffer guard window after the gate clears
	static constexpr double kEarlyGuardBuf = 1.0;    // …trips when the buffer drains below this
	bool saw_keyframe_ = false;   // a video keyframe packet arrived since open/seek (demux thread)
	double first_key_pts_ = -1;   // its pts
	bool rebuffer_ = false;       // this buffering episode is a stall rebuffer (ladder target), not the start gate
	bool seeking_ = false;        // a user seek is in flight (sub-0.5s gate via kSeekGate), not a stall
	double play_start_ = -1;      // when the start gate last cleared (the guard window); -1 = none
	std::atomic<bool> audio_live_{false};      // the first audio packet decoded and fed
	std::atomic<bool> audio_early_ok_{false};  // audio may start before the video gate (open + seeks, not stalls)
	double audio_lead_seconds();  // audio_end_pts_ - wall_base_pts_ (takes clock_m_)

	// Subtitles inside the file
	mutable std::mutex sub_m_;
	std::map<int, std::vector<Cue>> embedded_;

	// Stats
	std::atomic<int> dropped_{0}, decoded_{0}, shown_{0};
	std::atomic<int> dropped_late_{0}, dropped_pick_{0}, present_calls_{0};
	// log_stats(): counts at the previous call
	double log_at_ = 0;
	int log_shown_ = 0, log_dropped_ = 0;
	int log_dropped_late_ = 0, log_dropped_pick_ = 0, log_present_calls_ = 0;
	std::atomic<bool> too_heavy_{false};
	std::atomic<bool> hw_failed_midstream_{false};  // hw decoder gave up mid-stream
	double heavy_window_start_ = 0;
	int heavy_window_dropped_ = 0, heavy_window_decoded_ = 0;
	std::atomic<double> fps_{0};
};
