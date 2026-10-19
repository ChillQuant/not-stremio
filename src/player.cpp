#include "player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#if defined(__SSE4_1__) || defined(__x86_64__)
#include <smmintrin.h>
// Copy from GPU Write-Combined memory using SSE4.1 streaming loads.
// On x86-64 (PS5 Zen 2), _mm_stream_load_si128 (MOVNTDQA) bursts 64-byte
// cache lines from WC memory at 10+ GB/s, vs ~76 MB/s for regular memcpy.
static void copy_from_wc(uint8_t* __restrict dst, const uint8_t* __restrict src, size_t n) {
	// MOVNTDQA requires 16-byte aligned source.
	while (n > 0 && (uintptr_t(src) & 15)) {
		*dst++ = *src++;
		n--;
	}
	const __m128i* s = reinterpret_cast<const __m128i*>(src);
	__m128i* d = reinterpret_cast<__m128i*>(dst);
	size_t vecs = n / 64;
	for (size_t i = 0; i < vecs; i++, s += 4, d += 4) {
		__m128i v0 = _mm_stream_load_si128(s + 0);
		__m128i v1 = _mm_stream_load_si128(s + 1);
		__m128i v2 = _mm_stream_load_si128(s + 2);
		__m128i v3 = _mm_stream_load_si128(s + 3);
		_mm_storeu_si128(d + 0, v0);
		_mm_storeu_si128(d + 1, v1);
		_mm_storeu_si128(d + 2, v2);
		_mm_storeu_si128(d + 3, v3);
	}
	_mm_sfence();
	size_t done = vecs * 64;
	if (n > done) memcpy(reinterpret_cast<uint8_t*>(d), reinterpret_cast<const uint8_t*>(s), n - done);
}
#else
static void copy_from_wc(uint8_t* dst, const uint8_t* src, size_t n) {
	memcpy(dst, src, n);
}
#endif

#include "http.h"
#include "netstream.h"
#include "stremio.h"
#include "torrent/engine.h"
#include "torrent/torrent_stream.h"
#include "util.h"

static const int kOutRate = 48000;
static const double kMaxQueuedAudio = 0.35;            // seconds kept queued for output
// Seconds buffered before playback starts. Each time the download falls
// behind and playback has to stop, the next wait is longer (up to 30 s), so a
// stream that's barely fast enough stops a few times for longer instead of
// every few seconds (4K over a torrent, PS5 2026-10-06).
static const double kBufferTargets[] = {2, 5, 10, 20, 30};
// Packets held in memory: 30 s of a 4K remux at ~60 Mbit/s.
static const size_t kMaxQueueBytes = 256u << 20;
static const int kMaxFrames = 8;  // ~333 ms of 24 fps video: absorbs decode jitter
                             // that used to cascade into stall -> drop -> visible jumps
                             // (and false "too heavy" warnings). Queue depth never
                             // enters the pts-vs-clock presentation mapping, so no
                             // A/V drift risk.
// (The output box was a fixed 1920x1080 here; it is now per-stream —
// out_cap_w_/out_cap_h_, from Player::Options — so the GPU path can emit at
// up to 3840x2160 on a 2160p display.)

static void delay_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Custom stereo downmix for surround sources. mat[i + stride*o] is the
// weight of input channel i in output channel o; the input order is
// FFmpeg native (5.1: FL FR FC LFE BL BR; 7.1 adds SL SR). Center at full
// level restores dialogue, surrounds at the Dolby LoRo reference, LFE
// folded in at half instead of dropped. A custom matrix also skips
// swresample's auto-normalization, which was cutting 5.1 by ~7.7 dB
// (-9.9 dB on 7.1) on top of dialnorm.
static void surround_downmix_matrix(double* m, int in_ch) {
	// Normalize by 1/sqrt(sum c_i^2) so full-scale peaks across all channels
	// can't push the mix into the limiter's saturation curve. With
	// center=1.0, surrounds=0.7071, lfe=0.5: gain = 1/sqrt(2.75) ~= 0.603.
	const double c = 0.603023;    // center: dialogue at full level
	const double s = 0.426401;    // surrounds: Dolby reference (-3 dB), normalized
	const double lfe = 0.301511;  // fold the sub in instead of dropping it
	m[0] = 0.603023;              // FL -> L
	m[1 + in_ch] = 0.603023;      // FR -> R
	m[2] = c;
	m[2 + in_ch] = c;             // FC -> L+R
	m[3] = lfe;
	m[3 + in_ch] = lfe;           // LFE -> L+R
	m[4] = s;
	m[5 + in_ch] = s;             // BL -> L, BR -> R
	if (in_ch == 8) {
		m[6] = s;
		m[7 + in_ch] = s;         // SL -> L, SR -> R
	}
}

static std::string av_err(int err) {
	char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
	av_strerror(err, buf, sizeof(buf));
	return buf;
}

// ---------------------------------------------------------------------------
// Packet queue

void Player::PacketQueue::put(AVPacket* pkt, double pts_s, double dur_s) {
	std::lock_guard<std::mutex> lock(m_);
	q_.push_back(Item{pkt, pts_s, dur_s});
	bytes_ += pkt->size;
	if (pts_s >= 0) last_pts_ = std::max(last_pts_, pts_s);
	dur_sum_ += dur_s;
	cv_.notify_one();
}

int Player::PacketQueue::get(AVPacket** pkt, int* serial, int timeout_ms) {
	std::unique_lock<std::mutex> lock(m_);
	if (q_.empty() && !eof_ && !abort_)
		cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !q_.empty() || eof_ || abort_; });
	if (abort_) return 0;
	if (q_.empty()) return eof_ ? -1 : 0;
	Item it = q_.front();
	q_.pop_front();
	*pkt = it.pkt;
	bytes_ -= it.pkt->size;
	dur_sum_ = std::max(0.0, dur_sum_ - it.dur);
	*serial = serial_;
	if (q_.empty()) {
		last_pts_ = -1;
		dur_sum_ = 0;
	}
	cv_.notify_one();  // room freed: wake a demux thread waiting on full queues
	return 1;
}

void Player::PacketQueue::wait_for_room(int timeout_ms) {
	std::unique_lock<std::mutex> lock(m_);
	cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms));
}

Player::PacketQueue::~PacketQueue() {
	std::lock_guard<std::mutex> lock(m_);
	for (auto& it : q_) av_packet_free(&it.pkt);
	q_.clear();
}

void Player::PacketQueue::flush() {
	std::lock_guard<std::mutex> lock(m_);
	for (auto& it : q_) av_packet_free(&it.pkt);
	q_.clear();
	bytes_ = 0;
	last_pts_ = -1;
	dur_sum_ = 0;
	eof_ = false;
	serial_++;
	cv_.notify_all();
}

void Player::PacketQueue::set_eof(bool eof) {
	std::lock_guard<std::mutex> lock(m_);
	eof_ = eof;
	cv_.notify_all();
}

void Player::PacketQueue::abort() {
	std::lock_guard<std::mutex> lock(m_);
	abort_ = true;
	cv_.notify_all();
}

void Player::PacketQueue::reset() {
	std::lock_guard<std::mutex> lock(m_);
	for (auto& it : q_) av_packet_free(&it.pkt);
	q_.clear();
	bytes_ = 0;
	last_pts_ = -1;
	dur_sum_ = 0;
	eof_ = false;
	abort_ = false;
	serial_++;
}

size_t Player::PacketQueue::count() const {
	std::lock_guard<std::mutex> lock(m_);
	return q_.size();
}

size_t Player::PacketQueue::bytes() const {
	std::lock_guard<std::mutex> lock(m_);
	return bytes_;
}

double Player::PacketQueue::duration() const {
	std::lock_guard<std::mutex> lock(m_);
	if (q_.empty()) return 0;
	double first = q_.front().pts;
	double spread = (first >= 0 && last_pts_ >= first) ? last_pts_ - first : 0;
	return std::max(spread, dur_sum_);
}

int Player::PacketQueue::serial() const {
	std::lock_guard<std::mutex> lock(m_);
	return serial_;
}

// ---------------------------------------------------------------------------
// Lifecycle

Player::~Player() {
	close();
	// The last teardown must finish before the members it touches die.
	std::lock_guard<std::mutex> lock(lifecycle_m_);
	if (reaper_.joinable()) reaper_.join();
}

void Player::open(const Options& opts) {
	close();
	// The previous stream's teardown runs on the reaper thread; wait for it
	// before the new stream touches shared state (demux_, fmt_, codecs, the
	// audio outputs). This sits after close() — not at the very top — on
	// purpose: close() serializes reapers (never stacked), so joining here
	// covers every prior teardown. Joining before close() would still leave
	// the just-handed-off demux racing the new one.
	{
		std::lock_guard<std::mutex> lock(lifecycle_m_);
		if (reaper_.joinable()) reaper_.join();
	}
	{
		// A new stream must not show the previous stream's hold-frame. The
		// quick-reopen path in seek() sets hold_keep_ to survive this; it is
		// consumed here so it cannot leak into a later open().
		std::lock_guard<std::mutex> lock(fq_m_);
		if (!hold_keep_) {
			hold_ = Frame();
			hold_valid_ = false;
		}
		hold_keep_ = false;
	}
	opts_ = opts;
	// Seed the runtime volume from options; UI swipes update it live.
	volume_.store(opts.volume < 0 ? 0 : opts.volume > 200 ? 200 : opts.volume);
	out_cap_w_ = opts.max_out_w > 0 ? opts.max_out_w : 1920;
	out_cap_h_ = opts.max_out_h > 0 ? opts.max_out_h : 1080;
	// Frame-queue depth is set in demux_thread once the source size is known
	// (queued GPU-path frames are source-sized); kMaxFrames is the default.
	max_frames_ = kMaxFrames;
	use_gpu_convert_ = false;  // decided in demux_run, once the codec is known
	abort_ = false;
	quick_ = false;
	eof_ = false;
	paused_ = false;
	buffering_ = true;
	buffer_percent_ = 0;
	dropped_ = decoded_ = shown_ = 0;
	log_at_ = 0;
	stalls_ = 0;
	stalls_cool_start_ = -1;
	stall_count_ = 0;
	start_target_ = 0;
	saw_keyframe_ = false;
	first_key_pts_ = -1;
	rebuffer_ = false;
	seeking_ = false;
	play_start_ = -1;
	audio_live_.store(false);
	audio_early_ok_.store(true);  // the initial buffering may start audio early
	too_heavy_ = false;
	hw_failed_midstream_ = false;
	speed_.store(1.0);  // per-stream only; the session re-applies if it wants persistence
	pt_before_speed_ = false;
	audio_output_recheck_.store(false);
	{
		std::lock_guard<std::mutex> lock(err_m_);
		hw_note_.clear();
	}
	heavy_window_start_ = now_seconds();
	heavy_window_dropped_ = heavy_window_decoded_ = 0;
	video_drained_ = audio_drained_ = false;
	have_picture_ = false;
	seek_req_ = false;
	vdrop_ = adrop_ = opts.start > 0 ? opts.start : -1;
	audio_end_pts_ = -1;
	wall_base_pts_ = opts.start;
	wall_base_time_ = now_seconds();
	wall_running_ = false;
	duration_ = 0;
	fps_ = 0;
	start_offset_ = 0;
	{
		std::lock_guard<std::mutex> lock(err_m_);
		error_.clear();
	}
	{
		std::lock_guard<std::mutex> lock(sub_m_);
		embedded_.clear();
	}
	active_sub_stream_.store(-2);
	vq_.reset();
	aq_.reset();
	state_ = State::Opening;
	open_started_ = now_seconds();
	demux_ = std::thread([this] { demux_thread(); });
}

void Player::close() {
	// Everything stops first: the demux/decoder threads notice on their
	// next wait or packet (the packet queues are aborted, the frame queue
	// is jolted, the network input's interrupt fires on abort_). The actual
	// teardown runs on the reaper thread, so this returns in well under
	// 2 ms — the old synchronous join could block on the network input and
	// on the passthrough port's ~325 ms close.
	//
	// Lock-order audit (2026-10-16): lifecycle_m_ is taken only by the
	// close/open/destructor calling threads, and only around reaper_
	// handle operations (join the old, spawn the new). The reaper thread
	// itself NEVER takes lifecycle_m_: its demux_.join() runs with no lock
	// held, so a thread blocked in reaper_.join() while holding
	// lifecycle_m_ cannot wedge — the reaper never waits on it. demux_
	// never takes lifecycle_m_ either (nothing it waits on does), so the
	// join can't block on the joiner. The teardown inside the reaper uses
	// only the pre-existing leaf/ordered locks: the packet queues' own
	// mutexes (vq_/aq_.reset), PcmOut/Passthrough internals, and fq_m_ for
	// the frame containers. fq_m_ is never held while waiting on anything
	// (the clear block takes and releases it), so it can't invert the
	// established fq_m_ -> clock_m_ order (present()'s no-audio branch) —
	// clock_m_ is not taken here at all.
	abort_ = true;
	vq_.abort();
	aq_.abort();
	fq_cv_.notify_all();
	state_ = State::Idle;  // present() stops serving frames/hold immediately
	{
		std::lock_guard<std::mutex> lock(lifecycle_m_);
		if (reaper_.joinable()) reaper_.join();  // don't stack reapers
		reaper_ = std::thread([this] {
			// The demux thread joins the decoder threads itself before it
			// returns (demux_run's tail), so one join reaps all three.
			if (demux_.joinable()) demux_.join();
			// The old close()'s synchronous tail, unchanged in content:
			// the queues are reset here (not only on the next open()),
			// because stopping playback otherwise retains up to 256 MB of
			// demuxed packets while browsing.
			vq_.reset();
			aq_.reset();
			pcm_.close();
			pt_.close();
			pt_active_ = false;
			{
				std::lock_guard<std::mutex> fq_lock(fq_m_);
				frames_.clear();
				current_ = Frame();
				pool_.clear();
			}
			// NOTE: hold_ is deliberately not cleared here. The
			// quick-reopen path in seek() snapshots before open(), and
			// open()->close() must preserve it (see hold_keep_). open()
			// clears a stale hold for genuinely new streams.
		});
	}
}

