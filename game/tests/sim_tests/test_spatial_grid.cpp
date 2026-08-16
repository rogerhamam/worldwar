#include <catch2/catch_test_macros.hpp>

#include "sim/spatial_grid.h"

using namespace ww::sim;

namespace {
EntityRef ref(uint32_t slot) { return EntityRef{EntityKind::Unit, slot, 1}; }
} // namespace

TEST_CASE("SpatialGrid query finds entities within radius, ignores far ones") {
    SpatialGrid grid(64);
    grid.insert(ref(1), 10, 10);
    grid.insert(ref(2), 500, 500);

    auto near = grid.query(0, 0, 32);
    REQUIRE(near.size() == 1);
    REQUIRE(near[0] == ref(1));
}

TEST_CASE("SpatialGrid preserves insertion order within a bucket") {
    SpatialGrid grid(64);
    // All land in the same cell so they end up in one bucket.
    grid.insert(ref(3), 5, 5);
    grid.insert(ref(1), 6, 6);
    grid.insert(ref(2), 7, 7);

    auto hits = grid.query(5, 5, 10);
    REQUIRE(hits.size() == 3);
    REQUIRE(hits[0] == ref(3));
    REQUIRE(hits[1] == ref(1));
    REQUIRE(hits[2] == ref(2));
}

TEST_CASE("SpatialGrid clear empties all buckets") {
    SpatialGrid grid(64);
    grid.insert(ref(1), 0, 0);
    grid.clear();
    REQUIRE(grid.query(0, 0, 1000).empty());
}
