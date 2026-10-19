// Board snapshot: stale-while-revalidate for the Board page.
//
// The last fully loaded board rows live on disk as JSON (URLs only, no
// binary art). At boot, board_snapshot_load() paints them before any network
// call; the addon load then replaces each row as its catalog answers, so the
// page never flashes to empty. board_snapshot_save() persists a fresh board.
//
// Threading: both entry points run on the UI thread (init, and
// refresh_board() via the task pool's UI-thread delivery), the same thread
// that owns board_, so no locking is needed.

#include "session.hpp"

#include "board_cache.hpp"
#include "nav_memory.hpp"
#include "../src/util.h"

#include <algorithm>

namespace sx
{

namespace
{

constexpr std::int64_t kVersion = 3; // v3: rows carry continue_watching, items carry Continue Watching progress
constexpr std::size_t kMaxRows = 16;       // the board shows ~4; a dozen spare is cheap
constexpr std::size_t kMaxItemsPerRow = 50; // matches the board's kMaxRowCards cap
// NOTE: snapshots never expire (no kMaxAgeMs): an old snapshot is still
// better than a blank screen when the network is down.

json snapshot_item(const Item &it)
{
    json j;
    j["id"] = it.id;
    j["type"] = it.type;
    j["name"] = it.name;
    j["poster"] = it.poster;
    j["background"] = it.background;
    j["logo"] = it.logo;
    j["description"] = it.description;
    j["release_info"] = it.release_info;
    j["runtime"] = it.runtime;
    j["imdb_rating"] = it.imdb_rating;
    json genres = json::array();
    for (const auto &g : it.genres)
        genres.push_back(g);
    j["genres"] = std::move(genres);
    // Continue Watching progress (v3): resume offsets, episode art, watch counts.
    j["video_id"] = it.video_id;
    j["video_thumb"] = it.video_thumb;
    j["offset"] = it.offset;
    j["duration"] = it.duration;
    j["last_watched"] = it.last_watched;
    j["times_watched"] = it.times_watched;
    j["up_next"] = it.up_next;
    return j;
}

bool restore_item(const json &j, Item &it)
{
    if (!j.is_object())
        return false;
    it.id = jstr(j, "id");
    if (it.id.empty())
        return false;
    it.type = jstr(j, "type");
    it.name = jstr(j, "name");
    it.poster = jstr(j, "poster");
    it.background = jstr(j, "background");
    it.logo = jstr(j, "logo");
    it.description = jstr(j, "description");
    it.release_info = jstr(j, "release_info");
    it.runtime = jstr(j, "runtime");
    it.imdb_rating = jstr(j, "imdb_rating");
    const json &genres = jobj(j, "genres");
    if (genres.is_array())
    {
        for (const auto &g : genres)
        {
            if (g.is_string())
                it.genres.push_back(g.get<std::string>());
        }
    }
    // Continue Watching progress (v3): absent on v2 files, defaults keep them inert.
    it.video_id = jstr(j, "video_id");
    it.video_thumb = jstr(j, "video_thumb");
    it.offset = jnum(j, "offset");
    it.duration = jnum(j, "duration");
    it.last_watched = static_cast<std::int64_t>(jnum(j, "last_watched"));
    it.times_watched = static_cast<int>(jnum(j, "times_watched"));
    it.up_next = jbool(j, "up_next");
    return true;
}

} // namespace

std::string board_snapshot_path(const std::string &data_dir)
{
    return data_dir + "/board_snapshot.json";
}

std::string board_snapshot_age_label(std::int64_t saved_ms)
{
    const std::int64_t age_ms = now_epoch_ms() - saved_ms;
    if (age_ms < 0)
        return "Updated just now";
    const std::int64_t mins = age_ms / 60000;
    if (mins < 1)
        return "Updated just now";
    if (mins < 60)
        return "Updated " + std::to_string(mins) + "m ago";
    const std::int64_t hours = mins / 60;
    if (hours < 24)
        return "Updated " + std::to_string(hours) + "h ago";
    return "Updated " + std::to_string(hours / 24) + "d ago";
}

void Session::board_snapshot_save()
{
    // The board must have more than the Continue Watching row, and at least
    // one catalog row must have answered with items: persisting anything
    // less would paint a half-empty page on the next boot. Stale-while-
    // revalidate means rows can still carry snapshot items while their
    // catalogs re-answer, so there is deliberately no loading gate here:
    // a save during revalidation keeps the last good content, and shutdown
    // always flushes the cursor + Continue Watching for the next boot.
    if (board_.size() <= 1)
        return;
    json rows = json::array();
    bool any_catalog = false;
    for (const BoardRow &r : board_)
    {
        if (!r.loaded || r.items.empty())
            continue;
        if (rows.size() >= kMaxRows)
            break;
        json rj;
        rj["key"] = r.key;
        rj["title"] = r.title;
        rj["addon_url"] = r.addon_url;
        rj["continue_watching"] = r.continue_watching;
        json items = json::array();
        for (const Item &it : r.items)
        {
            if (items.size() >= kMaxItemsPerRow)
                break;
            items.push_back(snapshot_item(it));
        }
        rj["items"] = std::move(items);
        rows.push_back(std::move(rj));
        if (!r.continue_watching)
            any_catalog = true;
    }
    if (rows.empty() || !any_catalog)
        return;
    json j;
    j["version"] = kVersion;
    j["saved_ms"] = now_epoch_ms();
    j["cursor_row"] = last_cursor_.board_row;
    j["cursor_col"] = last_cursor_.board_col;
    const std::size_t nrows = rows.size();
    j["rows"] = std::move(rows);
    // save_json() is atomic (tmp file + rename), so a crash can never leave
    // a half-written file that the next boot would try to parse.
    if (!save_json(board_snapshot_path(data_dir_), j))
        dlog("board snapshot: save failed");
    else
        dlog("board snapshot: saved %zu rows", nrows);
}

bool Session::board_snapshot_load()
{
    json j;
    std::string err;
    if (!load_json(board_snapshot_path(data_dir_), j, &err))
        return false; // first launch: nothing cached yet
    const std::int64_t ver = static_cast<std::int64_t>(jnum(j, "version", 0));
    if (ver != 2 && ver != 3)
        return false;
    const std::int64_t saved = static_cast<std::int64_t>(jnum(j, "saved_ms", 0));
    const std::int64_t now = now_epoch_ms();
    // No age expiry: an old snapshot still beats a blank screen when the
    // network is down. Only impossible timestamps are rejected.
    if (saved <= 0 || saved > now)
        return false;
    const json &rows = jobj(j, "rows");
    if (!rows.is_array() || rows.empty())
        return false;
    board_.clear();
    for (const auto &rj : rows)
    {
        if (!rj.is_object())
            continue;
        BoardRow r;
        r.key = jstr(rj, "key");
        r.title = jstr(rj, "title");
        r.addon_url = jstr(rj, "addon_url");
        if (r.key.empty() || r.title.empty())
            continue;
        if (ver == 2 && r.key == "continue")
            continue; // v2 never wrote a continue row; the synthetic row below is the authority
        r.continue_watching = jbool(rj, "continue_watching", r.key == "continue");
        const json &items = jobj(rj, "items");
        if (!items.is_array())
            continue;
        for (const auto &ij : items)
        {
            Item it;
            if (restore_item(ij, it))
                r.items.push_back(std::move(it));
            if (r.items.size() >= kMaxItemsPerRow)
                break;
        }
        if (r.items.empty() && !r.continue_watching)
            continue;
        r.loaded = true;
        r.from_snapshot = true; // another worker's field (gl/model.hpp): this row is stale until a catalog answers
        r.validating = true;    // another worker's field (gl/model.hpp): its catalog fetch is still in flight
        board_.push_back(std::move(r));
        if (board_.size() > kMaxRows)
            break;
    }
    // The Continue Watching row always leads the board: v2 files never
    // carried one, and a v3 file whose continue row was empty at save time
    // gets the synthetic empty row too (refresh_board() fills it from local
    // progress right below).
    bool have_continue = false;
    for (const BoardRow &r : board_)
        if (r.key == "continue")
        {
            have_continue = true;
            break;
        }
    if (!have_continue)
    {
        BoardRow cw;
        cw.key = "continue";
        cw.title = "Continue Watching";
        cw.continue_watching = true;
        cw.loaded = true;
        board_.insert(board_.begin(), std::move(cw));
    }
    if (board_.size() <= 1)
    {
        board_.clear();
        return false;
    }
    // Restore the cursor the snapshot was saved with, and hand it to NavMem
    // so the grid opens on the card that was focused.
    last_cursor_.board_row = std::max(0, static_cast<int>(jnum(j, "cursor_row", 0)));
    last_cursor_.board_col = std::max(0, static_cast<int>(jnum(j, "cursor_col", 0)));
    NavMem::save_grid_pos("board", last_cursor_.board_row, last_cursor_.board_col);
    restored_page_ = Page::board;
    restored_row_ = last_cursor_.board_row;
    restored_col_ = last_cursor_.board_col;
    has_restored_ = true;
    board_snapshot_note_ = board_snapshot_age_label(saved);
    refresh_board();
    board.status = board_snapshot_note_ + " — refreshing…";
    dlog("board snapshot: v%lld %zu rows (%s)", (long long)ver, board_.size() - 1,
         board_snapshot_note_.c_str());
    return true;
}

} // namespace sx