void Player::fail(const std::string& msg) {
	dlog("player: %s", msg.c_str());
	{
		std::lock_guard<std::mutex> lock(err_m_);
		error_ = msg;
	}
	{
		// A failed stream must not keep showing the seek hold-frame. Separate
		// scope from err_m_ above: no lock nesting, no order inversion.
		std::lock_guard<std::mutex> lock(fq_m_);
		hold_valid_ = false;
	}
	state_ = State::Failed;
}

std::string Player::error() const {
	std::lock_guard<std::mutex> lock(err_m_);
	return error_;
}

int Player::interrupt_cb(void* opaque) {
	auto* p = static_cast<Player*>(opaque);
	// Only when the player is closed: the network input (NetStream) gives up
	// by itself after two minutes without data. A time limit here was what
	// cut slow torrents off ("Immediate exit requested").
	return p->abort_ ? 1 : 0;
}

double Player::to_sec(int64_t ts, double tb) const {
	if (ts == AV_NOPTS_VALUE) return -1;
	return double(ts) * tb - start_offset_.load();
}

bool Player::open_stream_codec(int stream, AVCodecContext** out) {
	AVStream* st = fmt_->streams[stream];
	const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec) {
		dlog("player: no decoder for %s", avcodec_get_name(st->codecpar->codec_id));
		return false;
	}
	AVCodecContext* ctx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(ctx, st->codecpar);
	ctx->pkt_timebase = st->time_base;
	if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
		// The PS5 has 8 Zen 2 cores; FFmpeg can't count them here, so say it.
		// Frame threading only: slice threading oversubscribes when combined
		// with frame threading in HEVC/AV1 software decoders.
		ctx->thread_count = 6;
		ctx->thread_type = FF_THREAD_FRAME;
	}
	int r = avcodec_open2(ctx, codec, nullptr);
	if (r < 0) {
		dlog("player: can't open %s decoder: %s", codec->name, av_err(r).c_str());
		avcodec_free_context(&ctx);
		return false;
	}
	*out = ctx;
	return true;
}

static bool is_text_subtitle(const AVCodecParameters* par) {
	if (par->codec_type != AVMEDIA_TYPE_SUBTITLE) return false;
	const AVCodecDescriptor* d = avcodec_descriptor_get(par->codec_id);
	return d && (d->props & AV_CODEC_PROP_TEXT_SUB);
}

// ---------------------------------------------------------------------------
// Demuxer

void Player::demux_thread() {
	demux_run();
	// The input is closed by now; the network stream behind it goes last.
	// Detached under fmt_m_ (serializes with UI-thread main_pb_ readers in
	// initial_throughput_bps/seconds_since_data), then the close is handed
	// to a detached thread so joining demux_ never waits on the curl-worker
	// teardown (0.5-2.5 s) behind close_avio. The detached thread owns pb
	// exclusively from here on; demux_thread touches nothing after.
	AVIOContext* pb = nullptr;
	{
		std::lock_guard<std::recursive_mutex> lock(fmt_m_);
		pb = main_pb_;
		main_pb_ = nullptr;
	}
	if (TorrentStream::is_ours(pb)) {
		std::thread([pb]() mutable { TorrentStream::close_avio(&pb); }).detach();
	} else if (pb) {
		std::thread([pb]() mutable { NetStream::close_avio(&pb); }).detach();
	}
}

void Player::demux_run() {
	AVDictionary* dict = nullptr;
	av_dict_set(&dict, "user_agent", kUserAgent, 0);
	if (!opts_.headers.empty()) av_dict_set(&dict, "headers", (join(opts_.headers, "\r\n") + "\r\n").c_str(), 0);
	av_dict_set(&dict, "reconnect", "1", 0);
	av_dict_set(&dict, "reconnect_streamed", "1", 0);
	av_dict_set(&dict, "reconnect_on_network_error", "1", 0);
	av_dict_set(&dict, "reconnect_delay_max", "5", 0);
	// A torrent's first bytes can take a while (the server is still finding
	// peers); Circle still cancels at once through the interrupt callback.
	av_dict_set(&dict, "rw_timeout", "120000000", 0);
	av_dict_set(&dict, "seekable", "-1", 0);
	if (!http_ca_bundle().empty()) av_dict_set(&dict, "ca_file", http_ca_bundle().c_str(), 0);

	fmt_ = avformat_alloc_context();
	fmt_->interrupt_callback.callback = interrupt_cb;
	fmt_->interrupt_callback.opaque = this;
	fmt_->probesize = 5 << 20;
	fmt_->max_analyze_duration = 5 * AV_TIME_BASE;

	// HTTP(S) goes through libcurl (see netstream.h): the stream itself here,
	// and whatever the demuxer opens later (HLS) through the hooks.
	io_hooks_.headers = opts_.headers;
	io_hooks_.abort = &abort_;
	NetStream::install(fmt_, &io_hooks_);
	dlog("player: opening %s", log_url(opts_.url).c_str());
	bool torrent = TorrentStream::is_url(opts_.url);
	if (torrent || starts_with(opts_.url, "http://") || starts_with(opts_.url, "https://")) {
		std::string err;
		// A torrent from the built-in engine, or HTTP(S) through libcurl.
		main_pb_ = torrent ? TorrentStream::open_avio(opts_.url, &abort_, &err)
		                   : NetStream::open_avio(opts_.url, opts_.headers, &abort_, &err, opts_.parallel);
		if (!main_pb_) {
			{
				std::lock_guard<std::recursive_mutex> lock(fmt_m_);
				avformat_free_context(fmt_);
				fmt_ = nullptr;
			}
			av_dict_free(&dict);
			if (!abort_) fail("Could not open the stream (" + err + ")");
			return;
		}
		fmt_->pb = main_pb_;
		fmt_->flags |= AVFMT_FLAG_CUSTOM_IO;
		// Matroska keeps tags, attachments and its index at the end of the
		// file, and FFmpeg reads them while opening. For a torrent that's
		// the part the server has least of, so it could take minutes before
		// the first frame. Start from the beginning instead, without them;
		// the first seek opens the file again with the index (see seek()).
		std::string magic = torrent ? TorrentStream::of(main_pb_)->peek(4) : NetStream::of(main_pb_)->peek(4);
		if (torrent && opts_.start <= 0 && magic == "\x1A\x45\xDF\xA3") {
			main_pb_->seekable = 0;
			quick_ = true;
			// Matroska describes its tracks in the header, so a short look
			// is enough; with few seeders every MB here is a long wait.
			fmt_->probesize = 1 << 20;
			fmt_->max_analyze_duration = AV_TIME_BASE / 2;
			dlog("player: Matroska, starting without reading the end of the file");
		}
		else {
			// Step 3 (sub-0.5s seek): tight probes for the fast start —
			// 2 MB for torrents (slow first bytes), 512 KB for HTTP(S).
			fmt_->probesize = torrent ? (2 << 20) : (512 << 10);
			fmt_->max_analyze_duration = torrent ? AV_TIME_BASE : (AV_TIME_BASE / 2);
		}
	}
	int r = avformat_open_input(&fmt_, opts_.url.c_str(), nullptr, &dict);
	av_dict_free(&dict);
	if (r < 0) {
		{
			std::lock_guard<std::recursive_mutex> lock(fmt_m_);
			fmt_ = nullptr;  // freed by avformat_open_input on failure
		}
		if (!abort_) fail("Could not open the stream (" + av_err(r) + ")");
		return;
	}
	r = avformat_find_stream_info(fmt_, nullptr);
	if (r < 0 && !abort_) dlog("player: find_stream_info: %s", av_err(r).c_str());
	if (abort_) {
		std::lock_guard<std::recursive_mutex> lock(fmt_m_);
		avformat_close_input(&fmt_);
		return;
	}
	if (fmt_->start_time != AV_NOPTS_VALUE) start_offset_ = double(fmt_->start_time) / AV_TIME_BASE;
	if (fmt_->duration != AV_NOPTS_VALUE && fmt_->duration > 0) duration_ = double(fmt_->duration) / AV_TIME_BASE;

	int vs = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	int as = -1;
	// Preferred audio language first, else FFmpeg's pick.
	for (auto& want : opts_.audio_langs) {
		for (unsigned i = 0; i < fmt_->nb_streams && as < 0; i++) {
			AVStream* st = fmt_->streams[i];
			if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
			AVDictionaryEntry* e = av_dict_get(st->metadata, "language", nullptr, 0);
			if (e && language_to_iso639_2(e->value) == want) as = int(i);
		}
		if (as >= 0) break;
	}
	if (as < 0) as = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, vs, nullptr, 0);
	if (vs < 0 && as < 0) {
		{
			std::lock_guard<std::recursive_mutex> lock(fmt_m_);
			avformat_close_input(&fmt_);
		}
		fail("No video or audio in this stream");
		return;
	}

	// Colours, from the stream's tags: HDR10 (PQ) and HLG get tone mapped; SD
	// pictures use BT.601, everything else BT.709. HLG is the ARIB_STD_B67
	// transfer, or BT.2020 primaries carrying an HLG stream whose transfer
	// tag is missing.
	if (vs >= 0) {
		const AVCodecParameters* p = fmt_->streams[vs]->codecpar;
		bool sd_space = p->color_space == AVCOL_SPC_BT470BG || p->color_space == AVCOL_SPC_SMPTE170M;
		bool hlg = p->color_trc == AVCOL_TRC_ARIB_STD_B67 || p->color_primaries == AVCOL_PRI_BT2020;
		// Dolby Vision Profile 5: no HDR10 base layer; the HEVC pixels are
		// reshaped IPT-PQ-c2, not YCbCr, so playing them as plain YCbCr shows
		// purple/green. Detected from the DOVI configuration record (public
		// FFmpeg API: the profile lives in the container's codec side data, no
		// per-frame RPU parsing needed for detection). Takes precedence over
		// the transfer tags below. v1 reshapes with the reference matrices
		// (identity curves); per-frame RPU curves are a follow-up.
		bool dovi_p5 = false;
		{
			const AVPacketSideData* sd = av_packet_side_data_get(
				p->coded_side_data, p->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
			if (sd && sd->size >= int(sizeof(AVDOVIDecoderConfigurationRecord))) {
				const auto* c = reinterpret_cast<const AVDOVIDecoderConfigurationRecord*>(sd->data);
				dovi_p5 = c->dv_profile == 5;
			}
		}
		colors_ = dovi_p5                                        ? YuvColors::DoviP5
		          : p->color_trc == AVCOL_TRC_SMPTE2084          ? YuvColors::Hdr10
		          : hlg                                             ? YuvColors::Hlg
		          : (sd_space || (p->color_space == AVCOL_SPC_UNSPECIFIED && p->height < 720)) ? YuvColors::Bt601
		                                                                                       : YuvColors::Bt709;
		dlog("player: colours %s (transfer %d, matrix %d, primaries %d%s)",
		     colors_ == YuvColors::DoviP5 ? "Dolby Vision P5, reshaped to HDR10"
		     : colors_ == YuvColors::Hdr10 ? "HDR10, tone mapped"
		     : colors_ == YuvColors::Hlg ? "HLG, tone mapped"
		     : colors_ == YuvColors::Bt601 ? "BT.601" : "BT.709",
		     int(p->color_trc), int(p->color_space), int(p->color_primaries),
		     dovi_p5 ? ", DV P5 detected" : "");
		logged_hdr_sws_ = false;  // new stream, new colours: re-arm the raw-swscale warning
	}
	// Video: the hardware decoder when it takes the stream; FFmpeg's software
	// decoder otherwise (and if the hardware one fails later).
	if (vs >= 0) {
		std::string why;
		hw_.reset(HwDecoder::open(fmt_->streams[vs]->codecpar, &why));
		hw_active_ = hw_ != nullptr;
		hw_shift_ = -1;
		{
			// Kept for the session's start-of-playback warning (read on the UI thread).
			std::lock_guard<std::mutex> lock(err_m_);
			hw_note_ = hw_ ? std::string() : why;
		}
		if (!hw_) dlog("player: software video decoding (%s)", why.c_str());
	}
	if (vs >= 0 && !hw_ && !open_stream_codec(vs, &vctx_)) {
		AVStream* st = fmt_->streams[vs];
		std::string name = avcodec_get_name(st->codecpar->codec_id);
		{
			std::lock_guard<std::recursive_mutex> lock(fmt_m_);
			avformat_close_input(&fmt_);
		}
		fail("This video's format (" + name + ") can't be played. Pick another stream.");
		return;
	}
	// The GPU YUV->RGB path, decided once per stream: the session's shaders
	// compiled, and the pictures are YUV 4:2:0 8/10-bit (what the shader
	// handles). The hardware decoder always gives NV12/P010. Anything else
	// keeps the exact old CPU behaviour. Set before the video thread starts.
	use_gpu_convert_ = opts_.allow_gpu_convert &&
	                   (hw_ || (vctx_ && (vctx_->pix_fmt == AV_PIX_FMT_YUV420P ||
	                                      vctx_->pix_fmt == AV_PIX_FMT_YUV420P10LE)));
	if (use_gpu_convert_) dlog("player: GPU YUV conversion");
	if (as >= 0 && !open_stream_codec(as, &actx_)) as = -1;
	for (unsigned i = 0; i < fmt_->nb_streams; i++) {
		AVStream* st = fmt_->streams[i];
		bool keep = int(i) == vs || int(i) == as;
		if (is_text_subtitle(st->codecpar)) {
			AVCodecContext* sc = nullptr;
			if (open_stream_codec(int(i), &sc)) {
				sctx_[int(i)] = sc;
				keep = true;
			}
		}
		// Other audio tracks stay readable so they can be switched to.
		if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) keep = true;
		st->discard = keep ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
	}
	video_stream_ = vs;
	audio_stream_ = as;
	if (vs < 0) vdrop_ = -1;
	if (vs >= 0) {
		// Frame-queue depth scales with SOURCE frame bytes (queued GPU-path
		// frames are source-sized): a 4K P010 YUV frame is ~25 MB, so 8
		// queued was ~200 MB. 4 frames still covers ~167 ms of 24 fps video.
		// (No free-memory gate yet: needs platform API plumbing.)
		const AVCodecParameters* vpar = fmt_->streams[vs]->codecpar;
		max_frames_ = (vpar->width > 1920 || vpar->height > 1080) ? 4 : kMaxFrames;
		AVRational fr = av_guess_frame_rate(fmt_, fmt_->streams[vs], nullptr);
		if (fr.num > 0 && fr.den > 0) fps_ = av_q2d(fr);
	}

	if (as >= 0) setup_audio_output();

	dlog("player: %s, %.0fs, video #%d %s, audio #%d %s", fmt_->iformat->name, duration_.load(), vs,
	     vs >= 0 ? avcodec_get_name(fmt_->streams[vs]->codecpar->codec_id) : "-", as,
	     as >= 0 ? avcodec_get_name(fmt_->streams[as]->codecpar->codec_id) : "-");

	// How much to re-buffer after a stall (the floor under the kBufferTargets
	// ladder): a high-bitrate file (a 4K remux) needs more, or playback
	// restarts before the download is up to speed and runs dry within the
	// first minute (PS5 2026-10-06: Avengers: Endgame 4K, ~25 Mbit/s, stopped
	// once at 45 s with 2 s buffered). The initial start gate is separate
	// and fixed (kStartGate): fast start, with the early-drain guard below
	// catching the slow link instead of a deep upfront wait.
	{
		int64_t size = fmt_->pb ? avio_size(fmt_->pb) : -1;
		double mbps = fmt_->bit_rate > 0 ? fmt_->bit_rate / 1e6
		              : (size > 0 && duration_.load() > 0) ? size * 8.0 / duration_.load() / 1e6
		                                            : 0;
		start_target_ = mbps >= 40 ? 15 : mbps >= 20 ? 10 : mbps >= 10 ? 5 : 2;
		dlog("player: about %.0f Mbit/s, start gate %.1f s (rebuffer floor %.0f s)", mbps, kStartGate,
		     start_target_);
	}
	if (opts_.start > 0) do_seek(opts_.start);
	state_ = State::Playing;
	apply_run_state();

	if (vs >= 0) video_ = std::thread([this] { video_thread(); });
	// Always start the audio thread when there's an audio stream: if the
	// port failed to open, the thread drains (decodes and discards) instead
	// of queueing, so the demux never blocks filling aq_ with no consumer.
	if (as >= 0) audio_ = std::thread([this] { audio_thread(); });

	AVPacket* pkt = av_packet_alloc();
	int errors = 0;
	while (!abort_) {
		double target = -1;
		{
			std::lock_guard<std::mutex> lock(seek_m_);
			if (seek_req_) {
				target = seek_target_;
				seek_req_ = false;
			}
		}
		int sw = audio_switch_.exchange(-1);
		if (sw >= 0 && sw != audio_stream_) {
			AVCodecContext* nc = nullptr;
			if (open_stream_codec(sw, &nc)) {
				{
					std::lock_guard<std::mutex> lock(actx_m_);
					avcodec_free_context(&actx_);
					actx_ = nc;
					audio_stream_ = sw;
					aq_.flush();  // drop packets from the old track (bumps the serial)
				}
				// Re-decide the output WITHOUT actx_m_ held: passthrough
				// close/open sleeps ~2s for the receiver to re-lock, and
				// holding the lock across it starves the audio thread and
				// stalls demux long enough to drain the video queue (a
				// full ~2s freeze on every PT track switch). Track switches
				// are serialized on this thread, so no other
				// setup_audio_output can interleave. (audit 2026-10-13)
				setup_audio_output();  // re-decide passthrough for the new track
				// Without the index (quick_) it just carries on from here.
				if (target < 0 && !quick_) target = position();
			}
		}
		// Speed change asking for a PT<->PCM re-decide (passthrough can't
		// time-stretch). Same constraints as the track switch above: demux
		// thread only, no actx_m_ held.
		if (audio_output_recheck_.exchange(false)) {
			{
				std::lock_guard<std::mutex> lock(actx_m_);
				aq_.flush();
			}
			setup_audio_output();
		}
		if (target >= 0) do_seek(target);

		update_buffering();

		if (vq_.bytes() + aq_.bytes() > kMaxQueueBytes ||
		    ((vs < 0 || vq_.duration() > 60) && (audio_stream_ < 0 || aq_.duration() > 60)) || eof_) {
			// Full, or at the end: rest until the decoders consume (they
			// signal on every packet taken), a seek flushes, or we abort —
			// instead of spinning the 10 ms busy-poll.
			vq_.wait_for_room(100);
			aq_.wait_for_room(100);
			continue;
		}

		r = av_read_frame(fmt_, pkt);
		if (r < 0) {
			if (abort_) break;
			// The download gave up (not the end of the video): say so,
			// rather than ending playback as if the video were over.
			std::string why;
			if (NetStream* ns = main_pb_ ? NetStream::of(main_pb_) : nullptr) why = ns->failure();
			if (TorrentStream* ts = main_pb_ ? TorrentStream::of(main_pb_) : nullptr) why = ts->failure();
			if (!why.empty()) {
				fail("The stream stopped (" + why + ")");
				break;
			}
			bool at_end = r == AVERROR_EOF || (fmt_->pb && avio_feof(fmt_->pb));
			if (!at_end && ++errors < 60) {
				delay_ms(100);
				continue;
			}
			if (!at_end) {
				// 60 consecutive read failures that are NOT end-of-file: the
				// stream died (dead link, corrupt data). Fail honestly instead
				// of pretending the video ended — State::Ended would mark it
				// watched and autoplay the next episode.
				fail("The stream stopped responding (" + std::string(av_err(r).c_str()) + ")");
				break;
			}
			eof_ = true;
			vq_.set_eof(true);
			aq_.set_eof(true);
			continue;
		}
		errors = 0;
		AVStream* st = fmt_->streams[pkt->stream_index];
		double tb = av_q2d(st->time_base);
		double pts = to_sec(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts, tb);
		double dur = pkt->duration > 0 ? pkt->duration * tb : 0;
		if (pkt->stream_index == vs) {
			AVPacket* q = av_packet_alloc();
			av_packet_move_ref(q, pkt);
			// Worker 9: remember the first keyframe since open/seek — the
			// start gate requires a decoded frame from it (never a delta).
			if ((q->flags & AV_PKT_FLAG_KEY) && !saw_keyframe_) {
				saw_keyframe_ = true;
				first_key_pts_ = pts;
				// Step 3 (sub-0.5s seek): when a seek is in flight, snap the
				// drop points and the wall clock to this first keyframe —
				// the seek target lands on a keyframe boundary, so waiting
				// for exact drop-point packets only delays the start.
				// Window widened to 15s behind / 2s ahead: 4K Web-DLs carry
				// 6s-12s GOPs, so the 4.5s window forced the decoder to decode
				// and discard hundreds of frames first. Trade-off: the seek
				// may now land up to 15s early — speed over frame-accuracy,
				// by design.
				double vd = vdrop_.load();
				if (seeking_ && vd >= 0 && pts >= 0 && (vd - pts) <= 15.0 && (pts - vd) <= 2.0) {
					vdrop_ = pts;
					adrop_ = pts;
					std::lock_guard<std::mutex> clk_lock(clock_m_);
					wall_base_pts_ = pts;
					wall_base_time_ = now_seconds();
				}
			}
			vq_.put(q, pts, dur);
		} else if (pkt->stream_index == audio_stream_) {
			AVPacket* q = av_packet_alloc();
			av_packet_move_ref(q, pkt);
			aq_.put(q, pts, dur);
		} else {
			if (sctx_.count(pkt->stream_index)) handle_subtitle_packet(pkt);
			av_packet_unref(pkt);
		}
	}
	av_packet_free(&pkt);

	vq_.abort();
	aq_.abort();
	fq_cv_.notify_all();
	if (video_.joinable()) video_.join();
	if (audio_.joinable()) audio_.join();

	avcodec_free_context(&vctx_);
	hw_.reset();
	hw_active_ = false;
	if (hw_sws_) {
		sws_freeContext(hw_sws_);
		hw_sws_ = nullptr;
	}
	if (hdr_sws_) {
		sws_freeContext(hdr_sws_);
		hdr_sws_ = nullptr;
	}
	if (hdr_tmp_) {
		av_freep(&hdr_tmp_->data[0]);
		av_frame_free(&hdr_tmp_);
		hdr_tmp_w_ = hdr_tmp_h_ = 0;
	}
	{
		std::lock_guard<std::mutex> lock(actx_m_);
		avcodec_free_context(&actx_);
	}
	for (auto& kv : sctx_) avcodec_free_context(&kv.second);
	sctx_.clear();
	if (sws_) {
		sws_freeContext(sws_);
		sws_ = nullptr;
	}
	if (swr_) swr_free(&swr_);
	{
		std::lock_guard<std::recursive_mutex> lock(fmt_m_);
		avformat_close_input(&fmt_);
	}
	video_stream_ = audio_stream_ = -1;
}

