// End-to-end sanity tests driving World::update() for real ticks -- this
// is the first point where bugs in the freshly-written, never-before-run
// World/Unit/Building/Control-AI code would actually surface, as opposed
// to merely compiling. Uses a flat, waterless "random" map (water=false)
// to keep movement/pathing simple; combat/gather/construction/AI are
// each exercised directly rather than through order_move-style input
// plumbing (which is a Phase C / session-layer concern, not sim's).
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <utility>

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
          world(data, bonuses, control, rng, events, 40, 40, "random", /*water=*/false) {}
};

} // namespace

TEST_CASE("World terrain generation is deterministic for a given seed") {
    Harness a(2, 12345);
    Harness b(2, 12345);
    REQUIRE(a.world.terrain == b.world.terrain);

    Harness c(2, 99999);
    REQUIRE_FALSE(a.world.terrain == c.world.terrain);
}

TEST_CASE("Civilian gathers a resource and deposits it at the base") {
    Harness h(2, 1);
    EntityRef base = h.world.spawn_building("base", 0, 500, 500);
    EntityRef tree = h.world.spawn_resource("tree", 540, 500); // within gather reach once adjacent
    EntityRef civ = h.world.spawn_unit("civilian", 0, 540, 500);

    Unit* u = h.world.get(civ);
    REQUIRE(u != nullptr);
    u->gather_target = tree;

    double start_wood = h.control.teams[0].res["wood"];
    for (int i = 0; i < 20 * 30; ++i) h.world.update(1.0 / 20.0); // 30 sim-seconds

    Resource* r = h.world.get_resource(tree);
    REQUIRE(r != nullptr);
    REQUIRE(r->res.amount < r->res.start_amount); // some wood was harvested
    REQUIRE(h.control.teams[0].res["wood"] > start_wood); // and deposited at the base
    (void)base;
}

TEST_CASE("A construction foundation completes when a builder is assigned") {
    Harness h(2, 2);
    EntityRef foundation = h.world.spawn_building("barracks", 0, 500, 500, /*constructing=*/true);
    EntityRef builder = h.world.spawn_unit("civilian", 0, 500, 540);

    Building* b = h.world.get_building(foundation);
    REQUIRE_FALSE(b->complete);
    Unit* u = h.world.get(builder);
    u->build_target = foundation;

    // Barracks builds at the generic 3/reload-cycle rate (not house/refinery's
    // 5, or base/fortress's 1 -- see update_unit's construction rate table),
    // so from 0 to 100 takes ~34 sim-seconds at one reload cycle/second; 40s
    // leaves headroom.
    for (int i = 0; i < 20 * 40 && !b->complete; ++i) h.world.update(1.0 / 20.0);

    REQUIRE(b->complete);
    REQUIRE(b->common.hp == b->full_max_hp);
}

TEST_CASE("AI build priority falls through to a lower item when higher ones are already met, "
          "and expands to a second base once at War era with resources to spare") {
    // Regression test for a real bug found via tournament metrics: the old
    // ai_build was a strict if/else chain that only ever proposed ONE
    // building per call (the first unmet priority) and did nothing else
    // that tick if it wasn't affordable/placeable -- silently blocking every
    // lower-priority item forever, including any war-era base expansion
    // (bases_built stayed exactly 1 across 40 team-instances in a 20-match
    // tournament, even in matches that reached War Era with resources to
    // spare). ai_build now builds an ordered candidate list and tries each
    // in turn, stopping at the first one that's both affordable and has a
    // real build spot -- this drives that fallthrough all the way down to
    // the war-era base-expansion branch by pre-satisfying every higher-
    // priority condition with REAL completed buildings (not a synthetic
    // list -- ai_build is private, only reachable through the real
    // Control::update_ai -> ai_tick -> ai_build dispatch, same "drive
    // through real order-dispatch" rule as everywhere else in this suite),
    // spread far from the home base so they don't crowd its build-spot ring.
    // Uses team 1, not 0 -- place_building has a fog-of-war explored-tiles
    // restriction that ONLY applies to team 0 (the conventional human/local
    // player; see World::place_building's comment), which silently no-ops
    // an AI-team placement test run under team 0 even with a valid spot.
    // Every real skirmish/tournament AI opponent is team 1+ anyway (team 0
    // is human by convention -- see ai_tick's header comment), so team 1
    // is the faithful choice here, not just a workaround.
    Harness h(2, 7);
    EntityRef base = h.world.spawn_building("base", 1, 1000, 1000);
    double fx = 1000, fy = 3000; // "far" cluster -- well outside ai_build_spot's ring search
    h.world.spawn_building("barracks", 1, fx, fy);
    h.world.spawn_building("barracks", 1, fx + 100, fy);
    h.world.spawn_building("house", 1, fx + 200, fy);
    h.world.spawn_building("house", 1, fx + 300, fy);
    h.world.spawn_building("tower", 1, fx + 400, fy);
    h.world.spawn_building("academy", 1, fx + 500, fy);
    h.world.spawn_building("refinery", 1, fx + 600, fy);
    h.world.spawn_building("market", 1, fx + 700, fy);
    for (int i = 0; i < 7; ++i) h.world.spawn_building("farm", 1, fx + i * 100, fy + 100);

    Team& td = h.control.teams[1];
    td.civ = 0;   // UK: "base" isn't civ-excluded (unlike e.g. waffen)
    td.era = 2;   // War era -- required for the base-expansion branch
    td.res["food"] = td.res["wood"] = td.res["oil"] = td.res["iron"] = 2000; // ample surplus
    // is_ai is already true for team 1 by default (Control's ctor); team 0
    // stays is_ai == false by default too, so it never competes for a build.

    auto count_bases = [&] {
        int n = 0;
        for (auto ref : h.world.active_buildings) {
            Building* b = h.world.get_building(ref);
            if (b && b->common.team == 1 && b->name == "base") ++n;
        }
        return n;
    };
    REQUIRE(count_bases() == 1);

    // Team 1's decision cadence at default (Normal) difficulty is 1s -- feed
    // it well past that in small dt chunks (matching every other call site's
    // fixed-timestep convention) so ai_tick's per-team gate actually fires.
    for (int i = 0; i < 60 && count_bases() < 2; ++i) h.control.update_ai(1.0 / 20.0, h.world);

    REQUIRE(count_bases() == 2);
    (void)base;
}

TEST_CASE("War-era base expansion survives a factory continuously competing for iron") {
    // Tournament follow-up to the test above: 48 real self-play matches
    // across two batches never once showed bases_built > 1, even the
    // handful that reached War Era -- traced to ai_train's factory/
    // barracks/academy/airbase/shipyard queues refilling on every AI tick
    // whenever there's room (queue.size() < 3) and it's affordable, which
    // keeps iron skimmed back down since every advanced unit is iron-heavy
    // (light tank 110, heavy tank 200, artillery 100, fighter 120, bomber
    // 350 -- catalog.json). The flush_for_expansion gate got loosened from
    // wood>=500/iron>=200 to wood>=400/iron>=150 (a 50% margin over the
    // 275/100 base cost) as a result, but that number was chosen by
    // reasoning about catalog costs, not observed against a live economy
    // that's actually spending. This test closes that gap: team 1 gets a
    // factory too (era >= 1 unlocks it in ai_train), so light/heavy
    // tank production competes for iron on every tick right alongside the
    // base-expansion check, same as it would in a real match -- and runs
    // long enough (2.5 sim-minutes) for that contention to matter, not
    // just a handful of ticks.
    Harness h(2, 8);
    h.world.spawn_building("base", 1, 1000, 1000);
    double fx = 1000, fy = 3000;
    h.world.spawn_building("barracks", 1, fx, fy);
    h.world.spawn_building("barracks", 1, fx + 100, fy);
    h.world.spawn_building("house", 1, fx + 200, fy);
    h.world.spawn_building("house", 1, fx + 300, fy);
    h.world.spawn_building("tower", 1, fx + 400, fy);
    h.world.spawn_building("academy", 1, fx + 500, fy);
    h.world.spawn_building("refinery", 1, fx + 600, fy);
    h.world.spawn_building("market", 1, fx + 700, fy);
    h.world.spawn_building("factory", 1, fx + 800, fy); // ongoing iron sink, same as ai_train's real usage
    for (int i = 0; i < 7; ++i) h.world.spawn_building("farm", 1, fx + i * 100, fy + 100);

    Team& td = h.control.teams[1];
    td.civ = 0;
    td.era = 2;
    // A believable, not maxed-out, War-era economy -- comfortably above the
    // loosened gate but not so far above it that the factory's draw can
    // never matter (2000 flat, as in the test above, would take many
    // tank-cycles to threaten either threshold and wouldn't actually
    // exercise the contention this test exists to check).
    td.res["food"] = 800;
    td.res["wood"] = 800;
    td.res["oil"] = 800;
    td.res["iron"] = 400;

    auto count_bases = [&] {
        int n = 0;
        for (auto ref : h.world.active_buildings) {
            Building* b = h.world.get_building(ref);
            if (b && b->common.team == 1 && b->name == "base") ++n;
        }
        return n;
    };
    REQUIRE(count_bases() == 1);

    // 2.5 sim-minutes at the fixed 1/20 dt every other test/tool in this
    // codebase uses -- long enough for several factory train cycles to
    // fire (each unit takes several seconds) while still finishing fast.
    for (int i = 0; i < 20 * 150 && count_bases() < 2; ++i) h.control.update_ai(1.0 / 20.0, h.world);

    REQUIRE(count_bases() == 2);
}

