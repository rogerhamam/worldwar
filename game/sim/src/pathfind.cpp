#include "sim/pathfind.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <unordered_map>

namespace ww::sim {

namespace {

struct Coord {
    int x, y;
    bool operator==(const Coord& o) const { return x == o.x && y == o.y; }
};
struct CoordHash {
    size_t operator()(const Coord& c) const {
        return (static_cast<uint64_t>(static_cast<uint32_t>(c.x)) << 32) ^
               static_cast<uint32_t>(c.y);
    }
};

// Mirrors Python heapq's tuple comparison on (priority, (x, y)): primary
// key is priority, ties broken by x then y. This exact tie-break matters
// for determinism -- see the memory note on iteration-order dependence.
struct HeapEntry {
    double priority;
    Coord node;
};
struct HeapCmp {
    // std::priority_queue is a max-heap; invert so top() is the smallest
    // (priority, x, y) tuple, matching Python's min-heap heapq.
    bool operator()(const HeapEntry& a, const HeapEntry& b) const {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.node.x != b.node.x) return a.node.x > b.node.x;
        return a.node.y > b.node.y;
    }
};

} // namespace

std::vector<PathPoint> astar(int cols, int rows, int tile_px,
                              double sx, double sy, double gx, double gy,
                              const std::function<bool(int, int)>& passable,
                              int max_nodes) {
    auto ok = [&](int tx, int ty) {
        if (tx < 0 || tx >= cols || ty < 0 || ty >= rows) return false;
        return passable(tx, ty);
    };

    Coord start{static_cast<int>(std::floor(sx / tile_px)), static_cast<int>(std::floor(sy / tile_px))};
    Coord goal{static_cast<int>(std::floor(gx / tile_px)), static_cast<int>(std::floor(gy / tile_px))};
    if ((start == goal) || !ok(goal.x, goal.y)) return {};

    // OCTILE distance -- the true cheapest route to the goal across open
    // ground given the 1.0 / 1.4142 step costs used below. The heuristic
    // used to be Manhattan (|dx| + |dy|), which OVERESTIMATES whenever a
    // diagonal is available: going 10 tiles diagonally really costs 14.1,
    // but Manhattan claims 20. An inadmissible heuristic turns A* into
    // something much closer to greedy best-first search, and greedy search
    // is worst exactly where the complaint is -- a concave pocket, where
    // "head toward the goal" dives straight in and every tile of the climb
    // back out looks like a step backwards. It also breaks A*'s
    // once-per-node guarantee, so nodes get re-expanded and the budget
    // below burns on tiles already visited. Octile is admissible AND
    // consistent, which fixes both.
    auto h = [&](int x, int y) {
        int dx = std::abs(x - goal.x), dy = std::abs(y - goal.y);
        int lo = std::min(dx, dy), hi = std::max(dx, dy);
        return 1.4142 * lo + static_cast<double>(hi - lo);
    };

    // Per-tile search state stored in FLAT grid arrays rather than
    // unordered_map<Coord> (which hashed and heap-allocated on every node
    // touch -- the dominant cost when a whole army re-paths on one tick and a
    // few searches hit the 12000-node budget, causing multi-hundred-ms sim
    // spikes). A monotonically-increasing generation stamp gives O(1) reset:
    // a cell counts as "seen this search" only when its stamp equals the
    // current generation, so the big buffers never have to be re-zeroed.
    // thread_local keeps the tournament's per-thread matches race-free. The
    // algorithm, node-expansion order and outputs are BYTE-IDENTICAL to the
    // map version -- this is purely a data-structure swap (golden checksum
    // unchanged).
    const size_t ncells = static_cast<size_t>(cols) * static_cast<size_t>(rows);
    thread_local std::vector<double> t_gc;
    thread_local std::vector<Coord> t_came;
    thread_local std::vector<uint32_t> t_seen, t_closed;
    thread_local uint32_t t_gen = 0;
    if (t_seen.size() < ncells) {
        t_gc.assign(ncells, 0.0);
        t_came.assign(ncells, Coord{});
        t_seen.assign(ncells, 0);
        t_closed.assign(ncells, 0);
        t_gen = 0;
    }
    if (++t_gen == 0) { // wrapped after 4B searches -- clear so stale != new gen
        std::fill(t_seen.begin(), t_seen.end(), 0);
        std::fill(t_closed.begin(), t_closed.end(), 0);
        t_gen = 1;
    }
    const uint32_t gen = t_gen;
    auto IDX = [cols](int x, int y) { return static_cast<size_t>(y) * cols + x; };
    auto getg = [&](int x, int y) { size_t i = IDX(x, y); return t_seen[i] == gen ? t_gc[i] : 1e18; };

    std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapCmp> openh;
    openh.push({h(start.x, start.y), start});
    { size_t si = IDX(start.x, start.y); t_seen[si] = gen; t_gc[si] = 0.0; } // gc[start]=0, no parent

    // Best-effort target: the reachable tile that got closest to the goal.
    // Used when the search fails outright (see the partial-path handling
    // after the loop).
    Coord best_node = start;
    double best_h = h(start.x, start.y);

    int nodes = 0;
    bool found = false;
    Coord cur{};
    while (!openh.empty() && nodes < max_nodes) {
        cur = openh.top().node;
        openh.pop();
        // A node can sit in the heap several times over (there's no
        // decrease-key); with the consistent heuristic above the first pop
        // is already optimal, so skip the stale repeats. This also makes
        // `nodes` count DISTINCT tiles examined, which is what makes
        // max_nodes a predictable budget rather than one that a few
        // heavily-re-pushed tiles can quietly eat.
        size_t ci = IDX(cur.x, cur.y);
        if (t_closed[ci] == gen) continue;
        t_closed[ci] = gen;
        ++nodes;
        if (cur == goal) {
            found = true;
            break;
        }
        double ch = h(cur.x, cur.y);
        if (ch < best_h) { best_h = ch; best_node = cur; }
        double cur_gc = t_gc[ci];
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                if (dx == 0 && dy == 0) continue;
                int nx = cur.x + dx, ny = cur.y + dy;
                if (!ok(nx, ny)) continue;
                if (dx != 0 && dy != 0 && !(ok(cur.x + dx, cur.y) && ok(cur.x, cur.y + dy))) {
                    continue; // don't cut corners
                }
                double nc = cur_gc + ((dx != 0 && dy != 0) ? 1.4142 : 1.0);
                if (nc < getg(nx, ny)) {
                    size_t ni = IDX(nx, ny);
                    t_seen[ni] = gen;
                    t_gc[ni] = nc;
                    t_came[ni] = cur;
                    openh.push({nc + h(nx, ny), {nx, ny}});
                }
            }
        }
    }

    // PARTIAL PATH. When the goal can't be reached -- genuinely walled off,
    // or the node budget ran out mid-search -- this used to return nothing
    // at all, and the callers fall back to steering straight at the
    // destination with no route. In a concave pocket that is the worst
    // possible response: the unit presses into the back wall, because
    // purely local steering has no way to know it must first travel AWAY
    // from the goal to get out. Handing back the route to the closest tile
    // the search did reach at least walks it somewhere real -- out of the
    // pocket mouth when the way out was found, or as near as it can
    // actually get when there's truly no way through -- and the caller
    // re-paths from there.
    Coord target = found ? goal : best_node;
    if (!found && best_node == start) return {}; // nowhere better to go at all

    std::vector<PathPoint> path;
    Coord c = target;
    while (!(c == start)) {
        path.push_back({c.x * static_cast<double>(tile_px) + tile_px / 2.0,
                         c.y * static_cast<double>(tile_px) + tile_px / 2.0});
        size_t i = IDX(c.x, c.y);
        if (t_seen[i] != gen) break; // reached start's sentinel (no parent recorded)
        c = t_came[i];
    }
    std::reverse(path.begin(), path.end());
    // Only snap the last waypoint onto the caller's exact destination when
    // the route actually got there -- a partial path ends at a tile centre
    // somewhere else entirely, and pretending otherwise would teleport the
    // final step across the gap.
    if (found && !path.empty()) path.back() = {gx, gy};
    return path;
}

} // namespace ww::sim
