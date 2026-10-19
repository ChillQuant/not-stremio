#include "aniskip.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "stremio.h"

namespace aniskip {

bool resolve_mal_id(const std::string& series_id, const std::string& title, int season,
                    const std::atomic<bool>* cancel, int& mal_id,
                    bool* via_jikan) {
	mal_id = 0;
	if (via_jikan) *via_jikan = false;
	try {
		// Kitsu series ids look like "kitsu:12345": resolve through the
		// anime's external mappings to its MyAnimeList entry.
		// kitsu_answered tracks whether the Kitsu API responded at all: a
		// record with no MAL mapping must NOT fall back to the Jikan
		// popularity-sorted title search, which would attach some other
		// show's id. (review 2026-10-13)
		bool kitsu_answered = false;
		if (series_id.compare(0, 6, "kitsu:") == 0) {
			const std::string kid = series_id.substr(6);
			if (!kid.empty() && kid.find(':') == std::string::npos && kid.find('/') == std::string::npos) {
				json anime;
				std::string err;
				if (fetch_json("https://kitsu.app/api/edge/anime/" + kid, anime, err, 12, cancel)) {
					const json& rel = jobj(jobj(jobj(anime, "data"), "relationships"), "mappings");
					const std::string url = jstr(jobj(rel, "links"), "related");
					if (!url.empty() && url.compare(0, 8, "https://") == 0) {
						json mappings;
						if (fetch_json(url, mappings, err, 12, cancel)) {
							kitsu_answered = true;
							const json& arr = jobj(mappings, "data");
							if (arr.is_array()) {
								for (const auto& m : arr) {
									const json& a = jobj(m, "attributes");
									if (jstr(a, "externalSite") == "myanimelist/anime") {
										const int n = std::atoi(jstr(a, "externalId").c_str());
										if (n > 0) {
											mal_id = n;
											return true;
										}
									}
								}
							}
						}
					}
				}
			}
		}
		// Fallback (or non-Kitsu ids): Jikan title search, first hit.
		// For season > 1, MyAnimeList treats each season as a separate
		// entry with episodes starting at 1, so search "title Season N"
		// first (the bare title returns Season 1's id).
		if (!kitsu_answered && !title.empty() && !(cancel && cancel->load())) {
			auto jikan_search = [&](const std::string& q) -> bool {
				json res;
				std::string err;
				const std::string url =
				    "https://api.jikan.moe/v4/anime?q=" + url_encode(q) + "&limit=3&order_by=members&sort=desc";
				if (!fetch_json(url, res, err, 12, cancel))
					return false;
				const json& arr = jobj(res, "data");
				if (arr.is_array() && !arr.empty()) {
					const int n = static_cast<int>(jnum(arr.front(), "mal_id", 0));
					if (n > 0) {
						mal_id = n;
						if (via_jikan) *via_jikan = true;
						return true;
					}
				}
				return false;
			};
			const std::string season_q = season > 1 ? title + " Season " + std::to_string(season) : std::string();
			if ((!season_q.empty() && jikan_search(season_q)) ||
			    (season_q != title && jikan_search(title))) {
				// Jikan title search is a guess (popularity-sorted first
				// hit): flag it so the caller doesn't persist a
				// possibly-wrong id forever.
				return true;
			}
		}
	} catch (...) {
		mal_id = 0;
	}
	return mal_id > 0;
}

bool fetch_skip_times(int mal_id, int episode, const std::atomic<bool>* cancel,
                      std::vector<SkipSegment>& out, bool* transport_failed,
                      double episode_len) {
	out.clear();
	if (transport_failed) *transport_failed = false;
	if (mal_id <= 0 || episode <= 0) return false;
	// Episode length for AniSkip's length-matching filter; the episodeLength=0
	// fallback below covers encodings whose duration differs from the database.
	const int len_hint = episode_len > 0 ? static_cast<int>(episode_len + 0.5) : 0;
	// Pass 0: query with the real episode length. Pass 1: retry with
	// episodeLength=0 when the length-matched query 404s or found==false.
	// Pass 2: one transport-retry of the episodeLength=0 query.
	for (int pass = 0; pass < 3; ++pass) {
		if (pass > 0) {
			// Brief pause before retrying, cancel-aware.
			for (int i = 0; i < 30; ++i) {
				if (cancel && cancel->load()) return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (cancel && cancel->load()) return false;
		}
		const int qlen = (pass == 0) ? len_hint : 0;
		try {
			char url[320];
			std::snprintf(url, sizeof(url),
			              "https://api.aniskip.com/v2/skip-times/%d/%d"
			              "?types[]=op&types[]=ed&types[]=mixed-op&types[]=mixed-ed&types[]=recap"
			              "&episodeLength=%d",
			              mal_id, episode, qlen);
			json j;
			std::string err;
			if (!fetch_json(url, j, err, 12, cancel)) {
				if (pass == 2) break; // transport died twice: give up
				continue;
			}
			if (!jbool(j, "found", false)) {
				// Definitive "no data" only when we already tried without a
				// length hint; otherwise the length filter may be hiding it.
				if (pass > 0 || len_hint == 0) return false;
				continue; // retry once with episodeLength=0
			}
			const json& results = jobj(j, "results");
			if (!results.is_array()) return false;
			for (const auto& r : results) {
				std::string kind = jstr(r, "skipType");
				// Normalize: mixed-op/ed are openings/endings played over
				// episode footage (~30% of modern anime episodes).
				if (kind == "mixed-op") kind = "op";
				else if (kind == "mixed-ed") kind = "ed";
				else if (kind != "op" && kind != "ed" && kind != "recap") continue;
				const json& iv = jobj(r, "interval");
				const double start = jnum(iv, "startTime", -1);
				const double end = jnum(iv, "endTime", -1);
				if (start < 0 || end <= start) continue;
				SkipSegment s;
				s.start = start;
				s.end = end;
				s.kind = kind;
				out.push_back(s);
			}
			return !out.empty();
		} catch (...) {
			out.clear();
		}
	}
	// All attempts died on transport (or were cancelled mid-retry).
	if (transport_failed) *transport_failed = true;
	return false;
}

std::string kind_label(const std::string& kind)
{
	if (kind == "ed") return "Outro";
	if (kind == "recap") return "Recap";
	if (kind == "credits") return "Credits";
	if (kind == "preview") return "Preview";
	return "Intro"; // "op", "intro", and anything unexpected
}

} // namespace aniskip