TEST_CASE("Team::blitz instantly completes construction and training") {
    // Chat-bar "blitz" cheat (GameClient::submit_chat): a building under
    // construction finishes on the very next tick, and so does whatever's
    // at the front of a complete building's production queue -- both
    // driven by the same Building::construction/percent mechanism
    // (building_behavior.cpp).
    Harness h(2, 30);
    h.control.teams[0].blitz = true;
    EntityRef foundation = h.world.spawn_building("barracks", 0, 500, 500, /*constructing=*/true);
    Building* b = h.world.get_building(foundation);
    REQUIRE_FALSE(b->complete);

    h.world.update(1.0 / 20.0);
    REQUIRE(b->complete);

    b->queue.push_back("rifleman");
    size_t units_before = h.world.active_units.size();
    h.world.update(1.0 / 20.0);
    REQUIRE(b->queue.empty());
    REQUIRE(h.world.active_units.size() == units_before + 1);
}

TEST_CASE("Team::blitz delivers gathered resources instantly with no carry trip") {
    Harness h(2, 31);
    h.control.teams[0].blitz = true;
    EntityRef tree = h.world.spawn_resource("tree", 540, 500);
    EntityRef civ_ref = h.world.spawn_unit("civilian", 0, 540, 500);
    Unit* civ = h.world.get(civ_ref);
    civ->gather_target = tree;

    double wood_before = h.control.teams[0].res["wood"];
    h.world.update(1.0 / 20.0);

    REQUIRE(h.control.teams[0].res["wood"] > wood_before); // delivered straight to the stockpile
    REQUIRE(civ->carry == 0.0);                            // never actually carried
}

TEST_CASE("Several builders converging on one foundation all keep building") {
    // Regression test: several builders queuing for perimeter slots around
    // the same foundation used to get jostled apart by a push-apart pass, so
    // their distance to the foundation fluctuated (not stuck, just
    // contested) even after arriving. The build-approach give-up check used
    // to measure exactly that fluctuating distance-to-target and abandon
    // the order (build_target cleared) if it didn't shrink monotonically
    // within 1.5s -- easy to trigger by accident with several units
    // crowding one small foundation, leaving a builder standing right next
    // to it but not actually building. It now tracks the unit's own
    // (near-zero when jostled-but-present) displacement instead, over a
    // longer window.
    //
    // Builders are spread across all four sides (rather than bunched in a
    // row on one side) so their individually-clamped perimeter points (see
    // side_edge_point) don't collide -- units no longer get shoved apart if
    // they physically can't all fit (see "units should still block other
    // units" -- pushing was removed), so packing more than ~2 builders onto
    // one 64px side without spacing them out is a real, separate crowding
    // problem, not what this test is about.
    Harness h(2, 7);
    EntityRef foundation = h.world.spawn_building("house", 0, 500, 500, /*constructing=*/true);
    std::vector<EntityRef> builders;
    for (auto [sx, sy] : {std::pair{470.0, 600.0}, std::pair{530.0, 600.0}, std::pair{500.0, 400.0},
                          std::pair{400.0, 500.0}, std::pair{600.0, 500.0}}) {
        EntityRef ref = h.world.spawn_unit("civilian", 0, sx, sy);
        Unit* u = h.world.get(ref);
        u->build_target = foundation;
        builders.push_back(ref);
    }
    h.world.prime();

    Building* b = h.world.get_building(foundation);
    REQUIRE_FALSE(b->complete);
    bool any_gave_up_early = false;
    for (int i = 0; i < 20 * 30 && !b->complete; ++i) {
        h.world.update(1.0 / 20.0);
        if (!b->complete) {
            for (auto ref : builders) {
                Unit* u = h.world.get(ref);
                if (u && u->build_target != foundation) any_gave_up_early = true;
            }
        }
    }

    REQUIRE(b->complete);
    REQUIRE_FALSE(any_gave_up_early);
}

TEST_CASE("Several builders spread across the perimeter instead of queuing") {
    // Regression test: advance_to_building's per-side reachability probe
    // used a fixed side-midpoint as the final walked-to target (needed to
    // reliably judge whether a side is externally blocked -- see the
    // "does not flip back" test above), which meant every builder
    // approaching the same reachable side beelined for the exact same
    // pixel -- the first arrival then physically blocked the rest from
    // ever reaching the foundation, forming a visible queue instead of
    // spreading across the rest of that (otherwise wide open) side.
    Harness h(2, 9);
    EntityRef foundation = h.world.spawn_building("house", 0, 500, 500, /*constructing=*/true);
    std::vector<EntityRef> builders;
    for (int i = 0; i < 4; ++i) {
        // >2*TILE from the foundation so advance_to_building's A*-routing
        // path (not the plain close-range fallback) is what's exercised.
        EntityRef ref = h.world.spawn_unit("civilian", 0, 460 + i * 30, 700);
        Unit* u = h.world.get(ref);
        u->build_target = foundation;
        builders.push_back(ref);
    }
    h.world.prime();

    h.world.update(1.0 / 20.0); // one tick is enough for them all to commit to a target

    std::set<std::pair<double, double>> distinct_targets;
    for (auto ref : builders) {
        Unit* u = h.world.get(ref);
        if (u && u->approach_target) distinct_targets.insert({u->approach_target->x, u->approach_target->y});
        else if (u) distinct_targets.insert({u->common.x, u->common.y}); // already arrived and working
    }
    REQUIRE(distinct_targets.size() > 1);

    Building* b = h.world.get_building(foundation);
    for (int i = 0; i < 20 * 30 && !b->complete; ++i) h.world.update(1.0 / 20.0);
    REQUIRE(b->complete);
}

TEST_CASE("Two opposing units fight when in range and one eventually dies") {
    Harness h(2, 3);
    EntityRef a_ref = h.world.spawn_unit("rifleman", 0, 500, 500);
    EntityRef b_ref = h.world.spawn_unit("rifleman", 1, 520, 500); // within sight/range

    bool someone_died = false;
    for (int i = 0; i < 20 * 60; ++i) {
        h.world.update(1.0 / 20.0);
        Unit* a = h.world.get(a_ref);
        Unit* b = h.world.get(b_ref);
        if (!a || !b || !a->common.alive || !b->common.alive) { someone_died = true; break; }
    }
    REQUIRE(someone_died);
}

TEST_CASE("A fire hazard burns a rifleman standing in it but spares a tank") {
    // World::fires is normally populated by artillery/ship impacts (see
    // spawn_missile_impact_fx, projectile_behavior.cpp) -- pushed directly
    // here for a deterministic, RNG-free test of the damage-over-time tick
    // itself (World::update's fire sweep, world.cpp).
    Harness h(2, 5);
    // Same team so they never fight each other -- the test is isolating the
    // fire tick's effect, not combat.
    EntityRef rifle_ref = h.world.spawn_unit("rifleman", 0, 500, 500);
    EntityRef tank_ref = h.world.spawn_unit("tank", 0, 505, 505); // both inside kFireRadius of the fire
    Unit* rifle = h.world.get(rifle_ref);
    Unit* tank = h.world.get(tank_ref);
    REQUIRE(rifle != nullptr);
    REQUIRE(tank != nullptr);
    double rifle_start_hp = rifle->common.hp;
    double tank_start_hp = tank->common.hp;

    h.world.fires.push_back({500.0, 500.0, 3.0});
    // A couple extra ticks past the nominal 3s so the hazard is safely
    // expired regardless of dt-accumulation floating-point slop landing
    // its timer a hair above or below exactly 0 on the boundary tick.
    for (int i = 0; i < 20 * 3 + 2; ++i) h.world.update(1.0 / 20.0);

    // Re-fetch rather than reuse the pre-loop pointers -- SlotMap contents
    // can move/erase across update() calls (see the "Two opposing units
    // fight" test's same re-fetch-after-each-tick pattern above).
    rifle = h.world.get(rifle_ref);
    tank = h.world.get(tank_ref);
    REQUIRE(rifle != nullptr);
    REQUIRE(tank != nullptr);
    REQUIRE(rifle->common.hp < rifle_start_hp); // burned
    REQUIRE(tank->common.hp == tank_start_hp);  // exempt, per TANK
    REQUIRE(h.world.fires.empty());             // hazard expired on schedule
}

TEST_CASE("An impassable decoration blocks movement but a passable one doesn't") {
    // World::decorations is normally populated by new_from_level
    // (scenario.cpp) from a campaign Level's TerrainFeature::resource
    // entries -- pushed directly here to test rebuild_occupied()'s
    // decoration_tiles_ set / passable()'s check of it in isolation.
    Harness h(2, 9);
    h.world.decorations.push_back({500.0, 500.0, "fence"});   // impassable, see decoration_kinds()
    h.world.decorations.push_back({600.0, 500.0, "pebbles"}); // passable
    h.world.prime(); // rebuild_occupied() -- decoration_tiles_ isn't populated until this runs

    REQUIRE_FALSE(h.world.passable(/*is_air=*/false, /*is_ship=*/false, 500.0, 500.0));
    REQUIRE(h.world.passable(/*is_air=*/false, /*is_ship=*/false, 600.0, 500.0));
    // Air units ignore all ground obstacles, decorations included.
    REQUIRE(h.world.passable(/*is_air=*/true, /*is_ship=*/false, 500.0, 500.0));
}

