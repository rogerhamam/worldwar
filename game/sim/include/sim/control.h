#pragma once
#include "sim/bonuses.h"
#include "sim/catalog.h"

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ww::sim {

class World;

// ---- static gameplay tables, direct port of the top of game/control.py ----
extern const std::unordered_map<std::string, int> POP_COST;
extern const std::unordered_map<std::string, std::vector<std::string>> PRODUCTION;
extern const std::vector<std::string> AGE_ITEMS; // era 1,2,3 advance items
extern const std::array<std::string, 4> RES_KEYS;

// "<x> upgrade" -> (old unit name or nullopt, new unit name)
extern const std::map<std::string, std::pair<std::optional<std::string>, std::string>> UPGRADE_MAP;
// Same idea as UPGRADE_MAP but for BUILDINGS -- "<tech>" -> (old building
// name, new building name). Unlike units there's no "trainable straight
// into the upgraded form" case (a building always starts as the old form
// and converts once researched), so no optional/nullopt slot is needed.
extern const std::map<std::string, std::pair<std::string, std::string>> BUILDING_UPGRADE_MAP;
extern const std::set<int> CAMEL_CIVS;
extern const std::set<std::string> CAMEL_UNITS;
extern const std::set<std::string> OTTOMAN_ONLY;
extern const std::unordered_map<std::string, int> CIV_ONLY_UNITS;
extern const std::set<std::string> UNIQUE_UNITS;
extern const std::map<std::string, std::set<int>> CIV_UPGRADE_OWNER;
extern const std::unordered_map<std::string, std::string> UNIT_REQUIRES; // new -> upgrade tech
extern const std::unordered_map<std::string, std::string> UNIT_UPGRADE;  // old -> upgrade tech
extern const std::map<std::pair<int, std::string>, std::string> CIV_UNIT_SUB;
extern const std::unordered_map<std::string, std::vector<std::string>> TECH_PREREQ;
extern const std::unordered_map<std::string, int> TECH_ERA;
extern const std::unordered_map<std::string, int> UNIT_ERA;
extern const std::unordered_map<std::string, int> BUILDING_ERA;
extern const std::vector<std::string> BUILDABLE;

int pop_cost(const std::string& name);
// Pop-space a unit occupies aboard a transport ship. Normally == pop_cost,
// but the UK's Royal Marines pack in at half a slot each (0.5), so a transport
// carries twice as many of them.
double transport_cost(const std::string& name);

struct TradeRates {
    double food_buy = 100, food_sell = 100;
    double wood_buy = 100, wood_sell = 100;
    double iron_buy = 100, iron_sell = 100;
};

struct Team {
    std::unordered_map<std::string, double> res = {
        {"food", 200}, {"wood", 200}, {"oil", 100}, {"iron", 100}};
    int cap = 0, pop = 0, era = 0, civ = 0, leader = 0;
    // Campaign whole-level age cap (Level::max_age): `era` may never advance past
    // this. -1 = no cap. Set by new_from_level; skirmish leaves it -1. Enforced
    // in World::enqueue (age-up items rejected once era >= max_era).
    int max_era = -1;
    bool alive = true;
    bool is_ai = false;
    std::set<std::string> tech;
    std::string strategy = "land";
    // Campaign AI controls (default = normal skirmish AI, so skirmish is
    // untouched). ai_behavior is a per-player preset ("passive"/"defensive"/
    // "balanced"/"aggressive"/"rusher"); ai_profile is an optional bespoke,
    // per-level named profile the AI branches on. Set by new_from_level from
    // the campaign Level; empty/"default" elsewhere. See control_ai.cpp.
    std::string ai_behavior = "default";
    std::string ai_profile;
    // Skirmish "Easy"/"Normal"/"Hard" (0/1/2, matching MenuController's
    // kDifficultyNames index -- see SkirmishSettings::difficulty and
    // new_skirmish, which copies it in for every team). Only consulted by
    // is_ai teams. Deliberately doesn't touch resource/cost numbers -- it
    // scales how FAST this team's AI re-evaluates its economy/build/research/
    // army decisions (ai_tick_accum below) and how eagerly it spends into
    // research, so Hard plays the same script better/faster rather than
    // just being handed bigger numbers. Campaign levels never set this, so
    // it stays 1 (Normal) there -- identical cadence to before this existed.
    // 3 == "Hardest", which is different in kind from 0-2. Those three only
    // change how FAST this team re-evaluates its decisions -- same script, no
    // stat cheating, which is why they are described as skill. Hardest is
    // openly a handicap on top of Hard's cadence: units train 50% faster
    // (building_behavior.cpp) and every technology of the current era is
    // granted free and instantly (Control::grant_era_techs). Map knowledge
    // needs no code -- the AI already reads the whole map with no fog at all
    // (see ai_read_enemy), so "fully scouted" is its permanent state on every
    // difficulty; fog only ever tracked team 0.
    int difficulty = 1;
    // One-shot latch: the Hardest grant of the STARTING era's techs has been
    // made. Later eras are granted on age-up instead (building_behavior.cpp).
    bool hardest_seeded = false;
    // Self-play/tournament lever (see the AI-arena plan): 0 is always the
    // current, shipped baseline logic; a nonzero value gates ONE candidate
    // change at a time behind an `if (td.ai_variant == N)` branch at
    // whichever decision point is being experimented with, so a single
    // in-process match can pit "current baseline" (0) directly against
    // "candidate" (1) head-to-head. Once a candidate provably wins enough
    // to be promoted, its branch becomes the new unconditional default and
    // this goes back to being all-0 until the next experiment. Skirmish/
    // campaign both default to 0 -- untouched unless a tournament harness
    // sets it explicitly.
    int ai_variant = 0;
    // Per-team decision-cadence accumulator (control_ai.cpp's ai_tick):
    // real seconds since this team's economy/build/research/train logic last
    // ran, compared each call against a difficulty-scaled interval. Separate
    // from Control::ai_timer_, which is just the shared heartbeat that
    // drives how often ai_tick itself gets a chance to check every team.
    double ai_tick_accum = 0.0;
    // Offensive-push hysteresis (ai_variant == 1 candidate, control_ai.cpp):
    // once the force has crossed attack_threshold and the push begins,
    // stays true so the push KEEPS going with whatever's left after a
    // costly battle thins the force back below that threshold -- baseline
    // has no hysteresis at all (drops the push the instant force size dips,
    // even mid-victory), which a diagnostic replay showed is why matches
    // routinely stalled for 20+ sim-minutes with neither base ever taking a
    // scratch of damage. Resets to false only once the force is fully wiped
    // (needs to rebuild before committing again).
    bool ai_committed = false;
    // Amphibious invasion (control_ai.cpp's ai_amphibious): when the enemy is
    // across water, the transport ship currently loading/ferrying the land army.
    // Tracked so the AI commits to ONE boat run rather than re-picking a ship
    // every tick. Cleared when the ship sinks or is otherwise lost.
    EntityRef ai_transport = kNullRef;
    // ---- Map-derived Victorian AI plan (control_ai.cpp's ai_assess_map) ----
    // Computed ONCE, the first time this team is serviced by ai_tick, from the
    // real map geometry around its base, then drives the whole Victorian-era
    // economy/build/train plan. Only skirmish AI opts in (ai_map_derive below);
    // campaign AI leaves this at its defaults so its hand-authored ai_behavior/
    // ai_profile are untouched.
    struct AiPlan {
        bool assessed = false;
        // Map-derived (with a small random nudge), NOT the old fully-random
        // ai_behavior roll: "aggressive" | "defensive" | "boom" | "naval".
        std::string playstyle = "boom";
        double enemy_dist = 0.0;  // px to nearest enemy base at assess time
        double water_score = 0.0; // 0..1 fraction of nearby tiles that are water
        bool can_fish = false;    // enough water + a fish shoal near the base
        // WHOLE-MAP versions of the two above, as opposed to water_score/
        // can_fish which only look at the pocket around this team's base.
        // Team::strategy is set to "navy" for every team on any map the
        // generator flagged want_water (see scenario.cpp), which is far too
        // coarse a signal to spend 5 shipyards on -- a map can carry a single
        // coastal strip, or a handful of inland lakes, and still leave a dock
        // with nothing to fish and nowhere for a warship to matter. These are
        // measured once in ai_assess_map from the real terrain/resource grid.
        double map_water_frac = 0.0; // 0..1 fraction of ALL tiles that are water
        bool map_has_fish = false;   // at least one live fish shoal anywhere on the map
        // The shipyard gate: this map supports a navy at all. Fish give a dock
        // an economy (fishing boats); a genuinely wet map gives it a war role.
        // Neither -> shipyards are a pure waste of wood, so the build order
        // skips them entirely (the lone exception being a team that needs a
        // transport to reach an enemy it can't walk to -- see ai_build).
        bool naval_viable = false;
        // Whether a ground unit could reach the enemy base overland (no water
        // barrier). Drives factory-vs-airbase: tanks are only worth building
        // when they can actually reach the enemy; on a pure island an airbase
        // (planes cross water) is the War-prereq instead. Defaults true (build
        // factory) when there's no enemy to check against.
        bool land_to_enemy = true;
        int vil_goal = 40;        // target civilian count for this plan
        int fish_goal = 0;        // target fishing-boat count (naval/water maps)
        // A single natural choke near the base a palisade run can seal (world
        // px endpoints). Only set when one was found (wall_planned).
        bool wall_planned = false;
        bool wall_built = false;
        double wx0 = 0, wy0 = 0, wx1 = 0, wy1 = 0;
        // Cosmetic scout: one roaming unit that sweeps map waypoints in era 0.
        EntityRef scout = kNullRef;
        int scout_wp = 0;
    };
    AiPlan ai_plan;
    // The ONE production hold left under the tempo doctrine (control_ai.cpp's
    // ai_economy): true only while the next age is genuinely within reach --
    // can_age_up satisfied, the age not already queued, and the stockpile
    // already most of the way to its cost. While set, ai_train refuses a unit
    // that would spend below the age cost IN THE RESOURCES THAT AGE NEEDS; a
    // unit paid for in anything else is unaffected, as is anything bought while
    // the team is below its defensive floor. Villager production is NEVER held
    // by it. Transient (recomputed every economy pass), not persisted.
    bool ai_banking = false;
    // Food held back for the NEXT VILLAGER, in the same spirit as ai_banking
    // above but for the economy rather than the age. Set by ai_economy to the
    // town centre's food gate while the team is still below its villager goal,
    // and zero otherwise (goal met, or the workforce is already saturated).
    //
    // Without it the villager line stalls whenever military production happens
    // to drain food below the gate on the same pass -- which got materially
    // more common once units train 20% faster and a fixed share of villagers
    // is held on wood, because both changes make the team spend its food
    // sooner. ai_train refuses a FOOD-COSTED unit that would spend below this
    // (units paid in oil/iron are unaffected, and the defensive floor is exempt
    // exactly as it is for banking), so "constant villager production" survives
    // contact with a busy barracks. Transient, recomputed every economy pass.
    double ai_vil_reserve = 0.0;
    // RETIRED, 2026-08-13: always false. This was the "grow the economy first,
    // suppress offensive military" ramp, and it is the specific mechanism that
    // made the AI harmless in the opening -- the player's report was that two
    // tanks could end the game before it fielded an army. Kept as a field
    // because ai_train still reads it; see the tempo doctrine in ai_economy.
    bool ai_boom_phase = false;
    // ---- vestiges of the retired reserved-production throttle ---------------
    // The AI used to cap military spending at a FRACTION of food income
    // (ai_intensity), accruing it into ai_mil_budget and debiting per unit --
    // which meant any income above that fraction had nowhere to go and simply
    // accumulated, so teams ended matches sitting on 8,000-30,000 food they had
    // refused to spend. That is gone: production is limited only by
    // affordability now, and ai_mil_budget is held wide open.
    //
    // The fields stay because ai_economy still computes ai_intensity (it is a
    // useful published read of how hard this team is being pressed, used by
    // nothing else today) and ai_food_mark still tracks income between passes.
    double ai_intensity_jitter = 0.0;
    double ai_intensity = 0.4;
    double ai_food_mark = -1.0; // -1 = uninitialised
    double ai_mil_budget = 0.0;
    // Enemy MILITARY units currently within ~24 tiles of this team's base --
    // the raw count behind ai_intensity's pressure bump, kept separately so
    // ai_build can react to being attacked too (it's what lets an
    // economy-first plan drop everything and put up a barracks when it's
    // actually being rushed, rather than only reacting via the spend rate).
    // Recomputed every economy pass, which runs immediately before ai_build
    // in the same ai_tick -- so it's always current when ai_build reads it.
    int ai_threat = 0;
    // Seconds of "we are actually being shot at" left on the clock: World::hurt
    // refreshes this to kUnderFireWindow whenever anything owned by this team
    // takes damage, and ai_tick counts it down. ai_threat alone can't carry
    // this -- it's a PROXIMITY count, and a lone scout strolling past the base
    // reads identically to a raid killing villagers, which is exactly why the
    // barracks gate needs two enemies before it trusts ai_threat. Damage is
    // unambiguous, so anything keyed off this one can react to a single
    // attacker without a wandering scout tripping it.
    double ai_under_fire = 0.0;
    // ---- "you are under attack" alert -------------------------------------
    // World::hurt only STAMPS these (two doubles and a flag -- no allocation,
    // safe from inside a damage primitive); Control::update_ai turns a pending
    // stamp into the actual event once per tick and runs the cooldown down. See
    // World::hurt for why the event is not raised there.
    //
    // Rate-limited team-wide rather than per-entity: a raid that walks from a
    // farm to a house to a villager is ONE attack, which the per-entity
    // Unit::warned/Building::warned flags cannot see.
    double warn_cd = 0.0;
    double warn_x = 0.0, warn_y = 0.0; // where the last hit landed (for the ping)
    bool warn_pending = false;         // something was hit since the last tick
    // This team's living non-gatherer count, cached by ai_tick before the
    // economy/build/train passes run so all three see the same number (ai_train
    // gets it as `force` directly; ai_build had no way to ask at all).
    int ai_army = 0;
    // Gatherers that finished the last economy pass with NOWHERE TO WORK: no
    // gather target, no foundation, no repair, nothing carried. Published every
    // pass (like ai_threat/ai_army) whatever the variant; only the workforce
    // ceiling (candidate feature 23, control_ai.cpp) currently reads it.
    //
    // It is deliberately NOT "idle right now" -- ai_economy's own idle_civs list
    // is every gatherer carrying nothing this instant, which is most of the
    // workforce most of the time. This is the residue AFTER that list has been
    // handed out: villagers the assignment loop could find no resource for, i.e.
    // the ones the map cannot employ. Distinct from Team::idle_villager_seconds,
    // which is a metrics accumulator for reporting rather than a live count.
    int ai_idle_gatherers = 0;
    // "Being attacked, and without the army to answer it" -- set in ai_economy
    // from ai_under_fire + ai_threat + ai_army, read by ai_build (put a
    // barracks up NOW) and ai_train (raise the defensive floor to the size of
    // the attack, ignore the age-banking hold, and deepen the queues). This is
    // the retaliation reflex: previously a team with no military that got
    // attacked kept booming to its villager goal, because the only military
    // response was a flat 2-5 unit floor that a real push walks straight
    // through, and the first barracks itself waited on cur_civs >= 12.
    bool ai_retaliate = false;
    // ---- Continuous read of what the OPPONENT is doing (ai_read_enemy) -------
    // Everything above answers "is something happening to me right now"; none of
    // it answers "what is he actually doing". ai_threat is a bare head-count in
    // a radius, so a scout strolling past and the leading edge of an all-in look
    // identical, and there was nothing at all for "he has stopped making army
    // and is booming" or "he has committed his whole force and it is 40 seconds
    // out". This is the standing answer to those, refreshed every AI tick before
    // the economy/build/train passes so all of them read the same snapshot.
    struct EnemyRead {
        bool valid = false; // false until a hostile team has been seen at all

        // --- is he building an army, and how fast ---
        int mil = 0;              // living hostile non-gatherer units
        int vil = 0;              // living hostile gatherers
        double mil_ratio = 0.0;   // mil / (mil + vil): ~0 is a pure boom
        double mil_rate = 0.0;    // his army's growth, units/minute, smoothed
        int mil_prev = 0;         // last tick's `mil`, for the rate above

        // --- is he coming for me ---
        // `incoming` counts hostile military that is genuinely committed at us:
        // either its order (rally / move_goal -- the AI is omniscient, so this
        // is his REAL intent, not a guess) points into our base, or it has
        // physically crossed onto our side of the map.
        int incoming = 0;
        double incoming_eta = 1e9;  // seconds until the nearest one arrives
        double committed_frac = 0.0; // incoming / his total army

        // How to read `incoming`. A single wanderer is not an attack, and
        // treating it as one is what made the old barracks gate demand two
        // enemies before it would believe anything.
        enum class Threat { None, Scout, Raid, Push } threat = Threat::None;

        // --- where he lives ---
        bool base_known = false;
        double base_x = 0.0, base_y = 0.0, base_dist = 0.0;

        // --- his posture, 0 = pure economy, 1 = all-in ---
        double aggression = 0.0;
    };
    EnemyRead ai_read;
    // Skirmish AI opts into the map-derived Victorian plan above; campaign AI
    // (new_from_level) leaves this false so it keeps its authored behaviour.
    bool ai_map_derive = false;
    int colour = 0;
    TradeRates trade;
    bool has_base = false; // recomputed each tick (World-coupled, see match.cpp)
    // Alliance group: teams sharing the same value are allied (never
    // hostile, can't win/lose independently of each other -- see
    // Control::allied/check_win). Defaults to this team's own index (every
    // team its own group, i.e. free-for-all) in Control's constructor and
    // new_skirmish, so unset behaves exactly as before the team system
    // existed.
    int ally = 0;
    // Direct port of scripts/give_points.gml: +10 every time a unit
    // finishes training (see building_behavior.cpp), for every team (the
    // original's give_points.gml oddly only listed teams 0-2, never
    // awarding team 3 any points -- treated as an oversight, not
    // replicated here). The score HUD (GameClient::draw_score_hud) adds
    // 10% of current resources on top of this for display, matching
    // Step.gml's current_score0 = floor(score0 + (res)*0.10).
    double score = 0.0;
    // Debug/cheat toggle (chat bar "blitz" command, see GameClient::
    // submit_chat): instantly completes this team's building construction
    // and production-queue items (units/tech alike, both driven by
    // Building::percent -- see building_behavior.cpp), and turns every
    // gather hit into an immediate full delivery instead of a carried trip
    // back to a dropoff (unit_behavior.cpp's update_gather).
    bool blitz = false;
    // Auto-replant toggle (market command-card button, GameClient): when on, a
    // farm that a farmer works to exhaustion is immediately re-sown into a fresh
    // farm at the cost of a new one (see unit_behavior.cpp's farm-deplete path),
    // so the farmer never has to be re-tasked. Team-wide policy.
    bool replant = false;

    // ---- AI-comparison metrics (Control::update_metrics / building_
    // behavior.cpp / world.cpp's death sweep) -- accumulated for every
    // team unconditionally (not just is_ai ones), so they're equally valid
    // for a human player. All running TOTALS, not rates or snapshots, so a
    // tournament harness can normalize by however it likes (per-minute,
    // per-match, etc.) after the fact rather than baking in one convention.
    // Raw resources harvested from the map by type -- NOT current stockpile
    // (which drops with spending) and NOT trade proceeds (converts one
    // resource into another, it doesn't newly produce anything). See
    // World::add_resource.
    std::unordered_map<std::string, double> total_gathered;
    // Real seconds any of this team's completed "base" buildings sat with
    // an empty production queue (the standard "idle TC" stat) -- summed
    // across bases, so two idle at once counts double. See Control::
    // update_metrics.
    double idle_tc_seconds = 0.0;
    // Real seconds a gatherer had genuinely nothing to do: no gather/build
    // target, nothing carried, not off fighting or fleeing (ai_defend_
    // civilians). See Control::update_metrics.
    double idle_villager_seconds = 0.0;
    // Non-civilian units that finished training (building_behavior.cpp) --
    // NOT enqueued, since a queued item can still be cancelled.
    int military_units_created = 0;
    // The same tally broken down by unit name, and WITHOUT the gatherer
    // exclusion -- civilians and fishing boats are counted here too, so this
    // is the full "what did this team actually produce" record rather than
    // just its army. Written at the identical point in building_behavior.cpp,
    // so a unit appears in both or neither. Used by the headless arena's
    // most-produced-unit report; nothing in the game reads it.
    std::unordered_map<std::string, int> units_created_by_name;
    // Own units/buildings that died in combat, NOT counting a player's own
    // deliberate delete (Unit::deleted/Building::deleted) -- see world.cpp's
    // death sweep. In a straight 1v1 match with no other loss cause in this
    // sim, the opponent's units_lost/buildings_lost IS that team's kill
    // count by elimination, with no separate kill-attribution needed.
    int units_lost = 0;
    int buildings_lost = 0;
    // High-water marks for reporting ("largest army this match ever had"),
    // NOT current counts -- sampled every tick in Control::update_metrics
    // from live world state, since there's no single choke point analogous
    // to add_resource/death-sweep for "count alive right now". army = named
    // non-"civilian", non-gatherer units (matches ai_tick's military_by_team
    // split); vil = "civilian" units specifically (matches ai_tick's
    // civ_count, NOT is_gatherer, since that also covers fishing boats).
    int peak_army_size = 0;
    int peak_vil_count = 0;
    // Ever-placed counts (spawn_building, World::spawn_building) for the
    // player-buildable base/shipyard/airbase -- counts a placement even if
    // it's destroyed before finishing construction, since "was it attempted"
    // is the interesting signal for a tournament report, not "how many
    // currently stand" (buildings_lost already covers attrition).
    int bases_built = 0;
    int shipyards_built = 0;
    int airbases_built = 0;
    // ---- extra post-game statistics (end-of-match stats screen) ----
    // Every building this team placed, of ANY type (bases_built etc. only cover
    // three) -- counted at World::spawn_building. Used for the Economy/Society
    // tabs' "buildings constructed".
    int buildings_built = 0;
    // Resources spent from the stockpile (Control::pay), per resource type --
    // the spending counterpart to total_gathered, for the Economy tab.
    std::unordered_map<std::string, double> total_spent;
    // Match time (in whole seconds, from Control::stats_elapsed) at which this
    // team first REACHED each era, index = era (0 Victorian .. 3 Scientific).
    // -1 = never reached. Era 0 is 0 (everyone starts there or at start_age).
    // Set in building_behavior.cpp's age-up completion. Drives the Timeline tab.
    double age_reached_s[4] = {-1.0, -1.0, -1.0, -1.0};
};

// The match brain: economy, population, production rates, tech gating.
// Direct port of game/control.py's Control class -- but ONLY the parts
// that don't touch World (entities list, audio, warn(), place_building):
// team/cost/availability/research-eligibility queries. The per-frame
// World-coupled methods (recompute, check_win, update/_ai_*, research
// application to live units, repair_tick) are NOT here -- they land in
// match.cpp/world.cpp once World exists, matching Unit/Building.
class Control {
public:
    Control(const DataStore& data, const Bonuses& bonuses, int n_players = 2, int max_pop = 100);

    static bool civ_has(const std::string& item, int civ,
                        const std::unordered_map<int, std::set<std::string>>& civ_exclude);
    bool civ_has(const std::string& item, int civ) const { return civ_has(item, civ, civ_exclude_); }
    // True unless `item` is a positively-owned upgrade/tech (CIV_UPGRADE_OWNER)
    // that this civ isn't on the owner list for -- the same gate available_techs
    // uses, exposed so the tech-tree viewer can CROSS OUT (rather than show as
    // available) an owner-gated tech like radar for a civ that can't get it.
    static bool civ_upgrade_allowed(const std::string& item, int civ);

    std::unordered_map<std::string, int> cost_of(const std::string& item, int team = 0) const;
    bool afford(const std::string& item, int team) const;
    void pay(const std::string& item, int team);
    void add(int team, const std::string& key, double amount);

    // Alliance-aware "is `a` hostile to `b`" query: true iff same group.
    // Every hostility check in sim/src (target auto-acquire, splash/impact,
    // AI attack-move targeting, win condition) must go through this rather
    // than a raw team-index comparison; ownership checks (is this MY unit/
    // building) must NOT -- they stay exact team-index comparisons.
    bool allied(int team_a, int team_b) const {
        if (team_a < 0 || team_a >= 8 || team_b < 0 || team_b >= 8) return false;
        return teams[team_a].ally == teams[team_b].ally;
    }

    bool has_tech(const std::string& key, int team) const { return teams[team].tech.count(key) != 0; }
    bool can_research(const std::string& key, int team) const;
    bool is_tech(const std::string& item) const;

    std::vector<std::string> available_units(const std::string& building, int team) const;
    std::vector<std::string> available_techs(const std::string& building, int team) const;
    std::vector<std::string> available_buildings(int team) const;

    // ---- World-coupled: per-frame economy/win-check/AI + research application ----
    void recompute(World& world);
    void check_win(World& world);
    void update_ai(double dt, World& world); // recompute + check_win + AI ticks, matches Control.update
    // AI-comparison metrics (Team::idle_tc_seconds/idle_villager_seconds) --
    // called exactly ONCE per real tick from World::update, deliberately
    // separate from recompute (which runs twice a tick, before and after
    // death bookkeeping -- fine for its own idempotent pop/cap snapshot, but
    // would double-count a dt-based accumulator). See control_ai.cpp.
    void update_metrics(double dt, World& world);
    bool repair_tick(EntityRef building, int team, double dt, World& world);
    bool trade(const std::string& action, const std::string& res, int team, World& world);
    // Oil cost (action=="buy") or oil gain (action=="sell") for one 100-unit
    // trade of `res` at the team's CURRENT rate, fee included. Single source of
    // truth for both Control::trade and the market tooltip.
    double trade_quote(const std::string& action, const std::string& res, int team) const;
    bool research(const std::string& key, int team, World& world);
    void apply_research(const std::string& key, int team, World& world);
    // "Hardest" AI (Team::difficulty 3): grant every technology this civ is
    // legally allowed at its CURRENT era, free and instantly. Called once when
    // the team is first serviced (so it starts with the whole Victorian tree)
    // and again on every age-up (building_behavior.cpp).
    //
    // Deliberately routed through apply_research -- the same function a
    // completed research order calls -- so a granted tech does everything a
    // bought one does: unit upgrades convert the standing army, building
    // upgrades convert the buildings, stat deltas apply. It is a free
    // completion, not a separate code path that could drift.
    //
    // Age advances themselves are NOT granted: the AI still has to earn each
    // era, it just arrives fully teched the moment it does.
    void grant_era_techs(int team, World& world);

    // Hand over any of the civ's free techs (civs.json "freeTech") that the
    // team has just become eligible for, i.e. whose era it has now reached.
    // Called on every age-up (building_behavior.cpp); scenario.cpp's
    // grant_free_techs does the era-0 pass at spawn.
    //
    // Routed through apply_research for the same reason grant_era_techs is: a
    // free tech has to do everything a bought one does. France picking up
    // Pesticide on reaching the War era should behave exactly as if it had
    // paid for it that second.
    void grant_unlocked_free_techs(int team, World& world);

    // Public (not just AI-internal) since the stress-test scenario
    // generator (stress_scenario.cpp) reuses it to place a full building
    // roster per team without duplicating the shipyard-near-water search.
    std::optional<std::pair<double, double>> ai_build_spot(int team, World& world, EntityRef base,
                                                            const std::string& name);
    // The real search. `gap` is the walkable moat (px) every candidate must
    // leave around itself; ai_build_spot runs it once at kAiBuildGap and, only
    // if that finds nowhere at all, once more at 0 so a genuinely boxed-in base
    // can still build flush. See ai_build_spot's comment.
    std::optional<std::pair<double, double>> ai_build_spot_gap(int team, World& world, EntityRef base,
                                                               const std::string& name, double gap);
    // One tile of clear ground between neighbouring AI buildings. A unit is
    // ~half a tile across, so this is the narrowest lane that is reliably
    // walkable; bigger values sprawl the base out far enough to be hard to
    // defend.
    static constexpr double kAiBuildGap = 32.0; // == TILE
    // How many SOLID buildings within 8 tiles of the town centre count as
    // "packed". Below this the base is still sparse enough to walk through and
    // the opening economy is better served by tight, short-walk placement.
    static constexpr int kAiPackedCount = 8;

    const nlohmann::json& techs() const { return data_.techs(); }
    const std::unordered_map<std::string, std::vector<std::string>>& building_techs() const {
        return building_techs_;
    }

    std::vector<Team> teams; // always 8 slots, teams[0..n) active
    int n;
    int max_pop;
    bool game_over = false;
    std::optional<int> winner;

    // ---- post-game statistics timeline (end-of-match stats screen) ----
    // Accumulated match seconds (sum of update_metrics' dt) -- the single clock
    // the stats screen reads for elapsed time, population-sample timestamps, and
    // combat-death timestamps. NOT wall-clock; pure sim time.
    double stats_elapsed = 0.0;
    double stats_sample_acc = 0.0; // time since the last population sample
    // Population (unit count) per team, sampled every kPopSampleInterval seconds
    // -- the data behind the Timeline tab's population-over-time graph.
    struct PopSample {
        double t = 0.0;
        int pop[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    };
    std::vector<PopSample> pop_samples;
    // Every combat death (unit or building), with where and when it happened
    // and whose it was. Deliberately EXCLUDES deliberate deletes (same as
    // units_lost/buildings_lost). Drives the "Largest Battle" tab: kills/razes
    // per team are counts of OTHER teams' entries, and the biggest space-time
    // cluster is the largest battle. Capped at kCombatLogCap (oldest dropped).
    struct CombatDeath {
        float x = 0.0f, y = 0.0f;
        double t = 0.0;
        int team = -1;
        bool is_building = false;
        std::string name; // unit/building type (for the battle unit-breakdown)
    };
    std::vector<CombatDeath> combat_log;
    static constexpr double kPopSampleInterval = 10.0; // seconds between samples
    static constexpr size_t kCombatLogCap = 6000;

private:
    nlohmann::json base_cost(const std::string& item) const;

    // `elapsed` is however much real time accumulated since the last call
    // (see ai_timer_ below) -- each team advances its OWN ai_tick_accum by
    // this and only actually runs its economy/build/research/train/offense
    // logic once that crosses its difficulty-scaled interval, so a Hard team
    // can react/rebuild its queues faster than an Easy one even though this
    // function itself may get called more often than any single team needs.
    void ai_tick(World& world, double elapsed);
    // One-time map read that fills Team::ai_plan (playstyle, villager/fishing
    // goals, a choke to wall off) from the geometry around this team's base.
    // Skirmish-only (gated on Team::ai_map_derive); campaign AI is untouched.
    void ai_assess_map(int team, World& world, EntityRef base);
    // Refresh Team::ai_read -- the standing answer to "what is the opponent
    // doing". Runs first in each team's AI tick, before economy/build/train, so
    // every later pass in that tick sees one consistent snapshot. `dt` is that
    // team's own elapsed time since it last ran (its difficulty cadence), which
    // is what the army-growth rate is measured against.
    void ai_read_enemy(int team, World& world, EntityRef base, double dt);
    // Seals the single planned choke (Team::ai_plan) with a palisade line --
    // the sim-side analogue of the client's click-drag wall builder. One-shot.
    void ai_build_walls(int team, World& world, EntityRef base);
    // Cosmetic early-game scout: sends one roaming unit around map waypoints so
    // the AI "explores" (the AI is already omniscient -- fog only tracks team
    // 0 -- so this is purely visual for now).
    void ai_scout(int team, World& world, const std::vector<EntityRef>& force);
    void ai_economy(int team, World& world, EntityRef base, int n_civ,
                     const std::vector<EntityRef>& team_buildings);
    void ai_build(int team, World& world, EntityRef base, const std::vector<std::string>& names);
    void ai_train(int team, World& world, const std::vector<EntityRef>& team_buildings,
                  const std::vector<EntityRef>& force);
    // Amphibious invasion: when the enemy base is across water (no land route,
    // ai_plan.land_to_enemy == false), ferry the land army over on a transport
    // -- load idle land units near home, sail to the enemy shore, unload, and
    // let the landed troops attack. Land units that have already landed (closer
    // to the enemy than home) are pushed on to the objective. Ships/aircraft
    // still attack directly via the normal offensive push.
    void ai_amphibious(int team, World& world, EntityRef base,
                       const std::vector<EntityRef>& force);
    // Muscateer-rush AI: research the cheapest useful upgrade available at any
    // completed building each tick (economy/combat first), without starving
    // unit/economy production. See control_ai.cpp.
    void ai_research(int team, World& world, const std::vector<EntityRef>& team_buildings);
    // Fortress doctrine: a hostile fortress is 2000 hp with an 8-tile gun, and
    // every mainline unit this AI builds (muscateer 3, rifleman 4, cavalry
    // melee) dies inside that radius for nothing. Only three things reach it
    // safely -- artillery (range 10), the ballistic missile (16), and aircraft,
    // since a fortress is not an AA structure and cannot fire at air at all
    // (building_behavior's `b.is_aa != e->is_air`). This sends those in and
    // keeps everything else out of the radius. See control_ai.cpp.
    void ai_siege(int team, World& world, const std::vector<EntityRef>& force);
    // Target priority + focus fire for this team's army (ai_variant == 12).
    // The offensive push only ever hands units an attack-MOVE (Unit::rally);
    // which enemy each one then shoots is decided by update_combat's plain
    // nearest-anything auto-acquire. This overrides that with a deliberate
    // allocation: rank what is actually in reach by how much it matters, and
    // concentrate enough units on each target to kill it before moving down
    // the list. See control_ai.cpp for the ranking and the saturation rule.
    void ai_focus_fire(int team, World& world, const std::vector<EntityRef>& force);
    // Defense reflex: recall military units that are still close to a
    // building currently being raided, leaving anything already committed
    // far forward (mid-offense) alone -- see control_ai.cpp for exactly how
    // "close" is judged.
    void ai_defend(int team, World& world, const std::vector<EntityRef>& team_buildings,
                   const std::vector<EntityRef>& force);
    // Villager fight-or-flee: a civilian/gatherer that notices a hostile
    // nearby estimates whether ITS local group of villagers can win that
    // fight (always yes against another villager; needs a real numbers
    // advantage against an actual military unit) and either counter-attacks
    // as a group or retreats toward each other first. See control_ai.cpp.
    void ai_defend_civilians(int team, World& world);
    // Bespoke, per-campaign-level AI: dispatched by ai_manage when a Team has a
    // non-empty ai_profile (set from the campaign Level, see new_from_level).
    // Returns true if the profile fully drove this team this tick (generic
    // offensive logic is then skipped). The default build has no profiles, so
    // it returns false -- each campaign's custom behaviour is added here on
    // request, keeping the generic skirmish AI untouched.
    bool ai_profile_manage(const std::string& profile, int team, World& world, EntityRef base,
                           const std::vector<EntityRef>& force);

    const DataStore& data_;
    const Bonuses& bonuses_;
    std::unordered_map<int, std::set<std::string>> civ_exclude_;
    std::unordered_map<std::string, std::vector<std::string>> building_techs_;
    // Shared heartbeat: how often ai_tick gets a chance to check every AI
    // team at all (see kAiHeartbeat in control_ai.cpp) -- fine-grained
    // enough that a Hard team's shorter per-team interval (Team::
    // ai_tick_accum) still gets serviced close to on time.
    double ai_timer_ = 0.0;
};

} // namespace ww::sim
