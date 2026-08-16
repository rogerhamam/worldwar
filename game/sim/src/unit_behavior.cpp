// Unit behavior: direct port of Unit.update and its helpers from
// game/entity.py, MINUS the aircraft subsystem (`is_air` units dispatch to
// aircraft_behavior.cpp's update_aircraft instead -- see world.h's class
// comment). Ships (battleship/yamato/destroyer/frigate/torpedo boat) and
// artillery ARE ported here with their original obj_missile-based firing:
// per-unit muzzle offsets (ship_shot_origin) and, for battleship/yamato,
// the clip_ammo/clip_reload burst-then-pause cadence from
// assets/gmk/objects/obj_unit/Step.gml -- direct GML ports, not the Python
// reference version's own (different) multi-shot broadside quirk.
#include "sim/behavior.h"
#include "sim/control.h"
#include "sim/pathfind.h"
#include "sim/unit.h"
#include "sim/world.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace ww::sim {

namespace {

// Matches the "body" size used both to block ordinary movement
// (blocked_by_unit) and to detect a genuine overlap worth correcting
// (resolve_overlap) -- two units are "overlapping" exactly when they're
// closer together than blocked_by_unit would ever have allowed them to
// walk into each other (allies executing a MOVE order are the one
// exception -- see blocked_by_unit's ignore_allies comment -- so those
// routinely close nearer than this, which is what makes resolve_overlap's
// job a routine one during a group move rather than a rare edge case).
constexpr double kBodyRadius = 14.0;

// Minimum center-to-center distance two unit bodies are allowed to close to.
// Both blocked_by_unit (stops a unit stepping any closer than this to
// another) and resolve_overlap (the one-time unstick pass for bodies that
// are somehow already closer than this) share this single constant so they
// can never drift apart and leave a gap where neither considers a pair
// "resolved". The client's formation slot spacing (kMinSpacing in
// game_client.cpp) is kept comfortably ABOVE this so a settled rank is never
// seen as overlapping and re-jostled.
// Deliberately less than kBodyRadius * 2.0 (which would mean bodies only
// ever touch edge-to-edge, never overlapping): units are allowed to crowd
// into each other by design, AoE-style, so a group packed into a tight space
// visually bunches up with partial body overlap instead of jamming into a
// wall the instant edges touch.
constexpr double kSeparation = kBodyRadius * 1.4;

// Gatherers pack tighter than that against EACH OTHER. Their whole job is
// to crowd a single point -- a berry bush, an ore node, a drop-off door --
// and the general spacing only fits about six bodies around a resource's
// gather ring, so the seventh onward can't reach and peels off to look
// elsewhere (see update_gather's progress-stall check) even when the node
// itself is nowhere near tapped out. Letting villagers overlap a bit more
// fits meaningfully more of them per node. Only applies when BOTH bodies
// in the pair are gatherers, so military spacing/formations are untouched
// (the client's formation slot spacing is sized against kSeparation).
constexpr double kGathererSeparation = kBodyRadius * 1.15;

// Ships are far bigger than the body radius units are sized around, so they
// visibly overlapped at the general spacing. A ship-vs-ship pair keeps a wider
// berth (query radii below widen to match when the mover is a ship, so a
// neighbour just outside the normal radius isn't missed).
constexpr double kShipSeparation = kBodyRadius * 2.6;

// The separation actually enforced between one specific pair of units.
// Every distance test below goes through this rather than kSeparation
// directly, so blocked_by_unit and resolve_overlap can't disagree about
// how close a given pair is allowed to get -- if they did, one would
// forbid a gap the other was actively pushing units into, and the pair
// would jitter against each other forever. Note the grid queries still
// use kSeparation as their RADIUS: it's the larger of the two, so it
// can't miss a neighbour that a tighter pair rule would care about.
inline double pair_separation(const Unit& a, const Unit& b) {
    if (a.common.is_ship || b.common.is_ship) return kShipSeparation;
    return (a.is_gatherer && b.is_gatherer) ? kGathererSeparation : kSeparation;
}

// True if some OTHER living unit's body already occupies (px, py) -- i.e. is
// closer than kSeparation to it. Enemies always block. Allies normally
// block too (gatherers/builders/attackers still need to jostle apart to
// spread across a resource/foundation/melee ring -- see the regression this
// guarded: two gatherers converging on the same tree from nearly the same
// angle used to spread to opposite sides purely because ally-blocking
// forced them to steer around each other early; without it they'd both
// beeline for, and fight over, the exact same approach point right at the
// tree). ignore_allies=true (only ever passed for a player MOVE order's own
// pathing -- see step_toward's callers in the move_goal branch) instead
// lets allies pass through each other while walking to a destination, so a
// group/formation move doesn't jostle and steer around every other selected
// unit converging on its own nearby slot (which read as messy rather than
// a clean group move or formation). Two allies that do end up literally
// coincident after phasing through one another are un-stacked by
// resolve_overlap below instead, which already only ever acts between
// allies. This is a pure "is this spot free" check -- it never moves
// anyone, so it can't push/shove; the caller (step_toward) is responsible
// for finding a different, actually-clear direction to move in instead.
// Aircraft never block or get blocked by this (they fly above ground
// traffic entirely). Terrain/buildings/resources are handled separately by
// World::passable and still block as before.
bool blocked_by_unit(World& world, const Unit& self, double px, double py, bool ignore_allies = false) {
    if (self.common.is_air) return false;
    // Widen the neighbour search when the mover is a ship, so a ship-separation
    // pair (larger than kSeparation) can't slip through the query radius.
    double qr = self.common.is_ship ? kShipSeparation : kSeparation;
    for (auto ref : world.grid.query(px, py, qr)) {
        if (ref.kind != EntityKind::Unit) continue;
        Unit* other = world.get(ref);
        if (!other || other == &self || !other->common.alive || other->common.is_air) continue;
        if (ignore_allies && world.control.allied(self.common.team, other->common.team)) continue;
        if (std::hypot(other->common.x - px, other->common.y - py) < pair_separation(self, *other)) {
            return true;
        }
    }
    return false;
}

// The planning predicate every A* call in this file shares.
//
// passable_PLANNING (not passable): routes must avoid not-yet-started
// foundations even though units can physically walk over them, or a path drawn
// through one becomes a wall the moment somebody starts building it.
//
// Deliberately UNIT-BLIND. A version that treated settled bodies as terrain on
// retry plans was written and measured here and did not earn its keep: every
// movement test passed identically with and without it, which by this file's
// own standard makes it unproven complexity (and it costs a spatial query per
// tile examined). The cases it was meant to catch are handled instead by
// approach_stalled actually detecting oscillation and by step_toward's
// phase-through tier. Revisit only alongside a test that fails without it.
auto make_passfn(World& world, const Unit& u) {
    return [&world, is_ship = u.common.is_ship, phase = u.phase_trees](int gx, int gy) {
        if (gx < 0 || gx >= world.cols || gy < 0 || gy >= world.rows) return false;
        double px = gx * TILE + TILE / 2.0, py = gy * TILE + TILE / 2.0;
        return world.passable_planning(/*is_air=*/false, is_ship, px, py, phase);
    };
}

// blocked_by_unit stops units from ever walking into each other, EXCEPT
// allies executing a MOVE order (ignore_allies -- see its comment), who
// pass through each other freely -- so allied overlap during a group move
// is now a routine, continuous byproduct of ordinary movement (several
// units converging on nearby-but-distinct destinations regularly cross
// paths and briefly coincide), not just the rare edge case this originally
// existed for (spawned on top of each other, several convergent orders
// landing on the exact same point, etc.) -- though it still catches those
// too. Called unconditionally, once per unit per tick, before that unit's
// own order logic runs: nudges apart from the single closest overlapping
// ally by half the overlap (a couple of ticks to fully separate for a
// typical case); the nudge is small enough that the unit's normal movement
// this same tick still applies on top of it. Only ever
// resolves overlap against an ALLIED unit (see Control::allied) -- without
// that check, a player's units ending up overlapped with an enemy's (easy
// to happen while approaching to attack) would shove the enemy unit around
// too, which reads as the player being able to push the enemy out of the
// way just by walking into them. (Enemy-vs-enemy overlap, e.g. from a unit
// dying and a new one spawning on the same spot, is left for blocked_by_unit
// to simply prevent from recurring rather than actively resolved -- enemies
// still solidly block each other outright, only allies ever phase through,
// so this stays rare.)
void resolve_overlap(Unit& u, World& world) {
    if (u.common.is_air) return;
    // A unit that has arrived / is holding station / is in melee should NOT be
    // shoved off its spot by a passing mover -- the incoming mover yields
    // instead (asymmetric push below). "Settled" == no active move/rally order
    // (an attacking unit chasing a target keeps its move_goal clear too, so it
    // counts as settled here and holds its firing position).
    bool u_settled = !u.move_goal && !u.rally;
    // A unit actively executing a plain MOVE order (u.move_goal, as opposed
    // to u.rally's AI attack-move charge, which is a different code path
    // still using normal ally-blocking) always ignores ally blocking for
    // its own step_toward calls (see blocked_by_unit's ignore_allies
    // comment) -- it can already walk straight through ANY other ally,
    // moving or settled, so it never needs separating from one while still
    // traveling. Nudging it here would only fight its own deliberate step
    // toward the goal for no benefit: this was actively preventing precise
    // arrival at a formation/group move's assigned destination, not just
    // adding cosmetic jitter -- a unit's "am I making progress" stall check
    // (update_unit) kept reading as stalled because THIS nudge, not any
    // genuine obstruction, was what kept knocking it off a steady path.
    // Only a unit that's actually stopped (arrived/idle/holding, or mid a
    // rally charge) still needs this pass, to un-stack it from another
    // stopped ally.
    if (u.move_goal) return;
    Unit* worst = nullptr;
    double worst_dist = 0.0, worst_overlap = 0.0;
    double qr = u.common.is_ship ? kShipSeparation : kSeparation; // widen for large ships
    for (auto ref : world.grid.query(u.common.x, u.common.y, qr)) {
        if (ref.kind != EntityKind::Unit) continue;
        Unit* other = world.get(ref);
        if (!other || other == &u || !other->common.alive || other->common.is_air) continue;
        if (!world.control.allied(u.common.team, other->common.team)) continue;
        double dist = std::hypot(other->common.x - u.common.x, other->common.y - u.common.y);
        // Ranked by how far INTO each other the pair is, not by raw
        // distance: two gatherers are allowed closer than a gatherer and a
        // soldier are (pair_separation), so a nearer pair isn't necessarily
        // the more overlapped one. Positive overlap also replaces the old
        // "closer than the threshold" test -- anything at or beyond its own
        // pair's separation simply never scores.
        double overlap = pair_separation(u, *other) - dist;
        if (overlap > worst_overlap) {
            worst = other;
            worst_dist = dist;
            worst_overlap = overlap;
        }
    }
    if (!worst) return;

    // Settled unit overlapped by a mover: hold, and let the mover route around
    // us on its own tick (below). Only movers give ground.
    bool worst_settled = !worst->move_goal && !worst->rally;
    if (u_settled && !worst_settled) return;

    double dx = u.common.x - worst->common.x, dy = u.common.y - worst->common.y;
    double nx, ny;
    if (worst_dist < 1e-4) {
        // Exactly coincident (e.g. two units spawned on the same point) --
        // straight-line direction is undefined, so pick an arbitrary one.
        double a = world.rng.uniform(0.0, 2.0 * M_PI);
        nx = std::cos(a);
        ny = std::sin(a);
    } else {
        nx = dx / worst_dist;
        ny = dy / worst_dist;
    }
    // A mover displacing a SETTLED unit steps fully aside itself (takes the
    // whole overlap); two movers (or two coincident settled units) split it
    // evenly as before. This resolves crowding by the incoming unit giving
    // way rather than both jittering, so arrived units stay on their goals.
    double frac = (!u_settled && worst_settled) ? 1.0 : 0.5;
    double push = worst_overlap * frac;
    double tx = u.common.x + nx * push, ty = u.common.y + ny * push;
    // NOTE: deliberately checks World::passable only, not blocked_by_unit --
    // the whole point of this pass is to make (partial, frac<1.0) progress
    // pulling apart from `worst` while still inside the pair's separation,
    // which blocked_by_unit would itself forbid. Landing briefly inside
    // separation of some OTHER third unit is fine: that pair gets picked up
    // and resolved on a subsequent tick the same way, same as any other
    // overlap.
    if (world.passable(/*is_air=*/false, u.common.is_ship, tx, ty)) {
        u.common.x = tx;
        u.common.y = ty;
    }
}

// Moves `u` one tick's distance toward (tx, ty), steering around anything
// blocking the direct line. The steering itself is a small, three-tier
// cascade, each tier only consulted if the previous one found nothing:
//
//   1. WIDE swing: angled candidates that are clear not just for this one
//      step but for a full body-width-plus beyond it (the `probe` point) --
//      the unit only commits to a direction with genuine room, so it swings
//      clear of a blocker instead of grinding along its edge.
//   2. TIGHT squeeze: same candidate angles, immediate-step room only --
//      for a crowded spot where nothing has a wide-open lane but a small
//      sideways nudge still helps.
//   3. RETREAT: heads directly away from the combined position of whatever
//      is crowding the unit right now (not a fixed angle relative to the
//      distant goal -- see the tier's own comment below for why that
//      distinction matters), for a unit genuinely wedged, e.g. boxed in
//      between two other units, where every forward-ish heading is blocked
//      and the only way to eventually get around is to back off and make
//      room first.
//
// Whichever tier succeeds, the chosen heading is held (Unit::avoid_offset/
// avoid_committed) until it's either no longer needed (direct line open) or
// no longer clear, rather than re-decided every tick -- re-deciding from
// scratch each tick picks whichever candidate is only MARGINALLY clear at
// that instant, which can flip between ticks as a neighbor shifts by a
// pixel (or even just from the unit's own movement changing the geometry).
// That flip-flopping is what reads as jitter. Deterministic throughout
// (fixed angle offsets, no RNG); which side gets tried first when a fresh
// decision IS needed is a FIXED global convention, not decided per-unit, so
// two units meeting head-on independently arrive at compatible choices.
// Returns true IF AND ONLY IF the unit is now actually at (tx, ty) --
// never as a stand-in for "gave up" or "done trying". Several call sites
// (advance_to_building's per-waypoint loop, rally, the plain move_goal
// case) treat a true return as license to advance to the next step or
// clear the order outright; conflating that with "couldn't move this tick"
// used to let a single bad tick (e.g. every steering candidate happening to
// be blocked) silently cancel a perfectly recoverable order. A unit that
// merely couldn't find a clear step this tick returns false and simply
// tries again next tick -- Unit::stuck_t/stall_strikes (update_unit, this
// file) is the sole mechanism responsible for eventually abandoning an
// order that's genuinely never going anywhere, not this return value.
bool step_toward(Unit& u, double tx, double ty, double dt, World& world,
                 double speed_override_px = -1.0, bool ignore_ally_blocking = false) {
    double dx = tx - u.common.x, dy = ty - u.common.y;
    double dist = std::hypot(dx, dy);
    double speed = (speed_override_px > 0.0) ? std::min(u.speed_px, speed_override_px) : u.speed_px;
    double step = speed * dt;
    auto clear = [&](double px, double py) {
        return world.passable(u.common.is_air, u.common.is_ship, px, py, u.phase_trees) &&
               !blocked_by_unit(world, u, px, py, ignore_ally_blocking);
    };
    if (dist <= step || dist < 1e-3) {
        if (clear(tx, ty)) {
            u.common.x = tx; u.common.y = ty;
            return true;
        }
        return false; // final pixel is blocked -- not actually there yet
    }
    double base = std::atan2(dy, dx);
    double nx = u.common.x + std::cos(base) * step;
    double ny = u.common.y + std::sin(base) * step;
    bool moved = clear(nx, ny);
    double chosen_offset = 0.0;
    bool committed = false;

    if (moved) {
        u.avoid_committed = false; // direct line open -- no detour needed, drop any old commitment
    } else {
        double probe = std::max(step, kBodyRadius * 2.4);
        auto try_step = [&](double off) {
            double a = base + off;
            double cx = u.common.x + std::cos(a) * step;
            double cy = u.common.y + std::sin(a) * step;
            if (!clear(cx, cy)) return false;
            nx = cx; ny = cy;
            return true;
        };
        auto try_wide = [&](double off) {
            if (!try_step(off)) return false;
            double a = base + off;
            return clear(u.common.x + std::cos(a) * probe, u.common.y + std::sin(a) * probe);
        };

        // Still-valid earlier commitment: keep using it rather than
        // re-deciding (see the function comment above).
        if (u.avoid_committed && try_step(u.avoid_offset)) {
            moved = true;
            chosen_offset = u.avoid_offset;
            committed = true;
        } else {
            // Which side (positive vs negative offset from `base`) gets
            // tried first is a FIXED global convention -- always positive
            // first -- not decided per-unit from anything that can change
            // while the unit is mid-maneuver (an earlier version derived it
            // from the unit's own position, which could flip the unit's own
            // preference for which way to swerve mid-decision for no real
            // reason, and gave two different units no reason to agree on
            // which side to pass each other on). A single shared convention
            // is what lets two units meeting head-on each independently
            // arrive at compatible choices without any negotiation, the
            // same way real traffic passes consistently on one side instead
            // of each side guessing.
            static const double kWide[] = {0.4, 0.75, 1.1, 1.45};
            static const double kTight[] = {0.4, 0.75, 1.1, 1.45, 1.8};
            struct Tier { const double* offs; int n; bool wide; };
            const Tier tiers[] = {
                {kWide, 4, true},
                {kTight, 5, false},
            };
            for (const Tier& t : tiers) {
                if (moved) break;
                for (int i = 0; i < t.n && !moved; ++i) {
                    for (double sign : {1.0, -1.0}) {
                        double off = sign * t.offs[i];
                        if (t.wide ? try_wide(off) : try_step(off)) {
                            moved = true;
                            chosen_offset = off;
                            committed = true;
                            u.avoid_retry_count = 0; // real forward-ish progress, not a retreat
                            break;
                        }
                    }
                }
            }

            // RETREAT tier: nothing forward-ish worked, so this unit is
            // genuinely crowded. Rather than backing off relative to the
            // (possibly still-distant) goal -- which just re-aims the unit
            // straight back at the same crowd the moment this retreat stops
            // being needed, producing exactly the back-and-forth wander
            // this replaced -- flee the ACTUAL local crowd: average the
            // direction to every nearby unit and head the opposite way.
            // That points out of the pocket regardless of where the goal
            // is. HELD as a commitment same as the other tiers (an earlier
            // version deliberately didn't, reasoning a retreat was just a
            // one-tick corrective nudge -- that was wrong: a symmetric
            // pinch, e.g. a mover centered squarely between two blockers,
            // produces a stable 2-tick limit cycle without it -- retreat
            // one step clears the direct line again, so the very next tick
            // advances straight back into the same blocked spot and
            // retreats again, repeating indefinitely. Holding the retreat
            // heading means it actually puts distance behind it instead of
            // immediately re-inviting the same block).
            //
            // In a dense crowd one retreat often just lands in another
            // pocket, so this tier can fire several times in a row -- each
            // one individually "succeeding" (the unit does move) but net
            // reading as vibrating in place rather than escaping. Rising
            // avoid_retry_count (consecutive retreats with no intervening
            // real progress) widens the candidate search, so repeated
            // failures escalate into actually breaking out instead of
            // repeating the same too-small nudge.
            if (!moved) {
                double sumx = 0.0, sumy = 0.0;
                int n = 0;
                for (auto ref : world.grid.query(u.common.x, u.common.y, kSeparation * 1.5)) {
                    if (ref.kind != EntityKind::Unit) continue;
                    Unit* other = world.get(ref);
                    if (!other || other == &u || !other->common.alive || other->common.is_air) continue;
                    double ox = other->common.x - u.common.x, oy = other->common.y - u.common.y;
                    double od = std::hypot(ox, oy);
                    if (od < 1e-6) continue;
                    sumx += ox / od; sumy += oy / od;
                    ++n;
                }
                if (n > 0) {
                    double flee = std::atan2(-sumy, -sumx);
                    static const double kFleeJitter[] = {0.0, 0.35, -0.35, 0.7, -0.7, 1.05, -1.05, 1.4, -1.4};
                    int tries = std::min(static_cast<int>(std::size(kFleeJitter)), 5 + u.avoid_retry_count * 2);
                    for (int i = 0; i < tries; ++i) {
                        double off = flee - base + kFleeJitter[i];
                        if (try_step(off)) {
                            moved = true;
                            chosen_offset = off;
                            committed = true;
                            break;
                        }
                    }
                    if (moved) ++u.avoid_retry_count;
                }
            }
        }
    }

    // LAST RESORT: phase through allied bodies to unwedge.
    //
    // Every tier above can fail together when the pocket is formed partly by a
    // unit and partly by terrain -- the player's "concavity of the villager and
    // the refinery". The retreat tier is the intended escape, but it averages
    // only the directions to nearby UNITS, so with one ally to the west and a
    // tree to the east it flees due east straight into the tree and the unit
    // has no legal heading whatsoever. It then sits there forever: no movement
    // means no progress, and (before this) the ladders above could replan all
    // they liked because the unit could not take the first step of ANY route.
    //
    // Allies already phase through each other freely during a plain move order
    // (blocked_by_unit's ignore_allies), and resolve_overlap un-stacks whatever
    // that leaves behind -- so this grants the same latitude, but only after a
    // full second of total immobility, and only far enough to get moving again.
    // Enemies still block absolutely: this can never be used to walk through an
    // enemy line.
    if (!moved && ++u.wedged_ticks >= 20 && !ignore_ally_blocking) {
        auto clear_ally_phased = [&](double px, double py) {
            return world.passable(u.common.is_air, u.common.is_ship, px, py, u.phase_trees) &&
                   !blocked_by_unit(world, u, px, py, /*ignore_allies=*/true);
        };
        static const double kEscape[] = {0.0, 0.4, -0.4, 0.9, -0.9, 1.6, -1.6, 2.4, -2.4, 3.14159};
        for (double off : kEscape) {
            double a = base + off;
            double cx = u.common.x + std::cos(a) * step, cy = u.common.y + std::sin(a) * step;
            if (!clear_ally_phased(cx, cy)) continue;
            nx = cx; ny = cy;
            moved = true;
            chosen_offset = off;
            committed = false; // don't hold a phase-through heading as a commitment
            break;
        }
    }

    if (!moved) return false; // fully boxed in on every heading this tick -- not there yet, try again next tick
    u.wedged_ticks = 0;
    u.avoid_offset = chosen_offset;
    u.avoid_committed = committed;
    u.common.x = nx; u.common.y = ny;
    if (dx > 1) u.facing = 1; else if (dx < -1) u.facing = -1;
    u.moving = true;
    return false;
}

double dist_to(const Unit& u, const EntityCommon& c) {
    return std::hypot(c.x - u.common.x, c.y - u.common.y);
}


// Distance remaining along the unit's actual A*-routed path (current
// position -> next waypoint -> ... -> move_goal), NOT as-the-crow-flies to
// move_goal. Used by the give-up check below instead of raw straight-line
// distance: any real detour around an obstacle has to walk away from the
// goal for a while (going around before coming back), which raw distance
// sees as "not making progress" and used to abandon the order mid-detour --
// exactly the case a proper A*-routed path around a V-shaped obstruction
// hits. Distance along the actual route only increases if the unit is
// truly failing to advance along it.
double remaining_path_distance(const Unit& u) {
    if (u.path.empty() || u.path_i >= u.path.size()) {
        return u.move_goal ? std::hypot(u.move_goal->x - u.common.x, u.move_goal->y - u.common.y) : 0.0;
    }
    double d = std::hypot(u.path[u.path_i].x - u.common.x, u.path[u.path_i].y - u.common.y);
    for (size_t i = u.path_i; i + 1 < u.path.size(); ++i) {
        d += std::hypot(u.path[i + 1].x - u.path[i].x, u.path[i + 1].y - u.path[i].y);
    }
    return d;
}

// Direct port of the per-unit origin_x/origin_y block in obj_unit/Step.gml
// (the shell/torpedo-spawning section) for every unit that fires an
// obj_missile-style shot: battleship and yamato pick one of a few barrel
// positions at random (n) within whichever turret group the CURRENT
// clip_ammo count falls into (so a burst visibly alternates between e.g.
// fore and aft turrets as the clip empties), frigate/destroyer/torpedo
// boat/artillery just have a fixed muzzle offset. Everything else fires
// from its own center, same as before this was ported (u.facing mirrors
// the GML local `facing`: 1 = right, -1 = left).
Vec2 ship_shot_origin(const Unit& u, Rng& rng) {
    double fx = u.facing;
    if (u.name == "battleship") {
        int n = rng.randint(0, 2);
        if (u.clip_ammo > 3) return {u.common.x + fx * (18 + n * 3), u.common.y - 3 - n * 2};
        return {u.common.x + fx * (34 + n * 3), u.common.y + 10 - n * 2};
    }
    if (u.name == "yamato") {
        int n = rng.randint(0, 2);
        bool near = u.clip_ammo > 9 || (u.clip_ammo > 3 && u.clip_ammo <= 6);
        if (near) return {u.common.x + fx * (34 + n * 5), u.common.y - n * 2};
        return {u.common.x + fx * (57 + n * 5), u.common.y + 11 - n * 2};
    }
    if (u.name == "frigate") {
        int n = rng.randint(0, 3);
        return {u.common.x + fx * (-68 + n * 20), u.common.y + 38 + n * 6};
    }
    if (u.name == "destroyer") return {u.common.x + fx * 22, u.common.y - 15};
    if (u.name == "torpedo boat") return {u.common.x + fx * 37, u.common.y};
    if (u.name == "artillery" || u.name == "artillery1") return {u.common.x + fx * 25, u.common.y - 14};
    // Ballistic missile: the shot leaves the tip of the raised rail (where the
    // loaded sprite shows the missile), not the chassis -- so the detached
    // projectile lines up exactly where the loaded missile was. Offset measured
    // from the launcher sprite origin (51,104) to the rail's missile nose (90,8).
    if (u.name == "ballistic missile") return {u.common.x + fx * 39, u.common.y - 96};
    return {u.common.x, u.common.y};
}

// Closest point on a building's rectangular footprint to the unit's
// current position (clamping to the rect is the standard nearest-point-
// on-AABB trick). Builders/repairers head here instead of the building's
// center, so several units given the same build/repair order approach
// from their own current side and naturally spread around the perimeter
// instead of all converging on one point behind/in front of each other.
Vec2 nearest_perimeter_point(const Unit& u, const Building& b) {
    double hw = b.foot_w * 0.5, hh = b.foot_h * 0.5;
    double px = std::clamp(u.common.x, b.common.x - hw, b.common.x + hw);
    double py = std::clamp(u.common.y, b.common.y - hh, b.common.y + hh);
    return {px, py};
}

// One candidate per side of the building's rect: `probe` is a point a full
// TILE beyond that side's MIDPOINT, used only to ask astar "is this side
// obstructed by something other than the target building itself" --
// deliberately NOT clamped to the unit's position (unlike
// nearest_perimeter_point): for a unit approaching from roughly the
// opposite side, clamping to its position pins the "left"/"right"
// candidates to whichever corner is nearest the unit, e.g. the bottom
// corners for a unit approaching from below -- and that corner can still
// be shadowed by a second, wider building the closest side is flush
// against, even though the middle of that same side is genuinely clear.
//
// The extra TILE offset matters too: every side's true edge point sits
// exactly on the building's OWN inclusive-blocked boundary (world.h's
// passable() comment), and astar's goal-tile check (pathfind.cpp) looks at
// that tile's CENTER, which for a "min" edge (cx-hw or cy-hh) rounds
// further INTO the rect -- making the goal self-blocked regardless of any
// neighbor. Probing a full tile past the midpoint is unambiguously outside
// the building on every side, so astar can correctly judge whether that
// side is externally obstructed instead of always failing on two of the
// four sides no matter what.
//
// `side` identifies which edge this is, so advance_to_building can derive
// the actual per-unit walked-to point afterward (see side_edge_point) --
// using the fixed midpoint as the final target too (instead of just for
// reachability probing) made every builder assigned to the same foundation
// beeline for the exact same pixel, queuing up single-file behind whoever
// got there first instead of spreading across the (still perfectly
// reachable) rest of that side.
enum class BuildingSide { Top, Bottom, Left, Right };
struct PerimeterCandidate { BuildingSide side; Vec2 mid_edge, probe; };

std::vector<PerimeterCandidate> perimeter_candidates(const Unit& u, const Building& b) {
    double hw = b.foot_w * 0.5, hh = b.foot_h * 0.5;
    double cx = b.common.x, cy = b.common.y;
    std::vector<PerimeterCandidate> pts = {
        {BuildingSide::Top, {cx, cy - hh}, {cx, cy - hh - TILE}},
        {BuildingSide::Bottom, {cx, cy + hh}, {cx, cy + hh + TILE}},
        {BuildingSide::Left, {cx - hw, cy}, {cx - hw - TILE, cy}},
        {BuildingSide::Right, {cx + hw, cy}, {cx + hw + TILE, cy}},
    };
    std::sort(pts.begin(), pts.end(), [&](const PerimeterCandidate& a, const PerimeterCandidate& c) {
        return std::hypot(a.probe.x - u.common.x, a.probe.y - u.common.y) <
               std::hypot(c.probe.x - u.common.x, c.probe.y - u.common.y);
    });
    return pts;
}

// The point this specific unit should actually walk to on the given side,
// clamped to the unit's own position along that side (same idea as
// nearest_perimeter_point) so several builders approaching the same
// reachable side spread out across it rather than all targeting one point.
Vec2 side_edge_point(const Unit& u, const Building& b, BuildingSide side) {
    double hw = b.foot_w * 0.5, hh = b.foot_h * 0.5;
    double cx = b.common.x, cy = b.common.y;
    double px = std::clamp(u.common.x, cx - hw, cx + hw);
    double py = std::clamp(u.common.y, cy - hh, cy + hh);
    switch (side) {
        case BuildingSide::Top: return {px, cy - hh};
        case BuildingSide::Bottom: return {px, cy + hh};
        case BuildingSide::Left: return {cx - hw, py};
        case BuildingSide::Right: return {cx + hw, py};
    }
    return {px, py};
}

// Pushes an arbitrary point on `side` a further TILE outward, same idea as
// perimeter_candidates' probe but for a specific per-unit edge point
// (which can differ from that side's own midpoint) -- used to sanity-check
// it's not itself shadowed by a different neighbor before committing to
// it, since the per-unit clamp in side_edge_point can still land near a
// corner even on a side whose midpoint is clear.
Vec2 side_probe_point(Vec2 edge, BuildingSide side) {
    switch (side) {
        case BuildingSide::Top: return {edge.x, edge.y - TILE};
        case BuildingSide::Bottom: return {edge.x, edge.y + TILE};
        case BuildingSide::Left: return {edge.x - TILE, edge.y};
        case BuildingSide::Right: return {edge.x + TILE, edge.y};
    }
    return edge;
}

// Routes a unit to a building's perimeter via A*, trying each side's
// candidate point (nearest first) until one has an actual path -- unlike a
// direct step_toward, this can route AROUND a second building blocking the
// straight line, not just slide along its wall. Reuses the unit's
// move-order path/path_i fields, which is safe here since build/repair
// movement and ordinary move orders are mutually exclusive (build_target/
// repair_target are checked before move_goal in update_unit and return
// before that block ever runs).
void advance_to_building(Unit& u, const Building& b, double dt, World& world) {
    if (!u.approach_target) {
        // First tick of this approach (or first tick after a fresh
        // (re)plan -- see approach_target's comment in unit.h): decide
        // which single side of the building to commit to, and stick with
        // it for the rest of the approach rather than re-deciding every
        // tick, which is what let the unit flip back to a blocked side
        // once it got close.
        Vec2 target = nearest_perimeter_point(u, b);
        double dist = std::hypot(target.x - u.common.x, target.y - u.common.y);
        // Only worth A*-routing (and paying its cost) when far enough that
        // a second building could plausibly be blocking the direct line.
        // Closer than that, just commit to the plain closest point --
        // every perimeter_candidates() point sits exactly on the target's
        // OWN edge, which is itself inside the target's own solid rect's
        // inclusive boundary (world.h's passable() comment), so astar's
        // goal-tile is frequently unreachable right up until the unit is
        // close enough that direct step_toward's one-axis slide can finish
        // the job anyway.
        // A REPLAN always routes, however close the target is. The
        // under-2-tiles shortcut below (commit to the nearest point and let
        // step_toward's one-axis slide finish) is right for a first attempt,
        // but it is precisely wrong for a retry: a short approach only stalls
        // when something is IN the way, and the shortcut's response is to keep
        // pressing straight at it with no route at all. Measured on the
        // player's screenshot geometry -- a carrier 36px from the refinery door
        // with two villagers 32px apart in front of it (each demanding 16.1px
        // of clearance, so the gap is ~0.2px too narrow to thread) -- the unit
        // sat at path=0 cycling replans forever, when one tile sideways was
        // open the whole time. astar aims at cand.probe, a tile BEYOND the
        // side's midpoint and so outside the building's own inclusive-blocked
        // boundary, which is what makes routing viable at this range at all.
        if (dist > 2.0 * TILE || u.approach_replans > 0) {
            auto passfn = make_passfn(world, u);
            for (const auto& cand : perimeter_candidates(u, b)) {
                auto raw = astar(world.cols, world.rows, TILE, u.common.x, u.common.y, cand.probe.x,
                                 cand.probe.y, passfn);
                if (!raw.empty()) {
                    // Prefer this unit's own point on the side (spreads
                    // several builders across it instead of queuing them
                    // at one pixel) but only if IT is also clear -- the
                    // per-unit clamp can still land in a corner shadowed
                    // by a different neighbor even on a side whose
                    // midpoint (just proven reachable above) is fine.
                    // Falls back to the shared, already-verified midpoint
                    // in that case, which only costs spreading for the
                    // unit(s) in that situation instead of getting them
                    // stuck again.
                    Vec2 edge = side_edge_point(u, b, cand.side);
                    Vec2 edge_probe = side_probe_point(edge, cand.side);
                    if (!world.passable(/*is_air=*/false, u.common.is_ship, edge_probe.x, edge_probe.y)) {
                        edge = cand.mid_edge;
                    }
                    for (auto& p : raw) u.path.push_back(Vec2{p.x, p.y});
                    u.path.back() = edge; // walk to the chosen edge point, not the outward probe
                    u.path_i = 0;
                    target = edge;
                    break;
                }
            }
        }
        u.approach_target = target;
    }
    if (!u.path.empty()) {
        Vec2 wp = u.path[u.path_i];
        if (step_toward(u, wp.x, wp.y, dt, world)) {
            u.path_i++;
            if (u.path_i >= u.path.size()) u.path.clear();
        }
    } else {
        // Either already close, the A*-routed path is fully walked, or no
        // candidate side had a path at all (e.g. fully boxed in) -- direct
        // step_toward's one-axis slide handles the final short approach
        // (including working around the target's own inclusive-blocked
        // edge) just fine, same as before A* routing was added. Keeps
        // heading for the SAME committed point, not a freshly-recomputed
        // closest one.
        step_toward(u, u.approach_target->x, u.approach_target->y, dt, world);
    }
}

// True once a unit has spent a whole window getting essentially NOWHERE while
// approaching a build/repair/drop-off target -- the trigger for the
// replan-then-escalate ladder at each approach's call site.
//
// NET displacement across the window, measured against an anchor, NOT per-tick
// displacement. The previous version asked "did the unit move less than 0.6px
// THIS tick?" and reset the timer the moment it moved more, which detects
// exactly one failure mode: a unit standing perfectly still. It is blind to the
// far more common one -- a unit **oscillating**. When step_toward's avoidance
// tiers can't find a way past a neighbour they fall through to the retreat
// tier, which moves the unit a real distance every tick; next tick the goal
// pulls it straight back. Per-tick displacement stays healthy forever while net
// progress is zero, so the timer never accumulated, the ladder never fired, and
// the unit jittered back and forth indefinitely. That is precisely the reported
// bug: a villager holding oil, two allies between it and the refinery, "kept
// going forever, jittering and moving back and forward, never making it".
//
// The window still has to tolerate a legitimate detour -- distance-to-TARGET
// can grow while routing around an obstacle, which is why this measures the
// unit's own displacement and not its distance to the goal (a distance-based
// version gave up on builders right as they were arriving). A villager runs
// 60px/s, so an unobstructed one covers ~150px per window; requiring a mere
// 24px of net travel is ~16% of nominal, far below anything a unit making real
// progress -- however winding its route -- would fail.
bool approach_stalled(Unit& u, double dt) {
    constexpr double kWindow = 2.5;        // seconds per progress sample
    constexpr double kMinProgress = 24.0;  // px of NET travel that counts as progress
    if (!u.approach_prev_pos) {
        u.approach_prev_pos = Vec2{u.common.x, u.common.y};
        u.approach_progress_check_t = 0.0;
        return false;
    }
    u.approach_progress_check_t += dt;
    if (u.approach_progress_check_t < kWindow) return false;
    double net = std::hypot(u.common.x - u.approach_prev_pos->x, u.common.y - u.approach_prev_pos->y);
    u.approach_prev_pos = Vec2{u.common.x, u.common.y};
    u.approach_progress_check_t = 0.0;
    return net < kMinProgress;
}

// Walks a unit toward its `rally` point (the AI's attack-move goal) along an
// A*-routed path, with the same replan-then-give-up ladder every other
// movement path in this file has.
//
// This branch used to be a single bare step_toward. That made rally the FIFTH
// movement path with no route and no watcher -- and the most consequential one
// missed, because rally is how every AI army crosses the map (offensive waves,
// scout sweeps, defensive recalls; see control_ai.cpp's rally assignments).
// Greedy steering walks straight at the goal, so any concave pocket of trees or
// buildings sitting on the straight line swallowed the whole force: it pressed
// into the back wall forever, because escaping a concavity means walking AWAY
// from the goal and purely local steering cannot discover that. The player's
// question was exactly right -- units should avoid a dead end in the first
// place, and they do now, because something finally plans the route.
//
// Deliberately keeps rally's own semantics: normal ally blocking (unlike a
// player MOVE order, which phases through allies), so a charging wave still
// spreads out rather than stacking into one column.
void advance_rally(Unit& u, double dt, World& world) {
    constexpr double kRallyRouteRange = 3.0 * TILE; // closer than this, steer directly
    constexpr double kArrive = 0.75 * TILE;
    Vec2 goal = *u.rally;

    double d = std::hypot(goal.x - u.common.x, goal.y - u.common.y);
    if (d <= kArrive) { // close enough; the wave re-targets from here
        u.rally.reset();
        u.rally_path_for.reset();
        u.rally_prev_pos.reset();
        u.rally_progress_t = 0.0;
        u.rally_strikes = 0;
        u.path.clear();
        u.path_i = 0;
        return;
    }

    // A new rally point (a fresh wave, a recall) invalidates the old route.
    bool same_goal = u.rally_path_for && std::hypot(u.rally_path_for->x - goal.x,
                                                    u.rally_path_for->y - goal.y) < 1.0;
    if (!same_goal) {
        u.rally_path_for = goal;
        u.rally_prev_pos.reset();
        u.rally_progress_t = 0.0;
        u.rally_strikes = 0;
        u.path.clear();
        u.path_i = 0;
    }

    if (u.path.empty() && d > kRallyRouteRange && world.consume_rally_astar_budget()) {
        // Aim at the nearest passable tile to the rally point: waves are
        // targeted at a building's centre or a ring position, either of which
        // can land on an impassable tile and fail astar's goal check outright.
        // (Gated on the per-step rally A* budget -- when a whole army re-paths
        // at once, units past the budget keep steering straight this step and
        // get their route next step, so no single step does hundreds of full
        // long-range searches. See World::consume_rally_astar_budget.)
        auto [ax, ay] = world.nearest_passable(goal.x, goal.y, /*is_air=*/false, u.common.is_ship);
        auto raw = astar(world.cols, world.rows, TILE, u.common.x, u.common.y, ax, ay,
                         make_passfn(world, u));
        for (auto& p : raw) u.path.push_back(Vec2{p.x, p.y});
        u.path_i = 0;
    }

    // NET progress over a window, not per-tick movement -- a unit bouncing off
    // step_toward's retreat tier moves every tick while going nowhere (see
    // approach_stalled for the full reasoning).
    u.rally_progress_t += dt;
    if (u.rally_progress_t >= 2.5) {
        double net = u.rally_prev_pos
                         ? std::hypot(u.common.x - u.rally_prev_pos->x, u.common.y - u.rally_prev_pos->y)
                         : 99.0;
        u.rally_prev_pos = Vec2{u.common.x, u.common.y};
        u.rally_progress_t = 0.0;
        if (net < 24.0) {
            if (++u.rally_strikes <= 2) {
                u.path.clear(); // two free replans against the current world
                u.path_i = 0;
            } else {
                // Genuinely can't get there. Drop the rally rather than grind
                // against the obstacle forever -- the AI re-issues waves on its
                // own tick, so this is a retry, not a permanent surrender, and
                // an idle unit is at least available for defense.
                u.rally.reset();
                u.rally_path_for.reset();
                u.rally_prev_pos.reset();
                u.rally_strikes = 0;
                u.path.clear();
                u.path_i = 0;
                return;
            }
        }
    }

    if (!u.path.empty()) {
        Vec2 wp = u.path[u.path_i];
        // Same waypoint tolerance as the other walkers: A* is unit-blind, so a
        // waypoint can sit inside another unit's exclusion radius and never be
        // reachable to the exact pixel.
        if (step_toward(u, wp.x, wp.y, dt, world) ||
            std::hypot(u.common.x - wp.x, u.common.y - wp.y) < kSeparation + 4.0) {
            if (++u.path_i >= u.path.size()) {
                u.path.clear();
                u.path_i = 0;
            }
        }
        return;
    }
    if (step_toward(u, goal.x, goal.y, dt, world)) u.rally.reset();
}

void update_gather(EntityRef self, Unit& u, double dt, World& world) {
    bool is_ai = world.control.teams[u.common.team].is_ai;

    if (u.carry > 0 && (u.carry >= u.max_carry || u.drop_target.valid())) {
        Building* dropoff = world.get_building(u.drop_target);
        // Re-pick a drop-off that died OR that isn't finished. nearest_dropoff
        // already filters incomplete buildings, so this is belt-and-braces --
        // but `drop_target` is a plain field that any code path can set (the
        // client's right-click picker, order queues, future callers), and THIS
        // is the one place a resource actually lands. Banking into an unbuilt
        // foundation was a real bug; the invariant is cheapest to guarantee at
        // the deposit site rather than in every producer of a drop_target.
        bool naval = (u.name == "fishing boat"); // unloads only at a dock (shipyard/base)
        if (!dropoff || !dropoff->common.alive || !dropoff->complete) {
            u.drop_target =
                world.nearest_dropoff(u.common.team, u.common.x, u.common.y, u.carry_type, kNullRef, naval);
            dropoff = world.get_building(u.drop_target);
        }
        if (!dropoff) return;
        // Delivery used to be a bare step_toward at the drop-off with NOTHING
        // watching it: no route, and no stall detection either. A full
        // villager with a building, a wall or a bay of terrain between it and
        // the drop-off simply pressed into the obstacle -- that is the
        // 55-minute "holding carry=10, path=0 need_path=0 stall=0" freeze
        // from the notes, and the last of the four movement paths that had no
        // route at all. It now uses exactly the same machinery as the build
        // approach: A*-routed perimeter approach, stall detection, and a
        // replan-then-escalate ladder.
        if (!(u.gather_path_for == u.drop_target)) {
            // New (or newly re-picked) drop-off: drop any route planned for
            // the previous one, and start this approach on a clean budget.
            u.gather_path_for = u.drop_target;
            u.path.clear();
            u.path_i = 0;
            u.approach_target.reset();
            u.approach_prev_pos.reset();
            u.approach_progress_check_t = 0.0;
            u.approach_replans = 0;
        }
        if (world.at_dropoff(*dropoff, u.common.x, u.common.y)) {
            world.add_resource(u.common.team, u.carry_type, u.carry);
            u.carry = 0;
            u.drop_target = kNullRef;
            u.gather_path_for = kNullRef;
            u.path.clear();
            u.path_i = 0;
            u.approach_target.reset();
            u.approach_prev_pos.reset();
            u.approach_progress_check_t = 0.0;
            u.approach_replans = 0;
        } else if (approach_stalled(u, dt)) {
            u.path.clear();
            u.path_i = 0;
            u.approach_target.reset();
            u.approach_prev_pos.reset();
            u.approach_progress_check_t = 0.0;
            if (u.approach_replans < 2) {
                ++u.approach_replans; // try a fresh route against the current world
            } else {
                // Two replans didn't help, so it's the DESTINATION that's the
                // problem, not the route. Ask for the next-best drop-off and
                // start over. If there genuinely isn't another one, keep this
                // target and reset the budget rather than clearing it -- a
                // villager that keeps trying is strictly better than one that
                // stands still holding a full load forever, which is the bug
                // this whole branch exists to kill.
                EntityRef other = world.nearest_dropoff(u.common.team, u.common.x, u.common.y,
                                                        u.carry_type, /*exclude=*/u.drop_target, naval);
                u.approach_replans = 0;
                if (other.valid()) {
                    u.drop_target = other;
                    u.gather_path_for = kNullRef; // forces a fresh plan next tick
                }
            }
        } else {
            advance_to_building(u, *dropoff, dt, world);
        }
        return;
    }

    bool target_alive = false;
    if (u.gather_target.kind == EntityKind::Resource) {
        Resource* r = world.get_resource(u.gather_target);
        target_alive = r && r->common.alive;
    } else if (u.gather_target.kind == EntityKind::Building) {
        Building* b = world.get_building(u.gather_target);
        target_alive = b && b->common.alive;
    }
    if (!u.gather_target.valid() || !target_alive) {
        if (is_ai) {
            bool want_fish = (u.name == "fishing boat");
            u.gather_target = world.nearest(u.common.x, u.common.y, 500, [&](EntityRef ref, EntityCommon& c) {
                if (c.kind != EntityKind::Resource || !c.alive) return false;
                Resource* r = world.get_resource(ref);
                return r && (r->name == "fish") == want_fish;
            });
        } else if (u.gather_rtype >= 0) {
            // The specific resource we were working just disappeared --
            // most commonly another villager finished it off first. Look
            // for the nearest live resource of the SAME type (set by
            // World::order_gather when this gather order was first given)
            // and keep working instead of going idle.
            int want_rtype = u.gather_rtype;
            u.gather_target = world.nearest(u.common.x, u.common.y, 500, [&](EntityRef ref, EntityCommon& c) {
                if (c.kind != EntityKind::Resource || !c.alive) return false;
                Resource* r = world.get_resource(ref);
                return r && r->res.rtype == want_rtype;
            });
        } else {
            u.gather_target = kNullRef;
        }
    }
    if (!u.gather_target.valid()) return;

    bool is_farm = (u.gather_target.kind == EntityKind::Building);
    Building* farm = is_farm ? world.get_building(u.gather_target) : nullptr;
    Resource* res = is_farm ? nullptr : world.get_resource(u.gather_target);
    if (is_farm) {
        if (farm->exhausted) { u.gather_target = kNullRef; return; }
        if (farm->occupied_by.valid() && !(farm->occupied_by == self)) {
            Unit* other = world.get(farm->occupied_by);
            if (other && other->common.alive) {
                // Only one villager works a given farm. Extras (e.g. several
                // villagers sent to the same farm at once) look for another
                // free farm nearby to work instead; if none exists, they
                // approach the farm they were sent to and idle beside it
                // rather than standing wherever the order was given. If the
                // occupant leaves/dies, occupied_by clears (building_behavior.cpp)
                // and an idling unit here picks the farm up on its own next tick.
                EntityRef target = u.gather_target;
                EntityRef alt = world.nearest(
                    u.common.x, u.common.y, 500, [&](EntityRef ref, EntityCommon& c) {
                        if (ref == target || c.kind != EntityKind::Building || !c.alive ||
                            c.team != u.common.team) {
                            return false;
                        }
                        Building* fb = world.get_building(ref);
                        return fb && fb->name == "farm" && fb->complete && !fb->exhausted &&
                               !fb->occupied_by.valid();
                    });
                if (alt.valid()) {
                    u.gather_target = alt;
                    return;
                }
                if (std::hypot(farm->common.x - u.common.x, farm->common.y - u.common.y) >
                    farm->build_radius) {
                    step_toward(u, farm->common.x, farm->common.y, dt, world);
                }
                return;
            }
        }
    }
    double tx = is_farm ? farm->common.x : res->common.x;
    double ty = is_farm ? farm->common.y : res->common.y;
    // A farm's actual worker walks all the way to its exact centre (farms
    // are non-solid, so nothing stops them) rather than stopping at
    // build_radius outside the footprint -- that outer radius is now only
    // used for OTHER villagers idling beside an occupied farm, just above.
    double reach = is_farm ? 4.0 : 20.0;
    double d = std::hypot(tx - u.common.x, ty - u.common.y);
    if (d > reach) {
        // Give up on a target that can't actually be reached (e.g. boxed in
        // by a dense cluster of other resources/buildings, or another
        // gatherer parked right on the approach line) after a window with
        // no real progress -- instead of endlessly pushing against
        // whatever's in the way, look for another same-type resource (or,
        // for a farm, another unoccupied farm) nearby and switch to that.
        // This IS the "this node is overcrowded, work somewhere else" rule:
        // in practice what stops a villager closing on a node is the ring
        // of other villagers already on it. The window is deliberately
        // patient (was 1.5s) -- a crowd around a node is transient, bodies
        // shuffle constantly as others fill up and leave for a drop-off, so
        // a short fuse made villagers abandon a perfectly good node they'd
        // have squeezed into a second later, then trek to a far one. Paired
        // with kGathererSeparation letting them pack tighter in the first
        // place, they now hold out for a slot instead of walking away from
        // the resource they're standing on.
        // Measured as distance-to-target shrinking across the whole
        // window, NOT raw per-tick displacement: a unit stuck weaving
        // against another nearby gatherer can easily move several px on
        // any given tick while making zero net progress over a couple of
        // seconds, which a per-tick "did it move at all" check can never
        // catch since each individual tick's movement is comfortably above
        // any such threshold. Same reasoning as move_goal's
        // progress_check_t/last_goal_dist (update_unit).
        u.gather_progress_check_t += dt;
        if (u.gather_progress_check_t >= 4.0) {
            bool progressed = u.gather_last_dist < 0.0 || d <= u.gather_last_dist - 5.0;
            u.gather_last_dist = d;
            u.gather_progress_check_t = 0.0;
            if (!progressed) {
                // Try a different ROUTE before giving up on the resource.
                // This branch used to jump straight to "pick another
                // resource", which is a large part of why villagers abandon
                // perfectly good nodes: usually the node was fine and the
                // route to it wasn't (a building went up across the path, or
                // the first plan went through a foundation that has since
                // turned solid). Same ladder the move order and the build
                // approach already use -- two free replans, then escalate.
                if (u.approach_replans < 2) {
                    ++u.approach_replans;
                    u.path.clear();
                    u.path_i = 0;
                    u.gather_path_for = kNullRef; // force a fresh plan...
                    u.gather_repath_t = 0.0;      // ...on the very next tick
                    u.gather_last_dist = -1.0;
                    return;
                }
                u.approach_replans = 0;
                EntityRef target = u.gather_target;
                EntityRef alt;
                if (is_farm) {
                    alt = world.nearest(u.common.x, u.common.y, 500, [&](EntityRef ref, EntityCommon& c) {
                        if (ref == target || c.kind != EntityKind::Building || !c.alive ||
                            c.team != u.common.team) {
                            return false;
                        }
                        Building* fb = world.get_building(ref);
                        return fb && fb->name == "farm" && fb->complete && !fb->exhausted &&
                               !fb->occupied_by.valid();
                    });
                } else {
                    int want_rtype = res->res.rtype;
                    alt = world.nearest(u.common.x, u.common.y, 500, [&](EntityRef ref, EntityCommon& c) {
                        if (ref == target || c.kind != EntityKind::Resource || !c.alive) return false;
                        Resource* nr = world.get_resource(ref);
                        return nr && nr->res.rtype == want_rtype;
                    });
                }
                u.gather_target = alt;
                u.gather_last_dist = -1.0;
                // If nothing else of the same type is in range either, go
                // properly idle instead of pushing against this same
                // target forever: clearing gather_rtype too stops the
                // "target disappeared, re-seek the nearest same-type
                // resource" fallback at the top of this function (it
                // doesn't exclude the very resource just given up on) from
                // immediately re-selecting THIS one again next tick and
                // walking straight back into the same stall -- give-up,
                // reacquire, give-up again, forever.
                if (!alt.valid()) u.gather_rtype = -1;
                return;
            }
        }
        // A*-ROUTED APPROACH. Walking to a resource used to be a bare
        // step_toward with no route at all: fine across open ground, but
        // with no answer to terrain. A villager whose vein sits around the
        // arm of a forest, or anywhere inside a concave pocket, pressed
        // into the obstacle and burned the stall window above -- then gave
        // up a perfectly good resource and trekked somewhere else, or
        // wedged again on the way there. Villagers do nearly all the
        // walking in a match, so this was most of the "units get stuck"
        // showing up in play.
        //
        // Only worth routing beyond a few tiles: closer in, the direct
        // step_toward's one-axis slide handles the last stretch (and the
        // target's own tile is frequently blocked by the resource itself,
        // which A* can't enter). Under that range this is exactly the
        // previous behaviour.
        constexpr double kGatherPathRange = 3.0 * TILE;
        if (!(u.gather_path_for == u.gather_target)) {
            // Reassigned (AI rebalance, node depleted, new player order):
            // the old route leads to the wrong place entirely.
            u.gather_path_for = u.gather_target;
            u.path.clear();
            u.path_i = 0;
            u.gather_repath_t = 0.0;
        }
        if (u.gather_repath_t > 0.0) u.gather_repath_t -= dt;
        if (u.path.empty() && d > kGatherPathRange && u.gather_repath_t <= 0.0) {
            // Cooldown regardless of outcome: a target that can't be routed
            // to would otherwise run a full-budget search every tick for
            // every villager assigned to it.
            u.gather_repath_t = 1.0;
            auto passfn = make_passfn(world, u);
            // Aim at a tile BESIDE the target, not the target itself: a
            // resource makes its own tile impassable (resource_tiles_ in
            // World::passable), so routing to it would fail on the goal
            // check every time and hand back nothing. Gathering happens
            // from alongside anyway -- same reason advance_to_building
            // routes to a building's perimeter rather than its centre.
            auto [ax, ay] = world.nearest_passable(tx, ty, /*is_air=*/false, u.common.is_ship);
            auto raw = astar(world.cols, world.rows, TILE, u.common.x, u.common.y, ax, ay, passfn);
            for (auto& p : raw) u.path.push_back(Vec2{p.x, p.y});
            u.path_i = 0;
        }
        if (!u.path.empty()) {
            Vec2 wp = u.path[u.path_i];
            // Same waypoint tolerance as the move-order walker: A* is blind
            // to other units, so a waypoint can land inside another
            // gatherer's exclusion radius and never be reachable to the
            // exact pixel.
            if (step_toward(u, wp.x, wp.y, dt, world) ||
                std::hypot(u.common.x - wp.x, u.common.y - wp.y) < kSeparation + 4.0) {
                if (++u.path_i >= u.path.size()) {
                    u.path.clear();
                    u.path_i = 0;
                }
            }
            return;
        }
        step_toward(u, tx, ty, dt, world);
        return;
    }
    // Arrived and working: clear the approach state so the next approach
    // (or the delivery run once this load fills) starts on a clean budget.
    u.gather_progress_check_t = 0.0;
    u.gather_last_dist = -1.0;
    u.path.clear();
    u.path_i = 0;
    u.gather_path_for = kNullRef;
    u.approach_replans = 0;

    if (is_farm) farm->occupied_by = self;
    u.working = true;
    int rtype = is_farm ? 0 /*FOOD*/ : res->res.rtype;

    u.swing_t += dt;
    if (u.swing_t >= 0.6) {
        u.swing_t = 0.0;
        std::string key;
        std::string res_name = is_farm ? "" : res->name;
        if (u.name == "fishing boat" || res_name == "fish") key = "fish";
        else if (rtype == 1) key = "chop";
        else if (rtype == 2 || rtype == 3) key = "mine";
        else key = "forage";
        world.events.push({EventType::Sound, key, u.common.x, u.common.y, 250, kNullRef, ""});
    }
    u.swing_down = u.swing_t < 0.3;

    if (u.reload_timer <= 0) {
        const Team& td = world.control.teams[u.common.team];
        double gm = world.bonuses.gather_multiplier(rtype, td.civ, td.leader, td.tech);
        u.reload_timer = u.reload * gm;
        double& amount = is_farm ? farm->amount : res->res.amount;
        // Chat-bar "blitz" cheat: take the whole node in one hit and
        // deliver it straight to the stockpile -- no carry cap, no walk
        // back to a dropoff.
        double take = td.blitz ? amount : std::min(1.0, amount);
        amount -= take;
        // Mirror the remaining amount into common.hp so the client's selected-
        // node amount bar stays current. This used to be done for EVERY resource
        // every tick in update_resource(); now it only happens on the node that
        // actually changed (see the removed per-tick loop in World::update).
        if (!is_farm) res->common.hp = res->res.amount;
        if (td.blitz) {
            world.add_resource(u.common.team, rtype, take);
        } else {
            u.carry += take;
            u.carry_type = rtype;
        }
        if (amount <= 0) {
            if (is_farm) {
                // Auto-replant (Team::replant, toggled at the market): re-sow
                // this farm in place so the farmer keeps working it instead of
                // going idle. Re-sowing costs a flat 40 WOOD (never food -- seed
                // is wood, same price as the initial farm build) -- paid straight
                // from the stockpile. Only if affordable; otherwise the farm just
                // exhausts as normal.
                constexpr double kFarmResowWood = 40.0;
                double& team_wood = world.control.teams[u.common.team].res["wood"];
                if (td.replant && team_wood >= kFarmResowWood) {
                    team_wood -= kFarmResowWood;
                    farm->amount = farm->max_farm_food;
                    farm->exhausted = false;
                    world.events.push(
                        {EventType::Sound, "build", farm->common.x, farm->common.y, 400, kNullRef, ""});
                } else {
                    farm->amount = 0;
                    farm->exhausted = true;
                    u.gather_target = kNullRef;
                    world.events.push({EventType::Sound, "farm_exhausted", farm->common.x, farm->common.y,
                                       400, kNullRef, ""});
                }
            } else {
                res->common.alive = false;
                int want_rtype = rtype;
                // Pick the next node of the same resource type to move on to.
                EntityRef next = world.nearest(u.common.x, u.common.y, 240, [&](EntityRef ref, EntityCommon& c) {
                    if (c.kind != EntityKind::Resource || !c.alive) return false;
                    Resource* nr = world.get_resource(ref);
                    return nr && nr->res.rtype == want_rtype;
                });
                u.gather_target = next;
                // But if the villager is carrying a partial load, DELIVER it to a
                // drop-off first (set drop_target so the top-of-update_gather
                // delivery gate fires) so the resources gathered off the depleted
                // node aren't left riding around until the next node fills the
                // load. It resumes gathering `next` after banking.
                if (u.carry > 0) {
                    bool naval = (u.name == "fishing boat");
                    u.drop_target =
                        world.nearest_dropoff(u.common.team, u.common.x, u.common.y, u.carry_type, kNullRef, naval);
                }
            }
        }
    }
}

// The muzzle/fire sound a unit makes when it shoots -- keyed by unit type so
// artillery boom / heavy-cannon / ballistic launch / tank cannon etc. all come
// out right. Shared by the normal-target fire path AND the attack-ground fire
// path so both play the SAME sound.
static std::string fire_sound(const Unit& u) {
    if (u.name == "frigate") return "cannon_fire";
    if (u.is_ballistic) return "plane_spawn"; // missile launches with a plane-takeoff roar
    if (u.name == "heavy artillery") return "heavy_cannon"; // Soviet: deeper + louder boom
    if (u.name == "artillery" || u.name == "artillery1") return "cannon_fire"; // field cannon boom
    if (u.bullet_sprite == "spr_bullet_large" || u.unit_type == "tank") return "cannon";
    if (u.melee) return "sword";
    if (u.name == "royal marine" || u.name == "elite royal marine") return "gunshot_deep";
    return "gunshot";
}

void update_combat(EntityRef self, Unit& u, double dt, World& world) {
    // Unarmed units (transport ship, base aircraft carrier) never fight -- no
    // auto-acquiring, and no chasing an enemy they can't scratch. (Gatherers
    // are dispatched to update_gather before this is ever called.) They must
    // still follow an AI attack-move RALLY though -- that's processed here in
    // update_combat, not in update_unit's move_goal path -- e.g. a transport
    // sailing to the invasion beach, or a carrier ordered to reposition.
    if (u.attack <= 0.0) {
        u.attack_target = kNullRef;
        u.forced = false;
        if (u.rally) advance_rally(u, dt, world);
        return;
    }
    // A packed (mobile) or mid-transition ballistic launcher cannot fire --
    // it must be deployed first (see update_unit's pack gate).
    if (u.is_ballistic && (u.packed || u.pack_t > 0.0)) return;
    EntityRef tgt = u.attack_target;
    EntityCommon* tgt_c = world.common(tgt);

    if (u.is_aa && tgt_c && !tgt_c->is_air) {
        tgt = kNullRef;
        u.attack_target = kNullRef;
        u.forced = false;
        tgt_c = nullptr;
    }
    // Melee units can't strike aircraft -- there's no reach into the air, so
    // drop an air target the same way an AA gun drops a ground one (covers a
    // force-attack order onto a plane too).
    if (u.melee && tgt_c && tgt_c->is_air) {
        tgt = kNullRef;
        u.attack_target = kNullRef;
        u.forced = false;
        tgt_c = nullptr;
    }
    bool tgt_alive = tgt_c && tgt_c->alive;
    bool keep_forced = u.forced && tgt.valid() && tgt_alive;
    // Auto-acquire out to whichever is greater, sight OR attack range: some units
    // (rifleman range 4/sight 3, artillery range 10/sight 6) can shoot FURTHER
    // than they see, and should still engage any enemy that's within their
    // weapon's reach -- "shoot enemy units once they are within range".
    double acq = std::max(u.sight_px, u.range_px);
    if (!keep_forced) {
        if (!tgt.valid() || !tgt_alive || dist_to(u, *tgt_c) > acq * 1.6) {
            // An explicitly-ordered attack whose target died before we ever
            // got there (as opposed to just wandering out of sight) --
            // keep heading toward where it was instead of stopping dead in
            // place. Reuses the attack-move ("rally") mechanism: the
            // auto-acquire search just below still runs unconditionally
            // every tick, so the unit peels off onto anything else sighted
            // along the way exactly like a normal attack-move.
            if (u.forced && tgt.valid() && tgt_c && !tgt_alive) {
                u.rally = Vec2{tgt_c->x, tgt_c->y};
            }
            u.forced = false;
            if (u.is_ballistic) {
                // Non-aggressive: the launcher never picks its own target. It
                // fires only at what the player explicitly ordered (kept above
                // via `forced` while that target lives) or an attack-ground
                // point; once its target dies it goes idle, awaiting new orders.
                tgt = kNullRef;
            } else if (u.is_gatherer) {
                tgt = kNullRef;
            } else if (u.is_aa) {
                tgt = world.nearest(u.common.x, u.common.y, acq, [&](EntityRef, EntityCommon& c) {
                    return c.alive && c.team >= 0 && !world.control.allied(c.team, u.common.team) && c.is_air;
                });
            } else {
                bool melee = u.melee;
                tgt = world.nearest(u.common.x, u.common.y, acq, [&](EntityRef, EntityCommon& c) {
                    return c.alive && c.team >= 0 && !world.control.allied(c.team, u.common.team) &&
                           (c.kind == EntityKind::Unit || c.kind == EntityKind::Building) &&
                           !(melee && c.is_air); // melee can't reach aircraft
                });
            }
            u.attack_target = tgt;
            tgt_c = world.common(tgt);
        }
    }
    if (!tgt.valid()) {
        // No target: if the clip's only partially spent and not already
        // reloading, start the reload countdown now rather than leaving it
        // stuck at a partial count forever (it only ever refills when
        // clip_reload_timer actually counts down to 0 -- see update_unit).
        // Direct port of obj_unit/Step.gml's own idle-regen rule.
        if (u.clip_max > 0 && u.clip_reload_timer <= 0 && u.clip_ammo < u.clip_max) {
            u.clip_reload_timer = u.clip_reload;
        }
        if (u.rally) {
            advance_rally(u, dt, world);
            return;
        }
        if (!u.hold) {
            u.hold = Vec2{u.common.x, u.common.y};
        } else if (std::hypot(u.hold->x - u.common.x, u.hold->y - u.common.y) > 16) {
            step_toward(u, u.hold->x, u.hold->y, dt, world);
        }
        return;
    }

    double d = dist_to(u, *tgt_c);
    double foot = (tgt.kind == EntityKind::Building) ? world.foot_px_of(tgt) * 0.5 : 0.0;
    double reach = (u.melee ? std::max(u.range_px, 30.0) : u.range_px) + foot;
    // A deployed (unpacked) ballistic launcher is a FIXED emplacement -- it never
    // moves, so it doesn't chase an out-of-range target (right-clicking a distant
    // unit must not make it crawl over) nor back away from a too-close one. It
    // simply can't engage a target outside its arc; the player packs it to move.
    if (d > reach) {
        if (u.is_ballistic) return;
        // Enver Pasha: cavalry loose a musket round at the enemy they are
        // charging every 10s, without breaking stride (musket_t ticks in
        // update_unit). A ranged plink while closing to the melee they normally
        // need to reach first.
        if (u.musket_t <= 0.0 && CAVALRY.count(u.name) &&
            world.bonuses.leader_name(world.control.teams[u.common.team].civ,
                                      world.control.teams[u.common.team].leader) == "Enver Pasha") {
            world.hurt(tgt, std::max(u.attack - world.armor_of(tgt), 1.0));
            world.events.push({EventType::Sound, "gunshot", u.common.x, u.common.y, 0, kNullRef, ""});
            world.events.push({EventType::Effect, "spr_spark", u.common.x, u.common.y, 0, kNullRef, ""});
            u.musket_t = 10.0;
        }
        step_toward(u, tgt_c->x, tgt_c->y, dt, world);
        return;
    }
    if (u.min_range_px > 0 && d < u.min_range_px) {
        // Point-blank: siege (artillery) can't fire inside its minimum arc --
        // back away to open the range instead of shooting.
        if (u.is_ballistic) return;
        step_toward(u, 2 * u.common.x - tgt_c->x, 2 * u.common.y - tgt_c->y, dt, world);
        return;
    }

    u.facing = (tgt_c->x >= u.common.x) ? 1 : -1;
    // Battleship/yamato have a clip (catalog "clip_size"/"clip_reload"):
    // clip_ammo shots fire back-to-back on the ordinary reload cadence,
    // then firing pauses entirely for clip_reload seconds once it hits 0
    // (clip_reload_timer, already ticked down/refilled generically every
    // tick up in update_unit) -- direct port of obj_unit/Step.gml's
    // "reload_timer=0 and clip_reload_timer=0" firing gate. Units with no
    // clip at all (clip_max==0, i.e. everything except the handful the
    // catalog gives one to) are never gated by this.
    bool clip_gated = u.clip_max > 0;
    if (u.reload_timer <= 0 && u.attack > 0 && (!clip_gated || u.clip_ammo > 0)) {
        u.reload_timer = u.reload;
        // GML decrements clip_ammo (and starts the reload pause once it
        // bottoms out) BEFORE computing this shot's origin_x/origin_y, so
        // ship_shot_origin below sees the POST-decrement count -- that's
        // what makes a battleship's/yamato's burst visibly alternate
        // between turret groups partway through the clip.
        if (clip_gated) {
            u.clip_ammo--;
            if (u.clip_ammo <= 0) {
                // A unit with clip_size but no (or zero) clip_reload stat
                // would otherwise set clip_reload_timer to 0 here -- which
                // never counts down (update_unit's decrement is gated on
                // "> 0"), so clip_ammo would stay stuck at 0 forever and
                // the unit would silently never fire again. Treat that as
                // an instant reload instead of a permanent jam.
                if (u.clip_reload > 0) u.clip_reload_timer = u.clip_reload;
                else u.clip_ammo = u.clip_max;
            }
        }
        if (u.melee) {
            double dmg = std::max(u.attack - world.armor_of(tgt), 1.0);
            // Chiang Kai-Shek: infantry +2 damage vs tanks (melee path; the
            // ranged path is handled in World::spawn_projectile).
            if (Unit* tu = world.get(tgt); tu && TANK.count(tu->name) && INFANTRY.count(u.name) &&
                world.bonuses.leader_name(world.control.teams[u.common.team].civ,
                                          world.control.teams[u.common.team].leader) == "Chiang Kai-Shek")
                dmg += 2.0;
            world.hurt(tgt, dmg);
            world.events.push({EventType::Effect, "spr_slice", tgt_c->x, tgt_c->y, 0, kNullRef, ""});
        } else {
            // GML always creates obj_spark at the exact same (origin_x,
            // origin_y) as the shot itself, no extra offset -- the old
            // flat "-8" here predates ship_shot_origin (back when every
            // unit fired from its own bare x/y) and, left in place, was
            // stacking on TOP of a ship's own already-correct muzzle
            // offset, pushing its flash well above the actual gun.
            Vec2 origin = ship_shot_origin(u, world.rng);
            world.spawn_projectile(u, tgt, origin);
            world.events.push({EventType::Effect, "spr_spark", origin.x, origin.y, 0, kNullRef, ""});
            // (The ballistic missile's launch exhaust -- red-orange particles +
            // smoke while it sits on the pad -- is emitted by the projectile
            // itself during its launch hold, see update_projectile.)
            // Direct port of obj_unit/Step.gml's muzzle-effect block:
            // battleship/yamato/destroyer/torpedo boat always get a smoke
            // poof at the gun; frigate only when firing at a target below
            // it (matches its origin_y offset, which only makes visual
            // sense pointing that way -- see ship_shot_origin). Separately,
            // obj_missile/Step.gml has every one of these shots trail 3
            // drifting/fading smoke puffs from its muzzle (ClientSmoke on
            // the client), and battleship/yamato specifically also kick up
            // a small fire_cloud puff right at the muzzle.
            static const std::set<std::string> kPoofUnits = {"battleship", "yamato", "destroyer",
                                                             "torpedo boat"};
            if (kPoofUnits.count(u.name) || (u.name == "frigate" && tgt_c->y >= u.common.y)) {
                world.events.push({EventType::Effect, "spr_poof", origin.x, origin.y, 0, kNullRef, ""});
            }
            static const std::set<std::string> kMissileSmokeUnits = {
                "battleship", "yamato", "destroyer", "frigate", "torpedo boat"};
            if (kMissileSmokeUnits.count(u.name)) {
                for (int i = 0; i < 3; ++i) {
                    world.events.push({EventType::Effect, "spr_smoke", origin.x, origin.y, 0, kNullRef, ""});
                }
                if (u.name == "battleship" || u.name == "yamato") {
                    world.events.push(
                        {EventType::Effect, "spr_fire_cloud", origin.x, origin.y, 0, kNullRef, ""});
                }
            }
        }
        // Direct port of Audio.attack_sound's dispatch (game/audio.py) --
        // resolved here (not on the client) since it needs Unit fields
        // that may no longer be valid by the time a dead entity's events
        // are drained. obj_unit/Step.gml's own cascade: frigate always
        // gets the distinct cannon_fire clip; a large bullet_sprite (tanks,
        // battleship/yamato) gets "cannon"; everything else (including
        // destroyer/torpedo boat/artillery, which fire an unmarked
        // "spr_bullet") falls through to a plain gunshot.
        world.events.push({EventType::Sound, fire_sound(u), u.common.x, u.common.y, 0, self, ""});
    }
}

// True once a unit has no order of any kind left to finish -- the signal
// advance_order_queue below waits for before popping the next shift-queued
// step. Move/build both naturally reach this on their own (move_goal/
// build_target clear themselves on arrival/completion elsewhere in this
// file); gather/attack don't (see active_queue_watch's comment in unit.h),
// so advance_order_queue force-clears them itself once their watched
// target is gone.
bool unit_idle_for_queue(const Unit& u) {
    return !u.move_goal && !u.attack_target.valid() && !u.gather_target.valid() &&
           !u.build_target.valid() && !u.repair_target.valid() && !u.drop_target.valid() &&
           !u.load_target.valid() && !u.forced && !u.rally && !u.attack_ground;
}

// Pops and issues Unit::order_queue's next step once the current one is
// done. For Gather/Attack, "done" isn't naturally detectable (see above),
// so this also watches active_queue_watch and force-clears the current
// order the instant that specific entity dies/depletes -- pre-empting
// update_gather's same-type-resource re-seek and update_combat's forced-
// attack rally-toward-last-position fallback, both of which would
// otherwise keep the unit busy indefinitely instead of ever going idle.
void advance_order_queue(EntityRef self, Unit& u, World& world) {
    if (u.order_queue.empty() && !u.active_queue_watch.valid()) {
        // Nothing left pending -- once the unit is ALSO genuinely idle
        // (the current order, if any, has actually finished), the queue
        // system is fully done with it: clear queue_active so the client
        // stops drawing a marker for what's now just an ordinary order.
        // Checked here rather than the instant the last step is popped,
        // since that step is still in flight for however many ticks it
        // takes to actually finish.
        if (unit_idle_for_queue(u)) u.queue_active = false;
        return;
    }
    if (u.active_queue_watch.valid()) {
        EntityCommon* wc = world.common(u.active_queue_watch);
        if (!wc || !wc->alive) {
            u.active_queue_watch = kNullRef;
            u.attack_target = kNullRef;
            u.gather_target = kNullRef;
            u.gather_rtype = -1;
            u.forced = false;
            u.rally.reset();
        }
    }
    if (u.order_queue.empty() || !unit_idle_for_queue(u)) return;
    while (!u.order_queue.empty()) {
        QueuedOrder next = u.order_queue.front();
        u.order_queue.erase(u.order_queue.begin());
        switch (next.kind) {
        case QueuedOrderKind::Move:
            world.order_move(self, next.x, next.y, /*from_queue=*/true);
            u.queue_active = true;
            return;
        case QueuedOrderKind::Gather: {
            EntityCommon* tc = world.common(next.target);
            if (!tc || !tc->alive) continue; // gone before its turn came up -- skip it
            world.order_gather(self, next.target, /*from_queue=*/true);
            u.active_queue_watch = next.target;
            u.queue_active = true;
            return;
        }
        case QueuedOrderKind::Attack: {
            EntityCommon* tc = world.common(next.target);
            if (!tc || !tc->alive) continue;
            world.order_attack(self, next.target, /*from_queue=*/true);
            u.active_queue_watch = next.target;
            u.queue_active = true;
            return;
        }
        case QueuedOrderKind::Build: {
            Building* b = world.get_building(next.target);
            if (!b || !b->common.alive || b->complete) continue;
            u.build_target = next.target;
            u.gather_target = kNullRef;
            u.gather_rtype = -1;
            u.move_goal.reset();
            u.path.clear();
            u.approach_prev_pos.reset();
            u.approach_progress_check_t = 0.0;
            u.approach_target.reset();
            u.queue_active = true;
            return;
        }
        }
    }
}

} // namespace