TEST_CASE("A message trigger fires once when a unit collides with it, then never again") {
    // World::message_triggers is normally populated by new_from_level
    // (scenario.cpp) from a campaign Level's "message"-type MapEvent
    // entries -- pushed directly here to test the collision sweep (World::update)
    // in isolation, same approach as the fire hazard/decoration tests
    // above.
    auto has_map_message = [](const EventBus& events) {
        for (auto& ev : events.events()) {
            if (ev.type == EventType::Notify && ev.key == "map_message") return true;
        }
        return false;
    };

    Harness h(2, 11);
    h.world.message_triggers.push_back({500.0, 500.0, "Hello from the map author!"});
    EntityRef rifle_ref = h.world.spawn_unit("rifleman", 0, 900, 900); // nowhere near the trigger yet
    h.world.update(1.0 / 20.0);

    // A fresh tick can push unrelated Sound/Effect events of its own (unit
    // upkeep, AI, etc.) -- only assert the ABSENCE of our specific event,
    // not that nothing happened at all.
    REQUIRE_FALSE(has_map_message(h.events));
    REQUIRE_FALSE(h.world.message_triggers[0].triggered);
    h.events.clear();

    Unit* rifle = h.world.get(rifle_ref);
    REQUIRE(rifle != nullptr);
    rifle->common.x = 505.0; // inside kMessageTriggerRadius of (500,500)
    rifle->common.y = 505.0;
    h.world.update(1.0 / 20.0);

    REQUIRE(h.world.message_triggers[0].triggered);
    bool found = false;
    for (auto& ev : h.events.events()) {
        if (ev.type == EventType::Notify && ev.key == "map_message") {
            REQUIRE(ev.text == "Hello from the map author!");
            found = true;
        }
    }
    REQUIRE(found);

    // Still standing right in it, but already triggered -- must not
    // re-fire (it's a one-shot story-beat marker, not a repeating hazard).
    h.events.clear();
    h.world.update(1.0 / 20.0);
    REQUIRE_FALSE(has_map_message(h.events));
}

TEST_CASE("A unit crossing paths with oncoming traffic doesn't give up its move order") {
    // Regression test: a unit that got shouldered aside for a tick or two
    // by oncoming traffic (blocked_by_unit / resolve_overlap) used to trip
    // the "no progress in 1.5s" give-up check on the very first bad
    // window, silently cancelling the whole move order even though the
    // unit was nowhere near its destination -- reported as "the villager
    // got pushed back and then gave up, even though he wasn't close to his
    // destination". It now gets two free repath attempts before giving up
    // for good (see Unit::stall_strikes). Two units cross paths almost
    // head-on (a slight lateral offset, like real traffic passing rather
    // than a perfectly aligned 1-tile-wide standoff neither could ever
    // step aside from) on open ground, so the fix is exercised the way it
    // actually comes up in play, not an artificially unsolvable deadlock.
    Harness h(2, 21);
    EntityRef westbound = h.world.spawn_unit("civilian", 0, 400, 600);
    EntityRef eastbound = h.world.spawn_unit("civilian", 0, 900, 615);
    h.world.order_move(westbound, 900, 600);
    h.world.order_move(eastbound, 400, 615);

    for (int i = 0; i < 20 * 30; ++i) h.world.update(1.0 / 20.0);

    Unit* uw = h.world.get(westbound);
    Unit* ue = h.world.get(eastbound);
    REQUIRE(uw != nullptr);
    REQUIRE(ue != nullptr);
    REQUIRE(uw->common.x > 800.0); // reached (or nearly reached) the east side
    REQUIRE(ue->common.x < 500.0); // reached (or nearly reached) the west side
}

TEST_CASE("An attack order continues to the target's last position if it dies first") {
    // Regression test: ordering a unit to attack something that dies
    // before the attacker arrives (or gets a single shot off) used to just
    // stop the attacker dead in place instead of continuing on -- reported
    // as "when i tell a unit to attack something, if it dies before that
    // unit gets there/starts attacking, it should 'attack move' to the
    // last location that unit was".
    Harness h(2, 22);
    h.control.teams[1].is_ai = false; // keep the target stationary/deterministic
    EntityRef attacker = h.world.spawn_unit("rifleman", 0, 500, 500);
    EntityRef target = h.world.spawn_unit("rifleman", 1, 500, 900); // far enough that death precedes arrival
    h.world.order_attack(attacker, target);

    // Let the attacker get partway there, well outside its attack range,
    // then kill the target directly (bypassing combat) before it ever
    // gets a chance to actually engage.
    for (int i = 0; i < 20 * 5; ++i) h.world.update(1.0 / 20.0);
    Unit* ua = h.world.get(attacker);
    REQUIRE(ua != nullptr);
    REQUIRE(std::hypot(ua->common.x - 500, ua->common.y - 900) > 100.0); // nowhere near yet
    Unit* ut = h.world.get(target);
    REQUIRE(ut != nullptr);
    double target_x = ut->common.x, target_y = ut->common.y;
    ut->common.hp = 0;
    ut->common.alive = false;

    for (int i = 0; i < 20 * 20; ++i) h.world.update(1.0 / 20.0);

    ua = h.world.get(attacker);
    REQUIRE(ua != nullptr);
    REQUIRE(std::hypot(ua->common.x - target_x, ua->common.y - target_y) < 50.0);
}

TEST_CASE("Two gatherers sent to the same tree together start working from any approach angle") {
    // Mirrors an actual "select 2 villagers, right-click a tree" order:
    // both start away from the resource and are ordered via
    // World::order_gather (not a direct field assignment) at the same
    // moment, so neither is pre-settled -- whoever gets there first stakes
    // a spot, and the other has to route around them while STILL
    // approaching, not after they're already parked. Tried from 8 evenly
    // spaced approach angles since the reported symptom ("often tries to
    // go the wrong way ... depends on the angle of approach") was
    // angle-sensitive.
    for (int a = 0; a < 8; ++a) {
        double angle = a * (2.0 * M_PI / 8.0);
        Harness h(2, 70 + a);
        EntityRef tree = h.world.spawn_resource("tree", 600, 600);
        EntityRef a_ref = h.world.spawn_unit("civilian", 0, 600, 690);
        double sx = 600 + std::cos(angle) * 90.0, sy = 600 + std::sin(angle) * 90.0;
        EntityRef b_ref = h.world.spawn_unit("civilian", 0, sx, sy);
        h.world.order_gather(a_ref, tree);
        h.world.order_gather(b_ref, tree);

        bool a_working = false, b_working = false;
        for (int i = 0; i < 20 * 10 && !(a_working && b_working); ++i) {
            h.world.update(1.0 / 20.0);
            Unit* av = h.world.get(a_ref);
            Unit* bv = h.world.get(b_ref);
            REQUIRE(av != nullptr);
            REQUIRE(bv != nullptr);
            a_working = a_working || av->working;
            b_working = b_working || bv->working;
        }
        REQUIRE(a_working);
        REQUIRE(b_working);
    }
}

TEST_CASE("A villager that genuinely can't fit at a resource gives up cleanly instead of looping forever") {
    // Regression test: reported as "a villager is trying to collect a
    // resource but another villager is working on that resource ... keeps
    // getting caught and often tries to go the wrong way ... oscillate
    // without success". Root cause was two-fold:
    //
    // 1. update_gather's stuck detection compared raw PER-TICK
    //    displacement to a 0.6px threshold -- a villager oscillating
    //    against a neighbor can easily move several px on any single tick
    //    while making zero net progress over a couple of seconds, so the
    //    threshold was never met and the stall was never detected at all.
    //    Replaced with a window-based check (same pattern as move_goal's
    //    progress_check_t) comparing actual distance-to-target shrinkage
    //    over ~1.5s.
    // 2. Once that correctly detects a stall and looks for a different
    //    same-type resource, finding none left gather_rtype set -- so the
    //    very next tick's "target disappeared, re-seek nearest same-type
    //    resource" fallback (a separate, pre-existing piece of logic)
    //    immediately reselected the SAME resource, walking straight back
    //    into the same stall: give up, reacquire, give up, forever. Fixed
    //    by clearing gather_rtype too when no alternative exists.
    //
    // 4 villagers around one tree, spaced so at most 3 can actually fit at
    // once: the 4th should give up and go idle within a few seconds rather
    // than oscillating or looping indefinitely.
    Harness h(2, 91);
    EntityRef tree = h.world.spawn_resource("tree", 600, 600);
    std::vector<EntityRef> vs;
    for (int i = 0; i < 4; ++i) {
        double angle = i * (2.0 * M_PI / 4.0);
        double sx = 600 + std::cos(angle) * 80.0, sy = 600 + std::sin(angle) * 80.0;
        vs.push_back(h.world.spawn_unit("civilian", 0, sx, sy));
    }
    for (auto r : vs) h.world.order_gather(r, tree);

    // Track whether each villager EVER started working, not just whether
    // it's working at the exact final tick -- with no dropoff building in
    // this minimal harness, a villager that successfully gathers will
    // fill its carry capacity and stop (correctly) waiting for a delivery
    // point that doesn't exist here, which would otherwise look
    // indistinguishable from never having started at all.
    std::vector<bool> ever_worked(4, false);
    for (int i = 0; i < 20 * 15; ++i) {
        h.world.update(1.0 / 20.0);
        for (int k = 0; k < 4; ++k) {
            Unit* u = h.world.get(vs[k]);
            if (u && u->working) ever_worked[k] = true;
        }
    }

    int worked_count = 0, idle_count = 0;
    for (int k = 0; k < 4; ++k) {
        Unit* u = h.world.get(vs[k]);
        REQUIRE(u != nullptr);
        if (ever_worked[k]) {
            ++worked_count;
        } else if (!u->gather_target.valid()) {
            // Idle (gave up, no gather order left) is an acceptable
            // terminal state for the one that can't fit.
            ++idle_count;
        }
        // Anything neither ever-worked nor idle would be still actively
        // (and fruitlessly) approaching after 15s -- the oscillate/loop
        // forever bug this test guards against.
    }
    REQUIRE(worked_count + idle_count == 4);
    REQUIRE(worked_count >= 3);
}

