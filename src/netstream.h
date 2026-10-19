// Network input for the player: FFmpeg reads HTTP(S) through libcurl.
//
// FFmpeg's own HTTP client never got a byte from the streaming server on the
// PS5 (it opened, waited and gave up), while libcurl talks to the same server
// fine. So every http(s) URL the player opens -- the stream itself and, for
// HLS, the playlists and segments -- goes through NetStream instead.
#pragma once

#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVIOContext;
struct AVFormatContext;
struct AVDictionary;

// Big files (debrid links, direct URLs) can be fetched by several connections
// at once ("parallel"): one connection is often what limits the speed, not
// the line (Wi-Fi caps a single stream, debrid hosts cap each connection).
// The idea and sizes come from Nuvio PS5's evo_parallel_io.c.
class NetStream {
public:
	NetStream(const std::string& url, const std::vector<std::string>& headers, const std::atomic<bool>* abort,
	          bool parallel = false);
	~NetStream();

	// Starts downloading and waits for the response headers. False on error
	// (see error()).
	bool start();
	const std::string& error() const { return error_; }  // after start(), same thread
	std::string failure();  // any time: why downloading stopped, if it did
	int64_t size() const { return size_; }  // -1 when unknown

	// The first bytes at the current position, without consuming them
	// (fewer at the end of the file or on error).
	std::string peek(size_t n);
	// Urgent prefetch of the chunk holding byte_offset (parallel mode only):
	// asks the download workers to fetch it now, without moving the
	// read-ahead window. No-op when not downloading in parallel or the size
	// is unknown.
	void prewarm_offset(int64_t byte_offset);
	static NetStream* of(AVIOContext* pb);

	// Bytes downloaded since the first byte arrived, over the elapsed time:
	// the session's Auto-quality decision in the first seconds of buffering.
	// Rides on the transfer's own counters: zero extra traffic. -1 until a
	// second of data has arrived.
	double measured_bps() const;
	// Seconds since the last byte arrived on any connection (-1 when nothing
	// arrived yet). For the session's no-data stall detection.
	double seconds_since_data() const;

	// AVIOContext callbacks: FFmpeg error codes on failure.
	int read(uint8_t* buf, int n);
	int64_t seek(int64_t offset, int whence);

	// An AVIOContext reading this stream; free with close_avio().
	// Boot cleanup: deletes leftover stream-cache-*.bin files from older builds.
	static void set_cache_dir(const std::string& dir);

	static AVIOContext* open_avio(const std::string& url, const std::vector<std::string>& headers,
	                              const std::atomic<bool>* abort, std::string* error, bool parallel = false);
	static bool is_ours(AVIOContext* pb);
	static void close_avio(AVIOContext** pb);

	// Pre-warms DNS + TCP/TLS to url's origin (following redirects to warm
	// the CDN edge origin too) with a 1-byte Range request, then stores the
	// live handle in g_pool so open_avio() reuses the warm socket immediately.
	// HTTP(S) only, side-effect-free. Call from a worker thread, never the UI.
	static void prewarm(const std::string& url, const std::vector<std::string>& headers);

	// Next-episode prefetch: the first 2 MB (one parallel chunk) of a URL,
	// fetched in the background on one connection and handed to the next
	// open_avio() for the same URL. One entry at a time; a new prefetch
	// supersedes the old, a stale one (>15 min) is dropped. The session
	// drives this (Worker 9); torrents go through bt::Engine::prefetch_file
	// instead.
	static void prefetch_start(const std::string& url, const std::vector<std::string>& headers);
	static void prefetch_cancel();
	// Takes a matching prefetch (even partial): false when there is none.
	static bool prefetch_take(const std::string& url, std::vector<uint8_t>* data, int64_t* total_size,
	                          std::string* final_url);

	// AVFormatContext.io_open / io_close2 for the files a demuxer opens
	// itself (HLS playlists and segments). opaque must point at the
	// player's IoHooks.
	struct IoHooks {
		std::vector<std::string> headers;
		const std::atomic<bool>* abort = nullptr;
		int (*default_open)(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) = nullptr;
		int (*default_close)(AVFormatContext*, AVIOContext*) = nullptr;
	};
	static void install(AVFormatContext* fmt, IoHooks* hooks);

private:
	void run();
	bool transfer(int64_t from, int gen);
	bool stopped() const;
	void note_bytes(int64_t n);  // m_ held: throughput + last-data accounting
	// Seeds buf_ with prefetched leading bytes (open_avio, before start()).
	void seed_prefetch(std::vector<uint8_t>&& data, int64_t total_size, const std::string& final_url);

