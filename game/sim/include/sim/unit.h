#pragma once
#include "sim/entity_common.h"

#include <optional>
#include <string>
#include <vector>

namespace ww::sim {

// AoE-style shift-queued follow-up order (see World::queue_order and
// unit_behavior.cpp's advance_order_queue). Deliberately limited to the
// four order kinds that have a clean, detectable "done" moment -- move
// (arrival), build (foundation completes), gather and attack (the
// specific watched entity dies/depletes, see Unit::active_queue_watch) --
// attack-move and repair are excluded, neither has a player-visible single
// completion point that would make "advance to the next queued step" read
// as correct rather than surprising.
enum class QueuedOrderKind : uint8_t { Move, Gather, Attack, Build };
struct QueuedOrder {
    QueuedOrderKind kind = QueuedOrderKind::Move;
    double x = 0.0, y = 0.0;        // Move
    EntityRef target = kNullRef;    // Gather/Attack/Build
};

// Data layout only -- direct field-for-field port of Unit.__init__ (and
// the attributes it accumulates during update()) from game/entity.py.
// Behavior (Unit::update and friends) is NOT implemented here: it needs
// World (spatial grid, control, pathfinding, spawn_projectile/effect) to
// exist first, so it lands in match.cpp/world.cpp alongside World itself
// (see the port task list). This header exists so the rest of the sim
// core has a concrete, complete type to store in a SlotMap<Unit> and to
// reference via EntityRef.
struct Unit {
    EntityCommon common;

    std::string name;    // catalog key, e.g. "civilian", "rifleman"
    std::string sprite;
    int armor = 0, pierce = 0;
    double hit_timer = 0.0;   // recently-damaged (shake/debris/warning)
    bool warned = false;      // attack-warning fired for this damage episode
    bool deleted = false;     // removed by the player (Delete) -> clean death, no explosion boom