void update_unit(EntityRef self, Unit& u, double dt, World& world) {
    // Garrisoned inside a transport ship: frozen and hidden (see game_client
    // draw skip + World fog/collision skips) but still alive, so it keeps
    // counting toward the pop cap. Rides along at the ship's position; if the
    // ship has sunk it drowns with its cargo.
    if (u.carrier.valid()) {
        Unit* ship = world.get(u.carrier);
        if (!ship || !ship->common.alive) {
            u.carrier = kNullRef;
            u.common.alive = false;
            u.deleted = true; // went down with the ship -- clean death, no boom
            return;
        }
        u.common.x = ship->common.x;
        u.common.y = ship->common.y;
        return;
    }

    // Campaign "dormant" trigger: frozen until the player trips its WakeTrigger
    // (world.cpp), which clears this flag and sets `rally` to make the group
    // charge. Stays alive/solid but takes no orders and neither moves nor fights,
    // exactly like the garrisoned case above.
    if (u.dormant) {
        u.moving = false;
        u.working = false;
        return;
    }

    advance_order_queue(self, u, world);

    u.moving = false;
    u.working = false;
    resolve_overlap(u, world);
    if (u.reload_timer > 0) u.reload_timer -= dt;
    if (u.musket_t > 0) u.musket_t -= dt;
    if (u.highlight > 0) u.highlight -= dt;
    if (u.hit_timer > 0) u.hit_timer -= dt;
    if (u.common.dmg_flash > 0) u.common.dmg_flash -= dt;
    if (!u.is_bomber && u.clip_reload_timer > 0) {
        u.clip_reload_timer -= dt;
        if (u.clip_reload_timer <= 0) u.clip_ammo = u.clip_max;
    }

    // Ballistic missile pack/unpack gate. A mid-transition launcher is frozen
    // (neither moves nor fires); a deployed (unpacked) one cannot move, so any
    // movement order is dropped and only combat/attack-ground below runs; a
    // packed one moves normally but update_combat refuses to fire.
    if (u.is_ballistic) {
        if (u.pack_t > 0.0) {
            u.pack_t -= dt;
            if (u.pack_t <= 0.0) { u.pack_t = 0.0; u.packed = u.pack_target; }
            u.moving = false;
            return;
        }
        if (u.packed) {
            // Non-aggressive launcher, but an EXPLICIT order to strike auto-
            // DEPLOYS it: right-clicking an enemy (kept alive via `forced`) or an
            // attack-ground point on a packed launcher starts the unpack now, so
            // the player doesn't have to deploy it by hand first -- once deployed
            // it fires at that target (update_combat). Stays packed/mobile until
            // then; a plain move order does NOT deploy it.
            EntityCommon* atc = world.common(u.attack_target);
            bool ordered = (u.forced && atc && atc->alive) || u.attack_ground.has_value();
            if (ordered) { u.pack_target = false; u.pack_t = 5.0; }
        } else {
            u.move_goal.reset();
            u.rally.reset();
            u.path.clear();
            u.path_i = 0;
            // The deployed launcher doesn't rotate -- it just faces left or right
            // toward whatever it's aiming at (its ordered target, else an
            // attack-ground point), so the client mirrors the sprite. Holds its
            // last facing when idle.
            std::optional<Vec2> aim_at;
            if (EntityCommon* tc = world.common(u.attack_target); tc && tc->alive)
                aim_at = Vec2{tc->x, tc->y};
            else if (u.attack_ground)
                aim_at = *u.attack_ground;
            if (aim_at) {
                if (aim_at->x > u.common.x + 1) u.facing = 1;
                else if (aim_at->x < u.common.x - 1) u.facing = -1;
            }
        }
    }

    if (!u.common.is_air && u.move_goal) {
        double moved = u.prev_pos ? std::hypot(u.common.x - u.prev_pos->x, u.common.y - u.prev_pos->y)
                                   : 99.0;
        u.prev_pos = Vec2{u.common.x, u.common.y};
        if (moved < 0.6) {
            u.stuck_t += dt;
            if (u.stuck_t > 1.5) {
                u.stuck_t = 0.0;
                u.need_path = true;
                u.path.clear();
                u.path_i = 0;
            }
        } else {
            u.stuck_t = 0.0;
        }

        // Give up the goal entirely if it hasn't gotten meaningfully closer
        // in ~1.5s, even though the unit IS moving (so the near-zero-
        // movement check above never fires). This is the case order_move's
        // nearest_passable redirect can't catch up front: a unit sliding
        // along a wall around a destination that only became blocked after
        // the order was issued, or several units all ordered to the exact
        // same point where only some of them can actually fit -- without
        // this, re-pathing toward the same unreachable/occupied point just
        // fails the same way again, forever. Measured along the remaining
        // path, not raw distance-to-goal -- see remaining_path_distance's
        // comment for why straight-line distance wrongly aborts legitimate
        // detours.
        //
        // The first stalled window just forces a fresh repath rather than
        // giving up immediately (see stall_strikes) -- a unit that got
        // knocked backward for a tick or two by an oncoming unit crossing
        // its path (blocked_by_unit) or an overlap correction
        // (resolve_overlap) isn't actually stuck, it just needs a moment
        // to recover, and used to have its whole order cancelled by this
        // check before it got the chance.
        u.progress_check_t += dt;
        if (u.progress_check_t >= 1.5) {
            double dist_now = remaining_path_distance(u);
            bool progressed = u.last_goal_dist < 0.0 || dist_now <= u.last_goal_dist - 8.0;
            if (progressed) {
                u.stall_strikes = 0;
            } else if (++u.stall_strikes <= 2) {
                // Two free repaths (~3s total) before giving up for good --
                // enough for two units contesting the same spot to jostle
                // past each other (see resolve_overlap) even if the first
                // repath alone doesn't immediately clear it.
                u.need_path = true;
                u.path.clear();
                u.path_i = 0;
            } else {
                u.move_goal.reset();
                u.path.clear();
                u.path_i = 0;
                u.stall_strikes = 0;
            }
            u.last_goal_dist = dist_now;
            u.progress_check_t = 0.0;
        }
    } else {
        u.stuck_t = 0.0;
        u.progress_check_t = 0.0;
        u.last_goal_dist = -1.0;
        u.stall_strikes = 0;
    }

    // 0a) board a transport ship: walk to the shore point nearest the ship,
    // then garrison inside once within reach. Takes priority over other
    // orders while a load is pending.
    if (u.load_target.valid()) {
        Unit* ship = world.get(u.load_target);
        bool ok = ship && ship->common.alive && ship->common.team == u.common.team &&
                  ship->transport_cap > 0 && !u.common.is_air && !u.common.is_ship;
        if (ok) {
            double used = 0.0;
            for (auto c : ship->cargo)
                if (Unit* cu = world.get(c)) used += transport_cost(cu->name);
            if (used + transport_cost(u.name) > ship->transport_cap) ok = false; // no room aboard
        }
        if (!ok) {
            u.load_target = kNullRef;
            u.move_goal.reset();
        } else {
            double d = std::hypot(ship->common.x - u.common.x, ship->common.y - u.common.y);
            if (d <= 2.5 * TILE) {
                ship->cargo.push_back(self);
                u.carrier = u.load_target;
                u.load_target = kNullRef;
                u.move_goal.reset();
                u.path.clear();
                u.path_i = 0;
                u.attack_target = kNullRef;
                u.gather_target = kNullRef;
                u.build_target = kNullRef;
                u.repair_target = kNullRef;
                return;
            }
            // Keep heading to the closest reachable land next to the ship.
            // Only (re)aim when idle so the path isn't thrashed every frame.
            if (!u.move_goal) {
                auto [tx, ty] = world.nearest_passable(ship->common.x, ship->common.y,
                                                       u.common.is_air, u.common.is_ship);
                u.move_goal = Vec2{tx, ty};
                u.need_path = true;
                u.path.clear();
                u.path_i = 0;
            }
        }
    }

    // 0) construct a foundation
    if (u.build_target.valid()) {
        Building* b = world.get_building(u.build_target);
        if (!b || !b->common.alive || b->complete) {
            // Wall-build chaining: a villager that just FINISHED a wall segment
            // auto-continues to the nearest connected, still-unfinished segment
            // of the same wall (within ~1 tile, incl. diagonals), so one build
            // order -- or a click-drag line -- raises the whole continuous wall
            // without ordering each segment. Only on a genuine completion (b
            // exists and is complete), not when the foundation was destroyed
            // mid-build (its position is gone).
            // A finished FARM: the villager that just built it seamlessly
            // becomes its farmer (AoE-style -- one order builds then works it).
            if (b && b->complete && b->name == "farm") {
                u.gather_target = u.build_target; // the farm we just finished
                u.gather_rtype = 0;               // farms are always food
                u.build_target = kNullRef;
                u.working = false;
                u.path.clear();
                u.approach_prev_pos.reset();
                u.approach_target.reset();
                u.approach_progress_check_t = 0.0;
                return;
            }
            // A finished HOUSE or REFINERY: the builder walks to the nearest
            // resource it can DROP OFF at that building and starts gathering --
            // a house is a food+wood dropoff (go cut wood / forage), a refinery
            // an oil+iron dropoff (go mine). Keeps the villager productive
            // instead of idling by the building it just raised.
            if (b && b->complete && (b->name == "house" || b->name == "refinery")) {
                u.build_target = kNullRef;
                u.working = false;
                u.path.clear();
                u.approach_prev_pos.reset();
                u.approach_target.reset();
                u.approach_progress_check_t = 0.0;
                // If the player SHIFT-QUEUED more orders (e.g. a whole row of
                // houses), let those run FIRST -- the explicit queue takes
                // priority over the auto-gather convenience below. Leaving the
                // unit idle here lets advance_order_queue pop the next queued
                // house instead of the villager wandering off to a resource.
                if (u.order_queue.empty()) {
                    bool wood_food = (b->name == "house");
                    EntityRef nr =
                        world.nearest(u.common.x, u.common.y, 400, [&](EntityRef ref, EntityCommon& c) {
                            if (c.kind != EntityKind::Resource || !c.alive) return false;
                            Resource* r = world.get_resource(ref);
                            if (!r) return false;
                            int rt = r->res.rtype;
                            return wood_food ? (rt == 0 || rt == 1) : (rt == 2 || rt == 3);
                        });
                    if (nr.valid()) {
                        u.gather_target = nr;
                        u.gather_rtype = -1; // resolved from the resource on arrival
                    }
                }
                return;
            }
            EntityRef next = kNullRef;
            if (b && b->complete && (b->name == "palisade" || b->name == "iron wall")) {
                // Reach well beyond one tile so a lone builder that started mid-
                // wall still finishes BOTH directions instead of orphaning the
                // far end -- it always heads to the nearest remaining segment,
                // so it builds outward and keeps going until the drawn wall is
                // done. (Bounded, not map-wide, so it won't wander to an
                // unrelated wall across the map.)
                next = world.next_wall_segment(u.common.team, b->name, b->common.x, b->common.y,
                                               /*radius=*/16.0 * TILE, u.build_target);
            }
            u.build_target = next;
            if (next.valid()) {
                u.path.clear();
                u.approach_prev_pos.reset();
                u.approach_target.reset();
                u.approach_progress_check_t = 0.0;
                return; // head to the next segment next tick
            }
        } else {
            if (!world.at_dropoff(*b, u.common.x, u.common.y)) {
                if (approach_stalled(u, dt)) {
                    // RE-PLAN before giving up. advance_to_building routes
                    // once and then commits, but the world moves underneath
                    // that route -- most sharply when a foundation the path
                    // was drawn through gets started and turns solid, which
                    // is exactly the case the player hit (two house
                    // foundations; the villager routed through the first,
                    // the other villager started it, and the first was
                    // wedged against its side indefinitely). Throwing away
                    // the committed route makes the next tick plan a fresh
                    // one against the CURRENT world. Only a target still
                    // unreachable after a couple of tries is abandoned --
                    // dropping it on the first stall meant a merely-stale
                    // route lost a perfectly reachable foundation.
                    u.path.clear();
                    u.path_i = 0;
                    u.approach_prev_pos.reset();
                    u.approach_target.reset();
                    u.approach_progress_check_t = 0.0;
                    if (u.approach_replans < 2) {
                        ++u.approach_replans;
                    } else {
                        u.build_target = kNullRef;
                        u.approach_replans = 0;
                    }
                } else {
                    advance_to_building(u, *b, dt, world);
                }
            } else {
                u.approach_replans = 0; // arrived: next approach starts fresh
                u.working = true;
                u.path.clear();
                u.approach_progress_check_t = 0.0;
                u.approach_prev_pos.reset();
                u.approach_target.reset();
                // A foundation nobody has started yet is walk-through (see
                // Building::blocks_movement), so the footprint has to be
                // cleared of units before the first hammer blow -- that
                // blow is what turns it solid, and anyone still standing
                // on it would be sealed inside. Two cases:
                //   1. THIS builder is on the footprint (its A* route to
                //      the perimeter is free to cut straight across a
                //      walk-through foundation, and at_dropoff's buffer
                //      covers the whole interior, so it can stop in the
                //      middle). Walk it back off the nearest edge.
                //   2. Somebody else is standing there. Wait -- and shove
                //      off any teammate that isn't actually going anywhere
                //      (no move goal, no path), since one parked there --
                //      idle, or stalled mid-order -- would otherwise sit
                //      forever and the foundation would never get built at
                //      all. Teammates with a path are just passing
                //      through; enemies genuinely do deny the build while
                //      they stand on it. Safe even for a gatherer: nothing
                //      productive can be standing inside a footprint in
                //      the first place (resources and farms both block a
                //      foundation from being placed over them), and
                //      move_goal outranks gathering in update_unit, so it
                //      steps aside and goes right back to work.
                // Only while construction == 0: past that the building is
                // already solid, so nothing can be inside it, and a
                // re-check could only ever wedge a half-built building
                // against a unit that has no way out.
                if (!b->complete && b->construction <= 0.0) {
                    if (world.inside_footprint(*b, u.common.x, u.common.y)) {
                        auto [ox, oy] = world.point_off_footprint(*b, u.common.x, u.common.y,
                                                                  u.common.is_air, u.common.is_ship);
                        step_toward(u, ox, oy, dt, world);
                        u.working = false;
                        return;
                    }
                    auto blockers = world.units_on_footprint(*b);
                    if (!blockers.empty()) {
                        for (auto bref : blockers) {
                            Unit* o = world.get(bref);
                            if (!o || o->common.team != u.common.team) continue;
                            bool going_somewhere = o->move_goal || !o->path.empty() ||
                                                  o->attack_target.valid();
                            if (going_somewhere) continue;
                            auto [ox, oy] = world.point_off_footprint(*b, o->common.x, o->common.y,
                                                                      o->common.is_air, o->common.is_ship);
                            o->move_goal = Vec2{ox, oy};
                            o->need_path = true;
                            o->path.clear();
                            o->path_i = 0;
                        }
                        u.working = false;
                        return;
                    }
                }
                // Direct port of the original's construction rate (obj_unit/
                // Step.gml, control.game_mode==0 branch): +5/reload-cycle for
                // house/refinery, +1 for base/fortress, +3 for everything
                // else, ticking once per builder's OWN reload cadence (same
                // reload_timer countdown update_gather uses, already
                // decremented every tick up in update_unit) -- NOT a flat
                // continuous dt*25 rate every tick regardless of building
                // type, which is actually the GML's OTHER, non-default
                // control.game_mode!=0 fast-build branch and was making
                // every building finish in a few seconds flat.
                if (u.reload_timer <= 0) {
                    u.reload_timer = u.reload;
                    // Walls are the two extremes of build speed: a palisade
                    // snaps up almost instantly (cheap wooden fence), while
                    // the iron/stone wall is the slowest thing in the game to
                    // raise (a heavy masonry segment). Everything else keeps
                    // the original per-type rates (house/refinery +5, base/
                    // fortress +1, all others +3).
                    double rate = (b->name == "palisade")                        ? 25.0
                                  : (b->name == "iron wall")                     ? 0.75
                                  : (b->name == "farm")                          ? 9.0  // 3x the generic rate
                                  : (b->name == "house" || b->name == "refinery") ? 5.0
                                  : (b->name == "base" || b->name == "fortress")  ? 1.0
                                                                                  : 3.0;
                    // Leader build-speed bonus (Franklin D. Roosevelt: +15%).
                    const Team& bt = world.control.teams[u.common.team];
                    rate *= world.bonuses.leader_build_mult(bt.civ, bt.leader);
                    b->construction += rate * world.build_speed;
                }
                // Same swing cadence as update_gather's hammer/tool animation
                // (see swing_t/swing_down there) -- not wired up for
                // build/repair before, so a builder just stood still holding
                // the tool instead of swinging it (game/entity.py's Draw
                // equivalent -- Draw.gml's obj_unit -- animates both the
                // same way, alternating spr_civilian_working1/2 on a shared
                // sine clock). The "build" sound fires once per swing (on
                // the cycle wrap), not every tick -- it used to push
                // unconditionally every simulation tick (20/sec), which
                // sounded like a continuous rattle instead of a hammer
                // strike landing in time with the swing animation.
                u.swing_t += dt;
                if (u.swing_t >= 0.6) {
                    u.swing_t = 0.0;
                    if (u.common.team == 0) {
                        world.events.push({EventType::Sound, "build", u.common.x, u.common.y, 350, kNullRef, ""});
                    }
                }
                u.swing_down = u.swing_t < 0.3;
            }
            return;
        }
    }

    // 0b) repair a damaged building
    if (u.repair_target.valid()) {
        Building* b = world.get_building(u.repair_target);
        if (!b || !b->common.alive || !b->complete || b->common.hp >= b->common.max_hp) {
            u.repair_target = kNullRef;
        } else {
            if (!world.at_dropoff(*b, u.common.x, u.common.y)) {
                if (approach_stalled(u, dt)) {
                    u.repair_target = kNullRef;
                    u.path.clear();
                    u.approach_prev_pos.reset();
                } else {
                    advance_to_building(u, *b, dt, world);
                }
            } else {
                u.working = true;
                u.path.clear();
                u.approach_progress_check_t = 0.0;
                u.approach_prev_pos.reset();
                u.approach_target.reset();
                bool alive = world.control.repair_tick(u.repair_target, u.common.team, dt, world);
                if (!alive) u.repair_target = kNullRef;
                u.swing_t += dt;
                if (u.swing_t >= 0.6) {
                    u.swing_t = 0.0;
                    if (alive && u.common.team == 0) {
                        world.events.push({EventType::Sound, "build", u.common.x, u.common.y, 350, kNullRef, ""});
                    }
                }
                u.swing_down = u.swing_t < 0.3;
            }
            return;
        }
    }

    // 1) aircraft: entirely separate flight/fuel/landing/combat model --
    // see aircraft_behavior.cpp.
    if (u.common.is_air) {
        update_aircraft(self, u, dt, world);
        return;
    }

    // Artillery attack-ground: shell a fixed map point (the player's T +
    // right-click target) at the normal fire rate, forever, until a new order
    // clears it. No enemy target needed -- the lob shell splashes wherever it
    // lands. A move/attack order resets attack_ground client-side, and the
    // move_goal branch below would take over anyway.
    // A packed/transitioning ballistic launcher can't attack-ground (it isn't
    // deployed); a deployed one can't reposition, so it simply can't fire at a
    // point beyond its range rather than driving toward it.
    if (u.attack_ground && !u.move_goal && !(u.is_ballistic && (u.packed || u.pack_t > 0.0))) {
        Vec2 g = *u.attack_ground;
        double d = std::hypot(g.x - u.common.x, g.y - u.common.y);
        if (u.range_px > 0 && d > u.range_px) {
            if (u.is_ballistic) return;          // deployed: can't move to close the gap
            step_toward(u, g.x, g.y, dt, world); // out of range -> advance until in range
            return;
        }
        if (u.min_range_px > 0 && d < u.min_range_px) {
            // too close for the min arc -> back off to the far side of the point
            step_toward(u, 2 * u.common.x - g.x, 2 * u.common.y - g.y, dt, world);
            return;
        }
        if (g.x > u.common.x + 1) u.facing = 1;
        else if (g.x < u.common.x - 1) u.facing = -1;
        u.moving = false;
        if (u.reload_timer <= 0 && u.attack > 0) {
            u.reload_timer = u.reload;
            world.spawn_projectile(u, kNullRef, ship_shot_origin(u, world.rng), g);
            world.events.push({EventType::Sound, fire_sound(u), u.common.x, u.common.y, 0, self, ""});
            // (Launch exhaust particles are emitted by the projectile itself
            // during its launch hold -- see update_projectile.)
        }
        return;
    }

    if (u.move_goal) {
        if (u.need_path) {
            u.need_path = false;
            bool is_ship = u.common.is_ship;
            // Route around solid buildings and resources too, not just
            // water/land -- otherwise the computed path can cut straight
            // through a building or a dense tree cluster and lean entirely
            // on step_toward's reactive one-axis-slide fallback to
            // improvise around it tile-by-tile, which easily gets stuck
            // rather than actually routing around the obstacle. Reuses
            // World::passable (tile-center pixel of tx,ty) so this stays in
            // sync with the movement-time collision check by construction.
            (void)is_ship;
            auto passfn = make_passfn(world, u);
            auto raw = astar(world.cols, world.rows, TILE, u.common.x, u.common.y, u.move_goal->x,
                             u.move_goal->y, passfn);
            u.path.clear();
            for (auto& p : raw) u.path.push_back(Vec2{p.x, p.y});
            u.path_i = 0;
            if (raw.empty()) {
                int sx = static_cast<int>(std::floor(u.common.x / TILE));
                int sy = static_cast<int>(std::floor(u.common.y / TILE));
                int gx = static_cast<int>(std::floor(u.move_goal->x / TILE));
                int gy = static_cast<int>(std::floor(u.move_goal->y / TILE));
                if (!(sx == gx && sy == gy)) {
                    // astar found no route at all (as opposed to "you're
                    // already standing in the goal tile", its other empty-
                    // result case) -- don't fall back to a blind straight
                    // line toward the goal. That fallback is exactly what
                    // used to walk a unit straight into the mouth of a
                    // V-shaped dead end and leave it sliding along the walls
                    // forever: it has zero awareness of the obstacle's
                    // shape, unlike astar's full grid search. Just give up.
                    u.move_goal.reset();
                    return;
                }
            }
        }
        if (!u.path.empty()) {
            Vec2 wp = u.path[u.path_i];
            bool arrived = step_toward(u, wp.x, wp.y, dt, world, u.group_speed_px,
                                       /*ignore_ally_blocking=*/true);
            bool last_wp = (u.path_i + 1 >= u.path.size());
            if (last_wp) {
                // The final waypoint IS the player's actual destination --
                // walk to it precisely rather than the generous tolerance
                // below (that's for intermediate routing checkpoints, not
                // the clicked point itself; using it here would stop a unit
                // visibly short of its destination even on open, empty
                // ground). Only settle for less than exact arrival if the
                // exact point is demonstrably unreachable -- something is
                // actually standing on/blocking it -- not just because the
                // unit happens to be within a body-radius on some ordinary
                // tick before its natural exact-arrival step; checking that
                // distance alone regardless of cause used to end the order
                // early even in wide open terrain with nothing in the way.
                // ignore_allies=true here too (matching step_toward above):
                // a group/formation move's per-unit destinations are always
                // distinct by construction (see formation_slots/the
                // centroid-offset in right_click_order), so another ally
                // merely passing through this exact point at this exact
                // moment shouldn't count as "genuinely unreachable" and
                // trigger the imprecise "close enough" fallback below --
                // that used to make units settle for a nearby-but-wrong
                // spot instead of their real, always-reachable slot,
                // reading as "never actually gets in position".
                bool dest_blocked = u.move_goal &&
                                     (!world.passable(u.common.is_air, u.common.is_ship, u.move_goal->x,
                                                       u.move_goal->y) ||
                                      blocked_by_unit(world, u, u.move_goal->x, u.move_goal->y,
                                                      /*ignore_allies=*/true));
                double goal_d = u.move_goal ? std::hypot(u.common.x - u.move_goal->x,
                                                         u.common.y - u.move_goal->y)
                                            : 1e9;
                if (arrived || (dest_blocked && goal_d < kBodyRadius)) {
                    u.path.clear();
                    u.path_i = 0;
                    u.move_goal.reset();
                }
            } else {
                // Intermediate waypoint: a generous tolerance is fine (and
                // necessary) here since it's only a routing checkpoint, not
                // the destination -- astar picks waypoints blind to other
                // units (see World::passable's callers), so one can land
                // inside another unit's blocking radius and be permanently
                // unreachable to the exact pixel, with step_toward never
                // returning true for it no matter how long it's given.
                // Worst case, a waypoint coincides exactly with a blocking
                // unit's own center, and the closest this unit's body can
                // ever get to that point is kSeparation (blocked_by_unit's
                // own floor) -- so that plus a few px of margin for float
                // slop reliably covers ANY such case. In the ordinary case
                // (nothing blocking the waypoint) this fires at essentially
                // the same moment `arrived` would anyway, since the unit is
                // passing right by it.
                double wp_d = std::hypot(u.common.x - wp.x, u.common.y - wp.y);
                if (arrived || wp_d < kSeparation + 4.0) u.path_i++;
            }
        } else if (u.move_goal) {
            bool arrived = step_toward(u, u.move_goal->x, u.move_goal->y, dt, world, u.group_speed_px,
                                       /*ignore_ally_blocking=*/true);
            // Same reasoning as the last-waypoint case above: only accept
            // "close enough" in place of exact arrival if the destination
            // is actually obstructed, not on distance alone.
            bool dest_blocked = !world.passable(u.common.is_air, u.common.is_ship, u.move_goal->x, u.move_goal->y) ||
                                 blocked_by_unit(world, u, u.move_goal->x, u.move_goal->y,
                                                 /*ignore_allies=*/true);
            double gd = std::hypot(u.common.x - u.move_goal->x, u.common.y - u.move_goal->y);
            if (arrived || (dest_blocked && gd < kBodyRadius)) {
                u.move_goal.reset();
            }
        }
        return;
    }

    if (u.is_gatherer && !(u.attack_target.valid() || u.forced || u.rally)) {
        update_gather(self, u, dt, world);
        return;
    }

    update_combat(self, u, dt, world);
}

} // namespace ww::sim
