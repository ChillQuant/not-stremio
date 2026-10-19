#include "skipdb.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "http.h"

namespace skipdb {

bool valid_imdb_id(const std::string& id)
{
    if (id.size() < 3 || id.compare(0, 2, "tt") != 0)
        return false;
    for (std::size_t i = 2; i < id.size(); ++i)
        if (id[i] < '0' || id[i] > '9')
            return false;
    return true;
}

namespace {

// Parse one segment object into out. Accepts start_ms/end_ms, start_sec/end_sec,
// and bare start/end. Returns false when the object has no usable range.
bool parse_one_segment(const json& e, const std::string& kind, aniskip::SkipSegment& out)
{
    if (!e.is_object())
        return false;
    double start = -1.0, end = -1.0;

    if (e.contains("start_ms") && !e["start_ms"].is_null())
        start = jnum(e, "start_ms", -1.0) / 1000.0;
    else if (e.contains("start_sec") && !e["start_sec"].is_null())
        start = jnum(e, "start_sec", -1.0);
    else if (e.contains("start") && !e["start"].is_null())
        start = jnum(e, "start", -1.0);
    else if (e.contains("start_ms") && e["start_ms"].is_null())
        start = 0.0; // null start_ms means 0:00 cold open

    if (e.contains("end_ms") && !e["end_ms"].is_null())
        end = jnum(e, "end_ms", -1.0) / 1000.0;
    else if (e.contains("end_sec") && !e["end_sec"].is_null())
        end = jnum(e, "end_sec", -1.0);
    else if (e.contains("end") && !e["end"].is_null())
        end = jnum(e, "end", -1.0);
    else if ((kind == "credits" || kind == "outro") &&
             ((e.contains("end_ms") && e["end_ms"].is_null()) ||
              (e.contains("end_sec") && e["end_sec"].is_null())))
        end = 1e9; // null end on credits/outro means runs to end of media

    if (!std::isfinite(start) || !std::isfinite(end) || start < 0.0 || end <= start)
        return false;

    out.start = start;
    out.end = end;
    // Normalize kind names: outro/post_credits/post-credits are all "credits".
    out.kind = (kind == "outro" || kind == "post_credits" || kind == "post-credits")
               ? "credits" : kind;
    return true;
}

// Parse a response body from either database into out. Handles:
//  - introdb.app/segments: {"intro": {...}|null, "recap": ..., "outro": ..., "post_credits": ...}
//  - introdb.app/intro:    {"imdb_id":..., "start_ms":..., "end_ms":...}
//  - theintrodb.org/v3:    {"intro": [...], "recap": [...], ...} (+ data/results wrappers)
void parse_segments_json(const json& raw, std::vector<aniskip::SkipSegment>& out)
{
    if (raw.is_discarded())
        return;
    const json& j = (raw.is_object() && raw.contains("data") && raw["data"].is_object()) ? raw["data"]
                  : (raw.is_object() && raw.contains("results") && raw["results"].is_object()) ? raw["results"]
                  : raw;

    // Case 1: array of segment objects with a type field.
    const json* list = j.is_array() ? &j
                     : (j.is_object() && j.contains("segments") && j["segments"].is_array()) ? &j["segments"]
                     : (j.is_object() && j.contains("results") && j["results"].is_array()) ? &j["results"]
                     : nullptr;
    if (list)
    {
        for (const auto& item : *list)
        {
            if (!item.is_object())
                continue;
            std::string k = jstr(item, "segment_type", jstr(item, "type", jstr(item, "segment", "intro")));
            if (k != "intro" && k != "recap" && k != "credits" &&
                k != "outro" && k != "post_credits" && k != "post-credits" && k != "preview")
                continue;
            aniskip::SkipSegment s;
            if (parse_one_segment(item, k, s))
                out.push_back(s);
        }
        if (!out.empty())
            return;
    }

    if (!j.is_object())
        return;

    // Case 2: keyed by segment kind. Each value is an array (theintrodb.org),
    // a single object (introdb.app), or null (introdb.app "no data").
    static const char* kinds[] = {"intro", "recap", "credits", "outro",
                                  "post_credits", "post-credits", "preview"};
    for (const char* k : kinds)
    {
        if (!j.contains(k) || j[k].is_null())
            continue;
        const json& val = j[k];
        if (val.is_array())
        {
            for (const auto& e : val)
            {
                aniskip::SkipSegment s;
                if (parse_one_segment(e, k, s))
                    out.push_back(s);
            }
        }
        else if (val.is_object())
        {
            aniskip::SkipSegment s;
            if (parse_one_segment(val, k, s))
                out.push_back(s);
        }
    }

    // Case 3: flat single-intro object, e.g. introdb.app/intro
    // {"imdb_id":"tt...","season":4,"episode":22,"start_ms":12000,"end_ms":32000}
    if (out.empty())
    {
        aniskip::SkipSegment s;
        if (parse_one_segment(j, jstr(j, "segment_type", "intro"), s))
            out.push_back(s);
    }
}

// GET url with one polite retry on transport failure (not on 404/204, which is
// definitive). Sets answered=true when the server gave a definitive answer.
// Returns false when cancelled or when both attempts failed on transport.
bool query_url_with_retry(const char* url, const std::atomic<bool>* cancel,
                          std::vector<aniskip::SkipSegment>& out, bool& answered)
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (attempt > 0)
        {
            for (int i = 0; i < 20; ++i)
            {
                if (cancel && cancel->load())
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (cancel && cancel->load())
                return false;
        }
        HttpResponse r = http_get(url, 8, cancel);
        if (r.status == 404 || r.status == 204)
        {
            answered = true;
            return true;
        }
        if (!r.ok())
            continue;
        json j = json::parse(r.body, nullptr, false);
        if (j.is_discarded())
            continue;
        answered = true;
        parse_segments_json(j, out);
        return true;
    }
    return false;
}

} // namespace

