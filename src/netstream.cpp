#include "netstream.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <unistd.h>
#include <vector>

#include "http.h"
#include "util.h"

namespace {

const size_t kMaxBuffered = 32u << 20;   // read-ahead
const int64_t kSkipWindow = 1 << 20;    // forward seeks shorter than this read on
const int kRetries = 20;  // slow torrents: keep trying, as the PC app does (Circle stops)

// Parallel mode (Nuvio PS5's numbers): files from 96 MB, 4 MB chunks, six
// connections. The chunks ahead of the reader go to a file in the app's
// storage, up to 6 GB (about half an hour of 4K at 25 Mbit/s), so a slow
// patch in the download doesn't stop playback; in memory (128 MB) when the
// file can't be used.
const int64_t kParallelMin = 96ll << 20;
const int64_t kChunk = 2ll << 20;
const int kWindowRam = 64;  // the whole parallel window, in RAM (128 MB)
const int kWorkers = 6;
// A streaming server may still be fetching that part of the torrent: be patient.
const int kChunkAttempts = 20;
const double kSpeedLogEvery = 30;

int read_cb(void* opaque, uint8_t* buf, int n) { return static_cast<NetStream*>(opaque)->read(buf, n); }
int64_t seek_cb(void* opaque, int64_t offset, int whence) {
	return static_cast<NetStream*>(opaque)->seek(offset, whence);
}

bool is_http(const char* url) { return !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8); }

// ---------------------------------------------------------------------------
// Idle connection pool: curl handles kept across NetStream lifetimes so that
// consecutive HLS segments (same scheme + host + port) reuse the TCP/TLS
// connection instead of handshaking per segment. A handle is checked out
// exclusively (the mutex only guards the pool itself); curl_easy_reset() on
// checkout keeps the pooled connection while clearing per-request options.
// Entries idle longer than kPoolIdleMax are dropped, bounding the damage if
// a server silently closed its side.

const size_t kPoolMax = 8;
const double kPoolIdleMax = 120.0;

struct IdleConn {
	CURL* c;
	std::string origin;
	double idle_since;
};
std::mutex g_pool_m;
std::vector<IdleConn> g_pool;

std::string conn_origin(const std::string& url) {  // "scheme://host:port"
	size_t scheme_end = url.find("://");
	if (scheme_end == std::string::npos) return url;
	std::string scheme = url.substr(0, scheme_end);
	size_t host_start = scheme_end + 3;
	size_t host_end = url.find('/', host_start);
	std::string authority =
	    url.substr(host_start, host_end == std::string::npos ? std::string::npos : host_end - host_start);
	size_t at = authority.rfind('@');  // strip userinfo; never part of the origin
	if (at != std::string::npos) authority = authority.substr(at + 1);
	std::string host = authority, port;
	if (!authority.empty() && authority[0] == '[') {  // IPv6 literal
		size_t close = authority.find(']');
		if (close != std::string::npos) {
			host = authority.substr(0, close + 1);
			if (close + 1 < authority.size() && authority[close + 1] == ':') port = authority.substr(close + 2);
		}
	} else {
		size_t colon = authority.rfind(':');
		if (colon != std::string::npos) {
			host = authority.substr(0, colon);
			port = authority.substr(colon + 1);
		}
	}
	if (port.empty()) port = (scheme == "https") ? "443" : "80";
	return scheme + "://" + host + ":" + port;
}

CURL* pool_checkout(const std::string& origin) {
	std::lock_guard<std::mutex> lock(g_pool_m);
	double now = now_seconds();
	for (size_t i = 0; i < g_pool.size(); i++) {
		if (g_pool[i].origin != origin) continue;
		CURL* c = g_pool[i].c;
		double idle_since = g_pool[i].idle_since;
		g_pool.erase(g_pool.begin() + i);
		if (now - idle_since > kPoolIdleMax) {  // stale: don't risk a dead connection
			curl_easy_cleanup(c);
			return nullptr;
		}
		return c;
	}
	return nullptr;
}

void pool_return(const std::string& origin, CURL* c) {
	if (!c) return;
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, (char*)nullptr);
	std::lock_guard<std::mutex> lock(g_pool_m);
	double now = now_seconds();
	for (size_t i = 0; i < g_pool.size();) {
		if (now - g_pool[i].idle_since > kPoolIdleMax) {
			curl_easy_cleanup(g_pool[i].c);
			g_pool.erase(g_pool.begin() + i);
		} else {
			i++;
		}
	}
	if (g_pool.size() >= kPoolMax) {
		curl_easy_cleanup(c);
		return;
	}
	g_pool.push_back({c, origin, now});
}

// Range response headers: the status plus the full size from Content-Range
// (a 206 to "Range: 0-0" answers "Content-Range: bytes 0-0/<size>"). One
// instance per request; fed by range_header_cb below. Moved up here so the
// prewarm probe (at global scope, between the two anonymous namespaces) can
// use it too.
struct RangeHeaders {
	long status = 0;
	int64_t size = -1;
	void feed(const std::string& line) {
		if (starts_with(line, "HTTP/")) {
			size_t sp = line.find(' ');
			status = sp == std::string::npos ? 0 : std::atol(line.c_str() + sp + 1);
		} else {
			std::string low = lower(line);
			if (starts_with(low, "content-range:")) {
				size_t slash = low.find('/');
				if (slash != std::string::npos && low[slash + 1] != '*') size = std::atoll(low.c_str() + slash + 1);
			} else if (starts_with(low, "content-length:") && status == 200 && size < 0) {
				size = std::atoll(low.c_str() + 15);
			}
		}
	}
};

static size_t range_header_cb(char* data, size_t size, size_t n, void* ud) {
	static_cast<RangeHeaders*>(ud)->feed(std::string(data, size * n));
	return size * n;
}

// ---------------------------------------------------------------------------
// Prewarm metadata cache: prewarm()'s 1-byte probe already pays the ~1 RTT
// header dance; on a 206 answer it stores {url, final_url, size} here so a
// parallel start() for the same URL skips its own header wait entirely (true
// zero-RTT). Entries expire after 1200 s (20 min); capped at 16, oldest dropped.
struct PrewarmMeta {
	std::string url;
	std::string final_url;
	int64_t size = -1;
	long status = 0;
	double at = 0.0;
};
std::mutex g_prewarm_m;
std::vector<PrewarmMeta> g_prewarm_meta;  // capped at 16 entries

// Pre-warm state: counts bytes so a server that ignores our Range (and
// would stream the whole file) gets aborted before it costs anything.
struct PrewarmState {
	size_t bytes = 0;
};
static size_t prewarm_discard(char*, size_t size, size_t n, void* userdata) {
	auto* st = static_cast<PrewarmState*>(userdata);
	st->bytes += size * n;
	if (st->bytes > 65536) return 0;  // Range ignored: abort, don't pool
	return size * n;
}

} // namespace — NetStream::prewarm must be at global scope