void Player::do_seek(double target) {
	seeking_ = true;  // step 3 (sub-0.5s seek): a fresh seek clears the start gate when the first keyframe lands
	// Arm the new drop horizon and flush stale queued packets BEFORE the
	// demuxer seek, so the decode threads stop on pre-seek packets while
	// the seek is in flight.
	vdrop_ = video_stream_ >= 0 ? target : -1;
	adrop_ = target;
	vq_.flush();
	aq_.flush();
	{
		// Recycle stale pre-seek decoded frames: a full frames_ (max_frames_ =
		// 4) would otherwise block video_thread in emit_frame() while the
		// decoder re-fills from the new position. fq_m_ alone here — never
		// nested with clock_m_ — so the fq_m_ -> clock_m_ order can't invert.
		std::lock_guard<std::mutex> lock(fq_m_);
		while (!frames_.empty()) {
			pool_.push_back(std::move(frames_.front()));
			frames_.pop_front();
		}
	}
	fq_cv_.notify_all();
	int64_t ts = int64_t((target + start_offset_.load()) * AV_TIME_BASE);
	int r = avformat_seek_file(fmt_, -1, INT64_MIN, ts, ts, 0);
	if (r < 0) r = av_seek_frame(fmt_, -1, ts, AVSEEK_FLAG_BACKWARD);
	if (r < 0) dlog("player: seek to %.1f failed: %s", target, av_err(r).c_str());
	eof_ = false;
	video_drained_ = audio_drained_ = false;
	{
		std::lock_guard<std::mutex> lock(clock_m_);
		audio_end_pts_ = -1;
		wall_base_pts_ = target;
		wall_base_time_ = now_seconds();
		pcm_.clear();
		if (pt_active_) pt_.reset();  // drop packer state and queued grains
	}
	buffering_ = true;
	buffer_percent_ = 0;
	saw_keyframe_ = false;
	first_key_pts_ = -1;
	rebuffer_ = false;
	play_start_ = -1;
	audio_live_.store(false);
	audio_early_ok_.store(true);  // a seek may start audio before the video gate
	apply_run_state();
}

void Player::handle_subtitle_packet(AVPacket* pkt) {
	// When many embedded subtitle tracks exist, only decode the active one.
	// -2 = not initialized yet (decode all), -1 = off/external (skip all),
	// >=0 = decode only the active stream.
	const int active = active_sub_stream_.load();
	if (sctx_.size() > 4 && active != -2 && pkt->stream_index != active)
		return;
	AVCodecContext* ctx = sctx_[pkt->stream_index];
	AVSubtitle sub;
	int got = 0;
	if (avcodec_decode_subtitle2(ctx, &sub, &got, pkt) < 0 || !got) return;
	double tb = av_q2d(fmt_->streams[pkt->stream_index]->time_base);
	double pts = to_sec(pkt->pts, tb);
	if (pts < 0) {
		avsubtitle_free(&sub);
		return;
	}
	double start = pts + sub.start_display_time / 1000.0;
	double end = sub.end_display_time > sub.start_display_time ? pts + sub.end_display_time / 1000.0
	             : pkt->duration > 0                         ? pts + pkt->duration * tb
	                                                         : start + 4;
	std::string text;
	for (unsigned i = 0; i < sub.num_rects; i++) {
		AVSubtitleRect* rect = sub.rects[i];
		std::string t;
		if (rect->ass) {
			// "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text"
			const char* p = rect->ass;
			for (int commas = 0; *p && commas < 8; p++)
				if (*p == ',') commas++;
			t = p;
		} else if (rect->text) {
			t = rect->text;
		}
		if (t.empty()) continue;
		if (!text.empty()) text += "\\N";
		text += t;
	}
	avsubtitle_free(&sub);
	if (text.empty()) return;
	Cue c;
	c.start = start;
	c.end = end;
	c.rml = subtitle_text_to_rml(text, &c.an);
	std::lock_guard<std::mutex> lock(sub_m_);
	auto& cues = embedded_[pkt->stream_index];
	auto it = std::lower_bound(cues.begin(), cues.end(), c.start, [](const Cue& a, double v) { return a.start < v; });
	for (auto j = it; j != cues.end() && j->start == c.start; ++j)
		if (j->rml == c.rml) return;  // seen before (seeking back re-reads packets)
	cues.insert(it, c);
}

// ---------------------------------------------------------------------------
// Video