    // ---- stats copied from catalog at spawn (entity.py Unit.__init__) ----
    double speed_px = 30.0;
    double reload = 1.0;
    double range_px = 0.0;
    double min_range_px = 0.0; // can't fire at anything closer than this (artillery/siege)
    double accuracy = 1.0;     // <1 scatters the shell's landing point (artillery)
    double sight_px = 5.0 * 32.0; // 32.0 = GML's tile size (== sim::TILE, unreachable from this header)
    double attack = 0.0;
    double base_attack = 0.0; // catalog attack before civ/tech bonuses (UI shows base+delta)
    bool melee = false;
    std::string unit_type;
    int facing = 1; // 1 = right, -1 = left
    bool moving = false;
    double fuel = 100.0, fuel_max = 100.0;
    // catalog "fuel_latency" (GML: steps per 1 fuel point burned, at 30
    // steps/sec) -- e.g. a fighter (25) burns its tank faster than a
    // heavy bomber (40). Cached at spawn (aircraft_behavior.cpp reads it
    // every tick) rather than re-read from the catalog per-tick. Only
    // meaningful for is_air units; irrelevant default for everyone else.
    double fuel_latency = 27.5;
    bool refueling = false;
    bool land_order = false; // player clicked "Land": fly home + set down regardless of fuel
    // Player told this plane to garrison at the airbase (via Land): once it has
    // touched down it STAYS parked -- even after refuelling to full -- until the
    // player gives it a fresh move/attack order. land_order is the transient
    // "fly home now" command (cleared on touchdown); this is the persistent
    // "stay put" state that survives the refuel-to-full takeoff gate.
    bool stationed = false;
    // Set when the PLAYER (or AI) gives this plane a fresh move/attack order, so
    // a parked plane launches IMMEDIATELY on that order even at partial fuel --
    // it bypasses the "keep refuelling to full before taking off" grounding
    // rule (aircraft_behavior.cpp's want_land subclause). Distinguishes a real
    // order from the internal low-fuel auto-return move_goal, which must still
    // refuel to full. Cleared on touchdown.
    bool player_launch = false;
    // The specific airbase (Building id) the player picked to land at (by
    // clicking it after the Land button). Empty -> fall back to the nearest own
    // airbase. Cleared on touchdown.
    std::optional<uint32_t> land_target_id;
    bool nuke_loaded = false; // heavy bomber loaded with a single atomic bomb (replaces its bombs)
    bool is_bomber = false;
    bool is_aa = false;
    bool mechanical = false;
    // ---- ballistic missile pack/unpack (fortress, scientific era) ----
    // A mobile launcher that must deploy to fire. PACKED (`packed`==true) it
    // moves at AA-gun speed but cannot shoot; UNPACKED it cannot move but
    // fires its lobbing missile. Toggling either way takes kPackSeconds, and
    // while `pack_t` > 0 the unit is mid-transition: it can neither move nor
    // fire. Spawns packed/mobile. Driven by update_unit; requested via the
    // command card's Pack/Unpack buttons (World::order_pack).
    bool is_ballistic = false;
    bool packed = true;       // true = mobile launcher; false = deployed to fire
    double pack_t = 0.0;      // seconds left in a pack/unpack transition (0 = settled)
    bool pack_target = true;  // the `packed` value the active transition heads toward
    // Deployed launcher's aim bearing (radians, screen space, 0=+x). Swivels
    // toward the current target / attack-ground point while stationary; starts
    // pointing up. Drives the sprite rotation client-side.
    double aim_angle = -1.5707963267948966; // -pi/2 (up)
    int clip_max = 0, clip_ammo = 0;
    double clip_reload_timer = 0.0;
    // Catalog "clip_reload" (GML frames, converted /60 same as `reload`) --
    // how long the WHOLE clip takes to refill once emptied. Battleship/
    // yamato use this for their burst-then-pause cannon cadence (see
    // update_combat); most units have no clip_size at all (0), so this
    // stays unused for them.
    double clip_reload = 0.0;
    bool diving = false;
    double dive_t = 0.0;
    // Strafing-run state (aircraft): after a firing pass the plane breaks off
    // and banks wide for strafe_t seconds before swinging back in, so it makes
    // repeated passes instead of a tight orbit. See aircraft_behavior.cpp.
    bool strafe_break = false;
    double strafe_t = 0.0;
    bool landed = false;
    std::optional<uint32_t> home_id; // Building id, not an EntityRef (Python stores building.id)
    // Altitude ramp, 0 (grounded) .. 64 (cruise ceiling) -- direct port of
    // the original GML's `height` (objects/obj_unit/Step.gml's takeoff/
    // landing ramp, ~0.5/step both ways), NOT present in the Python
    // reference port (game/entity.py's _update_air just snaps landed on/
    // off with a fixed visual offset). Added back here because a gradual
    // climb/descent is exactly what a real "takeoff" reads as -- see
    // aircraft_behavior.cpp. Only ever nonzero for is_air units; purely a
    // simulated altitude, NOT added to common.y (gameplay -- targeting,
    // range, splash damage -- always uses the true ground x/y), the
    // visual rise is applied client-side only (see game_client.cpp).
    double height = 0.0;
    // Current movement heading in radians (aircraft only) -- the plane's
    // actual travel direction, which TURNS toward wherever it's steering
    // at a limited rate (turn_speed below) instead of snapping instantly.
    // Direct port of the GML's `direction` var interpolating toward
    // `aim_direction` by `turn_speed` degrees/step (objects/obj_unit/
    // Step.gml) -- this is what makes a course change read as a banked
    // arc, and what makes circling a target/hover point look like a loose
    // orbit instead of jerky point-chasing. 0 = east, increases clockwise
    // in screen space (matches std::atan2(dy,dx) on this port's y-down
    // coordinate system).
    double heading = 0.0;
    // catalog "turn_speed" (GML: degrees turned per step at 30 steps/sec)
    // -- e.g. a heavy bomber (1.5) turns much more sluggishly than a jet
    // fighter (3.5). Cached at spawn like fuel_latency.
    double turn_speed = 2.0;
    std::string bullet_sprite = "spr_bullet";
    bool is_gatherer = false;
    double max_carry = 10.0;