void NetStream::prewarm(const std::string& url, const std::vector<std::string>& headers) {
	if (!is_http(url.c_str())) return;
	CURL* c = pool_checkout(conn_origin(url));
	if (!c) c = curl_easy_init();
	if (!c) return;
	curl_easy_reset(c);
	char errbuf[CURL_ERROR_SIZE] = {0};
	struct curl_slist* hdrs = nullptr;
	for (auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());
	PrewarmState st;
	RangeHeaders rh;  // feeds the metadata cache on success (status + size)
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
	curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 6L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, prewarm_discard);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &st);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, range_header_cb);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, &rh);
	if (!http_ca_bundle().empty())
		curl_easy_setopt(c, CURLOPT_CAINFO, http_ca_bundle().c_str());
	http_setup_handle(c);
	if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
	CURLcode rc = curl_easy_perform(c);
	if (hdrs) curl_slist_free_all(hdrs);
	// Only pool a cleanly-completed warm connection; anything else (abort,
	// timeout, error) gets destroyed so the pool never holds a dead handle.
	if (rc == CURLE_OK && st.bytes <= 65536) {
		char* eff = nullptr;
		curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
		std::string final_url = eff ? eff : url;
		pool_return(conn_origin(final_url), c);
		// Metadata cache: the probe already did the ~1 RTT header dance; a
		// 206 answer carries the full size, so a parallel start() for this
		// URL skips its own header wait entirely. Refresh an existing entry
		// for the URL, else push and trim the cache to 16.
		if (rh.status == 206 && rh.size > 0) {
			PrewarmMeta pm{url, final_url, rh.size, 206, now_seconds()};
			std::lock_guard<std::mutex> lock(g_prewarm_m);
			bool found = false;
			for (auto& e : g_prewarm_meta)
				if (e.url == url) {
					e = pm;
					found = true;
					break;
				}
			if (!found) {
				g_prewarm_meta.push_back(pm);
				while (g_prewarm_meta.size() > 16) g_prewarm_meta.erase(g_prewarm_meta.begin());
			}
		}
	} else {
		curl_easy_cleanup(c);
	}
}