void Player::video_thread() {
	if (hw_ && video_thread_hw()) return;
	AVPacket* pkt = nullptr;
	AVFrame* frame = av_frame_alloc();
	int serial = vq_.serial();
	bool sent_eof = false;
	double tb = av_q2d(fmt_->streams[video_stream_]->time_base);

	while (!abort_) {
		int s = 0;
		int r = vq_.get(&pkt, &s, 50);
		if (abort_) break;
		if (r == 0) continue;
		if (r == 1 && s != serial) {
			avcodec_flush_buffers(vctx_);
			serial = s;
			sent_eof = false;
			video_drained_ = false;
		}
		if (r == -1) {
			if (sent_eof) {
				delay_ms(10);
				if (vq_.serial() != serial) {
					avcodec_flush_buffers(vctx_);
					serial = vq_.serial();
					sent_eof = false;
					video_drained_ = false;
				}
				continue;
			}
			avcodec_send_packet(vctx_, nullptr);  // drain
			sent_eof = true;
		} else {
			int sr = avcodec_send_packet(vctx_, pkt);
			if (sr < 0 && sr != AVERROR(EAGAIN)) dlog("player: video send: %s", av_err(sr).c_str());
			av_packet_free(&pkt);
		}

		while (!abort_) {
			int rr = avcodec_receive_frame(vctx_, frame);
			if (rr == AVERROR_EOF) {
				video_drained_ = true;
				break;
			}
			if (rr < 0) break;
			decoded_++;
			double pts = to_sec(frame->best_effort_timestamp, tb);
			double sar = frame->sample_aspect_ratio.num > 0 ? av_q2d(frame->sample_aspect_ratio) : 1.0;
			bool ok = emit_frame(pts, serial, [&](Frame& f) {
				// Untagged HDR: the container didn't say, but the frame does.
				// Upgrade the stream colours once; everything downstream
				// (fast_convert's capture, f.colors) reads colors_ after this
				// point. Runs on the video thread, like all other readers.
				// BT.2020 primaries with no transfer tag get the same HLG
				// guess as the container-level check above: 2020 is never
				// SDR, and leaving them BT.709 oversaturates (wrong matrix
				// on 2020 data, PQ treated as gamma).
				if (colors_ != YuvColors::Hdr10 && colors_ != YuvColors::Hlg && colors_ != YuvColors::DoviP5) {
					if (frame->color_trc == AVCOL_TRC_SMPTE2084) {
						colors_ = YuvColors::Hdr10;
						dlog("player: frame transfer is PQ, treating stream as HDR10");
					} else if (frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
						colors_ = YuvColors::Hlg;
						dlog("player: frame transfer is HLG, treating stream as HLG");
					} else if (frame->color_primaries == AVCOL_PRI_BT2020) {
						colors_ = YuvColors::Hlg;
						dlog("player: frame primaries are BT.2020, treating stream as HLG");
					}
					if (colors_ == YuvColors::Hdr10 || colors_ == YuvColors::Hlg)
						logged_hdr_sws_ = false;
				}
				// 4:2:0 HDR (and plain 4:2:0) through the same conversion as
				// hardware pictures; anything else through swscale.
				if (frame->format == AV_PIX_FMT_YUV420P10LE || frame->format == AV_PIX_FMT_YUV420P) {
					YuvPicture yp;
					yp.y = frame->data[0];
					yp.u = frame->data[1];
					yp.v = frame->data[2];
					yp.y_stride = frame->linesize[0];
					yp.c_stride = frame->linesize[1];
					yp.wide = frame->format == AV_PIX_FMT_YUV420P10LE;
					yp.width = frame->width;
					yp.height = frame->height;
					if (use_gpu_convert_) {
						// The shader converts (and scales); no CPU work.
						if (store_yuv(yp, f)) return;
						f.is_yuv = false;  // degenerate layout: CPU for this frame
					} else if (frame->linesize[1] == frame->linesize[2] && fast_convert(yp, f)) return;
				} else if (use_gpu_convert_) {
					// A format the shader doesn't handle on this picture:
					// this one frame goes through the CPU path instead.
					f.is_yuv = false;
				}
				f.pixels.resize(size_t(f.w) * f.h * 4);
				// Any HDR frame swscale would otherwise convert to RGBA raw
				// goes through a 4:2:0 10-bit intermediate at the output size
				// and then the normal tone-mapping path instead: raw swscale
				// ignores the transfer curve (and converts with the wrong
				// matrix), so HDR10/HLG/DV pixels come out with wrong colours.
				// This also covers 4:2:0 frames fast_convert skipped (odd
				// linesizes or an output size it doesn't do).
				if ((colors_ == YuvColors::Hdr10 || colors_ == YuvColors::Hlg || colors_ == YuvColors::DoviP5) &&
				    hdr_fallback_convert(frame, f))
					return;
				if (!logged_hdr_sws_ &&
				    (colors_ == YuvColors::Hdr10 || colors_ == YuvColors::Hlg || colors_ == YuvColors::DoviP5)) {
					logged_hdr_sws_ = true;
					dlog("player: HDR frame fell through to raw swscale; transfer ignored, colours will be wrong");
				}
				sws_ = sws_getCachedContext(sws_, frame->width, frame->height, AVPixelFormat(frame->format), f.w, f.h,
				                            AV_PIX_FMT_RGBA,
				                            (f.w == frame->width && f.h == frame->height) ? SWS_POINT : SWS_BILINEAR,
				                            nullptr, nullptr, nullptr);
				if (!sws_) return;
				uint8_t* dst[4] = {f.pixels.data(), nullptr, nullptr, nullptr};
				int dst_stride[4] = {f.w * 4, 0, 0, 0};
				sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, dst, dst_stride);
			}, frame->width, frame->height, sar);
			av_frame_unref(frame);
			if (!ok) break;
		}
	}
	if (pkt) av_packet_free(&pkt);
	av_frame_free(&frame);
}

// A decoded picture into the frame queue, scaled to the screen by `fill`.
// Skips pictures before a seek target or hopelessly late ones. False when
// playback was closed or seeked meanwhile.
bool Player::emit_frame(double pts, int serial, const std::function<void(Frame&)>& fill, int src_w, int src_h,
                        double sar) {
	double drop = vdrop_;
	if (drop >= 0) {
		if (pts >= 0 && pts < drop - 0.02) return true;
		vdrop_ = -1;
	}
	// Hopelessly late and something else is ready: skip the conversion.
	// (Never hold clock_m_ while taking fq_m_: present() nests them the other way.)
	{
		double clk;
		{
			std::lock_guard<std::mutex> cl(clock_m_);
			clk = clock_locked();
		}
		std::lock_guard<std::mutex> fl(fq_m_);
		if (!buffering_ && !frames_.empty() && pts >= 0 && pts < clk - 0.2) {
			dropped_++;
			dropped_late_++;
			return true;
		}
	}

	// Wait for room in the frame queue.
	std::unique_lock<std::mutex> lock(fq_m_);
	fq_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] {
		return abort_ || int(frames_.size()) < max_frames_ || vq_.serial() != serial;
	});
	while (!abort_ && int(frames_.size()) >= max_frames_ && vq_.serial() == serial)
		fq_cv_.wait_for(lock, std::chrono::milliseconds(50));
	if (abort_ || vq_.serial() != serial) return false;
	Frame f;
	if (!pool_.empty()) {
		f = std::move(pool_.back());
		pool_.pop_back();
	}
	// Reset the GPU-path marker: a recycled frame must never carry a stale
	// is_yuv with stale/null planes. store_yuv() is the only legitimate
	// setter of is_yuv=true.
	f.is_yuv = false;
	lock.unlock();

	// Scale to fit the output box (the 1080p box, or the 4K box when the GPU
	// path runs on a 2160p display), keeping the display aspect ratio.
	double dar = src_w * sar / src_h;
	int dw = out_cap_w_, dh = int(out_cap_w_ / dar);
	if (dh > out_cap_h_) {
		dh = out_cap_h_;
		dw = int(out_cap_h_ * dar);
	}
	f.w = dw & ~1;
	f.h = dh & ~1;
	// The GPU path stores the decoded planes (no RGBA buffer); the CPU path
	// converts into f.pixels exactly as before.
	if (!use_gpu_convert_) f.pixels.resize(size_t(f.w) * f.h * 4);
	fill(f);
	f.pts = pts;
	f.serial = serial;

	lock.lock();
	frames_.push_back(std::move(f));
	return true;
}

// ---------------------------------------------------------------------------
// Hardware decoding

namespace {

// A few threads that convert one picture together: the conversion of a 4K
// picture is too much for one core at 60 frames a second.
class RowPool {
public:
	static RowPool& get() {
		static RowPool* p = new RowPool();  // never destroyed: threads outlive statics at exit
		return *p;
	}
	// fn(first_row, end_row) over [0, rows), split between the threads.
	// run() itself is serialized: the video thread and the render thread can
	// both convert now, and overlapping run() calls would corrupt the shared
	// fn_/pending_ state.
	void run(int rows, const std::function<void(int, int)>& fn) {
		std::lock_guard<std::mutex> gate(gate_);
		const int parts = kThreads + 1;
		std::unique_lock<std::mutex> lock(m_);
		fn_ = &fn;
		rows_ = rows;
		parts_ = parts;
		pending_ = kThreads;
		gen_++;
		cv_.notify_all();
		lock.unlock();
		fn(0, rows / parts);  // this thread does the first part
		lock.lock();
		done_cv_.wait(lock, [&] { return pending_ == 0; });
		fn_ = nullptr;
	}

private:
	static const int kThreads = 3;
	RowPool() {
		for (int i = 0; i < kThreads; i++) std::thread([this, i] { loop(i + 1); }).detach();
	}
	void loop(int part) {
		int seen = 0;
		for (;;) {
			std::unique_lock<std::mutex> lock(m_);
			cv_.wait(lock, [&] { return gen_ != seen; });
			seen = gen_;
			const std::function<void(int, int)>* fn = fn_;
			int rows = rows_, parts = parts_;
			lock.unlock();
			int a = rows * part / parts, b = rows * (part + 1) / parts;
			if (fn && a < b) (*fn)(a, b);
			lock.lock();
			if (--pending_ == 0) done_cv_.notify_all();
		}
	}
	std::mutex m_;
	std::condition_variable cv_, done_cv_;
	const std::function<void(int, int)>* fn_ = nullptr;
	int rows_ = 0, parts_ = 1, pending_ = 0, gen_ = 0;
	std::mutex gate_;  // serializes run() calls across threads (see above).
	                     // Non-recursive: never call run() from inside a pool fn.
};

}  // namespace

// A picture to the RGBA frame with the right colours (yuv_convert.h), at the
// same size or exactly half (4K to the 1080p screen), on four threads.
// False for other sizes (the caller uses swscale).
bool Player::fast_convert(const YuvPicture& pic, Frame& f) {
	auto near = [](int a, int b) { return a >= b - 1 && a <= b + 1; };
	int scale = (f.w == pic.width && f.h == pic.height) ? 1
	            : (near(f.w * 2, pic.width) && near(f.h * 2, pic.height)) ? 2
	                                                                      : 0;
	if (!scale) return false;
	YuvColors colors = colors_;
	RowPool::get().run(
	    f.h, [&](int a, int b) { yuv_to_rgba(pic, colors, scale, f.pixels.data(), f.w, a, b); });
	return true;
}

// The row pool for callers outside the video thread (the session's CPU
// fallback): same threading as fast_convert, no private thread management.
void Player::parallel_yuv_to_rgba(const YuvPicture& pic, YuvColors colors, int scale, uint8_t* out, int out_w,
                                  int rows) {
	RowPool::get().run(rows, [&](int a, int b) { yuv_to_rgba(pic, colors, scale, out, out_w, a, b); });
}

// The GPU path: pack the decoded planes tightly into the frame. No colour
// conversion happens here at all; the session uploads the planes as textures
// and its fragment shader converts (and scales to f.w/f.h). False for
// degenerate input, so the caller can convert on the CPU instead.
bool Player::store_yuv(const YuvPicture& pic, Frame& f) {
	if (!pic.y || !pic.u || pic.width <= 0 || pic.height <= 0) return false;
	if (!pic.interleaved && !pic.v) return false;
	const int bs = pic.wide ? 2 : 1;  // bytes per sample
	const int cw = pic.width / 2, ch = pic.height / 2;
	const size_t y_size = size_t(pic.width) * pic.height * bs;
	const size_t c_row = size_t(cw) * bs * (pic.interleaved ? 2 : 1);
	const size_t need = y_size + c_row * ch * (pic.interleaved ? 1 : 2);
	// resize() zero-fills only on growth; pooled frames keep their buffer.
	if (f.yuv.size() < need)
		f.yuv.resize(need);
	uint8_t* dst = f.yuv.data();
	// NOTE: the frame must own this copy. The decoder reuses its plane
	// memory (HW pictures are valid only until the next send/receive/flush;
	// SW frames are unref'd right after emit) while the frame sits queued,
	// so referencing the decoder's planes directly is not safe. When the
	// source rows are already tight, one memcpy per plane beats the row loop.
	// NOTE: pic.y/pic.u point into GPU Write-Combined memory (slot->frame_mem).
	// Use copy_from_wc() (SSE4.1 MOVNTDQA streaming loads) instead of memcpy:
	// regular memcpy reads WC memory at ~76 MB/s (163ms per 4K frame!),
	// while streaming loads hit 10+ GB/s (~1ms per 4K frame).
	if (pic.y_stride == pic.width * bs)
		copy_from_wc(dst, pic.y, y_size);
	else
		for (int y = 0; y < pic.height; y++)
			copy_from_wc(dst + size_t(y) * pic.width * bs, pic.y + size_t(y) * pic.y_stride, size_t(pic.width) * bs);
	dst += y_size;
	if (size_t(pic.c_stride) == c_row) {
		copy_from_wc(dst, pic.u, c_row * ch);
		if (!pic.interleaved)
			copy_from_wc(dst + c_row * ch, pic.v, c_row * ch);
	}
	else {
		for (int y = 0; y < ch; y++)
			copy_from_wc(dst + size_t(y) * c_row, pic.u + size_t(y) * pic.c_stride, c_row);
		if (!pic.interleaved) {
			dst += c_row * ch;
			for (int y = 0; y < ch; y++)
				copy_from_wc(dst + size_t(y) * c_row, pic.v + size_t(y) * pic.c_stride, c_row);
		}
	}
	f.planes.y = f.yuv.data();
	f.planes.u = f.yuv.data() + y_size;
	f.planes.v = pic.interleaved ? nullptr : f.yuv.data() + y_size + c_row * ch;
	f.planes.y_stride = pic.width * bs;
	f.planes.c_stride = int(c_row);
	f.planes.interleaved = pic.interleaved;
	f.planes.wide = pic.wide;
	f.planes.shift = pic.shift;
	f.planes.width = pic.width;
	f.planes.height = pic.height;
	f.colors = colors_;
	f.is_yuv = true;
	return true;
}

