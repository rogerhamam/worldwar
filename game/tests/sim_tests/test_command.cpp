// Uses a plain flat, waterless World (not a full scenario) so building
// placement/movement destinations are never accidentally blocked by
// scenario-scattered resources or a coastline -- keeps these tests
// focused on command dispatch, not scenario placement luck.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "sim/command.h"
#include "sim/world.h"

using namespace ww::sim;

namespace {
struct Harness {
    DataStore data{WW_DATA_DIR};
    Bonuses bonuses{data};
    Control control;
    Rng rng;
    EventBus events;
    World world;

    Harness(int n_players, uint64_t seed)
        : control(data, bonuses, n_players, 100), rng(seed),
          world(data, bonuses, control, rng, events, 60, 60, "random", /*water=*/false) {}
};
} // namespace

TEST_CASE("MoveCommand addressed by id drives a unit toward a destination") {
    Harness h(2, 10);
    EntityRef civ_ref = h.world.spawn_unit("civilian", 0, 500, 500);
    Unit* civ = h.world.get(civ_ref);
    uint32_t id = civ->common.id;

    apply_command(h.world, MoveCommand{id, 700, 500});
    REQUIRE(civ->move_goal.has_value());
    REQUIRE(civ->move_goal->x == 700);

    for (int i = 0; i < 20 * 10; ++i) h.world.update(1.0 / 20.0);
    REQUIRE(civ->common.x > 500); // actually moved toward the goal
}

TEST_CASE("GatherCommand and EnqueueCommand addressed by id work end-to-end") {
    Harness h(2, 11);
    EntityRef base_ref = h.world.spawn_building("base", 0, 500, 500);
    EntityRef tree_ref = h.world.spawn_resource("tree", 540, 500);
    EntityRef civ_ref = h.world.spawn_unit("civilian", 0, 540, 500);
    h.control.recompute(h.world); // populates team.cap from the base we just spawned

    apply_command(h.world, GatherCommand{h.world.get(civ_ref)->common.id,
                                         h.world.get_resource(tree_ref)->common.id});
    REQUIRE(h.world.get(civ_ref)->gather_target == tree_ref);

    Building* base = h.world.get_building(base_ref);
    apply_command(h.world, EnqueueCommand{base->common.id, "civilian"});
    REQUIRE(base->queue.size() == 1);
    REQUIRE(base->queue[0] == "civilian");
}

TEST_CASE("Training a unit awards its team 10 score (scripts/give_points.gml)") {
    Harness h(2, 12);
    EntityRef base_ref = h.world.spawn_building("base", 0, 500, 500);
    h.control.recompute(h.world);
    double score_before = h.control.teams[0].score;

    apply_command(h.world, EnqueueCommand{h.world.get_building(base_ref)->common.id, "civilian"});
    // Training progresses +4%/sim-second (building_behavior.cpp), so a
    // fresh queue item takes ~25s to finish -- 30s leaves headroom.
    for (int i = 0; i < 20 * 30; ++i) h.world.update(1.0 / 20.0);

    REQUIRE(h.control.teams[0].score == score_before + 10.0);
}

TEST_CASE("AttackCommand addressed by id sets a forced attack target") {
    Harness h(2, 12);
    EntityRef a = h.world.spawn_unit("rifleman", 0, 500, 500);
    EntityRef b = h.world.spawn_unit("rifleman", 1, 700, 500);
    apply_command(h.world, AttackCommand{h.world.get(a)->common.id, h.world.get(b)->common.id});
    REQUIRE(h.world.get(a)->attack_target == b);
    REQUIRE(h.world.get(a)->forced);
}

TEST_CASE("PlaceBuildingCommand and ResearchCommand work through apply_command") {
    Harness h(2, 13);
    h.control.teams[0].res["wood"] = 1000;
    h.control.teams[0].res["iron"] = 1000;
    // Team 0 placements are fog-gated (dark/unexplored ground can't be built
    // on -- see World::footprint_explored); this test only cares about
    // command dispatch, so just mark everything explored rather than route a
    // unit's sight over the target first.
    for (auto& row : h.world.fog) std::fill(row.begin(), row.end(), 2);

    apply_command(h.world, PlaceBuildingCommand{0, "house", 800, 800});
    bool found = false;
    for (auto ref : h.world.active_buildings) {
        if (h.world.get_building(ref)->name == "house") found = true;
    }
    REQUIRE(found);

    h.control.teams[0].era = 1;
    h.control.teams[0].res["iron"] = 1000;
    h.control.teams[0].res["oil"] = 1000;
    apply_command(h.world, ResearchCommand{0, "rifleman upgrade"});
    REQUIRE(h.control.teams[0].tech.count("rifleman upgrade") == 1);
}