TEST_CASE("A mover routes around two stationary units with a real passable gap between them") {
    // Two blockers 70px apart (their 28px exclusion circles leave a genuine
    // ~14px-plus gap between them) -- a unit ordered straight through
    // should route cleanly around, same as the single-blocker case.
    Harness h(2, 131);
    h.world.spawn_unit("civilian", 0, 565, 600);
    h.world.spawn_unit("civilian", 0, 635, 600);
    EntityRef m_ref = h.world.spawn_unit("civilian", 0, 600, 700);
    h.world.order_move(m_ref, 600, 500);

    bool arrived = false;
    for (int i = 0; i < 20 * 12 && !arrived; ++i) {
        h.world.update(1.0 / 20.0);
        Unit* m = h.world.get(m_ref);
        REQUIRE(m != nullptr);
        if (!m->move_goal) arrived = true;
    }
    REQUIRE(arrived);
    Unit* m = h.world.get(m_ref);
    REQUIRE(std::hypot(m->common.x - 600.0, m->common.y - 500.0) < 5.0);
}

// NOTE: two blockers closer together than 2*kSeparation (kSeparation is
// unit_behavior.cpp's kBodyRadius * 1.4) leave NO passable gap at all --
// their exclusion circles fully overlap into one
// wider combined obstacle. A mover sent straight at the gap has to work
// out it needs to swing wide around the WHOLE combined shape, which this
// purely local/reactive avoidance system was not able to reliably solve in
// testing (several different fixes were tried; each either didn't resolve
// it or regressed other, more realistic scenarios -- see Unit::avoid_offset's
// comment in unit.h). It does still recover honestly: the unit tries for a
// few seconds and then gives up the order cleanly (Unit::stall_strikes)
// rather than looping or freezing forever. No test is asserted for that
// specific configuration; it's a known, documented limitation rather than
// a regression to guard.

TEST_CASE("A unit ordered to move across open terrain reaches the exact clicked point") {
    // Regression test: reported as "units stop short of where they are
    // supposed to go to ... even if im just moving one unit in open map".
    // A prior fix (routing cleanly around a stationary unit, see the test
    // above) widened the "close enough, treat as arrived" fallback so a
    // unit wouldn't grind forever against a destination point that's
    // genuinely obstructed by another unit -- but that fallback checked
    // distance alone, so it also fired on completely open terrain the
    // moment the unit got within a body-radius of its destination on some
    // ordinary tick, before its natural exact-arrival step ever got a
    // chance to run. Now gated on the destination actually being
    // obstructed, not just nearby.
    Harness h(2, 61);
    EntityRef m_ref = h.world.spawn_unit("civilian", 0, 500, 500);
    h.world.order_move(m_ref, 500, 300);
    bool arrived = false;
    for (int i = 0; i < 20 * 10 && !arrived; ++i) {
        h.world.update(1.0 / 20.0);
        Unit* m = h.world.get(m_ref);
        REQUIRE(m != nullptr);
        if (!m->move_goal) {
            arrived = true;
            REQUIRE(m->common.x == 500.0);
            REQUIRE(m->common.y == 300.0);
        }
    }
    REQUIRE(arrived);
}

TEST_CASE("A real move order routes cleanly around one stationary unit, no multi-second thrashing") {
    // Regression test: reported as "even just one villager going around
    // another stationary unit ... 10+ seconds before it even gets to the
    // other side". Earlier ad hoc single-unit-avoidance checks in this file
    // used move_goal set directly, which bypasses World::order_move's
    // astar() call entirely -- real player orders always go through
    // order_move, and that turned out to matter: astar picks path
    // waypoints with zero awareness of other units (only terrain/
    // buildings, see World::passable's callers), so a waypoint can land
    // deep inside another unit's blocking radius (kSeparation) and
    // become permanently unreachable to the exact pixel. Before the fix,
    // the mover would grind at point-blank range trying to close a gap it
    // geometrically could never close, visibly thrashing for several
    // seconds until the much slower top-level stall timer (~1.5s per
    // attempt) eventually forced enough re-paths to escape by chance.
    // Fixed by widening the "close enough, move on to the next waypoint"
    // tolerance from a flat body-radius to account for the worst case (a
    // waypoint coinciding exactly with a blocking unit's center).
    Harness h(2, 61);
    h.world.spawn_unit("civilian", 0, 500, 500); // stationary blocker, same team, sitting on the direct route
    EntityRef m_ref = h.world.spawn_unit("civilian", 0, 500, 600);
    h.world.order_move(m_ref, 500, 400); // real order dispatch, not a direct move_goal assignment

    int prev_facing = 0;
    int flips = 0;
    int arrived_tick = -1;
    for (int i = 0; i < 20 * 15; ++i) {
        h.world.update(1.0 / 20.0);
        Unit* m = h.world.get(m_ref);
        REQUIRE(m != nullptr);
        if (prev_facing != 0 && m->facing != prev_facing) ++flips;
        prev_facing = m->facing;
        if (!m->move_goal) { arrived_tick = i; break; }
    }
    REQUIRE(arrived_tick >= 0);       // didn't run out the clock still stuck
    REQUIRE(arrived_tick < 20 * 5);   // well under 5s -- not the multi-second thrash this replaced
    REQUIRE(flips < 10);              // smooth pass, not visible back-and-forth
}

TEST_CASE("Two groups of six units crossing paths all get through without excessive oscillation") {
    // Regression test: reported as "units moving against each other doesn't
    // work currently, there is a lot of oscillation and confusion". A
    // single pair avoiding each other (see the head-on and single-blocker
    // cases covered elsewhere) turns out to behave fine on its own -- the
    // failure mode specific to this report only shows up under real local
    // DENSITY, where each unit's steering decision changes what its
    // neighbors see as clear, which can cascade. Two six-unit columns
    // walking straight through each other is close to the density a real
    // group-vs-group crossing produces.
    //
    // Asserts two things a player would actually notice: every unit
    // eventually gets to the far side (nobody permanently wedged), and the
    // total number of hard facing-direction reversals across the whole
    // 12-unit crossing stays bounded -- a smooth crossing has each unit
    // commit to a side and swing through it once or twice; unbounded
    // reversals is what "oscillation" looks like from the outside.
    Harness h(2, 61);
    std::vector<EntityRef> east, west;
    for (int i = 0; i < 6; ++i) {
        double y = 470 + i * 12;
        east.push_back(h.world.spawn_unit("civilian", 0, 400, y));
        west.push_back(h.world.spawn_unit("civilian", 0, 600, y));
    }
    for (auto r : east) h.world.get(r)->move_goal = Vec2{600, h.world.get(r)->common.y};
    for (auto r : west) h.world.get(r)->move_goal = Vec2{400, h.world.get(r)->common.y};

    auto all = east;
    all.insert(all.end(), west.begin(), west.end());
    std::vector<int> prev_facing(all.size(), 0);
    std::vector<int> flips(all.size(), 0);
    for (int i = 0; i < 20 * 25; ++i) {
        h.world.update(1.0 / 20.0);
        for (size_t k = 0; k < all.size(); ++k) {
            Unit* u = h.world.get(all[k]);
            if (!u) continue;
            if (prev_facing[k] != 0 && u->facing != prev_facing[k]) ++flips[k];
            prev_facing[k] = u->facing;
        }
    }

    int total_flips = 0;
    for (size_t k = 0; k < all.size(); ++k) {
        Unit* u = h.world.get(all[k]);
        REQUIRE(u != nullptr);
        REQUIRE_FALSE(u->move_goal); // reached the far side (or gave up cleanly -- neither is "stuck")
        total_flips += flips[k];
    }
    REQUIRE(total_flips < 30); // generous ceiling well above smooth-crossing levels (observed ~11)
}

