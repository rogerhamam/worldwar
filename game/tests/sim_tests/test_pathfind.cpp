#include <catch2/catch_test_macros.hpp>

#include "sim/pathfind.h"

using namespace ww::sim;

namespace {
constexpr int TILE = 24;
} // namespace

TEST_CASE("astar finds a straight path with no obstacles") {
    auto passable = [](int, int) { return true; };
    auto path = astar(20, 20, TILE, TILE * 2 + TILE / 2, TILE * 2 + TILE / 2,
                       TILE * 10 + TILE / 2, TILE * 2 + TILE / 2, passable);
    REQUIRE_FALSE(path.empty());
    // Final waypoint must land exactly on the requested destination.
    REQUIRE(path.back().x == TILE * 10 + TILE / 2);
    REQUIRE(path.back().y == TILE * 2 + TILE / 2);
}

TEST_CASE("astar routes around a wall instead of crossing it") {
    // A vertical wall at x==10 blocking rows 0..8, with a gap at row 9.
    auto passable = [](int tx, int ty) {
        if (tx == 10 && ty < 9) return false;
        return true;
    };
    auto path = astar(20, 20, TILE, TILE / 2, TILE / 2,
                       TILE * 15 + TILE / 2, TILE / 2, passable);
    REQUIRE_FALSE(path.empty());
    for (auto& p : path) {
        int tx = static_cast<int>(p.x / TILE);
        int ty = static_cast<int>(p.y / TILE);
        REQUIRE_FALSE((tx == 10 && ty < 9));
    }
}

TEST_CASE("astar walks as close as it can when the goal is unreachable") {
    // Used to return an empty path here. That left the CALLER (see
    // unit_behavior.cpp) with no route and steering straight at the
    // destination, which inside a concave pocket means pressing into the
    // back wall forever -- escaping one requires travelling away from the
    // goal first, and purely local steering can't discover that. Now it
    // hands back the route to the nearest tile it could actually reach.
    auto passable = [](int tx, int ty) {
        if (tx == 5) return false; // a solid wall spanning the whole map
        return true;
    };
    auto path = astar(20, 20, TILE, TILE / 2, TILE / 2,
                       TILE * 15 + TILE / 2, TILE / 2, passable);
    REQUIRE_FALSE(path.empty());
    // It goes toward the wall, never through it, and never pretends to
    // have arrived: the last waypoint is a real tile short of the goal.
    for (auto& p : path) REQUIRE(static_cast<int>(p.x / TILE) < 5);
    REQUIRE(path.back().x < TILE * 15);
}

TEST_CASE("astar escapes a concave pocket rather than pressing into its back wall") {
    // The reported symptom, reduced: the mover starts deep inside a
    // U-shaped pocket whose mouth faces AWAY from the goal, so every step
    // of the way out increases distance-to-goal. A greedy/local approach
    // wedges in the corner; a real search leaves through the mouth.
    // Pocket: walls at x==2 and x==6 spanning y 0..8, floor at y==8,
    // opening upward at y==0. Goal is far to the east.
    auto passable = [](int tx, int ty) {
        if ((tx == 2 || tx == 6) && ty >= 1 && ty <= 8) return false;
        if (ty == 8 && tx >= 2 && tx <= 6) return false;
        return true;
    };
    auto path = astar(20, 20, TILE, TILE * 4 + TILE / 2, TILE * 7 + TILE / 2,
                       TILE * 15 + TILE / 2, TILE * 7 + TILE / 2, passable);
    REQUIRE_FALSE(path.empty());
    REQUIRE(path.back().x == TILE * 15 + TILE / 2); // actually got there
    REQUIRE(path.back().y == TILE * 7 + TILE / 2);
    // The route must leave through the mouth, i.e. reach the top of the
    // pocket before it can head east -- proof it searched rather than
    // just hill-climbed toward the goal.
    bool exited_north = false;
    for (auto& p : path) {
        if (static_cast<int>(p.y / TILE) == 0) exited_north = true;
    }
    REQUIRE(exited_north);
}

TEST_CASE("astar returns empty when already at the goal tile") {
    auto passable = [](int, int) { return true; };
    auto path = astar(20, 20, TILE, TILE * 3 + 1, TILE * 3 + 1,
                       TILE * 3 + 5, TILE * 3 + 5, passable);
    REQUIRE(path.empty());
}