// A hardware picture as a YuvPicture, shared by the CPU and GPU paths.
YuvPicture Player::hw_picture(const HwDecoder::Picture& pic) {
	if (pic.ten_bit && hw_shift_ < 0) {
		// 10-bit samples sit either in the low or the high bits of each
		// 16-bit word; look at a few to tell.
		int max = 0;
		for (int y = 0; y < pic.height; y += 97) {
			auto* row = reinterpret_cast<const uint16_t*>(pic.y + size_t(y) * pic.pitch);
			for (int x = 0; x < pic.width; x += 31) max = std::max<int>(max, row[x]);
		}
		hw_shift_ = max > 1023 ? 6 : 0;
		dlog("player: 10-bit pictures, samples in the %s bits", hw_shift_ ? "high" : "low");
	}
	YuvPicture yp;
	yp.y = pic.y;
	yp.u = pic.uv;
	yp.y_stride = yp.c_stride = pic.pitch;
	yp.interleaved = true;
	yp.wide = pic.ten_bit;
	yp.shift = pic.ten_bit ? hw_shift_ : 0;
	yp.width = pic.width;
	yp.height = pic.height;
	return yp;
}

// A hardware picture: two planes (Y, then interleaved UV), 8 or 16-bit samples.
void Player::hw_to_rgba(const HwDecoder::Picture& pic, Frame& f) {
	YuvPicture yp = hw_picture(pic);
	if (fast_convert(yp, f)) return;
	// HDR at a size the fast path doesn't do: swscale can't tone-map, so go
	// through a 4:2:0 10-bit intermediate, then the normal HDR conversion.
	// (The HW decoder never reports per-frame transfer, so this covers
	// streams the container tagged correctly.)
	if (colors_ == YuvColors::Hdr10 || colors_ == YuvColors::Hlg || colors_ == YuvColors::DoviP5) {
		AVFrame* wrap = av_frame_alloc();
		if (wrap) {
			wrap->format = pic.ten_bit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
			wrap->width = pic.width;
			wrap->height = pic.height;
			wrap->data[0] = const_cast<uint8_t*>(pic.y);
			wrap->data[1] = const_cast<uint8_t*>(pic.uv);
			wrap->linesize[0] = wrap->linesize[1] = pic.pitch;
			bool done = hdr_fallback_convert(wrap, f);
			av_frame_free(&wrap);  // frees the wrapper only; plane data is the decoder's
			if (done) return;
		}
		if (!logged_hdr_sws_) {
			logged_hdr_sws_ = true;
			dlog("player: HDR hw picture fell through to raw swscale; transfer ignored, colours will be wrong");
		}
		// Fall through to raw swscale on failure (old behavior).
	}
	// Other sizes: swscale from NV12 / P010.
	AVPixelFormat fmt = pic.ten_bit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
	hw_sws_ = sws_getCachedContext(hw_sws_, pic.width, pic.height, fmt, f.w, f.h, AV_PIX_FMT_RGBA, SWS_BILINEAR,
	                               nullptr, nullptr, nullptr);
	if (!hw_sws_) return;
	const uint8_t* src[4] = {pic.y, pic.uv, nullptr, nullptr};
	int src_stride[4] = {pic.pitch, pic.pitch, 0, 0};
	uint8_t* dst[4] = {f.pixels.data(), nullptr, nullptr, nullptr};
	int dst_stride[4] = {f.w * 4, 0, 0, 0};
	sws_scale(hw_sws_, src, src_stride, 0, pic.height, dst, dst_stride);
}

// HDR fallback for pictures swscale would otherwise convert to RGBA raw,
// ignoring the transfer curve (washed out). Converts through a YUV420P10LE
// intermediate at the output size, then the normal tone-mapping path.
// True when it handled the frame.
bool Player::hdr_fallback_convert(const AVFrame* frame, Frame& f) {
	if (f.w <= 0 || f.h <= 0) return false;
	hdr_sws_ = sws_getCachedContext(hdr_sws_, frame->width, frame->height, AVPixelFormat(frame->format), f.w,
	                                f.h, AV_PIX_FMT_YUV420P10LE, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!hdr_sws_) return false;
	// The intermediate frame and its buffer are reused across frames and
	// reallocated only when the output size changes (was: av_frame_alloc +
	// av_image_alloc + av_freep per frame, ~16 MB of churn for 4K).
	if (!hdr_tmp_) {
		hdr_tmp_ = av_frame_alloc();
		if (!hdr_tmp_) return false;
	}
	if (hdr_tmp_w_ != f.w || hdr_tmp_h_ != f.h) {
		av_freep(&hdr_tmp_->data[0]);
		if (av_image_alloc(hdr_tmp_->data, hdr_tmp_->linesize, f.w, f.h, AV_PIX_FMT_YUV420P10LE, 32) < 0) {
			hdr_tmp_w_ = hdr_tmp_h_ = 0;
			return false;
		}
		hdr_tmp_w_ = f.w;
		hdr_tmp_h_ = f.h;
	}
	AVFrame* tmp = hdr_tmp_;
	if (sws_scale(hdr_sws_, frame->data, frame->linesize, 0, frame->height, tmp->data, tmp->linesize) != f.h)
		return false;
	// DV Profile 5 is full-range by spec, but the swscale intermediate
	// above is studio-range: expand to 0..1023 in place so put_dovi
	// (which normalizes by /1023) sees the codes it expects. Without
	// this, black (64) reads as 0.0626 and white (940) as 0.919.
	if (colors_ == YuvColors::DoviP5) {
		for (int y = 0; y < f.h; ++y) {
			uint16_t* row = reinterpret_cast<uint16_t*>(tmp->data[0] + size_t(y) * tmp->linesize[0]);
			for (int x = 0; x < f.w; ++x) {
				const int v = (int(row[x]) - 64) * 1023 + 438;
				row[x] = uint16_t(std::max(0, std::min(1023, v / 876)));
			}
		}
		for (int p = 1; p <= 2; ++p)
			for (int y = 0; y < (f.h + 1) / 2; ++y) {
				uint16_t* row =
				    reinterpret_cast<uint16_t*>(tmp->data[p] + size_t(y) * tmp->linesize[p]);
				for (int x = 0; x < (f.w + 1) / 2; ++x) {
					const int v = (int(row[x]) - 64) * 1023 + 448;
					row[x] = uint16_t(std::max(0, std::min(1023, v / 896)));
				}
			}
	}
	YuvPicture yp;
	yp.y = tmp->data[0];
	yp.u = tmp->data[1];
	yp.v = tmp->data[2];
	yp.y_stride = tmp->linesize[0];
	yp.c_stride = tmp->linesize[1];
	yp.wide = true;
	// swscale's 10-bit planar alignment isn't contractual; detect it
	// the same way hw_picture() does (samples > 1023 => high bits).
	int mx = 0;
	for (int y = 0; y < f.h; y += 16) {
		const uint16_t* row =
		    reinterpret_cast<const uint16_t*>(tmp->data[0] + size_t(y) * tmp->linesize[0]);
		for (int x = 0; x < f.w; x += 16) mx = std::max(mx, int(row[x]));
	}
	yp.shift = mx > 1023 ? 6 : 0;
	yp.width = f.w;
	yp.height = f.h;
	if (f.pixels.size() < size_t(f.w) * f.h * 4) f.pixels.resize(size_t(f.w) * f.h * 4);
	// The tone-mapping rows go through the shared pool, like fast_convert
	// (was: single-threaded over all rows on the video thread).
	YuvColors colors = colors_;
	RowPool::get().run(f.h, [&](int a, int b) { yuv_to_rgba(yp, colors, 1, f.pixels.data(), f.w, a, b); });
	return true;
}

// The video thread while the hardware decoder plays. Returns true when
// playback ended or closed; false when the decoder failed and the software
// decoder should take over from here.
bool Player::video_thread_hw() {
	AVPacket* pkt = nullptr;
	int serial = vq_.serial();
	bool sent_eof = false;
	AVStream* st = fmt_->streams[video_stream_];
	double tb = av_q2d(st->time_base);
	double sar = st->codecpar->sample_aspect_ratio.num > 0 ? av_q2d(st->codecpar->sample_aspect_ratio) : 1.0;
	bool failed = false;

	auto drain = [&]() -> bool {  // decoded pictures out; false if playback moved on
		HwDecoder::Picture pic;
		for (;;) {
			int rr = hw_->receive(&pic);
			if (rr < 0) {
				failed = true;
				return true;
			}
			if (rr == 0) return true;
			decoded_++;
			double pts = pic.pts_us == INT64_MIN ? -1 : double(pic.pts_us) / 1e6;
			if (!emit_frame(pts, serial, [&](Frame& f) {
				    if (use_gpu_convert_) {
					    // The shader converts (and scales); the 10-bit
					    // shift detection still runs (the shader needs it).
					    if (store_yuv(hw_picture(pic), f)) return;
					    f.is_yuv = false;
					    f.pixels.resize(size_t(f.w) * f.h * 4);
				    }
				    hw_to_rgba(pic, f);
			    }, pic.width, pic.height, sar))
				return false;
		}
	};

	while (!abort_ && !failed) {
		int s = 0;
		int r = vq_.get(&pkt, &s, 50);
		if (abort_) break;
		if (r == 0) continue;
		if (r == 1 && s != serial) {
			hw_->flush();
			serial = s;
			sent_eof = false;
			video_drained_ = false;
		}
		if (r == -1) {
			if (sent_eof) {
				delay_ms(10);
				if (vq_.serial() != serial) {
					hw_->flush();
					serial = vq_.serial();
					sent_eof = false;
					video_drained_ = false;
				}
				continue;
			}
			hw_->send(nullptr, 0, 0);
			sent_eof = true;
			drain();
			video_drained_ = true;
			continue;
		}
		double ps = -1;
		int64_t pts_us = INT64_MIN;
		{
			int64_t raw_ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
			if (raw_ts != AV_NOPTS_VALUE) {
				ps = to_sec(raw_ts, tb);
				pts_us = int64_t(ps * 1e6);
			}
		}
		for (;;) {
			int sr = hw_->send(pkt->data, pkt->size, pts_us);
			if (sr == 1) {  // pictures waiting: take them first
				if (!drain()) break;
				continue;
			}
			if (sr < 0) failed = true;
			break;
		}
		av_packet_free(&pkt);
		if (!failed) drain();
	}
	if (pkt) av_packet_free(&pkt);
	if (!failed) return true;

	// The hardware decoder gave up on this stream: software from here on
	// (it picks up at the next keyframe).
	dlog("player: the hardware decoder failed; switching to software decoding");
	hw_.reset();
	hw_active_ = false;
	hw_failed_midstream_ = true;  // the session warns about 4K software decode
	if (!open_stream_codec(video_stream_, &vctx_)) {
		fail("This video can't be decoded. Pick another stream.");
		return true;
	}
	// The software codec may give a format the shader doesn't handle; the
	// per-frame check in the video thread still guards each picture.
	use_gpu_convert_ = opts_.allow_gpu_convert && vctx_ &&
	                   (vctx_->pix_fmt == AV_PIX_FMT_YUV420P || vctx_->pix_fmt == AV_PIX_FMT_YUV420P10LE);
	// Resync: the software decoder can only resume at the next keyframe, so
	// the picture on screen may lag the (still running) audio clock by up to
	// a GOP. If it lags by more than ~0.5 s, arm the video drop counter at
	// the audio clock: emit_frame() then skips the stale pictures without
	// converting them until the decoder catches up, instead of queueing them
	// behind the frozen frame. Smaller lags keep the old behavior (the 0.2 s
	// hopelessly-late drop in emit_frame already covers those), and arming
	// unconditionally would risk skipping a merely-slightly-late keyframe and
	// waiting another whole GOP. Audio is deliberately untouched: it never
	// stopped, so moving adrop_ would only open an audible gap.
	{
		double video_pts = -1;
		{
			// The two locks are taken in separate scopes and never nested,
			// so there is no lock-order hazard here.
			std::lock_guard<std::mutex> fl(fq_m_);
			if (!frames_.empty()) video_pts = frames_.back().pts;
		}
		double now;
		{
			std::lock_guard<std::mutex> cl(clock_m_);
			now = clock_locked();
		}
		if (video_pts >= 0 && now - video_pts > 0.5) {
			dlog("player: video %.1f s behind audio after hw->sw fallback; dropping to catch up",
			     now - video_pts);
			vdrop_ = now;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Audio

// AC-3 passthrough (v1): bitstream Dolby Digital to the receiver instead of
// decoding it. Only AC-3 at 48 kHz is attempted; everything else, and every
// failure, falls back to today's PCM path. Never leaves the user silent.
bool Player::decide_passthrough(int as) {
	if (!opts_.dolby_passthrough || as < 0 || !fmt_) return false;
	if (speed_.load() != 1.0) {
		dlog("player: %.1fx speed: passthrough can't time-stretch, using PCM", speed_.load());
		return false;
	}
	AVCodecParameters* par = fmt_->streams[as]->codecpar;
	Passthrough::PtCodec ptc = Passthrough::PtCodec::None;
	switch (par->codec_id) {
		case AV_CODEC_ID_AC3: ptc = Passthrough::PtCodec::Ac3; break;
		case AV_CODEC_ID_EAC3: ptc = Passthrough::PtCodec::Eac3; break;
		case AV_CODEC_ID_DTS: ptc = Passthrough::PtCodec::Dts; break;
		case AV_CODEC_ID_AAC: ptc = Passthrough::PtCodec::Aac; break;
		default: return false;
	}
	// E-AC-3 carries its own sample rate in the bitstream; the 192 kHz
	// carrier handles 32/44.1/48 kHz content. The rest need 48 kHz.
	if (ptc != Passthrough::PtCodec::Eac3 && par->sample_rate != 48000) {
		dlog("player: %s at %d Hz, not 48 kHz: PCM it is",
		     iec61937::codec_name(Passthrough::to_iec(ptc)), par->sample_rate);
		return false;
	}
	if (!pt_.resolve()) {
		dlog("player: passthrough NIDs not on this firmware, using PCM");
		return false;
	}
	// Raw AAC needs the AudioSpecificConfig to synthesize ADTS headers.
	if (ptc == Passthrough::PtCodec::Aac && par->extradata && par->extradata_size > 0)
		pt_.set_aac_extradata(par->extradata, size_t(par->extradata_size));
	// No EDID probe: sceAudioOutSysGetHdmiMonitorInfo's struct layout is
	// unverifiable from public sources, so calling it risks corruption.
	// The opt-in setting is the consent gate; ExConfigureOutput's return
	// code is the safety check.
	if (!pt_.open(ptc)) {
		dlog("player: %s passthrough open failed, falling back to PCM",
		     iec61937::codec_name(Passthrough::to_iec(ptc)));
		return false;
	}
	return true;
}

// (Re-)decide the audio output for audio_stream_. Called on the demux
// thread only (initial stream setup before the audio thread starts, and
// track switches while it runs); close() joins demux first, so calls never
// overlap. Must NOT be called with actx_m_ held: the passthrough close/open
// sleeps for the receiver and would stall the pipeline (see the
// track-switch site).
void Player::setup_audio_output() {
	if (pt_active_) {
		pt_active_ = false;
		pt_.close();
	}
	if (pcm_.is_open()) pcm_.close();
	if (audio_stream_ >= 0 && decide_passthrough(audio_stream_)) {
		pt_active_ = true;
		dlog("player: Dolby Digital passthrough active");
		if (opts_.night_mode)
			dlog("player: night mode has no effect on bitstream passthrough");
	} else if (audio_stream_ >= 0) {
		const int ch = opts_.audio_output == 1 ? 8 : 2;
		if (!pcm_.open(ch))
			dlog("player: can't open the audio output");
		else
			dlog("player: audio output %dch", pcm_.channels());
	}
	apply_run_state();  // sync pause state to whichever output is live
}

double Player::queued_audio_seconds() const {
	if (pt_active_) return pt_.queued_seconds();
	if (!pcm_.is_open()) return 0;
	return double(pcm_.queued_bytes()) / double(kOutRate * pcm_.channels() * 2);
}

// Worker 9: how far the audio clock has run past the start (takes clock_m_;
// the audio thread calls it with actx_m_ released, so no lock-order hazard).
double Player::audio_lead_seconds() {
	std::lock_guard<std::mutex> lock(clock_m_);
	if (audio_end_pts_ < 0) return 0;
	return audio_end_pts_ - wall_base_pts_;
}

// Worker 9: throughput-based initial quality. Bytes downloaded since the
// first byte arrived, over the elapsed time — zero extra traffic. -1 until
// a second of data (torrents: the engine's download rate).
double Player::initial_throughput_bps() const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);  // serializes main_pb_ with demux_thread's close
	if (NetStream* ns = main_pb_ ? NetStream::of(main_pb_) : nullptr) return ns->measured_bps();
	if (main_pb_ && TorrentStream::is_ours(main_pb_) && TorrentStream::is_url(opts_.url)) {
		std::string hash;
		int idx = 0;
		if (TorrentStream::parse_url(opts_.url, &hash, &idx))
			return bt::Engine::get().stats(hash).download_rate;
	}
	return -1;
}

double Player::stream_bitrate_bps() const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);
	if (!fmt_ || fmt_->bit_rate <= 0) return -1;
	return double(fmt_->bit_rate);
}

