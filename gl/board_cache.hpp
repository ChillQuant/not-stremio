// Board snapshot: stale-while-revalidate for the Board page.
//
// Session::board_snapshot_load() runs at boot, before any network call, and
// fills Session::board_ with the last successfully loaded rows so the Board
// paints instantly; the normal addon load then replaces each row as its
// catalog answers. Session::board_snapshot_save() writes those rows back to
// disk (called at the end of refresh_board(), on the UI thread that owns
// board_). Both live in board_cache.cpp next to these helpers.

#pragma once

#include <cstdint>
#include <string>

namespace sx
{

// The snapshot file inside the app's data dir: JSON holding each row's key
// and title plus its items' id/type/name/poster/backdrop URLs. No binary art.
std::string board_snapshot_path(const std::string &data_dir);

// "Updated just now" / "Updated 3m ago" / "Updated 2h ago" / "Updated 4d ago"
// for a wall-clock epoch-ms timestamp.
std::string board_snapshot_age_label(std::int64_t saved_ms);

} // namespace sx