	// Parallel first segments (HTTP only): the header + first ~8 MB over 4
	// range requests, coalesced; the single connection then continues after
	// them. Falls back to the single connection from 0 when the server
	// rejects ranges or anything fails.
	struct SegRange {
		int64_t from = 0, to = 0;  // [from, to)
		std::vector<uint8_t> data;
		bool done = false, failed = false;
	};
	static const int kSegFailed = INT_MIN;  // read_first_seg: segments abandoned, use the normal path
	void parallelize_first_segments();  // probe concurrently; on big files promote the first 8 MB to 4 ranges
	void fetch_range(int idx, int64_t from, int64_t to, const std::string& url, int gen);
	void mark_range_failed(int idx, const std::string& why);  // m_ held
	SegRange* range_for(int64_t pos);                         // m_ held, null outside the head
	int read_first_seg(std::unique_lock<std::mutex>& lock, uint8_t* buf, int n);
	int read_normal(std::unique_lock<std::mutex>& lock, uint8_t* buf, int n);
	void abandon_first_segments(std::unique_lock<std::mutex>& lock, int64_t pos);  // m_ held
	struct RangeCtx {
		NetStream* s;
		int idx;
		int gen;
	};
	static size_t range_data_cb(char* p, size_t size, size_t n, void* ud);
	static int range_progress_cb(void* ud, int64_t, int64_t, int64_t, int64_t);
	bool first_seg_active_ = false;
	int64_t first_seg_end_ = 0;
	int seg_gen_ = 0;  // range threads abort when this moves
	bool seg_failed_ = false;
	std::vector<SegRange> seg_ranges_;
	std::vector<std::thread> seg_threads_;

	// Parallel mode: chunks of the file in a window ahead of the reader.
	struct Chunk {
		int64_t index = -1;  // which chunk this slot holds
		int state = 0;       // 0 empty, 1 downloading, 2 ready
		int attempts = 0;
		double retry_at = 0;
		int64_t len = 0;
		std::vector<uint8_t> data;  // when the read-ahead is in memory
	};
	void start_parallel();
	void worker(int idx = 0);
	int read_parallel(std::unique_lock<std::mutex>& lock, uint8_t* buf, int n);
	void log_speed(int64_t bytes);
	bool parallel_wanted_ = false, parallel_ = false;
	int window_ = 32;           // chunks held ahead of the reader (pure RAM)
	double speed_at_ = 0;
	int64_t speed_bytes_ = 0;
	std::string final_url_;  // after redirects, so each chunk goes straight there
	void* cur_curl_ = nullptr;  // the single download's handle (for its final URL)
	std::vector<Chunk> chunks_;
	std::vector<std::thread> workers_;
	int64_t window_base_ = 0;  // first chunk of the window
	int64_t urgent_chunk_ = -1;  // tail-seek chunk: fetch it without moving the window
	int64_t par_bytes_ = 0;
	double par_started_ = 0;

	static size_t on_header(char* data, size_t size, size_t n, void* self);
	static size_t on_data(char* data, size_t size, size_t n, void* self);
	static int on_progress(void* self, int64_t, int64_t, int64_t, int64_t);

	std::string url_;
	std::vector<std::string> headers_;
	const std::atomic<bool>* abort_;

	mutable std::mutex m_;
	std::condition_variable cv_;
	std::thread thread_;
	bool stop_ = false;

	// Requests to the download thread: (re)start at req_pos_ when gen_ moves.
	int gen_ = 0, started_gen_ = -1;
	int64_t req_pos_ = 0;

	// Downloaded, not yet read: buf_[head_..] holds bytes from pos_ on.
	std::vector<uint8_t> buf_;
	size_t head_ = 0;
	int64_t pos_ = 0;
	bool eof_ = false;
	bool headers_done_ = false;
	long status_ = 0;
	int64_t size_ = -1;
	std::string error_;
	int64_t discard_ = 0;  // bytes still to drop before buf_ (short forward seeks, or a server that ignored Range)
	bool header_ok_ = false;
	int64_t range_from_ = 0;  // where the running transfer was asked to start
	int cur_gen_ = 0;      // generation of the running transfer (for the callbacks)
	// Throughput accounting (m_): bytes since the first byte arrived.
	double tput_start_ = 0;
	int64_t tput_bytes_ = 0;
	double last_data_at_ = 0;  // wall time of the last byte on any connection
	bool prefetch_seeded_ = false;  // buf_ holds prefetched leading bytes
};