    // ---- transport ship (amphibious carrier) ----
    // catalog "capacity" -- pop-space this ship can hold (0 = not a transport).
    int transport_cap = 0;
    // ---- aircraft carrier (mobile sea airbase) ----
    // catalog "air_capacity" -- how many friendly planes can land/park on this
    // ship to refuel/rearm (0 = not a carrier). is_carrier caches air_capacity>0
    // so aircraft_behavior.cpp's landing search can cheaply skip non-carriers.
    int air_capacity = 0;
    bool is_carrier = false;
    bool phase_trees = false; // Li Zongren's infantry: walk through trees/palms
    // Units currently garrisoned inside this transport (kept alive so they
    // still count toward the pop cap, but hidden/skipped while carrier-held).
    std::vector<EntityRef> cargo;
    // Transport ordered to unload its cargo near this ground point.
    std::optional<Vec2> unload_point;
    // Passenger side: the transport this unit has been ordered to board
    // (walks to it, then garrisons), and the one currently carrying it
    // (valid -> the unit is inside a transport: not drawn, updated, or hit).
    EntityRef load_target = kNullRef;
    EntityRef carrier = kNullRef;

    // ---- dynamic order/movement state ----
    std::optional<Vec2> move_goal;   // player move order (ignores enemies)
    // Set alongside move_goal by a GROUP move order (see World::order_move's
    // group_speed_px param) to the slowest member's speed_px, so the whole
    // group arrives together instead of fast units outrunning slow ones and
    // scattering along the way. <=0 means "no cap, move at full speed" --
    // the default for a lone unit's order, and reset back to that by
    // order_move on every fresh call, so a stale cap can never survive onto
    // an unrelated later order.
    double group_speed_px = -1.0;
    std::optional<Vec2> rally;       // AI attack-move goal
    // Routing state for `rally`. This was the FIFTH movement path with no
    // route and no watcher: the rally branch was a bare step_toward, so every
    // AI army crossed the map by pure greedy steering and any concave pocket
    // of trees/buildings on the straight line was a trap it had no way to
    // see -- it would press into the back wall indefinitely (the player's
    // "large force completely stuck in a concavity"). Mirrors the gather
    // approach's proven shape: plan once per rally point, walk the waypoints,
    // watch NET progress, replan twice, then give up rather than jam forever.
    std::optional<Vec2> rally_path_for;
    std::optional<Vec2> rally_prev_pos;
    double rally_progress_t = 0.0;
    int rally_strikes = 0;
    std::optional<Vec2> attack_ground; // artillery: shell this fixed map point forever, until re-ordered
    std::optional<Vec2> hold;        // hold-position anchor once settled
    // Campaign "dormant" trigger: while true the unit is frozen (skips all
    // movement/combat in update_unit, like a garrisoned unit) yet stays alive
    // and solid. Cleared by a WakeTrigger when the player trips it (world.cpp),
    // which then sets `rally` so the group charges. See World::wake_triggers.
    bool dormant = false;
    EntityRef attack_target = kNullRef;
    EntityRef gather_target = kNullRef;
    EntityRef drop_target = kNullRef;
    EntityRef repair_target = kNullRef;
    EntityRef build_target = kNullRef;
    double carry = 0.0;
    int carry_type = 0; // FOOD/WOOD/OIL/IRON
    // The CURRENT gather_target's resource type, set by World::order_gather
    // at assignment time (not just once actually collecting, like carry_type
    // above) -- lets update_gather re-seek the nearest resource of the same
    // type if this exact target disappears (e.g. another villager finishes
    // it off first) without needing to dereference the now-dead/erased
    // target to ask what it was. -1 = no gather order in effect.
    int gather_rtype = -1;
    double reload_timer = 0.0;
    double musket_t = 0.0; // Enver Pasha: countdown between charging musket shots
    bool forced = false; // commanded to pursue attack_target regardless of range/sight
    std::vector<Vec2> path; // A* waypoints for the current move order
    size_t path_i = 0;
    bool need_path = false;
    bool working = false; // civilian actively harvesting
    double highlight = 0.0;

