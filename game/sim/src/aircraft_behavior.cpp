// Aircraft behavior (`is_air` units): fuel, landing/parking at an airbase
// (capacity 5), takeoff, airborne dogfighting/bombing, and the shot-down
// dive-to-crash sequence.
//
// Primarily a port of game/entity.py's Unit._update_air (fuel/target-
// acquisition/combat state machine), which is itself already a
// simplification of the original GML (assets/gmk/objects/obj_unit/
// Step.gml). Not replicated: civ-specific plane reskins (spitfire/zero/
// mustang/messerschmitt/etc, obj_unit/Draw.gml) -- 9 extra sprite sets for
// a purely cosmetic difference the original gets away with because it's
// rendered at a tiny scale.
//
// Two pieces ARE restored from the raw GML that Python's port dropped:
// - A real `height` 0..64 takeoff/landing ramp (Unit::height), instead of
//   Python's instant landed/airborne snap -- see unit.h's comment on that
//   field. `height` only ever affects effective flight speed and the
//   client's render offset; it never touches common.x/y, so targeting/
//   range/splash-damage math always uses the true ground position
//   regardless of how "high" a plane currently looks.
// - Turn-rate-limited heading (Unit::heading/turn_speed, see steer_toward
//   below), instead of Python's instant point-and-go direction snap --
//   GML's `direction` interpolating toward `aim_direction` by `turn_speed`
//   degrees/step. This is what makes a course change read as a banked arc
//   and makes circling a target/hover point look like a loose orbit
//   instead of jerky point-chasing.
//
// Also deliberately NOT replicated: the original GML's fuel=0 behavior is
// dead code (an unconditional `if fuel=0 {exit}` earlier in the same Step
// event makes the intended auto-return-to-base/crash logic unreachable --
// see the GML research for this task), so a plane that runs dry just
// freezes in place forever, unorderable. That's almost certainly a bug,
// not a design intent, so this port keeps Python's INTENDED behavior
// instead: glide home to refuel if a friendly airbase exists, otherwise
// dive and crash.
#include "sim/behavior.h"
#include "sim/building.h"
#include "sim/control.h"
#include "sim/unit.h"
#include "sim/world.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ww::sim {

