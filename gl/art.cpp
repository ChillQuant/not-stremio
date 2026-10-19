#include "art.hpp"

#include "http.h"
#include "tasks.h"
#include "util.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// The picture codecs are private to this file.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#ifdef HAVE_WEBP
#include <webp/decode.h>
#endif

namespace sx
{

namespace
{

constexpr int kPosterW = 480, kPosterH = 720;
// A wallpaper is cut between these widths: never narrower than a 1080p screen (a smaller picture is
// scaled up here, with a good filter, not left to the GPU), and no wider than 2880 (a 4K picture is
// 33 MB on the GPU, 2880 is 19 MB and still sharp on a 4K television).
constexpr int kBackdropMin = 1280, kBackdropMax = 1920;
constexpr int kThumbW = 320;
constexpr int kIconSize = 192;
constexpr int kQrMin = 400;
// Priority boost (subtracted) for jobs whose file is already on disk: a
// local decode beats any download.
constexpr float kDiskPriorityBoost = 50.0f;
// A title logo is only ever shrunk: it keeps its own aspect on transparency,
// at most this wide (the hero draws it at 420).
constexpr int kLogoW = 560;

// PNG and JPEG through stb_image; WebP (some addons serve posters as it) through libwebp.
// Hard dimension cap, checked BEFORE any pixel allocation: a hostile file
// claiming gigapixel dimensions would otherwise OOM the art worker and the
// uncaught std::bad_alloc would terminate the process.
constexpr int kDecodeMaxDim = 8000;

bool decode(const std::string &data, std::vector<unsigned char> &px, int &w, int &h)
{
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(data.data());
#ifdef HAVE_WEBP
    if (data.size() >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WEBP", 4) == 0)
    {
        if (!WebPGetInfo(bytes, data.size(), &w, &h) || w <= 0 || h <= 0 ||
            w > kDecodeMaxDim || h > kDecodeMaxDim)
            return false;
        uint8_t *out = WebPDecodeRGBA(bytes, data.size(), &w, &h);
        if (!out)
            return false;
        px.assign(out, out + static_cast<std::size_t>(w) * h * 4);
        WebPFree(out);
        return true;
    }
#endif
    int comp = 0;
    int iw = 0, ih = 0, ic = 0;
    if (!stbi_info_from_memory(bytes, static_cast<int>(data.size()), &iw, &ih, &ic) ||
        iw <= 0 || ih <= 0 || iw > kDecodeMaxDim || ih > kDecodeMaxDim)
        return false;
    unsigned char *out = stbi_load_from_memory(bytes, static_cast<int>(data.size()), &w, &h, &comp, 4);
    if (!out)
        return false;
    px.assign(out, out + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(out);
    return true;
}

// ---- Lanczos-3 resampling (premultiplied, so transparent edges do not fringe) ----

float lanczos3(float x)
{
    x = std::fabs(x);
    if (x < 1e-6f)
        return 1.0f;
    if (x >= 3.0f)
        return 0.0f;
    const float px = 3.14159265f * x;
    return 3.0f * std::sin(px) * std::sin(px / 3.0f) / (px * px);
}

struct Taps
{
    int first = 0;
    std::vector<float> w;
};

std::vector<Taps> make_taps(int n_out, int n_in, float start, float span)
{
    const float scale = span / static_cast<float>(n_out), wide = std::max(1.0f, scale);
    std::vector<Taps> taps(static_cast<std::size_t>(n_out));
    for (int i = 0; i < n_out; ++i)
    {
        const float c = start + (static_cast<float>(i) + 0.5f) * scale - 0.5f;
        int lo = std::max(static_cast<int>(std::floor(c - 3 * wide)) + 1, 0);
        const int hi = std::min(static_cast<int>(std::floor(c + 3 * wide)), n_in - 1);
        Taps &t = taps[static_cast<std::size_t>(i)];
        if (hi < lo)
        {
            lo = std::min(std::max(static_cast<int>(c + 0.5f), 0), n_in - 1);
            t.first = lo;
            t.w.assign(1, 1.0f);
            continue;
        }
        t.first = lo;
        t.w.resize(static_cast<std::size_t>(hi - lo + 1));
        float sum = 0;
        for (int k = lo; k <= hi; ++k)
            sum += t.w[static_cast<std::size_t>(k - lo)] = lanczos3((static_cast<float>(k) - c) / wide);
        if (sum <= 0)
        {
            t.w.assign(t.w.size(), 1.0f);
            sum = static_cast<float>(t.w.size());
        }
        for (float &v : t.w)
            v /= sum;
    }
    return taps;
}

// The source rectangle (sx, sy, sw, sh) into dw x dh.
void resample(const unsigned char *src, int src_w, int src_h, float sx, float sy, float sw, float sh,
              unsigned char *dst, int dw, int dh)
{
    const std::vector<Taps> xt = make_taps(dw, src_w, sx, sw), yt = make_taps(dh, src_h, sy, sh);
    int row0 = src_h, row1 = -1;
    for (const Taps &t : yt)
    {
        row0 = std::min(row0, t.first);
        row1 = std::max(row1, t.first + static_cast<int>(t.w.size()) - 1);
    }
    static thread_local std::vector<float> tls_mid;
    const std::size_t needed = static_cast<std::size_t>(row1 - row0 + 1) * dw * 4;
    if (tls_mid.size() < needed)
        tls_mid.resize(needed);
    float *mid = tls_mid.data();
    for (int r = row0; r <= row1; ++r)
    {
        const unsigned char *line = src + static_cast<std::size_t>(r) * src_w * 4;
        float *out = &mid[static_cast<std::size_t>(r - row0) * dw * 4];
        for (int x = 0; x < dw; ++x, out += 4)
        {
            const Taps &t = xt[static_cast<std::size_t>(x)];
            float acc[4] = {0, 0, 0, 0};
            const unsigned char *s = line + static_cast<std::size_t>(t.first) * 4;
            for (std::size_t k = 0; k < t.w.size(); ++k, s += 4)
            {
                const float a = static_cast<float>(s[3]) * (1.0f / 255.0f), w = t.w[k];
                acc[0] += s[0] * a * w;
                acc[1] += s[1] * a * w;
                acc[2] += s[2] * a * w;
                acc[3] += a * w;
            }
            std::memcpy(out, acc, sizeof(acc));
        }
    }
    for (int y = 0; y < dh; ++y)
    {
        const Taps &t = yt[static_cast<std::size_t>(y)];
        for (int x = 0; x < dw; ++x)
        {
            float acc[4] = {0, 0, 0, 0};
            for (std::size_t k = 0; k < t.w.size(); ++k)
            {
                const float *m = &mid[(static_cast<std::size_t>(t.first + static_cast<int>(k) - row0) * dw + x) * 4];
                const float w = t.w[k];
                acc[0] += m[0] * w;
                acc[1] += m[1] * w;
                acc[2] += m[2] * w;
                acc[3] += m[3] * w;
            }
            unsigned char *d = dst + (static_cast<std::size_t>(y) * dw + x) * 4;
            const float a = std::min(std::max(acc[3], 0.0f), 1.0f);
            if (a > 1e-4f)
            {
                const float inv = 1.0f / a;
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<unsigned char>(std::min(std::max(acc[c] * inv, 0.0f) + 0.5f, 255.0f));
            }
            else
            {
                d[0] = d[1] = d[2] = 0;
            }
            d[3] = static_cast<unsigned char>(a * 255.0f + 0.5f);
        }
    }
}

// Fills the aspect aw:ah from the middle of the picture, between min_w and max_w wide (with min_w 0 a
// small picture is not scaled up: it is left to the GPU's smooth scaling).
void cover(const std::vector<unsigned char> &src, int w, int h, int aw, int ah, int max_w, std::vector<unsigned char> &out,
           int &ow, int &oh, int min_w = 0)
{
    float sx = 0, sy = 0, sw = static_cast<float>(w), sh = static_cast<float>(h);
    const float target = static_cast<float>(aw) / static_cast<float>(ah), aspect = sw / sh;
    if (aspect > target)
    {
        sw = sh * target;
        sx = (static_cast<float>(w) - sw) * 0.5f;
    }
    else
    {
        sh = sw / target;
        sy = (static_cast<float>(h) - sh) * 0.5f;
    }
    ow = std::max(std::max(1, min_w), std::min(max_w, static_cast<int>(sw)));
    oh = std::max(1, static_cast<int>(std::lround(static_cast<float>(ow) * static_cast<float>(ah) / static_cast<float>(aw))));
    out.resize(static_cast<std::size_t>(ow) * oh * 4);
    resample(src.data(), w, h, sx, sy, sw, sh, out.data(), ow, oh);
}

// A picture that was scaled up has lost its edges: a mild unsharp mask (a [1 2 1] blur, taken off
// the picture and a part of it put back) brings them back without a halo.
void sharpen(std::vector<unsigned char> &px, int w, int h, float amount)
{
    std::vector<unsigned char> blur(px.size());
    std::vector<unsigned char> tmp(px.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int xl = std::max(0, x - 1), xr = std::min(w - 1, x + 1);
            for (int c = 0; c < 3; ++c)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4 + static_cast<std::size_t>(c);
                tmp[i] = static_cast<unsigned char>((px[(static_cast<std::size_t>(y) * w + xl) * 4 + static_cast<std::size_t>(c)] +
                                                      2 * px[i] +
                                                      px[(static_cast<std::size_t>(y) * w + xr) * 4 + static_cast<std::size_t>(c)] + 2) /
                                                     4);
            }
        }
    for (int y = 0; y < h; ++y)
    {
        const int yu = std::max(0, y - 1), yd = std::min(h - 1, y + 1);
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4 + static_cast<std::size_t>(c);
                blur[i] = static_cast<unsigned char>((tmp[(static_cast<std::size_t>(yu) * w + x) * 4 + static_cast<std::size_t>(c)] +
                                                       2 * tmp[i] +
                                                       tmp[(static_cast<std::size_t>(yd) * w + x) * 4 + static_cast<std::size_t>(c)] + 2) /
                                                      4);
            }
    }
    for (std::size_t i = 0; i < px.size(); i += 4)
        for (int c = 0; c < 3; ++c)
        {
            const float v = static_cast<float>(px[i + static_cast<std::size_t>(c)]);
            const float b = static_cast<float>(blur[i + static_cast<std::size_t>(c)]);
            px[i + static_cast<std::size_t>(c)] = static_cast<unsigned char>(std::clamp(v + amount * (v - b), 0.0f, 255.0f));
        }
}

// The whole picture inside a square on transparency.
void fit_square(const std::vector<unsigned char> &src, int w, int h, int size, std::vector<unsigned char> &out)
{
    const float aspect = static_cast<float>(w) / static_cast<float>(h);
    int fw = size, fh = size;
    if (aspect > 1.0f)
        fh = std::max(1, static_cast<int>(std::lround(static_cast<float>(size) / aspect)));
    else
        fw = std::max(1, static_cast<int>(std::lround(static_cast<float>(size) * aspect)));
    std::vector<unsigned char> fitted(static_cast<std::size_t>(fw) * fh * 4);
    resample(src.data(), w, h, 0, 0, static_cast<float>(w), static_cast<float>(h), fitted.data(), fw, fh);
    out.assign(static_cast<std::size_t>(size) * size * 4, 0);
    const int ox = (size - fw) / 2, oy = (size - fh) / 2;
    for (int y = 0; y < fh; ++y)
        std::memcpy(&out[(static_cast<std::size_t>(y + oy) * size + ox) * 4], &fitted[static_cast<std::size_t>(y) * fw * 4],
                    static_cast<std::size_t>(fw) * 4);
}

// Shrink to at most max_w wide, keeping the aspect; smaller pictures are left
// alone. No crop, no letterbox: the alpha stays exactly the artwork's own.
void fit_width(const std::vector<unsigned char> &src, int w, int h, int max_w, std::vector<unsigned char> &out,
              int &ow, int &oh)
{
    const float scale = std::min(1.0f, static_cast<float>(max_w) / static_cast<float>(w));
    ow = std::max(1, static_cast<int>(std::lround(static_cast<float>(w) * scale)));
    oh = std::max(1, static_cast<int>(std::lround(static_cast<float>(h) * scale)));
    out.assign(static_cast<std::size_t>(ow) * oh * 4, 0);
    resample(src.data(), w, h, 0, 0, static_cast<float>(w), static_cast<float>(h), out.data(), ow, oh);
}

void to_string_sink(void *context, void *data, int size)
{
    static_cast<std::string *>(context)->append(static_cast<const char *>(data), static_cast<std::size_t>(size));
}

// How far apart two pictures are (0..255): the average difference of their brightness on a 32 x 18 grid.
float luma_distance(const std::vector<unsigned char> &a, int aw, int ah, const std::vector<unsigned char> &b, int bw, int bh)
{
    const auto luma = [](const std::vector<unsigned char> &p, int w, int h, int x, int y) {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
        (void)h;
        return (p[i] * 299 + p[i + 1] * 587 + p[i + 2] * 114) / 1000;
    };
    long sum = 0;
    for (int gy = 0; gy < 18; ++gy)
        for (int gx = 0; gx < 32; ++gx)
        {
            const float fx = (static_cast<float>(gx) + 0.5f) / 32.0f, fy = (static_cast<float>(gy) + 0.5f) / 18.0f;
            sum += std::abs(luma(a, aw, ah, static_cast<int>(fx * aw), static_cast<int>(fy * ah)) -
                            luma(b, bw, bh, static_cast<int>(fx * bw), static_cast<int>(fy * bh)));
        }
    return static_cast<float>(sum) / (32.0f * 18.0f);
}

// The image host keeps a bigger copy of many wallpapers: the "medium" one is often 1280 wide, the
// "big" one 3840. Not always the same picture, though: for some titles "big" is other artwork (a poster-like
// still with the name in it), so it is only taken when it is the picture we already have, bigger.
void use_bigger_copy(const std::string &url, std::vector<unsigned char> &src, int &w, int &h)
{
    static const std::string kMedium = "/background/medium/";
    const std::size_t at = url.find(kMedium);
    if (at == std::string::npos || w >= 2880)
        return;
    std::string big_url = url;
    big_url.replace(at, kMedium.size(), "/background/big/");
    const HttpResponse r = http_get(big_url, 40);
    if (!r.ok())
        return;
    std::vector<unsigned char> big;
    int bw = 0, bh = 0;
    if (!decode(r.body, big, bw, bh) || bw <= w || bh <= 0)
        return;
    if (luma_distance(src, w, h, big, bw, bh) < 4.0f)
    {
        src.swap(big);
        w = bw;
        h = bh;
    }
}

bool opaque_kind(ArtKind kind)
{
    return kind == ArtKind::poster || kind == ArtKind::backdrop || kind == ArtKind::thumb;
}

const char *suffix(ArtKind kind)
{
    switch (kind)
    {
    case ArtKind::poster:
        return "p";
    case ArtKind::backdrop:
        return "b2"; // (b: the first cuts, 1920 wide at most)
    case ArtKind::thumb:
        return "t";
    case ArtKind::icon:
        return "i";
    case ArtKind::logo:
        return "l";
    case ArtKind::qr:
        return "q";
    }
    return "x";
}

} // namespace

// Forward: defined below; runs on a worker thread, never on the GL thread.
static void prune_disk_dir(const std::string &dir);

bool TexCache::init(const std::string &dir, hui::gfx::Renderer *renderer, int workers)
{
    dir_ = dir;
    renderer_ = renderer;
    make_dirs(dir_);
    stopping_ = false;
    for (int i = 0; i < workers; ++i)
        threads_.emplace_back([this] { worker(); });
    // One-time scan of the disk cache so fresh jobs can be priority-boosted
    // when their file is already on disk. Runs on the shared task pool,
    // never on the GL thread; entries land in disk_keys_ under mutex_.
    g_tasks.run([this] {
        // Trim an oversized art/ folder BEFORE the scan below, so the scan
        // only registers files that survived pruning.
        prune_disk_dir(dir_);
        if (DIR *d = opendir(dir_.c_str()))
        {
            while (dirent *e = readdir(d))
            {
                if (e->d_name[0] == '.')
                    continue;
                const std::string name = e->d_name;
                // Skip in-flight temp files, same as prune_disk_dir.
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0)
                    continue;
                std::lock_guard<std::mutex> lock(mutex_);
                disk_keys_.insert(dir_ + "/" + name);
            }
            closedir(d);
        }
    });
    return true;
}

