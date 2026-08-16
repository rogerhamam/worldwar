#pragma once
#include "sim/bonuses.h"
#include "sim/building.h"
#include "sim/catalog.h"
#include "sim/control.h"
#include "sim/deer.h"
#include "sim/entity_common.h"
#include "sim/event.h"
#include "sim/projectile.h"
#include "sim/resource.h"
#include "sim/rng.h"
#include "sim/spatial_grid.h"
#include "sim/unit.h"

#include <array>
#include <string>
#include <unordered_set>
#include <vector>

namespace ww::sim {

// 32, matching the original GameMaker game's own grid (confirmed via
// assets/gmk's many `div 32` grid references, e.g.
// ds_grid_create(room_width div 32, room_height div 32)) and unit sprites'
// native 32x32 pixel size (assets/manifest.json's
// spr_civilian etc, fw/fh:32). Previously 24 (a deviation introduced by
// the old Python prototype, reason undocumented there) -- that
// mismatch between TILE and the 32px-native sprite art, and between TILE
// and catalog stat conversions that were already hardcoded to GML's
// literal 32 (range_px/sight_px below, bonuses.cpp, unit.h's sight_px
// default), was part of what made unit speed/scale feel off relative to
// the original game.
constexpr int TILE = 32; // game/world.py TILE
constexpr int WATER = 2;
// Building footprints (building_wh(), world.cpp) are now each building's
// actual native sprite pixel size (assets/manifest.json's
// fw/fh) instead of a "grid squares * some tile constant" scheme -- the
// old scheme assumed every footprint was a multiple of 24px, which doesn't
// hold for most buildings (house/farm/barracks/etc. are true 64x64
// sprites, not 72x72 as 3 squares * 24 implied). Every real footprint size
// happens to be an exact multiple of TILE (32), so snap()/footprint_clear()
// grid-align directly to TILE now, with no separate building-grid constant
// needed. BTILE survives only as a small fixed px spacing constant used by
// a couple of "step back from the building" offsets (building_behavior.cpp)
// -- not for footprint sizing.
constexpr int BTILE = 24; // game/entity.py BTILE (building-grid tile)

// Fog of war grid resolution, in cells per terrain-tile edge. 1 = one fog
// cell per full TILE-px tile (matches game/world.py exactly). A per-tile
// subdivided grid was tried for a rounder reveal silhouette but looked too
// finely staircased; the client now rounds each tile's own outer corners
// for softening instead (see draw()'s fog overlay), so this stays at the
// original tile resolution.
constexpr int kFogSubdiv = 1;
constexpr int kFogTilePx = TILE / kFogSubdiv; // must evenly divide TILE

// resource kind -> (sprite, rtype, amount) -- game/world.py RESOURCE_KINDS
struct ResourceKindInfo {
    std::string sprite;
    int rtype;
    double amount;
};
const std::unordered_map<std::string, ResourceKindInfo>& resource_kinds();

// Non-gatherable clutter/obstacle kind -> (sprite, frame, passable) --
// editor's kTerrainKinds decoration entries (rubble,
// stones, cliffs, fences, hedge, bush, crate, pebbles, the 5 destroyed-
// building variants, and the named vehicle-wreckage rubbles), authorable
// there via TerrainFeature::resource same as any gatherable resource, but
// with no economic amount/gather behavior -- see World::decorations and
// Decoration below. `passable` mirrors the editor's own judgment calls
// (see its kTerrainKinds comment): there's no original-game source data
// for any of this.
struct DecorationKindInfo {
    std::string sprite;
    int frame = 0;
    bool passable = false;
};
const std::unordered_map<std::string, DecorationKindInfo>& decoration_kinds();

std::pair<int, int> building_wh(const std::string& name); // game/entity.py BUILDING_SIZE

// How far below a building's own world anchor (common.x/y) its footprint
// rectangle (building_wh) is actually centered -- see world.cpp's
// footprint_dy for the full explanation (tower/aa tower/outpost's
// footprint is just the base tile of a much taller sprite). Exposed here
// (not file-local to world.cpp) so client-side collision-adjacent code
// (GameClient::select_at's click hit-test, right_click_order's building
// scan) can place the SAME rectangle the sim itself collides against,
// rather than drifting out of sync with a second, independently-tuned copy.
double footprint_dy(const std::string& name);

// The authoritative simulation world: terrain, entities, spatial grid.
// Direct port of game/world.py's World class, PLUS the behavioral parts
// of Unit/Building/Deer/Projectile update() from game/entity.py (those
// needed World to exist -- see the unit.h/building.h header comments).
//
// Scope note: aircraft (`is_air` units -- fighters/bombers/etc., their
// fuel/dogfight/bombing/landing subsystem, and ohka's kamikaze dive) ARE
// ported, in sim/src/aircraft_behavior.cpp -- a merge of game/entity.py's
// Unit._update_air (fuel/target-acquisition/combat/orbit logic, which
// avoids the original GML's own fuel=0 dead-code bug -- see that file's
// header comment) with a `height` 0..64 takeoff/landing ramp restored from
// the raw GML (Python's port simplified that away to a binary landed/
// airborne snap). Yamato's rotating 3-barrel volley (a ship-specific
// special case, not aircraft) is still NOT ported -- yamato fires through
// the ordinary single-shot ranged-combat path like every other ship.
class World {
public:
    World(const DataStore& data, const Bonuses& bonuses, Control& control, Rng& rng,
          EventBus& events, int cols, int rows, const std::string& map_type, bool water);

