#pragma once
#include "sim/match.h"

#include <array>
#include <string>
#include <vector>

// Post-game statistics model + procedural battlefield naming, surfaced by the
// end-of-match statistics screen (GameClient::draw_stats_screen). Everything
// here is computed ONCE from a finished/quitting Match -- the sim tracks the
// raw running totals (Control/Team metrics, Control::combat_log, pop_samples);
// this turns them into per-team rows, a population timeline, and the single
// "largest battle" with a made-up place name. The names exist only here.
namespace ww::stats {

struct TeamStats {
    int team = 0, civ = 0, leader = 0, colour = 0;
    std::string name;   // leader name (falls back to civ name)
    bool is_ai = false;
    bool alive = false; // still had a base standing at capture
    bool winner = false;
    int era = 0;
    int score = 0;
    // military
    int military_created = 0;
    int peak_army = 0;
    int units_lost = 0;     // own units killed in combat
    int units_killed = 0;   // enemy units this team killed
    int buildings_lost = 0; // own buildings destroyed
    int buildings_razed = 0; // enemy buildings this team destroyed
    // economy (food/wood/oil/iron)
    std::array<double, 4> gathered{{0, 0, 0, 0}};
    std::array<double, 4> spent{{0, 0, 0, 0}};
    int buildings_built = 0;
    int peak_vil = 0;
    double idle_tc = 0.0, idle_vil = 0.0;
    // society / current live snapshot
    int cur_units = 0, cur_buildings = 0, cur_vil = 0;
    // technology / timeline
    int techs_researched = 0;
    std::array<double, 4> age_reached{{-1, -1, -1, -1}}; // match-seconds each era reached (-1 = never)
};

// One unit type's line in a battle side's order of battle.
struct BattleUnitGroup {
    std::string unit;   // unit/building type key (for the icon + name)
    int involved = 0;   // committed to the battle (casualties + survivors nearby)
    int casualties = 0; // how many of this type were lost
};

struct BattleSide {
    int team = 0;
    int civ = 0, leader = 0, colour = 0;
    std::string leader_name;
    int total_involved = 0;
    int total_casualties = 0;
    int kills = 0; // enemy units this side felled in the battle (= enemy casualties)
    std::vector<BattleUnitGroup> groups; // order of battle, biggest first
};

struct LargestBattle {
    bool valid = false;
    std::string name;    // "Battle of <place>"
    double x = 0.0, y = 0.0;
    double t_start = 0.0, t_end = 0.0;
    int total_casualties = 0;
    int winner_team = -1;         // representative team of the winning side (-1 = stalemate)
    std::string outcome;          // "Decisive victory", "Pyrrhic victory", "Stalemate", ...
    std::string territory;        // territorial-change note (e.g. a base that changed hands)
    std::vector<BattleSide> sides; // per participating team, attacker/defender order
};

struct PopPoint {
    double t = 0.0;
    std::array<int, 8> pop{};
};

struct MatchStats {
    double elapsed_s = 0.0;
    int n_teams = 0;
    bool team0_won = false; // convenience: is the human's alliance the winner
    bool decided = false;   // did the match actually reach game_over
    std::vector<TeamStats> teams;
    std::vector<PopPoint> timeline;
    LargestBattle battle;
};

// Snapshot everything the stats screen needs from a finished/quitting match.
// Non-const because Match's world()/control() accessors are non-const; it does
// not mutate the match.
MatchStats compute_match_stats(ww::sim::Match& match);

// A made-up name for the map location nearest (x,y): "Blackwood Forest",
// "Aldford", "the Silverwater", "Hill 172", etc. Deterministic from position
// so the same battlefield always reads the same. Public for reuse/testing.
std::string feature_name_near(const ww::sim::World& world, double x, double y);

} // namespace ww::stats
