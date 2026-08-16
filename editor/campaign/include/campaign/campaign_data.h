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
    // resource stockpile for this player's battlefield presence -- set on
    // the editor's Players tab. Defaults mirror sim::Team's own defaults
    // (see game's sim/include/sim/control.h) so an unedited player
    // starts exactly like a normal skirmish team.
    int era = 0;
    double food = 200, wood = 200, oil = 100, iron = 100;
    // AI behaviour preset for this player when the level is played (ignored for
    // the human P1 slot): "default" | "passive" | "defensive" | "balanced" |
    // "aggressive" | "rusher". "default" = the normal skirmish AI. Set on the
    // editor's Units-mode player strip.
    std::string ai_behavior = "default";
};

// Default battlefield grid size for a new level's editable map -- a flat
// placeholder (all grass unless painted otherwise) rather than the main
// game's scaled random-map generation, so placement can ship now;
// "generate a random map onto this grid" is a natural later extension
// once map types exist for it to choose from, not built yet. Actual size
// is per-level (Level::grid_size below), matching the main game's Random
// Map Setup presets (Tiny/Normal/Large/Huge) -- this is just the fallback
// for a level that predates that selector.
constexpr int kLevelGridSize = 32;

// One terrain tile worth recording (grass with no resource -- the vast
// majority of tiles -- is the implicit default for every (tx,ty) not
// listed here, keeping a mostly-empty level's JSON small). `base` and
// `resource` are independent: a water tile can also carry a fish resource
// (the fish sits IN the water, it doesn't replace it), matching how the
// sim treats terrain (a land/water tile grid) and resources (spawned
// entities sitting on top of a tile) as two separate things.
// `base`: "" (grass, default) or "water".
// `resource`: "" (none, default) or one of the sim's existing hardcoded
// resource-kind strings (see scenario.cpp's spawn_resource/spawn_deer):
// "tree", "palm", "berry", "oil", "iron", "deer", "fish". Land resources
// only make sense with base=="" ; "fish" only makes sense with
// base=="water" -- enforced by the editor's placement logic, not here.
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
    // needs to reference it (see the editor's finalize_objective_area)
    // rather than backfilled for every existing level on load. Kept LAST
    // (not first) so the editor's existing positional
    // `{type, player_index, tx, ty}` aggregate-inits (place_entity_at)
    // still compile and just default this to "" -- exactly the "assigned
    // lazily" behaviour above.
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
// hidden one is. In short: hidden -> event-trigger only; shown ->
// event-trigger AND counts toward win/lose.
// Not yet consumed by the live game (same "schema-only for now" status as
// MapEvent below) -- wiring up actual kill/reach-area/protect tracking
// and a campaign win/defeat trigger is a follow-up, since no "play this
// level" flow exists anywhere yet (see this repo's own README/CLAUDE.md).
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
//     the map (same tool Objective's kill_units/move_to_area areas use,
//     see the editor's finalize_event_area) -- every Blocks-category
//     terrain tile inside it (fence, hedge, rubble, crates, etc., see
//     editor.cpp's kTerrainKinds) acts as a locked gate: impassable until
//     `unlock_objective_id` (an Objective::id) is met, then open. This is
//     NOT a snapshot -- which tiles count is whatever Blocks-category
//     terrain is actually inside the area at play time, live, same as
//     move_to_area above.
//   "dormant": area_t* is the tile rectangle (same tool as "gate"); every
//     unit standing inside it does not move at all until
//     `unlock_objective_id` is met. Also a live check, not a snapshot --
//     units that wander into or out of the area afterward gain/lose the
//     effect accordingly, unlike Objective's kill_units capture.
//   "resources": no tx/ty, no area -- grants `res_food`/`res_wood`/
//     `res_oil`/`res_iron` (each defaulting to 0, independently settable)
//     to the player once `unlock_objective_id` is met. A one-time grant,
//     not a rate -- these are flat amounts added once, same units as
//     LevelPlayer's own food/wood/oil/iron starting stockpile.
// `unlock_objective_id` (gate/dormant/resources only) empty means "none"
// -- deliberately NOT the same as "always open"/"always active"/"already
// granted": no objective assigned means the effect never fires. If the
// referenced objective is ever deleted (see the editor's
// "delete_objective" click handler), every event pointing at it has this
// field reset back to "" rather than left dangling, so a stale id can
// never silently linger.
// Not yet consumed by the live game (same "schema-only for now" status as
// Objective above) -- wiring up the actual collision-trigger, gate-
// blocking, dormant-unit, and resource-grant behaviour is a follow-up,
// since no "play this level" flow exists anywhere yet.
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
    // DEPRECATED (dormant): the old single trigger LINE, superseded by area2_*.
    // Kept only so older campaigns still load. trig_tx0 == -1 = no line.
    int trig_tx0 = -1, trig_ty0 = 0, trig_tx1 = -1, trig_ty1 = 0;
};

// A single mission: where it sits on the Europe map, who's involved, its
// briefing text, and its battlefield (flat grid + terrain features +
// starting units/buildings + map events + objectives).
struct Level {
    std::string id; // stable, auto-generated -- for future save/progress tracking
    std::string name;
    std::string description; // multi-line: up to a paragraph of background/briefing text
    double loc_x = 0.5, loc_y = 0.5; // fractional position on spr_europe_map, 0..1 each
    std::vector<LevelPlayer> players; // players[0] is always the locked default P1, see above
    // Battlefield grid is grid_size x grid_size tiles -- matches the main
    // game's Random Map Setup presets (Tiny/Normal/Large/Huge = map_size
    // 28/40/52/64, world cols = map_size*2, see sim/include/sim/scenario.h),
    // chosen at level-creation time, not changeable afterward (existing
    // terrain/placements would no longer make sense against a different
    // grid size).
    int grid_size = kLevelGridSize;
    // Starting fog-of-war state for this level, mirroring single-player's
    // Standard/No fog/Revealed option (sim SkirmishSettings::reveal_mode):
    // 0 = Standard fog, 1 = No fog (whole map explored), 2 = Revealed (full
    // vision). Applied by the engine when the level is played.
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
    // Optional per-level briefing image: a relative path (under the editor's
    // assets dir, e.g. "campaign_images/briefing_3.png") to a PNG the designer
    // uploaded, shown below the objectives. Empty = none.
    std::string briefing_image;
    // Optional named AI profile for BESPOKE, campaign-specific AI behaviour
    // (e.g. "jena_defensive"): the engine's AI branches on it, so a campaign
    // can get hand-written AI without touching the generic skirmish AI. Empty
    // = no override (use each player's ai_behavior preset instead).
    std::string ai_profile;
    std::vector<TerrainFeature> terrain; // sparse -- absent tile = grass, no resource
    std::vector<PlacedEntity> units;
    std::vector<PlacedEntity> buildings;
    std::vector<Objective> objectives;
    std::vector<MapEvent> events;
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
