#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <utility>

#include "sim/scenario.h"

using namespace ww::sim;

namespace {
struct ScenarioHarness {
    DataStore data{WW_DATA_DIR};
    Bonuses bonuses{data};
    Control control;
    Rng rng;
    EventBus events;
    std::unique_ptr<World> world;

    ScenarioHarness(int n_players, uint64_t seed, SkirmishSettings settings = {})
        : control(data, bonuses, n_players, 100), rng(seed) {
        settings.n_players = n_players;
        world = new_skirmish(data, bonuses, control, rng, events, settings);
    }
};
} // namespace

TEST_CASE("new_skirmish places one base per team and starting units") {
    ScenarioHarness h(2, 42);
    int base_count = 0, civilian_count = 0, cavalry_count = 0;
    for (auto ref : h.world->active_buildings) {
        if (h.world->get_building(ref)->name == "base") base_count++;
    }
    for (auto ref : h.world->active_units) {
        Unit* u = h.world->get(ref);
        if (u->name == "civilian") civilian_count++;
        else if (u->name == "cavalry") cavalry_count++;
    }
    REQUIRE(base_count == 2);
    REQUIRE(civilian_count >= 6); // 3 per team, minimum
    REQUIRE(cavalry_count == 2);
}

TEST_CASE("new_skirmish scatters resources without overlapping bases") {
    ScenarioHarness h(2, 7);
    REQUIRE(h.world->active_resources.size() > 20);
    REQUIRE(h.world->active_deer.size() > 0);
}

TEST_CASE("new_skirmish is fully deterministic for a given seed") {
    ScenarioHarness a(2, 555);
    ScenarioHarness b(2, 555);
    REQUIRE(a.world->terrain == b.world->terrain);
    REQUIRE(a.world->active_resources.size() == b.world->active_resources.size());
    REQUIRE(a.world->active_units.size() == b.world->active_units.size());

    // spot-check a handful of resource positions match exactly
    for (size_t i = 0; i < a.world->active_resources.size(); ++i) {
        Resource* ra = a.world->get_resource(a.world->active_resources[i]);
        Resource* rb = b.world->get_resource(b.world->active_resources[i]);
        REQUIRE(ra->common.x == rb->common.x);
        REQUIRE(ra->common.y == rb->common.y);
        REQUIRE(ra->name == rb->name);
    }
}

TEST_CASE("Deathmatch mode grants huge starting resources and a build_speed boost") {
    SkirmishSettings settings;
    settings.deathmatch = true;
    ScenarioHarness h(2, 11, settings);

    for (int t = 0; t < 2; ++t) {
        REQUIRE(h.control.teams[t].res["food"] == 20000);
        REQUIRE(h.control.teams[t].res["wood"] == 20000);
        REQUIRE(h.control.teams[t].res["oil"] == 20000);
        REQUIRE(h.control.teams[t].res["iron"] == 20000);
    }
    REQUIRE(h.world->build_speed == 8.0);
}

TEST_CASE("Standard mode keeps the ordinary starting resources and build_speed") {
    ScenarioHarness h(2, 12); // default settings -- deathmatch = false
    REQUIRE(h.control.teams[0].res["food"] == 200);
    REQUIRE(h.world->build_speed == 1.0);
}

TEST_CASE("World::cancel_queue refunds cost and removes the item from the queue") {
    ScenarioHarness h(2, 21);
    EntityRef base = kNullRef;
    for (auto ref : h.world->active_buildings) {
        if (h.world->get_building(ref)->name == "base" && h.world->get_building(ref)->common.team == 0) {
            base = ref;
            break;
        }
    }
    REQUIRE(base.valid());
    double food_before = h.control.teams[0].res["food"];
    REQUIRE(h.world->enqueue(base, "civilian"));
    double food_after_enqueue = h.control.teams[0].res["food"];
    REQUIRE(food_after_enqueue < food_before); // paid up front

    Building* b = h.world->get_building(base);
    REQUIRE(b->queue.size() == 1);
    REQUIRE(h.world->cancel_queue(base, 0));
    REQUIRE(b->queue.empty());
    REQUIRE(h.control.teams[0].res["food"] == food_before); // fully refunded
    REQUIRE(b->percent == 0.0);

    // Out-of-range index is a no-op, not a crash.
    REQUIRE_FALSE(h.world->cancel_queue(base, 0));
}

