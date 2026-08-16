// Smoke/regression test for the stress-test debug tool (GameClient's
// Stress Test menu toggle -> populate_stress_test): verifies the generator
// actually produces a full 8-team roster (buildings of every production
// type, ~200 pop worth of mixed units including ships and planes) rather
// than silently no-op-ing if a spot/water search fails, which wouldn't be
// obvious from just eyeballing a screenshot of one corner of the map.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>

#include "sim/control.h"
#include "sim/scenario.h"
#include "sim/stress_scenario.h"

using namespace ww::sim;

namespace {
struct StressHarness {
    DataStore data{WW_DATA_DIR};
    Bonuses bonuses{data};
    Control control;
    Rng rng;
    EventBus events;
    std::unique_ptr<World> world;

    StressHarness(int n_players, uint64_t seed)
        : control(data, bonuses, n_players, 200), rng(seed) {
        SkirmishSettings settings;
        settings.n_players = n_players;
        settings.map_size = 64; // Huge -- matches the menu's auto-selected size for Stress Test
        settings.max_pop = 200;
        settings.water = true;
        world = new_skirmish(data, bonuses, control, rng, events, settings);
        populate_stress_test(*world, control, rng);
    }
};
} // namespace

TEST_CASE("populate_stress_test fills every team with a full building roster and ~200 pop") {
    StressHarness h(8, 123);

    std::map<int, std::set<std::string>> buildings_by_team;
    std::map<int, int> pop_by_team;
    std::map<int, int> unit_count_by_team;
    std::map<int, bool> has_ship, has_plane;

    for (auto ref : h.world->active_buildings) {
        Building* b = h.world->get_building(ref);
        REQUIRE(b != nullptr);
        REQUIRE(b->common.alive);
        buildings_by_team[b->common.team].insert(b->name);
    }
    static const std::set<std::string> kShipUnits = {"frigate", "destroyer", "battleship"};
    static const std::set<std::string> kPlaneUnits = {"fighter", "bomber"};
    for (auto ref : h.world->active_units) {
        Unit* u = h.world->get(ref);
        REQUIRE(u != nullptr);
        REQUIRE(u->common.alive);
        pop_by_team[u->common.team] += pop_cost(u->name);
        unit_count_by_team[u->common.team]++;
        if (kShipUnits.count(u->name)) has_ship[u->common.team] = true;
        if (kPlaneUnits.count(u->name)) has_plane[u->common.team] = true;
    }

    int teams_with_ships = 0;
    for (int t = 0; t < 8; ++t) {
        INFO("team " << t);
        REQUIRE(buildings_by_team[t].count("base") == 1);
        // One of every production building type (may occasionally be
        // missing exactly one on a cramped map, but not most of them).
        static const char* kExpected[] = {"barracks", "academy",  "factory", "fortress",
                                          "market",   "refinery", "university"};
        int found = 0;
        for (const char* name : kExpected) found += buildings_by_team[t].count(name);
        REQUIRE(found >= static_cast<int>(sizeof(kExpected) / sizeof(kExpected[0])) - 1);
        REQUIRE(buildings_by_team[t].count("house") == 1); // at least some houses placed
        REQUIRE(buildings_by_team[t].count("airbase") == 1);

        REQUIRE(unit_count_by_team[t] > 50);
        // Roster is fraction-of-budget-based (see populate_stress_test),
        // targeting control.max_pop (200 here) minus a small allowance for
        // new_skirmish's own starting civilians/cavalry -- should land
        // close to the configured max_pop, not just "some slack above 100"
        // (a fixed-count roster used to fall well short of this, e.g. ~185
        // even before any placement failures -- see the reported "only 122
        // pop" bug this was rewritten to fix).
        REQUIRE(pop_by_team[t] > 160);
        REQUIRE(pop_by_team[t] < 230);
        REQUIRE(has_plane[t]); // planes don't need water -- should never be skipped
        if (has_ship[t]) ++teams_with_ships;
    }
    // A Huge water map should give most (if not all) of 8 spawn points some
    // coastline to work with -- not requiring literally all 8 since spawn
    // point placement is randomized per-seed and a couple of interior
    // starts on a big map is plausible.
    REQUIRE(teams_with_ships >= 6);
}
