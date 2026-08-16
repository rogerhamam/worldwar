// Aircraft behavior tests (sim/src/aircraft_behavior.cpp): takeoff ramp,
// fuel drain/low-fuel return, landing/parking capacity, no-airbase fuel
// exhaustion -> crash, and a bomber actually dropping ordnance. Uses the
// same direct-World Harness pattern as test_world_integration.cpp (a
// flat, waterless map with hand-placed entities) rather than a full
// scenario, so each test only has to reason about the handful of
// entities it actually spawns.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "sim/bonuses.h"
#include "sim/catalog.h"
#include "sim/control.h"
#include "sim/rng.h"
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
        : control(data, bonuses, n_players, 100),
          rng(seed),
          world(data, bonuses, control, rng, events, 60, 60, "random", /*water=*/false) {}
};

} // namespace

TEST_CASE("A freshly spawned fighter ramps height up from 0 while flying") {
    Harness h(2, 1);
    EntityRef airbase = h.world.spawn_building("airbase", 0, 800, 800);
    EntityRef fighter = h.world.spawn_unit("fighter", 0, 820, 830); // just off the airbase, like a real spawn
    Unit* u = h.world.get(fighter);
    REQUIRE(u != nullptr);
    REQUIRE(u->common.is_air);
    REQUIRE(u->height == 0.0);

    u->move_goal = Vec2{1400, 800}; // far away -- not "idle", so it shouldn't try to land immediately
    for (int i = 0; i < 20 * 3; ++i) h.world.update(1.0 / 20.0); // 3 sim-seconds

    REQUIRE(u->height > 0.0); // climbing
    REQUIRE_FALSE(u->landed);
    REQUIRE(u->common.x > 820.0); // made real progress toward the goal
    (void)airbase;
}

TEST_CASE("An idle fighter over its airbase lands and refuels") {
    Harness h(2, 2);
    EntityRef airbase = h.world.spawn_building("airbase", 0, 800, 800);
    EntityRef fighter = h.world.spawn_unit("fighter", 0, 800, 800); // right on top of the airbase, idle
    Unit* u = h.world.get(fighter);
    u->fuel = 40.0; // below max -> eligible to land even though "idle" alone would suffice
    h.control.teams[0].res["oil"] = 500;

    for (int i = 0; i < 20 * 90; ++i) h.world.update(1.0 / 20.0); // up to 90 sim-seconds to fully refuel

    REQUIRE(u->landed);
    REQUIRE(u->home_id.has_value());
    Building* b = h.world.get_building(airbase);
    REQUIRE(*u->home_id == b->common.id);
    REQUIRE(u->fuel >= u->fuel_max - 0.01);
    REQUIRE(u->height == 0.0);
}

TEST_CASE("An airbase parks at most 5 planes; a 6th keeps circling") {
    Harness h(2, 3);
    EntityRef airbase = h.world.spawn_building("airbase", 0, 800, 800);
    std::vector<EntityRef> fighters;
    for (int i = 0; i < 6; ++i) {
        EntityRef ref = h.world.spawn_unit("fighter", 0, 800 + i * 4, 800 + i * 4);
        h.world.get(ref)->fuel = 10.0; // all want to land (well below max)
        fighters.push_back(ref);
    }

    for (int i = 0; i < 20 * 30; ++i) h.world.update(1.0 / 20.0); // 30 sim-seconds

    int landed_count = 0;
    for (auto ref : fighters) {
        if (h.world.get(ref)->landed) ++landed_count;
    }
    REQUIRE(landed_count == 5); // exactly the cap, not 6
    (void)airbase;
}

TEST_CASE("A plane with no friendly airbase dives and crashes when fuel runs out") {
    Harness h(2, 4);
    EntityRef fighter = h.world.spawn_unit("fighter", 0, 500, 500); // no airbase anywhere on this map
    Unit* u = h.world.get(fighter);
    u->fuel = 0.5; // will hit 0 within the first tick or two
    u->move_goal = Vec2{900, 500}; // not idle, so it never tries the (nonexistent) landing path

    bool saw_diving = false;
    for (int i = 0; i < 20 * 5 && u->common.alive; ++i) {
        h.world.update(1.0 / 20.0);
        if (u->diving) saw_diving = true;
    }

    REQUIRE(saw_diving);       // went into the dive-to-crash sequence
    REQUIRE_FALSE(u->common.alive); // ...and actually died, unlike the original GML's fuel=0 freeze bug
}

TEST_CASE("A bomber in range of an enemy drops a bomb (lob + bomb projectile)") {
    Harness h(2, 5);
    EntityRef bomber = h.world.spawn_unit("bomber", 0, 500, 500);
    EntityRef target = h.world.spawn_building("house", 1, 520, 500); // adjacent enemy building, well within range
    Unit* u = h.world.get(bomber);
    u->attack_target = target;
    u->forced = true;

    bool spawned_bomb = false;
    for (int i = 0; i < 20 * 5 && !spawned_bomb; ++i) {
        h.world.update(1.0 / 20.0);
        for (auto ref : h.world.active_projectiles) {
            Projectile* p = h.world.get_projectile(ref);
            if (p && p->bomb) { spawned_bomb = true; break; }
        }
    }
    REQUIRE(spawned_bomb);
}