bool fetch_tidb_segments(const std::string& imdb_id, int season, int episode,
                         bool is_movie, const std::atomic<bool>* cancel,
                         std::vector<aniskip::SkipSegment>& out)
{
    out.clear();
    if (!valid_imdb_id(imdb_id))
        return false;
    if (!is_movie && (season < 1 || episode < 1))
        return false;

    bool any_answered = false;
    try
    {
        char url[256];

        // 1. TheIntroDB v3 (api.theintrodb.org) — Jellyfin/Emby-oriented,
        //    TMDB-keyed; imdb_id coverage is partial.
        if (is_movie)
            std::snprintf(url, sizeof(url),
                          "https://api.theintrodb.org/v3/media?imdb_id=%s",
                          imdb_id.c_str());
        else
            std::snprintf(url, sizeof(url),
                          "https://api.theintrodb.org/v3/media?imdb_id=%s&season=%d&episode=%d",
                          imdb_id.c_str(), season, episode);
        query_url_with_retry(url, cancel, out, any_answered);
        if (!out.empty() || (cancel && cancel->load()))
        {
            dlog("skipdb: %s S%dE%d -> %zu segment(s) from theintrodb.org",
                 imdb_id.c_str(), season, episode, out.size());
            return true;
        }

        // 2. IntroDB.app (api.introdb.app/segments) — IMDb-native database used
        //    by Stremio/Nuvio/Infuse, 132k+ accepted TV episodes.
        if (is_movie)
            std::snprintf(url, sizeof(url),
                          "https://api.introdb.app/segments?imdb_id=%s&is_movie=true",
                          imdb_id.c_str());
        else
            std::snprintf(url, sizeof(url),
                          "https://api.introdb.app/segments?imdb_id=%s&season=%d&episode=%d",
                          imdb_id.c_str(), season, episode);
        query_url_with_retry(url, cancel, out, any_answered);
        if (!out.empty() || (cancel && cancel->load()))
        {
            dlog("skipdb: %s S%dE%d -> %zu segment(s) from introdb.app/segments",
                 imdb_id.c_str(), season, episode, out.size());
            return true;
        }

        // 3. IntroDB.app legacy /intro endpoint (TV episodes only).
        if (!is_movie)
        {
            std::snprintf(url, sizeof(url),
                          "https://api.introdb.app/intro?imdb_id=%s&season=%d&episode=%d",
                          imdb_id.c_str(), season, episode);
            query_url_with_retry(url, cancel, out, any_answered);
            if (!out.empty())
            {
                dlog("skipdb: %s S%dE%d -> %zu segment(s) from introdb.app/intro",
                     imdb_id.c_str(), season, episode, out.size());
                return true;
            }
        }
    }
    catch (...)
    {
        out.clear();
        return false;
    }
    dlog("skipdb: %s S%dE%d -> no segments in theintrodb.org or introdb.app (answered=%d)",
         imdb_id.c_str(), season, episode, any_answered ? 1 : 0);
    return any_answered;
}

} // namespace skipdb