namespace { // reopen anonymous namespace for the remaining helpers

int io_open_cb(AVFormatContext* s, AVIOContext** pb, const char* url, int flags, AVDictionary** options) {
	auto* hooks = static_cast<NetStream::IoHooks*>(s->opaque);
	if ((flags & AVIO_FLAG_WRITE) || !is_http(url)) return hooks->default_open(s, pb, url, flags, options);
	std::string err;
	*pb = NetStream::open_avio(url, hooks->headers, hooks->abort, &err);
	if (!*pb) {
		dlog("net: %s: %s", log_url(url).c_str(), err.c_str());
		return err == "cancelled" ? AVERROR_EXIT : AVERROR(EIO);
	}
	return 0;
}

int io_close_cb(AVFormatContext* s, AVIOContext* pb) {
	if (NetStream::is_ours(pb)) {
		NetStream::close_avio(&pb);
		return 0;
	}
	auto* hooks = static_cast<NetStream::IoHooks*>(s->opaque);
	return hooks->default_close(s, pb);
}

// ---------------------------------------------------------------------------
// Parallel first segments (Worker 9): the header + first ~8 MB of an HTTP(S)
// file over 4 range requests, so playback's critical first bytes arrive over
// several connections. One blocking header probe (~1 RTT) decides; anything
// unexpected falls back to the single connection.

const int64_t kSegBytes = 2ll << 20;    // each first-segment range
const int kSegCount = 4;                // ranges: 4 x 2 MB = first 8 MB
const int64_t kSegMinSize = 16ll << 20;  // parallel first segments only above this file size
const int64_t kPrefetchBytes = 2ll << 20;  // next-episode prefetch: the first 2 MB (one parallel chunk)
const double kPrefetchMaxAge = 900.0;     // a prefetch older than this is dropped

// The probe only wants headers: abort the body past 64 KB (a server that
// ignores Range would stream the whole file at us).
struct ProbeBody {
	size_t bytes = 0;
};
static size_t probe_body_cb(char*, size_t size, size_t n, void* ud) {
	auto* b = static_cast<ProbeBody*>(ud);
	b->bytes += size * n;
	if (b->bytes > 65536) return 0;
	return size * n;
}
static int probe_progress_cb(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	auto* abort = static_cast<const std::atomic<bool>*>(ud);
	return (abort && abort->load()) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Next-episode prefetch (Worker 9): one entry, detached worker, flag-driven
// (no joins: superseding/cancelling never blocks).

struct PrefetchState {
	std::mutex m;
	std::string url;
	std::vector<std::string> headers;
	std::vector<uint8_t> data;
	int64_t total = -1;
	std::string final_url;
	double started_at = 0;
	int gen = 0;
	std::atomic<bool> cancelled{false};
	RangeHeaders rh;  // the latest response's headers (m_ held)
};
PrefetchState g_prefetch;
struct PrefetchGen {
	int gen;
};

static size_t prefetch_header_cb(char* data, size_t size, size_t n, void* ud) {
	auto* pg = static_cast<PrefetchGen*>(ud);
	std::lock_guard<std::mutex> lock(g_prefetch.m);
	if (pg->gen == g_prefetch.gen) {
		g_prefetch.rh.feed(std::string(data, size * n));
		if (g_prefetch.rh.size > 0) g_prefetch.total = g_prefetch.rh.size;
	}
	return size * n;
}

// The prefetch only wants the head: stop past 2 MB (a server that ignores
// Range would stream the whole file at us — politeness).
static size_t prefetch_data_cb(char* p, size_t size, size_t n, void* ud) {
	auto* pg = static_cast<PrefetchGen*>(ud);
	size_t len = size * n;
	std::lock_guard<std::mutex> lock(g_prefetch.m);
	if (pg->gen != g_prefetch.gen || g_prefetch.cancelled.load()) return 0;
	size_t room = g_prefetch.data.size() < size_t(kPrefetchBytes) ? size_t(kPrefetchBytes) - g_prefetch.data.size() : 0;
	if (room == 0) return 0;
	size_t k = std::min(len, room);
	g_prefetch.data.insert(g_prefetch.data.end(), p, p + k);
	return k < len ? 0 : len;  // short write aborts the transfer once the head is complete
}
static int prefetch_progress_cb(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	auto* pg = static_cast<PrefetchGen*>(ud);
	std::lock_guard<std::mutex> lock(g_prefetch.m);
	return (pg->gen != g_prefetch.gen || g_prefetch.cancelled.load()) ? 1 : 0;
}

}  // namespace

NetStream::NetStream(const std::string& url, const std::vector<std::string>& headers, const std::atomic<bool>* abort,
                     bool parallel)
    : url_(url), headers_(headers), abort_(abort), parallel_wanted_(parallel) {}

NetStream::~NetStream() {
	{
		std::lock_guard<std::mutex> lock(m_);
		stop_ = true;
		gen_++;
		seg_gen_++;  // the range threads abort in their progress callback
	}
	cv_.notify_all();
	if (thread_.joinable()) thread_.join();
	for (auto& w : workers_)
		if (w.joinable()) w.join();
	for (auto& w : seg_threads_)
		if (w.joinable()) w.join();
}

// Boot cleanup only: parallel streams no longer write stream-cache-*.bin,
// but an older build (or a crash) may have left them behind.
void NetStream::set_cache_dir(const std::string& dir) {
	for (int i = 0; i < 16; i++) remove((dir + "/stream-cache-" + std::to_string(i) + ".bin").c_str());  // left by a crash
}

// The download speed now and then, for the log (not shown in the app).
void NetStream::log_speed(int64_t bytes) {  // m_ held
	speed_bytes_ += bytes;
	double now = now_seconds();
	if (speed_at_ == 0) speed_at_ = now;
	if (now - speed_at_ < kSpeedLogEvery) return;
	double rate = speed_bytes_ / (now - speed_at_) / (1 << 20);
	if (parallel_) {
		int64_t idx = pos_ / kChunk, ahead = 0;  // ready chunks from the reader on
		while (ahead < window_) {
			const Chunk& s = chunks_[size_t((idx + ahead) % window_)];
			if (s.index != idx + ahead || s.state != 2) break;
			ahead++;
		}
		dlog("net: %.2f MB/s over %d connections, %lld MB downloaded ahead of the player (in memory)", rate, kWorkers,
		     (long long)(ahead * (kChunk >> 20)));
	} else {
		dlog("net: %.2f MB/s over one connection", rate);
	}
	speed_at_ = now;
	speed_bytes_ = 0;
}

bool NetStream::stopped() const { return stop_ || (abort_ && abort_->load()); }

bool NetStream::start() {
	std::unique_lock<std::mutex> lock(m_);
	// Prewarm metadata fast path: prewarm()'s 1-byte probe for this URL already
	// learned the size and final URL, so the parallel transfer opens with
	// zero RTT — no single-connection thread is spawned and there is no
	// header wait. start_parallel() is self-contained (own gen_++, cache
	// setup, chunks_, worker spawn), so everything the normal path sets up
	// after its header wait is covered here too.
	if (parallel_wanted_) {
		std::lock_guard<std::mutex> pl(g_prewarm_m);
		const double now = now_seconds();
		for (const auto &pm : g_prewarm_meta) {
			if (pm.url == url_ && now - pm.at < 1200.0 && pm.status == 206 && pm.size >= kParallelMin) {
				size_ = pm.size;
				final_url_ = pm.final_url;
				status_ = 206;
				headers_done_ = true;
				break;
			}
		}
	}
	if (parallel_wanted_ && headers_done_ && size_ >= kParallelMin && status_ == 206) {
		dlog("net: prewarm metadata hit for %s, skipping header wait", log_url(url_).c_str());
		start_parallel();
		return true;
	}
	req_pos_ = prefetch_seeded_ ? int64_t(buf_.size()) : 0;
	gen_++;
	thread_ = std::thread([this] { run(); });  // the single connection starts now: fast first byte
	lock.unlock();
	// Concurrently: when the file is big and the server takes ranges, the
	// first 8 MB go over 4 parallel range requests instead (the transfer
	// restarts after them). Small files and HLS segments never notice.
	parallelize_first_segments();
	lock.lock();
	while (!headers_done_ && error_.empty() && !eof_) {
		if (abort_ && abort_->load()) {
			error_ = "cancelled";
			break;
		}
		cv_.wait_for(lock, std::chrono::milliseconds(100));
	}
	// A big file from a server that takes byte ranges: several connections.
	if (error_.empty() && parallel_wanted_ && size_ >= kParallelMin && status_ == 206) start_parallel();
	return error_.empty();
}

// Parallel first segments, part 2 (Worker 9): a blocking header probe
// (Range: 0-0, ~1 RTT) while the single connection is already running. On
// success the transfer's prefix is dropped (re-fetched by range 0 — at most
// ~2 MB, usually a few KB) and it restarts after the 8 MB the 4 ranges
// fetch. Anything else: the transfer carries on untouched, zero overhead
// beyond one tiny concurrent request.
void NetStream::parallelize_first_segments() {
	if (!is_http(url_.c_str()) || parallel_wanted_ || stopped()) return;
	{
		std::lock_guard<std::mutex> lock(m_);
		if (prefetch_seeded_ || first_seg_active_ || !error_.empty()) return;
	}
	RangeHeaders rh;
	std::string final_url;
	{
		CURL* c = curl_easy_init();
		if (!c) return;
		struct curl_slist* hdrs = nullptr;
		for (auto& h : headers_) hdrs = curl_slist_append(hdrs, h.c_str());
		curl_easy_setopt(c, CURLOPT_URL, url_.c_str());
		curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
		curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
		curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
		curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
		curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, range_header_cb);
		curl_easy_setopt(c, CURLOPT_HEADERDATA, &rh);
		ProbeBody pb;
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, probe_body_cb);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, &pb);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, probe_progress_cb);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, abort_);
		if (!http_ca_bundle().empty())
			curl_easy_setopt(c, CURLOPT_CAINFO, http_ca_bundle().c_str());
		http_setup_handle(c);
		if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
		CURLcode rc = curl_easy_perform(c);
		char* eff = nullptr;
		if (rc == CURLE_OK || rc == CURLE_WRITE_ERROR)  // WRITE_ERROR: our 64 KB body cap; headers are in
			curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
		if (rc == CURLE_OK || rc == CURLE_WRITE_ERROR) pool_return(conn_origin(eff ? eff : url_.c_str()), c);
		else curl_easy_cleanup(c);
		if (hdrs) curl_slist_free_all(hdrs);
		final_url = eff ? eff : url_;
		if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) return;
	}
	if (rh.status != 206 || rh.size < kSegMinSize) return;
	std::vector<std::thread> threads;
	{
		std::lock_guard<std::mutex> lock(m_);
		if (stopped() || prefetch_seeded_ || first_seg_active_ || !error_.empty()) return;
		// Drop the transfer's head start: the ranges re-fetch 0..8 MB.
		buf_.clear();
		head_ = 0;
		pos_ = 0;
		discard_ = 0;
		size_ = rh.size;
		final_url_ = final_url;
		status_ = 206;
		headers_done_ = true;
		first_seg_active_ = true;
		first_seg_end_ = int64_t(kSegCount) * kSegBytes;
		seg_gen_++;
		seg_failed_ = false;
		seg_ranges_.clear();
		seg_ranges_.reserve(kSegCount);
		for (int i = 0; i < kSegCount; i++) {
			SegRange r;
			r.from = int64_t(i) * kSegBytes;
			r.to = r.from + kSegBytes;
			r.data.reserve(size_t(kSegBytes));
			seg_ranges_.push_back(std::move(r));
		}
		req_pos_ = first_seg_end_;
		gen_++;  // the running transfer(0) aborts; run() restarts it after the ranges
		cv_.notify_all();
		const int gen = seg_gen_;
		for (int i = 0; i < kSegCount; i++) {
			int64_t from = int64_t(i) * kSegBytes, to = from + kSegBytes;
			threads.emplace_back([this, i, from, to, final_url, gen] { fetch_range(i, from, to, final_url, gen); });
		}
		dlog("net: first %lld MB over %d connections (%s)", (long long)(first_seg_end_ >> 20), kSegCount,
		     log_url(url_).c_str());
	}
	for (auto& t : threads) seg_threads_.push_back(std::move(t));
}

void NetStream::fetch_range(int idx, int64_t from, int64_t to, const std::string& url, int gen) {
	CURL* c = pool_checkout(conn_origin(url));
	if (!c) c = curl_easy_init();
	if (!c) {
		std::lock_guard<std::mutex> lock(m_);
		mark_range_failed(idx, "curl_easy_init failed");
		return;
	}
	RangeCtx ctx{this, idx, gen};
	char errbuf[CURL_ERROR_SIZE] = {0};
	struct curl_slist* hdrs = nullptr;
	for (auto& h : headers_) hdrs = curl_slist_append(hdrs, h.c_str());
	std::string range = std::to_string(from) + "-" + std::to_string(to - 1);
	curl_easy_reset(c);
	curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 256L * 1024L);  // reset() clears it: re-apply per range
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
	curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 120L);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, range_data_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, range_progress_cb);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
	if (!http_ca_bundle().empty())
		curl_easy_setopt(c, CURLOPT_CAINFO, http_ca_bundle().c_str());
	http_setup_handle(c);
	if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
	CURLcode rc = curl_easy_perform(c);
	long status = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	if (hdrs) curl_slist_free_all(hdrs);
	if (rc == CURLE_OK && status == 206)
		pool_return(conn_origin(url), c);
	else
		curl_easy_cleanup(c);
	std::lock_guard<std::mutex> lock(m_);
	if (gen != seg_gen_) return;  // abandoned meanwhile
	SegRange& r = seg_ranges_[size_t(idx)];
	if (rc == CURLE_OK && status == 206 && int64_t(r.data.size()) == to - from) {
		r.done = true;
	} else {
		std::string why = status >= 400 ? "HTTP " + std::to_string(status)
		                  : errbuf[0]    ? std::string(errbuf)
		                                 : curl_easy_strerror(rc);
		mark_range_failed(idx, why);
	}
	cv_.notify_all();
}