TEST_CASE("A unit boxed in on every forward heading backs away instead of freezing, "
          "and cleanly gives up if truly encircled") {
    // Regression test: reported as "sometimes the unit couldn't find its way
    // around two units when it was wedged between them, it didn't realise it
    // needed to basically go backwards first". Ten stationary blockers ring
    // the mover across a 300-degree arc centered on its goal direction,
    // leaving only a 60-degree gap directly behind it -- every forward-
    // biased steering candidate step_toward tries (wide swing and tight
    // squeeze, both within ~103 degrees of the goal heading) is blocked, so
    // only the retreat tier (flee the local crowd) can possibly move the
    // unit at all.
    //
    // A near-full ring like this is a harder case than anything normal play
    // produces (usually 2-3 units, not 10 in a deliberate near-encirclement)
    // and is a genuine local minimum no purely-local/reactive steering can
    // solve on its own -- that's a known limitation shared by every
    // reactive avoidance scheme, not something specific to this one. So
    // this test doesn't require escaping the ring: it requires (a) the unit
    // actually moves off the spawn point (the bug this replaced: frozen
    // with zero net displacement) and (b) the existing stuck/give-up safety
    // net (Unit::stall_strikes) eventually abandons the order cleanly
    // instead of leaving it silently "stuck" with a live order forever.
    Harness h(2, 59);
    EntityRef w_ref = h.world.spawn_unit("civilian", 0, 500, 500);
    double base = std::atan2(300.0 - 500.0, 0.0); // bearing from (500,500) toward goal (500,300)
    for (int i = 0; i < 10; ++i) {
        double a = base - (5.0 * M_PI / 6.0) + i * (5.0 * M_PI / 3.0) / 9.0; // spans 300 deg around base
        h.world.spawn_unit("civilian", 0, 500 + std::cos(a) * 32.0, 500 + std::sin(a) * 32.0);
    }
    Unit* w = h.world.get(w_ref);
    w->move_goal = Vec2{500, 300};

    double max_disp = 0.0;
    for (int i = 0; i < 20 * 10; ++i) {
        h.world.update(1.0 / 20.0);
        w = h.world.get(w_ref);
        REQUIRE(w != nullptr);
        max_disp = std::max(max_disp, std::hypot(w->common.x - 500.0, w->common.y - 500.0));
    }
    REQUIRE(max_disp > 10.0);      // moved meaningfully -- not frozen at the spawn point
    REQUIRE_FALSE(w->move_goal);   // ...and eventually gave up cleanly rather than staying stuck forever
}

TEST_CASE("A villager gathering a resource doesn't permanently block a second villager sent to the same resource") {
    // Regression test: reported as "when i send two villagers to work on
    // something, one gets stuck behind the one in front while it works".
    // Villager A parks just outside the tree's reach radius (20px) and
    // starts gathering, sitting directly on the straight line between the
    // tree and villager B, who arrives after. Before blocked_by_unit
    // actually blocked (it was previously a stub that always returned
    // false), this wasn't a collision problem so much as a symptom of units
    // being able to walk through each other and pile onto the exact same
    // point -- but the fix (real blocked_by_unit + step_toward's existing
    // detour fan) needs to be verified end-to-end: B must route AROUND A
    // rather than stalling against A's now-solid body, and both must end up
    // actively gathering, not just "not stuck".
    Harness h(2, 41);
    EntityRef tree = h.world.spawn_resource("tree", 500, 500);
    EntityRef a_ref = h.world.spawn_unit("civilian", 0, 500, 470); // 30px north of the tree
    EntityRef b_ref = h.world.spawn_unit("civilian", 0, 500, 400); // further north, same line
    Unit* a = h.world.get(a_ref);
    Unit* b = h.world.get(b_ref);
    a->gather_target = tree;
    a->gather_rtype = 1; // wood, matches World::order_gather's bookkeeping
    b->gather_target = tree;
    b->gather_rtype = 1;

    // 8 sim-seconds is enough for both to arrive and start chopping, but
    // short of a lone civilian fully exhausting the tree (which would make
    // `working` legitimately drop back to false as a separate, unrelated
    // effect -- observed happening somewhere past ~13.5s in practice).
    for (int i = 0; i < 20 * 8; ++i) h.world.update(1.0 / 20.0);

    a = h.world.get(a_ref);
    b = h.world.get(b_ref);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->working);
    REQUIRE(b->working);
    // Both actively gathering means both bodies settled close to the tree
    // without ending up stacked on top of each other.
    double dist = std::hypot(a->common.x - b->common.x, a->common.y - b->common.y);
    // Two civilians are a gatherer/gatherer pair, so the floor is
    // kGathererSeparation (unit_behavior.cpp) = 16.1, not the general
    // kSeparation of 19.6 -- small tolerance for float error.
    REQUIRE(dist >= 15.5);
}

TEST_CASE("Units that end up exactly overlapping push apart to a free location") {
    // Regression test: with no continuous push-apart pass (units only
    // BLOCK each other, see blocked_by_unit's comment), several units that
    // end up exactly coincident -- e.g. all spawned on the same point --
    // used to have no way to ever separate again: every direction looks
    // "blocked" by whichever other unit they're tangled with, forever.
    // resolve_overlap (unit_behavior.cpp) handles just that one case
    // (bodies already closer together than normal movement would ever
    // have allowed), without reintroducing a general personal-space push
    // during ordinary movement.
    Harness h(2, 11);
    std::vector<EntityRef> units;
    for (int i = 0; i < 4; ++i) units.push_back(h.world.spawn_unit("civilian", 0, 500, 500));

    for (int i = 0; i < 20 * 5; ++i) h.world.update(1.0 / 20.0); // 5 sim-seconds, no orders at all

    for (size_t i = 0; i < units.size(); ++i) {
        Unit* ui = h.world.get(units[i]);
        REQUIRE(ui != nullptr);
        for (size_t j = i + 1; j < units.size(); ++j) {
            Unit* uj = h.world.get(units[j]);
            REQUIRE(uj != nullptr);
            double dist = std::hypot(ui->common.x - uj->common.x, ui->common.y - uj->common.y);
            // Civilians, so the pair floor is kGathererSeparation
            // (unit_behavior.cpp) = 16.1 rather than the general
            // kSeparation of 19.6 -- villagers are allowed to bunch tighter
            // against each other so more of them fit around one resource.
            // What this test actually guards is unchanged: coincident
            // bodies must still find their way apart, not stay stacked.
            REQUIRE(dist >= 15.5);
        }
    }
}

TEST_CASE("Overlap resolution never displaces an enemy unit, only allied ones") {
    // Regression test: resolve_overlap (unit_behavior.cpp) used to have no
    // team check at all, so two units ending up coincident would nudge
    // EACH OTHER apart regardless of side -- meaning a player's unit could
    // effectively shove an enemy unit around just by ending up tangled
    // with it (easy to happen approaching to attack, or from group-move
    // clumping). It's now gated on Control::allied: a team-0 and a
    // non-allied team-1 unit spawned exactly coincident must both stay
    // exactly where they were placed, while a same-team pair in the same
    // situation still separates (matching the existing "push apart" test
    // above).
    Harness h(2, 13);
    EntityRef friendly = h.world.spawn_unit("civilian", 0, 500, 500);
    EntityRef enemy = h.world.spawn_unit("civilian", 1, 500, 500);

    for (int i = 0; i < 20 * 5; ++i) h.world.update(1.0 / 20.0); // 5 sim-seconds, no orders at all

    Unit* uf = h.world.get(friendly);
    Unit* ue = h.world.get(enemy);
    REQUIRE(uf != nullptr);
    REQUIRE(ue != nullptr);
    REQUIRE(uf->common.x == 500.0);
    REQUIRE(uf->common.y == 500.0);
    REQUIRE(ue->common.x == 500.0);
    REQUIRE(ue->common.y == 500.0);
}

TEST_CASE("A builder routes around a neighboring building instead of getting stuck") {
    // Regression test: a foundation placed flush against an existing
    // building (now possible since snap() aligns every footprint's edges
    // to the TILE grid -- see the flush-houses test above) can have its
    // *closest* side sit exactly on that neighbor's edge, which is
    // unreachable (you can't stand inside the neighbor). The old
    // build/repair movement walked straight toward whichever side was
    // geometrically closest with a naive step_toward, with no A* routing
    // and no give-up timer -- if that closest side happened to be the one
    // touching the neighbor, the builder would slide along the neighbor's
    // wall forever and the foundation would never complete.
    Harness h(2, 6);
    auto [bx, by] = h.world.snap("base", 500, 500);
    h.world.spawn_building("base", 0, bx, by);

    auto [w, hh] = building_wh("base");
    auto [hx, hy] = h.world.snap("house", bx + w, by); // flush against the base's right edge
    EntityRef foundation = h.world.spawn_building("house", 0, hx, hy, /*constructing=*/true);
    h.world.prime();

    // Builder starts well to the LEFT of the base, so the naive closest
    // point on the house's perimeter is its left edge -- the one flush
    // against (and blocked by) the base.
    EntityRef builder = h.world.spawn_unit("civilian", 0, bx - 150, by);
    Unit* u = h.world.get(builder);
    u->build_target = foundation;

    Building* b = h.world.get_building(foundation);
    REQUIRE_FALSE(b->complete);
    for (int i = 0; i < 20 * 30 && !b->complete; ++i) h.world.update(1.0 / 20.0);

    REQUIRE(b->complete);
}