double Player::seconds_since_data() const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);  // serializes main_pb_ with demux_thread's close
	if (NetStream* ns = main_pb_ ? NetStream::of(main_pb_) : nullptr) return ns->seconds_since_data();
	return -1;  // torrents: the engine doesn't track last-byte time per reader
}

// Hint the parallel downloader at a playback-time position (next-episode /
// chapter-skip prewarming). Goes through main_pb_ like the other NetStream
// accessors: a no-op for torrents (NetStream::of returns null) and when the
// duration or size is unknown.
void Player::prewarm_time(double sec) {
	double dur = duration_.load();
	if (dur <= 0 || sec < 0) return;
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);  // serializes main_pb_ with demux_thread's close
	NetStream* ns = main_pb_ ? NetStream::of(main_pb_) : nullptr;
	if (!ns) return;
	int64_t sz = ns->size();
	if (sz <= 0) return;
	ns->prewarm_offset(int64_t(sec / dur * double(sz)));
}

void Player::audio_thread() {
	AVPacket* pkt = nullptr;
	AVFrame* frame = av_frame_alloc();
	int serial = aq_.serial();
	bool sent_eof = false;
	std::vector<uint8_t> out;
	int swr_rate = 0, swr_fmt = -1, swr_out_ch = 0;
	double swr_speed = 1.0;  // the speed the resampler was built for
	bool swr_custom = false;  // the active resampler uses the custom downmix matrix
	AVChannelLayout swr_layout;
	memset(&swr_layout, 0, sizeof(swr_layout));
	// Output channels from the actually-opened port: PcmOut falls back to
	// stereo inside open() if the console refuses multichannel, so this is
	// what the resampler must target. Fixed for the life of this thread.
	const int out_ch = pcm_.channels();
	const int out_frame_bytes = out_ch * 2;  // s16 interleaved

	while (!abort_) {
		int s = 0;
		int r = aq_.get(&pkt, &s, 50);
		if (abort_) break;
		if (r == 0) continue;
		std::unique_lock<std::mutex> alock(actx_m_);
		if (!actx_) {
			if (r == 1) av_packet_free(&pkt);
			continue;
		}
		if (r == 1 && s != serial) {
			avcodec_flush_buffers(actx_);
			serial = s;
			sent_eof = false;
			audio_drained_ = false;
			if (swr_) swr_free(&swr_);
			if (pt_active_) pt_.reset();  // seek/switch: drop packer state (port stays up)
			for (int i = 0; i < 8; ++i) nm_env_[i] = 0;  // night-mode envelope reset
		}
		if (pt_active_) {
			// Dolby passthrough: the raw AC-3 packet goes to the IEC 61937
			// packer; decode -> resample -> PCM are skipped entirely.
			// Locking: pt_'s mutex is always taken *under* actx_m_ (never
			// the reverse). setup_audio_output() no longer takes actx_m_
			// at all (it runs on the demux thread only), so no ABBA is
			// possible in either direction. (audit 2026-10-13)
			if (r == 1) {
				double tb = av_q2d(fmt_->streams[audio_stream_]->time_base);
				double pts = to_sec(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts, tb);
				int samples = pt_.push_packet(pkt->data, pkt->size);  // fast; alock held
				av_packet_free(&pkt);
				audio_live_.store(true);  // Worker 9: the first audio packet decoded
				// Don't hold actx_m_ while waiting for queue room.
				alock.unlock();
				while (!abort_ && aq_.serial() == serial && pt_active_ &&
				       pt_.queued_seconds() > kMaxQueuedAudio)
					delay_ms(5);
				// Audio-first start: while the video gate still holds, don't
				// let the audio clock run more than kAudioEarlyLead past the
				// start, or present() would skip the opening video when the
				// gate clears.
				while (!abort_ && aq_.serial() == serial && pt_active_ && audio_early_ok_.load() &&
				       buffering_.load() && audio_lead_seconds() > kAudioEarlyLead)
					delay_ms(20);
				alock.lock();
				if (abort_ || aq_.serial() != serial || !pt_active_) continue;
				// The audio clock advances from packet PTS + burst sample
				// counts at the 48 kHz carrier rate (1536 samples/frame),
				// mirroring the PCM path's n/kOutRate update below.
				std::lock_guard<std::mutex> clock_lock(clock_m_);
				if (aq_.serial() != serial) continue;
				double end = (pts >= 0 ? pts : std::max(0.0, audio_end_pts_)) +
				             double(samples) / 48000.0;
				if (pts < 0 && audio_end_pts_ >= 0) end = audio_end_pts_ + double(samples) / 48000.0;
				audio_end_pts_ = end;
				last_audio_feed_ = now_seconds();
			} else if (r == -1 && !sent_eof) {
				pt_.flush();  // no-op for fixed-size AC-3 frames
				sent_eof = true;
				audio_drained_ = true;
			}
			continue;
		}
		if (r == -1) {
			if (sent_eof) {
				alock.unlock();
				delay_ms(10);
				continue;
			}
			avcodec_send_packet(actx_, nullptr);
			sent_eof = true;
		} else {
			avcodec_send_packet(actx_, pkt);
			av_packet_free(&pkt);
		}
		double tb = av_q2d(fmt_->streams[audio_stream_]->time_base);

		while (!abort_) {
			int rr = avcodec_receive_frame(actx_, frame);
			if (rr == AVERROR_EOF) {
				audio_drained_ = true;
				break;
			}
			if (rr < 0) break;
			double pts = to_sec(frame->best_effort_timestamp, tb);
			double drop = adrop_;
			if (drop >= 0) {
				if (pts >= 0 && pts + double(frame->nb_samples) / frame->sample_rate < drop) {
					av_frame_unref(frame);
					continue;
				}
				adrop_ = -1;
			}

			AVChannelLayout in_layout;
			if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || frame->ch_layout.nb_channels == 0)
				av_channel_layout_default(&in_layout, frame->ch_layout.nb_channels ? frame->ch_layout.nb_channels : 2);
			else
				av_channel_layout_copy(&in_layout, &frame->ch_layout);
			if (!swr_ || swr_rate != frame->sample_rate || swr_fmt != frame->format || swr_out_ch != out_ch ||
			    swr_speed != speed_.load() ||
			    av_channel_layout_compare(&swr_layout, &in_layout) != 0) {
				if (swr_) swr_free(&swr_);
				// Match the resampler's output layout to the port that is
				// actually open: 7.1 keeps every surround channel discrete
				// (no downmix at all); stereo folds down with the custom
				// matrix below. FFmpeg's 7.1 order matches the PS5's
				// interleave order, so no remapping is needed.
				AVChannelLayout out_layout;
				if (out_ch == 8)
					av_channel_layout_from_mask(&out_layout, AV_CH_LAYOUT_7POINT1);
				else
					av_channel_layout_default(&out_layout, 2);
				// Speed: play s× faster by decimating (pitch rises, like a
				// tape). Telling swr the input runs at rate×speed makes it
				// emit 1/speed as many samples; the clock below advances in
				// media time so A/V stays in sync. No tempo-preserving
				// stretcher is plumbed (would need an atempo filter graph).
				const double spd = speed_.load();
				swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_S16, kOutRate, &in_layout,
				                    AVSampleFormat(frame->format),
				                    int(frame->sample_rate * spd), 0, nullptr);
				av_channel_layout_uninit(&out_layout);
				// Custom stereo downmix for standard 5.1/7.1 sources (must sit
				// between alloc and init). Any other layout keeps FFmpeg's
				// default behavior. Gated on the exact masks rather than
				// just the channel count, so an exotic 6/8ch layout can
				// never have channels misassigned.
				const int in_ch = in_layout.nb_channels;
				const AVChannelLayout std51 = AV_CHANNEL_LAYOUT_5POINT1;
				const AVChannelLayout std71 = AV_CHANNEL_LAYOUT_7POINT1;
				bool custom = out_ch == 2 &&
					(av_channel_layout_compare(&in_layout, &std51) == 0 ||
					 av_channel_layout_compare(&in_layout, &std71) == 0);
				if (swr_ && custom) {
					double mat[16] = {0};  // stride in_ch (6 or 8) x 2 outputs
					surround_downmix_matrix(mat, in_ch);
					// A failed matrix set must not claim the custom state:
					// fall back to the default matrix (and no limiter).
					if (swr_set_matrix(swr_, mat, in_ch) < 0)
						custom = false;
				}
				if (!swr_ || swr_init(swr_) < 0) {
					dlog("player: can't resample audio");
					if (swr_) swr_free(&swr_);
					av_channel_layout_uninit(&in_layout);
					av_frame_unref(frame);
					continue;
				}
				swr_rate = frame->sample_rate;
				swr_fmt = frame->format;
				swr_out_ch = out_ch;
				swr_speed = spd;
				swr_custom = custom;
				av_channel_layout_uninit(&swr_layout);
				av_channel_layout_copy(&swr_layout, &in_layout);
			}
			av_channel_layout_uninit(&in_layout);

			int max_out = swr_get_out_samples(swr_, frame->nb_samples) + 32;
			out.resize(size_t(max_out) * size_t(out_frame_bytes));
			uint8_t* outp[1] = {out.data()};
			int n = swr_convert(swr_, outp, max_out, (const uint8_t**)frame->extended_data, frame->nb_samples);
			av_frame_unref(frame);
			if (n <= 0) continue;

			// Volume boost + night mode + soft limiter. Skipped entirely for
			// plain stereo at 100% with night mode off: the bit-transparent
			// path is preserved.
			const int vol = volume_.load(std::memory_order_relaxed);
			if (opts_.volume_boost != 100 || vol != 100 || swr_custom || opts_.night_mode) {
				int16_t* s = reinterpret_cast<int16_t*>(out.data());
				const int total = n * out_ch;
				const int gain = opts_.volume_boost * vol / 100;
				const bool nm = opts_.night_mode;
				for (int i = 0; i < total; ++i) {
					int32_t v = int32_t(s[i]) * gain / 100;
					if (nm) {
						// Night mode: per-channel peak-envelope compressor,
						// 4:1 above -18 dBFS with +6 dB makeup so quiet
						// dialogue lifts. The soft-clip below stays as the
						// final safety stage.
						const int ch = i % out_ch;  // out_ch <= 8 (nm_env_)
						float peak = float(v < 0 ? -v : v);
						float env = nm_env_[ch];
						// Attack ~5 ms, release ~200 ms at 48 kHz.
						env += (peak - env) * (peak > env ? 0.00416f : 0.000104f);
						nm_env_[ch] = env;
						float g = 2.0f;  // +6 dB makeup
						if (env > 4128.0f)  // -18 dBFS
							g = 2.0f * (4128.0f + (env - 4128.0f) * 0.25f) / env;
						v = int32_t(float(v) * g);
					}
					int32_t a = v < 0 ? -v : v;
					if (a > 23197) {  // -3 dBFS soft knee
						const double x = double(a - 23197) / double(32767 - 23197);
						a = 23197 + int32_t(double(32767 - 23197) * std::tanh(x));
						v = v < 0 ? -a : a;
					}
					if (v > 32767)
						v = 32767;
					else if (v < -32768)
						v = -32768;
					s[i] = int16_t(v);
				}
			}

			// Keep the output queue short so the clock (and seeking) stay responsive.
			alock.unlock();
			while (!abort_ && aq_.serial() == serial && queued_audio_seconds() > kMaxQueuedAudio) delay_ms(5);
			// Audio-first start: while the video gate still holds, don't let
			// the audio clock run more than kAudioEarlyLead past the start,
			// or present() would skip the opening video when the gate clears.
			while (!abort_ && aq_.serial() == serial && audio_early_ok_.load() && buffering_.load() &&
			       audio_lead_seconds() > kAudioEarlyLead)
				delay_ms(20);
			alock.lock();
			if (abort_ || aq_.serial() != serial || !actx_) break;

			std::lock_guard<std::mutex> lock(clock_m_);
			if (aq_.serial() != serial) break;
			// Port failed to open: drain (decode and discard) so the demux
			// never blocks on a full aq_; the ring would grow unboundedly
			// with no consumer otherwise.
			if (pcm_.is_open()) {
				pcm_.queue(out.data(), size_t(n) * size_t(out_frame_bytes));
				audio_live_.store(true);  // Worker 9: the first audio packet decoded and fed
			}
			// n output samples are  n/kOutRate real seconds but n/kOutRate*speed
			// media seconds: the clock runs at speed_.
			const double spd = speed_.load();
			double end = (pts >= 0 ? pts : std::max(0.0, audio_end_pts_)) + double(n) / kOutRate * spd;
			if (pts < 0 && audio_end_pts_ >= 0) end = audio_end_pts_ + double(n) / kOutRate * spd;
			audio_end_pts_ = end;
			last_audio_feed_ = now_seconds();
		}
	}
	av_channel_layout_uninit(&swr_layout);
	if (pkt) av_packet_free(&pkt);
	av_frame_free(&frame);
}