    // ---- local avoidance (steering around other units) ----
    // A chosen detour heading, expressed as a signed offset (radians) from
    // the straight-line bearing to whatever step_toward's caller is
    // currently aiming at. Once picked, held -- not re-decided from
    // scratch every tick -- until either the direct line opens back up or
    // this specific heading stops being clear. Re-deciding on any fixed
    // timer (an earlier version expired the commitment after a fraction of
    // a second) reopens the choice of which side to pass an obstacle on
    // before the unit has actually finished going around it, and the fresh
    // decision can land on the OTHER side just as easily -- that's what
    // read as a unit flip-flopping between two headings while skirting a
    // single stationary obstacle. Holding until genuinely invalidated means
    // a chosen side is kept for the whole maneuver.
    //
    // (An absolute-point version of this commitment -- a fixed world-space
    // spot to walk toward instead of a relative heading -- was tried to fix
    // a specific pathological case: a mover squarely between two blockers
    // close enough together that their bodies' exclusion zones fully
    // overlap, leaving no passable gap at all. It didn't actually resolve
    // that case either, and regressed several ordinary, previously-solid
    // scenarios in the process (single-obstacle routing, dense group
    // crossings), so it was reverted. That merged-obstacle, zero-gap
    // configuration remains a known hard case for this purely local/
    // reactive scheme -- the unit still gives up cleanly rather than
    // looping forever, see Unit::stall_strikes, just doesn't find a route
    // around within the usual few seconds.)
    double avoid_offset = 0.0;
    bool avoid_committed = false;
    // Consecutive times step_toward has had to fall through to the RETREAT
    // tier (wide swing and tight squeeze both failed) without an
    // intervening tick of genuinely open direct movement. In a dense crowd
    // a single small retreat often lands the unit right back in another
    // pocket, so it retreats again, and again -- each retreat individually
    // "succeeds" (the unit does move a little) but the net effect reads as
    // vibrating in place rather than actually escaping. Rising retry count
    // scales up both how far and how long the next retreat commits to, so
    // repeated failures escalate into a real breakout instead of repeating
    // the same too-small nudge. Reset to 0 the moment the direct line opens.
    int avoid_retry_count = 0;
    // Consecutive ticks step_toward found NO legal heading at all (see its
    // last-resort phase-through tier). Reset the instant the unit moves.
    int wedged_ticks = 0;

    // ---- anti-stuck / animation-only state accumulated during update ----
    std::optional<Vec2> prev_pos;
    double stuck_t = 0.0;
    // Remaining-path-distance progress tracking: re-checked every ~1.5s
    // (progress_check_t) against last_goal_dist to catch a unit that IS
    // moving (so stuck_t/re-pathing above never fires) but never actually
    // advancing along its route -- e.g. wedged against a wall, or jostling
    // against other units all converging on the same exact point. Measured
    // along the remaining A*-routed path rather than raw distance to
    // move_goal (see remaining_path_distance in unit_behavior.cpp), since a
    // real detour around an obstacle often has to walk further from the
    // goal as the crow flies before it gets closer. See update_unit's
    // give-up check.
    double progress_check_t = 0.0;
    double last_goal_dist = -1.0;
    // Consecutive 1.5s windows (see progress_check_t) with no real
    // progress. The first two just force a fresh repath instead of giving
    // up outright -- most stalls are transient traffic (another unit
    // crossing the same spot, a brief overlap-correction push, see
    // resolve_overlap) that resolve themselves with a new route (or a
    // second try at one), not a genuinely unreachable goal. Only gives up
    // for real if a THIRD window in a row also shows no progress. Reset to
    // 0 on any real progress or fresh order.
    int stall_strikes = 0;
    double orbit_a = 0.0;
    std::optional<Vec2> hover;
    int barrel = 0;         // yamato rotating turret position (0..2)
    double swing_t = 0.0;   // gather hammer/tool swing cycle
    bool swing_down = false;

    // Same give-up-if-stuck idea as stuck_t/prev_pos above, but tracked
    // separately for build_target/repair_target movement: those never set
    // move_goal (see update_unit's build/repair branches, which return
    // before the move_goal block even runs), so prev_pos never gets
    // updated while a builder is walking to a foundation -- reusing it
    // here would compare against a stale position from whatever the
    // unit's last ordinary move order was, if any.
    std::optional<Vec2> approach_prev_pos;
    double approach_progress_check_t = 0.0;
    // The specific perimeter point (one side of the target building) this
    // approach has committed to, set once when a build/repair movement
    // starts and then reused every tick until arrival/give-up -- NOT
    // recomputed each call. Without this, once the A*-routed path to a
    // clear side is fully walked, advance_to_building would ask
    // nearest_perimeter_point() for the closest side again from the new
    // (closer-in) position, which can flip back to whichever side is
    // flush against the neighbor that was routed around in the first
    // place, undoing the routing and getting stuck again right at the
    // end of the approach.
    std::optional<Vec2> approach_target;