    // ---- terrain ----
    int cols, rows;
    double px_w, px_h;
    std::string map_type;
    bool want_water;
    std::vector<std::vector<int>> terrain;

    // Fog of war, local player (team 0) only: 0 unexplored, 1 explored,
    // 2 visible. Sized (cols*kFogSubdiv) x (rows*kFogSubdiv) -- finer than
    // terrain's cols x rows, see kFogSubdiv above. Direct port of
    // game/world.py's World.fog/update_fog/visible (adapted to the finer
    // grid) -- Python throttles the recompute to ~5x/sec (session.py) to
    // bound interpreter overhead; update() below just calls it every tick
    // instead, since one pass over a skirmish-sized grid is negligible in
    // C++ and this keeps fog exactly in sync with movement instead of
    // trailing by up to 0.2s.
    std::vector<std::vector<int>> fog;
    bool reveal_all = false; // debug: fully revealed map (unused for now, ported for parity)
    // Whose vision the fog grid currently represents. 0 -- the local player --
    // in normal play; the dev POV cheat (GameClient's "pov" chat command)
    // repoints it at another team so that team's view can be watched, and puts
    // it back afterwards.
    //
    // Safe to point at an AI: fog only ever affects what is DRAWN and team 0's
    // own placement check, which place_building gates on `team == 0 &&
    // !teams[0].is_ai` -- an AI's own placement never consults fog (see
    // footprint_explored), so this cannot change how the watched AI behaves.
    int fog_player = 0;
    void update_fog(int player = 0);
    int fog_at(double px, double py) const;

