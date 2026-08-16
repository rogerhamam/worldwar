#include "match_stats.h"

#include "menu/civ_data.h" // ww::menu::leader_name
#include "sim/control.h"
#include "sim/resource.h"
#include "sim/world.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace ww::stats {
namespace {

using ww::sim::Control;
using ww::sim::World;

// ---- deterministic hash / word-bank name generation -------------------------
// Names are picked by hashing a feature's representative tile, so the same
// battlefield always yields the same name within (and across) runs -- no RNG
// state, and no dependence on Math.random-style calls the sim forbids.
uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
uint32_t seed_at(int tx, int ty, uint32_t salt) {
    return mix(mix(static_cast<uint32_t>(tx) * 73856093u) ^ mix(static_cast<uint32_t>(ty) * 19349663u) ^
               (salt * 2654435761u));
}
const std::string& pick(const std::vector<std::string>& bank, uint32_t seed) {
    return bank[seed % bank.size()];
}

// Two-part place name from a prefix/suffix bank pair (e.g. "Ald"+"ford").
std::string join_name(const std::vector<std::string>& pre, const std::vector<std::string>& suf,
                      uint32_t seed) {
    return pick(pre, seed) + pick(suf, mix(seed ^ 0x9e3779b9u));
}

std::string city_name(uint32_t s) {
    static const std::vector<std::string> pre = {"Ald",  "Bran", "Cael", "Dun",  "Eber", "Falk", "Gild",
                                                 "Harl", "Ives", "Kel",  "Mor",  "Norl", "Oster", "Perc",
                                                 "Raven", "Stor", "Thorn", "Vald", "Wend", "Yar"};
    static const std::vector<std::string> suf = {"ford", "borough", "ton",  "mere", "field", "gate",
                                                 "haven", "stead",  "wick", "burg", "hold",  "march",
                                                 "reach", "vale",   "crest"};
    return join_name(pre, suf, s);
}
std::string forest_name(uint32_t s) {
    static const std::vector<std::string> adj = {"Black",  "Grey",  "Elder", "Thorn", "Raven", "White",
                                                 "Iron",   "Shadow", "Green", "Whisper", "Ash",  "Hollow",
                                                 "Wolf",   "Mist"};
    static const std::vector<std::string> noun = {"wood", "holt", "wold", "shaw"};
    std::string base = pick(adj, s) + pick(noun, mix(s ^ 0x5bd1e995u));
    // Half the time append "Forest"/"Woods" for variety.
    if ((mix(s) & 1u)) base += (mix(s) & 2u) ? " Forest" : " Woods";
    return base;
}
std::string water_name(uint32_t s, bool big) {
    static const std::vector<std::string> adj = {"Silver", "Red",  "Swift", "Dark",  "Bright", "Cold",
                                                 "Green",  "Long", "Still", "Black", "Broad",  "Grey"};
    static const std::vector<std::string> river = {"water", "brook", "run", "flow", "reach", "mere"};
    if (big) return "the " + pick(adj, s) + " Sea";
    return "the " + pick(adj, s) + pick(river, mix(s ^ 0x1b56c4e9u));
}
std::string desert_name(uint32_t s) {
    static const std::vector<std::string> adj = {"Amber", "Ash",   "Bone",  "Dust", "Salt",
                                                 "Sun",   "Scorched", "Pale", "Red", "Silent"};
    static const std::vector<std::string> noun = {"Waste", "Sands", "Reach", "Flats", "Barrens", "Expanse"};
    return "the " + pick(adj, s) + " " + pick(noun, mix(s ^ 0x27d4eb2fu));
}
std::string plains_name(uint32_t s) {
    static const std::vector<std::string> adj = {"Green", "Wide",  "Golden", "Windy", "Broad",
                                                 "Far",   "Still", "Long",   "High",  "Fallow"};
    static const std::vector<std::string> noun = {"field", "meadow", "moor", "vale", "downs", "heath"};
    // "Goldenfield" or "the Windy Moor" style.
    if (mix(s) & 1u) return pick(adj, s) + pick(noun, mix(s ^ 0x165667b1u));
    return "the " + pick(adj, s) + " " + [&] {
        static const std::vector<std::string> up = {"Field", "Meadow", "Moor", "Vale", "Downs", "Heath"};
        return pick(up, mix(s ^ 0x165667b1u));
    }();
}
std::string ore_name(uint32_t s, bool iron) {
    static const std::vector<std::string> adj = {"Deep", "Black", "Old", "Rich", "Grim", "Rust", "Low"};
    return "the " + pick(adj, s) + (iron ? " Iron Mines" : " Oil Fields");
}
std::string hill_name(uint32_t s) {
    // High ground reads as a named ridge/heights/bluff, or the classic
    // numbered feature ("Hill 172", the user's example) -- picked by hash.
    switch (mix(s) % 4) {
        case 0: {
            int n = 100 + static_cast<int>(s % 350); // 100..449
            return "Hill " + std::to_string(n);
        }
        default: {
            static const std::vector<std::string> adj = {"Iron",  "Raven", "Grey",  "Storm", "Eagle",
                                                         "North", "Red",   "Cold",  "High",  "Wolf",
                                                         "Falcon", "Stone"};
            static const std::vector<std::string> noun = {"Heights", "Ridge", "Bluff", "Crest", "Rise"};
            return "the " + pick(adj, s) + " " + pick(noun, mix(s ^ 0x2c1b3c6du));
        }
    }
}

} // namespace

