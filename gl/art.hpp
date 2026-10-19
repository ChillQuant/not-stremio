// Artwork as GL textures: downloaded (or read from the disk cache), cut to the
// shape it is drawn in, decoded on worker threads and uploaded on the thread
// that owns the GL context.
//
// Every picture is stored once on disk already cut and scaled (a JPEG for
// opaque pictures, a PNG where transparency matters), so the second visit costs
// a small decode instead of a download. The GPU draws rounded corners itself, so
// nothing is pre-rounded and one file serves every screen size.

#pragma once

#include "gfx/renderer.hpp"
#include "util.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sx
{

enum class ArtKind
{
    poster,   // 2:3, up to 480x720 (cards)
    backdrop, // 16:9, up to 1920x1080 (the hero, a title's page)
    thumb,    // 16:9, up to 320x180 (episodes)
    icon,     // 192x192, fitted on transparency (addon logos)
    logo,     // transparent title art, up to 560 wide, aspect kept (the hero)
    qr,       // scaled by whole steps to at least 400 px, never smoothed
};

class TexCache
{
  public:
    ~TexCache()
    {
        shutdown();
    }

    // `dir` is where the processed pictures are kept.
    bool init(const std::string &dir, hui::gfx::Renderer *renderer, int workers);
    void shutdown();

    // The texture, or 0 while it is still coming (the download is queued;
    // the most urgent job goes first). Call from the GL thread only.
    // `priority`: lower values are served first (viewport order); a job whose
    // file is already on disk is boosted automatically.
    std::uint32_t get(const std::string &url, ArtKind kind, float priority = 0.0f);
    // Cancels queued and in-flight downloads whose keys are not in keep_keys
    // (off-screen jobs the cursor moved past). Call from the GL thread.
    void cancel_outside(const std::unordered_set<std::string> &keep_keys);
    // The texture if it is already on the GPU, without asking for it.
    std::uint32_t peek(const std::string &url, ArtKind kind) const;
    // The decoded pixel dimensions of a cached texture; false when unknown.
    // Lets callers draw artwork at its own aspect (title logos must not stretch).
    bool dims(const std::string &url, ArtKind kind, int *w, int *h) const;
    // Age in seconds since this texture was uploaded to the GPU (1000.0 if unknown).
    float age(const std::string &url, ArtKind kind, double now) const;
    // Dominant ambient color (0xRRGGBB) of a cached backdrop/poster, or 0x3a2a8f.
    std::uint32_t accent_of(const std::string &url, ArtKind kind) const;

    // Once per frame on the GL thread: uploads up to max_uploads decoded
    // pictures. Returns true if any arrived.
    bool pump(int max_uploads);
    // Deletes the least recently asked-for textures until the cache is under
    // `budget` bytes. Textures asked for in the last 120 seconds stay.
    // Returns true if it deleted any: the owners of those ids must look again.
    bool trim(std::size_t budget);
    // The cursor moved on: queued jobs from the old position are stale and
    // the workers drop them before downloading. Call from the GL thread.
    void bump_epoch() { epoch_.fetch_add(1); }
    std::size_t bytes() const
    {
        return bytes_;
    }
    // Synchronous on-disk fast path for the snapshot boot: while the deadline
    // holds, get() decodes on-disk artwork inline on the GL thread instead of
    // queueing it. Call with 0.0 (or a negative value) to disable.
    void set_sync_disk_budget(double seconds)
    {
        sync_disk_deadline_ = seconds > 0.0 ? now_seconds() + seconds : 0.0;
    }

  private:
    struct Entry
    {
        std::uint32_t id = 0;
        std::size_t bytes = 0;
        double used_sec = 0.0; // last asked-for, wall clock (eviction grace)
        double uploaded_at = 0.0; // GPU upload time, for the 180ms fade-in
        std::uint32_t accent = 0x3a2a8f; // dominant artwork color (ambilight)
        bool pending = false;
        bool failed = false;
        int tw = 0; // decoded pixel width, for aspect-kept drawing
        int th = 0; // decoded pixel height
    };
    struct Job
    {
        std::string url;
        std::string key;
        ArtKind kind;
        std::uint64_t epoch = 0; // the cursor epoch that queued it
        float priority = 0.0f; // lower = higher priority
        bool on_disk = false;  // true if key exists in disk_keys_
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    struct Ready
    {
        std::string key;
        int w = 0;
        int h = 0;
        std::vector<unsigned char> pixels; // empty when it failed
        std::uint32_t accent = 0x3a2a8f; // dominant color for ambilight
        bool dropped = false; // stale epoch: clear pending, not failed
    };

    std::string key_for(const std::string &url, ArtKind kind) const;
    void worker();
    bool make(const Job &job, Ready *out, bool *from_disk = nullptr);
    void write_disk_async(const Job &job, const std::vector<unsigned char> &px, int w, int h);
    // LRU-by-mtime prune of the disk cache (GL thread only).


    std::string dir_;
    hui::gfx::Renderer *renderer_ = nullptr;
    std::unordered_map<std::string, Entry> entries_; // GL thread only
    std::size_t bytes_ = 0;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_set<std::string> disk_keys_; // guarded by mutex_
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> inflight_; // key -> cancel token
    std::vector<Job> queue_; // sorted by priority, highest value first: the worker pops from the back
    std::vector<Ready> ready_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
    std::atomic<std::uint64_t> epoch_{0}; // bumped when the cursor moves on
    std::atomic<bool> disk_written_{false}; // a worker saved a file: prune may be due
    double last_disk_prune_ = 0.0;          // GL thread only
    double sync_disk_deadline_ = 0.0;         // GL thread only: 0.0 = sync disk fast path off
};

} // namespace sx