TEST_CASE("A full skirmish runs for simulated minutes without crashing or NaNs") {
    ScenarioHarness h(2, 99);
    h.control.teams[1].is_ai = true; // team 0 stays human/idle, team 1 plays itself

    for (int i = 0; i < 20 * 180; ++i) h.world->update(1.0 / 20.0); // 3 simulated minutes

    for (int t = 0; t < h.control.n; ++t) {
        for (auto& [k, v] : h.control.teams[t].res) {
            REQUIRE(v == v);   // not NaN
            REQUIRE(v < 1e9);  // not runaway/inf
        }
    }
    // The AI team should have grown its economy at least somewhat over 3 minutes.
    bool ai_has_buildings_beyond_base = false;
    for (auto ref : h.world->active_buildings) {
        Building* b = h.world->get_building(ref);
        if (b->common.team == 1 && b->name != "base") ai_has_buildings_beyond_base = true;
    }
    REQUIRE(ai_has_buildings_beyond_base);
}

// ---- randomised starting positions (spawn_points, scenario.cpp) ----

namespace {

// Team `t`'s base position in a finished skirmish.
std::pair<double, double> base_pos(World& w, int t) {
    for (auto ref : w.active_buildings) {
        Building* b = w.get_building(ref);
        if (b && b->name == "base" && b->common.team == t) return {b->common.x, b->common.y};
    }
    return {-1.0, -1.0};
}

} // namespace

TEST_CASE("Starting positions rotate around the map instead of a fixed corner") {
    // Regression test for the real complaint: spawn_points used to be a
    // fixed table of map fractions with no random input, so team 0 opened
    // in the top-left and team 1 in the bottom-right of EVERY skirmish --
    // the player always knew where the enemy was without scouting. Both
    // teams should now turn up in every quadrant across seeds.
    std::set<int> p0_quadrants, p1_quadrants;
    for (uint64_t seed = 900; seed < 940; ++seed) {
        ScenarioHarness h(2, seed);
        double mx = h.world->px_w * 0.5, my = h.world->px_h * 0.5;
        auto [x0, y0] = base_pos(*h.world, 0);
        auto [x1, y1] = base_pos(*h.world, 1);
        REQUIRE(x0 >= 0.0);
        REQUIRE(x1 >= 0.0);
        p0_quadrants.insert((x0 > mx ? 1 : 0) + (y0 > my ? 2 : 0));
        p1_quadrants.insert((x1 > mx ? 1 : 0) + (y1 > my ? 2 : 0));
    }
    // All four quadrants, for the human slot AND the enemy slot -- not just
    // "it moved a bit", which a jittered fixed table would also satisfy.
    REQUIRE(p0_quadrants.size() == 4);
    REQUIRE(p1_quadrants.size() == 4);
}

TEST_CASE("Opposing bases always start a long way apart, on land maps and water maps alike") {
    // The other half of the requirement: randomising the rotation must
    // never produce two bases next to each other. Checked on both map
    // flavours because a water map's usable land is a narrow western strip
    // (World's terrain gen floods the east), which is exactly the lopsided
    // case that squeezes the spawn ellipse flat.
    for (bool water : {false, true}) {
        for (uint64_t seed = 700; seed < 730; ++seed) {
            SkirmishSettings s;
            s.water = water;
            ScenarioHarness h(2, seed, s);
            auto [x0, y0] = base_pos(*h.world, 0);
            auto [x1, y1] = base_pos(*h.world, 1);
            double d = std::hypot(x1 - x0, y1 - y0);
            // 20 tiles, comfortably beyond any early rush distance and well
            // under what the layout actually targets -- a floor, not a fit
            // to current numbers.
            INFO("water=" << water << " seed=" << seed << " dist=" << d);
            REQUIRE(d > 20.0 * TILE);
        }
    }
}

TEST_CASE("Starting bases sit on land, not in the sea") {
    // spawn_points inscribes its ring in the measured land bounding box,
    // and the caller nudges toward the map centre as a failsafe; between
    // them no base should ever end up on a water tile, on any map type.
    for (const char* type : {"random", "guam", "stalingrad", "normandy", "ostland"}) {
        for (uint64_t seed = 500; seed < 512; ++seed) {
            SkirmishSettings s;
            s.map_type = type;
            s.water = true; // only honoured by "random"; themed maps set their own
            ScenarioHarness h(2, seed, s);
            for (int t = 0; t < 2; ++t) {
                auto [bx, by] = base_pos(*h.world, t);
                INFO("map=" << type << " seed=" << seed << " team=" << t);
                REQUIRE_FALSE(h.world->is_water(bx, by));
            }
        }
    }
}

// ---- AI opening: economy first, farms matched to food workers ----