TEST_CASE("A builder does not flip back to a blocked side once routed around") {
    // Regression test: advance_to_building used to recompute
    // nearest_perimeter_point() fresh every tick once the unit was close
    // enough to stop A*-routing (see advance_to_building's 2*TILE
    // threshold). So a builder that A* had successfully routed around a
    // neighboring building toward a clear side could, once close, have
    // that fresh recompute hand back the geometrically-closest side again
    // -- which can be the very side flush against the neighbor that was
    // just routed around -- undoing the routing and freezing right at the
    // end of the approach (reported as "the builder got stuck at the
    // bottom of the base, right next to the unfinished house"). Building
    // now commits to one approach_target for the whole approach instead.
    Harness h(2, 8);
    auto [bx, by] = h.world.snap("base", 500, 500);
    h.world.spawn_building("base", 0, bx, by);

    auto [bw, bhh] = building_wh("base");
    auto [hw2, hhh] = building_wh("house");
    (void)bw;
    (void)hw2;
    // House placed flush against the base's TOP edge (matches the
    // reported screenshot: base/"palace" with a house foundation directly
    // above it).
    auto [hx, hy] = h.world.snap("house", bx, by - bhh / 2.0 - hhh / 2.0);
    EntityRef foundation = h.world.spawn_building("house", 0, hx, hy, /*constructing=*/true);
    h.world.prime();

    // Builder starts well BELOW the base, so it has to route around the
    // base's side to reach the house at all, then approach from a clear
    // side rather than the (base-adjacent) bottom edge.
    EntityRef builder = h.world.spawn_unit("civilian", 0, bx, by + bhh / 2.0 + 150);
    Unit* u = h.world.get(builder);
    u->build_target = foundation;

    Building* b = h.world.get_building(foundation);
    REQUIRE_FALSE(b->complete);
    for (int i = 0; i < 20 * 30 && !b->complete; ++i) h.world.update(1.0 / 20.0);

    REQUIRE(b->complete);
}

TEST_CASE("Two houses can be placed flush against each other with snap()") {
    // Regression test: snap() used to quantize a building's position to the
    // TILE (32px) grid while footprint sizes were expressed in BTILE (24px)
    // units -- since 24 doesn't evenly divide 32, two same-size buildings
    // could never land with their edges exactly touching (the closest
    // reachable snapped spot always either overlapped by a few px, rejected
    // by footprint_clear, or left an unwanted gap). Footprints are now each
    // building's real native pixel size (all exact multiples of TILE), so
    // snapping both buildings to the TILE grid should always let a second
    // house be placed directly against a first one's edge.
    Harness h(2, 5);
    auto [x0, y0] = h.world.snap("house", 500, 500);
    h.world.spawn_building("house", 0, x0, y0);
    h.world.prime();

    auto [w, hh] = building_wh("house");
    REQUIRE(w == 64);

    // Snap a second house whose nominal center is just to the right of the
    // first -- close enough that only a position with its left edge flush
    // against the first house's right edge should come back valid.
    auto [x1, y1] = h.world.snap("house", x0 + w, y0);
    REQUIRE(x1 == x0 + w); // edges exactly flush, not overlapping or gapped
    REQUIRE(h.world.footprint_clear("house", x1, y1));
}

TEST_CASE("A unit routes around a V-shaped dead end instead of getting stuck in it forever") {
    // Regression test for "click a unit to the other side of an obstruction
    // and it nestles into a V-shaped gap and keeps walking forever": a
    // straight line toward a goal on the far side of a wedge-shaped
    // obstruction dives into the notch (the locally-shortest direction) and
    // wedges against its sealed apex, unable to move in either axis. See
    // update_unit's move_goal handling -- it now re-plans a corner-safe path
    // from wherever the unit actually got wedged instead of blindly
    // retrying the same doomed straight line, and gives up for good after a
    // few failed attempts in a row so this can never loop forever even if
    // truly trapped.
    // Harness's world is a fixed 40x40 tiles (1280x1280px) -- keep the whole
    // wedge and both endpoints comfortably inside that.
    Harness h(2, 42);
    int apex_tx = 20, apex_ty = 25;
    auto tile_px = [](int tx, int ty) {
        return std::pair<double, double>{tx * TILE + TILE / 2.0, ty * TILE + TILE / 2.0};
    };
    // A 3-tile-thick diagonal wall on each side, converging on a sealed
    // apex -- a genuine wedge with no diagonal gaps to slip through, open
    // only at the wide mouth (increasingly far apart going up from the
    // apex) and around each arm's far end.
    for (int k = 0; k <= 6; ++k) {
        for (int t = -1; t <= 1; ++t) {
            auto [lx, ly] = tile_px(apex_tx - k + t, apex_ty - k);
            h.world.spawn_resource("tree", lx, ly);
            auto [rx, ry] = tile_px(apex_tx + k + t, apex_ty - k);
            h.world.spawn_resource("tree", rx, ry);
        }
    }

    auto [apex_x, apex_y] = tile_px(apex_tx, apex_ty);
    EntityRef civ = h.world.spawn_unit("civilian", 0, apex_x, apex_y - 5 * TILE); // inside the notch's mouth
    double goal_x = apex_x, goal_y = apex_y + 250; // sealed on the opposite side of the apex
    h.world.order_move(civ, goal_x, goal_y);

    bool resolved = false;
    for (int i = 0; i < 20 * 60 && !resolved; ++i) {
        h.world.update(1.0 / 20.0);
        Unit* u = h.world.get(civ);
        REQUIRE(u != nullptr);
        if (!u->move_goal) resolved = true; // arrived, or gave up -- either way, it stopped
    }
    REQUIRE(resolved); // must not still be wandering after 60 sim-seconds

    Unit* u = h.world.get(civ);
    double final_dist = std::hypot(u->common.x - goal_x, u->common.y - goal_y);
    REQUIRE(final_dist < 100.0); // actually routed all the way around, not just gave up nearby
}

TEST_CASE("AI team builds its economy without crashing over simulated time") {
    Harness h(2, 4);
    h.control.teams[1].is_ai = true;
    h.world.spawn_building("base", 1, 500, 500);
    for (int i = 0; i < 3; ++i) {
        h.world.spawn_unit("civilian", 1, 540 + i * 20, 500);
    }
    for (int i = 0; i < 20; ++i) {
        h.world.spawn_resource("tree", 600 + (i % 5) * 24, 500 + (i / 5) * 24);
    }

    for (int i = 0; i < 20 * 90; ++i) h.world.update(1.0 / 20.0); // 90 sim-seconds of AI ticks

    // The AI should have queued or built at least a barracks by now, and
    // should not have produced any NaN/negative resource totals.
    bool has_barracks_or_foundation = false;
    for (auto ref : h.world.active_buildings) {
        Building* b = h.world.get_building(ref);
        if (b && b->name == "barracks") has_barracks_or_foundation = true;
    }
    REQUIRE(has_barracks_or_foundation);
    for (auto& [k, v] : h.control.teams[1].res) {
        REQUIRE(v == v); // not NaN
        REQUIRE(v >= 0.0);
    }
}

// ---- walk-through foundations (Building::blocks_movement) ----

TEST_CASE("An unstarted foundation is walk-through, and turns solid once construction begins") {
    Harness h(2, 63);
    EntityRef foundation = h.world.spawn_building("barracks", 0, 500, 500, /*constructing=*/true);
    h.world.prime(); // rebuild_occupied() -- solid_building_rects_ isn't populated until this runs

    Building* b = h.world.get_building(foundation);
    REQUIRE(b != nullptr);
    REQUIRE(b->construction == 0.0);
    // Nobody has laid a hammer on it, so it's still just a marker on the
    // ground: units path straight over the middle of it.
    REQUIRE(h.world.passable(/*is_air=*/false, /*is_ship=*/false, 500, 500));

    EntityRef builder = h.world.spawn_unit("civilian", 0, 500, 560);
    h.world.get(builder)->build_target = foundation;
    for (int i = 0; i < 20 * 20 && b->construction <= 0.0; ++i) h.world.update(1.0 / 20.0);

    REQUIRE(b->construction > 0.0);
    REQUIRE_FALSE(b->complete); // still a foundation -- solidity is about work having STARTED
    // One more tick: rebuild_occupied() runs at the TOP of update(), so the
    // blow that started construction landed after this tick's rebuild and
    // the collision rects only pick it up on the next one.
    h.world.update(1.0 / 20.0);
    REQUIRE_FALSE(h.world.passable(/*is_air=*/false, /*is_ship=*/false, 500, 500));
}

TEST_CASE("Construction never starts under a unit, and an idle one standing there is moved off") {
    // The whole point of the walk-through foundation: since units can now
    // stand inside a footprint, the first hammer blow -- which is what makes
    // it solid -- must wait until they're clear, or it would seal them in
    // with no passable tile to escape to.
    Harness h(2, 64);
    EntityRef foundation = h.world.spawn_building("barracks", 0, 500, 500, /*constructing=*/true);
    EntityRef loiterer = h.world.spawn_unit("civilian", 0, 500, 500); // dead centre, no orders
    EntityRef builder = h.world.spawn_unit("civilian", 0, 500, 560);
    h.world.get(builder)->build_target = foundation;

    Building* b = h.world.get_building(foundation);
    for (int i = 0; i < 20 * 60 && !b->complete; ++i) {
        h.world.update(1.0 / 20.0);
        // The invariant, checked every single tick: no unit is ever strictly
        // inside a footprint that has already been worked on.
        if (b->construction > 0.0) {
            REQUIRE(h.world.units_on_footprint(*b).empty());
        }
    }

    REQUIRE(b->complete); // the loiterer got shoved aside rather than deadlocking the build
    Unit* l = h.world.get(loiterer);
    REQUIRE(l != nullptr);
    REQUIRE(l->common.alive);
    REQUIRE_FALSE(h.world.inside_footprint(*b, l->common.x, l->common.y)); // and isn't walled in
}