void TexCache::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (std::thread &t : threads_)
        t.join();
    threads_.clear();
}

std::string TexCache::key_for(const std::string &url, ArtKind kind) const
{
    char name[64];
    std::snprintf(name, sizeof(name), "/%016llx%s.%s", static_cast<unsigned long long>(fnv1a(url)), suffix(kind),
                  opaque_kind(kind) ? "jpg" : "png");
    return dir_ + name;
}

std::uint32_t TexCache::get(const std::string &url, ArtKind kind, float priority)
{
    if (url.empty() || (!starts_with(url, "http://") && !starts_with(url, "https://")))
        return 0;
    const std::string key = key_for(url, kind);
    Entry &e = entries_[key];
    e.used_sec = now_seconds();
    if (e.id != 0)
        return e.id;
    if (e.failed)
        return 0;
    if (e.pending)
    {
        // Already queued or being downloaded: a lower (more urgent) priority
        // moves the still-queued job up. Only the queue order changes; the
        // entry stays pending and we still return 0.
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < queue_.size(); ++i)
        {
            if (queue_[i].key != key)
                continue;
            Job &q = queue_[i];
            const float eff = q.on_disk ? priority - kDiskPriorityBoost : priority;
            if (eff < q.priority)
            {
                Job moved = std::move(q);
                moved.priority = eff;
                queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
                auto it = queue_.begin();
                while (it != queue_.end() && it->priority >= moved.priority)
                    ++it;
                queue_.insert(it, std::move(moved));
            }
            break;
        }
        return 0;
    }
    // Snapshot boot: while the sync disk budget holds and the processed file
    // is on disk, decode it right here on the GL thread instead of queueing a
    // worker job. Any failure falls through to the normal queued path below.
    if (sync_disk_deadline_ > 0.0 && now_seconds() < sync_disk_deadline_ && file_exists(key))
    {
        Job sync_job;
        sync_job.url = url;
        sync_job.key = key;
        sync_job.kind = kind;
        sync_job.epoch = epoch_.load();
        sync_job.priority = 0.0f;
        sync_job.on_disk = true;
        sync_job.cancel = nullptr;
        Ready r;
        r.key = key;
        bool from_disk = false;
        if (make(sync_job, &r, &from_disk) && !r.pixels.empty())
        {
            e.id = renderer_->batch().create_texture(r.w, r.h, r.pixels.data());
            e.bytes = r.pixels.size();
            e.used_sec = now_seconds();
            e.uploaded_at = e.used_sec - 1.0; // age >= 1.0s so poster_fade is 1.0f on Frame 1
            e.accent = r.accent;
            e.tw = r.w;
            e.th = r.h;
            bytes_ += e.bytes;
            return e.id;
        }
    }
    e.pending = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        inflight_[key] = cancel;
        const bool on_disk = disk_keys_.count(key) > 0;
        Job job;
        job.url = url;
        job.key = key;
        job.kind = kind;
        job.epoch = epoch_.load();
        job.on_disk = on_disk;
        job.priority = on_disk ? priority - kDiskPriorityBoost : priority;
        job.cancel = std::move(cancel);
        // Keep the queue sorted (highest priority value first): the worker
        // pops from the back, so the lowest value is served first.
        auto it = queue_.begin();
        while (it != queue_.end() && it->priority >= job.priority)
            ++it;
        queue_.insert(it, std::move(job));
    }
    cv_.notify_one();
    return 0;
}