    int tile_at(double px, double py) const;
    bool is_water(double px, double py) const;
    bool passable(bool is_air, bool is_ship, double px, double py, bool phase_trees = false) const;
    // Nearest point to (cx,cy) that passable(is_air,is_ship,...) accepts,
    // searching outward ring by ring (Chebyshev distance) up to 12 tiles.
    // Used to redirect a move order that targets an unreachable point
    // (e.g. clicked inside a building) to the closest reachable spot next
    // to it, instead of letting the unit walk at the exact unreachable
    // point forever (see order_move).
    // Passability for ROUTE PLANNING, as opposed to passable()'s physical
    // "can a body be at this pixel" test. The two deliberately disagree on
    // exactly one thing: a foundation nobody has started yet is
    // walk-through physically (Building::blocks_movement -- so a unit can
    // never be sealed in or walled off by one), but A* must still plan
    // AROUND it. A route drawn through a foundation is a trap: the first
    // hammer blow turns it solid, and whoever committed to that path is
    // then walking into a wall with no replan. That is a real bug the
    // player hit -- two house foundations, one villager routed straight
    // through the first one, the other villager started building it, and
    // the first was stuck against the side of it for the rest of the game.
    bool passable_planning(bool is_air, bool is_ship, double px, double py, bool phase_trees = false) const;
    std::pair<double, double> nearest_passable(double cx, double cy, bool is_air, bool is_ship) const;
    std::pair<double, double> clear_point_near(double cx, double cy, double dist, bool ship) const;
    std::pair<double, double> snap(const std::string& name, double cx, double cy) const;
    // `exclude` (optional) lists units that should NOT block the placement --
    // the villagers selected to build it. A large footprint (e.g. a fortress)
    // otherwise self-rejects when a member of a clustered multi-villager
    // selection is standing inside it; foundations are walk-through and the
    // builder steps off anyway, so its own crew must not veto the spot.
    bool footprint_clear(const std::string& name, double cx, double cy,
                         const std::vector<EntityRef>* exclude = nullptr) const;
    // footprint_clear, plus a `gap` px moat: the footprint inflated by `gap` on
    // every side must ALSO miss every existing building. footprint_clear only
    // rejects an actual overlap, so buildings placed by it can end up welded
    // edge-to-edge with no walkable lane between them -- which is what packs an
    // AI base solid and leaves its own units unable to get in or out. gap <= 0
    // behaves exactly like footprint_clear. Only the building-vs-building test
    // is inflated; the terrain/resource/shore rules are unchanged, since a
    // shipyard still has to sit on water touching the shore.
    bool footprint_clear_gap(const std::string& name, double cx, double cy, double gap,
                             const std::vector<EntityRef>* exclude = nullptr) const;
    // Local-player-only: true unless some tile of the footprint is still
    // unexplored ("dark") fog. Explored ("light") fog and full visibility
    // are both fine -- see footprint_explored's comment in world.cpp.
    bool footprint_explored(const std::string& name, double cx, double cy) const;
    bool at_dropoff(const Building& b, double ux, double uy) const;

    // ---- footprint queries (see Building::blocks_movement) ----
    // A live building's footprint rectangle as (x0,y0,x1,y1), the same one
    // all_building_rects_/footprint_clear collide against.
    std::array<double, 4> footprint_rect(const Building& b) const;
    // Strictly inside that rectangle -- edge-exact positions are OUT, so a
    // builder standing on the perimeter isn't considered to be on top of
    // the thing it's building.
    bool inside_footprint(const Building& b, double px, double py) const;
    // Every alive GROUND unit currently standing strictly inside b's
    // footprint. Construction can't begin while this is non-empty (see
    // unit_behavior.cpp's build branch), which is what guarantees an
    // unstarted, walk-through foundation can never turn solid around a
    // unit and trap it.
    std::vector<EntityRef> units_on_footprint(const Building& b) const;
    // The nearest passable point just OUTSIDE b's footprint -- where to
    // send a unit that's standing on a foundation and holding up its
    // construction.
    std::pair<double, double> point_off_footprint(const Building& b, double px, double py,
                                                   bool is_air, bool is_ship) const;

    // ---- entity storage ----
    SlotMap<Unit> units;
    SlotMap<Building> buildings;
    SlotMap<Resource> resources;
    SlotMap<Deer> deer;
    SlotMap<Projectile> projectiles;
    std::vector<EntityRef> active_units, active_buildings, active_resources, active_deer,
        active_projectiles;

    SpatialGrid grid{3 * TILE}; // cell size unchanged (96px) across the TILE 24->32 migration
    // Set whenever a building or resource is spawned or dies, so the grid's
    // STATIC layer (buildings+resources) is rebuilt next tick instead of every
    // tick (see rebuild_spatial_grid). Starts true so it builds on the first tick.
    bool static_grid_dirty_ = true;
    uint32_t next_id = 1;
    double build_speed = 1.0;