namespace {

// Villagers a team currently has on FOOD (a farm, or a food-type resource).
// Deliberately re-derived here rather than shared with ai_build's copy, so
// this is an independent check of the rule and not the same expression
// compared against itself.
int food_workers_of(World& w, int team) {
    int n = 0;
    for (auto ref : w.active_units) {
        Unit* u = w.get(ref);
        if (!u || !u->common.alive || u->common.team != team || u->name != "civilian") continue;
        int rt = -1;
        if (u->gather_target.kind == EntityKind::Building) rt = 0;
        else if (Resource* r = w.get_resource(u->gather_target)) rt = r->res.rtype;
        if (rt < 0) rt = u->gather_rtype;
        if (rt == 0) ++n;
    }
    return n;
}

int count_buildings(World& w, int team, const std::string& name, bool skip_exhausted = false) {
    int n = 0;
    for (auto ref : w.active_buildings) {
        Building* b = w.get_building(ref);
        if (!b || !b->common.alive || b->common.team != team || b->name != name) continue;
        if (skip_exhausted && b->exhausted) continue;
        ++n;
    }
    return n;
}

int civ_count_of(World& w, int team) {
    int n = 0;
    for (auto ref : w.active_units) {
        Unit* u = w.get(ref);
        if (u && u->common.alive && u->common.team == team && u->name == "civilian") ++n;
    }
    return n;
}

} // namespace

TEST_CASE("An economy-first AI doesn't open with an instant barracks") {
    // The AI used to push its first barracks 4th in the build order
    // unconditionally -- above houses, the academy and every farm -- so every
    // skirmish AI regardless of its map-derived plan had a barracks up inside
    // the first minute and started making muscateers off three villagers.
    // That's a rush, and it should only happen when the plan actually calls
    // for one.
    int checked = 0;
    for (uint64_t seed = 300; seed < 316; ++seed) {
        ScenarioHarness h(2, seed);
        bool seen = false;
        for (int i = 0; i < 20 * 60 * 12 && !seen; ++i) { // up to 12 sim-minutes
            h.world->update(1.0 / 20.0);
            if (count_buildings(*h.world, 1, "barracks") == 0) continue;
            seen = true;
            if (h.control.teams[1].ai_plan.playstyle == "aggressive") break; // rushing is its job
            ++checked;
            // Whenever it does go up, one of the gate's conditions must hold.
            // Measured across seeds 300-315: non-aggressive plans land the
            // barracks at 4-10 sim-minutes with 12-26 villagers, never off
            // the threat term.
            INFO("seed=" << seed << " playstyle=" << h.control.teams[1].ai_plan.playstyle
                         << " t=" << i / 20 << "s civs=" << civ_count_of(*h.world, 1));
            REQUIRE((civ_count_of(*h.world, 1) >= 12 || h.control.teams[1].era >= 1 ||
                     h.control.teams[1].ai_threat > 0));
        }
    }
    REQUIRE(checked > 0); // the run has to actually exercise a non-aggressive plan
}

TEST_CASE("The AI stops building farms past what its food villagers can work") {
    // A farm feeds exactly one villager (Building::occupied_by), so farms
    // beyond the food workforce produce nothing. The old target scaled with
    // the TOTAL workforce on the assumption that ~half of it ends up on food,
    // which nothing enforces -- observed in a real game as ~10 farms with one
    // occupied.
    // Checked at the moment each farm is BUILT, not as a standing invariant:
    // farms are permanent while the food workforce moves around constantly
    // (the resource-weight rebalance shifts villagers between food and
    // wood/oil/iron all game), so a farm count that was justified when it
    // went up can legitimately exceed the workforce ten seconds later. What
    // the fix actually governs is the build DECISION, so that's what this
    // pins down.
    for (uint64_t seed = 400; seed < 410; ++seed) {
        ScenarioHarness h(2, seed);
        int prev_farms = 0;
        for (int i = 0; i < 20 * 60 * 12; ++i) {
            h.world->update(1.0 / 20.0);
            if (i % 10) continue; // 0.5s: fine enough to attribute a new farm to the moment it appeared
            int farms = count_buildings(*h.world, 1, "farm", /*skip_exhausted=*/true);
            if (farms > prev_farms) {
                int fw = food_workers_of(*h.world, 1);
                // +4 is ai_build's starvation allowance (food_workers + 4),
                // the loosest the target ever gets; +1 of slack for a worker
                // reassigned within the sampling window.
                INFO("seed=" << seed << " t=" << i / 20 << "s farms=" << farms << " food_workers=" << fw);
                REQUIRE(farms <= fw + 5);
            }
            prev_farms = farms;
        }
    }
}