std::uint32_t TexCache::peek(const std::string &url, ArtKind kind) const
{
    const auto it = entries_.find(key_for(url, kind));
    return it == entries_.end() ? 0 : it->second.id;
}

bool TexCache::dims(const std::string &url, ArtKind kind, int *w, int *h) const
{
    const auto it = entries_.find(key_for(url, kind));
    if (it == entries_.end() || it->second.id == 0 || it->second.tw <= 0 || it->second.th <= 0)
        return false;
    *w = it->second.tw;
    *h = it->second.th;
    return true;
}

float TexCache::age(const std::string &url, ArtKind kind, double now) const
{
    if (url.empty()) return 1000.0f;
    const auto it = entries_.find(key_for(url, kind));
    if (it == entries_.end() || it->second.id == 0) return 1000.0f;
    return float(now - it->second.uploaded_at);
}

std::uint32_t TexCache::accent_of(const std::string &url, ArtKind kind) const
{
    const auto it = entries_.find(key_for(url, kind));
    return it == entries_.end() ? 0x3a2a8f : it->second.accent;
}

bool TexCache::pump(int max_uploads)
{
    // The disk cache is capped at 2 GB, oldest first; the scan runs at most
    // once a minute and only after a worker actually wrote something.
    // Pruning walks the filesystem (stat/sort/unlink over up to 2 GB of
    // files) so it runs on a background worker, never on the GL thread.
    if (disk_written_.load())
    {
        const double now = now_seconds();
        if (now - last_disk_prune_ > 60.0)
        {
            last_disk_prune_ = now;
            disk_written_.store(false);
            const std::string dir = dir_;
            g_tasks.run([dir] { prune_disk_dir(dir); });
        }
    }
    std::vector<Ready> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Per-frame upload budget: at most one large texture (> 1 MB, e.g. a
        // backdrop/poster) per frame, plus small ones up to a modest byte cap.
        // A burst of ready backdrops used to stall the render thread for a
        // whole frame of back-to-back glTexImage2D uploads.
        static const std::size_t kLargeBytes = 1u << 20;
        static const std::size_t kSmallCap = 2u << 20;
        bool large_taken = false;
        std::size_t small_bytes = 0;
        std::size_t taken = 0;
        for (auto it = ready_.begin(); it != ready_.end() && taken < static_cast<std::size_t>(max_uploads);)
        {
            const std::size_t bytes = it->pixels.size();
            const bool large = bytes > kLargeBytes;
            if (!it->pixels.empty() && (large ? large_taken : small_bytes + bytes > kSmallCap))
            {
                ++it; // over budget this frame: stays queued for the next one
                continue;
            }
            if (large)
                large_taken = true;
            else
                small_bytes += bytes;
            batch.push_back(std::move(*it));
            it = ready_.erase(it);
            ++taken;
        }
    }
    for (Ready &r : batch)
    {
        Entry &e = entries_[r.key];
        e.pending = false;
        if (r.dropped)
            continue; // stale: may be asked for again
        if (r.pixels.empty())
        {
            e.failed = true;
            continue;
        }
        e.id = renderer_->batch().create_texture(r.w, r.h, r.pixels.data());
        e.bytes = r.pixels.size();
        e.used_sec = now_seconds();
        e.uploaded_at = e.used_sec;
        e.accent = r.accent;
        e.tw = r.w;
        e.th = r.h;
        bytes_ += e.bytes;
    }
    return !batch.empty();
}