    // Per-sim-step budget for AI rally (attack-move) A* searches. When a whole
    // army re-commits during a big battle, hundreds of units would otherwise all
    // run a full long-range A* in a SINGLE step -- seconds of work at once, an
    // unresponsive multi-second freeze at ~1000 units. This caps how many rally
    // searches run per step; the rest DEFER (steer straight toward the goal for a
    // step and re-plan next step -- advance_rally self-corrects), so the army
    // still commits together but the pathfinding spreads over a few steps and no
    // single frame hangs. Reset each World::update. High enough that small
    // skirmishes rarely reach it. Deterministic (consumed in active_units order).
    int rally_astar_budget_ = 0;
    bool consume_rally_astar_budget() {
        if (rally_astar_budget_ > 0) { --rally_astar_budget_; return true; }
        return false;
    }

    // A persistent damaging fire patch -- unlike the client's fire-and-
    // forget cosmetic "spr_flame"/"spr_fire_cloud" Effect events (still
    // pushed separately, see spawn_missile_impact_fx), this one actually
    // hurts ground units standing in it each tick (see update()'s fire
    // sweep) for as long as `timer` has left. Left behind by artillery/
    // ship shell impacts today; a hand-placed campaign "flames" decoration
    // (editor) spawns one of these too, see
    // scenario.cpp's new_from_level.
    struct FireHazard {
        double x, y;
        double timer; // seconds remaining before this patch burns out
    };
    std::vector<FireHazard> fires;
    // Per-second damage a fire hazard deals to any non-tank ground unit
    // standing in it, and the radius (px) around its centre that counts as
    // "standing in it" -- there's no original-game value to port (the
    // source GameMaker obj_flame does no damage at all, see
    // spawn_missile_impact_fx's comment), so these are new, tunable
    // numbers: roughly "a squad standing in it for ~3-4s takes serious
    // damage but can still retreat out" for a typical infantry HP pool.
    static constexpr double kFireDamagePerSecond = 0.2; // 1 damage every 5 seconds
    static constexpr double kFireRadius = 28.0;

    // A static, non-gatherable piece of clutter/obstacle (see
    // decoration_kinds()) -- unlike Resource, it never depletes, has no
    // hp/team/combat relevance, and is never added to active_* lists or
    // the spatial grid; it exists purely for rendering (GameClient iterates
    // this directly) and, if its kind is impassable, for blocking movement
    // (see rebuild_occupied()'s decoration_tiles_ set, checked by
    // passable()). Populated by new_from_level (scenario.cpp) from a
    // campaign Level's TerrainFeature::resource entries; nothing removes
    // an entry once placed (there's no in-match way to destroy a rock or
    // fence).
    struct Decoration {
        double x, y;
        std::string kind; // key into decoration_kinds()
    };
    std::vector<Decoration> decorations;

    // A one-shot map trigger (editor's Events tab): the
    // first time any living unit comes within kMessageTriggerRadius of
    // (x,y), pushes an EventType::Notify (key "map_message", see event.h)
    // carrying `text` verbatim and never fires again -- a story-beat
    // marker, not a repeating hazard like FireHazard above. Populated by
    // new_from_level (scenario.cpp) from a campaign Level's "message"-type
    // MapEvent entries. Never rendered (see GameClient) -- purely a
    // trigger, unlike Decoration.
    struct MessageTrigger {
        double x, y;
        std::string text;
        bool triggered = false;
    };
    std::vector<MessageTrigger> message_triggers;
    // Roughly "must actually be standing on the trigger's own tile", a bit
    // tighter than kFireRadius since this is about precise position, not a
    // blast radius -- there's no original-game value to port (this trigger
    // kind doesn't exist in the source game).
    static constexpr double kMessageTriggerRadius = 20.0;