void NetStream::mark_range_failed(int idx, const std::string& why) {
	seg_ranges_[size_t(idx)].failed = true;
	seg_failed_ = true;
	dlog("net: first-segment range %d failed (%s); falling back to one connection", idx, why.c_str());
	cv_.notify_all();
}

NetStream::SegRange* NetStream::range_for(int64_t pos) {
	for (auto& r : seg_ranges_)
		if (pos >= r.from && pos < r.to) return &r;
	return nullptr;
}

// Serves [pos_, …) from the first-segment ranges, blocking for the needed
// range. kSegFailed: the ranges failed and were abandoned; the caller uses
// the normal path (the main transfer restarts from pos_). The range is
// re-looked-up every iteration: never held across a wait.
int NetStream::read_first_seg(std::unique_lock<std::mutex>& lock, uint8_t* buf, int n) {
	for (;;) {
		if (abort_ && abort_->load()) return AVERROR_EXIT;
		if (seg_failed_ || !first_seg_active_) {
			abandon_first_segments(lock, pos_);
			return kSegFailed;
		}
		if (size_ >= 0 && pos_ >= size_) return AVERROR_EOF;
		SegRange* r = range_for(pos_);
		if (!r) {
			abandon_first_segments(lock, pos_);
			return kSegFailed;
		}
		if (r->failed) continue;  // seg_failed_ is set; the top of the loop abandons
		if (r->done) {
			size_t off = size_t(pos_ - r->from);
			if (off >= r->data.size()) {
				abandon_first_segments(lock, pos_);
				return kSegFailed;
			}
			size_t k = std::min(size_t(n), r->data.size() - off);
			memcpy(buf, r->data.data() + off, k);
			pos_ += int64_t(k);
			return int(k);
		}
		cv_.wait_for(lock, std::chrono::milliseconds(25));
	}
}

void NetStream::abandon_first_segments(std::unique_lock<std::mutex>& lock, int64_t pos) {
	(void)lock;  // m_ held by the caller
	// The range threads see seg_gen_ move and abort; the main transfer
	// restarts from pos_ (some bytes are downloaded twice on this rare path).
	first_seg_active_ = false;
	seg_gen_++;
	seg_ranges_.clear();
	seg_failed_ = false;
	buf_.clear();
	head_ = 0;
	discard_ = 0;
	pos_ = pos;
	error_.clear();
	eof_ = false;
	req_pos_ = pos;
	gen_++;
	cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Parallel mode

void NetStream::start_parallel() {  // m_ held
	// Stop the single download (it has served its purpose: the size, the
	// final URL and that ranges work) and drop what it brought.
	gen_++;
	started_gen_ = gen_;
	buf_.clear();
	head_ = 0;
	discard_ = 0;
	eof_ = false;
	parallel_ = true;
	// Pure-RAM sliding window: 64 chunks = 128 MB ahead of the reader. No
	// stream-cache-*.bin on /download0 — the disk file grew unboundedly
	// (6 GB window) and its pwrite/pread/fclose races crashed the app.
	window_ = kWindowRam;
	chunks_.assign(size_t(window_), Chunk());
	window_base_ = pos_ / kChunk;
	par_started_ = now_seconds();
	par_bytes_ = 0;
	if (final_url_.empty()) final_url_ = url_;
	for (int i = 0; i < kWorkers; i++) workers_.emplace_back([this, i] { worker(i); });
	dlog("net: %lld MB file, downloading with %d connections, up to %d MB ahead in memory", (long long)(size_ >> 20),
	     kWorkers, int(window_ * (kChunk >> 20)));
}

namespace {
struct ChunkFetch {
	std::vector<uint8_t>* data;
	size_t expect;
	std::function<void(const uint8_t*, size_t)> on_slice;
	std::function<bool()> keep_going;
};
size_t chunk_data(char* p, size_t size, size_t n, void* ud) {
	auto* f = static_cast<ChunkFetch*>(ud);
	size_t len = size * n;
	if (f->data->size() + len > f->expect) return 0;  // more than asked for: not a range answer
	f->data->insert(f->data->end(), p, p + len);
	if (f->on_slice) f->on_slice(reinterpret_cast<const uint8_t*>(p), len);
	return len;
}
int chunk_progress(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	return static_cast<ChunkFetch*>(ud)->keep_going() ? 0 : 1;
}
}  // namespace

void NetStream::worker(int idx) {
	// Stagger: all workers racing for chunk 0 simultaneously starves the
	// critical first chunk, so workers 1..n wait up to 80 ms for it to
	// become ready (state 2) before joining the pick loop.
	if (idx > 0) {
		std::unique_lock<std::mutex> stagger_lock(m_);
		cv_.wait_for(stagger_lock, std::chrono::milliseconds(80), [this] {
			return stopped() || (!chunks_.empty() && chunks_[0].state == 2);
		});
	}
	// A warm pooled handle to the stream origin; the per-chunk
	// curl_easy_reset() below keeps its connection while options are re-applied.
	CURL* c = pool_checkout(conn_origin(final_url_));
	if (!c) c = curl_easy_init();
	std::unique_lock<std::mutex> lock(m_);
	while (c && !stopped()) {
		// The first chunk of the window that nobody has or is fetching.
		double now = now_seconds();
		int64_t last = (size_ - 1) / kChunk;
		int64_t pick = -1;
		// A tail seek's chunk comes first, without moving the window — but only
		// if that slot isn't already downloading or holding the chunk ready.
		int64_t my_urgent = -1;
		if (urgent_chunk_ >= 0 && urgent_chunk_ <= last) {
			Chunk& us = chunks_[size_t(urgent_chunk_ % window_)];
			if (us.state != 1 && !(us.index == urgent_chunk_ && us.state == 2)) {
				pick = my_urgent = urgent_chunk_;
			}
			urgent_chunk_ = -1;
		}
		if (pick < 0) {
			for (int64_t i = window_base_; i < window_base_ + window_ && i <= last; i++) {
				Chunk& s = chunks_[size_t(i % window_)];
				if (s.state == 1) continue;  // busy (this chunk, or an old one still finishing)
				if (s.index == i && s.state == 2) continue;
				if (s.index == i && now < s.retry_at) continue;
				pick = i;
				break;
			}
		}
		if (pick < 0) {
			cv_.wait_for(lock, std::chrono::milliseconds(100));
			continue;
		}
		const int my_window = window_;
		const size_t si = size_t(pick % window_);
		int64_t from = pick * kChunk, to = std::min(size_, from + kChunk) - 1;
		const size_t expect_len = size_t(to - from + 1);
		{
			Chunk& slot = chunks_[si];
			if (slot.index != pick) slot.attempts = 0;
			slot.index = pick;
			slot.state = 1;
			slot.len = 0;
			slot.data.clear();
			slot.data.reserve(expect_len);  // pre-reserve: progressive appends never reallocate
		}
		std::string url = final_url_;
		lock.unlock();

		std::vector<uint8_t> data;
		data.reserve(expect_len);
		size_t flushed = 0;
		ChunkFetch f{
		    &data,
		    expect_len,
		    [this, pick, si, my_window, my_urgent, expect_len, &data, &flushed](const uint8_t*, size_t) {
			    // Stream partial data into the slot in 128KB increments, but
			    // ONLY for the playhead (or tail-seek urgent) chunk: lookahead
			    // workers flush once on completion, so this extra lock is
			    // only taken where the reader is actually waiting on it.
			    if (data.size() - flushed < (128u << 10))
				    return;
			    std::lock_guard<std::mutex> l(m_);
			    if (stopped() || window_ != my_window || chunks_[si].index != pick || chunks_[si].state != 1)
				    return;
			    if (pick != pos_ / kChunk && pick != my_urgent)
				    return;
			    Chunk& slot = chunks_[si];
			    if (slot.data.capacity() < expect_len)
				    slot.data.reserve(expect_len);
			    slot.data.insert(slot.data.end(), data.begin() + ptrdiff_t(flushed), data.end());
			    slot.len = int64_t(slot.data.size());
			    flushed = data.size();
			    cv_.notify_all();
		    },
		    [this, pick, my_window, my_urgent, &data] {
			    std::lock_guard<std::mutex> l(m_);
			    // Give up when closed, or when a seek left this chunk behind.
			    if (stopped() || window_ != my_window)
				    return false;
			    // A tail-seek chunk sits outside the window: finish it
			    // anyway, so the TCP/TLS connection stays warm for the
			    // next chunk. Likewise once a megabyte is already
			    // here, letting curl finish beats a fresh handshake.
			    if (pick == my_urgent || data.size() > (1u << 20))
				    return true;
			    return pick >= window_base_ && pick < window_base_ + window_;
		    }};
		char errbuf[CURL_ERROR_SIZE] = {0};
		struct curl_slist* hdrs = nullptr;
		for (auto& h : headers_) hdrs = curl_slist_append(hdrs, h.c_str());
		std::string range = std::to_string(from) + "-" + std::to_string(to);
		curl_easy_reset(c);
		curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 256L * 1024L);  // reset() clears it: re-apply per chunk
		curl_easy_setopt(c, CURLOPT_URL, url.c_str());
		curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
		curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
		curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 120L);
		curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, chunk_data);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, &f);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, chunk_progress);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, &f);
		if (!http_ca_bundle().empty())
			curl_easy_setopt(c, CURLOPT_CAINFO, http_ca_bundle().c_str());
		http_setup_handle(c);
		if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
		CURLcode rc = curl_easy_perform(c);
		long status = 0;
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
		if (hdrs) curl_slist_free_all(hdrs);

		lock.lock();
		if (window_ != my_window || chunks_[si].index != pick) continue;  // not ours any more
		Chunk& slot = chunks_[si];
		if (rc == CURLE_OK && status == 206 && data.size() == size_t(to - from + 1)) {
			// Pure RAM: the whole 64-chunk window fits in 128 MB, so every
			// finished chunk stays resident. No disk I/O, no unlock, no
			// generation stamps — slot is only touched under m_.
			slot.data = std::move(data);
			slot.len = int64_t(slot.data.size());
			slot.state = 2;
			par_bytes_ += to - from + 1;
			note_bytes(to - from + 1);
			log_speed(to - from + 1);
		} else {
			slot.state = 0;
			if (!stopped() && pick >= window_base_ && pick < window_base_ + window_) {
				slot.attempts++;
				slot.retry_at = now_seconds() + std::min(8, slot.attempts);
				std::string why = status >= 400 ? "HTTP " + std::to_string(status)
				                  : errbuf[0]    ? std::string(errbuf)
				                                 : curl_easy_strerror(rc);
				dlog("net: chunk %lld: %s (attempt %d)", (long long)pick, why.c_str(), slot.attempts);
				if (slot.attempts >= kChunkAttempts || (status >= 400 && status < 500)) error_ = why;
			}
		}
		cv_.notify_all();
	}
	lock.unlock();
	if (c) {
		if (!stopped())
			pool_return(conn_origin(final_url_), c);  // warm: reusable for the next stream
		else
			curl_easy_cleanup(c);
	}
}