bool TexCache::trim(std::size_t budget)
{
    if (bytes_ <= budget)
        return false;
    bool deleted = false;
    // Wall-clock grace: a texture unused for 120 s may go; scrolling back
    // within that window finds it still on the GPU (no re-decode pop-in).
    // The 640 MB budget below is still the hard cap.
    const double now = now_seconds();
    // Drop long-stale failed entries: they hold no texture (id == 0) but keep
    // the URL out of retry forever. Erasure here is iterator-safe.
    for (auto it = entries_.begin(); it != entries_.end();)
        if (it->second.failed && now - it->second.used_sec > 120.0)
        {
            it = entries_.erase(it);
            deleted = true;
        }
        else
        {
            ++it;
        }
    std::vector<std::unordered_map<std::string, Entry>::iterator> loaded;
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (it->second.id != 0 && now - it->second.used_sec > 120.0)
            loaded.push_back(it);
    std::sort(loaded.begin(), loaded.end(),
              [](const auto &a, const auto &b) { return a->second.used_sec < b->second.used_sec; });
    for (auto &it : loaded)
    {
        if (bytes_ <= budget)
            break;
        const GLuint id = it->second.id;
        glDeleteTextures(1, &id);
        bytes_ -= it->second.bytes;
        entries_.erase(it);
        deleted = true;
    }
    return deleted;
}

