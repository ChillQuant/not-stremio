// Back-navigation focus memory: when the detail page closes, the grid it came
// from restores the exact card (and with it the scroll position).
//
// Page keys and the (row, col) convention:
//   "board"    -> (board_row, board_col)
//   "discover" -> (flat / 4, flat % 4)   (the Discover grid is 4 columns)
//   "library"  -> (flat / 6, flat % 6)   (the Library grid is 6 columns)
//   "search"   -> (search_row, search_col)
// The grid converts back with row * cols + col; restoring the focused card
// restores the scroll position, because the grid always scrolls the focused
// card into view. Plain in-memory map: entries live for the session, keyed by
// page name so grids never collide, and never touch disk.

#pragma once

namespace sx
{
namespace NavMem
{

void save_grid_pos(const char *page, int row, int col);
bool restore_grid_pos(const char *page, int &row, int &col);
void clear(const char *page);

} // namespace NavMem
} // namespace sx