    // ---- Campaign map triggers (worldwar_campaign_editor's Events tab) ----
    // All populated by new_from_level (scenario.cpp) from a Level's MapEvents and
    // checked each tick in World::update, next to the message_triggers loop. Their
    // booleans mutate deterministically so they stay Match::checksum lockstep.
    struct TriggerRect { int tx = 0, ty = 0, tw = 0, th = 0; }; // tile-space box

    // "spawn": create `count` `unit`s owned by `player` inside `box` when the
    // gating objective clears (objective_id in cleared_objectives) or, if no
    // objective, when a team-0 unit enters `box`. One-shot.
    struct SpawnTrigger {
        TriggerRect box;
        std::string unit;
        int count = 0;
        int player = 1;
        std::string objective_id; // "" -> fire on a player unit entering box instead
        std::string text;         // notification shown when it fires
        bool fired = false;
    };
    std::vector<SpawnTrigger> spawn_triggers;

    // "resources": a visible marker tile at (x,y); the first time a team-0 unit
    // touches it, grant the four amounts to team 0, show `text`, and remove the
    // tile (fired -> client stops drawing it). One-shot.
    struct ResourcePickup {
        double x = 0, y = 0;
        double food = 0, wood = 0, oil = 0, iron = 0;
        std::string text;
        bool fired = false;
    };
    std::vector<ResourcePickup> resource_pickups;

    // "dormant": units in `group` are frozen (Unit::dormant) until a team-0 unit
    // enters `trip`; then they wake and charge `target`, the `los` boxes are added
    // to reveal_areas, and `text` shows. One-shot.
    struct WakeTrigger {
        std::vector<TriggerRect> trips; // any one entered by a team-0 unit wakes the group
        double target_x = 0, target_y = 0;
        std::vector<EntityRef> group;
        std::vector<TriggerRect> los;
        std::string text;
        bool fired = false;
    };
    std::vector<WakeTrigger> wake_triggers;

    // Rectangles permanently revealed to the local player (team 0): re-stamped to
    // fog=2 every update_fog. Grown by fired WakeTriggers' los boxes.
    std::vector<TriggerRect> reveal_areas;

    // Objective ids met so far this match (see the objective-tracking loop in
    // World::update); gates spawn_triggers.
    std::unordered_set<std::string> cleared_objectives;
    // What each objective needs to be "met", resolved at setup. kill_units ->
    // every `targets` unit dead; move_to_area -> a team-0 unit inside the box;
    // protect_unit -> deferred (checked only at level end, not here).
    struct ObjectiveWatch {
        std::string id, type;
        int tx = 0, ty = 0, tw = 0, th = 0;      // move_to_area box
        std::vector<EntityRef> targets;          // kill_units/protect_unit unit refs
    };
    std::vector<ObjectiveWatch> objective_watches;

    Unit* get(EntityRef r) { return r.kind == EntityKind::Unit ? units.get(r) : nullptr; }
    Building* get_building(EntityRef r) {
        return r.kind == EntityKind::Building ? buildings.get(r) : nullptr;
    }
    Resource* get_resource(EntityRef r) {
        return r.kind == EntityKind::Resource ? resources.get(r) : nullptr;
    }
    Deer* get_deer(EntityRef r) { return r.kind == EntityKind::Deer ? deer.get(r) : nullptr; }
    Projectile* get_projectile(EntityRef r) {
        return r.kind == EntityKind::Projectile ? projectiles.get(r) : nullptr;
    }

    // Resolve just the fields generic combat/AI code needs, regardless of
    // kind (mirrors accessing e.x/e.team/e.alive/... on a Python Entity
    // without caring about the concrete subclass).
    EntityCommon* common(EntityRef r);
    // generic damage application + death bookkeeping. `shake`=false suppresses
    // the cosmetic hit shudder (used by continuous fire DoT).
    void hurt(EntityRef target, double dmg, bool shake = true);
    double armor_of(EntityRef r);
    double pierce_of(EntityRef r);
    double foot_px_of(EntityRef r); // 0 for non-buildings