// Thread-safe: only touches the passed-in directory, no TexCache members.
static void prune_disk_dir(const std::string &dir)
{
    struct File
    {
        std::string path;
        std::uint64_t mtime = 0;
        std::uint64_t size = 0;
    };
    std::vector<File> files;
    std::uint64_t total = 0;
    if (DIR *d = opendir(dir.c_str()))
    {
        while (dirent *e = readdir(d))
        {
            if (e->d_name[0] == '.')
                continue;
            const std::string name = e->d_name;
            // Skip in-flight temp files: write_file() is still filling them.
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0)
                continue;
            const std::string p = dir + "/" + name;
            struct stat st;
            if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            files.push_back({p, static_cast<std::uint64_t>(st.st_mtime), static_cast<std::uint64_t>(st.st_size)});
            total += static_cast<std::uint64_t>(st.st_size);
        }
        closedir(d);
    }
    static const std::uint64_t kDiskCap = 512ull << 20; // 512 MB
    if (total <= kDiskCap)
        return;
    std::sort(files.begin(), files.end(),
              [](const File &a, const File &b) { return a.mtime < b.mtime; });
    for (const File &f : files)
    {
        if (total <= kDiskCap)
            break;
        if (unlink(f.path.c_str()) == 0)
            total -= f.size;
    }
    dlog("art: disk cache pruned to %llu MB", static_cast<unsigned long long>(total >> 20));
}