int NetStream::read_parallel(std::unique_lock<std::mutex>& lock, uint8_t* buf, int n) {
	for (;;) {
		if (abort_ && abort_->load()) return AVERROR_EXIT;
		if (pos_ >= size_) return AVERROR_EOF;
		int64_t idx = pos_ / kChunk;
		// The window follows the reader, one chunk kept behind it for short
		// steps back; a jump outside it starts a new one there. A tail seek
		// (MP4/MKV moov at the end of the file) instead asks for just that
		// chunk, leaving the window where it is so the download connections
		// stay alive. For small files the window on disk covers the whole
		// file, so the old "outside the window" test is never true; instead
		// a tail seek is detected by position: the window is still at the
		// head (window_base_ <= 2) while the reader jumps to the last chunks
		// (the moov). The urgent flag is only set when the slot doesn't
		// already hold that chunk downloading or ready.
		int64_t last = (size_ - 1) / kChunk;
		if (window_base_ <= 2 && idx >= std::max<int64_t>(4, last - 2)) {
			Chunk& us = chunks_[size_t(idx % window_)];
			if (us.index != idx || us.state == 0) {
				urgent_chunk_ = idx;
				cv_.notify_one();
			}
		} else {
			int64_t base =
			    idx < window_base_ || idx >= window_base_ + window_ ? idx : std::max(window_base_, idx - 1);
			if (base != window_base_) {
				window_base_ = base;
				// Free RAM from chunks that fell behind the window (one
				// chunk is kept behind for short steps back). state==2
				// only, and reset to 0 so the reader never serves an
				// emptied slot and a later seek re-downloads it without
				// stale backoff delay.
				for (size_t ci = 0; ci < chunks_.size(); ci++) {
					Chunk& c = chunks_[ci];
					if (c.index < base && c.state == 2) {
						c.data.clear();
						c.data.shrink_to_fit();
						c.state = 0;
						c.attempts = 0;
						c.retry_at = 0;
					}
				}
				cv_.notify_all();
			}
		}
		size_t si = size_t(idx % window_);
		Chunk& s = chunks_[si];
		size_t off = size_t(pos_ - idx * kChunk);
		if (s.index == idx && (s.state == 2 || (s.state == 1 && !s.data.empty() && size_t(s.len) > off))) {
			size_t k = std::min(size_t(n), size_t(s.len) - off);
			// Pure RAM: every ready chunk is resident, served under m_.
			memcpy(buf, s.data.data() + off, k);
			pos_ += int64_t(k);
			return int(k);
		}
		if (!error_.empty()) return AVERROR(EIO);
		cv_.wait_for(lock, std::chrono::milliseconds(25));
	}
}

// ---------------------------------------------------------------------------
// Download thread

void NetStream::run() {
	std::unique_lock<std::mutex> lock(m_);
	while (!stop_) {
		cv_.wait(lock, [this] { return stop_ || gen_ != started_gen_; });
		if (stop_) break;
		int gen = gen_;
		int64_t from = req_pos_;
		if (eof_ && size_ >= 0 && from >= size_) {
			started_gen_ = gen;  // fully prefetched: nothing to download
			continue;
		}
		started_gen_ = gen;
		lock.unlock();
		transfer(from, gen);
		lock.lock();
	}
}