    // ---- spawning ----
    EntityRef spawn_unit(const std::string& name, int team, double x, double y);
    EntityRef spawn_building(const std::string& name, int team, double x, double y,
                             bool constructing = false);
    EntityRef spawn_resource(const std::string& kind, double x, double y);
    EntityRef spawn_deer(double x, double y);
    // `origin`, if given, is the exact world-space muzzle point the shot
    // visually leaves from (per-cannon offset from the shooter's own
    // position -- see unit_behavior.cpp's ship_shot_origin); defaults to
    // the shooter's own position for every other unit, same as before.
    // `target_pos`, if given, aims the shot at a fixed map POINT instead of an
    // entity (used for artillery attack-ground -- a lob shell that splashes
    // wherever it lands, no target unit needed).
    EntityRef spawn_projectile(Unit& shooter, EntityRef target,
                               std::optional<Vec2> origin = std::nullopt,
                               std::optional<Vec2> target_pos = std::nullopt);
    // A bomber's dropped ordnance: always lob+big (full mushroom, falls
    // from altitude rather than flying straight at the target) -- see
    // aircraft_behavior.cpp's is_bomber firing branch.
    EntityRef spawn_bomb(Unit& shooter, EntityRef target, bool nuke = false,
                         std::optional<Vec2> target_pos = std::nullopt);
    // A defensive structure's shot (tower/fortress/aa tower): a plain homing
    // bullet from the building toward `target`, dealing the building's attack
    // as a direct hit. See update_building's combat block.
    EntityRef spawn_building_shot(Building& shooter, EntityRef target);
    void transform_unit(EntityRef ref, const std::string& new_name);
    // Building analog of transform_unit -- re-derives combat/visual stats
    // (sprite, attack, range, reload, sight, is_aa) from `new_name`'s
    // catalog entry in place (same slot/EntityRef/position/team/hp-fraction/
    // construction state), for a building-kind upgrade tech (see
    // control.h's BUILDING_UPGRADE_MAP). Footprint/solid/is_dropoff/gather
    // point are left untouched -- every current use case (tower -> aa
    // tower) shares them exactly, so re-deriving would be a no-op anyway.
    void transform_building(EntityRef ref, const std::string& new_name);
    // `builders`, if non-empty, are assigned as this foundation's initial
    // construction crew (each must be a live, own-team gatherer, e.g. the
    // player's actually-selected civilians) instead of the fallback
    // "nearest idle civilian" search, which only ever picks one and
    // ignores selection entirely -- used for AI/scripted placement, which
    // doesn't pass any builders.
    // `assign_builders` false places the foundation but assigns NO construction
    // crew (neither the passed builders nor the idle-civilian fallback) -- used
    // by shift-queued placement, where the client instead appends a Build order
    // to each selected villager's order_queue so the foundation waits its turn.
    EntityRef place_building(const std::string& name, int team, double x, double y,
                             const std::vector<EntityRef>& builders = {},
                             bool assign_builders = true);

    // Queues a unit/age/tech item on a production building if affordable
    // (pays on enqueue). Direct port of Building.enqueue (game/entity.py),
    // moved here since it needs Control + the event bus. Emits a Sound
    // "error" + Warn event on failure (insufficient resources / pop cap).
    // `priority`: insert at the FRONT of the queue instead of the back, so
    // it starts progressing immediately instead of waiting behind whatever
    // was already queued (resets the building's in-progress percent/acc,
    // same as cancel_queue does when the front slot changes -- there's no
    // "resume where the bumped item left off" state to preserve, since
    // percent/acc always track whatever's currently at queue.front()).
    // Used by the AI's age-up-priority experiment (control_ai.cpp's
    // ai_economy, ai_variant == 1); default false leaves ordinary FIFO
    // behavior unchanged for every existing caller including the player.
    bool enqueue(EntityRef building_ref, const std::string& item, bool priority = false);