void TexCache::worker()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_)
                return;
            job = queue_.back();
            queue_.pop_back();
        }
        Ready r;
        r.key = job.key;
        bool from_disk = false;
        try
        {
            if (job.epoch != epoch_.load() || (job.cancel && job.cancel->load()))
            {
                r.dropped = true;
            }
            else if (!make(job, &r, &from_disk))
                r.pixels.clear();
        }
        catch (const std::bad_alloc &)
        {
            dlog("art: out of memory decoding %s", job.url.c_str());
            r.pixels.clear();
        }
        // Cancelled mid-download (cancel_outside flipped the token): drop it,
        // don't fail it — the entry stays askable-for when it scrolls back
        // into view.
        if (!r.dropped && job.cancel && job.cancel->load())
        {
            r.dropped = true;
            r.pixels.clear();
        }
        // Queue the Ready FIRST so the next pump() uploads to the GPU
        // immediately; the JPEG/PNG encode + disk write happens after,
        // outside the lock, saving 40-120ms of GPU-visible latency.
        // Copy the pixels for the deferred disk write BEFORE taking the lock:
        // r is thread-local here, so this 18MB memcpy never holds mutex_.
        std::vector<unsigned char> px_copy;
        int pw = 0, ph = 0;
        if (!from_disk && !r.pixels.empty() && !r.dropped) {
            px_copy = r.pixels;
            pw = r.w; ph = r.h;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready_.push_back(std::move(r));
            inflight_.erase(job.key); // job finished: its cancel token is spent
        }
        if (!px_copy.empty())
            write_disk_async(job, px_copy, pw, ph);
    }
}