    // How many times the CURRENT approach has been re-planned after
    // stalling -- shared by all three approach kinds (build/repair, the
    // walk out to a resource, and the walk back to a drop-off), which are
    // mutually exclusive per tick and each reset it on arrival.
    // advance_to_building routes ONCE, on the first tick of an approach,
    // and then commits -- but the world moves underneath it: a building
    // goes up, a wall closes, or (the case the player actually hit) a
    // foundation the route was drawn through gets started and turns solid.
    // A stall used to mean "give up the order outright", so a unit whose
    // route merely went stale abandoned a perfectly reachable target. Now
    // the first couple of stalls throw the committed route away and plan a
    // fresh one against the CURRENT world, and only a target that's still
    // unreachable after that is dropped.
    int approach_replans = 0;

    // Same give-up-if-stuck idea again, tracked separately for GATHER
    // approach (heading to a resource/farm to start working): gathering
    // never sets move_goal OR build/repair's approach_* fields either, so
    // reusing those would compare against a stale position from whatever
    // the unit was last doing. If a villager can't make progress toward
    // its target for a ~4s window (measured as DISTANCE-to-target
    // shrinking, not raw per-tick displacement -- a unit can be
    // oscillating, moving several px every tick while making zero net
    // progress, e.g. wedged approaching a resource with another gatherer
    // parked right on the approach line; a per-tick "did it move at all"
    // check can never catch that, since each individual tick's movement
    // is comfortably above any such threshold), update_gather retargets
    // to the nearest resource of the same type instead of pushing against
    // the obstacle forever. Mirrors move_goal's progress_check_t/
    // last_goal_dist (update_unit), simplified since gather has only one
    // escalation action (switch resource), not a repath/give-up ladder.
    double gather_progress_check_t = 0.0;
    double gather_last_dist = -1.0;

    // A*-routed approach to a gather target, sharing path/path_i with move
    // and build orders (all three are mutually exclusive -- see
    // advance_to_building's comment). Walking to a resource used to be
    // PURE local steering with no route at all, which is fine in the open
    // but has no answer to terrain: a villager whose berry vein sits round
    // the arm of a forest, or anywhere inside a concave pocket, just
    // pressed into the obstacle until the stall check above gave up on the
    // resource entirely. Since villagers do almost all the walking in a
    // match, that was most of the "units get stuck" the player sees.
    //
    // gather_path_for is the target the current path was planned for, so a
    // reassignment (the AI's resource rebalance, a depleted node, a new
    // player order) drops the stale route instead of walking it. Replans
    // are on a cooldown rather than every tick: an unreachable target
    // would otherwise run a full-budget search every single tick for every
    // villager stuck on it. It covers BOTH legs of the gather loop -- the
    // walk out to the resource and the walk back to the drop-off -- since
    // those are mutually exclusive (delivery returns early in
    // update_gather), so the one field means "whatever this villager's
    // current route was planned for".
    EntityRef gather_path_for = kNullRef;
    double gather_repath_t = 0.0;

    // ---- shift-queued follow-up orders (see World::queue_order) ----
    // Up to World::kMaxQueuedOrders pending steps, popped and issued one at
    // a time by unit_behavior.cpp's advance_order_queue as each prior step
    // finishes. NOT touched by a plain (non-shift) order -- issuing one
    // clears this (see World::order_move/order_gather/order_attack's
    // `from_queue` guard and right_click_order's non-shift branches).
    std::vector<QueuedOrder> order_queue;
    // The specific gather/attack target the CURRENTLY active queued step is
    // watching for death/depletion -- set only while that step came from
    // the queue, so advance_order_queue can force a clean completion
    // instead of letting update_gather's same-type-resource re-seek or
    // update_combat's forced-attack rally-toward-last-position fallback
    // keep the unit busy indefinitely once its actual target is gone.
    EntityRef active_queue_watch = kNullRef;
    // True while the unit's CURRENTLY active order (not just order_queue's
    // remaining entries) was itself popped from the queue -- lets the
    // client draw the whole chain (what it's doing now + what's still
    // queued after that), not just the not-yet-started tail. Set whenever
    // advance_order_queue pops a step; cleared once the unit is genuinely
    // idle again with nothing left queued/watched, or a fresh non-queued
    // order supersedes it (World::order_move/gather/attack's `from_queue`
    // guard).
    bool queue_active = false;
};

} // namespace ww::sim