namespace {

// No A*/passability check, planes fly over everything (matches the GML:
// the whole shoreline/terrain collision correction block is `not
// aerial`-gated, and planes never call mp_potential_step against
// obj_solid the way ground/ship units do). `speed_mult` scales speed_px
// down while ramping through takeoff/landing (Unit::height < 64) -- GML's
// `speed = speeds * (height/64)`.
//
// Turns u.heading toward (tx,ty) at u.turn_speed (converted from the
// catalog's degrees-per-GML-step to radians/sec), THEN moves forward
// along that heading -- NOT straight at (tx,ty) -- so a course change
// arcs through the turn instead of snapping. std::remainder gives the
// shortest signed angular distance in (-pi,pi], so it always turns
// whichever way is shorter.
void steer_toward(Unit& u, double tx, double ty, double dt, double speed_mult) {
    double aim = std::atan2(ty - u.common.y, tx - u.common.x);
    double diff = std::remainder(aim - u.heading, 2.0 * M_PI);
    // deg/step -> rad/sec: the room actually runs at 60 steps/sec, not 30
    // (assets/gmk/objects/control/Step.gml:501 -- see world.cpp's
    // speed_px comment for the same correction already applied there).
    double turn_rate = u.turn_speed * 60.0 * (M_PI / 180.0);
    double max_turn = turn_rate * dt;
    u.heading += std::clamp(diff, -max_turn, max_turn);
    u.common.x += std::cos(u.heading) * u.speed_px * speed_mult * dt;
    u.common.y += std::sin(u.heading) * u.speed_px * speed_mult * dt;
    u.facing = std::cos(u.heading) >= 0 ? 1 : -1;
    u.moving = true;
}

// The forward-gun muzzle in WORLD space for a fighter: a FIXED sprite-pixel
// nose offset (planes are drawn at a fixed orientation, only mirrored by
// facing -- they don't rotate to their heading), mirrored by facing. Kept in
// sync with game_client.cpp's plane_props/plane_nose. Unlisted planes (jets)
// use a plain forward offset so bullets still leave the front, not the centre.
Vec2 plane_muzzle(const Unit& u) {
    static const std::unordered_map<std::string, std::pair<double, double>> kNose = {
        {"spr_spitfire", {5, 17}},      {"spr_messerschmitt", {17, 14}}, {"spr_mustang", {5, 17}},
        {"spr_zero", {14, 15}},         {"spr_fighter", {5, 17}},        {"spr_biplane", {5, 17}},
    };
    auto it = kNose.find(u.sprite);
    double dx = it != kNose.end() ? it->second.first : 16.0;
    double dy = it != kNose.end() ? it->second.second : 0.0;
    double sign = u.facing < 0 ? -1.0 : 1.0;
    return {u.common.x + dx * sign, u.common.y + dy};
}

// Circular holding pattern around (cx, cy) -- used both to orbit-and-fire
// on an in-range attack target and to loiter when idle with nothing to do.
// Ported as "steer toward a point that itself travels in a circle" rather
// than snapping directly onto that circle (Unit._orbit, game/entity.py),
// so a plane settles into the loop gradually, same as any other turn.
void orbit(Unit& u, double cx, double cy, double dt, double speed_mult, double radius = 52.0) {
    u.orbit_a += dt * 1.6;
    double tx = cx + std::cos(u.orbit_a) * radius;
    double ty = cy + std::sin(u.orbit_a) * radius;
    steer_toward(u, tx, ty, dt, speed_mult);
}

// A place a plane can set down to refuel/rearm: an airbase (a building) OR an
// aircraft carrier (a mobile sea "airbase" -- a unit). Both park/refuel/rearm
// planes; only an airbase (airbase != null) stores nukes for auto-loading.
struct LandingSite {
    bool valid = false;
    uint32_t id = 0;
    double x = 0.0, y = 0.0;
    double foot_w = 0.0, foot_h = 0.0, foot_px = 0.0;
    int capacity = 5;
    int facing = 1;              // carrier heading sign (+1 right / -1 left); 1 for airbases
    Building* airbase = nullptr; // non-null only for a real airbase
};

// Resolve where this plane should land. If `preferred` is set (the player's
// Land-button target) and that exact site still exists, return it; otherwise
// the nearest own site of EITHER kind. Unbounded, like Python's min-by-distance
// over every building -- a plane always knows the way home.
//
// FULL SITES ARE SKIPPED. This used to be a pure nearest-site search, so a
// plane whose closest airbase already had its five bays occupied flew to it and
// then circled indefinitely (see the "airbase full -- keep circling" fallthrough
// in update_aircraft), even with a half-empty second airbase in easy reach --
// and on a low-fuel return that circling is what killed it. Now occupancy is
// part of the search: the answer is the nearest site with a FREE BAY, so planes
// spread across the airbases the team actually built.
//
// `stay_id` is the site this plane is already parked at, which always counts as
// available to it -- without that exemption a plane sitting in the last bay of a
// full base would find its own home "full", read as homeless, and take off to
// fly somewhere else the moment it finished refuelling.
LandingSite find_landing_site(World& world, int team, double px, double py,
                              std::optional<uint32_t> preferred,
                              std::optional<uint32_t> stay_id) {
    LandingSite best;
    double best_d2 = 1e18;
    bool specific = preferred.has_value();
    // Parked-plane census by home site -- the same "landed planes claiming this
    // site" count update_aircraft does for the bay it parks in, hoisted here so
    // the capacity test can be applied to every candidate, not just the winner.
    // Counts only planes that have actually TOUCHED DOWN, so an inbound plane
    // doesn't reserve a bay it hasn't reached; two planes can briefly pick the
    // same last free bay, and whichever loses simply re-searches next tick.
    std::unordered_map<uint32_t, int> parked;
    for (auto ref : world.active_units) {
        Unit* p = world.get(ref);
        if (!p || !p->common.alive || !p->landed || !p->home_id) continue;
        ++parked[*p->home_id];
    }
    auto has_room = [&](uint32_t id, int cap) {
        if (stay_id && *stay_id == id) return true;
        auto it = parked.find(id);
        return (it == parked.end() ? 0 : it->second) < cap;
    };
    auto consider = [&](uint32_t id, double x, double y, double fw, double fh,
                        double fpx, int cap, int facing, Building* ab) {
        if (!has_room(id, cap)) return;
        if (specific) {
            if (id != *preferred) return;
        } else {
            double dx = x - px, dy = y - py, d2 = dx * dx + dy * dy;
            if (d2 >= best_d2) return;
            best_d2 = d2;
        }
        best.valid = true; best.id = id; best.x = x; best.y = y;
        best.foot_w = fw; best.foot_h = fh; best.foot_px = fpx; best.capacity = cap;
        best.facing = facing; best.airbase = ab;
    };
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive || !b->complete || b->common.team != team ||
            b->name != "airbase")
            continue;
        consider(b->common.id, b->common.x, b->common.y, b->foot_w, b->foot_h,
                 b->foot_px, 5, 1, b);
    }
    for (auto ref : world.active_units) {
        Unit* c = world.get(ref);
        if (!c || !c->common.alive || !c->is_carrier || c->common.team != team) continue;
        // Fixed deck footprint per tier (a unit has no building footprint),
        // sized so the parking slots sit along the flight deck.
        bool big = (c->name == "aircraft carrier2");
        double fw = big ? 168.0 : 150.0, fh = big ? 90.0 : 64.0;
        consider(c->common.id, c->common.x, c->common.y, fw, fh, std::max(fw, fh),
                 std::max(1, c->air_capacity), (c->facing < 0 ? -1 : 1), nullptr);
    }
    return best;
}

