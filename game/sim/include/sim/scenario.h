#pragma once
#include "campaign/campaign_data.h"
#include "sim/world.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ww::sim {

// Direct port of game/scenario.py's new_skirmish settings dict.
struct SkirmishSettings {
    int n_players = 2;
    int map_size = 64; // World cols/rows = map_size * 2; 64 = the new "Normal" (was "Huge")
    int max_pop = 100;
    bool water = true;
    std::string map_type = "random";
    std::vector<int> civs;              // empty -> default {0, 2, 3, 4}[:n]
    std::optional<int> enemy_civ;
    int leader = 0;
    int enemy_leader = 0;
    std::optional<int> player_colour;
    std::optional<int> enemy_colour;
    // Per-team colour index (0-7, see get_col_rgb.gml's palette), parallel
    // to `civs` above -- lets a menu with more than 2 player slots assign
    // every team its own colour, not just team 0/1 via player_colour/
    // enemy_colour. Empty (the default) falls back to those two fields /
    // new_skirmish's own per-team default exactly as before.
    std::vector<int> colours;
    // Per-team alliance group (see Team::ally/Control::allied), parallel to
    // `civs`/`colours`. Empty (the default) leaves every team in its own
    // group -- free-for-all, matching pre-team-system behavior exactly.
    std::vector<int> allies;
    // Per-team leader index (0-2 into civs.json's per-civ "leaders" list),
    // parallel to `civs`. Empty falls back to `leader`/`enemy_leader`/0 below.
    std::vector<int> leaders;
    // "Deathmatch" game mode (vs. "Standard"): every team starts with a
    // huge resource stockpile and construction/training/research run much
    // faster, matching the original GML's game_mode==1 (see
    // objects/control/Other_4.gml's food/wood/oil/iron set and
    // objects/obj_building/Step.gml's queue `percent` base=25 vs 4).
    bool deathmatch = false;
    // 0 = Standard fog of war, 1 = "No fog" (map starts/stays fully
    // explored -- terrain and buildings visible everywhere, but units only
    // within actual sight range, same as returning from an explored-but-
    // not-currently-visible tile), 2 = "Revealed" (everything visible
    // always, World::reveal_all). See new_skirmish's use of this.
    int reveal_mode = 0;
    // True for a "Start Stress Test" preview (Graphics options screen,
    // MenuController::build_settings()): makes EVERY team AI-controlled,
    // including team 0 (which new_skirmish otherwise always leaves human) --
    // a pure spectator match with no human side to click into. See
    // GameClient's spectator_ flag (game_client.h) for the client-side half
    // of this (blocking input entirely), which this field alone doesn't
    // cover -- an AI-controlled team's units still obey an explicit manual
    // order if one somehow reached them.
    bool spectator = false;
    // "Easy"/"Normal"/"Hard" (0/1/2) -- copied into every AI team's
    // Team::difficulty (see control.h) by new_skirmish below. Drives how
    // fast the AI re-evaluates its decisions and how eagerly it spends into
    // research (control_ai.cpp); it deliberately does NOT touch costs or
    // stats. Defaults to Normal so anything that builds a SkirmishSettings
    // without setting this explicitly (tests, headless_runner) is unaffected.
    int difficulty = 1;
};

// Builds a fresh skirmish match: for each team, places a base, starting
// villagers + a scout, resource nodes near each base, and scattered
// global resources -- direct port of scenario.py's new_skirmish, except
// terrain generation AND resource placement now draw from the SAME
// Match-owned seeded Rng (fixing determinism bug #1 -- Python's terrain
// generation used its own always-unseeded random.Random()).
std::unique_ptr<World> new_skirmish(const DataStore& data, const Bonuses& bonuses, Control& control,
                                    Rng& rng, EventBus& events, const SkirmishSettings& settings);

// Builds a World from a hand-authored campaign Level (worldwar_campaign_
// editor's schema, ww::campaign::Level) instead of new_skirmish's random
// terrain/starting-position generation: a flat grass grid sized to
// level.grid_size, with only the level's own explicit terrain/units/
// buildings placed, and each Level::players[i] seeded into
// control.teams[i] (index 0 is always the locked human slot, matching
// Control/GameClient's own team-0-is-human assumption throughout).
//
// Every editor-authorable base/resource kind now has a live counterpart:
// `base` maps to a real terrain tile id (water/sand use the sim's
// pre-existing ids; dirt/gravel/brown dirt/pavement got new ids 4-7, see
// terrain_sprite() in game_client.cpp); `resource` is a gatherable
// resource (spawn_resource), deer (spawn_deer), a real World::FireHazard
// for "flames" (so a hand-placed flame actually burns units, same as a
// shell-impact fire), or one of the ~35 non-gatherable decoration kinds
// (see World::decorations/decoration_kinds() -- rendered and, per each
// kind's `passable` flag, blocking movement).
std::unique_ptr<World> new_from_level(const DataStore& data, const Bonuses& bonuses, Control& control,
                                      Rng& rng, EventBus& events, const ww::campaign::Level& level);

} // namespace ww::sim