// Fast dominant-color extraction: samples a 16x16 grid, weights by
// saturation * mid-luminance (ignoring near-black/white), blends 55%
// with 0x2a1f5e so the result is always rich and dark-theme safe.
std::uint32_t extract_accent(const std::vector<unsigned char> &px, int w, int h)
{
    if (px.empty() || w <= 0 || h <= 0) return 0x3a2a8f;
    double rs = 0, gs = 0, bs = 0, ws = 0;
    for (int gy = 0; gy < 16; ++gy) {
        for (int gx = 0; gx < 16; ++gx) {
            const int x = (gx * w) / 16, y = (gy * h) / 16;
            const size_t i = (size_t(y) * w + x) * 4;
            if (i + 2 >= px.size()) continue;
            const double r = px[i] / 255.0, g = px[i+1] / 255.0, b = px[i+2] / 255.0;
            const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
            const double lum = (mx + mn) * 0.5;
            if (lum < 0.10 || lum > 0.92) continue;  // ignore near-black/white
            const double sat = mx > 0 ? (mx - mn) / mx : 0;
            const double wgt = sat * (1.0 - std::abs(lum - 0.5) * 2.0);
            if (wgt <= 0) continue;
            rs += r * wgt; gs += g * wgt; bs += b * wgt; ws += wgt;
        }
    }
    double r, g, b;
    if (ws > 0) { r = rs / ws; g = gs / ws; b = bs / ws; }
    else { r = 0.23; g = 0.16; b = 0.56; }  // fallback: theme accent
    // Blend 55% artwork color with 0x2a1f5e for dark-theme safety.
    r = r * 0.55 + 0x2a / 255.0 * 0.45;
    g = g * 0.55 + 0x1f / 255.0 * 0.45;
    b = b * 0.55 + 0x5e / 255.0 * 0.45;
    return (std::uint32_t(r * 255) << 16) | (std::uint32_t(g * 255) << 8) |
           std::uint32_t(b * 255);
}