bool NetStream::transfer(int64_t from, int gen) {
	// Reuse an idle connection to the same origin (HLS segments, seeks);
	// curl_easy_reset() keeps the pooled connection, options re-applied below.
	const std::string origin = conn_origin(url_);
	CURL* c = pool_checkout(origin);
	if (!c) c = curl_easy_init();
	if (!c) {
		std::lock_guard<std::mutex> lock(m_);
		error_ = "curl_easy_init failed";
		cv_.notify_all();
		return false;
	}
	for (int attempt = 0;; attempt++) {
		curl_easy_reset(c);
		curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 256L * 1024L);  // reset() clears it: re-apply per attempt
		{
			std::lock_guard<std::mutex> lock(m_);
			cur_gen_ = gen;
			header_ok_ = false;
			status_ = 0;
		}
		char errbuf[CURL_ERROR_SIZE] = {0};
		struct curl_slist* hdrs = nullptr;
		for (auto& h : headers_) hdrs = curl_slist_append(hdrs, h.c_str());
		std::string range = std::to_string(from) + "-";
		curl_easy_setopt(c, CURLOPT_URL, url_.c_str());
		curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
		curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
		curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
		// A torrent with few seeders can go quiet for minutes; reconnect
		// only after three minutes without any data.
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 180L);
		curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
		curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
		curl_easy_setopt(c, CURLOPT_HEADERDATA, this);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, this);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, this);
		if (!http_ca_bundle().empty())
			curl_easy_setopt(c, CURLOPT_CAINFO, http_ca_bundle().c_str());
		http_setup_handle(c);
		if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
		{
			std::lock_guard<std::mutex> lock(m_);
			discard_ = 0;
			range_from_ = from;
			cur_curl_ = c;
		}
		CURLcode rc = curl_easy_perform(c);
		{
			std::lock_guard<std::mutex> lock(m_);
			cur_curl_ = nullptr;
		}
		long status = 0;
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
		if (hdrs) curl_slist_free_all(hdrs);

		std::unique_lock<std::mutex> lock(m_);
		if (stop_ || gen != gen_) {
			lock.unlock();
			pool_return(origin, c);  // keep the connection for the next request
			return true;             // closed or seeked meanwhile
		}
		if (rc == CURLE_OK && status >= 200 && status < 300) {
			eof_ = true;
			cv_.notify_all();
			lock.unlock();
			pool_return(origin, c);
			return true;
		}
		if (status >= 400 && status < 500) {
			error_ = "HTTP " + std::to_string(status);
			dlog("net: %s -> %s", log_url(url_).c_str(), error_.c_str());
			cv_.notify_all();
			lock.unlock();
			pool_return(origin, c);
			return false;
		}
		// 502/503/504: the proxy in front of the streaming server gave up
		// waiting (a torrent still looking for its first pieces). Not ready
		// yet, so ask again, as the PC app's player does.
		std::string why = status >= 500 ? "HTTP " + std::to_string(status) + " (server still preparing)"
		                                : (errbuf[0] ? errbuf : curl_easy_strerror(rc));
		if (attempt >= kRetries || (abort_ && abort_->load())) {
			error_ = why;
			dlog("net: %s -> %s (gave up)", log_url(url_).c_str(), why.c_str());
			cv_.notify_all();
			lock.unlock();
			pool_return(origin, c);
			return false;
		}
		// Dropped connection: carry on from where the data ends.
		from = pos_ + int64_t(buf_.size() - head_) + discard_;
		dlog("net: %s -> %s; reconnecting at %lld", log_url(url_).c_str(), why.c_str(), (long long)from);
		cv_.wait_for(lock, std::chrono::seconds(1), [&] { return stop_ || gen != gen_; });
		if (stop_ || gen != gen_) {
			lock.unlock();
			pool_return(origin, c);
			return true;
		}
	}
}