std::string feature_name_near(const ww::sim::World& world, double x, double y) {
    // We only read; cast away const for the entity accessors (World::get_* are
    // non-const only). Safe -- naming never mutates the world.
    World& w = const_cast<World&>(world);
    int tx = std::clamp(static_cast<int>(x / ww::sim::TILE), 0, std::max(0, w.cols - 1));
    int ty = std::clamp(static_cast<int>(y / ww::sim::TILE), 0, std::max(0, w.rows - 1));
    uint32_t s = seed_at(tx, ty, 0);

    auto d2 = [](double ax, double ay, double bx, double by) {
        double dx = ax - bx, dy = ay - by;
        return dx * dx + dy * dy;
    };

    // 1) Nearest town centre -> named after that settlement ("city"). Within
    //    ~14 tiles the battle reads as being "at" the town.
    {
        double best = 1e18;
        double bx = 0, by = 0;
        bool found = false;
        for (auto ref : w.active_buildings) {
            ww::sim::Building* b = w.get_building(ref);
            if (!b || !b->common.alive || b->name != "base") continue;
            double dd = d2(x, y, b->common.x, b->common.y);
            if (dd < best) {
                best = dd;
                bx = b->common.x;
                by = b->common.y;
                found = true;
            }
        }
        double reach = 14.0 * ww::sim::TILE;
        if (found && best <= reach * reach) {
            int btx = static_cast<int>(bx / ww::sim::TILE), bty = static_cast<int>(by / ww::sim::TILE);
            return city_name(seed_at(btx, bty, 11));
        }
    }

    // 2) Nearby resources decide forest / ore. Count trees and ore within ~6
    //    tiles of the battle centre.
    int trees = 0, oil = 0, iron = 0;
    double near_tree_x = x, near_tree_y = y, best_tree = 1e18;
    double reach2 = 6.0 * ww::sim::TILE;
    for (auto ref : w.active_resources) {
        ww::sim::Resource* r = w.get_resource(ref);
        if (!r || !r->common.alive) continue;
        double dd = d2(x, y, r->common.x, r->common.y);
        if (dd > reach2 * reach2) continue;
        if (r->name == "tree" || r->name == "palm") {
            ++trees;
            if (dd < best_tree) {
                best_tree = dd;
                near_tree_x = r->common.x;
                near_tree_y = r->common.y;
            }
        } else if (r->name == "oil") {
            ++oil;
        } else if (r->name == "iron") {
            ++iron;
        }
    }
    if (trees >= 4) {
        int ftx = static_cast<int>(near_tree_x / ww::sim::TILE), fty = static_cast<int>(near_tree_y / ww::sim::TILE);
        return forest_name(seed_at(ftx, fty, 22));
    }
    if (oil >= 2 || iron >= 2) return ore_name(s, iron >= oil);

    // 3) Terrain-based: water body / desert / plains, else a numbered hill.
    //    Sample a small window around the centre to classify the local terrain.
    int water = 0, sand = 0, total = 0;
    for (int dx = -3; dx <= 3; ++dx) {
        for (int dy = -3; dy <= 3; ++dy) {
            int cx = tx + dx, cy = ty + dy;
            if (cx < 0 || cy < 0 || cx >= w.cols || cy >= w.rows) continue;
            ++total;
            int t = w.terrain[cx][cy];
            if (t == ww::sim::WATER) ++water;
            else if (t == 3) ++sand; // sand id (scenario.cpp)
        }
    }
    if (total > 0 && water * 3 >= total) return water_name(s, water * 5 >= total * 4);
    if (total > 0 && sand * 2 >= total) return desert_name(s);
    // A fair bit of open ground -> plains; otherwise a numbered hill feature.
    if (total > 0 && (water + sand) * 4 < total && (mix(s) & 1u)) return plains_name(s);
    return hill_name(s);
}