bool TexCache::make(const Job &job, Ready *out, bool *from_disk)
{
    std::string data;
    if (read_file(job.key, data) && decode(data, out->pixels, out->w, out->h)) {
        out->accent = extract_accent(out->pixels, out->w, out->h);
        if (from_disk) *from_disk = true;
        return true; // already cut: use as it is
    }
    if (from_disk) *from_disk = false;

    const HttpResponse r = http_get(job.url, 30, job.cancel ? job.cancel.get() : nullptr);
    if (!r.ok())
    {
        dlog("art: %s: %s", job.url.c_str(), r.describe().c_str());
        return false;
    }
    std::vector<unsigned char> src;
    int w = 0, h = 0;
    if (!decode(r.body, src, w, h) || w <= 0 || h <= 0)
    {
        dlog("art: cannot decode %s", job.url.c_str());
        return false;
    }
    std::vector<unsigned char> px;
    int ow = 0, oh = 0;
    switch (job.kind)
    {
    case ArtKind::poster:
        cover(src, w, h, 2, 3, kPosterW, px, ow, oh);
        break;
    case ArtKind::backdrop:
    {
        cover(src, w, h, 16, 9, kBackdropMax, px, ow, oh, kBackdropMin);
        const float cropped_w = std::min(static_cast<float>(w), static_cast<float>(h) * 16.0f / 9.0f);
        if (static_cast<float>(ow) > cropped_w * 1.1f)
            sharpen(px, ow, oh, 0.45f);
        break;
    }
    case ArtKind::thumb:
        cover(src, w, h, 16, 9, kThumbW, px, ow, oh);
        break;
    case ArtKind::icon:
        fit_square(src, w, h, kIconSize, px);
        ow = oh = kIconSize;
        break;
    case ArtKind::logo:
        fit_width(src, w, h, kLogoW, px, ow, oh);
        break;
    case ArtKind::qr:
    {
        const int step = std::max(1, (kQrMin + w - 1) / w);
        ow = w * step;
        oh = h * step;
        px.resize(static_cast<std::size_t>(ow) * oh * 4);
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x)
                std::memcpy(&px[(static_cast<std::size_t>(y) * ow + x) * 4], &src[(static_cast<std::size_t>(y / step) * w + x / step) * 4], 4);
        break;
    }
    }

    // NOTE: the disk encode/write happens in worker() AFTER the Ready is
    // queued, so the GPU upload isn't stalled 40-120ms by JPEG encoding.
    // Stash the processed pixels for the deferred write.
    out->accent = extract_accent(px, ow, oh);
    out->pixels = std::move(px);
    out->w = ow;
    out->h = oh;
    return true;
}

// Deferred disk write, run on the worker thread after the Ready is queued.
void TexCache::write_disk_async(const Job &job, const std::vector<unsigned char> &px,
                                int w, int h)
{
    std::string file;
    if (opaque_kind(job.kind))
        stbi_write_jpg_to_func(to_string_sink, &file, w, h, 4, px.data(),
                               job.kind == ArtKind::backdrop ? 93 : 90);
    else
        stbi_write_png_to_func(to_string_sink, &file, w, h, 4, px.data(), w * 4);
    if (!file.empty())
    {
        write_file(job.key, file);
        disk_written_.store(true);
        std::lock_guard<std::mutex> lock(mutex_);
        disk_keys_.insert(job.key);
    }
}

// Cancels every queued/in-flight download whose key is not in keep_keys
// (the cursor moved on: off-screen jobs). Call from the GL thread.
void TexCache::cancel_outside(const std::unordered_set<std::string> &keep_keys)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = inflight_.begin(); it != inflight_.end();)
    {
        if (keep_keys.count(it->first) == 0)
        {
            it->second->store(true);
            it = inflight_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

} // namespace sx