TEST_CASE("A builder standing on its own foundation steps off it instead of sealing itself in") {
    // A builder can arrive INSIDE the footprint now that it's walk-through:
    // advance_to_building's A* is free to route across it, and at_dropoff's
    // buffer covers the whole interior, so it can stop dead in the middle.
    Harness h(2, 65);
    EntityRef foundation = h.world.spawn_building("barracks", 0, 500, 500, /*constructing=*/true);
    EntityRef builder = h.world.spawn_unit("civilian", 0, 500, 500); // spawned on the footprint
    h.world.get(builder)->build_target = foundation;

    Building* b = h.world.get_building(foundation);
    for (int i = 0; i < 20 * 60 && !b->complete; ++i) h.world.update(1.0 / 20.0);

    REQUIRE(b->complete); // it walked off, then built -- no standoff with itself
    Unit* u = h.world.get(builder);
    REQUIRE(u != nullptr);
    REQUIRE(u->common.alive);
    REQUIRE_FALSE(h.world.inside_footprint(*b, u->common.x, u->common.y));
}

TEST_CASE("A foundation still blocks placement of a second building even while walk-through") {
    // Walk-through is about MOVEMENT only: footprint_clear keeps checking
    // all_building_rects_, which includes foundations, so you can't stack a
    // second building on one.
    Harness h(2, 66);
    h.world.spawn_building("barracks", 0, 500, 500, /*constructing=*/true);
    h.world.prime();

    REQUIRE_FALSE(h.world.footprint_clear("house", 500, 500));
    REQUIRE_FALSE(h.world.footprint_clear("barracks", 520, 500)); // overlapping, not stacked
}

TEST_CASE("A villager routes out of a concave pocket to reach its resource") {
    // The reported symptom, in the sim rather than in isolation: walking to
    // a gather target used to be a bare step_toward with no route at all,
    // so a villager inside a concave pocket pressed into the back wall --
    // escaping means travelling AWAY from the target first, which purely
    // local steering has no way to discover. It would burn its stall window
    // and abandon a perfectly reachable resource.
    //
    // Layout (tiles): tree walls down both sides at tx 10 and 16 from ty 10
    // to 18, closed off along the bottom at ty 18. The pocket's only mouth
    // faces NORTH. The berry sits well to the SOUTH, so the direct line
    // runs straight through the closed end and the real route is out the
    // top, around a wall and back down.
    Harness h(2, 77);
    auto tile_px = [](int tx, int ty) {
        return std::pair<double, double>{tx * TILE + TILE / 2.0, ty * TILE + TILE / 2.0};
    };
    for (int ty = 10; ty <= 18; ++ty) {
        auto [lx, ly] = tile_px(10, ty);
        auto [rx, ry] = tile_px(16, ty);
        h.world.spawn_resource("tree", lx, ly);
        h.world.spawn_resource("tree", rx, ry);
    }
    for (int tx = 10; tx <= 16; ++tx) {
        auto [bx, by] = tile_px(tx, 18);
        h.world.spawn_resource("tree", bx, by);
    }
    auto [vx, vy] = tile_px(13, 16); // deep inside, near the closed end
    auto [gx, gy] = tile_px(13, 25); // outside, beyond the closed end
    EntityRef civ = h.world.spawn_unit("civilian", 0, vx, vy);
    EntityRef berry = h.world.spawn_resource("berry", gx, gy);
    h.world.prime(); // resource_tiles_ isn't populated until this runs

    // Issued through the real order-dispatch function, not by assigning
    // gather_target directly -- order_gather also sets gather_rtype, which
    // the give-up/re-seek logic depends on.
    h.world.order_gather(civ, berry);

    bool arrived = false;
    for (int i = 0; i < 20 * 90 && !arrived; ++i) { // 90 sim-seconds
        h.world.update(1.0 / 20.0);
        Unit* u = h.world.get(civ);
        REQUIRE(u != nullptr);
        if (std::hypot(u->common.x - gx, u->common.y - gy) < 30.0) arrived = true;
    }
    REQUIRE(arrived);

    Unit* u = h.world.get(civ);
    // Still on the berry it was sent to -- it routed there rather than
    // giving the order up. (The pocket walls are wood, the berry is food,
    // so there's no same-type alternative it could have silently swapped
    // to instead.)
    REQUIRE(u->gather_target == berry);
}

TEST_CASE("A builder routes around a foundation between it and its own target") {
    // Exactly the layout the player reported: two house foundations side by
    // side, one villager assigned to each. The far villager sits to the LEFT
    // of the near foundation, so the straight line to ITS foundation runs
    // through the near one.
    //
    // Two things this pins down. Route planning must go AROUND an unstarted
    // foundation even though units can physically walk over it -- a path
    // drawn through one becomes a wall the moment the other villager starts
    // building it. And when a committed route does go stale, the approach
    // must re-plan instead of giving the order up.
    Harness h(2, 91);
    EntityRef near_f = h.world.spawn_building("house", 0, 500, 500, /*constructing=*/true);
    EntityRef far_f = h.world.spawn_building("house", 0, 700, 500, /*constructing=*/true);
    // Left of the near foundation, level with both -- straight line to the
    // far one passes through the near one.
    EntityRef far_builder = h.world.spawn_unit("civilian", 0, 380, 500);
    EntityRef near_builder = h.world.spawn_unit("civilian", 0, 500, 580);
    h.world.prime();

    h.world.get(far_builder)->build_target = far_f;
    h.world.get(near_builder)->build_target = near_f;

    Building* far_b = h.world.get_building(far_f);
    for (int i = 0; i < 20 * 60 && !far_b->complete; ++i) h.world.update(1.0 / 20.0);

    // The far villager got there and finished, rather than wedging against
    // the side of the near foundation once it turned solid.
    REQUIRE(far_b->complete);
}

TEST_CASE("Route planning avoids an unstarted foundation that movement can cross") {
    // The two passability rules must disagree in exactly one way: physically
    // walk-through (so nothing is ever sealed in or walled off), but off
    // limits to A*.
    Harness h(2, 92);
    h.world.spawn_building("house", 0, 500, 500, /*constructing=*/true);
    h.world.prime();

    REQUIRE(h.world.passable(/*is_air=*/false, /*is_ship=*/false, 500, 500));
    REQUIRE_FALSE(h.world.passable_planning(/*is_air=*/false, /*is_ship=*/false, 500, 500));

    // Once started it's solid, so BOTH rules agree it's blocked.
    Building* b = h.world.get_building(h.world.active_buildings.back());
    b->construction = 5.0;
    h.world.update(1.0 / 20.0); // rebuild_occupied picks the change up
    REQUIRE_FALSE(h.world.passable(/*is_air=*/false, /*is_ship=*/false, 500, 500));
    REQUIRE_FALSE(h.world.passable_planning(/*is_air=*/false, /*is_ship=*/false, 500, 500));
}

TEST_CASE("A loaded villager routes around terrain to reach its drop-off") {
    // The delivery leg was the last movement path with no route at all -- a
    // bare step_toward at the drop-off, with no stall detection either. A
    // full villager with something between it and the base pressed into the
    // obstacle indefinitely; that's the "holding carry=10 for 55 minutes,
    // path=0 need_path=0 stall=0" freeze in the resume notes.
    //
    // Layout: the base sits behind a wall of trees with its only opening to
    // the north. The tree the villager works is due SOUTH of the base, so the
    // straight line back runs into the closed side.
    Harness h(2, 84);
    auto tile_px = [](int tx, int ty) {
        return std::pair<double, double>{tx * TILE + TILE / 2.0, ty * TILE + TILE / 2.0};
    };
    auto [bx, by] = tile_px(13, 13);
    EntityRef base = h.world.spawn_building("base", 0, bx, by);
    // Tree wall enclosing the base on the west, east and south; open north.
    for (int ty = 11; ty <= 17; ++ty) {
        auto [lx, ly] = tile_px(10, ty);
        auto [rx, ry] = tile_px(16, ty);
        h.world.spawn_resource("tree", lx, ly);
        h.world.spawn_resource("tree", rx, ry);
    }
    for (int tx = 10; tx <= 16; ++tx) {
        auto [sx, sy] = tile_px(tx, 17);
        h.world.spawn_resource("tree", sx, sy);
    }
    // The villager's own tree, south of the closed side, plus the villager.
    auto [wx, wy] = tile_px(13, 21);
    EntityRef work_tree = h.world.spawn_resource("tree", wx, wy);
    EntityRef civ = h.world.spawn_unit("civilian", 0, wx - TILE, wy);
    h.world.prime();

    h.world.order_gather(civ, work_tree);

    double start_wood = h.control.teams[0].res["wood"];
    // Long enough to fill a load and walk it around the tree wall to the base.
    for (int i = 0; i < 20 * 180; ++i) h.world.update(1.0 / 20.0);

    // Wood actually banked means the round trip completed -- gathered AND
    // delivered around the obstacle, not just gathered and stuck holding it.
    INFO("wood delivered: " << (h.control.teams[0].res["wood"] - start_wood));
    REQUIRE(h.control.teams[0].res["wood"] > start_wood);
    (void)base;
}