MatchStats compute_match_stats(ww::sim::Match& match) {
    World& world = match.world();
    Control& ctrl = match.control();
    MatchStats out;
    out.elapsed_s = ctrl.stats_elapsed;
    out.n_teams = ctrl.n;
    out.decided = ctrl.game_over;

    static const char* kRes[4] = {"food", "wood", "oil", "iron"};

    // Live per-team snapshot counts.
    std::vector<int> cur_units(8, 0), cur_vil(8, 0), cur_buildings(8, 0);
    for (auto ref : world.active_units) {
        ww::sim::Unit* u = world.get(ref);
        if (!u || !u->common.alive || u->common.team < 0 || u->common.team >= 8) continue;
        cur_units[u->common.team]++;
        if (u->name == "civilian") cur_vil[u->common.team]++;
    }
    for (auto ref : world.active_buildings) {
        ww::sim::Building* b = world.get_building(ref);
        if (!b || !b->common.alive || b->common.team < 0 || b->common.team >= 8) continue;
        cur_buildings[b->common.team]++;
    }

    // Kills/razes derived from the combat log: an entry counts toward every
    // team NOT allied with the dead entity's owner. In a 1v1 this is exactly
    // "the enemy's losses = your kills"; it also generalises to FFA/teams.
    std::vector<int> kills(8, 0), razes(8, 0);
    for (const auto& ev : ctrl.combat_log) {
        if (ev.team < 0 || ev.team >= ctrl.n) continue;
        for (int i = 0; i < ctrl.n; ++i) {
            if (ctrl.teams[i].ally == ctrl.teams[ev.team].ally) continue; // allies don't get the credit
            if (ev.is_building) razes[i]++;
            else kills[i]++;
        }
    }

    bool team0_won = false;
    for (int i = 0; i < ctrl.n; ++i) {
        const ww::sim::Team& t = ctrl.teams[i];
        TeamStats ts;
        ts.team = i;
        ts.civ = t.civ;
        ts.leader = t.leader;
        ts.colour = t.colour;
        ts.name = ww::menu::leader_name(t.civ, t.leader);
        ts.is_ai = (i != 0);
        ts.alive = t.has_base;
        ts.era = t.era;
        ts.score = static_cast<int>(std::floor(t.score));
        ts.military_created = t.military_units_created;
        ts.peak_army = t.peak_army_size;
        ts.units_lost = t.units_lost;
        ts.units_killed = kills[i];
        ts.buildings_lost = t.buildings_lost;
        ts.buildings_razed = razes[i];
        ts.buildings_built = t.buildings_built;
        ts.peak_vil = t.peak_vil_count;
        ts.idle_tc = t.idle_tc_seconds;
        ts.idle_vil = t.idle_villager_seconds;
        ts.cur_units = cur_units[i];
        ts.cur_vil = cur_vil[i];
        ts.cur_buildings = cur_buildings[i];
        ts.techs_researched = static_cast<int>(t.tech.size());
        for (int r = 0; r < 4; ++r) {
            auto g = t.total_gathered.find(kRes[r]);
            ts.gathered[r] = g != t.total_gathered.end() ? g->second : 0.0;
            auto sp = t.total_spent.find(kRes[r]);
            ts.spent[r] = sp != t.total_spent.end() ? sp->second : 0.0;
            ts.age_reached[r] = t.age_reached_s[r];
        }
        // Era 0 is always "reached" at the start (or the campaign start age).
        if (ts.age_reached[0] < 0.0) ts.age_reached[0] = 0.0;
        if (ctrl.winner.has_value() && t.ally == ctrl.teams[*ctrl.winner].ally) {
            ts.winner = true;
            if (i == 0) team0_won = true;
        }
        out.teams.push_back(ts);
    }
    out.team0_won = team0_won;

    // Population timeline.
    for (const auto& s : ctrl.pop_samples) {
        PopPoint p;
        p.t = s.t;
        for (int i = 0; i < 8; ++i) p.pop[i] = s.pop[i];
        out.timeline.push_back(p);
    }

    // ---- Largest battle: densest space-time cluster of combat deaths --------
    // Bucket every death into a coarse (space x time) grid, find the densest
    // bucket, then gather everything near that bucket's centroid.
    const auto& log = ctrl.combat_log;
    if (!log.empty()) {
        const double R = 8.0 * ww::sim::TILE; // spatial bucket ~8 tiles
        const double Wt = 45.0;               // temporal bucket 45s
        std::unordered_map<int64_t, int> counts;
        auto key = [&](const Control::CombatDeath& e) {
            int gx = static_cast<int>(e.x / R), gy = static_cast<int>(e.y / R), gt = static_cast<int>(e.t / Wt);
            return (static_cast<int64_t>(gx) << 40) ^ (static_cast<int64_t>(gy) << 20) ^ (gt & 0xfffff);
        };
        int64_t best_key = 0;
        int best_count = 0;
        for (const auto& e : log) {
            if (e.is_building) continue; // a battle is a clash of UNITS -- razed buildings don't count
            int c = ++counts[key(e)];
            if (c > best_count) {
                best_count = c;
                best_key = key(e);
            }
        }
        // Centroid of the densest bucket.
        double cx = 0, cy = 0, ct = 0;
        int n = 0;
        for (const auto& e : log) {
            if (e.is_building) continue;
            if (key(e) != best_key) continue;
            cx += e.x;
            cy += e.y;
            ct += e.t;
            ++n;
        }
        if (n > 0) {
            cx /= n;
            cy /= n;
            ct /= n;
            const double gatherR = 1.5 * R, gatherT = 1.5 * Wt;
            LargestBattle b;
            b.x = cx;
            b.y = cy;
            double tmin = 1e18, tmax = -1e18;
            // Per-team casualties, and per-team per-unit-type casualties, from
            // the deaths that fall inside the battle's space-time window.
            std::unordered_map<int, int> cas;
            std::unordered_map<int, std::unordered_map<std::string, int>> cas_by_type;
            for (const auto& e : log) {
                if (e.is_building) continue; // buildings never appear in the battle roster/casualties
                double dx = e.x - cx, dy = e.y - cy;
                if (dx * dx + dy * dy > gatherR * gatherR) continue;
                if (std::abs(e.t - ct) > gatherT) continue;
                cas[e.team]++;
                cas_by_type[e.team][e.name.empty() ? "unit" : e.name]++;
                b.total_casualties++;
                tmin = std::min(tmin, e.t);
                tmax = std::max(tmax, e.t);
            }
            b.t_start = tmin;
            b.t_end = tmax;
            // Survivors still on the field at match end: living units within the
            // battle radius, per team per type -- these plus casualties make up
            // "units involved".
            std::unordered_map<int, std::unordered_map<std::string, int>> alive_by_type;
            for (auto ref : world.active_units) {
                ww::sim::Unit* u = world.get(ref);
                if (!u || !u->common.alive || u->common.team < 0 || u->common.team >= 8) continue;
                // Only MILITARY survivors count as battle participants -- a
                // villager/civilian standing near the site at match end (e.g. a
                // base built there later) is not a combatant, and counting them
                // would wildly inflate a side's "engaged" strength.
                if (u->name == "civilian" || u->is_gatherer) continue;
                double dx = u->common.x - cx, dy = u->common.y - cy;
                if (dx * dx + dy * dy > gatherR * gatherR) continue;
                alive_by_type[u->common.team][u->name]++;
            }
            // Assemble one BattleSide per participating team (any team that lost
            // a unit here, or has survivors here).
            std::unordered_map<int, bool> participates;
            for (auto& [tm, c] : cas) participates[tm] = true;
            for (auto& [tm, m] : alive_by_type) participates[tm] = true;
            for (auto& [tm, _] : participates) {
                if (tm < 0 || tm >= ctrl.n) continue;
                BattleSide s;
                s.team = tm;
                s.civ = ctrl.teams[tm].civ;
                s.leader = ctrl.teams[tm].leader;
                s.colour = ctrl.teams[tm].colour;
                s.leader_name = ww::menu::leader_name(s.civ, s.leader);
                s.total_casualties = cas.count(tm) ? cas[tm] : 0;
                // Union of casualty types and surviving types.
                std::unordered_map<std::string, BattleUnitGroup> groups;
                for (auto& [nm, c] : cas_by_type[tm]) {
                    groups[nm].unit = nm;
                    groups[nm].casualties += c;
                    groups[nm].involved += c;
                }
                for (auto& [nm, c] : alive_by_type[tm]) {
                    groups[nm].unit = nm;
                    groups[nm].involved += c;
                }
                for (auto& [nm, g] : groups) {
                    s.total_involved += g.involved;
                    s.groups.push_back(g);
                }
                std::sort(s.groups.begin(), s.groups.end(),
                          [](const BattleUnitGroup& a, const BattleUnitGroup& c) {
                              return a.involved > c.involved;
                          });
                b.sides.push_back(s);
            }
            // A side's kills = the casualties of every enemy (non-allied) side.
            for (auto& s : b.sides) {
                int k = 0;
                for (auto& o : b.sides)
                    if (ctrl.teams[o.team].ally != ctrl.teams[s.team].ally) k += o.total_casualties;
                s.kills = k;
            }
            // Order sides by involvement (attacker/defender read top-to-bottom).
            std::sort(b.sides.begin(), b.sides.end(),
                      [](const BattleSide& a, const BattleSide& c) { return a.total_involved > c.total_involved; });

            // Winner = the alliance that lost the FEWEST; representative team is
            // the least-battered participant on that side.
            std::unordered_map<int, int> ally_cas, ally_start;
            for (auto& s : b.sides) {
                ally_cas[ctrl.teams[s.team].ally] += s.total_casualties;
                ally_start[ctrl.teams[s.team].ally] += s.total_involved;
            }
            // Winner = fewest casualties; on a tie, whoever kept more surviving
            // military on the field ("held the ground").
            int win_ally = -1, fewest = 1 << 30, best_surv = -1;
            for (auto& [al, c] : ally_cas) {
                int surv = (ally_start.count(al) ? ally_start[al] : 0) - c;
                if (c < fewest || (c == fewest && surv > best_surv)) {
                    fewest = c;
                    best_surv = surv;
                    win_ally = al;
                }
            }
            int rep = -1, rep_cas = 1 << 30;
            for (auto& s : b.sides) {
                if (ctrl.teams[s.team].ally == win_ally && s.total_casualties < rep_cas) {
                    rep_cas = s.total_casualties;
                    rep = s.team;
                }
            }
            b.winner_team = rep;

            // Outcome category from the winner's loss RATIO and the margin over
            // the loser (classic wargame terms).
            int loser_cas = 0, winner_cas = fewest, winner_start = ally_start.count(win_ally) ? ally_start[win_ally] : 1;
            for (auto& [al, c] : ally_cas) if (al != win_ally) loser_cas = std::max(loser_cas, c);
            double win_ratio = winner_start > 0 ? double(winner_cas) / winner_start : 0.0;
            bool close = loser_cas > 0 && winner_cas >= loser_cas * 0.8;
            // A true dead heat (only one side, or equal losses AND neither side
            // left holding the field) is inconclusive; equal losses but a
            // surviving winner counts as a narrow victory (they held the ground).
            if (ally_cas.size() < 2 || (winner_cas == loser_cas && best_surv <= 0)) {
                b.outcome = "Inconclusive";
                b.winner_team = -1;
            } else if (win_ratio >= 0.6) {
                b.outcome = "Pyrrhic victory"; // won, but gutted
            } else if (close) {
                b.outcome = "Hard-fought victory";
            } else if (win_ratio <= 0.2 && loser_cas >= winner_cas * 3) {
                b.outcome = "Decisive victory";
            } else if (loser_cas >= winner_cas * 5) {
                b.outcome = "Heroic victory";
            } else {
                b.outcome = "Marginal victory";
            }

            // Territorial change: the nearest base to the battle centre, and
            // whose it is now (a captured/held forward position).
            {
                double best = 1e18;
                int owner = -1;
                double bx = 0, by = 0;
                for (auto ref : world.active_buildings) {
                    ww::sim::Building* bd = world.get_building(ref);
                    if (!bd || !bd->common.alive || bd->name != "base") continue;
                    double dx = bd->common.x - cx, dy = bd->common.y - cy;
                    double dd = dx * dx + dy * dy;
                    if (dd < best) { best = dd; owner = bd->common.team; bx = bd->common.x; by = bd->common.y; }
                }
                if (owner >= 0 && owner < ctrl.n && best <= (18.0 * ww::sim::TILE) * (18.0 * ww::sim::TILE)) {
                    std::string place = feature_name_near(world, bx, by);
                    if (b.winner_team >= 0 && ctrl.teams[owner].ally == ctrl.teams[b.winner_team].ally)
                        b.territory = ww::menu::leader_name(ctrl.teams[owner].civ, ctrl.teams[owner].leader) +
                                      " held the approaches to " + place + ".";
                    else
                        b.territory = "The line near " + place + " held.";
                } else {
                    b.territory = "No ground changed hands.";
                }
            }

            int sides_with_loss = 0;
            for (auto& s : b.sides) if (s.total_casualties > 0) ++sides_with_loss;
            if (b.total_casualties >= 6 && sides_with_loss >= 2) {
                b.name = "Battle of " + feature_name_near(world, cx, cy);
                b.valid = true;
                out.battle = b;
            }
        }
    }

    return out;
}

} // namespace ww::stats