// 64 units over ~2.1s: the GML ramp is 0.5/step, and the room actually
// runs at 60 steps/sec (see steer_toward's comment), so 0.5*60 = 30/sec.
constexpr double kHeightRate = 30.0;

} // namespace

void update_aircraft(EntityRef self, Unit& u, double dt, World& world) {
    if (u.diving) { // shot down -> keep flying its line, crash ahead, then blow up
        u.dive_t += dt;
        if (u.dive_t < 0.05) {
            world.events.push({EventType::Sound, "plane_crash", u.common.x, u.common.y, 0, kNullRef, ""});
        }
        // Carry on along the heading it had when hit (a stricken plane doesn't
        // stop dead -- it coasts forward), losing altitude as it goes.
        double fwd = u.speed_px * 0.75;
        u.common.x += std::cos(u.heading) * fwd * dt;
        u.common.y += std::sin(u.heading) * fwd * dt;
        u.height = std::max(0.0, u.height - kHeightRate * 1.3 * dt);
        if (u.height <= 0.0 || u.dive_t >= 1.6) {
            // Impact: the death sweep spawns the wreck + explosion FX; here we
            // add the blast DAMAGE to anything hostile caught at the crash site.
            u.common.alive = false;
            constexpr double kCrashR = 48.0;
            double pow = std::max(u.attack, 40.0);
            for (auto ref : world.grid.query(u.common.x, u.common.y, kCrashR)) {
                if (ref == self) continue;
                EntityCommon* e = world.common(ref);
                if (!e || !e->alive || e->team < 0) continue;
                if (ref.kind != EntityKind::Unit && ref.kind != EntityKind::Building) continue;
                if (world.control.allied(e->team, u.common.team)) continue;
                if (e->is_air) continue; // a ground crash doesn't hit other planes
                double d = std::hypot(e->x - u.common.x, e->y - u.common.y);
                if (d <= kCrashR) world.hurt(ref, pow);
            }
        }
        return;
    }

    Team& team = world.control.teams[u.common.team];
    // fuel: catalog "fuel_latency" (steps per 1 fuel point, at the room's
    // real 60 steps/sec -- see steer_toward's comment) converted to a
    // per-second drain rate, so a fighter (25) burns faster than a heavy
    // bomber (40) -- matches the original's per-plane-type table instead
    // of one flat rate for every aircraft. gasoline -40% burn, synthetic
    // fuel -60% burn (not stacked -- synthetic fuel supersedes gasoline,
    // matching the tech's own "60%" description more directly than the
    // raw GML's fuel_latency*1.4/*1.6 multipliers, which don't actually
    // land on 40%/60% burn reduction).
    double eff = team.tech.count("synthetic fuel") ? 0.4 : team.tech.count("gasoline") ? 0.6 : 1.0;
    double drain_per_sec = (60.0 / std::max(1.0, u.fuel_latency)) * eff;
    // If the player picked a specific landing site (Land button + click), route
    // to THAT one -- an airbase OR an aircraft carrier; otherwise (or if it's
    // since been destroyed/sunk) fall back to the nearest own site of either
    // kind.
    // Preference order for where to set down: the player's explicit Land target,
    // else the CARRIER this plane launched from (its home_id -- so a carrier's
    // air wing returns to IT, not just the nearest airbase), else the nearest own
    // site. Only a live own carrier triggers the home-preference; a plane whose
    // home is an airbase keeps the nearest-site default.
    std::optional<uint32_t> pref = u.land_target_id;
    if (!pref && u.home_id) {
        for (auto ref : world.active_units) {
            Unit* c = world.get(ref);
            if (c && c->common.alive && c->is_carrier && c->common.team == u.common.team &&
                c->common.id == *u.home_id) {
                pref = u.home_id;
                break;
            }
        }
    }
    // The bay this plane is already sitting in is always still its own (see
    // find_landing_site's stay_id).
    std::optional<uint32_t> stay = u.landed ? u.home_id : std::nullopt;
    LandingSite home = find_landing_site(world, u.common.team, u.common.x, u.common.y, pref, stay);
    if (pref && !home.valid) {
        // The preferred site is gone (destroyed/sunk) -- or, now, FULL. Either
        // way it can't take this plane, so divert to the nearest site that can
        // rather than orbit a base with no bay free. Dropping land_target_id is
        // what stops the next tick re-picking the same full site: an explicit
        // Land order that can't be honoured resolves to the nearest one that
        // can, which is what the player meant by "go and land".
        u.land_target_id.reset(); // no-op if it wasn't the land target
        home = find_landing_site(world, u.common.team, u.common.x, u.common.y, std::nullopt, stay);
    }
    bool over_home =
        home.valid && std::hypot(home.x - u.common.x, home.y - u.common.y) < home.foot_px * 0.7;
    bool idle = !(u.move_goal || u.rally || u.attack_target.valid() || u.forced);
    // Land opportunistically when idle (nothing better to do, might as
    // well top off), or when fuel is genuinely getting low -- NOT merely
    // "not exactly full", which is true on almost every tick of active
    // flight (fuel drains continuously) and would otherwise divert a
    // plane with a live order into landing the instant it grazes its own
    // airbase's capture radius. (The Python reference's condition is the
    // literal `fuel < fuel_max or idle`, which has exactly this bug --
    // confirmed by testing here: a plane ordered elsewhere but still
    // within the radius gets caught in a land/instantly-refuel/take-off
    // loop, never actually leaving. 25 matches the low-fuel auto-return
    // threshold used later in this function.)
    // The `landed && fuel < fuel_max` clause makes a plane that has actually
    // TOUCHED DOWN keep refueling all the way to a full tank before taking off
    // again (Python's `if fuel>=fuel_max and not idle: landed=False` gate).
    // Without it a low-fuel auto-return plane lands, refuels a hair past 25,
    // immediately reads want_land=false, takes off, drains back under 25 and
    // re-lands -- ping-ponging on the airbase stuck at ~25% fuel forever. The
    // `landed` gate preserves the anti-graze-loop fix: a plane merely passing
    // overhead with a live order is !landed, so it's still not caught until
    // fuel genuinely drops below 25.
    // Manual "Land" order (spr_land_icon command button): fly to the home
    // airbase and set down there regardless of fuel. Cleared on touchdown
    // (below), or immediately if there's no airbase to land at.
    if (u.land_order) {
        if (!home.valid) {
            u.land_order = false;
        } else if (!over_home) {
            u.attack_target = kNullRef;
            u.move_goal = Vec2{home.x, home.y};
        }
    }
    // A plane returns to the airbase ONLY when the player orders it (land_order,
    // the UI Land button) or fuel is genuinely running out -- NOT just because
    // it's idle. Idle planes loiter/patrol overhead instead of auto-parking, so
    // they stay on station to strafe and re-acquire targets. (`idle` is still
    // used below to decide whether a plane already parked should take off again.)
    // A fresh PLAYER order (player_launch) lets a parked plane take off right
    // now at whatever fuel it has -- it suppresses the "refuel to full before
    // launching" subclause, but NOT the genuine low-fuel (<25) return or an
    // explicit Land/garrison order.
    bool want_land =
        home.valid && over_home &&
        (u.fuel < 25.0 || (u.landed && u.fuel < u.fuel_max && !u.player_launch) || u.land_order || u.stationed);

    if (want_land) {
        int parked_count = 0, self_index = -1, i = 0;
        for (auto ref : world.active_units) {
            Unit* other = world.get(ref);
            if (!other || !other->common.alive || !other->landed) continue;
            if (!other->home_id || *other->home_id != home.id) continue;
            if (ref == self) self_index = i;
            ++parked_count; ++i;
        }
        if (u.landed || parked_count < home.capacity) {
            int cols = std::max(1, home.capacity);
            int slot = (self_index >= 0) ? self_index : parked_count;
            double park_x, park_y;
            if (home.airbase) {
                // Airbase: a straight row of parking bays below the building.
                park_x = home.x - home.foot_w * 0.35 + (slot % cols) * (home.foot_w * 0.18);
                park_y = home.y - home.foot_h * 0.5 - 6.0;
            } else {
                // Carrier: park the planes ALONG the flight deck's diagonal (aft
                // raised, bow lower/forward) and face them the same way as the
                // ship -- so they reverse (mirror) when it turns to the other
                // side. The X offset mirrors with the ship's heading; the deck's
                // vertical slope stays (a horizontal sprite flip doesn't flip Y).
                double denom = std::max(1, cols - 1);
                double along = (double(slot % cols) / denom - 0.5) * 2.0; // -1 aft .. +1 fore
                double sign = (home.facing < 0) ? -1.0 : 1.0;
                // Compact spread + a slope that hugs the drawn flight-deck angle
                // (rises ~1 tile back-to-front over the deck's length).
                park_x = home.x + sign * along * (home.foot_w * 0.42);
                park_y = home.y + along * (home.foot_h * 0.34) + 2.0;
                u.facing = home.facing; // point along the deck; flips with the carrier
            }
            u.height = std::max(0.0, u.height - kHeightRate * dt);
            if (u.height > 0.0) {
                // still on final approach -- descend toward the parking
                // slot, slowing as height drops (never fully stalling).
                steer_toward(u, park_x, park_y, dt, std::max(0.25, u.height / 64.0));
                u.landed = false;
            } else {
                // touched down: snap into the exact slot and refuel/rearm.
                u.landed = true;
                u.home_id = home.id;
                u.common.x = park_x;
                u.common.y = park_y;
                // Once actually parked, a prior "launch now" order is spent --
                // clearing it means a subsequent LOW-FUEL auto-return refuels to
                // full normally (no partial-fuel ping-pong off the airbase).
                u.player_launch = false;
                // A manual Land order is now fulfilled -- drop it (and the
                // fly-home move goal) so the plane just sits parked (idle)
                // instead of taking straight back off once refuelled.
                if (u.land_order) {
                    u.land_order = false;
                    u.land_target_id.reset(); // arrived -- clear the chosen airbase
                    u.move_goal.reset();
                }
                auto oil_it = team.res.find("oil");
                double oil = oil_it == team.res.end() ? 0.0 : oil_it->second;
                if (u.fuel < u.fuel_max && oil > 0) {
                    u.fuel = std::min(u.fuel_max, u.fuel + u.fuel_max / 60.0 * dt); // ~60s to full
                    team.res["oil"] = std::max(0.0, oil - dt * 0.5);
                }
                if (!u.nuke_loaded) u.clip_ammo = u.clip_max; // rearm (a loaded nuke keeps its single shot)
                // Auto-load a nuke from this airbase's stockpile: a landed
                // heavy bomber / b29 immediately swaps its bombs for a single
                // atomic bomb if the airbase has one stored. Carriers (airbase
                // == null) don't store nukes, so nothing to load there.
                if (!u.nuke_loaded && home.airbase && home.airbase->nuke_count > 0 &&
                    (u.name == "heavy bomber" || u.name == "b29")) {
                    home.airbase->nuke_count--;
                    u.nuke_loaded = true;
                    u.clip_ammo = 1;
                }
            }
            return;
        }
        // else: airbase full -- fall through and keep circling nearby
        // until a slot frees up (re-checked every tick).
    }
    u.landed = false;
    u.refueling = false; // kept for data-layout fidelity; the original never sets this true either

    u.height = std::min(64.0, u.height + kHeightRate * dt);
    double speed_mult = u.height < 64.0 ? std::max(0.3, u.height / 64.0) : 1.0;

    // Light mutual separation so airborne aircraft don't fully stack on one
    // another (they otherwise ignore unit collision entirely). Sums a gentle
    // push away from every nearby friendly plane and nudges out of the overlap a
    // little each tick -- small enough not to fight steering/attack runs.
    // Deterministic: grid.query order is fixed and the maths is pure.
    {
        constexpr double kAirSep = 22.0;
        double sx = 0.0, sy = 0.0;
        for (auto ref : world.grid.query(u.common.x, u.common.y, kAirSep)) {
            if (ref.kind != EntityKind::Unit) continue;
            Unit* o = world.get(ref);
            if (!o || o == &u || !o->common.alive || !o->common.is_air ||
                o->common.team != u.common.team)
                continue;
            double dx = u.common.x - o->common.x, dy = u.common.y - o->common.y;
            double d = std::hypot(dx, dy);
            if (d > 0.01 && d < kAirSep) {
                sx += dx / d * (kAirSep - d);
                sy += dy / d * (kAirSep - d);
            }
        }
        u.common.x += std::clamp(sx * 0.5 * dt, -3.0, 3.0);
        u.common.y += std::clamp(sy * 0.5 * dt, -3.0, 3.0);
    }

    // (An empty-clip bomber no longer auto-flies home to rearm -- planes only
    // return when the player orders it via the Land button or fuel runs low.
    // It loiters and re-engages once its clip reloads, or waits for the next
    // order/Land command.)

    u.fuel -= dt * drain_per_sec;
    if (u.fuel <= 0) {
        u.fuel = 0;
        if (home.valid) {
            u.attack_target = kNullRef;
            u.move_goal = Vec2{home.x, home.y}; // glide home to refuel
        } else {
            u.diving = true; // nowhere to land -> crash
            return;
        }
    } else if (u.fuel < 25 && home.valid && !u.forced) { // low fuel -> return to base
        u.attack_target = kNullRef;
        u.move_goal = Vec2{home.x, home.y};
    }

    // Aircraft attack-ground (T + right-click, bombers/ohka): fly to the point
    // and bomb it (bombers, nuke-aware) or kamikaze into it (ohka), forever,
    // until re-ordered. Yields to a fuel return (which sets move_goal).
    if (u.attack_ground && !u.move_goal) {
        Vec2 g = *u.attack_ground;
        double d = std::hypot(g.x - u.common.x, g.y - u.common.y);
        if (u.name == "ohka") { // kamikaze straight into the point
            steer_toward(u, g.x, g.y, dt, speed_mult * 1.4);
            if (d < 28) {
                world.events.push(
                    {EventType::Effect, "spr_explosion_large", u.common.x, u.common.y, 0, kNullRef, ""});
                world.events.push({EventType::Sound, "explosion", u.common.x, u.common.y, 0, kNullRef, ""});
                for (auto ref2 : world.grid.query(u.common.x, u.common.y, 60)) {
                    EntityCommon* o = world.common(ref2);
                    if (!o || !o->alive || o->team < 0 || world.control.allied(o->team, u.common.team)) continue;
                    if (o->kind != EntityKind::Unit && o->kind != EntityKind::Building) continue;
                    if (std::abs(o->x - u.common.x) < 50 && std::abs(o->y - u.common.y) < 50) {
                        world.hurt(ref2, u.attack + 20);
                        if (o->kind == EntityKind::Building) world.get_building(ref2)->big_death = true;
                    }
                }
                u.common.alive = false;
            }
            return;
        }
        steer_toward(u, g.x, g.y, dt, speed_mult); // bomber: keep passing over the point
        if (u.is_bomber && d < u.range_px * 0.7 && u.reload_timer <= 0 && u.clip_ammo > 0 && u.attack > 0) {
            u.reload_timer = u.reload;
            if (u.nuke_loaded) { u.clip_ammo = 0; u.nuke_loaded = false; world.spawn_bomb(u, kNullRef, true, g); }
            else { u.clip_ammo--; world.spawn_bomb(u, kNullRef, false, g); }
        }
        return;
    }

    EntityRef tgt = u.attack_target;
    EntityCommon* tgt_c = world.common(tgt);
    bool tgt_alive = tgt_c && tgt_c->alive;
    bool keep_forced = u.forced && tgt.valid() && tgt_alive;
    // A non-forced (self-selected) target that's drifted out of practical
    // engagement range gets dropped rather than chased forever -- same
    // give-up idea as ground units' sight_px*1.6 threshold in
    // unit_behavior.cpp's update_combat, so a plane that "can't shoot it
    // right now" lets go and looks for something else instead of staying
    // locked on regardless of distance.
    bool tgt_out_of_range =
        tgt_alive && !u.forced &&
        std::hypot(tgt_c->x - u.common.x, tgt_c->y - u.common.y) > u.sight_px * 1.6;
    if (!keep_forced && (!tgt.valid() || !tgt_alive || tgt_out_of_range)) {
        tgt = kNullRef;
        // Self-selecting a NEW target only happens when it's actually
        // appropriate to: a bomber never does (obj_unit/Step.gml never
        // auto-targets one either -- it only ever bombs a target the
        // player explicitly attacked), and a unit under a plain passive
        // move order (move_goal, no rally/forced) leaves enemies alone
        // until it arrives, same as every ground/naval unit -- only while
        // idling/loitering or on an attack-move (rally) does a fighter
        // look for something to engage on its own.
        if (!u.move_goal) {
            if (!u.is_bomber) {
                tgt = world.nearest(u.common.x, u.common.y, u.sight_px, [&](EntityRef, EntityCommon& c) {
                    return c.alive && c.team >= 0 && !world.control.allied(c.team, u.common.team) &&
                           (c.kind == EntityKind::Unit || c.kind == EntityKind::Building);
                });
            } else if (u.rally.has_value()) {
                // A bomber on an ATTACK-MOVE (rally) -- which is how the AI sends
                // bombers at the enemy -- self-acquires the nearest enemy GROUND
                // target (buildings or non-air units; it can't bomb aircraft) so
                // it actually drops its payload instead of just loitering. (A
                // plain loitering bomber with no rally still holds fire, as
                // before, so it doesn't randomly bomb near its own base.)
                tgt = world.nearest(u.common.x, u.common.y, u.sight_px, [&](EntityRef, EntityCommon& c) {
                    return c.alive && c.team >= 0 && !world.control.allied(c.team, u.common.team) &&
                           !c.is_air &&
                           (c.kind == EntityKind::Unit || c.kind == EntityKind::Building);
                });
            }
        }
        u.attack_target = tgt;
        u.forced = false;
        tgt_c = world.common(tgt);
    }

    if (tgt.valid() && tgt_c && u.name == "ohka") { // kamikaze: dive into the target & detonate
        double d = std::hypot(tgt_c->x - u.common.x, tgt_c->y - u.common.y);
        steer_toward(u, tgt_c->x, tgt_c->y, dt, speed_mult * 1.4);
        if (d < 28) { // impact: heavy blast, plane is sacrificed
            world.events.push(
                {EventType::Effect, "spr_explosion_large", u.common.x, u.common.y, 0, kNullRef, ""});
            world.events.push({EventType::Sound, "explosion", u.common.x, u.common.y, 0, kNullRef, ""});
            for (auto ref2 : world.grid.query(u.common.x, u.common.y, 60)) {
                EntityCommon* o = world.common(ref2);
                if (!o || !o->alive || o->team < 0 || world.control.allied(o->team, u.common.team)) continue;
                if (o->kind != EntityKind::Unit && o->kind != EntityKind::Building) continue;
                if (std::abs(o->x - u.common.x) < 50 && std::abs(o->y - u.common.y) < 50) {
                    world.hurt(ref2, u.attack + 20);
                    if (o->kind == EntityKind::Building) world.get_building(ref2)->big_death = true;
                }
            }
            u.common.alive = false;
        }
        return;
    }

    if (tgt.valid() && tgt_c) {
        double tdx = tgt_c->x - u.common.x, tdy = tgt_c->y - u.common.y;
        double d = std::hypot(tdx, tdy);
        if (u.strafe_break) {
            // Break-off: having just overflown the target, keep going on the
            // current heading to build separation, then swing back in for the
            // next pass. This is what turns a tight orbit into a proper
            // strafing RUN -- pass, peel off, loop, pass again.
            steer_toward(u, u.common.x + std::cos(u.heading) * 80.0,
                         u.common.y + std::sin(u.heading) * 80.0, dt, speed_mult);
            u.strafe_t -= dt;
            if (u.strafe_t <= 0.0) u.strafe_break = false;
            return;
        }
        if (d > u.range_px * 0.85) {
            steer_toward(u, tgt_c->x, tgt_c->y, dt, speed_mult); // line up the run
        } else {
            // Firing pass: fly THROUGH the target (aim past it) so the guns
            // bear on it as it strafes across, rather than circling in place.
            steer_toward(u, tgt_c->x + tdx, tgt_c->y + tdy, dt, speed_mult);
            if (u.is_bomber) {
                if (u.reload_timer <= 0 && u.clip_ammo > 0 && u.attack > 0) {
                    u.reload_timer = u.reload;
                    if (u.nuke_loaded) { // drop the single atomic bomb, then revert to normal bombs
                        u.clip_ammo = 0;
                        u.nuke_loaded = false;
                        world.spawn_bomb(u, tgt, /*nuke=*/true);
                    } else {
                        u.clip_ammo--;
                        world.spawn_bomb(u, tgt);
                    }
                }
            } else if (u.clip_reload_timer <= 0 && u.attack > 0) { // fighter: burst-fire
                if (u.reload_timer <= 0 && u.clip_ammo > 0) {
                    u.reload_timer = 0.12; // rapid fire within the burst
                    u.clip_ammo--;
                    // Bullets leave the plane's NOSE (fixed sprite muzzle
                    // point, mirrored by facing), not the sprite centre.
                    world.spawn_projectile(u, tgt, plane_muzzle(u));
                    world.events.push(
                        {EventType::Sound, "plane_gun", u.common.x, u.common.y, 0, kNullRef, ""});
                    if (u.clip_ammo <= 0) u.clip_reload_timer = 2.4; // reload after the burst
                }
            }
            // Overflew the target -> break off and loop back for another pass.
            if (d < u.range_px * 0.4) { u.strafe_break = true; u.strafe_t = 1.1; }
        }
        return;
    }

    if (u.move_goal) { // fly straight to a move order
        double d = std::hypot(u.move_goal->x - u.common.x, u.move_goal->y - u.common.y);
        if (d < 8) {
            u.hover = u.move_goal;
            u.move_goal.reset();
        } else {
            steer_toward(u, u.move_goal->x, u.move_goal->y, dt, speed_mult);
        }
        return;
    }

    if (u.rally) { // attack-move: fly toward it (still eligible to peel off onto anything sighted, above)
        double d = std::hypot(u.rally->x - u.common.x, u.rally->y - u.common.y);
        if (d < 8) {
            u.hover = u.rally;
            u.rally.reset();
        } else {
            steer_toward(u, u.rally->x, u.rally->y, dt, speed_mult);
        }
        return;
    }

    if (!u.hover) u.hover = Vec2{u.common.x, u.common.y}; // idle: loiter in a circle
    orbit(u, u.hover->x, u.hover->y, dt, speed_mult);
}

} // namespace ww::sim
