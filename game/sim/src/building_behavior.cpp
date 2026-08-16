// Building behavior: direct port of Building.update (game/entity.py),
// minus the cosmetic random debris/smoke particle spawning (determinism
// bug #2/#6 -- those were driven by Python's un-seeded `random` module
// and are pure decoration, so they belong entirely on the client, keyed
// off hit_timer/hp-fraction state the client already has).
#include "sim/behavior.h"
#include "sim/building.h"
#include "sim/control.h"
#include "sim/resource.h"
#include "sim/unit.h"
#include "sim/world.h"

#include <algorithm>
#include <set>

namespace ww::sim {

// Every unit and villager trains 20% faster than the catalog rate, for every
// civ. Applied once, in the production tick below, so it lands on villagers
// and military identically and stacks multiplicatively with the civ/leader/
// tech modifiers rather than replacing any of them.
constexpr double kGlobalTrainSpeed = 1.2;

void update_building(EntityRef self, Building& b, double dt, World& world) {
    Control& ctrl = world.control;
    if (b.highlight > 0) b.highlight -= dt;
    if (b.hit_timer > 0) b.hit_timer -= dt;
    if (b.common.dmg_flash > 0) b.common.dmg_flash -= dt;

    // Nuclear reactor: passively generates OIL into its owner's stockpile once
    // complete -- a late-game (era 3) oil well that doesn't need a villager,
    // worth roughly a steady miner's income (~2 oil/sec).
    if (b.complete && b.name == "nuclear reactor" && b.common.team >= 0 && b.common.team < 8) {
        Team& rt = world.control.teams[b.common.team];
        // Mustafa Ataturk: all oil generation +10% (matches his gather bonus).
        double rate = 2.0 * (world.bonuses.leader_name(rt.civ, rt.leader) == "Mustafa Ataturk" ? 1.10 : 1.0);
        rt.res["oil"] += rate * dt;
        rt.total_gathered["oil"] += rate * dt; // counts on the stats screen
    }

    // Defensive-structure combat: a completed tower/fortress/aa tower fires at
    // the nearest hostile unit in range on its reload cadence. aa towers shoot
    // ONLY aircraft; the others shoot only ground units.
    if (b.complete && b.attack > 0.0) {
        if (b.reload_timer > 0) b.reload_timer -= dt;
        if (b.reload_timer <= 0) {
            // Radar gives a team's defensive buildings +1 tile of firing range.
            double eff_range = b.range_px;
            if (b.common.team >= 0 && b.common.team < static_cast<int>(ctrl.teams.size()) &&
                ctrl.teams[b.common.team].tech.count("radar")) {
                eff_range += TILE;
            }
            // Gather hostiles in range (aa->air only, others->ground only),
            // nearest first.
            std::vector<std::pair<double, EntityRef>> targets;
            for (auto ref : world.grid.query(b.common.x, b.common.y, eff_range)) {
                if (ref.kind != EntityKind::Unit) continue;
                EntityCommon* e = world.common(ref);
                if (!e || !e->alive || e->team < 0 || ctrl.allied(e->team, b.common.team)) continue;
                if (b.is_aa != e->is_air) continue; // aa->air only; others->ground only
                double d = std::hypot(e->x - b.common.x, e->y - b.common.y);
                if (d <= eff_range) targets.emplace_back(d, ref);
            }
            if (!targets.empty()) {
                std::sort(targets.begin(), targets.end(),
                          [](const auto& a, const auto& b2) { return a.first < b2.first; });
                // The fortress rakes the area with a BARRAGE (5 rounds spread
                // across the nearest foes -- gmk obj_fortress's burst fire);
                // towers/AA fire a single aimed round.
                int shots = (b.name == "fortress") ? 5 : 1;
                for (int i = 0; i < shots; ++i) {
                    world.spawn_building_shot(b, targets[i % targets.size()].second);
                }
                b.reload_timer = b.reload;
                world.events.push({EventType::Sound, b.is_aa ? "cannon" : "gunshot", b.common.x,
                                   b.common.y, 0, kNullRef, ""});
            }
        }
    }

    if (b.name == "farm" && b.occupied_by.valid()) {
        Unit* oc = world.get(b.occupied_by);
        if (!oc || !oc->common.alive || !(oc->gather_target == self)) {
            b.occupied_by = kNullRef;
        }
    }

    if (!b.complete) {
        // Chat-bar "blitz" cheat (Team::blitz) -- finish construction the
        // instant it's checked, regardless of how far along it actually is.
        if (ctrl.teams[b.common.team].blitz) b.construction = 100.0;
        double dc = b.construction - b.prev_con;
        if (dc > 0) {
            b.common.hp = std::min(b.full_max_hp, b.common.hp + (b.full_max_hp - 40) * dc / 100.0);
        }
        b.prev_con = b.construction;
        if (b.common.hp <= 0) { b.common.alive = false; return; }
        if (b.construction >= 100) {
            b.construction = 100.0;
            b.complete = true;
            b.common.hp = b.full_max_hp;
            // The footprint has been walk-through the whole time it sat at
            // 0% (Building::blocks_movement) and turns solid the instant it
            // completes -- anything still standing inside would be sealed
            // in with no passable tile left to step onto. Ordinary
            // construction can't get here with a unit inside (the build
            // branch in unit_behavior.cpp refuses to lay the first blow on
            // an occupied footprint), but the blitz cheat jumps straight
            // from 0 to 100 above, so eject whoever's there rather than
            // trust that. Displaced outright, not just re-ordered: a unit
            // already inside a solid rect can't walk its way out.
            for (auto uref : world.units_on_footprint(b)) {
                Unit* u = world.get(uref);
                if (!u) continue;
                auto [ox, oy] = world.point_off_footprint(b, u->common.x, u->common.y,
                                                          u->common.is_air, u->common.is_ship);
                u->common.x = ox;
                u->common.y = oy;
                u->path.clear();
                u->path_i = 0;
                u->need_path = true;
            }
            world.events.push({EventType::Sound, "build", b.common.x, b.common.y, 0, kNullRef, ""});
            if (b.common.team == 0) {
                world.events.push({EventType::Sound, "building_ready", b.common.x, b.common.y, 0, self, b.name});
                world.events.push({EventType::Notify, "building_ready", b.common.x, b.common.y, 0, self, b.name});
            }
            // A tower finishing construction after the team already
            // researched its upgrade tech (see Control::apply_research's
            // BUILDING_UPGRADE_MAP) completes straight into the upgraded
            // form, same as a unit that finishes training after its own
            // upgrade tech is already researched.
            for (auto& [tech, pair] : BUILDING_UPGRADE_MAP) {
                if (b.name == pair.first && ctrl.teams[b.common.team].tech.count(tech)) {
                    world.transform_building(self, pair.second);
                    break;
                }
            }
        }
        return;
    }

    if (b.queue.empty()) { b.percent = 0.0; return; }
    const std::string& item = b.queue.front();
    Team& team = ctrl.teams[b.common.team];
    b.acc += dt;
    double spd = world.build_speed;
    bool is_age = std::find(AGE_ITEMS.begin(), AGE_ITEMS.end(), item) != AGE_ITEMS.end();
    if (team.blitz) {
        // Chat-bar "blitz" cheat -- instant unit/tech/age completion, and
        // (unlike the normal path) not blocked by the population cap
        // either, matching "instant unit creation" rather than just
        // "instant once there's room".
        b.percent = 100.0;
    } else {
        while (b.acc >= 1.0 && b.percent < 100) {
            b.acc -= 1.0;
            if (is_age) {
                // Winston Churchill: this team advances an era 50% faster (was a
                // hardcoded UK-civ bonus; now the leader carries it).
                bool fast_age = world.bonuses.leader_name(team.civ, team.leader) == "Winston Churchill";
                b.percent += (fast_age ? 1.5 : 1.0) * spd;
            } else if (ctrl.is_tech(item)) {
                // Emperor Hirohito: economic upgrades research 100% faster (2x).
                double rmul = (world.bonuses.leader_name(team.civ, team.leader) == "Emperor Hirohito" &&
                               world.bonuses.is_economic_tech(item))
                                  ? 2.0
                                  : 1.0;
                b.percent += 3.3 * spd * rmul;
            } else if (team.pop + pop_cost(item) <= team.cap) {
                double tmul = 1.0;
                // Conscription (fortress, scientific): LAND military units train
                // 33% faster -- excludes civilians, aircraft, and ships.
                if (team.tech.count("conscription") && item != "civilian") {
                    const auto& units = world.data.catalog().at("units");
                    bool land = units.contains(item) && !units.at(item).value("aerial", false) &&
                                !units.at(item).value("ship", false);
                    if (land) tmul = 1.33;
                }
                // Ottoman Empire: everything trained at the BARRACKS comes out
                // faster (civs.json "barracksTrainSpeed", a rate multiplier).
                if (b.name == "barracks")
                    tmul *= world.bonuses.civ_effects(team.civ).value("barracksTrainSpeed", 1.0);
                // Leader production bonus (Adolf Hitler: civilians 15% faster).
                tmul *= world.bonuses.leader_train_mult(team.civ, team.leader, item);
                // "Hardest" AI (Team::difficulty 3): every unit trains 50%
                // faster. Applied here rather than to the AI's decision cadence
                // because it is a HANDICAP, not skill -- see Team::difficulty.
                // Only an AI team gets it; a human on any difficulty trains at
                // the normal rate.
                if (team.is_ai && team.difficulty >= 3) tmul *= 1.5;
                // Global training-speed buff: every unit and villager, every
                // civ, human and AI alike. A flat multiplier here rather than
                // per-civ catalog edits so it cannot skew the civ balance --
                // everyone gets the same 20%.
                tmul *= kGlobalTrainSpeed;
                b.percent += 4.0 * spd * tmul;
            } // else: population-blocked, no progress
        }
    }
    if (b.percent >= 100) {
        b.percent = 0.0;
        std::string finished = item;
        b.queue.erase(b.queue.begin());
        if (is_age) {
            team.era = static_cast<int>(std::find(AGE_ITEMS.begin(), AGE_ITEMS.end(), finished) -
                                        AGE_ITEMS.begin()) + 1;
            // Post-game Timeline tab: stamp the match-time this era was reached.
            if (team.era >= 0 && team.era < 4 && team.age_reached_s[team.era] < 0.0)
                team.age_reached_s[team.era] = ctrl.stats_elapsed;
            // "Hardest" AI: the new era's entire tech tree, free and instantly,
            // the moment the age completes. Unit upgrades convert the standing
            // army as part of this (apply_research), so its riflemen become
            // infantrymen the same second it reaches Industrial.
            if (team.is_ai && team.difficulty >= 3)
                ctrl.grant_era_techs(b.common.team, world);
            if (b.common.team == 0) {
                world.events.push({EventType::Sound, "age_advance", 0, 0, 0, kNullRef, ""});
                world.events.push({EventType::Notify, "age_advance", 0, 0, 0, kNullRef, ""});
            }
            // ...and a chat-feed line for EVERY team's age-up, including the
            // opponents'. Aging is the single biggest power spike in the game
            // and the player had no way to see one happen on the other side of
            // the map -- an enemy reaching War era is the difference between
            // facing riflemen and facing tanks, which is worth knowing before
            // they arrive. This does not leak anything the AI isn't already
            // given: it reads the whole map with no fog at all (see ai_read_
            // enemy), so the asymmetry currently runs entirely the other way.
            //
            // Separate key from "age_advance" above on purpose -- that one is
            // the player's own banner+sound and stays exactly as it was; this
            // one carries the team so the client can name whose it is.
            {
                SimEvent ev{EventType::Notify, "team_age_advance", 0, 0, 0, kNullRef, ""};
                ev.team = b.common.team;
                world.events.push(ev);
            }
        } else if (ctrl.is_tech(finished)) {
            ctrl.apply_research(finished, b.common.team, world);
        } else {
            static const std::set<std::string> ship_units = {
                "destroyer", "frigate", "torpedo boat", "battleship", "yamato", "fishing boat",
                "transport ship", "aircraft carrier", "aircraft carrier2"};
            static const std::set<std::string> plane_units = {
                "fighter", "biplane", "jet fighter", "bomber", "heavy bomber", "b29"};
            bool is_ship = ship_units.count(finished) != 0;
            // Spawn from whichever side of the footprint is closest to the
            // rally point (gather_x/gather_y -- always a real point, never
            // unset: spawn_building seeds it to just below the building,
            // see world.cpp, so this naturally reduces to the old fixed
            // bottom-only behavior whenever the player hasn't moved the
            // rally point elsewhere), skipping any side that's currently
            // obstructed (World::passable) so a unit doesn't try to walk
            // out through a wall of adjacent buildings/units. footprint_dy
            // accounts for tower/aa tower/outpost's footprint sitting below
            // their sprite anchor (World::footprint_dy's own comment) --
            // moot for the unit-training buildings this actually runs for
            // today, but keeps the math correct if that ever changes.
            double fcy = b.common.y + footprint_dy(b.name);
            double hw = b.foot_w * 0.5, hh = b.foot_h * 0.5;
            // The transport ship and the aircraft carriers are big enough that a
            // one-tile side offset would drop them ON TOP of the shipyard, so they
            // launch FAR outward -- straight along the direction to the player's
            // rally flag -- clearing both the dock and their own long hull. Normal
            // ships/land units keep the tight "nearest clear side to the rally"
            // placement.
            bool big_ship = (finished == "transport ship" || finished == "aircraft carrier" ||
                             finished == "aircraft carrier2");
            double bx, by;
            double clear_r = BTILE;
            if (big_ship) {
                double dx = b.gather_x - b.common.x, dy = b.gather_y - fcy;
                double d = std::hypot(dx, dy);
                if (d < 1e-6) { dx = 0.0; dy = 1.0; d = 1.0; } // no rally set -> straight down
                double reach = std::max(hw, hh) + 5.0 * BTILE; // past the dock + the hull
                bx = b.common.x + dx / d * reach;
                by = fcy + dy / d * reach;
                clear_r = 2.0 * BTILE; // search a wider patch of open water
            } else {
                const double side_x[4] = {b.common.x, b.common.x, b.common.x - hw - BTILE,
                                          b.common.x + hw + BTILE};
                const double side_y[4] = {fcy + hh + BTILE, fcy - hh - BTILE, fcy, fcy};
                bx = b.common.x;
                by = fcy + hh + BTILE; // default: bottom, same as before this change
                double best_d = 1e18;
                for (int i = 0; i < 4; ++i) {
                    if (!world.passable(/*is_air=*/false, is_ship, side_x[i], side_y[i])) continue;
                    double dx = side_x[i] - b.gather_x, dy = side_y[i] - b.gather_y;
                    double d = dx * dx + dy * dy;
                    if (d < best_d) { best_d = d; bx = side_x[i]; by = side_y[i]; }
                }
            }
            auto [sx, sy] = world.clear_point_near(bx, by, clear_r, is_ship);
            EntityRef uref = world.spawn_unit(finished, b.common.team, sx, sy);
            if (Unit* u = world.get(uref)) {
                // A rally point placed directly onto a resource, a workable
                // farm, or an own foundation (see GameClient::
                // right_click_order) sends a freshly-trained gatherer
                // straight into that job instead of just walking to the
                // point and standing idle -- re-checked here (not trusted
                // from when the rally was set) since the target may have
                // died/been finished/exhausted in the meantime.
                bool sent_to_job = false;
                if (u->is_gatherer && b.rally_target.valid()) {
                    if (b.rally_target.kind == EntityKind::Building) {
                        if (Building* tb = world.get_building(b.rally_target); tb && tb->common.alive) {
                            if (!tb->complete) {
                                u->build_target = b.rally_target;
                                u->gather_target = kNullRef;
                                u->gather_rtype = -1;
                                sent_to_job = true;
                            } else if (tb->name == "farm" && !tb->exhausted) {
                                world.order_gather(uref, b.rally_target);
                                sent_to_job = true;
                            }
                        }
                    } else if (b.rally_target.kind == EntityKind::Resource) {
                        if (Resource* tr = world.get_resource(b.rally_target); tr && tr->common.alive) {
                            world.order_gather(uref, b.rally_target);
                            sent_to_job = true;
                        }
                    }
                }
                // Airbase "park new planes" toggle: a freshly-built plane parks
                // at this airbase (kept down via `stationed`) instead of taking
                // off -- but only while a slot is free (capacity 5); otherwise it
                // launches as usual. A parked plane skips the rally move_goal.
                bool parked = false;
                if (plane_units.count(finished) && b.park_new_planes) {
                    int landed_here = 0;
                    for (auto ref2 : world.active_units) {
                        Unit* p = world.get(ref2);
                        if (p && p->common.alive && p->common.is_air && p->landed &&
                            p->home_id == b.common.id)
                            ++landed_here;
                    }
                    if (landed_here < 5) {
                        u->stationed = true; // stay parked until re-ordered
                        u->landed = true;
                        u->home_id = b.common.id;
                        u->height = 0.0;
                        u->move_goal.reset();
                        parked = true;
                    }
                }
                // Heavy bombers grab an available atomic bomb from THIS airbase as
                // they take off, so a freshly-built (or newly-launched) bomber is
                // armed with the nuke it was built to carry without first landing.
                if ((finished == "heavy bomber" || finished == "b29") && b.nuke_count > 0 &&
                    !u->nuke_loaded) {
                    b.nuke_count--;
                    u->nuke_loaded = true;
                    u->clip_ammo = 1;
                    u->home_id = b.common.id;
                }
                if (!sent_to_job && !parked) u->move_goal = Vec2{b.gather_x, b.gather_y};
                // AI-comparison metric -- is_gatherer (not just "!= civilian")
                // so fishing boats/transport ships don't get miscounted as
                // military, matching the same split ai_tick's civ_count/
                // military_by_team classification already uses.
                if (!u->is_gatherer) team.military_units_created++;
                team.units_created_by_name[finished]++;
            }
            team.score += 10.0; // scripts/give_points.gml
            if (b.common.team == 0) {
                // objects/obj_building/Step.gml:655: planes get their own
                // "snd_plane" creation sound, distinct from the generic
                // military-unit spawn sound.
                std::string sound = finished == "civilian"      ? "villager_spawn"
                                     : plane_units.count(finished) ? "plane_spawn"
                                                                    : "military_spawn";
                world.events.push({EventType::Sound, sound, 0, 0, 0, kNullRef, ""});
                world.events.push({EventType::Notify, "unit_created", 0, 0, 0, kNullRef, finished});
            }
        }
    }
}

} // namespace ww::sim
