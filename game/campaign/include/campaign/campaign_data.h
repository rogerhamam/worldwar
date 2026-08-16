#pragma once
#include <string>
#include <vector>

// Campaign data model + JSON load/save, shared by the campaign editor and
// the main game's in-game campaign-select screen (GameClient's
// Screen::CampaignList, see client/src/menu). Deliberately plain data (no
// sim/rendering dependencies) so both programs can include it freely --
// see data/campaigns/*.json for the on-disk format this
// reads/writes.
namespace ww::campaign {

// One player slot in a level -- up to 8, mirroring the original
// GameMaker game's civ0..civ3/ally0..ally3 fields (assets/gmk/objects/
// obj_map_icon/Draw.gml), but as a resizable list instead of 4 fixed
// slots. `team` groups slots into sides (1-4, matching the main game's
// skirmish team selector, see Team::ally in sim/include/sim/control.h):
// slots sharing a value fight on the same side. Slot 0 is always the
// campaign's own civilisation (Campaign::civ) on team 1 -- locked/
// non-removable, enforced by the editor, not by this struct.
struct LevelPlayer {
    int civ = 0;
    int team = 1;
    // Starting era (0=Victorian, 1=Industrial, 2=War, 3=Scientific) and
    // resource stockpile, set on the campaign editor's Players tab.
    // Defaults mirror sim::Team's own defaults (sim/include/sim/control.h)
    // so an unedited player starts exactly like a normal skirmish team.
    // Not yet consumed by new_from_level()/scenario.cpp -- every campaign
    // level still spawns teams at the sim's own hardcoded era-0/default-
    // resources start regardless of these fields; wiring them in is a
    // follow-up, this is just schema sync with the editor's own copy.
    int era = 0;
    double food = 200, wood = 200, oil = 100, iron = 100;
    // AI behaviour preset (editor's Units-mode player strip): "default" |
    // "passive" | "defensive" | "balanced" | "aggressive" | "rusher".
    // "default" = the normal skirmish AI. Read by new_from_level -> Team.
    std::string ai_behavior = "default";
};

// Default grid size for a level whose JSON predates the per-level size
// picker (editor's draw_new_level) -- Level::grid_size
// is the actual source of truth for every level now, this is only the
// fallback level_from_json uses if a file is missing the field entirely.
constexpr int kLevelGridSize = 32;

// One non-grass terrain tile (grass is the implicit default for every
// (tx,ty) not listed here, keeping a mostly-empty level's JSON small).
// `base` and `resource` are independent slots on the same tile (a water
// tile can still carry a resource on top, e.g. fish) rather than one
// mutually-exclusive `kind` string -- see
// editor/src/editor.cpp's kTerrainKinds for the full,
// currently-authorable set: `base` is "" (grass), "water", or one of the
// ground-texture variants ("dirt"/"brown dirt"/"gravel"/"sand"/
// "pavement"); `resource` is "" or one of the gatherable resources
// ("tree"/"palm"/"berry"/"oil"/"iron"/"deer"/"fish") or one of the
// (currently editor-only, not yet wired into a live match -- see World's
// lack of a campaign-level loader) decoration/obstacle kinds (rubble,
// stones, rocks, trees, fences, hedge, bush, crate, flames, pebbles, the
// 5 destroyed-building variants, and the named vehicle-wreckage rubbles).
struct TerrainFeature {
    int tx = 0, ty = 0;
    std::string base;
    std::string resource;
};

// A hand-placed starting unit or building, owned by one of Level::players
// (by index, not civ -- so it stays correct if a player's civ is changed
// later). Position is a grid tile, same as TerrainFeature.
struct PlacedEntity {
    std::string type; // catalog unit/building name, e.g. "rifleman", "base"
    int player_index = 0;
    int tx = 0, ty = 0;
    // Stable, auto-generated (see next_entity_id) -- lets an Objective (see
    // below) reference this exact unit even if others are placed/removed
    // around it later and vector indices shift. Empty for any entity placed
    // before this field existed; assigned lazily the first time something
    // needs to reference it rather than backfilled for every existing
    // level on load. Kept LAST (not first) so the editor's own positional
    // aggregate-inits still compile and just default this to "". Schema
    // sync with the editor's own copy.
    std::string id;
};

// A mission objective -- placed/edited on the editor's Objectives tab.
// `area_t*` is the tile rectangle the editor's area tool (click-drag-
// release on the battlefield canvas) last drew for it; what it MEANS
// depends on `type`:
//   "kill_units": `target_unit_ids` is a ONE-TIME SNAPSHOT of which
//     PlacedEntity::id's were sitting inside the area at the moment it was
//     drawn -- not a live query, so units placed/moved into the same
//     rectangle afterward do NOT retroactively join the objective;
//     redrawing the area is what re-captures the snapshot. Intended rule:
//     met once every id in target_unit_ids is dead.
//   "move_to_area": no snapshot -- `target_unit_ids` is unused/empty for
//     this type. Intended rule: met once any of the player's own units is
//     standing inside the area, checked live during play, not at
//     drawing time (there's nothing to capture up front).
//   "protect_unit": `target_unit_ids` is a snapshot, same capture rule as
//     kill_units. Intended rule is the INVERSE of kill_units though: met
//     (successfully protected) once the level ends with every id in
//     target_unit_ids still alive; losing even one before then FAILS it
//     instead. A failed, non-`hidden` objective is what ends the campaign
//     in defeat -- see `hidden` below.
// `hidden`: if true, this objective is never checked for campaign win/
// lose at all -- met or failed, it only matters as something an event
// (see MapEvent::unlock_objective_id) can react to. A non-hidden
// ("shown") objective is the opposite: every one of them has to be met
// (and none of them failed, for protect_unit) to win the campaign, IN
// ADDITION to still being usable as an event trigger the same way a
// hidden one is.
// Not yet consumed here (same "schema-only for now" status as MapEvent
// below) -- wiring up actual kill/reach-area/protect tracking and a
// campaign win/defeat trigger is a follow-up, since no "play this level"
// flow exists anywhere yet. Schema sync with the editor's own copy.
struct Objective {
    std::string id; // stable, auto-generated, see next_objective_id
    std::string name;
    std::string type = "kill_units"; // "kill_units", "move_to_area", or "protect_unit"
    bool hidden = false; // see the comment above -- false ("shown") is the default for a new objective
    int area_tx = 0, area_ty = 0, area_tw = 0, area_th = 0; // tile rect; area_tw/th == 0 means "not drawn yet"
    // kill_units/protect_unit only -- PlacedEntity::id's captured inside the area
    std::vector<std::string> target_unit_ids;
};

// A placed map event -- created/edited on the editor's Events tab, using
// the same create/list/select-to-edit UI as Objective (see above):
// "+ Create Event" adds one, the left sidebar lists every event in this
// level, clicking a row selects it for editing. `type` picks which of the
// type-specific fields below apply:
//   "message": tx/ty is a single tile, set by clicking the map while this
//     event is selected (tx == -1 means "not placed yet", the default for
//     a freshly-created one -- 0 is a valid real tile coordinate, so it
//     can't double as "unset"). Shows `text` as a one-line notification
//     when a player unit collides with its tile.
//   "gate": area_t* is the tile rectangle, drawn by click-drag-release on
//     the map (same tool Objective's kill_units/move_to_area areas use)
//     -- every Blocks-category terrain tile inside it (fence, hedge,
//     rubble, crates, etc.) acts as a locked gate: impassable until
//     `unlock_objective_id` (an Objective::id) is met, then open. This is
//     NOT a snapshot -- which tiles count is whatever Blocks-category
//     terrain is actually inside the area at play time, live, same as
//     move_to_area above.
//   "dormant": area_t* is the tile rectangle (same tool as "gate"); every
//     unit standing inside it does not move at all until
//     `unlock_objective_id` is met. Also a live check, not a snapshot.
//   "resources": no tx/ty, no area -- grants `res_food`/`res_wood`/
//     `res_oil`/`res_iron` (each defaulting to 0, independently settable)
//     to the player once `unlock_objective_id` is met. A one-time grant,
//     not a rate -- flat amounts added once, same units as LevelPlayer's
//     own food/wood/oil/iron starting stockpile.
// `unlock_objective_id` (gate/dormant/resources only) empty means "none"
// -- deliberately NOT the same as "always open"/"always active"/"already
// granted": no objective assigned means the effect never fires. If the
// referenced objective is ever deleted, every event pointing at it has
// this field reset back to "" rather than left dangling. Not yet
// consumed here (same "schema-only for now" status as Objective above) --
// wiring up the actual collision-trigger, gate-blocking, dormant-unit,
// and resource-grant behaviour is a follow-up, since no "play this level"
// flow exists anywhere yet. Schema sync with the editor's own copy.
// A tile-space rectangle. tw/th == 0 means "empty / not drawn yet".
struct TileRect { int tx = 0, ty = 0, tw = 0, th = 0; };

struct MapEvent {
    std::string id; // stable, auto-generated, see next_event_id
    std::string name; // free-typed label, shown in the sidebar list (like Objective::name)
    std::string type = "message"; // "message", "gate", "dormant", "resources", or "spawn"
    int tx = -1, ty = 0; // message/resources marker tile; tx == -1 means "not placed yet"
    std::string text; // author message shown when the trigger fires (message/spawn/dormant/resources)
    int area_tx = 0, area_ty = 0, area_tw = 0, area_th = 0; // gate; dormant "units" box; spawn box. tw/th == 0 = not drawn
    std::string unlock_objective_id; // gate/spawn -- Objective::id gating the trigger, "" = none
    double res_food = 0, res_wood = 0, res_oil = 0, res_iron = 0; // resources: one-time grant on pickup
    // "spawn" only -- units created inside area_* when the trigger fires (its
    // condition is unlock_objective_id being met, else a player unit entering area_*).
    std::string spawn_unit; // unit type (catalog name); "" = none chosen yet
    int spawn_count = 0;    // how many to spawn
    int spawn_player = 1;   // owning player index (into Level::players)
    // "dormant" only -- box #2, the tripwire rectangle. When a player unit enters
    // area2_*, the units inside area_* wake and charge area2_* (tw/th == 0 = not drawn).
    int area2_tx = 0, area2_ty = 0, area2_tw = 0, area2_th = 0;
    // "dormant" only -- extra rectangles revealed to the player's vision when the
    // tripwire (area2_*) fires. Any number.
    std::vector<TileRect> los_areas;
    // "dormant" only -- one or more tripwire boxes; a player unit entering ANY
    // of them wakes the group. Supersedes the single area2_* (still loaded as
    // one trip box for older campaigns that have no trip_areas).
    std::vector<TileRect> trip_areas;
    // DEPRECATED (dormant): the old single trigger LINE, superseded by area2_*.
    // Kept only so older campaigns still load. trig_tx0 == -1 = no line.
    int trig_tx0 = -1, trig_ty0 = 0, trig_tx1 = -1, trig_ty1 = 0;
};

// A per-level custom unit: a clone of a base catalog unit with overridden
// stats and, optionally, a custom sprite. Placed units reference it by `name`
// (PlacedEntity::type == CustomUnit::name); the engine injects it into the
// catalog at match load (campaign Match ctor). Authored in the editor's
// Custom Units panel.
struct CustomUnit {
    std::string name;               // unique display name + id within the level
    std::string base = "muscateer"; // template catalog unit to clone stats/behaviour from
    double hp = 100, attack = 10, armor = 0; // stat overrides
    std::string sprite;             // optional custom sprite name (no .png); "" = use base's
    int sprite_w = 0, sprite_h = 0; // uploaded sprite pixel dims (drawn as one static frame)
};

// A single mission: where it sits on the Europe map, who's involved, its
// briefing text, and its battlefield (flat grid + terrain features +
// starting units/buildings + map events). Deliberately does NOT yet
// include win/lose conditions.
struct Level {
    std::string id; // stable, auto-generated -- for future save/progress tracking
    std::string name;
    std::string description; // multi-line: up to a paragraph of background/briefing text
    double loc_x = 0.5, loc_y = 0.5; // fractional position on spr_europe_map, 0..1 each
    // Tiny/Normal/Large/Huge presets (editor's
    // draw_new_level), matching the main game's Random Map Setup sizes --
    // chosen once at level creation and fixed after (changing it would
    // invalidate any terrain/units/buildings already placed against the
    // old dimensions). Falls back to kLevelGridSize for a level saved
    // before this field existed.
    int grid_size = kLevelGridSize;
    // Starting fog-of-war state (mirrors SkirmishSettings::reveal_mode):
    // 0 = Standard fog, 1 = No fog (map explored), 2 = Revealed. Set in the
    // campaign editor; applied by new_from_level. Falls back to 0 for a level
    // saved before this field existed.
    int reveal_mode = 0;
    // Optional whole-level age cap: the highest era (0 Victorian / 1 Industrial
    // / 2 War / 3 Scientific) any player -- human or AI -- may advance to.
    // -1 = no cap. Enforced by the engine (Team::max_era) via new_from_level.
    int max_age = -1;
    // Optional per-level STARTING age: the era (0 Victorian / 1 Industrial /
    // 2 War / 3 Scientific) every player -- human and AI -- begins the level
    // already advanced to. Default 0 = Victorian (unchanged legacy behavior).
    // Applied by the engine (Team::era) in new_from_level; clamped to max_age
    // if a cap is set.
    int start_age = 0;
    // Whole-level population cap: the max_pop ceiling applied to every player
    // (human and AI). Clamped to 50..200 by the engine (Match ctor). Default 100
    // matches prior campaign behavior.
    int pop_cap = 100;
    // Optional per-level briefing image (relative path under the editor's
    // assets dir); set in the campaign editor, shown below the objectives.
    std::string briefing_image;
    // Optional named AI profile for bespoke campaign-specific AI (e.g.
    // "jena_defensive"); the engine AI branches on it so skirmish AI is
    // untouched. Empty = use each player's ai_behavior preset.
    std::string ai_profile;
    std::vector<LevelPlayer> players; // players[0] is always the locked default P1, see above
    std::vector<TerrainFeature> terrain; // sparse -- absent tile = grass
    std::vector<PlacedEntity> units;
    std::vector<PlacedEntity> buildings;
    std::vector<Objective> objectives;
    std::vector<MapEvent> events;
    std::vector<CustomUnit> custom_units; // per-level custom (base-template) units
};

struct Campaign {
    std::string name;
    int civ = 0; // which civ this campaign follows, index into ww::menu::civ_names()
    std::vector<Level> levels;
    std::string file_path; // absolute path this was loaded from / will be saved to; empty until first save
};

// Scans <data_dir>/campaigns/*.json and loads each as a Campaign. A file
// that fails to parse is skipped (not fatal) so one corrupt campaign
// doesn't hide every other one.
std::vector<Campaign> load_all_campaigns(const std::string& data_dir);

// Writes campaign.file_path (creating <data_dir>/campaigns/ if needed).
// If file_path is still empty (a brand new campaign that's never been
// saved), derives one from the campaign name (lowercased, non-alphanumeric
// runs collapsed to underscores) and disambiguates against any existing
// file of the same name, then fills in campaign.file_path.
void save_campaign(Campaign& campaign, const std::string& data_dir);

// Generates a level id that isn't already used by any level in
// `campaign` -- called when a new level is added.
std::string next_level_id(const Campaign& campaign);

// Generates an entity id that isn't already used by any unit or building in
// `lvl` -- called lazily (see PlacedEntity::id's comment) rather than at
// placement time, so levels saved before this field existed don't all need
// re-saving just to gain ids.
std::string next_entity_id(const Level& lvl);

// Generates an objective id that isn't already used by any objective in
// `lvl` -- called when a new objective is created.
std::string next_objective_id(const Level& lvl);

// Generates a map event id that isn't already used by any event in `lvl`
// -- called when a new event is created.
std::string next_event_id(const Level& lvl);

} // namespace ww::campaign