    // Building prerequisite for the team's NEXT age-up, checked inside enqueue
    // (so it gates human and AI identically) and reusable by the AI to avoid
    // banking for an age it can't yet start. Counts only COMPLETED buildings
    // the team owns. The tiers (see world.cpp for the exact name lists):
    //   -> Industrial: any 2 era-0 tech/economy/military buildings (NOT
    //      house/farm/walls/outpost/base)
    //   -> War:        any 2 Industrial-era buildings (factory/university/
    //      airbase/aa tower)
    //   -> Scientific: any 2 War-era buildings (base/fortress) OR any 1 fortress
    // Returns true once the team is already at the max era (nothing left to
    // gate).
    bool can_age_up(int team);

    // Nearest still-under-construction wall foundation of `team` with the same
    // `name` (palisade / iron wall) to (x,y) within `radius`, skipping
    // `exclude` and preferring one no other unit is already building (so a
    // build crew spreads along the line rather than stacking on one segment).
    // Returns kNullRef if none. Powers wall-build chaining: a villager that
    // finishes one segment auto-continues to the next connected one
    // (unit_behavior.cpp), and a click-drag wall assigns its crew to the
    // nearest segment (game_client.cpp), so one order raises the whole wall.
    EntityRef next_wall_segment(int team, const std::string& name, double x, double y, double radius,
                                EntityRef exclude);

    // Cancels the queue slot at `index`, refunding its full cost to the
    // building's team (enqueue already paid up front). If the cancelled
    // slot is the one currently in progress (index 0), its accumulated
    // percent/acc progress is reset too, so the next item in the queue
    // (if any) starts fresh rather than inheriting partial progress.
    bool cancel_queue(EntityRef building_ref, int index);

    void add_resource(int team, int rtype, double amount);
    // `naval` (a fishing boat) can only unload at the base or shipyard, never a
    // land house/refinery; a land gatherer never routes to a water shipyard.
    EntityRef nearest_dropoff(int team, double x, double y, int carry_type,
                               EntityRef exclude = kNullRef, bool naval = false);
    EntityRef find_by_id(uint32_t id); // linear scan across all kinds; fine for scripted/test use

    // Order-issuing setters, direct port of Unit.order_move/order_gather/
    // order_attack (game/entity.py). These are the entry points a future
    // input/session layer (Phase C) and the lockstep command stream
    // (Phase D) both funnel through -- see sim/include/sim/command.h.
    // `from_queue` is set only by advance_order_queue (unit_behavior.cpp)
    // when popping the unit's OWN Unit::order_queue -- it skips the
    // "cancel any pending queue" clear below so issuing a queued step
    // doesn't wipe the very queue it came from. Every other caller (a
    // fresh player/AI order) leaves it false, which cancels the queue --
    // an ordinary new order always supersedes whatever was queued.
    // group_speed_px: caps this unit's move at min(its own speed_px, this),
    // so several units ordered together (see GameClient::right_click_order/
    // issue_formation) move at the slowest member's pace instead of
    // scattering -- see Unit::group_speed_px. <=0 (the default) means no
    // cap, and is what every non-group call site should pass.
    void order_move(EntityRef unit, double x, double y, bool from_queue = false,
                    double group_speed_px = -1.0);
    void order_gather(EntityRef unit, EntityRef target, bool from_queue = false);
    void order_attack(EntityRef unit, EntityRef target, bool from_queue = false);