// ---------------------------------------------------------------------------
// Clock, buffering, presentation

double Player::clock_locked() {
	if (audio_stream_ >= 0 && (pcm_.is_open() || pt_active_)) {
		if (audio_end_pts_ < 0) return wall_base_pts_;
		double q = queued_audio_seconds();
		// Audio starvation watchdog: if the queue is nearly empty and no
		// audio packet has been fed for over a second, the audio path has
		// stalled. Fall back to the wall clock so video keeps moving
		// instead of freezing on a frozen audio_end_pts_.
		if (q < 0.01 && !audio_drained_ && now_seconds() - last_audio_feed_ > 1.0) {
			if (!wall_running_) return wall_base_pts_;
			return wall_base_pts_ + (now_seconds() - wall_base_time_) * speed_.load();
		}
		double latency = 1024.0 / kOutRate;
		// The lip-sync offset shifts the audio-driven clock: +delay moves the
		// clock ahead of the heard audio, so video (presented against the
		// clock) leads the audio by the delay. Subtitles use position(), so
		// they stay glued to the video.
		return std::max(0.0, audio_end_pts_ + audio_delay_.load() - queued_audio_seconds() - latency);
	}
	if (!wall_running_) return wall_base_pts_;
	// No audio: the wall clock drives presentation; it runs at speed_.
	return wall_base_pts_ + (now_seconds() - wall_base_time_) * speed_.load();
}

void Player::set_clock_running(bool running) {
	std::lock_guard<std::mutex> lock(clock_m_);
	if (running == wall_running_) return;
	if (running) {
		wall_base_time_ = now_seconds();
	} else {
		wall_base_pts_ = wall_base_pts_ + (now_seconds() - wall_base_time_) * speed_.load();
	}
	wall_running_ = running;
}

void Player::apply_run_state() {
	bool video_run = !paused_ && !buffering_ && state_ == State::Playing;
	// Audio-first start (Worker 9): the audio output runs as soon as the
	// first audio packet decoded, even while the video gate still holds
	// (initial open and seeks only — the session shows the poster frame
	// meanwhile). Stall rebuffers freeze everything, as before.
	bool audio_run = !paused_ && state_ == State::Playing &&
	                 (!buffering_ || (audio_early_ok_.load() && audio_live_.load()));
	pcm_.pause(!audio_run);
	if (pt_active_) pt_.set_paused(!audio_run);
	set_clock_running(video_run);
}

void Player::set_paused(bool p) {
	paused_ = p;
	apply_run_state();
}

void Player::set_speed(double s) {
	// Snap to the supported set.
	double v = 1.0;
	if (s < 0.75) v = 0.5;
	else if (s < 1.25) v = 1.0;
	else if (s < 1.75) v = 1.5;
	else v = 2.0;
	if (v == speed_.load()) return;
	speed_.store(v);
	// Passthrough can't time-stretch: leave it for PCM at non-1x, and come
	// back when the speed returns to 1x. The demux thread does the actual
	// re-decide (setup_audio_output sleeps for the receiver).
	if (pt_active_.load() && v != 1.0) {
		pt_before_speed_ = true;
		audio_output_recheck_.store(true);
	} else if (!pt_active_.load() && v == 1.0 && pt_before_speed_) {
		pt_before_speed_ = false;
		audio_output_recheck_.store(true);
	}
}

void Player::set_audio_delay(double d) {
	// Clamp to the UI range; the value is an offset on the audio-driven
	// clock, so it takes effect on the next clock read without a restart.
	if (d < -0.5) d = -0.5;
	if (d > 0.5) d = 0.5;
	audio_delay_.store(d);
}

void Player::set_volume(int v) {
	// Clamp 0-200. Takes effect on the next audio buffer (no restart).
	if (v < 0) v = 0;
	if (v > 200) v = 200;
	volume_.store(v);
}

bool Player::buffering(int* percent) const {
	if (percent) *percent = buffer_percent_;
	return state_ == State::Opening || (state_ == State::Playing && buffering_);
}

// Seconds of media queued ahead of the playhead (the same estimate the stall
// logic and the stats line use). 0 when not playing or when unknown.
double Player::buffered_seconds() const {
	if (state_ != State::Playing) return 0.0;
	bool has_v = video_stream_ >= 0, has_a = audio_stream_ >= 0 && (pcm_.is_open() || pt_active_);
	double buf = std::min(has_v ? vq_.duration() : 1e9, has_a ? aq_.duration() : 1e9);
	return (buf >= 1e9 || buf < 0) ? 0.0 : buf;
}

// Embedded chapter markers (MKV/MP4) as skip segments. Many scene/P2P releases
// tag chapters "Intro", "Opening", "Recap", "Credits" — reading them is free
// (FFmpeg parsed them at open) and works offline when neither skip database
// has the episode.
std::vector<aniskip::SkipSegment> Player::chapter_skip_segments() const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);
	std::vector<aniskip::SkipSegment> out;
	if (!fmt_ || fmt_->nb_chapters == 0)
		return out;
	for (unsigned i = 0; i < fmt_->nb_chapters; ++i) {
		const AVChapter* ch = fmt_->chapters[i];
		if (!ch)
			continue;
		AVDictionaryEntry* t = av_dict_get(ch->metadata, "title", nullptr, 0);
		if (!t || !t->value)
			continue;
		std::string name = lower(trim(t->value));
		std::string kind;
		if (name == "intro" || name == "opening" || name == "op" ||
		    name == "theme" || name == "opening theme" ||
		    name.find("intro ") == 0 || name.find("opening ") == 0)
			kind = "intro";
		else if (name == "recap" || name == "previously on" ||
		         name.find("recap") == 0 || name.find("previously") != std::string::npos)
			kind = "recap";
		else if (name == "outro" || name == "ending" || name == "ed" ||
		         name == "credits" || name == "end credits" || name == "creditless")
			kind = "credits";
		if (kind.empty())
			continue;
		double tb = av_q2d(ch->time_base);
		if (tb <= 0.0)
			continue;
		double start = double(ch->start) * tb - start_offset_.load();
		double end = double(ch->end) * tb - start_offset_.load();
		// Sanity: chapters must be a real range; intros longer than 4 min
		// are almost certainly mislabeled.
		if (start >= 0.0 && end > start + 3.0 && (kind != "intro" || end - start <= 240.0))
			out.push_back({start, end, kind});
	}
	return out;
}

void Player::update_buffering() {
	if (state_ != State::Playing) return;
	bool has_v = video_stream_ >= 0, has_a = audio_stream_ >= 0 && (pcm_.is_open() || pt_active_);
	if (buffering_) {
		// The start gate: 0.25 s of contiguous playable data from the first
		// keyframe for the initial open, 0.25 s for a seek (step 3,
		// sub-0.5s seek) — never start on a delta frame. The demux keeps
		// filling toward the deep target in the background after the gate
		// clears. Stall rebuffers climb the kBufferTargets ladder instead
		// (a stream that keeps falling behind waits longer each time).
		double floor_target = (stalls_ >= 2) ? start_target_ : kBufferTargets[stalls_];
		double target = rebuffer_ ? std::max(kBufferTargets[stalls_], floor_target)
		              : seeking_  ? kSeekGate
		                          : kStartGate;
		double decoded_sec = 0.0;
		if (has_v && fps_.load() > 0.0) {
			std::lock_guard<std::mutex> lock(fq_m_);
			decoded_sec = double(frames_.size()) / fps_.load();
		}
		double vbuf = has_v ? (vq_.duration() + decoded_sec) : 1e9, abuf = has_a ? aq_.duration() : 1e9;
		double buf = std::min(vbuf, abuf);
		buffer_percent_ = int(std::min(100.0, buf / target * 100));
		bool have_keyframe_frame;
		{
			std::lock_guard<std::mutex> lock(fq_m_);
			have_keyframe_frame = !has_v || (!frames_.empty() &&
			                                (!saw_keyframe_ || frames_.back().pts >= first_key_pts_ - 0.05));
		}
		// Also go on when memory is nearly full (a very high bitrate).
		if (eof_ || (buf >= target && have_keyframe_frame) || vq_.bytes() + aq_.bytes() > kMaxQueueBytes * 9 / 10) {
			if (stalls_ > 0) dlog("player: buffered %.1f s, playing again", buf);
			buffering_ = false;
			rebuffer_ = false;
			seeking_ = false;  // the seek's sub-0.5s gate cleared; back to the normal start gate
			play_start_ = now_seconds();  // the early-drain guard window starts here
			audio_early_ok_.store(false);
			stalls_cool_start_ = now_seconds();  // stall-free stretch starts here
			apply_run_state();
		}
		return;
	}
	if (eof_) return;
	bool starving = false;
	if (has_v) {
		std::lock_guard<std::mutex> lock(fq_m_);
		if (frames_.empty() && vq_.count() == 0 && !video_drained_) starving = true;
	}
	if (has_a && aq_.count() == 0 && queued_audio_seconds() < 0.05 && !audio_drained_) starving = true;
	if (starving) {
		// The download fell behind: wait longer this time.
		const int last = int(sizeof(kBufferTargets) / sizeof(kBufferTargets[0])) - 1;
		if (stalls_ < last) stalls_++;
		stalls_cool_start_ = -1;  // stall breaks the stall-free stretch
		rebuffer_ = true;
		audio_early_ok_.store(false);  // a stall rebuffer freezes everything, as before
		dlog("player: the stream fell behind (stop %d); buffering %.0f s before going on", ++stall_count_,
		     std::max(kBufferTargets[stalls_], start_target_));
		buffering_ = true;
		buffer_percent_ = 0;
		apply_run_state();
		return;
	}
	// Early-drain guard (Worker 9): in the first 10 s after the start gate
	// cleared, a buffer under ~1 s means the link can't sustain the stream:
	// pause and re-buffer now instead of stuttering through the next seconds
	// (the 2026-10-06 4K-remux case, without the deep upfront wait). Only
	// while data is still arriving — a >10 s no-data stall stays the
	// session's failover trigger, which this never preempts: it only ever
	// re-buffers, and never touches failover state.
	if (play_start_ > 0 && now_seconds() - play_start_ < kEarlyGuardSecs) {
		double decoded_sec = 0.0;
		if (has_v && fps_.load() > 0.0) {
			std::lock_guard<std::mutex> lock(fq_m_);
			decoded_sec = double(frames_.size()) / fps_.load();
		}
		double vbuf = has_v ? (vq_.duration() + decoded_sec) : 1e9;
		double guard_buf = has_v ? vbuf : (has_a ? aq_.duration() : 1e9);
		if (guard_buf < kEarlyGuardBuf) {
			const int last = int(sizeof(kBufferTargets) / sizeof(kBufferTargets[0])) - 1;
			if (stalls_ < last) stalls_++;
			stalls_cool_start_ = -1;
			rebuffer_ = true;
			audio_early_ok_.store(false);
			dlog("player: buffer drained to %.1f s in the first %.0f s (stop %d); re-buffering %.0f s", guard_buf,
			     kEarlyGuardSecs, ++stall_count_, std::max(kBufferTargets[stalls_], start_target_));
			buffering_ = true;
			buffer_percent_ = 0;
			apply_run_state();
			return;
		}
	}
	// Decay the rebuffer penalty: 60 s of uninterrupted playback earns one
	// step back down the kBufferTargets ladder (floor 0). The targets and
	// the escalation itself are untouched: a genuinely bad stream re-escalates
	// on its next stall, but a recovered one stops being punished for history.
	{
		double now = now_seconds();
		if (stalls_cool_start_ < 0) stalls_cool_start_ = now;
		else if (stalls_ > 0 && now - stalls_cool_start_ >= 60.0) {
			stalls_--;
			stalls_cool_start_ = now;
			dlog("player: 60 s without a stall; rebuffer target back down to %.0f s", kBufferTargets[stalls_]);
		}
	}
}

