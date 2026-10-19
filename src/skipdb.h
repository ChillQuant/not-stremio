#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "aniskip.h"
#include "util.h"

// Crowd-sourced skip segments for TV shows and movies (TheIntroDB). All
// functions are blocking and meant for worker threads; `cancel` may be
// flipped to abort. They never throw: bad HTTP or malformed JSON just
// yields false / empty output.
namespace skipdb {

// "tt" followed by digits: safe to interpolate into the query string as-is.
bool valid_imdb_id(const std::string& id);

// Segments for one episode (or movie) in seconds, kinds "intro" | "recap" |
// "credits" | "preview" (see aniskip::SkipSegment).
// Returns true when the server gave a definitive answer: HTTP 200 with the
// segments parsed into `out` (possibly empty), or HTTP 404 (definitively no
// data — `out` left empty, safe to negative-cache). Returns false on
// transport failure, timeout, or a malformed response: nothing may be
// cached, the request simply didn't happen.
bool fetch_tidb_segments(const std::string& imdb_id, int season, int episode,
                         bool is_movie, const std::atomic<bool>* cancel,
                         std::vector<aniskip::SkipSegment>& out);

} // namespace skipdb
