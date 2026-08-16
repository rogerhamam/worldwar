#pragma once
#include <functional>
#include <vector>

namespace ww::sim {

struct PathPoint {
    double x, y;
};

// A* over the tile grid, so land units route around water (and ships
// within it). Direct port of game/pathfind.py. Deliberately decoupled
// from World: `passable(tx, ty)` already encodes land/water-for-this-mover
// and solid-building checks (mirrors the `ok()` closure in the Python
// version, which captures `is_ship`), so this file has no dependency on
// the rest of the sim.
//
// Returns waypoints in world pixel coordinates (tile centers, with the
// final point snapped to the exact goal), or an empty vector if the mover
// is already at the goal tile / the goal tile isn't passable / there is
// nowhere better to go at all.
//
// PARTIAL PATHS: if the goal turns out unreachable, or the search runs out
// of budget, this returns the route to the closest tile it did reach
// rather than nothing. Callers steer straight at the destination when
// handed an empty path, which is precisely the wrong move inside a concave
// pocket -- getting out means walking AWAY from the goal first, and local
// steering can't discover that. A partial route at least leaves the
// pocket, and the caller re-paths from wherever it ends up. Only a path
// that genuinely reached the goal ends exactly on (gx, gy).
//
// max_nodes counts DISTINCT tiles expanded. 12000 covers a long way around
// a big obstacle on the largest maps (Huge is 192x192); the old 4000 could
// be swallowed whole just filling in a deep bay before ever finding the
// way around it, and the caller then got an empty path and blind-steered.
std::vector<PathPoint> astar(int cols, int rows, int tile_px,
                              double sx, double sy, double gx, double gy,
                              const std::function<bool(int tx, int ty)>& passable,
                              int max_nodes = 12000);

} // namespace ww::sim