size_t NetStream::on_header(char* data, size_t size, size_t n, void* self) {
	auto* s = static_cast<NetStream*>(self);
	std::string line(data, size * n);
	std::lock_guard<std::mutex> lock(s->m_);
	if (starts_with(line, "HTTP/")) {
		size_t sp = line.find(' ');
		s->status_ = sp == std::string::npos ? 0 : std::atol(line.c_str() + sp + 1);
		return size * n;
	}
	std::string low = lower(line);
	if (starts_with(low, "content-range:")) {
		size_t slash = low.find('/');
		if (slash != std::string::npos && low[slash + 1] != '*') s->size_ = std::atoll(low.c_str() + slash + 1);
	} else if (starts_with(low, "content-length:") && s->status_ == 200 && s->size_ < 0) {
		s->size_ = std::atoll(low.c_str() + 15);
	}
	if (line == "\r\n" || line == "\n") {
		// End of one response's headers; redirects come before the real one.
		if (s->status_ >= 200 && s->status_ < 300) {
			s->header_ok_ = true;
			s->headers_done_ = true;
			char* eff = nullptr;
			if (s->cur_curl_ && curl_easy_getinfo(static_cast<CURL*>(s->cur_curl_), CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK &&
			    eff)
				s->final_url_ = eff;
			// 200 instead of 206: the server ignored Range and sends the
			// whole file, so drop everything before where we asked to start.
			if (s->status_ == 200 && s->range_from_ > 0) s->discard_ = s->range_from_;
			// Normal-path cache fill: the single connection's header probe
			// already paid the RTT, so feed the prewarm metadata cache too
			// (Play-Next preresolves at ~80% of an episode, 5-9 min before
			// the next start(), which would otherwise find a cold cache).
			// Refresh-or-push, capped at 16 entries, oldest dropped.
			if (s->status_ == 206 && s->size_ > 0) {
				PrewarmMeta pm{s->url_, s->final_url_.empty() ? s->url_ : s->final_url_, s->size_, 206, now_seconds()};
				std::lock_guard<std::mutex> pl(g_prewarm_m);
				bool found = false;
				for (auto& e : g_prewarm_meta)
					if (e.url == s->url_) {
						e = pm;
						found = true;
						break;
					}
				if (!found) {
					g_prewarm_meta.push_back(pm);
					while (g_prewarm_meta.size() > 16) g_prewarm_meta.erase(g_prewarm_meta.begin());
				}
			}
			s->cv_.notify_all();
		}
	}
	return size * n;
}

size_t NetStream::on_data(char* data, size_t size, size_t n, void* self) {
	auto* s = static_cast<NetStream*>(self);
	size_t len = size * n;
	std::unique_lock<std::mutex> lock(s->m_);
	if (s->stop_ || s->cur_gen_ != s->gen_) return 0;  // closed or seeked: abort this transfer
	if (!s->header_ok_) return size * n;               // a redirect's body
	if (s->discard_ > 0) {
		size_t drop = size_t(std::min<int64_t>(s->discard_, int64_t(len)));
		s->discard_ -= int64_t(drop);
		data += drop;
		len -= drop;
		if (len == 0) return size * n;
	}
	// Read-ahead full: wait for the player to catch up.
	while (s->buf_.size() - s->head_ >= kMaxBuffered) {
		if (s->stop_ || s->cur_gen_ != s->gen_ || (s->abort_ && s->abort_->load())) return 0;
		s->cv_.wait_for(lock, std::chrono::milliseconds(100));
	}
	if (s->cur_gen_ != s->gen_) return 0;
	if (s->head_ > (8u << 20) && s->head_ * 2 > s->buf_.size()) {
		s->buf_.erase(s->buf_.begin(), s->buf_.begin() + s->head_);
		s->head_ = 0;
	}
	s->buf_.insert(s->buf_.end(), data, data + len);
	s->log_speed(int64_t(len));
	s->note_bytes(int64_t(len));
	s->cv_.notify_all();
	return size * n;
}

int NetStream::on_progress(void* self, int64_t, int64_t, int64_t, int64_t) {
	auto* s = static_cast<NetStream*>(self);
	std::lock_guard<std::mutex> lock(s->m_);
	return (s->stop_ || s->cur_gen_ != s->gen_ || (s->abort_ && s->abort_->load())) ? 1 : 0;
}

size_t NetStream::range_data_cb(char* p, size_t size, size_t n, void* ud) {
	auto* ctx = static_cast<RangeCtx*>(ud);
	NetStream* s = ctx->s;
	size_t len = size * n;
	std::lock_guard<std::mutex> lock(s->m_);
	if (ctx->gen != s->seg_gen_ || s->stopped()) return 0;  // abandoned: abort
	SegRange& r = s->seg_ranges_[size_t(ctx->idx)];
	if (int64_t(r.data.size() + len) > r.to - r.from) return 0;  // more than asked: not a range answer
	r.data.insert(r.data.end(), p, p + len);
	s->note_bytes(int64_t(len));
	s->cv_.notify_all();
	return len;
}

int NetStream::range_progress_cb(void* ud, int64_t, int64_t, int64_t, int64_t) {
	auto* ctx = static_cast<RangeCtx*>(ud);
	NetStream* s = ctx->s;
	std::lock_guard<std::mutex> lock(s->m_);
	return (ctx->gen != s->seg_gen_ || s->stopped()) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Reading and seeking (the demuxer's thread)

int NetStream::read(uint8_t* buf, int n) {
	std::unique_lock<std::mutex> lock(m_);
	if (parallel_) return read_parallel(lock, buf, n);
	if (first_seg_active_ && pos_ < first_seg_end_) {
		int k = read_first_seg(lock, buf, n);
		if (k != kSegFailed) return k;
		// The ranges failed and were abandoned; the main transfer restarts
		// from pos_ below.
	}
	return read_normal(lock, buf, n);
}

int NetStream::read_normal(std::unique_lock<std::mutex>& lock, uint8_t* buf, int n) {
	for (;;) {
		if (abort_ && abort_->load()) return AVERROR_EXIT;
		size_t avail = buf_.size() - head_;
		if (avail > 0) {
			size_t k = std::min(avail, size_t(n));
			memcpy(buf, buf_.data() + head_, k);
			head_ += k;
			pos_ += int64_t(k);
			if (head_ == buf_.size()) buf_.clear(), head_ = 0;
			cv_.notify_all();
			return int(k);
		}
		if (!error_.empty()) return AVERROR(EIO);
		if (eof_) return AVERROR_EOF;
		cv_.wait_for(lock, std::chrono::milliseconds(25));
	}
}

std::string NetStream::peek(size_t n) {
	std::unique_lock<std::mutex> lock(m_);
	if (parallel_) {
		std::string out(n, '\0');
		int64_t keep = pos_;
		size_t got = 0;
		while (got < n) {
			int k = read_parallel(lock, reinterpret_cast<uint8_t*>(&out[got]), int(n - got));
			if (k <= 0) break;
			got += size_t(k);
		}
		pos_ = keep;
		out.resize(got);
		return out;
	}
	if (first_seg_active_ && pos_ < first_seg_end_) {
		// The container sniff (a few bytes at the head), without consuming.
		// The range is re-looked-up every iteration: never held across a wait.
		std::string out;
		int64_t p = pos_;
		while (out.size() < n && first_seg_active_ && p < first_seg_end_) {
			if (abort_ && abort_->load()) break;
			if (seg_failed_) {
				abandon_first_segments(lock, pos_);
				break;
			}
			if (size_ >= 0 && p >= size_) break;
			SegRange* r = range_for(p);
			if (!r) break;
			if (r->failed) continue;  // seg_failed_ is set; the top of the loop abandons
			if (!r->done) {
				cv_.wait_for(lock, std::chrono::milliseconds(25));
				continue;
			}
			size_t off = size_t(p - r->from);
			if (off >= r->data.size()) break;
			size_t k = std::min(n - out.size(), r->data.size() - off);
			out.append(reinterpret_cast<const char*>(r->data.data() + off), k);
			p += int64_t(k);
		}
		if (first_seg_active_) return out;
		// Abandoned above: fall through to the normal path (the main
		// transfer restarts from pos_).
	}
	while (buf_.size() - head_ < n && error_.empty() && !eof_ && !(abort_ && abort_->load()))
		cv_.wait_for(lock, std::chrono::milliseconds(25));
	size_t k = std::min(n, buf_.size() - head_);
	return std::string(reinterpret_cast<const char*>(buf_.data() + head_), k);
}

std::string NetStream::failure() {
	std::lock_guard<std::mutex> lock(m_);
	return error_;
}

// Throughput accounting (Worker 9): every byte on every connection feeds
// one counter, so the session's Auto-quality decision rides on bytes already
// downloaded — zero extra traffic.
void NetStream::note_bytes(int64_t n) {  // m_ held
	double now = now_seconds();
	if (tput_start_ == 0) tput_start_ = now;
	tput_bytes_ += n;
	last_data_at_ = now;
}

double NetStream::measured_bps() const {
	std::lock_guard<std::mutex> lock(m_);
	if (tput_start_ == 0) return -1;
	double el = now_seconds() - tput_start_;
	if (el < 1.0) return -1;
	return double(tput_bytes_) / el;
}

double NetStream::seconds_since_data() const {
	std::lock_guard<std::mutex> lock(m_);
	if (last_data_at_ == 0) return -1;
	return now_seconds() - last_data_at_;
}

void NetStream::seed_prefetch(std::vector<uint8_t>&& data, int64_t total_size, const std::string& final_url) {
	std::lock_guard<std::mutex> lock(m_);
	buf_ = std::move(data);
	head_ = 0;
	pos_ = 0;
	size_ = total_size;
	final_url_ = final_url;
	status_ = 206;
	headers_done_ = true;
	prefetch_seeded_ = true;
	if (size_ >= 0 && int64_t(buf_.size()) >= size_) eof_ = true;
	dlog("net: %lld prefetched bytes ready", (long long)buf_.size());
}

// Urgent prefetch of one chunk (parallel mode): the next-episode or
// chapter-skip hint asks for the chunk at byte_offset now, without moving
// the read-ahead window. Mirrors the tail-seek urgent logic in
// read_parallel: the flag is only set when the slot doesn't already hold
// that chunk downloading or ready.
void NetStream::prewarm_offset(int64_t byte_offset) {
	std::lock_guard<std::mutex> l(m_);
	if (!parallel_ || size_ <= 0) return;
	int64_t idx = std::clamp(byte_offset / kChunk, int64_t(0), (size_ - 1) / kChunk);
	auto& slot = chunks_[size_t(idx % window_)];
	if (slot.index != idx || slot.state == 0) {
		urgent_chunk_ = idx;
		cv_.notify_one();
	}
}

void NetStream::prefetch_start(const std::string& url, const std::vector<std::string>& headers) {
	if (!is_http(url.c_str())) return;
	int gen;
	{
		std::lock_guard<std::mutex> lock(g_prefetch.m);
		g_prefetch.gen++;
		gen = g_prefetch.gen;
		g_prefetch.cancelled.store(true);  // supersede any running prefetch
		g_prefetch.url = url;
		g_prefetch.headers = headers;
		g_prefetch.data.clear();
		g_prefetch.total = -1;
		g_prefetch.final_url.clear();
		g_prefetch.rh = RangeHeaders();
		g_prefetch.started_at = now_seconds();
		g_prefetch.cancelled.store(false);
	}
	// Detached and flag-driven: superseding never blocks.
	std::thread([url, headers, gen] {
		PrefetchGen pg{gen};
		CURL* c = curl_easy_init();
		if (!c) return;
		char errbuf[CURL_ERROR_SIZE] = {0};
		struct curl_slist* hdrs = nullptr;
		for (auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());
		curl_easy_setopt(c, CURLOPT_URL, url.c_str());
		curl_easy_setopt(c, CURLOPT_RANGE, ("0-" + std::to_string(kPrefetchBytes - 1)).c_str());
		curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
		curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
		curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
		curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, prefetch_header_cb);
		curl_easy_setopt(c, CURLOPT_HEADERDATA, &pg);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, prefetch_data_cb);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, &pg);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, prefetch_progress_cb);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pg);
		if (!http_ca_bundle().empty())
			curl_easy_setopt(c, CURLOPT_CAINFO, http_ca_bundle().c_str());
		http_setup_handle(c);
		if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
		CURLcode rc = curl_easy_perform(c);
		char* eff = nullptr;
		curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
		if (hdrs) curl_slist_free_all(hdrs);
		curl_easy_cleanup(c);
		std::lock_guard<std::mutex> lock(g_prefetch.m);
		if (pg.gen != g_prefetch.gen || g_prefetch.cancelled.load()) return;  // superseded
		if (eff) g_prefetch.final_url = eff;
		if (g_prefetch.rh.size > 0) g_prefetch.total = g_prefetch.rh.size;
		// Keep what arrived, even partial: the player takes it as a head
		// start. An empty failure leaves nothing to take.
		dlog("net: prefetched %lld bytes of %s (%s)", (long long)g_prefetch.data.size(),
		     log_url(url).c_str(), rc == CURLE_OK ? "ok" : curl_easy_strerror(rc));
	}).detach();
}