TEST_CASE("A loaded villager gets past ALLIED bodies blocking its drop-off") {
    // The player's screenshot: a villager holding oil, a refinery, and two
    // allied villagers parked on the ore node between the two. It "kept going
    // forever, jittering and moving back and forward, never making it".
    //
    // Two separate holes, both needed:
    //   1. approach_stalled measured PER-TICK displacement, so a unit bouncing
    //      off step_toward's retreat tier every tick looked like it was moving
    //      and the replan ladder never fired at all.
    //   2. A* is unit-blind, so even when the ladder did fire the replan
    //      re-derived the identical blocked route. Retry plans now treat
    //      SETTLED bodies as terrain (settled_unit_at).
    //
    // Unlike the terrain test above, every obstacle here is a friendly unit --
    // which is exactly why the pre-existing routing could not see it.
    Harness h(2, 91);
    auto tile_px = [](int tx, int ty) {
        return std::pair<double, double>{tx * TILE + TILE / 2.0, ty * TILE + TILE / 2.0};
    };
    auto [rx, ry] = tile_px(13, 13);
    EntityRef refinery = h.world.spawn_building("refinery", 0, rx, ry);
    Building* rb = h.world.get_building(refinery);
    REQUIRE(rb != nullptr);
    rb->complete = true;
    rb->construction = 100.0;

    // Oil directly south of the refinery, and a wall of trees boxing in the
    // approach so the ONLY short way back to the refinery door runs through
    // where the blockers stand -- the concavity in the screenshot.
    auto [ox, oy] = tile_px(13, 17);
    EntityRef oil = h.world.spawn_resource("oil", ox, oy);
    for (int ty = 12; ty <= 18; ++ty) {
        auto [lx, ly] = tile_px(11, ty);
        auto [ex, ey] = tile_px(15, ty);
        h.world.spawn_resource("tree", lx, ly);
        h.world.spawn_resource("tree", ex, ey);
    }

    // Two settled villagers wedged in the gap between the oil and the door.
    auto [b1x, b1y] = tile_px(13, 15);
    auto [b2x, b2y] = tile_px(12, 15);
    EntityRef blocker1 = h.world.spawn_unit("civilian", 0, b1x, b1y);
    EntityRef blocker2 = h.world.spawn_unit("civilian", 0, b2x, b2y);
    for (EntityRef b : {blocker1, blocker2}) {
        Unit* bu = h.world.get(b);
        REQUIRE(bu != nullptr);
        bu->working = true; // parked on the node: never moves out of the way
    }

    // The carrier, already holding a full load of oil, told to deliver.
    auto [cx, cy] = tile_px(13, 18);
    EntityRef civ = h.world.spawn_unit("civilian", 0, cx, cy);
    h.world.prime();
    {
        Unit* u = h.world.get(civ);
        REQUIRE(u != nullptr);
        u->carry = 10;
        u->carry_type = 2; // OIL -- only the base or a refinery accepts it
        u->drop_target = refinery;
        REQUIRE(u->carry == 10); // the delivery branch needs carry > 0 to run at all
    }

    double start_oil = h.control.teams[0].res["oil"];
    for (int i = 0; i < 20 * 60; ++i) {
        h.world.update(1.0 / 20.0); // 60 sim-seconds
        // Re-fetch every time: World::update's death sweep compacts the unit
        // pool, so a Unit* held across an update can dangle.
    }

    Unit* end_u = h.world.get(civ);
    INFO("oil delivered: " << (h.control.teams[0].res["oil"] - start_oil)
                           << "  carry still held: " << (end_u ? end_u->carry : -1));
    REQUIRE(h.control.teams[0].res["oil"] > start_oil);
    (void)oil;
}

TEST_CASE("A settled ally plugging the only doorway is routed around, not pressed into") {
    // The player's second screenshot: "the villager on the right continuously
    // tries to get to the oil to the left, but it can never make it past the
    // concavity of the villager and the refinery."
    //
    // A* is unit-blind, which is normally correct -- units are transient. But a
    // SETTLED unit (one working a node, holding station) is a permanent wall,
    // and a unit-blind replan hands back the identical route into it every
    // time. Local steering cannot rescue that: escaping a concavity means
    // walking AWAY from the goal, which reactive avoidance has no way to
    // discover (the standing lesson in this file's notes).
    //
    // Layout: a tree wall with exactly ONE doorway, a settled villager standing
    // in it, and the only oil on the map on the far side. The doorway is a
    // single 32px tile and a villager pair needs 16.1px of clearance, so a
    // plugged doorway is genuinely impassable -- but the way around the end of
    // the wall is wide open. A route therefore exists only if the planner can
    // see that the doorway is shut.
    Harness h(2, 77);
    auto tile_px = [](int tx, int ty) {
        return std::pair<double, double>{tx * TILE + TILE / 2.0, ty * TILE + TILE / 2.0};
    };
    // Tree wall across y=14 spanning x=6..24, with a single gap at x=15.
    for (int tx = 6; tx <= 24; ++tx) {
        if (tx == 15) continue; // the doorway
        auto [wx, wy] = tile_px(tx, 14);
        h.world.spawn_resource("tree", wx, wy);
    }
    auto [px, py] = tile_px(15, 14);
    EntityRef plug = h.world.spawn_unit("civilian", 0, px, py);

    // The only oil on the map, north of the wall. Oil (not wood) so the wall's
    // own trees can't be substituted for the target and pass the test trivially.
    auto [gx, gy] = tile_px(15, 11);
    EntityRef oil = h.world.spawn_resource("oil", gx, gy);

    // Base well south of the wall: the drop-off, and on the traveller's side.
    auto [bx, by] = tile_px(15, 24);
    EntityRef base = h.world.spawn_building("base", 0, bx, by);
    auto [sx, sy] = tile_px(15, 19);
    EntityRef civ = h.world.spawn_unit("civilian", 0, sx, sy);
    h.world.prime();

    h.world.order_gather(civ, oil);

    double start_oil = h.control.teams[0].res["oil"];
    for (int i = 0; i < 20 * 150; ++i) {
        h.world.update(1.0 / 20.0); // 150 sim-seconds: long way round, then back
        Unit* pu = h.world.get(plug);
        if (pu) {
            pu->working = true; // parked for good -- never yields the doorway
            pu->common.x = px;
            pu->common.y = py;
        }
    }

    // Banked oil proves the traveller reached the far side and returned.
    INFO("oil delivered: " << (h.control.teams[0].res["oil"] - start_oil));
    REQUIRE(h.control.teams[0].res["oil"] > start_oil);
    (void)base;
}

TEST_CASE("An attack-move wave routes around a concave pocket instead of into it") {
    // The player's third screenshot: "a large force getting completely stuck in
    // a concavity. the pathfinding should tell them to avoid that."
    //
    // They were right that it should -- and the reason it didn't is that the
    // rally branch (the AI's attack-move, how every army crosses the map) was a
    // bare step_toward with no route and no watcher at all. Greedy steering
    // heads straight at the goal, so a U of trees straddling the straight line
    // is a trap: the force presses into the back wall, and escaping means
    // walking AWAY from the goal, which local steering cannot discover.
    //
    // Layout: a U-shaped tree pocket opening EAST, with the units starting
    // inside the mouth and the goal due west beyond the closed back. The only
    // way out is to reverse east and go around -- unreachable by steering,
    // trivial for a planner.
    Harness h(2, 55);
    auto tile_px = [](int tx, int ty) {
        return std::pair<double, double>{tx * TILE + TILE / 2.0, ty * TILE + TILE / 2.0};
    };
    for (int ty = 10; ty <= 20; ++ty) { // back wall of the U (west side)
        auto [wx, wy] = tile_px(12, ty);
        h.world.spawn_resource("tree", wx, wy);
    }
    for (int tx = 12; tx <= 20; ++tx) { // north and south arms
        auto [nx, ny] = tile_px(tx, 10);
        auto [sx2, sy2] = tile_px(tx, 20);
        h.world.spawn_resource("tree", nx, ny);
        h.world.spawn_resource("tree", sx2, sy2);
    }

    // A squad sitting inside the pocket, attack-moving to a point due west of
    // the closed back wall.
    auto [gx, gy] = tile_px(6, 15);
    std::vector<EntityRef> squad;
    for (int i = 0; i < 4; ++i) {
        auto [ux, uy] = tile_px(17 + (i % 2), 14 + (i / 2));
        squad.push_back(h.world.spawn_unit("muscateer", 0, ux, uy));
    }
    h.world.prime();
    for (EntityRef s : squad) {
        Unit* su = h.world.get(s);
        REQUIRE(su != nullptr);
        su->rally = Vec2{gx, gy};
    }

    for (int i = 0; i < 20 * 120; ++i) h.world.update(1.0 / 20.0); // 120 sim-seconds

    // At least most of the squad should have escaped the pocket westward past
    // the wall. Stuck-in-the-pocket means every unit is still east of x=12.
    int escaped = 0;
    for (EntityRef s : squad) {
        Unit* su = h.world.get(s);
        if (su && su->common.x < 12 * TILE) ++escaped;
    }
    INFO("escaped the pocket: " << escaped << " of " << squad.size());
    REQUIRE(escaped >= 3);
}