double Player::position() {
	{
		std::lock_guard<std::mutex> lock(seek_m_);
		if (seek_req_) return seek_target_;
	}
	double drop = vdrop_;
	if (drop >= 0) return drop;
	std::lock_guard<std::mutex> lock(clock_m_);
	return clock_locked();
}

void Player::seek(double t) {
	if (duration_.load() > 0) t = std::min(t, std::max(0.0, duration_.load() - 2));
	t = std::max(0.0, t);
	{
		// Snapshot the on-show frame before the seek tears it down; present()
		// serves it until the decoder catches up. seek() otherwise only takes
		// seek_m_, and do_seek() takes fq_m_ alone (never nested with clock_m_),
		// so taking fq_m_ alone here cannot invert the lock order. A second seek before the first
		// completes keeps the original frame: have_picture_ is still false.
		std::lock_guard<std::mutex> lock(fq_m_);
		if (have_picture_) {
			// Move (not copy): the yuv vector's buffer transfers ownership,
			// so hold_.planes pointers stay valid without rebasing.
			// Recycle the previous hold-frame's buffer into the pool instead
			// of freeing it: the snapshot below replaces it.
			if (!hold_.yuv.empty() || !hold_.pixels.empty())
				pool_.push_back(std::move(hold_));
			hold_ = std::move(current_);
			current_ = Frame();
			hold_valid_ = true;
			pic_gen_++;  // new hold frame: upload_video() will see the gen change
		}
	}
	if (quick_) {
		// Opened without the file's index (see demux_run): open again,
		// properly this time, at the new spot. Same file, same tracks.
		dlog("player: first seek, reopening at %.1fs with the index", t);
		Options o = opts_;
		o.start = t;
		hold_keep_ = true;  // open() must not drop the snapshot above
		open(o);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(seek_m_);
		seek_req_ = true;
		seek_target_ = t;
		have_picture_ = false;
		if (state_ == State::Ended) state_ = State::Playing;
	}
	// Jolt the demux/decoder threads out of their waits: the demux thread
	// picks the seek up immediately instead of idling in wait_for_room.
	vq_.notify();
	aq_.notify();
	fq_cv_.notify_all();
}

const Player::Picture* Player::present() {
	present_calls_++;
	const Picture* out = nullptr;
	if (state_ == State::Playing) update_buffering();
	if (state_ == State::Playing || state_ == State::Ended) {
		double clk;
		{
			std::lock_guard<std::mutex> lock(clock_m_);
			clk = clock_locked();
		}
		std::lock_guard<std::mutex> lock(fq_m_);
		int cur = vq_.serial();
		while (!frames_.empty() && frames_.front().serial != cur) {
			pool_.push_back(std::move(frames_.front()));
			frames_.pop_front();
		}
		int pick = -1;
		for (int i = 0; i < int(frames_.size()); i++) {
			if (frames_[i].pts <= clk + 0.008) pick = i;
			else break;
		}
		if (pick < 0 && !have_picture_ && !frames_.empty()) pick = 0;
		if (pick >= 0) {
			Frame& f = frames_[pick];
			have_picture_ = true;
			shown_++;
			// No audio: the first picture after a seek starts the clock.
			if (audio_stream_ < 0 && pick == 0 && !wall_running_) {
				std::lock_guard<std::mutex> cl(clock_m_);
				wall_base_pts_ = f.pts;
			}
			dropped_ += pick;
			dropped_pick_ += pick;
			heavy_window_dropped_ += pick;
			heavy_window_decoded_ += pick + 1;
			// The picked frame is the one on show now; the buffer it replaces goes
			// back to the pool with the frames that were skipped.
			std::swap(current_, f);
			for (int i = 0; i <= pick; i++) {
				pool_.push_back(std::move(frames_.front()));
				frames_.pop_front();
			}
			pic_.yuv = current_.is_yuv;
			pic_.rgba = current_.is_yuv ? nullptr : current_.pixels.data();
			pic_.planes = current_.planes;  // into current_'s storage, like rgba
			pic_.colors = current_.colors;
			pic_.src_w = current_.planes.width;
			pic_.src_h = current_.planes.height;
			pic_.w = current_.w;
			pic_.h = current_.h;
			pic_.gen = ++pic_gen_;
			out = &pic_;
			fq_cv_.notify_all();
			// A fresh frame makes the seek hold-frame stale. Recycle its buffer
			// into the pool instead of freeing it on the GL thread, so the
			// decode thread can reuse it next frame.
			if (!hold_.yuv.empty() || !hold_.pixels.empty())
				pool_.push_back(std::move(hold_));
			hold_valid_ = false;
		}
		else if (hold_valid_) {
			// Seek in flight and the decoder hasn't caught up: keep showing
			// the last picture instead of tearing into a half-ready frame.
			// Not a new frame, so none of the counters or the clock logic run.
			pic_.yuv = hold_.is_yuv;
			pic_.rgba = hold_.is_yuv ? nullptr : hold_.pixels.data();
			pic_.planes = hold_.planes;  // into hold_'s storage, like rgba
			pic_.colors = hold_.colors;
			pic_.src_w = hold_.planes.width;
			pic_.src_h = hold_.planes.height;
			pic_.w = hold_.w;
			pic_.h = hold_.h;
			pic_.gen = pic_gen_;  // hold frame gen was bumped in seek()
			out = &pic_;
		}

		// Ended?
		if (state_ == State::Playing && eof_ && vq_.count() == 0 && aq_.count() == 0 &&
		    (video_stream_ < 0 || (video_drained_ && frames_.empty())) &&
		    (audio_stream_ < 0 || (!pcm_.is_open() && !pt_active_) || (audio_drained_ && queued_audio_seconds() < 0.02)))
			state_ = State::Ended;
	}

	double now = now_seconds();
	if (now - heavy_window_start_ > 8) {
		// too_heavy_ is a software-decode heuristic: when the hardware block
		// is doing the work, frame drops are pacing artifacts, not CPU overload.
		if (!hw_active_.load() && !paused_ && !buffering_ && heavy_window_decoded_ > 60 && heavy_window_dropped_ * 4 > heavy_window_decoded_)
			too_heavy_ = true;
		heavy_window_start_ = now;
		heavy_window_dropped_ = heavy_window_decoded_ = 0;
	}
	return out;
}
// ---------------------------------------------------------------------------
// Tracks and stats

std::string Player::track_label(int stream) const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);
	if (!fmt_ || stream < 0 || stream >= int(fmt_->nb_streams)) return "";
	AVStream* st = fmt_->streams[stream];
	AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
	AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
	std::vector<std::string> parts;
	if (lang && strcmp(lang->value, "und") != 0) parts.push_back(language_name(lang->value));
	if (title && title->value[0]) parts.push_back(title->value);
	std::string codec = avcodec_get_name(st->codecpar->codec_id);
	for (auto& c : codec) c = char(toupper((unsigned char)c));
	if (codec == "SUBRIP") codec = "SRT";
	if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
		int ch = st->codecpar->ch_layout.nb_channels;
		std::string chs = ch == 1 ? "mono" : ch == 2 ? "stereo" : ch == 6 ? "5.1" : ch == 8 ? "7.1" : std::to_string(ch) + "ch";
		codec += " " + chs;
	}
	parts.push_back(codec);
	if (st->disposition & AV_DISPOSITION_FORCED) parts.push_back("forced");
	return join(parts, " · ");
}

std::vector<Player::Track> Player::audio_tracks() const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);
	std::vector<Track> out;
	if (!fmt_ || state_ != State::Playing) return out;
	for (unsigned i = 0; i < fmt_->nb_streams; i++) {
		AVStream* st = fmt_->streams[i];
		if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
		AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
		out.push_back(Track{int(i), track_label(int(i)), lang ? language_to_iso639_2(lang->value) : ""});
	}
	return out;
}

std::vector<Player::Track> Player::subtitle_tracks() const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);
	std::vector<Track> out;
	if (!fmt_ || state_ != State::Playing) return out;
	for (auto& kv : sctx_) {
		AVStream* st = fmt_->streams[kv.first];
		AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
		out.push_back(Track{kv.first, track_label(kv.first), lang ? language_to_iso639_2(lang->value) : ""});
	}
	return out;
}

void Player::select_audio(int stream) { audio_switch_ = stream; }

std::string Player::embedded_subtitle(int stream, double t, int* an) {
	std::lock_guard<std::mutex> lock(sub_m_);
	auto it = embedded_.find(stream);
	if (it == embedded_.end()) return "";
	return cues_at(it->second, t, an);
}

std::string Player::hw_note() const {
	std::lock_guard<std::mutex> lock(err_m_);
	return hw_note_;
}

// The coded video stream's parameters, or null when there is no video.
// Mirrors codec_summary(): the session reads these on the UI thread.
const AVCodecParameters* Player::video_par() const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);
	int vs = video_stream_.load();
	if (!fmt_ || vs < 0 || vs >= int(fmt_->nb_streams)) return nullptr;
	return fmt_->streams[vs]->codecpar;
}

int Player::video_w() const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);
	const AVCodecParameters* p = video_par();
	return p ? p->width : 0;
}

int Player::video_h() const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);
	const AVCodecParameters* p = video_par();
	return p ? p->height : 0;
}

std::string Player::video_codec() const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);
	const AVCodecParameters* p = video_par();
	if (!p) return "";
	const char* n = avcodec_get_name(p->codec_id);
	return n ? n : "";
}

std::string Player::codec_summary() const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);
	if (!fmt_ || state_ != State::Playing) return "";
	std::vector<std::string> parts;
	int vs = video_stream_, as = audio_stream_;
	if (vs >= 0) {
		AVCodecParameters* p = fmt_->streams[vs]->codecpar;
		std::string c = avcodec_get_name(p->codec_id);
		for (auto& ch : c) ch = char(toupper((unsigned char)ch));
		parts.push_back(c + " " + std::to_string(p->width) + "x" + std::to_string(p->height) +
		                (hw_active_ ? " (hardware)" : " (software)"));
	}
	if (as >= 0) parts.push_back(track_label(as));
	return join(parts, " · ");
}

void Player::log_stats() {
	double now = now_seconds();
	if (state_ != State::Playing) {
		log_at_ = 0;
		return;
	}
	if (log_at_ == 0) {  // start counting
		log_at_ = now;
		log_shown_ = shown_;
		log_dropped_ = dropped_;
		log_dropped_late_ = dropped_late_;
		log_dropped_pick_ = dropped_pick_;
		log_present_calls_ = present_calls_;
		return;
	}
	if (now - log_at_ < 10) return;
	double secs = now - log_at_;
	int shown = shown_ - log_shown_, dropped = dropped_ - log_dropped_;
	int late = dropped_late_ - log_dropped_late_;
	int pick = dropped_pick_ - log_dropped_pick_;
	int pcalls = present_calls_ - log_present_calls_;
	dlog("player: %s | shown %.1f fps (video %.3g fps), dropped %d (late=%d pick=%d) in %.0f s, present_calls=%d, buffered %.1f s%s%s",
	     codec_summary().c_str(), shown / secs, fps_.load(), dropped, late, pick, secs, pcalls,
	     std::max(vq_.duration(), aq_.duration()), paused_ ? ", paused" : "", buffering_ ? ", buffering" : "");
	log_at_ = now;
	log_shown_ = shown_;
	log_dropped_ = dropped_;
	log_dropped_late_ = dropped_late_;
	log_dropped_pick_ = dropped_pick_;
	log_present_calls_ = present_calls_;
}

std::string Player::stats() {
	if (state_ != State::Playing) return "";
	char buf[256];
	snprintf(buf, sizeof(buf), "%s · %.3g fps · buffered %.1fs", codec_summary().c_str(), fps_.load(),
	         std::max(vq_.duration(), aq_.duration()));
	return buf;
}

std::vector<std::string> Player::nerd_stats() const {
	std::lock_guard<std::recursive_mutex> lock(fmt_m_);
	std::vector<std::string> lines;
	if (state_ != State::Playing || !fmt_) return lines;
	lines.push_back(codec_summary());
	char buf[160];
	snprintf(buf, sizeof(buf), "%.3g fps · dropped %d of %d", fps_.load(), dropped_.load(), decoded_.load());
	lines.push_back(buf);
	const double spd = speed_.load();
	if (spd != 1.0) {
		snprintf(buf, sizeof(buf), "speed %.1fx", spd);
		lines.push_back(buf);
	}
	snprintf(buf, sizeof(buf), "buffered %.1f s", std::max(vq_.duration(), aq_.duration()));
	lines.push_back(buf);
	if (fmt_->bit_rate > 0) {
		snprintf(buf, sizeof(buf), "bitrate %.1f Mbps", fmt_->bit_rate / 1000000.0);
		lines.push_back(buf);
	}
	return lines;
}