    // Shift-queue a follow-up order (see Unit::order_queue): appends unless
    // the unit already has kMaxQueuedOrders pending, in which case the
    // click is silently ignored (no error/warn cue -- matches AoE's own
    // "full queue eats the click" behavior). Actually issuing the order
    // happens later, when advance_order_queue notices the unit has gone
    // idle -- this just records it.
    static constexpr size_t kMaxQueuedOrders = 10;
    bool queue_order(EntityRef unit, QueuedOrder order);
    // Amphibious unload: disgorge a transport's cargo onto land at (wx,wy).
    // Valid only when the ship is within ~1 tile of land AND the drop point
    // is a shoreline tile (on land, touching an ocean tile) -- returns false
    // (no units moved) otherwise, so the client can play a denied cue.
    bool unload_transport(EntityRef ship, double wx, double wy);
    // pred receives the resolved EntityRef alongside its common fields, so
    // callers needing kind-specific data (e.g. Resource::name) can look it
    // up themselves (get_resource(ref), etc.) inside the predicate.
    EntityRef nearest(double x, double y, double radius,
                       const std::function<bool(EntityRef, EntityCommon&)>& pred);

    // ---- per-tick ----
    void update(double dt);

    // Rebuilds occupied-tiles + spatial grid immediately after scenario
    // setup, so queries work correctly before the first update() tick
    // (matches scenario.py's explicit grid.rebuild()/control.recompute()
    // calls at the end of new_skirmish).
    void prime() { rebuild_occupied(); rebuild_spatial_grid(); control.recompute(*this); }
    // Public: refresh ONLY the building collision sets (footprint_clear/occupied),
    // no resource/grid/recompute -- for cheap incremental use during bulk building
    // placement (see stress_scenario.cpp). rebuild_occupied() (private) calls it.
    void rebuild_building_tiles();

    Control& control;
    const DataStore& data;
    const Bonuses& bonuses;
    Rng& rng;
    EventBus& events;

private:
    std::vector<std::pair<int, int>> building_tile_list(const std::string& name, double cx,
                                                         double cy) const;
    void rebuild_occupied();
    void rebuild_spatial_grid();

    std::unordered_set<int64_t> occupied_;      // tiles covered by SOLID buildings
    std::unordered_set<int64_t> resource_tiles_;
    // Subset of resource_tiles_ that are TREES/palms (not ore/berries). Units
    // whose leader is Li Zongren (Unit::phase_trees) treat these as passable.
    std::unordered_set<int64_t> tree_tiles_;
    // Tiles covered by an IMPASSABLE Decoration (see decoration_kinds()) --
    // passable ones (pebbles, rubble, hedge, etc.) never enter this set at
    // all, same "only what actually blocks" convention as resource_tiles_.
    std::unordered_set<int64_t> decoration_tiles_;

    // Axis-aligned bounding boxes (x0,y0,x1,y1) of currently-alive SOLID
    // buildings, rebuilt alongside occupied_/etc in rebuild_occupied().
    // passable() checks these directly (not the coarser per-TILE occupied_
    // set) so units can walk right up to a building's true pixel edge
    // instead of being stopped by whichever whole TILE-sized (32px) cell
    // happens to touch that edge. Every footprint is now an exact multiple
    // of TILE (see building_wh()) and snapped onto the TILE grid, so this
    // mostly agrees with occupied_ anyway -- it's kept for the sub-tile
    // precision that still matters for units approaching at an angle.
    std::vector<std::array<double, 4>> solid_building_rects_;
    // Same idea as solid_building_rects_ but for EVERY alive building
    // (matches building_tiles_'s "any building" scope, not just solid
    // ones -- a farm shouldn't overlap a house either). footprint_clear()
    // checks these directly instead of the coarser building_tiles_ set for
    // the same reason: two buildings placed with a real gap between them
    // could still share a whole 32px terrain tile that only partially
    // touches each footprint, wrongly rejecting a genuinely free spot.
    std::vector<std::array<double, 4>> all_building_rects_;
    // Foundations that are solid-by-type but haven't been started yet, so
    // they're currently walk-through. Physically passable, but off-limits
    // to route planning -- see passable_planning().
    std::vector<std::array<double, 4>> pending_foundation_rects_;

    Unit make_unit(const std::string& name, int team, double x, double y) const;
};

} // namespace ww::sim