void NetStream::prefetch_cancel() {
	std::lock_guard<std::mutex> lock(g_prefetch.m);
	g_prefetch.gen++;
	g_prefetch.cancelled.store(true);
	g_prefetch.url.clear();
	g_prefetch.data.clear();
}

bool NetStream::prefetch_take(const std::string& url, std::vector<uint8_t>* data, int64_t* total_size,
                              std::string* final_url) {
	std::lock_guard<std::mutex> lock(g_prefetch.m);
	if (g_prefetch.url != url || g_prefetch.data.empty()) return false;
	if (now_seconds() - g_prefetch.started_at > kPrefetchMaxAge) {
		g_prefetch.cancelled.store(true);
		g_prefetch.url.clear();
		g_prefetch.data.clear();
		return false;
	}
	*data = std::move(g_prefetch.data);
	if (total_size) *total_size = g_prefetch.total;
	if (final_url) *final_url = g_prefetch.final_url.empty() ? url : g_prefetch.final_url;
	g_prefetch.cancelled.store(true);  // single use: stop the worker
	g_prefetch.url.clear();
	g_prefetch.data.clear();
	return true;
}

NetStream* NetStream::of(AVIOContext* pb) { return is_ours(pb) ? static_cast<NetStream*>(pb->opaque) : nullptr; }

int64_t NetStream::seek(int64_t offset, int whence) {
	std::unique_lock<std::mutex> lock(m_);
	if (whence & AVSEEK_SIZE) return size_ >= 0 ? size_ : AVERROR(ENOSYS);
	whence &= ~AVSEEK_FORCE;
	int64_t target;
	if (whence == SEEK_SET) target = offset;
	else if (whence == SEEK_CUR) target = pos_ + offset;
	else if (whence == SEEK_END && size_ >= 0) target = size_ + offset;
	else return AVERROR(EINVAL);
	if (target < 0) return AVERROR(EINVAL);
	if (first_seg_active_) {
		if (target < first_seg_end_) {  // inside the fetched head: just move
			pos_ = target;
			cv_.notify_all();
			return target;
		}
		// Leaving the head: the range threads stop being useful.
		first_seg_active_ = false;
		seg_gen_++;
		seg_ranges_.clear();
		seg_failed_ = false;
		cv_.notify_all();
	}
	if (parallel_) {  // the next read moves the window there
		pos_ = target;
		cv_.notify_all();
		return target;
	}

	int64_t avail = int64_t(buf_.size() - head_);
	int64_t end = pos_ + avail;  // first byte not yet downloaded
	if (target >= pos_ && target <= end) {  // already here
		head_ += size_t(target - pos_);
		pos_ = target;
		cv_.notify_all();
		return target;
	}
	if (target > end && target - end < kSkipWindow && error_.empty() && !eof_) {
		// A little further on: keep the connection and drop what's between.
		buf_.clear();
		head_ = 0;
		discard_ += target - end;
		pos_ = target;
		cv_.notify_all();
		return target;
	}
	// Anywhere else: a new request from there.
	dlog("net: jump to %lld of %lld (new request)", (long long)target, (long long)size_);
	buf_.clear();
	head_ = 0;
	discard_ = 0;
	pos_ = target;
	error_.clear();
	gen_++;
	if (size_ >= 0 && target >= size_) {
		eof_ = true;
		started_gen_ = gen_;  // nothing to download
	} else {
		eof_ = false;
		req_pos_ = target;
	}
	cv_.notify_all();
	return target;
}

// ---------------------------------------------------------------------------
// FFmpeg glue

AVIOContext* NetStream::open_avio(const std::string& url, const std::vector<std::string>& headers,
                                  const std::atomic<bool>* abort, std::string* error, bool parallel) {
	auto* ns = new NetStream(url, headers, abort, parallel);
	// A prefetched head start for this exact URL (next-episode prefetch):
	// the first bytes are already here. Single-connection streams seed
	// from it. Parallel streams take none: a partial in-flight prefetch
	// (total==-1) would poison seed_prefetch with headers_done_/206 +
	// size_==-1 and start_parallel() would never launch. `total > 0`
	// hardens even the single-connection path against that race.
	if (!parallel) {
		std::vector<uint8_t> pre; int64_t total = -1; std::string final_url;
		if (prefetch_take(url, &pre, &total, &final_url) && !pre.empty() && total > 0)
			ns->seed_prefetch(std::move(pre), total, final_url);
	} else {
		prefetch_cancel();
	}
	if (!ns->start()) {
		if (error) *error = ns->error();
		delete ns;
		return nullptr;
	}
	const int kBufSize = 256 * 1024;
	auto* buf = static_cast<uint8_t*>(av_malloc(kBufSize));
	AVIOContext* pb = buf ? avio_alloc_context(buf, kBufSize, 0, ns, read_cb, nullptr, seek_cb) : nullptr;
	if (!pb) {
		av_free(buf);
		delete ns;
		if (error) *error = "out of memory";
		return nullptr;
	}
	pb->seekable = ns->size() > 0 ? AVIO_SEEKABLE_NORMAL : 0;
	dlog("net: opened %s (%lld bytes)", log_url(url).c_str(), (long long)ns->size());
	return pb;
}

bool NetStream::is_ours(AVIOContext* pb) { return pb && pb->read_packet == read_cb; }

void NetStream::close_avio(AVIOContext** pb) {
	if (!pb || !*pb) return;
	delete static_cast<NetStream*>((*pb)->opaque);
	av_freep(&(*pb)->buffer);
	avio_context_free(pb);
}

void NetStream::install(AVFormatContext* fmt, IoHooks* hooks) {
	hooks->default_open = fmt->io_open;
	hooks->default_close = fmt->io_close2;
	fmt->opaque = hooks;
	fmt->io_open = io_open_cb;
	fmt->io_close2 = io_close_cb;
}
