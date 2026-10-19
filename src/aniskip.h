#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "util.h"

// Crowd-sourced anime intro/outro times (AniSkip). All functions are blocking
// and meant for worker threads; `cancel` may be flipped to abort. They never
// throw: bad HTTP or malformed JSON just yields false / empty output.
namespace aniskip {

struct SkipSegment {
	double start = 0; // seconds
	double end = 0;   // seconds
	std::string kind; // "op", "ed" or "recap" (mixed-op/ed normalized on parse)
};

// MyAnimeList id for a Stremio series id. Kitsu ids ("kitsu:12345") resolve
// through Kitsu's anime mappings; anything else falls back to a Jikan title
// search. The prefs MAL-id cache is checked by the caller, not here.
// `season` (1-based) matters: MyAnimeList treats each anime season as a
// separate entry with episodes starting at 1, so for season > 1 the Jikan
// search tries "title Season N" first.
// If `via_jikan` is non-null, it is set true when the id came from the
// Jikan title-search fallback (less trustworthy than a Kitsu mapping) so
// the caller can choose not to persist it.
bool resolve_mal_id(const std::string& series_id, const std::string& title, int season,
                    const std::atomic<bool>* cancel, int& mal_id,
                    bool* via_jikan = nullptr);

// Intro/outro segments for one episode (AniSkip absolute episode numbers).
// Returns false (leaving `out` empty) on any failure: no data is silent.
// If `transport_failed` is non-null, set to true when both attempts died on
// transport (vs a definitive "no data" answer from the API).
// `episode_len` (seconds, 0 when unknown) is passed as AniSkip's episodeLength
// so length-matched submissions rank first; when that query 404s or returns
// found==false it retries once with episodeLength=0.
bool fetch_skip_times(int mal_id, int episode, const std::atomic<bool>* cancel,
                      std::vector<SkipSegment>& out, bool* transport_failed = nullptr,
                      double episode_len = 0);

// "Intro" / "Outro" / "Recap" / "Credits" / "Preview" for the Skip prompt
// and the "skipped" toast. Covers both AniSkip ("op"/"ed") and TheIntroDB
// kinds; anything unknown reads as "Intro".
std::string kind_label(const std::string& kind);

} // namespace aniskip
