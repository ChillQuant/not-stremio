#include "nav_memory.hpp"

#include <map>
#include <string>
#include <utility>

namespace sx
{
namespace NavMem
{
namespace
{

std::map<std::string, std::pair<int, int>> &positions()
{
    static std::map<std::string, std::pair<int, int>> m;
    return m;
}

} // namespace

void save_grid_pos(const char *page, int row, int col)
{
    if (!page || !*page || row < 0 || col < 0)
        return;
    positions()[page] = std::make_pair(row, col);
}

bool restore_grid_pos(const char *page, int &row, int &col)
{
    if (!page || !*page)
        return false;
    const auto it = positions().find(page);
    if (it == positions().end())
        return false;
    row = it->second.first;
    col = it->second.second;
    return true;
}

void clear(const char *page)
{
    if (!page || !*page)
        return;
    positions().erase(page);
}

} // namespace NavMem
} // namespace sx
