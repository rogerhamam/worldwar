// World-coupled Control methods: recompute/check_win/AI/research/trade.
// Split from control.cpp (which stays World-independent) because these
// need entities, the spatial grid, and the event bus. See control.h's
// header comment.
#include "sim/control.h"
#include "sim/world.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace ww::sim {

namespace {
// How often ai_tick's shared heartbeat gets a chance to check every AI team
// at all (Control::ai_timer_) -- fine-grained enough that Hard's own
// interval below still fires close to on time. Lowered to match Hard's
// interval exactly (both 0.05s): that's the practical floor for a still-
// logic-only speedup -- below it there's no meaningfully "faster" reaction
// left to give, and every AI team's own bookkeeping scan runs on every
// heartbeat regardless of which team is actually due, so this is also
// about as often as that scan should run.
constexpr double kAiHeartbeat = 0.05;

// Team::difficulty (0=Easy/1=Normal/2=Hard, MenuController::kDifficultyNames)
// -> how many real seconds must pass between this team's own economy/build/
// research/train/offense passes. This is the whole difficulty lever: same
// script, same numbers, just re-evaluated faster (Hard) or slower (Easy) --
// a slower team leaves idle villagers/military sitting longer, mistimes
// age-ups, and reassigns dead objectives later, without any stat cheating.
// Normal's 1.0s matches the fixed cadence every difficulty used before this
// existed, so campaign teams (which never set difficulty, staying at the
// Team::difficulty default of 1) are completely unaffected. Hard is pushed
// as low as the shared heartbeat can actually service (kAiHeartbeat) --
// as fast as this decision-cadence lever can go without inventing a finer
// heartbeat than the sim needs for anything else.
// Does this team's ai_variant include candidate feature `feature`?
//
// PROMOTED, 2026-08-09. Features 3 (army massing), 5 (bank only on the final
// stretch), 6 (counter-composition) and 8 (value-based research) are now
// BASELINE -- this returns true for them regardless of variant.
//
// Each measured positive but not significant alone over 336 tournament matches:
// 54.7%, 52.7%, 55.4% (feature 8 never got a clean solo read). They are
// independent levers -- when to attack, when to spend, what to build, what to
// research -- so they were combined as variant 9 and measured together:
//
//     456 matches, 437 decided: candidate 294, baseline 143
//     = 67.3% of decided games, 95% CI 62.8-71.5, all 19 shards won
//
// The interval clears 50% by a wide margin, so the combination ships. Kept as a
// function rather than deleting the gates so the four are still named, still
// individually findable, and can be switched back off one at a time if a later
// regression needs bisecting -- flip the entry here, don't hunt call sites.
//
// ---- PROMOTED, 2026-08-13. Features 10, 11, 16 and 19 are now BASELINE ----
//
// A second round of candidates, all economic, all skirmish-only (every one is
// additionally gated on Team::ai_map_derive at its call site, so campaign AI's
// hand-authored balance is untouched):
//
//   10  pop-cap headroom -- stop hard-capping housing at 12 houses
//   11  every town centre trains villagers, not just the first
//   16  build housing AHEAD of the cap instead of once it is hit
//   19  rebalance the workforce toward ANY under-quota resource, damped
//
// Measured as the candidate share of DECIDED games against the then-baseline
// (tournament mode, ostland, 200 pop, --vary-civs, sides alternated, 95% Wilson
// interval). Individually:
//
//   10   192 matches  55.4%  48.2-62.3   positive, interval includes 50
//   11   192 matches  53.8%  46.6-60.9   positive, interval includes 50
//   16   192 matches  57.4%  50.3-64.3   WINS on its own
//   19   192 matches  56.1%  49.0-63.0   positive, interval includes 50
//
// None of the four is decisive alone, so they were combined and measured
// together, exactly as features 3/5/6/8 were before them:
//
//   10+11+16       336 matches  61.3%  55.9-66.3
//   10+11+16+19    192 matches  69.8%  63.0-75.8   <- shipped
//
// The four-feature interval clears 50% by a wide margin and beats the
// three-feature bundle, so the damped rebalance ships with the rest.
//
// ---- and the ones that did NOT ship, which is the more useful half --------
//
//   12  focus fire / target priority   192 matches  50.0%  42.9-57.1
//   13  villagers repair buildings     192 matches  44.6%  36.6-52.9
//   15  the UNDAMPED rebalance         192 matches  28.6%  22.6-35.5
//   14  bundle of 10-13                336 matches  44.7%  39.5-50.1
//   17  bundle of 10, 11, 15, 16       336 matches  43.4%  38.1-48.8
//
// Still reachable as `--candidate-variant 12/13/15` so a later attempt can
// bisect against them rather than rediscover them. Two lessons are worth more
// than the numbers:
//
// AN ECONOMIC GAIN IS NOT AUTOMATICALLY A WIN. Bundle 17 did exactly what it
// was built to do -- idle town-centre time down 250s a match, peak villagers
// +5.8, peak army +2.2, population +5.5 -- and lost significantly anyway. Split
// by match length it won 34% of games decided inside 15 minutes and 47% of
// longer ones. These matches average ~17 sim-minutes and end at era 1.5, so an
// investment that pays off later is bought with the exact tempo that decides
// them; average era went DOWN 0.2 while every economic measure went up.
//
// A CORRECTION COSTS A TRIP. Feature 15 is the worst result ever measured on
// this AI, and it is a "bug fix": it made the half-implemented workforce
// rebalance symmetric. Every reassignment makes a villager abandon its current
// trip, and steering off a quota that a food emergency rewrites several times a
// minute turned that into an oscillator. The same idea, damped (feature 19),
// ships. When touching villager assignment, count the moves per match.
//
// Variant 99 turns all four promoted features back OFF -- the AI exactly as it
// stood before this promotion -- so the change can be re-measured or bisected
// without reverting anything. `--candidate-variant 99` should read ~30%, the
// mirror of the 69.8% above.
//
// ---- UNMEASURED CANDIDATES, 2026-08-14: features 20-24 --------------------
//
// Written without a tournament behind them -- NOTHING below has been measured,
// and on this AI's record roughly half of everything that looked obviously
// right has lost its A/B (see 13, 15 and 17 above). Treat each as a hypothesis
// with an argument attached, not as an improvement. All five are additionally
// gated on Team::ai_map_derive at their call sites, so campaign AI is untouched.
//
//   20  counter-composition against the enemy DISTRIBUTION, not its mean --
//       and stop the counter pick silently clobbering the deliberate ones
//   21  age prerequisites for EVERY plan, and they jump the build queue
//   22  an air force on land maps: airbases past era 1, bombers sooner
//   23  stop buying villagers there is no job for; let the farm count follow
//   24  static defence that scales, faces the right way, and calls the army
//       home when the town centre itself is being hit
//
// Variant 25 is the BUNDLE of all five, following the 9 / 14 / 17 precedent:
// each of these is a small independent lever, and this AI's history is that
// individually-positive-but-not-significant levers only clear the interval when
// combined. Measure the bundle first; bisect with the individual numbers only if
// it wins, or to find the culprit if it loses.
//
//   headless_runner --tournament N --candidate-variant 25 ...   <- start here
//   headless_runner --tournament N --candidate-variant 20 ...   <- then bisect
//
// Pick a FRESH disjoint seed range (see the arena notes) so results can pool.
//
// ---- SHIPPED WITHOUT MEASUREMENT, 2026-08-14: features 30-35 --------------
//
// A second batch, and unlike 20-24 these are ON BY DEFAULT. That is a
// deliberate departure from this file's usual promote-on-evidence rule and the
// reason is worth writing down: every one of them comes from a direct report of
// what the AI does in a real game, and the fix is to a behaviour that is wrong
// on inspection rather than merely suspected of being suboptimal. An A/B cannot
// tell you whether feeding units into an enemy army one at a time is bad; it
// can only tell you whether the version that stops doing it wins more, which is
// a different and much noisier question.
//
//   30  never advance on the enemy alone -- reinforcements travel as a squad
//   31  shoot UNITS before buildings, and concentrate fire (promotes 12)
//   32  siege stands off behind the line and never advances unescorted
//   33  back out of a fortress's arc on contact, not after taking fire
//   34  production capacity scales with the bank -- stop hoarding
//   35  build the WHOLE roster, unique units and heavy armour included
//
// `--candidate-variant 98` turns all six back OFF, i.e. the AI exactly as it
// stood before this batch, so the batch can still be measured against its own
// predecessor without reverting anything (same trick as 99 for the 10/11/16/19
// promotion). Expect a tournament at 98 to read BELOW 50% if these are right.
bool variant_has(int variant, int feature) {
    if (feature == 3 || feature == 5 || feature == 6 || feature == 8) return true;
    if (feature == 10 || feature == 11 || feature == 16 || feature == 19) return variant != 99;
    if (feature >= 20 && feature <= 24) return variant == feature || variant == 25;
    // (A 2026-08-15 note claiming this batch caused heap corruption was WRONG
    // and has been removed. The "crashes" were headless_runner segfaulting
    // under Git Bash, which it does on any seed and any build -- an environment
    // artifact, not a defect. Run the harness from PowerShell; under it every
    // one of those configurations exits 0. The batch remains unmeasured, which
    // is a different and much smaller problem.)
    if (feature >= 30 && feature <= 35) return variant != 98;
    if (variant == feature) return true; // 12/13/15: kept measurable, not shipped
    return false;
}

// A weapon long enough that its owner should be firing from BEHIND the line
// rather than standing in it: artillery is range 10, heavy artillery the same,
// the ballistic missile 16. Everything the AI masses -- muscateer 3, rifleman 4,
// tanks 5-6 -- is comfortably under this, so the test cleanly separates "siege"
// from "the army" without a name list that would drift as the catalog changes.
constexpr double kSiegeRange = 9.0 * TILE;
// How close counts as "together" for every group rule below (feature 30/32).
// Six tiles is roughly a screen-and-a-half of frontage at this zoom: close
// enough that the units are in the same fight, wide enough that a wave spread
// by the golden-angle fan-out below still reads as one group.
constexpr double kSquadRadius = 6.0 * TILE;

double ai_decision_interval(int difficulty) {
    switch (difficulty) {
        case 0: return 1.6;   // Easy
        // Hard AND Hardest: as fast as the shared heartbeat can service. Hardest
        // does not get a shorter interval because there isn't one to give -- it
        // is already the floor; its extra strength is the handicaps instead
        // (training speed, free era techs -- see Team::difficulty).
        case 2:
        case 3: return 0.05;
        default: return 1.0;  // Normal
    }
}
} // namespace

void Control::recompute(World& world) {
    for (auto& t : teams) { t.pop = 0; t.cap = 0; t.has_base = false; }
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (!u || !u->common.alive || u->common.team < 0 || u->common.team >= 8) continue;
        teams[u->common.team].pop += pop_cost(u->name);
    }
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive || b->common.team < 0 || b->common.team >= 8) continue;
        Team& t = teams[b->common.team];
        if (b->name == "base") { t.cap += 8; t.has_base = true; }
        else if (b->name == "house" && b->complete) t.cap += (t.civ == 3 ? 5 : 4);
    }
    for (auto& t : teams) {
        if (t.cap > max_pop) t.cap = max_pop;
    }
}

void Control::check_win(World& world) {
    if (game_over) return;
    std::vector<int> alive;
    for (int i = 0; i < n; ++i) {
        if (teams[i].has_base) alive.push_back(i);
    }
    // Group surviving teams by alliance -- the match ends once at most one
    // alliance still has a base standing anywhere, so an ally can carry a
    // fallen teammate's side to victory rather than every single team
    // needing its own base alive.
    std::set<int> groups;
    for (int i : alive) groups.insert(teams[i].ally);
    if (static_cast<int>(groups.size()) <= 1) {
        game_over = true;
        winner = alive.empty() ? std::nullopt : std::optional<int>(alive[0]);
        world.events.push({EventType::MusicStop, "", 0, 0, 0, kNullRef, ""});
        // "Did team 0's alliance win", not "is team 0 literally alive[0]" --
        // team 0 may itself have fallen while an ally carried the group.
        bool team0_won = !alive.empty() && teams[0].ally == teams[alive[0]].ally;
        world.events.push({EventType::Sound, team0_won ? "victory" : "lose", 0, 0, 0, kNullRef, ""});
    }
}

void Control::update_metrics(double dt, World& world) {
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive || !b->complete || b->name != "base") continue;
        if (b->common.team < 0 || b->common.team >= 8) continue;
        if (b->queue.empty()) teams[b->common.team].idle_tc_seconds += dt;
    }
    std::unordered_map<int, int> army_count, vil_count;
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (!u || !u->common.alive) continue;
        if (u->common.team < 0 || u->common.team >= 8) continue;
        if (u->is_gatherer) {
            bool idle = u->carry == 0 && !u->gather_target.valid() && !u->build_target.valid() &&
                        !u->attack_target.valid() && !u->forced && !u->rally.has_value();
            if (idle) teams[u->common.team].idle_villager_seconds += dt;
        }
        if (u->name == "civilian") vil_count[u->common.team]++;
        else if (!u->is_gatherer) army_count[u->common.team]++;
    }
    for (auto& [tm, c] : army_count) teams[tm].peak_army_size = std::max(teams[tm].peak_army_size, c);
    for (auto& [tm, c] : vil_count) teams[tm].peak_vil_count = std::max(teams[tm].peak_vil_count, c);

    // ---- post-game stats: advance the match clock and sample population ----
    stats_elapsed += dt;
    stats_sample_acc += dt;
    if (pop_samples.empty() || stats_sample_acc >= kPopSampleInterval) {
        stats_sample_acc = 0.0;
        // Total living units per team (villagers + army + everything else) --
        // the population curve the Timeline tab stacks. Counts all alive units,
        // matching the reference's "population" line.
        PopSample s;
        s.t = stats_elapsed;
        for (auto ref : world.active_units) {
            Unit* u = world.get(ref);
            if (!u || !u->common.alive) continue;
            if (u->common.team < 0 || u->common.team >= 8) continue;
            s.pop[u->common.team]++;
        }
        pop_samples.push_back(s);
    }
}

void Control::update_ai(double dt, World& world) {
    recompute(world);
    check_win(world);
    // Per-team per-tick clocks that are NOT part of the AI decision cadence and
    // so must run for the human team too -- note that ai_under_fire deliberately
    // is NOT here: it is wound down inside ai_tick on each AI team's own
    // interval, and nothing reads it for team 0.
    //
    // This is also where the "you are under attack" alert is raised. World::hurt
    // only stamps warn_pending/warn_x/warn_y (see the comment there: emitting a
    // heap-allocating event from inside the damage primitive was the source of
    // intermittent heap corruption). Here we are at the top of the tick, so
    // pushing is as safe as check_win's own events.
    constexpr double kWarnCooldown = 10.0; // seconds between alerts per team
    for (int i = 0; i < n; ++i) {
        Team& t = teams[i];
        if (t.warn_cd > 0.0) t.warn_cd = std::max(0.0, t.warn_cd - dt);
        if (!t.warn_pending) continue;
        t.warn_pending = false;
        if (t.warn_cd > 0.0) continue; // still inside this episode's cooldown
        t.warn_cd = kWarnCooldown;
        SimEvent ev{EventType::Notify, "under_attack", t.warn_x, t.warn_y, 0, kNullRef, ""};
        ev.team = i;
        world.events.push(ev);
    }
    ai_timer_ += dt;
    if (ai_timer_ >= kAiHeartbeat) {
        double elapsed = ai_timer_;
        ai_timer_ = 0.0;
        ai_tick(world, elapsed);
    }
}

void Control::ai_tick(World& world, double elapsed) {
    std::unordered_map<int, std::vector<EntityRef>> buildings_by_team, military_by_team;
    std::unordered_map<int, int> civ_count;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (b && b->common.alive && b->common.team >= 0) buildings_by_team[b->common.team].push_back(ref);
    }
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (!u || !u->common.alive || u->common.team < 0) continue;
        if (u->name == "civilian") civ_count[u->common.team]++;
        else if (!u->is_gatherer) military_by_team[u->common.team].push_back(ref);
    }
    std::vector<EntityRef> bases;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (b && b->common.alive && b->name == "base") bases.push_back(ref);
    }

    // Starts at team 0 (not 1): a normal skirmish's team 0 is human
    // (Team::is_ai left false by new_skirmish), so it's skipped here by the
    // is_ai guard exactly as before -- but a spectator match
    // (SkirmishSettings::spectator, "Start Stress Test") sets team 0's
    // is_ai TRUE too, and used to get no economy/build/train/defense at all
    // despite that flag, since this loop simply never reached it. Also what
    // self-play/tournament matches (two AI teams, neither human) rely on.
    for (int team = 0; team < n; ++team) {
        Team& td = teams[team];
        if (!td.is_ai || !td.alive) continue;
        // Difficulty gate: this team only actually re-evaluates anything
        // once its own interval has elapsed, independent of every other
        // team (see ai_decision_interval above).
        td.ai_tick_accum += elapsed;
        if (td.ai_tick_accum < ai_decision_interval(td.difficulty)) continue;
        double team_dt = td.ai_tick_accum; // real time since this team last ran
        td.ai_tick_accum = 0.0;
        auto& blds = buildings_by_team[team];
        // Run the "under fire" clock down (World::hurt winds it back up to
        // kUnderFireWindow on every point of damage) and publish this team's
        // army size, so the economy/build/train passes below all read the same
        // two numbers when they decide whether this team is being overrun.
        td.ai_under_fire = std::max(0.0, td.ai_under_fire - team_dt);
        td.ai_army = static_cast<int>(military_by_team[team].size());

        // "Hardest": hand this team its starting era's entire tech tree, once.
        // Done here rather than at scenario setup so it covers every entry point
        // (skirmish, campaign, spectator, the arena) without each having to
        // remember, and so a level that starts at a later age gets that age's
        // tree rather than Victorian's.
        if (td.difficulty >= 3 && !td.hardest_seeded) {
            td.hardest_seeded = true;
            grant_era_techs(team, world);
        }

        // ---- derelict-foundation watchdog --------------------------------
        // A foundation whose construction % hasn't moved is one nobody is
        // raising: unreachable, or its builder keeps giving up (unit_behavior
        // drops build_target after two failed replans). Two things go wrong if
        // nothing watches for it. It sits on the map half-built for the rest of
        // the match -- which the player called out directly -- and, worse, it
        // permanently occupies a slot under ai_build's work-in-progress cap, so
        // the ENTIRE build order jams behind it. Measured before this watchdog:
        // three teams in a 40-instance run ended at era 0 with zero or one
        // age-qualifying building and thousands of banked resources, purely
        // because one stuck foundation blocked everything else forever.
        //
        // So: past kStallFree it stops counting against the cap (the build
        // order moves on), and past kStallCancel it is refunded and removed.
        // Walls are exempt -- a choke line is dozens of segments raised in
        // sequence by one chaining villager, so the untouched ones are stalled
        // by design, not stuck.
        constexpr double kStallCancel = 45.0;
        for (auto ref : blds) {
            Building* b = world.get_building(ref);
            if (!b || !b->common.alive || b->complete) continue;
            if (b->name == "palisade" || b->name == "iron wall") continue;
            if (b->construction > b->ai_stall_con + 1e-6) {
                b->ai_stall_con = b->construction;
                b->ai_stall_t = 0.0;
                continue;
            }
            b->ai_stall_t += team_dt;
            if (b->ai_stall_t < kStallCancel) continue;
            // Refund what was never spent on progress, then poof it (deleted =>
            // dust, no rubble, no buildings_lost -- same path as the player
            // cancelling a foundation with Delete).
            double unspent = (100.0 - b->construction) / 100.0;
            for (auto& [k, v] : cost_of(b->name, team)) add(team, k, v * unspent);
            b->deleted = true;
            world.hurt(ref, b->common.hp);
        }

        std::vector<std::string> names;
        for (auto ref : blds) {
            Building* b = world.get_building(ref);
            if (b && b->common.alive) names.push_back(b->name);
        }
        EntityRef base = kNullRef;
        for (auto ref : blds) {
            if (world.get_building(ref)->name == "base") { base = ref; break; }
        }
        // Skirmish AI reads the map once and locks in a Victorian plan
        // (playstyle, villager/fishing goals, a choke to wall). Retries next
        // tick if the base wasn't resolved yet (assessed stays false).
        if (td.ai_map_derive && !td.ai_plan.assessed) ai_assess_map(team, world, base);
        // Read the opponent FIRST -- economy, build and train below all consult
        // the same snapshot, so it has to be refreshed before any of them run.
        ai_read_enemy(team, world, base, team_dt);
        ai_economy(team, world, base, civ_count[team], blds);
        ai_build(team, world, base, names);
        ai_build_walls(team, world, base);
        ai_research(team, world, blds);
        ai_train(team, world, blds, military_by_team[team]);
        // Defense reflex runs BEFORE the offensive push below: it hands a
        // recalled unit a rally straight to the threat, and the offensive
        // loop's own idle check (`!u->rally.has_value()`) then skips
        // reassigning that same unit to the forward objective this tick --
        // so a raid always wins the tug-of-war for anything close enough to
        // home to matter, without needing two competing systems to agree.
        ai_defend(team, world, blds, military_by_team[team]);
        ai_defend_civilians(team, world);

        // Campaign AI controls (skirmish leaves these at their defaults, so it
        // is unaffected). A bespoke per-level `ai_profile` gets first refusal
        // via ai_profile_manage(); if it fully handles this team this tick it
        // returns true and we skip the generic logic below. Otherwise the
        // per-player `ai_behavior` preset just tunes how aggressive the generic
        // offensive push is.
        if (!td.ai_profile.empty() &&
            ai_profile_manage(td.ai_profile, team, world, base, military_by_team[team])) {
            continue;
        }
        // ai_behavior preset -> attack threshold (how big the army must be
        // before it commits). "passive" never attacks; "rusher"/"aggressive"
        // commit early; "defensive" waits for a big army; "balanced"/"default"
        // keep the standard threshold.
        const std::string& beh = td.ai_behavior;
        // Bigger thresholds than before so the AI masses a real army and hits
        // with a coordinated WAVE instead of feeding units in a few at a time
        // (see the muster below, which groups them at home until this is met).
        size_t attack_threshold = 12;
        bool never_attack = false;
        if (beh == "passive") never_attack = true;
        else if (beh == "rusher") attack_threshold = 5;
        else if (beh == "aggressive") attack_threshold = 8;
        else if (beh == "defensive") attack_threshold = 20;
        // ---- let the read of the opponent move that threshold ---------------
        // A fixed threshold attacks on OUR schedule and ignores his entirely.
        // Two readings genuinely change what the right moment is:
        //
        //  * He is booming -- little army, nothing committed at us. This is the
        //    window a human player attacks into, and the old AI sat through it
        //    massing to 12 while he built the economy that would beat it. Drop
        //    the bar to just above what he could defend with.
        //  * A committed push of his is inbound. Marching past each other trades
        //    our army for his buildings and loses the base; hold, let it break
        //    on our defences, then leave. Raising the bar keeps the force home
        //    without needing a separate "defend" state machine, because the
        //    muster point below IS home.
        const auto& rd = td.ai_read;
        using ThreatKind = Team::EnemyRead::Threat;
        if (rd.valid && !never_attack) {
            if (rd.threat == ThreatKind::None && rd.mil_ratio < 0.15 && td.era >= 1) {
                attack_threshold = std::max<size_t>(4, static_cast<size_t>(rd.mil) + 3);
            } else if (rd.threat == ThreatKind::Push && !td.ai_committed) {
                attack_threshold = std::max<size_t>(attack_threshold,
                                                    static_cast<size_t>(rd.incoming) + 4);
            }
        }
        // ---- HARD FLOOR: never attack with less than a wave --------------
        // Every adjustment above can only LOWER the bar, and the booming-enemy
        // branch in particular drops it to as few as four. Attacking with four
        // is how an army gets spent a handful at a time without ever fighting a
        // battle it could win. Past Victorian the floor is a real wave; in
        // Victorian, where armies are small, nobody has defences and the whole
        // era is meant to be short, a smaller group is a legitimate attack.
        constexpr size_t kWaveEarly = 4;  // Victorian: more than three
        constexpr size_t kWaveLate = 10;  // Industrial and beyond
        if (!never_attack)
            attack_threshold = std::max(attack_threshold, td.era >= 1 ? kWaveLate : kWaveEarly);

        // Offensive push: once a real army has massed, commit every idle unit
        // to attack instead of letting them stand around at home. They fan out
        // around the target (a per-unit ring offset) so the whole force doesn't
        // A*-path onto one pixel and jam -- the main cause of the "massed but
        // motionless" look. Idle units (no attack_target, no rally) are
        // (re)assigned every tick, so a unit that reaches a dead objective
        // immediately gets a fresh one instead of going quiet.
        auto& force = military_by_team[team];
        // Cosmetic early-game scouting: peel one spare unit off to sweep the
        // map. Runs before the offensive push so the scout's standing rally
        // makes that loop skip it; released back once era>0 or committed.
        ai_scout(team, world, force);
        // Hysteresis (Team::ai_committed, promoted from the ai_variant==1
        // candidate after a 12-match tournament: 75% win rate, and -- more
        // tellingly -- EVERY match that ended in an actual base destruction
        // rather than a timeout went to this side). Without it, the instant
        // a real battle thinned the force back below attack_threshold the
        // push froze completely, even mid-victory -- a diagnostic replay
        // showed both bases sitting at full HP for a full 20-minute match
        // despite a battle that killed 19 of one side's units. Once
        // committed, keeps going with whatever survives until the force is
        // fully wiped, rather than re-checking the threshold every tick.
        if (force.empty()) td.ai_committed = false;
        else if (force.size() >= attack_threshold) td.ai_committed = true;
        // PROMOTED (was ai_variant == 3, see variant_has): also release the commitment once a push
        // has been GROUND DOWN, rather than only when it is wiped to the last
        // man.
        //
        // The wipe-only reset above has a failure mode the arena data shows
        // plainly: across 320 self-play matches each side produced ~117
        // military units and lost ~113 of them (96%), yet peak army size only
        // ever reached ~25-31. Units were not dying in big battles -- they were
        // dying one at a time. Once committed, the rally target is the enemy
        // base for every idle unit, so a push that has been beaten down to two
        // survivors STAYS committed, and each freshly-trained unit walks alone
        // across the map into the enemy army the moment it leaves the barracks.
        // The force can then never climb back to attack_threshold, so the
        // "muster into a wave" logic below never gets to run again for the rest
        // of the match. 65% of matches hit the 40-minute cap and 27.5% were
        // outright draws.
        //
        // Releasing at a third of the threshold keeps what the hysteresis was
        // added for -- a push thinned by a costly battle but still a real force
        // presses on, including mid-victory -- while a spent push falls back to
        // the muster point and re-forms with new production instead of feeding
        // itself in piecemeal.
        else if (variant_has(td.ai_variant, 3) && td.ai_committed &&
                 force.size() <= attack_threshold / 3) {
            td.ai_committed = false;
        }
        if (!never_attack) {
            // Home position (our base) and the ONE team objective: the enemy base
            // nearest our own, else any enemy building, else any enemy unit -- so
            // the army keeps hunting even after the enemy's bases are gone.
            double hx = 0.0, hy = 0.0;
            if (Building* mb = base.valid() ? world.get_building(base) : nullptr) {
                hx = mb->common.x; hy = mb->common.y;
            } else if (!force.empty()) {
                if (Unit* fu = world.get(force.front())) { hx = fu->common.x; hy = fu->common.y; }
            }
            double ox = hx, oy = hy;
            EntityRef obj = kNullRef;
            double bestd = 1e18;
            for (auto bref : bases) {
                Building* bb = world.get_building(bref);
                if (!bb || allied(bb->common.team, team)) continue;
                double d = (bb->common.x - hx) * (bb->common.x - hx) +
                           (bb->common.y - hy) * (bb->common.y - hy);
                if (d < bestd) { bestd = d; obj = bref; }
            }
            if (!obj.valid()) {
                double R = std::hypot(world.px_w, world.px_h);
                obj = world.nearest(hx, hy, R, [&](EntityRef, EntityCommon& c) {
                    return c.alive && c.team >= 0 && !allied(c.team, team) &&
                           (c.kind == EntityKind::Unit || c.kind == EntityKind::Building);
                });
            }
            if (EntityCommon* oc = obj.valid() ? world.common(obj) : nullptr) {
                ox = oc->x;
                oy = oc->y;
            }
            // Rally point: the enemy objective once COMMITTED (attack together),
            // else a MUSTER point ~6 tiles out from home toward the enemy, so
            // freshly-built units gather into a group instead of trickling toward
            // the front one at a time. When the massed force hits the threshold,
            // ai_committed flips and the whole group is redirected at the enemy.
            double mx = hx, my = hy; // muster point, always defined
            {
                double dx = ox - hx, dy = oy - hy, d = std::hypot(dx, dy);
                if (d < 1e-6) d = 1.0;
                mx = hx + dx / d * 6.0 * TILE;
                my = hy + dy / d * 6.0 * TILE;
            }
            double tx = td.ai_committed ? ox : mx;
            double ty = td.ai_committed ? oy : my;

            // ---- REINFORCEMENTS TRAVEL AS A WAVE TOO -------------------------
            // The muster only ever applied BEFORE the first commitment. Once
            // ai_committed latched, the rally for every idle unit became the
            // enemy objective -- including units that had just walked out of the
            // barracks, one at a time, minutes after the wave left. Each one
            // crossed the map alone into a standing army. That is the trickle:
            // the AI can produce 100+ units in a match and never have more than
            // ~25 alive at once, because they are consumed in ones and twos.
            //
            // So a unit still at home while a push is away does NOT chase it.
            // It waits at the muster point until enough of them have gathered to
            // be a wave in their own right, then they all leave together. Units
            // already forward keep their objective and are unaffected.
            constexpr double kAtHome = 12.0 * TILE;
            size_t home_group = 0;
            for (auto uref : force) {
                Unit* u = world.get(uref);
                if (!u || !u->common.alive) continue;
                if (std::hypot(u->common.x - mx, u->common.y - my) <= kAtHome) ++home_group;
            }
            bool release_home = home_group >= attack_threshold;

            int idx = 0;
            for (auto uref : force) {
                Unit* u = world.get(uref);
                if (!u) { ++idx; continue; }
                if (u->attack_target.valid() || u->rally.has_value() || u->attack_ground.has_value()) {
                    ++idx;
                    continue;
                }
                // Committed, but this one is still at home and the group behind
                // it isn't a wave yet -- hold it at the muster point.
                bool at_home = std::hypot(u->common.x - mx, u->common.y - my) <= kAtHome;
                double gx = tx, gy = ty;
                if (td.ai_committed && at_home && !release_home) { gx = mx; gy = my; }
                // Enemy across water: a LAND unit can't rally to the objective
                // (its route dead-ends at the shore), so leave it for
                // ai_amphibious to ferry over. Ships/aircraft cross on their own
                // and still push directly here.
                if (!td.ai_plan.land_to_enemy && !u->common.is_ship && !u->common.is_air) {
                    ++idx;
                    continue;
                }
                // ---- NEVER ADVANCE ALONE (feature 30) ----------------------
                // The muster rule above has a hole, and it is the one that
                // produces the reported "it feeds me units one at a time". Both
                // the wave gate and the reinforcement gate test AT HOME -- 12
                // tiles of the muster point -- so they only ever hold a unit
                // that happens to be standing near the town centre. Anything
                // else gets the enemy objective: a unit trained at a forward
                // barracks, one that survived a wipe and is walking back, one
                // ai_defend recalled to a raid that then ended, one the fortress
                // rule pushed out of an arc. Each of those walks at the enemy
                // base by itself, arrives alone, and dies alone.
                //
                // So the test is not "where are you" but "who is with you".
                // Count friends within a squad's reach; below that, fall back to
                // the muster point and re-form. The one exception is a unit
                // already deeper into his half than ours -- it is committed
                // whether we like it or not, and turning it round mid-map just
                // means dying tired.
                //
                // Cheap despite the nested loop: this only runs for units that
                // reached here, i.e. ones with no attack target and no standing
                // rally, which after the first tick of a push is a handful.
                if (variant_has(td.ai_variant, 30) && td.ai_committed && !at_home) {
                    size_t squad = 0;
                    for (auto oref : force) {
                        Unit* o = world.get(oref);
                        if (!o || !o->common.alive) continue;
                        if (std::hypot(o->common.x - u->common.x, o->common.y - u->common.y) <=
                            kSquadRadius)
                            ++squad;
                    }
                    // Includes itself, so this is "three friends" at the default.
                    size_t need = std::min<size_t>(4, std::max<size_t>(2, attack_threshold));
                    double d_obj = std::hypot(u->common.x - ox, u->common.y - oy);
                    double d_home = std::hypot(u->common.x - hx, u->common.y - hy);
                    if (squad < need && d_obj > d_home) { gx = mx; gy = my; }
                }
                // ---- SIEGE STANDS OFF, AND NEVER ALONE (feature 32) --------
                // Artillery outranges everything the enemy fields by 4-6 tiles
                // and dies to anything that closes. The push treated it as
                // ordinary infantry -- same rally, same fan-out ring, straight
                // at the objective -- so it walked to the SAME point as the
                // riflemen, which is inside the range of everything defending,
                // and its entire range advantage was handed back.
                //
                // Two rules, both about not being where it can be reached:
                // it does not move up at all without infantry around it, and
                // when it does it stops short by most of its own reach, so the
                // line forms in front of it and it shells over the top.
                bool no_spread = false;
                if (variant_has(td.ai_variant, 32) && !u->common.is_air && !u->melee &&
                    u->range_px >= kSiegeRange) {
                    no_spread = true; // see the fan-out ring below
                    size_t escorts = 0;
                    for (auto oref : force) {
                        Unit* o = world.get(oref);
                        if (!o || !o->common.alive || oref == uref) continue;
                        if (o->range_px >= kSiegeRange) continue; // another gun is not an escort
                        if (std::hypot(o->common.x - u->common.x, o->common.y - u->common.y) <=
                            kSquadRadius)
                            ++escorts;
                    }
                    if (escorts < 2) {
                        gx = mx; // wait at the muster point for a line to form
                        gy = my;
                    } else {
                        double dx2 = gx - u->common.x, dy2 = gy - u->common.y;
                        double d2 = std::hypot(dx2, dy2);
                        double standoff = u->range_px * 0.8;
                        if (d2 <= standoff) {
                            // Already in range of where it was going. Leave it
                            // with no rally at all so it holds position and
                            // shoots (update_combat / ai_focus_fire pick the
                            // target); handing it its own coordinates would just
                            // churn a rally it satisfies the same tick.
                            ++idx;
                            continue;
                        }
                        gx = u->common.x + dx2 / d2 * (d2 - standoff);
                        gy = u->common.y + dy2 / d2 * (d2 - standoff);
                    }
                }
                double ang = idx * 2.399963;                 // golden-angle fan-out
                // 2-6 tile spread ring -- EXCEPT for a gun that just computed a
                // stand-off point (feature 32): a 6-tile scatter on an 8-tile
                // stand-off can put it back inside the fight, which is the exact
                // thing the stand-off was for.
                double rad = no_spread ? 0.0 : (2.0 + (idx % 5)) * TILE;
                u->rally = Vec2{gx + std::cos(ang) * rad, gy + std::sin(ang) * rad};
                ++idx;
            }
            // Ballistic missiles are siege: advance PACKED (mobile) with the wave
            // via the rally above, then -- once the enemy objective is within
            // firing range -- order an attack-ground on it, which auto-deploys the
            // launcher and fires (see update_unit/update_combat). A DEPLOYED
            // launcher can't move (it's a fixed emplacement), so we only unpack
            // when actually in range, and re-stow it to keep advancing otherwise.
            if (td.ai_committed) {
                if (EntityCommon* tc = obj.valid() ? world.common(obj) : nullptr) {
                    // Spread missile fire across DISTINCT enemy targets near the
                    // objective instead of dumping every warhead on one spot
                    // (overkill). Round-robin the launchers over the target list.
                    std::vector<Vec2> spots;
                    for (auto r2 : world.grid.query(tc->x, tc->y, 8.0 * TILE)) {
                        EntityCommon* e = world.common(r2);
                        if (e && e->alive && e->team >= 0 && !allied(e->team, team) &&
                            (e->kind == EntityKind::Building || e->kind == EntityKind::Unit))
                            spots.push_back(Vec2{e->x, e->y});
                    }
                    int si = 0;
                    for (auto uref : force) {
                        Unit* u = world.get(uref);
                        if (!u || !u->is_ballistic) continue;
                        double dd = std::hypot(tc->x - u->common.x, tc->y - u->common.y);
                        if (dd <= u->range_px * 0.95) {
                            u->attack_ground = spots.empty() ? Vec2{tc->x, tc->y}
                                                             : spots[si++ % spots.size()];
                            u->rally.reset();
                        } else {
                            u->attack_ground.reset();
                            // deployed but out of range -> re-stow so it can advance
                            if (!u->packed && u->pack_t <= 0.0) { u->pack_target = true; u->pack_t = 5.0; }
                        }
                    }
                }
            }
            // Enemy across water: ferry the land army over on a transport (load
            // near home -> cross -> unload on the enemy shore). The push above
            // already sent ships/aircraft; this moves the land troops it skipped.
            ai_amphibious(team, world, base, force);
        }
        // LAST, so it overrides every rally handed out above: decide what the
        // army in contact actually shoots (ai_variant == 12). Outside the
        // never_attack guard on purpose -- a passive team still has to defend
        // itself, and picking targets is not attacking.
        ai_focus_fire(team, world, force);
        // Later still: the fortress rule overrides even that. Whatever the push
        // or the focus-fire allocation decided, a unit that cannot outrange a
        // fortress must not be standing in front of one.
        ai_siege(team, world, force);
    }
}

bool Control::ai_profile_manage(const std::string& profile, int team, World& world, EntityRef base,
                                 const std::vector<EntityRef>& force) {
    (void)team;
    (void)world;
    (void)base;
    (void)force;
    // No bespoke campaign AI profiles are wired up yet. Each one is added as a
    // branch here on request (e.g. `if (profile == "jena_defensive") { ... }`),
    // so a campaign can get hand-written AI without ever touching the generic
    // skirmish logic. Returning false means "not handled -> fall through to the
    // generic ai_behavior-tuned offensive push in ai_manage".
    (void)profile;
    return false;
}

void Control::ai_read_enemy(int team, World& world, EntityRef base, double dt) {
    Team& td = teams[team];
    Team::EnemyRead r;
    Building* home = base.valid() ? world.get_building(base) : nullptr;

    // ---- where does he live -------------------------------------------------
    // Nearest hostile base to ours. Needed before the unit sweep because "has he
    // crossed the map" is measured against the midpoint between the two.
    if (home) {
        double best = 1e18;
        for (auto ref : world.active_buildings) {
            Building* b = world.get_building(ref);
            if (!b || !b->common.alive || b->name != "base") continue;
            if (b->common.team < 0 || allied(b->common.team, team)) continue;
            double d = std::hypot(b->common.x - home->common.x, b->common.y - home->common.y);
            if (d < best) {
                best = d;
                r.base_known = true;
                r.base_x = b->common.x;
                r.base_y = b->common.y;
                r.base_dist = d;
            }
        }
    }

    // ---- what is he made of, and what is heading at us ----------------------
    // "Heading at us" is deliberately two independent tests, because either one
    // alone has a blind spot. Reading his units' ORDERS (rally for attack-move,
    // move_goal for a plain move) is the true statement of intent -- this AI is
    // omniscient, so it may as well use the real thing rather than infer from
    // motion -- but a unit that has already arrived and is mid-fight may hold no
    // order at all. The territorial test catches those: anything of his sitting
    // closer to our base than to his own has, by definition, come to us.
    double mid = r.base_known ? r.base_dist * 0.5 : 0.0;
    double nearest_incoming = 1e18;
    double slowest_speed = 0.0;
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (!u || !u->common.alive || u->common.team < 0 || allied(u->common.team, team)) continue;
        if (u->is_gatherer) { ++r.vil; continue; }
        ++r.mil;
        if (!home) continue;
        double d_us = std::hypot(u->common.x - home->common.x, u->common.y - home->common.y);
        bool ordered_at_us = false;
        // Within ~14 tiles of our town centre counts as "aimed at our base"
        // rather than at some outlying farm -- wide enough to catch a staging
        // point just outside, tight enough that a march past does not trip it.
        constexpr double kOrderedRadius = 14.0 * TILE;
        if (u->rally) {
            ordered_at_us = std::hypot(u->rally->x - home->common.x,
                                       u->rally->y - home->common.y) < kOrderedRadius;
        }
        if (!ordered_at_us && u->move_goal) {
            ordered_at_us = std::hypot(u->move_goal->x - home->common.x,
                                       u->move_goal->y - home->common.y) < kOrderedRadius;
        }
        bool crossed = r.base_known && d_us < mid;
        if (!ordered_at_us && !crossed) continue;
        ++r.incoming;
        if (d_us < nearest_incoming) {
            nearest_incoming = d_us;
            slowest_speed = u->speed_px;
        }
    }
    r.valid = (r.mil + r.vil) > 0;
    int total = r.mil + r.vil;
    r.mil_ratio = total ? static_cast<double>(r.mil) / total : 0.0;
    r.committed_frac = r.mil ? static_cast<double>(r.incoming) / r.mil : 0.0;
    if (r.incoming > 0 && slowest_speed > 1.0) r.incoming_eta = nearest_incoming / slowest_speed;

    // ---- army growth rate ---------------------------------------------------
    // Carried across ticks from the previous read. Exponentially smoothed
    // because a single tick's delta is mostly noise -- one unit finishing
    // training reads as a huge rate at a 1-second cadence.
    r.mil_prev = td.ai_read.mil;
    if (dt > 0.01 && td.ai_read.valid) {
        double inst = (r.mil - td.ai_read.mil) * 60.0 / dt; // units per minute
        r.mil_rate = td.ai_read.mil_rate * 0.7 + inst * 0.3;
    }

    // ---- classify the incoming force ---------------------------------------
    // The distinction the old bare-count threat lacked. A scout is one or two
    // units that are a small slice of his army; a push is a force he has
    // actually committed to. Between them sits harassment, which deserves a
    // response but not the whole build order.
    using Threat = Team::EnemyRead::Threat;
    if (r.incoming == 0) {
        r.threat = Threat::None;
    } else if (r.incoming <= 2 && r.committed_frac < 0.34) {
        r.threat = Threat::Scout;
    } else if (r.incoming >= 5 && r.committed_frac >= 0.5) {
        r.threat = Threat::Push;
    } else {
        r.threat = Threat::Raid;
    }

    // ---- posture ------------------------------------------------------------
    // 0 = pure economy, 1 = everything committed at us. The army-share term is
    // what he has BUILT, the commitment term is what he is DOING with it; a
    // hoarded army that never leaves home is only half as threatening as the
    // same army walking at our gate.
    r.aggression = std::clamp(r.mil_ratio * 0.5 + r.committed_frac * 0.5, 0.0, 1.0);

    td.ai_read = r;
}

void Control::ai_assess_map(int team, World& world, EntityRef base) {
    Team& td = teams[team];
    Building* bb = base.valid() ? world.get_building(base) : nullptr;
    if (!bb) return; // no base yet -- leave assessed=false and retry next tick
    td.ai_plan.assessed = true;
    const double bx = bb->common.x, by = bb->common.y;
    const double map_diag = std::hypot(world.px_w, world.px_h);

    // ---- nearest enemy base + attack axis (direction raids will come from) --
    double enemy_d = map_diag;
    double ex = world.px_w * 0.5, ey = world.px_h * 0.5; // default axis: map centre
    bool have_enemy = false;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive || b->name != "base" || allied(b->common.team, team)) continue;
        double d = std::hypot(b->common.x - bx, b->common.y - by);
        if (d < enemy_d) { enemy_d = d; ex = b->common.x; ey = b->common.y; have_enemy = true; }
    }
    td.ai_plan.enemy_dist = enemy_d;

    // ---- land route to the enemy? (flood-fill non-water tiles from our base) --
    // If the nearest enemy base is reachable overland, a factory's tanks can
    // actually get there; if it's across water (island), an airbase is the
    // useful War-prereq instead. Left at its default (true) when there's no
    // enemy to check (e.g. an allied economy match).
    if (have_enemy) {
        int etx = static_cast<int>(ex / TILE), ety = static_cast<int>(ey / TILE);
        int btx0 = static_cast<int>(bx / TILE), bty0 = static_cast<int>(by / TILE);
        bool reached = false;
        if (btx0 >= 0 && btx0 < world.cols && bty0 >= 0 && bty0 < world.rows &&
            world.terrain[btx0][bty0] != WATER) {
            // The cap MUST be able to cover the whole land mass, or a perfectly
            // walkable map reports "no land route" and the AI concludes it is
            // stranded on an island. It used to be a flat 6000 tiles, which is
            // fine for the 64x64 grid it was presumably sized against -- but
            // new_skirmish builds `map_size * 2` tiles per side, so the arena's
            // "map size 64" is a 128x128 = 16384-tile world and the fill quit
            // after 37% of it. Whether it happened to find the enemy first came
            // down to which way this depth-first search charged off.
            //
            // Measured over the 600-match Ostland arena (avg map water 0.49%,
            // i.e. essentially solid land): 146 of 1200 team-instances decided
            // they had no land route, built the "stranded" shipyard, and then
            // spent the match making warships -- 7.5 naval units each and only
            // 0.5 transports, on maps with no water to sail. In the 142 matches
            // where exactly one side did this, that side went 4-117-21 (10.2%)
            // and produced a third less army than its opponent (69.6 vs 106.5)
            // off the same map. It was the single most expensive AI error in
            // the run.
            //
            // Bound by the actual tile count -- `seen` already guarantees
            // termination, so this is only a safety cap and can afford to be
            // exact. Runs ONCE per team per match (ai_plan.assessed latches it),
            // and the seen-set is a flat byte grid rather than a std::set of
            // pairs so a full-map fill is a few hundred microseconds.
            std::vector<char> seen(static_cast<size_t>(world.cols) * world.rows, 0);
            auto seen_at = [&](int x, int y) -> char& {
                return seen[static_cast<size_t>(x) * world.rows + y];
            };
            std::vector<std::pair<int, int>> stk;
            stk.push_back({btx0, bty0});
            seen_at(btx0, bty0) = 1;
            static const int dxs[4] = {1, -1, 0, 0}, dys[4] = {0, 0, 1, -1};
            const int max_visits = world.cols * world.rows;
            int visited = 0;
            while (!stk.empty() && visited < max_visits) {
                auto [tx, ty] = stk.back();
                stk.pop_back();
                ++visited;
                if (std::abs(tx - etx) <= 2 && std::abs(ty - ety) <= 2) { reached = true; break; }
                for (int d = 0; d < 4; ++d) {
                    int nx = tx + dxs[d], ny = ty + dys[d];
                    if (nx < 0 || nx >= world.cols || ny < 0 || ny >= world.rows) continue;
                    if (world.terrain[nx][ny] == WATER) continue;
                    if (seen_at(nx, ny)) continue;
                    seen_at(nx, ny) = 1;
                    stk.push_back({nx, ny});
                }
            }
        }
        td.ai_plan.land_to_enemy = reached;
    }

    // ---- local water fraction + fish availability ------------------------
    const int scan = 20; // tiles each way around the base
    int water = 0, total = 0;
    int btx = static_cast<int>(bx / TILE), bty = static_cast<int>(by / TILE);
    for (int dx = -scan; dx <= scan; ++dx) {
        for (int dy = -scan; dy <= scan; ++dy) {
            int tx = btx + dx, ty = bty + dy;
            if (tx < 0 || tx >= world.cols || ty < 0 || ty >= world.rows) continue;
            ++total;
            if (world.terrain[tx][ty] == WATER) ++water;
        }
    }
    td.ai_plan.water_score = total > 0 ? static_cast<double>(water) / total : 0.0;
    EntityRef fish = world.nearest(bx, by, 40 * TILE, [&](EntityRef r, EntityCommon&) {
        Resource* rr = world.get_resource(r);
        return rr && rr->common.alive && rr->name == "fish";
    });
    td.ai_plan.can_fish = td.ai_plan.water_score > 0.06 && fish.valid();

    // ---- whole-map naval viability (the shipyard gate) --------------------
    // The two measures above are deliberately LOCAL (a 41x41 tile window round
    // the base, a 40-tile fish search) because they answer "can this town work
    // the water". Whether the map is worth a navy at all is a different, map-
    // wide question, and it's the one the build order needs: Team::strategy is
    // "navy" for every team on any want_water map, which on e.g. a random map
    // with one coastal strip had the AI put up as many as five shipyards on
    // what is effectively a land map. Count every tile once, and look for a
    // fish shoal anywhere rather than only near home.
    {
        int wet = 0, all = 0;
        for (int tx = 0; tx < world.cols; ++tx)
            for (int ty = 0; ty < world.rows; ++ty) {
                ++all;
                if (world.terrain[tx][ty] == WATER) ++wet;
            }
        td.ai_plan.map_water_frac = all > 0 ? static_cast<double>(wet) / all : 0.0;
        td.ai_plan.map_has_fish = false;
        for (auto ref : world.active_resources) {
            Resource* r = world.get_resource(ref);
            if (r && r->common.alive && r->name == "fish") { td.ai_plan.map_has_fish = true; break; }
        }
        td.ai_plan.naval_viable = td.ai_plan.map_has_fish || td.ai_plan.map_water_frac > 0.20;
    }

    // ---- choke detection: narrowest natural gap straddling the attack axis --
    // Scan perpendicular to the base->enemy axis at a few distances out; a real
    // choke has an impassable blocker (forest/water) within reach on BOTH sides
    // of an otherwise-open lane. The wall then spans blocker-to-blocker.
    double axx = ex - bx, ayy = ey - by, axlen = std::hypot(axx, ayy);
    if (axlen < 1e-3) { axx = 1; ayy = 0; axlen = 1; }
    double ux = axx / axlen, uy = ayy / axlen; // toward the enemy
    double nx = -uy, ny = ux;                   // perpendicular ("normal")
    td.ai_plan.wall_planned = false;
    double best_gap = 1e18;
    for (int dtile = 4; dtile <= 12; ++dtile) {
        double cxp = bx + ux * dtile * TILE, cyp = by + uy * dtile * TILE;
        if (!world.passable(false, false, cxp, cyp)) continue; // lane point must be open
        auto scan_side = [&](double sgn, double& out) -> bool {
            for (int k = 1; k <= 12; ++k) {
                double sx = cxp + nx * sgn * k * TILE, sy = cyp + ny * sgn * k * TILE;
                if (sx < 0 || sy < 0 || sx >= world.px_w || sy >= world.px_h) return false; // opens to edge
                if (!world.passable(false, false, sx, sy)) { out = k * TILE; return true; }
            }
            return false; // nothing to anchor to on this side -> too open
        };
        double left = 0, right = 0;
        if (!scan_side(+1, left) || !scan_side(-1, right)) continue;
        double gap = left + right;
        if (gap <= 10 * TILE && gap < best_gap) {
            best_gap = gap;
            td.ai_plan.wall_planned = true;
            td.ai_plan.wx0 = cxp + nx * left;  td.ai_plan.wy0 = cyp + ny * left;
            td.ai_plan.wx1 = cxp - nx * right; td.ai_plan.wy1 = cyp - ny * right;
        }
    }

    // ---- pick the playstyle (map-derived, with a small random nudge) ------
    double closeness = enemy_d / std::max(1.0, map_diag);     // ~0 near .. ~0.7+ far
    // CANDIDATE (ai_variant == 4), NOT promoted -- it LOSES its A/B. Kept here
    // because the finding it rests on is real and someone will otherwise redo
    // the analysis and reach for the same fix.
    //
    // 336 tournament matches (ab_v4, seeds 8000+, --vary-civs, sides alternated):
    // candidate 134, baseline 185 -- 42.0% of decided games, 95% CI 36.7-47.5,
    // i.e. significantly WORSE. The per-match logs say why: the aggressive
    // branch below runs a deliberately lean economy (vil_goal = cap_goal * 3/5)
    // and simply gets out-boomed -- a losing candidate typically ends on era 1
    // with 8-31 villagers against a baseline on era 2 with ~49. The unreachable
    // 0.40 threshold was, by accident, protecting the AI from a playstyle that
    // does not currently work. Making "aggressive" reachable is therefore
    // blocked on making it *good* first (a rush needs the early army to arrive
    // before the boom pays off, which is an ai_train/push-timing problem, not a
    // threshold problem). The diagnosis below stands regardless:
    //
    // The bare enemy_dist/map_diagonal ratio can't get near the 0.40 cut when
    // there are only two players. spawn_points puts the bases diametrically
    // opposite on a ring at 0.74-0.94 of the land half-extent, so on a square
    // all-land map enemy_dist ~= 0.98 * width * fill and the diagonal is
    // width * sqrt(2) -- the ratio is pinned to ~0.51-0.65, or ~0.45-0.71 once
    // the jitter below is added. It is arithmetically incapable of going under
    // 0.40. The 600-match arena bears that out exactly: 9 aggressive out of
    // 1200 team-instances (0.75%) against 962 defensive (80%), and with both
    // sides turtling 63.7% of matches ran out the clock and 30.3% were draws.
    //
    // The ratio is also not comparable ACROSS player counts, which is why the
    // cut can't simply be raised: the same ring packs N players at 2r*sin(pi/N)
    // apart, so an 8-player game sits near 0.22 and would go all-aggressive.
    // Dividing by sin(pi/N) removes the packing term, leaving a number that
    // means the same thing at every player count -- 1v1 is the sin(pi/2) = 1
    // case, so its range is unchanged and only the threshold moves.
    if (td.ai_variant == 4) {
        double ring = std::sin(M_PI / std::max(2, n)); // 1.00 at 2p, 0.71 at 4p, 0.38 at 8p
        closeness = enemy_d / std::max(1.0, map_diag * ring);
    }
    // One draw either way -- the variants must consume the RNG identically or a
    // head-to-head tournament match stops being the same map for both sides.
    closeness += world.rng.uniform(-0.06, 0.06);              // keep skirmishes varied
    // 0.55 sits inside the ~0.45-0.71 band a 1v1 actually produces, so roughly
    // the nearer third of layouts open aggressively instead of ~none of them.
    const double aggressive_cut = (td.ai_variant == 4) ? 0.55 : 0.40;
    std::string style;
    if (td.ai_plan.can_fish && td.ai_plan.water_score > 0.12) style = "naval";
    else if (closeness < aggressive_cut) style = "aggressive"; // enemy near -> pressure
    else if (td.ai_plan.wall_planned && closeness < 0.75) style = "defensive"; // sealable + not far
    else style = "boom";                                      // safe/far -> economy
    td.ai_plan.playstyle = style;

    // Map the playstyle onto the existing offensive-push tuning (ai_behavior)
    // and a villager goal. Economic plans run a bigger workforce then bank hard
    // for the age-up; the aggressive plan stays lean for early pressure.
    int cap_goal = std::clamp(static_cast<int>(max_pop * 0.30 + 10), 20, 80);
    if (style == "aggressive") {
        td.ai_behavior = "aggressive";
        td.ai_plan.vil_goal = std::max(12, cap_goal * 3 / 5);
    } else if (style == "defensive") {
        td.ai_behavior = "defensive";
        td.ai_plan.vil_goal = std::min(80, cap_goal * 6 / 5);
    } else { // boom / naval
        td.ai_behavior = "balanced";
        td.ai_plan.vil_goal = std::min(80, cap_goal * 6 / 5);
    }
    td.ai_plan.fish_goal = td.ai_plan.can_fish ? std::clamp(td.ai_plan.vil_goal / 4, 3, 12) : 0;
    if (style == "aggressive") td.ai_plan.wall_planned = false; // rushers don't pre-wall
    // Per-team spread on production intensity (see Team::ai_intensity): mostly
    // small, but the high tail pushes a few AIs toward all-in unit production
    // while the average stays reserved (stockpiles food for the age-up).
    td.ai_intensity_jitter = world.rng.uniform(-0.12, 0.28);
}

void Control::ai_build_walls(int team, World& world, EntityRef base) {
    Team& td = teams[team];
    if (!td.ai_map_derive) return;
    (void)base;
    auto& plan = td.ai_plan;
    if (!plan.wall_planned || plan.wall_built) return;
    if (td.res["wood"] < 30) return; // wait until the whole run is clearly affordable
    // Defense comes AFTER the opening economy -- don't wall at tick zero, collect
    // resources first. Wait for a working villager base, and build the whole
    // wall with a SINGLE villager (the chaining in unit_behavior carries it
    // segment to segment). Assigning many builders is what left some trapped on
    // the far side of the completed wall.
    int n_civ = 0;
    EntityRef builder = kNullRef, any_civ = kNullRef;
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (!u || !u->common.alive || u->common.team != team || u->name != "civilian") continue;
        ++n_civ;
        if (!any_civ.valid()) any_civ = ref;
        if (!builder.valid() && !u->build_target.valid() && u->carry == 0) builder = ref;
    }
    if (n_civ < 12) return; // economy first; wall once the base is established
    std::vector<EntityRef> crew;
    crew.push_back(builder.valid() ? builder : any_civ); // exactly one wall builder
    // Rasterize the choke line onto the palisade's 32px grid (the sim-side
    // twin of the client's drag-builder), dropping a palisade on every cell.
    // Cells landing on the natural blockers/existing buildings just fail
    // footprint_clear inside place_building and are skipped, so the wall fills
    // exactly the open lane between the two anchors.
    auto cell = [&](double wx, double wy) {
        auto p = world.snap("palisade", wx, wy);
        return std::pair<int, int>{static_cast<int>(std::lround((p.first - 16.0) / TILE)),
                                   static_cast<int>(std::lround((p.second - 16.0) / TILE))};
    };
    auto [cx0, cy0] = cell(plan.wx0, plan.wy0);
    auto [cx1, cy1] = cell(plan.wx1, plan.wy1);
    int span = std::max(std::abs(cx1 - cx0), std::abs(cy1 - cy0));
    int px = 0, py = 0; bool have_prev = false;
    for (int i = 0; i <= span; ++i) {
        double t = span == 0 ? 0.0 : static_cast<double>(i) / span;
        int cx = cx0 + static_cast<int>(std::lround((cx1 - cx0) * t));
        int cy = cy0 + static_cast<int>(std::lround((cy1 - cy0) * t));
        if (have_prev && cx == px && cy == py) continue;
        px = cx; py = cy; have_prev = true;
        if (!afford("palisade", team)) break;
        world.place_building("palisade", team, cx * static_cast<double>(TILE) + 16.0,
                             cy * static_cast<double>(TILE) + 16.0, crew);
    }
    plan.wall_built = true; // one-shot -- don't re-attempt the same line every tick
}

void Control::ai_scout(int team, World& world, const std::vector<EntityRef>& force) {
    Team& td = teams[team];
    if (!td.ai_map_derive) return;
    auto& plan = td.ai_plan;
    // Scout only early (era 0) and while not committed to an attack; once the
    // army mobilizes, hand the scout back to the offensive loop.
    if (td.era > 0 || td.ai_committed) {
        if (Unit* old = plan.scout.valid() ? world.get(plan.scout) : nullptr) old->rally.reset();
        plan.scout = kNullRef;
        return;
    }
    Unit* s = plan.scout.valid() ? world.get(plan.scout) : nullptr;
    if (!s || !s->common.alive || s->common.team != team) {
        plan.scout = kNullRef;
        if (force.size() >= 2) plan.scout = force.front(); // spare one only if 2+ at home
        s = plan.scout.valid() ? world.get(plan.scout) : nullptr;
        if (!s) return;
    }
    // Sweep a fixed ring of map waypoints (corners + edge midpoints), advancing
    // to the next once close. Purely cosmetic exploration (the AI already sees
    // the whole map -- fog only tracks team 0).
    static const double frac[8][2] = {{0.12, 0.12}, {0.88, 0.12}, {0.88, 0.88}, {0.12, 0.88},
                                      {0.50, 0.10}, {0.90, 0.50}, {0.50, 0.90}, {0.10, 0.50}};
    int wp = plan.scout_wp % 8;
    double wx = world.px_w * frac[wp][0], wy = world.px_h * frac[wp][1];
    double dx = s->common.x - wx, dy = s->common.y - wy;
    if (dx * dx + dy * dy < (3 * TILE) * (3 * TILE)) plan.scout_wp = (plan.scout_wp + 1) % 8;
    s->rally = Vec2{wx, wy};
}

void Control::ai_economy(int team, World& world, EntityRef base, int n_civ,
                          const std::vector<EntityRef>& team_buildings) {
    Team& td = teams[team];
    Building* base_b = base.valid() ? world.get_building(base) : nullptr;
    double food = td.res["food"];
    // Villager target scales with the game's pop cap (~40 in a 100-pop game,
    // ~70 in a 200-pop game). For skirmish AI it comes straight from the
    // map-derived plan (ai_assess_map); campaign AI keeps the original
    // ai_behavior-tuned clamp so its authored balance is untouched.
    int vil_target;
    if (td.ai_map_derive) {
        vil_target = td.ai_plan.vil_goal;
    } else {
        vil_target = std::clamp(static_cast<int>(max_pop * 0.3 + 10), 20, 80);
        // Campaign AI-behaviour preset tilts the economy/military balance: a
        // rusher keeps a lean economy, a passive/defensive turtle a bigger one.
        if (td.ai_behavior == "rusher") vil_target = vil_target * 3 / 5;
        else if (td.ai_behavior == "aggressive") vil_target = vil_target * 4 / 5;
        else if (td.ai_behavior == "passive" || td.ai_behavior == "defensive")
            vil_target = std::min(80, vil_target * 6 / 5);
    }
    // ---- Reserved production intensity (see Team::ai_intensity) ------------
    // Instead of the old all-or-nothing "bank HARD vs mass units", spend only a
    // FRACTION of food income on military, so food net-accumulates toward the
    // age cost while still trickling out units. The average AI is reserved and
    // gradually stockpiles the ~500; a per-team random spread lets some go
    // all-in. The fraction climbs for aggressive plans and when enemies press.
    double gathered_food = td.total_gathered.count("food") ? td.total_gathered.at("food") : 0.0;
    if (td.ai_food_mark < 0.0) td.ai_food_mark = gathered_food;
    double food_income = std::max(0.0, gathered_food - td.ai_food_mark); // gathered since last pass
    td.ai_food_mark = gathered_food;
    double intensity;
    if (td.ai_map_derive) {
        const std::string& ps = td.ai_plan.playstyle;
        intensity = (ps == "aggressive") ? 1.10 : (ps == "defensive") ? 0.55 : (ps == "naval") ? 0.50 : 0.40;
    } else {
        intensity = 0.60; // campaign default: moderately reserved
    }
    intensity += td.ai_intensity_jitter;
    // Opponent pressure: enemy military near home -> spend more (produce faster).
    int threat = 0;
    if (base_b) {
        double R = 24.0 * TILE;
        for (auto ref : world.active_units) {
            Unit* eu = world.get(ref);
            if (!eu || !eu->common.alive || eu->common.team < 0 || allied(eu->common.team, team)) continue;
            if (eu->is_gatherer) continue;
            double dx = eu->common.x - base_b->common.x, dy = eu->common.y - base_b->common.y;
            if (dx * dx + dy * dy <= R * R) ++threat;
        }
    }
    intensity += std::min(0.6, threat * 0.08);
    // ---- Retaliation reflex (see Team::ai_retaliate) -----------------------
    // Being SHOT AT (ai_under_fire, stamped by World::hurt) while holding less
    // army than the attacker has brought. Both halves matter: the damage clock
    // is what separates a real attack from a scout walking through the proximity
    // radius, and the army comparison is what stops a team that already has a
    // standing force from abandoning its plan every time a raid trades a shot.
    // A single attacker is enough here, unlike the bare-proximity ai_threat gate
    // further down in ai_build.
    //
    // The behaviour this fixes: a boom plan with no barracks and no soldiers had
    // no path from "my villagers are being killed" to "make something that
    // shoots back" -- ai_train's floor is 2-5 units and only ever applies to
    // military buildings that already exist, and the first barracks waited on
    // cur_civs >= 12 or two enemies in the radius.
    //
    // DEFENCELESS, not merely outnumbered. The first cut of this asked for
    // `ai_army < threat + 2`, i.e. "attacked while behind on numbers", and that
    // is a completely different and much commoner condition: a 100-match arena
    // run with it showed Scientific-era reach collapse from 50% of
    // team-instances to 10% and median match length halve to 20.8 minutes.
    // The mechanism is the banking suppression below -- banking is the ONLY way
    // food ever reaches the age cost, so a flag that trips on any losing skirmish
    // keeps the age-up permanently switched off and the match degenerates into
    // an early all-in. Holding it to "essentially nothing to fight with" keeps
    // the emergency an emergency: three soldiers is enough to clear it, so a team
    // that answers the raid goes straight back to its economic plan.
    constexpr int kDefencelessArmy = 2;
    td.ai_retaliate = td.ai_under_fire > 0.0 && threat >= 1 && td.ai_army <= kDefencelessArmy;
    // Retaliating spends at the aggressive ceiling: the food is worth more as
    // defenders now than as anything it could become later.
    if (td.ai_retaliate) intensity = std::max(intensity, 1.10);
    // ---- act on the read of the opponent (Team::ai_read) --------------------
    using Threat = Team::EnemyRead::Threat;
    const auto& rd = td.ai_read;
    // A committed push, seen while it is still crossing the map. This is the
    // whole point of reading his ORDERS rather than waiting for arrivals: the
    // old proximity count only noticed an attack once it was already inside the
    // 24-tile radius, by which time nothing built in response can be ready.
    if (rd.threat == Threat::Push) intensity = std::max(intensity, 1.10);
    // He is out-producing us. Match the investment rather than discovering the
    // gap when his army arrives -- mil_rate is HIS army's growth per minute.
    if (rd.mil_rate > 2.0 && rd.mil > td.ai_army) intensity = std::max(intensity, 0.85);
    // He is booming: few military, nothing committed at us. Military spend is
    // worth less against an opponent with nothing to punish us with, so ease
    // off and put the food into our own economy instead -- but never below the
    // floor, because a boom read that turns out to be wrong is how a team gets
    // caught with no army at all.
    if (rd.valid && rd.threat == Threat::None && rd.mil_ratio < 0.15 && !td.ai_retaliate)
        intensity = std::min(intensity, 0.45);
    intensity = std::clamp(intensity, 0.20, 1.50);
    td.ai_intensity = intensity;
    td.ai_threat = threat; // also read by ai_build's army-timing gate
    td.ai_banking = false; // real value assigned by the tempo doctrine below

    // If we're spending hard on units (high intensity, e.g. under pressure),
    // grow the workforce a bit MORE so income catches up and we recover a food
    // surplus -- the "add a few villagers if you need to keep producing" ask.
    int vil_goal_eff = (intensity > 0.80) ? std::min(80, vil_target + 8) : vil_target;
    // Age-up push -- THE fix for "boomed forever but never advanced": once there
    // is a WORKING economy (a moderate villager count), briefly focus the age
    // instead of booming all the way to the villager goal first. Otherwise the
    // town centre spends every spare 80 food on another villager and food never
    // climbs to the ~500 age cost until the whole boom is done, so the AI hit
    // Industrial absurdly late. While banking, pause villager AND military
    // production so all gathered resources pile straight toward the cost; the
    // moment it's affordable this ends (age enqueues below) and the boom
    // resumes. Only fires for an age that's actually buildable and not already
    // affordable/queued.
    int age_food = 0;
    if (td.era < 3) {
        auto agecost = cost_of(AGE_ITEMS[td.era], team);
        age_food = agecost.count("food") ? agecost.at("food") : 0;
    }
    // "Good enough" economy to age on. Was clamp(vil_target / 2, 14, 22), which
    // on a 200-pop map (vil_target 48-80) always resolved to the TOP of that
    // range -- the AI waited for 22 villagers before it would even start
    // thinking about Industrial. Fixed at 14 now: the age is the single biggest
    // power spike available and every extra villager before it is time spent in
    // the weakest era in the game. See the tempo doctrine below.
    constexpr int kAgeAtVillagers = 14;
    int age_ready = kAgeAtVillagers;
    bool age_queued = false;
    if (base_b) {
        for (const auto& q : base_b->queue)
            if (std::find(AGE_ITEMS.begin(), AGE_ITEMS.end(), q) != AGE_ITEMS.end()) {
                age_queued = true;
                break;
            }
    }
    bool can_afford_age = td.era < 3 && afford(AGE_ITEMS[td.era], team);
    bool banking =
        age_food > 0 && world.can_age_up(team) && !age_queued && !can_afford_age && n_civ >= age_ready;
    // Never bank through a beating. Banking zeroes ai_mil_budget and holds the
    // factory/airbase, so a team that started saving for an age just before it
    // got rushed sat on a growing pile of food while its base was dismantled.
    if (td.ai_retaliate) banking = false;
    // PROMOTED (was ai_variant == 5, see variant_has): bank only on the FINAL STRETCH to the age
    // cost, not for the whole climb.
    //
    // Banking holds the town centre (see the villager enqueue below), and it
    // switches on as soon as the team has a working economy -- age_ready is
    // 14-22 villagers -- then stays on until the full ~500 food is banked. That
    // is a very long hold: measured across 672 team-instances the town centre
    // sat idle a MEAN of 489 seconds, median 94s, worst case 3762s. A town
    // centre doing nothing for eight minutes is the single largest identified
    // economic waste in the AI.
    //
    // The hold exists for a real reason ("the TC spends every spare 80 food on
    // another villager and food never climbs to the age cost"), so this doesn't
    // remove it -- it delays it. While food is still under half the age cost the
    // team is nowhere near aging anyway, and a villager bought at that point has
    // hundreds of seconds left to pay back its 80 food several times over. Once
    // past the halfway mark the original all-in hold takes over and the last
    // stretch is banked fast. Net effect should be a bigger economy reaching the
    // same age at a similar time, rather than a stalled one reaching it slightly
    // sooner.
    // ---- FEATURE 5 RETIRED, 2026-08-15 -------------------------------------
    // "Bank only on the final stretch" existed because banking used to hold the
    // TOWN CENTRE, and holding it from the start left it idle for minutes. The
    // tempo doctrine removed that: villager production is never held any more,
    // and ai_banking now only refuses MILITARY spending, per resource, in
    // whatever the pending age is short of. With the town centre out of it, a
    // late-starting reserve has no upside left and one fatal downside -- see the
    // deadlock described at kAgeHoldFrom below. Kept reachable as variant 5 so
    // the old behaviour can still be measured against the new one.
    if (td.ai_variant == 5 && banking && food < age_food * 0.5) banking = false;

    // ================= TEMPO DOCTRINE (2026-08-13) ==========================
    // Player report, from actually playing against it: "the AI is really good at
    // making economy and banking resources but really poor at making units -- I
    // make 2 tanks and kill his entire base before he can get enough units out."
    // The arena numbers agree and always did: teams routinely FINISH matches
    // sitting on 8,000-30,000 food, having spent the game refusing to buy
    // anything with it. That is not an economy, it is a stockpile.
    //
    // The cause is the reserved-production throttle: military spending was
    // capped at a FRACTION of food income (ai_intensity, 0.2-1.5, accrued into
    // ai_mil_budget and debited per unit), so income above that fraction had
    // nowhere to go and simply accumulated forever. Three separate holds sat on
    // top of it -- the age bank, the boom ramp, and the defensive floor -- each
    // of which could stop unit production outright.
    //
    // The doctrine now is the opposite one, and it is deliberately simple:
    //
    //   SPEND EVERYTHING, ALWAYS. No income fraction, no accrued budget, no
    //   boom phase. Every production building queues whenever it can afford to.
    //   Resources sitting in the bank are units that are not on the map.
    //
    //   NEVER PAUSE THE TOWN CENTRE. Villager production runs continuously to
    //   the map-derived goal and is no longer held for the age-up.
    //
    //   AGE AS FAST AS POSSIBLE. At kAgeAtVillagers the AI wants the next age,
    //   and ai_build puts its prerequisite buildings at the top of the order.
    //   Victorian is the weakest era in the game; time spent there is the thing
    //   being minimised.
    //
    // The one hold left is kAgeHold, and it is deliberately narrow -- see it
    // below. Everything else the player asked for follows from spending.
    //
    // ai_banking/ai_boom_phase stay as fields because other passes read them,
    // but the boom ramp is retired outright: it is the specific mechanism that
    // made the opening harmless.
    td.ai_boom_phase = false;

    // The narrow exception to "spend everything". While the team can actually
    // age up and is close to affording it, military spending stops short of the
    // age cost so the last few hundred food is not eaten by another rifleman.
    //
    // This is a deviation from the letter of "hold nothing", and it is here
    // because holding nothing is self-defeating for the OTHER half of the same
    // instruction: with three production buildings each queueing on sight, food
    // never climbs past a few hundred, so the ~500 an age costs is never
    // reached and the AI would stay in Victorian forever -- the exact opposite
    // of "spend as little time as possible in the Victorian age".
    //
    // It is a TRANSIENT hold, not a standing reserve: it applies only once
    // can_age_up is satisfied and food is already most of the way there, and it
    // releases the instant the age is queued. In the steady state -- and for the
    // whole of era 3, where there is no next age -- nothing is held back at all.
    // Set kAgeHold to false for the literal zero-reserve behaviour.
    constexpr bool kAgeHold = true;
    // ---- RESERVE THE AGE COST AS SOON AS THE AGE IS LEGAL (2026-08-15) ------
    // Was 0.6: don't protect the age fund until 60% of it is already saved.
    // That is a deadlock, and it is THE reason the AI stalls at Industrial with
    // a huge score and never reaches War. Measured, 20-minute match, both sides:
    //
    //     era=1 canage=1 ageQ=0 now(food/oil)=29/789 idle_tc=1377s warbldgs=6
    //
    // canage=1 means the War prerequisites are already standing. War costs 800
    // food; the team has 29. It has 789 OIL -- it is not poor, it is poor in the
    // one resource the age needs. Every barracks and academy queues the instant
    // it can afford a unit, so food is spent within a second of being gathered
    // and never climbs; it therefore never reaches 60% of 800, so the protection
    // that would have let it climb never switches on. The threshold can only be
    // crossed by the very saving it is gating.
    //
    // It gets worse the better the economy gets, which is why this reads as
    // "lots of resources, no advancement": more production buildings drain food
    // faster, so a richer team stalls harder. Industrial (500 food) survives it
    // only because era 0 has too little production capacity to drain the pile.
    //
    // Reserving from zero costs much less than it looks like: ai_banking holds
    // ONLY military spending, ONLY in the resources that specific age needs
    // (train_mil checks per resource -- a tank cannot delay a food age-up), it
    // is exempt below the defensive floor, ai_retaliate clears it outright, and
    // it releases the moment the age is queued. Villager production is untouched.
    constexpr double kAgeHoldFrom = 0.0;
    td.ai_banking = kAgeHold && banking && food >= age_food * kAgeHoldFrom;
    // ai_mil_budget is retired as a THROTTLE -- it is what capped spending at a
    // fraction of income and let the rest pile up. It is kept as a field (other
    // code reads it) and simply held wide open, so train_mil's affordability
    // check is the only thing that ever limits unit production now.
    td.ai_mil_budget = 1e9;
    (void)intensity;
    (void)food_income;

    // ---- town centre: one villager at a time, requeued the moment it idles ---
    // The queue was "unit building + one on deck", which keeps a villager's 50
    // food frozen in the queue at all times for no gain. Depth ONE instead: the
    // AI re-checks on its own cadence and requeues as soon as the centre is
    // empty, which is what the player asked for -- there is no need to hold one
    // in reserve when it can be queued the instant the building frees up.
    //
    // The `!banking` hold that used to sit on this line is GONE. Villager
    // production is now continuous: it is never paused for an age-up, which is
    // what made town centres idle for minutes at a time.
    // ---- CANDIDATE (feature 23): don't buy a villager there is no job for ----
    // The villager goal is derived from the POP CAP (AiPlan::vil_goal, up to 80
    // on a 200-pop map) and from nothing else -- not from how much work the map
    // actually contains. The map is what decides: a farm feeds exactly ONE
    // villager (Building::occupied_by, a second one sent to the same farm
    // bounces off), farms were separately capped at 24, and berry veins run dry
    // within minutes. So a team can buy its way to a goal the terrain cannot
    // employ, and the surplus stands around -- which is the "villager
    // over-saturation on some maps (high idle_vil)" already on the open list.
    //
    // Every one of those villagers cost 50 food and a point of population that
    // an army unit could have had. The self-correcting version of the goal is
    // therefore not a cleverer formula but the observation itself: if the last
    // pass finished with gatherers it could find no resource for, this team does
    // not need another gatherer, whatever its goal says.
    //
    // Deliberately tolerant (kIdleTolerance, and only past the opening): one or
    // two villagers between jobs is ordinary churn, and stopping the town centre
    // on that would be a far worse error than a couple of spare workers. It also
    // releases the instant the workforce is employed again -- a depleted vein
    // that gets replaced by farms re-opens production on the very next pass.
    constexpr int kIdleTolerance = 3;
    bool workforce_saturated = variant_has(td.ai_variant, 23) && td.ai_map_derive &&
                               n_civ >= 12 && td.ai_idle_gatherers >= kIdleTolerance;
    double vil_food_gate = 80.0;
    if (base_b && base_b->queue.empty() && !workforce_saturated &&
        (n_civ < 8 || (n_civ < vil_goal_eff && food >= vil_food_gate))) {
        world.enqueue(base, "civilian");
    }
    // Hold the gate's worth of food for that enqueue (Team::ai_vil_reserve).
    // The condition mirrors the line above rather than approximating it, so the
    // reserve is in force exactly when a villager is still wanted -- and drops
    // to zero the moment the goal is met, which is when the army should get the
    // food back. Reserving the GATE and not the 50-food unit cost is deliberate:
    // reserving only the cost lets military spend food down to 50, where the
    // gate above can never be satisfied and the centre stalls anyway.
    td.ai_vil_reserve =
        (!workforce_saturated && n_civ < vil_goal_eff) ? vil_food_gate : 0.0;
    // ---- PROMOTED (was ai_variant == 11): every town centre trains villagers -
    // `base` is simply the FIRST base in this team's building list, and it was
    // the only one this function ever enqueued at. A team that expands (which
    // ai_build actively pushes at era >= 2 -- a second base is the cheapest
    // Scientific prerequisite) therefore pays 275 wood + 100 iron for a town
    // centre that trains nothing at all for the rest of the match. It buys pop
    // cap and a dropoff, and then stands there.
    //
    // Two things come out of that. Villager throughput stays pinned to ONE
    // building's train time however many town centres are standing, which is
    // the actual ceiling on how fast the economy can reach its goal; and the
    // idle-town-centre metric -- which sums across every base, so two idle at
    // once counts double -- is dominated by expansions that were never given a
    // queue. Matches routinely end with idle_tc in the hundreds or thousands of
    // seconds against bases=2 or bases=3.
    //
    // Every gate the first town centre answers to still applies: the age-up
    // bank still holds all of them, the villager goal is still the villager
    // goal, and the food gate is re-read per enqueue. `pending` counts what is
    // already on order across ALL bases so N town centres in the same pass
    // can't each queue the same last villager and overshoot the goal (every
    // enqueue pre-pays, so an overshoot is frozen food, not just a stray unit).
    if (variant_has(td.ai_variant, 11) && td.ai_map_derive && !workforce_saturated) {
        for (auto ref : team_buildings) {
            if (ref == base) continue;
            Building* tb = world.get_building(ref);
            if (!tb || !tb->common.alive || !tb->complete || tb->name != "base") continue;
            if (!tb->queue.empty()) continue; // depth one, same as the main centre
            int pending = 0;
            for (auto oref : team_buildings) {
                Building* ob = world.get_building(oref);
                if (!ob || !ob->common.alive || ob->name != "base") continue;
                pending += static_cast<int>(
                    std::count(ob->queue.begin(), ob->queue.end(), std::string("civilian")));
            }
            int projected = n_civ + pending;
            if (!(projected < 8 || (projected < vil_goal_eff && td.res["food"] >= vil_food_gate))) break;
            world.enqueue(ref, "civilian");
        }
    }
    // Age up the moment it's affordable with a working economy behind it --
    // later eras unlock the siege (factory/artillery) that backs the rush.
    if (base_b && td.era < 3) {
        const std::string& age = AGE_ITEMS[td.era];
        if (afford(age, team) && n_civ >= 6) {
            // ALWAYS jump the age-up to the FRONT of the base queue (not just
            // for the candidate variant). Appending it at the back is not just
            // "slow" -- it's a hard deadlock: the AI keeps the queue topped with
            // civilians, and once the team hits its population cap a civilian at
            // the FRONT makes zero progress (building_behavior.cpp pop check) and
            // strict-FIFO blocks the queued age-up FOREVER. Measured directly:
            // era-1 teams sitting on 18k food / 3k oil with can_age_up satisfied
            // and the War age IN the queue (ageQ=1), stuck at Victorian the whole
            // match. The age itself has no pop check, so at the front it always
            // completes. See World::enqueue's `priority` param.
            world.enqueue(base, age, /*priority=*/true);
        }
    }

    // Convert surplus wood/iron into oil at the market. Land maps are oil-poor
    // (a handful of small wells that deplete), but muscateers, age-ups and
    // research all burn oil, while wood/iron otherwise just pile up unused.
    // Sell in 100-unit lots (once/sec) whenever oil runs low and there's a
    // surplus to spare -- prefer dumping wood, since iron backs later siege.
    bool has_market = false;
    for (auto ref : team_buildings) {
        Building* b = world.get_building(ref);
        if (b && b->complete && b->name == "market") { has_market = true; break; }
    }
    // Auto-replant (Team::replant): a farm worked to exhaustion is re-sown in
    // place for the price of a new one, so the farmer never stops working and
    // the farm never leaves its dropoff-adjacent spot. This is a POLICY, not an
    // upgrade -- it costs exactly what building a replacement farm costs -- and
    // the human toggles it on the market command card (GameClient's "replant"
    // button). The AI has no UI, so it was the one player-available policy it
    // could never express: its farms exhausted permanently and ai_build put the
    // replacements wherever there was room, sprawling further from the base
    // every cycle. Gated on a completed market for exact parity with where the
    // player's button lives, and on ai_map_derive so campaign AI is untouched.
    // Always on for an AI, market or no market. This was gated on a completed
    // market purely for parity with where the PLAYER's toggle lives (the market
    // command card) -- but the AI has no UI, so all the gate achieved was
    // letting its farms exhaust permanently through the whole opening, before
    // it had a market. Re-sowing costs exactly what a replacement farm costs,
    // and keeps the farm on its dropoff-adjacent tile instead of sprawling one
    // farm further out each cycle.
    td.replant = true;
    (void)has_market;

    if (has_market) {
        // Keep oil topped up for muscateers/siege...
        if (td.res["oil"] < 120) {
            if (td.res["wood"] >= 300) trade("sell", "wood", team, world);
            else if (td.res["iron"] >= 400) trade("sell", "iron", team, world);
        } else if (td.res["wood"] >= 800 && td.res["oil"] < 350) {
            // ...and when wood is clearly piling up with little else to spend it
            // on, aggressively convert the surplus into oil -- the army
            // bottleneck on oil-poor land maps (Plan 2). Only kicks in above a
            // fat wood reserve so it never starves building/farm construction.
            trade("sell", "wood", team, world);
        }
    }

    // ---- construction crews ---------------------------------------------
    // Wall segments are deliberately NOT crewed here. ai_build_walls rasterizes
    // a whole choke line into dozens of separate palisade foundations and hands
    // ONE villager to the first of them, relying on unit_behavior's wall
    // chaining to carry that villager segment to segment. This loop used to see
    // every other segment as an uncrewed foundation and pull an idle villager
    // onto each -- so a single wall order yanked ~10 villagers off the economy
    // at once (observed in play). Cap wall crew at 1: if the chain-builder dies
    // or gives up, exactly one replacement is sent, never a swarm.
    std::vector<EntityRef> foundations, wall_foundations;
    for (auto ref : team_buildings) {
        Building* b = world.get_building(ref);
        if (!b || b->complete) continue;
        if (b->name == "palisade" || b->name == "iron wall") wall_foundations.push_back(ref);
        else foundations.push_back(ref);
    }
    std::vector<EntityRef> idle_civs, carrying_civs;
    int wall_builders = 0;
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        // Fishing boats are gatherers too, but they work fish on water and are
        // handled by their own block below -- never mixed into the land-
        // resource quota system (they can't reach berries/wood/ore).
        if (!u || !u->common.alive || u->common.team != team || !u->is_gatherer ||
            u->name == "fishing boat")
            continue;
        if (u->build_target.valid()) {
            Building* bt = world.get_building(u->build_target);
            if (bt && (bt->name == "palisade" || bt->name == "iron wall")) ++wall_builders;
            continue;
        }
        if (u->carry == 0) idle_civs.push_back(ref);
        else carrying_civs.push_back(ref);   // fallback pool, see below
    }

    // Send `crew_size` villagers to `fref`, preferring the closest idle ones.
    // A villager mid-delivery (carry > 0) is only pulled in when NOTHING is
    // idle: its carry survives the switch, and leaving a foundation with zero
    // builders is far worse -- an uncrewed foundation just sits there, and the
    // pop cap / build-order logic that counts it as "coming" then blocks on a
    // building nobody is raising.
    auto crew_foundation = [&](EntityRef fref, int crew_size) {
        Building* f = world.get_building(fref);
        if (!f) return;
        int have = 0;
        for (auto ref : world.active_units) {
            Unit* u = world.get(ref);
            if (u && u->common.alive && u->common.team == team && u->build_target == fref) ++have;
        }
        for (; have < crew_size; ++have) {
            std::vector<EntityRef>* pool = !idle_civs.empty() ? &idle_civs
                                           : (have == 0 ? &carrying_civs : nullptr);
            if (!pool || pool->empty()) return;
            EntityRef best = kNullRef;
            double bestd = 1e18;
            size_t best_i = 0;
            for (size_t i = 0; i < pool->size(); ++i) {
                Unit* u = world.get((*pool)[i]);
                if (!u) continue;
                double dx = u->common.x - f->common.x, dy = u->common.y - f->common.y;
                double d = dx * dx + dy * dy;
                if (d < bestd) { bestd = d; best = (*pool)[i]; best_i = i; }
            }
            Unit* civ = world.get(best);
            if (!civ) return;
            civ->build_target = fref;
            civ->gather_target = kNullRef;
            civ->move_goal.reset();
            civ->path.clear();
            civ->approach_prev_pos.reset();
            civ->approach_progress_check_t = 0.0;
            civ->approach_target.reset();
            pool->erase(pool->begin() + best_i);
        }
    };

    // Non-wall foundations get a real crew. ai_build now keeps only one or two
    // foundations open at a time (see its work-in-progress cap), so doubling up
    // is affordable and is the whole point: the player's complaint was
    // foundations sitting half-raised for minutes while villagers trickled onto
    // them one at a time. Two builders roughly halve that, and the second one
    // is only spent when there are idle villagers to spare.
    // "Idle" overstates the slack: idle_civs is every gatherer that happens to
    // be carrying nothing this instant, which is most of the workforce most of
    // the time. So size the crew off the WORKFORCE, not off that list -- a
    // second builder only once there are enough villagers that losing one to
    // construction doesn't dent gathering, and never more than two.
    int crew = (static_cast<int>(idle_civs.size()) >= 8 && foundations.size() <= 2) ? 2 : 1;
    for (auto fref : foundations) crew_foundation(fref, crew);
    if (wall_builders == 0 && !wall_foundations.empty()) crew_foundation(wall_foundations[0], 1);

    // ---- CANDIDATE (ai_variant == 13): repair damaged buildings ------------
    // Unit::repair_target and Control::repair_tick are a complete, working
    // mechanic -- a villager walks to a damaged building and heals it for half
    // its build cost pro-rata -- and the AI has never once used it. Only the
    // player's repair button ever sets repair_target, so every point of damage
    // an AI building takes is permanent for the rest of the match.
    //
    // That compounds badly, because raids are constant and buildings never die
    // in one visit: a town centre chipped to a third across three separate
    // raids stays at a third, and the fourth raid takes it -- and losing the
    // last base IS the loss condition (Control::check_win). Half cost to undo
    // damage is also strictly cheaper than the full cost of replacing the
    // building, and the AI ends these matches sitting on the resources either
    // way.
    //
    // Deliberately NOT a repair-under-fire behaviour: a villager sent to patch
    // a building with enemies still on it just dies, and dead villagers cost
    // more than the wall they were mending. So it waits for the raid to leave
    // (nothing hostile within kRepairSafe) and heals up in the lull before the
    // next one. One building at a time, most valuable first, two villagers max
    // -- the same restraint the foundation crew above uses, for the same
    // reason: gathering is what pays for everything else.
    if (variant_has(td.ai_variant, 13) && !idle_civs.empty()) {
        constexpr double kRepairSafe = 8.0 * TILE;
        auto repair_value = [](const std::string& n) {
            if (n == "base") return 100;               // the win condition itself
            if (n == "tower") return 70;               // static defence, and it shoots back
            if (n == "barracks" || n == "academy" || n == "factory" || n == "airbase" ||
                n == "shipyard" || n == "fortress" || n == "university")
                return 50;                             // military production
            if (n == "palisade" || n == "iron wall" || n == "farm") return 0; // not worth a villager
            return 20;
        };
        EntityRef worst = kNullRef;
        double worst_score = 0.0;
        for (auto ref : team_buildings) {
            Building* b = world.get_building(ref);
            if (!b || !b->common.alive || !b->complete) continue;
            if (b->common.hp >= b->common.max_hp * 0.9 || b->common.max_hp <= 0.0) continue;
            int value = repair_value(b->name);
            if (value <= 0) continue;
            // Missing HP as a fraction, so a barely-scratched building doesn't
            // outrank a half-demolished one of the same type.
            double missing = 1.0 - b->common.hp / b->common.max_hp;
            double score = value * missing;
            if (score <= worst_score) continue;
            EntityRef near = world.nearest(b->common.x, b->common.y, kRepairSafe,
                                           [&](EntityRef, EntityCommon& c) {
                                               return c.alive && c.team >= 0 &&
                                                      c.kind == EntityKind::Unit &&
                                                      !allied(c.team, team);
                                           });
            if (near.valid()) continue; // still hot -- let the raid finish first
            worst_score = score;
            worst = ref;
        }
        if (worst.valid()) {
            Building* wb = world.get_building(worst);
            int have = 0;
            for (auto ref : world.active_units) {
                Unit* u = world.get(ref);
                if (u && u->common.alive && u->common.team == team && u->repair_target == worst) ++have;
            }
            // Closest idle villagers first, same as crew_foundation -- a repair
            // trip across the base is a gathering trip not taken.
            for (; have < 2 && !idle_civs.empty(); ++have) {
                size_t best_i = 0;
                double bestd = 1e18;
                for (size_t i = 0; i < idle_civs.size(); ++i) {
                    Unit* u = world.get(idle_civs[i]);
                    if (!u) continue;
                    double dx = u->common.x - wb->common.x, dy = u->common.y - wb->common.y;
                    if (dx * dx + dy * dy < bestd) { bestd = dx * dx + dy * dy; best_i = i; }
                }
                Unit* civ = world.get(idle_civs[best_i]);
                idle_civs.erase(idle_civs.begin() + best_i);
                if (!civ) continue;
                civ->repair_target = worst;
                civ->build_target = kNullRef;
                civ->move_goal.reset();
                civ->path.clear();
                civ->approach_prev_pos.reset();
                civ->approach_progress_check_t = 0.0;
                civ->approach_target.reset();
            }
        }
    }
    // ---- resource-type quotas -------------------------------------------
    // Spread the gathering workforce across the four resource types by a
    // target mix tuned for the muscateer rush: muscateers cost food+oil,
    // civilians and age-ups eat food, buildings eat wood, and later-age siege
    // (artillery) needs oil+iron -- so food and oil are weighted heavily,
    // wood is kept flowing for buildings, and iron ramps up once siege is on
    // the table (era >= 1). Index by rtype: 0=food, 1=wood, 2=oil, 3=iron.
    // Baseline mix (rtype 0=food,1=wood,2=oil,3=iron): the fixed per-era/
    // playstyle recipe the AI used before any goal steering.
    double base_w[4];
    if (td.era == 0) {
        if (td.ai_map_derive && td.ai_plan.playstyle != "aggressive") {
            base_w[0] = 0.55; base_w[1] = 0.30; base_w[2] = 0.15; base_w[3] = 0.00;
        } else {
            base_w[0] = 0.45; base_w[1] = 0.20; base_w[2] = 0.35; base_w[3] = 0.00;
        }
    } else if (td.era == 1) {
        base_w[0] = 0.40; base_w[1] = 0.15; base_w[2] = 0.30; base_w[3] = 0.15;
    } else {
        base_w[0] = 0.35; base_w[1] = 0.12; base_w[2] = 0.30; base_w[3] = 0.23;
    }
    double weight[4] = {base_w[0], base_w[1], base_w[2], base_w[3]};

    // Wood needed to SUSTAIN the farm economy. A farm is worth ~farm_food food
    // and costs 40 wood to (re)build when it exhausts, so keeping farmers fed
    // consumes wood at ~40/farm_food per unit of food produced. With ~one farmer
    // per farm, the wood workforce must be at least that fraction of the food
    // workforce (plus a little for ongoing buildings), or the farms lapse faster
    // than they're rebuilt and the food economy collapses. farm_food rises with
    // the farm techs, which lengthens each farm's life and lowers the upkeep.
    double farm_food = 200.0;
    if (td.tech.count("irrigation")) farm_food += 75.0;
    if (td.tech.count("fertilizer")) farm_food += 125.0;
    if (td.tech.count("pesticide")) farm_food += 200.0;
    double upkeep_ratio = 40.0 / farm_food;                  // wood per food produced
    double wood_floor = upkeep_ratio * weight[0] + 0.06;     // farm upkeep + a little for buildings
    weight[1] = std::max(weight[1], wood_floor);
    weight[0] = std::max(weight[0], 0.20);                   // never fully starve food/villagers
    // The quota WITHOUT the transient food-emergency override below -- i.e. the
    // mix this team is steering toward in the steady state, as opposed to what
    // it wants for the next few seconds. Only the rebalance (feature 19) uses
    // it, and it exists because that emergency flips the weights hard and
    // often: it trips at under 250 food, and food crosses 250 constantly (a
    // villager alone costs 50). Steering a symmetric rebalance off the live
    // weights therefore means chasing that flip in BOTH directions, which is
    // exactly what made the first attempt at this (feature 15) so destructive.
    double stable_w[4] = {weight[0], weight[1], weight[2], weight[3]};
    {
        double ssum = stable_w[0] + stable_w[1] + stable_w[2] + stable_w[3];
        if (ssum > 1e-6)
            for (int rt = 0; rt < 4; ++rt) stable_w[rt] /= ssum;
    }
    // Food emergency override: granary low -> pull hard onto food so villager/
    // age production never stalls.
    if (food < 250) { weight[0] = 0.60; weight[1] = 0.10; weight[2] = 0.22; weight[3] = 0.08; }
    // Normalise to a proper distribution (the deficit picker below compares
    // weight[rt]*total against current assignments).
    double wsum = weight[0] + weight[1] + weight[2] + weight[3];
    if (wsum > 1e-6)
        for (int rt = 0; rt < 4; ++rt) weight[rt] /= wsum;

    // Count villagers already assigned to each resource type (farms count as
    // food). Builders are excluded -- they're accounted for above.
    int cur[4] = {0, 0, 0, 0};
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (!u || !u->common.alive || u->common.team != team || !u->is_gatherer) continue;
        if (u->name == "fishing boat") continue; // counted/assigned separately below
        if (u->build_target.valid()) continue;
        int rt = -1;
        if (u->gather_target.valid()) {
            if (u->gather_target.kind == EntityKind::Building) rt = 0; // farm -> food
            else if (Resource* r = world.get_resource(u->gather_target)) rt = r->res.rtype;
        }
        if (rt < 0) rt = u->gather_rtype;
        if (rt >= 0 && rt < 4) cur[rt]++;
    }

    // A resource of `rtype` actually exists on the map (else assigning to it
    // just leaves the villager idle). Cached once per type below.
    auto type_available = [&](int rtype) {
        for (auto rref : world.active_resources) {
            Resource* r = world.get_resource(rref);
            if (r && r->common.alive && r->name != "fish" && r->res.rtype == rtype) return true;
        }
        return false;
    };
    bool avail[4] = {type_available(0), type_available(1), type_available(2), type_available(3)};
    // Farms are a food source too, even when the map's berries are gone.
    for (auto ref : team_buildings) {
        Building* b = world.get_building(ref);
        if (b && b->complete && b->name == "farm" && !b->exhausted) { avail[0] = true; break; }
    }

    // The opening the player asked for: five on food, then three on wood. Shared
    // by the idle-assignment loop below and by the rebalance pass further down,
    // which has to agree with it -- the rebalance is what moves the villagers
    // that spawned already assigned, and if it worked to the quota alone it
    // would just undo the opening on the next tick.
    constexpr int kOpenFood = 5, kOpenWood = 3;
    for (auto ref : idle_civs) {
        Unit* u = world.get(ref);
        if (u->gather_target.valid()) continue;
        int total = cur[0] + cur[1] + cur[2] + cur[3] + 1;
        int pick = -1;
        // ---- the opening is fixed, and wood has a floor ----------------------
        // The quota system below is a steady-state tool: it balances a workforce
        // that already exists. It is a poor opener, because with three villagers
        // the weights round to "one of everything" and the first minute is spent
        // spreading thin instead of building the food economy every other
        // decision depends on.
        //
        // So the first five gatherers go to FOOD and the next three to WOOD --
        // the standard opening, and the one the player asked for -- and only
        // past that does the quota system get a say.
        //
        // The floor outlasts the opening: never fewer than one wood gatherer per
        // two on food. Food buys villagers and ages; wood buys the farms that
        // keep the food coming (40 each, and they exhaust), plus every building.
        // A team that drifts all-food starves its own farm upkeep -- see
        // wood_floor in the weights above, which this enforces per-villager
        // rather than as a ratio the picker can round away.
        // Counted as HEADCOUNT ON EACH RESOURCE (cur[]), not as "how many have
        // been handed out so far". Against a running total the rule silently
        // fails: the villagers that spawn with the town centre are already on
        // wood before the first economy pass, so the total is past five before
        // anyone has touched a berry and the food half never fires at all --
        // measured openings sat at three-on-wood/none-on-food for the first
        // minute. Against headcount it also self-heals, which the running total
        // cannot do: pull a villager off food to put up a house and the next
        // idle one refills the gap.
        if (avail[0] && cur[0] < kOpenFood) {
            pick = 0;
        } else if (avail[1] && cur[1] < kOpenWood) {
            pick = 1;
        } else if (avail[1] && cur[1] * 2 < cur[0]) {
            pick = 1; // at least one on wood for every two on food
        }
        if (pick < 0) {
            // Steady state: the resource with the biggest shortfall vs its quota
            // that actually has a source available.
            double worst = -1e9;
            for (int rt = 0; rt < 4; ++rt) {
                if (!avail[rt] || weight[rt] <= 0.0) continue;
                double deficit = weight[rt] * total - cur[rt];
                if (deficit > worst) { worst = deficit; pick = rt; }
            }
        }
        if (pick < 0) continue;
        // For food, prefer an open farm (steady supply) over raw berries.
        EntityRef best = kNullRef;
        double bestd = 1e18;
        if (pick == 0) {
            for (auto bref : team_buildings) {
                Building* b = world.get_building(bref);
                if (!b || !b->complete || b->name != "farm" || b->exhausted || b->occupied_by.valid()) continue;
                double dx = b->common.x - u->common.x, dy = b->common.y - u->common.y;
                double d = dx * dx + dy * dy;
                if (d < bestd) { bestd = d; best = bref; }
            }
        }
        if (!best.valid()) {
            for (auto rref : world.active_resources) {
                Resource* r = world.get_resource(rref);
                if (!r || !r->common.alive || r->name == "fish" || r->res.rtype != pick) continue;
                double dx = r->common.x - u->common.x, dy = r->common.y - u->common.y;
                double d = dx * dx + dy * dy;
                if (d < bestd) { bestd = d; best = rref; }
            }
        }
        if (best.valid()) {
            u->gather_target = best;
            u->gather_rtype = pick;
            cur[pick]++;
        }
    }

    // ---- publish the workforce's unemployment (see Team::ai_idle_gatherers) --
    // Whatever is STILL without a gather target after the loop above had its
    // chance is a villager this map could not employ: either every resource type
    // it wants is exhausted/absent (`avail`), or the only ones left are too far
    // to have been picked. `idle_civs` has already had the construction crew and
    // any repair detail erased out of it, so what remains genuinely has no job.
    //
    // Computed unconditionally -- it is a free read (the list is already built)
    // and it costs baseline nothing, since only feature 23 acts on it.
    {
        int unemployed = 0;
        for (auto ref : idle_civs) {
            Unit* u = world.get(ref);
            if (u && u->common.alive && !u->gather_target.valid() && !u->build_target.valid() &&
                !u->repair_target.valid())
                ++unemployed;
        }
        td.ai_idle_gatherers = unemployed;
    }

    // ---- PROMOTED (was ai_variant == 19): rebalance TOWARD ANY resource -----
    //
    // The DAMPED form ships. The aggressive one (still reachable as
    // `--candidate-variant 15`) LOSES CATASTROPHICALLY -- 192 matches, 28.6% of
    // decided games, 95% CI 22.6-35.5, the worst result ever measured on this
    // AI -- and the two differ only in how hard they push. The differences are
    // marked inline below, and the reason each one matters is the same in every
    // case: a reassigned villager abandons its current trip and walks somewhere
    // new, so every "correction" costs real gathering time. The aggressive form
    // paid that cost roughly two thousand times a match across a workforce of
    // about thirty.
    //
    // What went wrong, precisely: it steered off the LIVE quota, which the food
    // emergency (food < 250) rewrites wholesale -- and food crosses 250
    // constantly. The old food-only rebalance survived that because it could
    // only ever push ONE WAY, so a flip cost it a little drift. Made symmetric,
    // the same flip becomes an oscillator, and the workforce spends the match
    // walking between wood and food instead of gathering either.
    const bool rebalance_v2 = variant_has(td.ai_variant, 19) && td.ai_map_derive;
    // The rebalance below only ever moves villagers ONTO FOOD. Every other
    // resource can therefore sit under its own quota indefinitely: nothing in
    // the AI ever moves a villager onto wood, oil or iron once the opening
    // assignment is done, because the only two things that assign a resource at
    // all are (a) the idle-villager loop above, which only ever sees villagers
    // that have NO gather target -- most of the workforce never has a free
    // moment -- and (b) update_gather's own re-seek, which deliberately picks
    // another node of the SAME type when one runs out.
    //
    // So the mix is set by whatever the opening happened to assign and then
    // drifts one way only, toward food, for the rest of the match. That is the
    // reported "the AI doesn't keep a consistent number of villagers on each
    // resource, e.g. wood" -- the quotas were being computed correctly and then
    // only half enforced.
    //
    // This makes the same machinery symmetric: find the type furthest UNDER its
    // quota and the type furthest OVER, and move villagers from the second to
    // the first.
    //
    // Feature 19 keeps that idea and damps it three ways, each aimed at a
    // specific way the aggressive version burned gathering time:
    //   * steer off `stable_w` (the quota WITHOUT the food-emergency override)
    //     rather than the live one, which removes the oscillator outright;
    //   * demand a gap of kRebalanceGap on BOTH sides, so it only corrects a
    //     genuine, sustained imbalance rather than every rounding wobble;
    //   * one move per pass, not two.
    // The emergency itself is not ignored, just left to the mechanism that
    // already handles it well: while food is short the idle-assignment loop
    // above puts every free villager and every new one onto food immediately,
    // which is a fast response that costs nothing, because those villagers were
    // not mid-trip to begin with.
    if (variant_has(td.ai_variant, 15) || rebalance_v2) {
        const double* quota = rebalance_v2 ? stable_w : weight;
        const double kRebalanceGap = rebalance_v2 ? 2.5 : 1.0;
        const int kRebalancePasses = rebalance_v2 ? 1 : 2;
        // Where a villager assigned to `rtype` should actually go. Food prefers
        // an unworked farm (steady, close to a dropoff) over raw berries, same
        // preference the idle-assignment loop above uses.
        auto find_node = [&](int rtype, Unit* u) {
            EntityRef best = kNullRef;
            double bestd = 1e18;
            if (rtype == 0) {
                for (auto bref : team_buildings) {
                    Building* b = world.get_building(bref);
                    if (!b || !b->complete || b->name != "farm" || b->exhausted ||
                        b->occupied_by.valid())
                        continue;
                    double dx = b->common.x - u->common.x, dy = b->common.y - u->common.y;
                    if (dx * dx + dy * dy < bestd) { bestd = dx * dx + dy * dy; best = bref; }
                }
            }
            if (!best.valid()) {
                for (auto rref : world.active_resources) {
                    Resource* r = world.get_resource(rref);
                    if (!r || !r->common.alive || r->name == "fish" || r->res.rtype != rtype) continue;
                    double dx = r->common.x - u->common.x, dy = r->common.y - u->common.y;
                    if (dx * dx + dy * dy < bestd) { bestd = dx * dx + dy * dy; best = rref; }
                }
            }
            return best;
        };
        for (int moved = 0; moved < kRebalancePasses; ++moved) {
            int total_r = cur[0] + cur[1] + cur[2] + cur[3];
            if (total_r <= 0) break;
            int under = -1, over = -1;
            double best_deficit = kRebalanceGap, best_surplus = kRebalanceGap;
            for (int rt = 0; rt < 4; ++rt) {
                double gap = quota[rt] * total_r - cur[rt];
                // Only a type with a quota AND somewhere to actually gather it
                // can receive villagers; anything can give them up (a type whose
                // quota has dropped to zero is pure surplus).
                if (avail[rt] && quota[rt] > 0.0 && gap > best_deficit) {
                    best_deficit = gap;
                    under = rt;
                }
                if (-gap > best_surplus) { best_surplus = -gap; over = rt; }
            }
            // ---- the opening and the wood floor both outrank the quota -------
            // Without this the rebalance simply undoes them on the next tick:
            // the steady-state weights put 35-55% of the workforce on food, so
            // a team sitting at the floor still reads as "short of food" and
            // this would pull the very villager the floor just placed back off
            // the woodline. The opening needs it for a second reason -- the
            // villagers that spawn with the town centre are never idle, so the
            // assignment loop above cannot reach them and THIS is the only pass
            // that can walk them over to the berries.
            //
            // Priority, highest first: fill food to kOpenFood, then wood to
            // kOpenWood, then the one-per-two wood floor, then the quota.
            int want[4] = {kOpenFood, kOpenWood, 0, 0};
            // Never rob a resource that is itself still short of its opening
            // target AND outranks the one asking -- otherwise food and wood take
            // turns stealing the same villager back and forth and neither target
            // is ever reached. Strictly one-way (rt < to), so food can always
            // pull off wood while it is short: at the three villagers a team
            // starts with, a symmetric guard deadlocks on the very first pass --
            // wood is short too, so it is untouchable, and food never opens.
            auto donor_for = [&](int to) {
                int pickd = -1, most = 0;
                for (int rt = 0; rt < 4; ++rt) {
                    if (rt == to || cur[rt] <= 0) continue;
                    if (rt < to && cur[rt] < want[rt] && avail[rt]) continue; // outranks it
                    if (cur[rt] > most) { most = cur[rt]; pickd = rt; }
                }
                return pickd;
            };
            for (int rt = 0; rt < 2; ++rt) {
                if (!avail[rt] || cur[rt] >= want[rt]) continue;
                int d = donor_for(rt);
                if (d < 0) break; // everyone else is short too; leave it to the quota
                under = rt;
                over = d;
                break;
            }
            if (under != 0 && avail[1] && cur[1] * 2 < cur[0]) {
                under = 1;
                if (over == 1 || over < 0) over = 0; // take it from food, not from wood
            }
            if (under < 0 || over < 0 || under == over) break;
            bool did_move = false;
            for (auto ref : world.active_units) {
                Unit* u = world.get(ref);
                if (!u || !u->common.alive || u->common.team != team || !u->is_gatherer) continue;
                if (u->name == "fishing boat" || u->build_target.valid()) continue;
                if (u->repair_target.valid()) continue; // mid-repair, leave it
                int rt = -1;
                if (u->gather_target.valid()) {
                    if (u->gather_target.kind == EntityKind::Building) rt = 0;
                    else if (Resource* r = world.get_resource(u->gather_target)) rt = r->res.rtype;
                }
                if (rt < 0) rt = u->gather_rtype;
                if (rt != over) continue;
                EntityRef dest = find_node(under, u);
                if (!dest.valid()) break; // nothing of that type reachable after all
                u->gather_target = dest;
                u->gather_rtype = under;
                u->drop_target = kNullRef; // the old dropoff may be the wrong one now
                --cur[over];
                ++cur[under];
                did_move = true;
                break;
            }
            if (!did_move) break;
        }
    } else if (avail[0]) {
        int total_r = cur[0] + cur[1] + cur[2] + cur[3];
        if (total_r > 0 && weight[0] * total_r - cur[0] >= 2.0) {
            int over = -1;
            double worst = 1.0; // require a real surplus, not a rounding wobble
            for (int rt = 1; rt < 4; ++rt) {
                double surplus = cur[rt] - weight[rt] * total_r;
                if (surplus > worst) { worst = surplus; over = rt; }
            }
            if (over >= 0) {
                for (auto ref : world.active_units) {
                    Unit* u = world.get(ref);
                    if (!u || !u->common.alive || u->common.team != team || !u->is_gatherer) continue;
                    if (u->name == "fishing boat" || u->build_target.valid()) continue;
                    int rt = -1;
                    if (u->gather_target.valid()) {
                        if (u->gather_target.kind == EntityKind::Building) rt = 0;
                        else if (Resource* r = world.get_resource(u->gather_target)) rt = r->res.rtype;
                    }
                    if (rt < 0) rt = u->gather_rtype;
                    if (rt != over) continue;
                    EntityRef best = kNullRef;
                    double bestd = 1e18;
                    for (auto bref : team_buildings) {
                        Building* b = world.get_building(bref);
                        if (!b || !b->complete || b->name != "farm" || b->exhausted ||
                            b->occupied_by.valid())
                            continue;
                        double dx = b->common.x - u->common.x, dy = b->common.y - u->common.y;
                        double d = dx * dx + dy * dy;
                        if (d < bestd) { bestd = d; best = bref; }
                    }
                    if (!best.valid()) {
                        for (auto rref : world.active_resources) {
                            Resource* r = world.get_resource(rref);
                            if (!r || !r->common.alive || r->name != "berry") continue;
                            double dx = r->common.x - u->common.x, dy = r->common.y - u->common.y;
                            double d = dx * dx + dy * dy;
                            if (d < bestd) { bestd = d; best = rref; }
                        }
                    }
                    if (best.valid()) {
                        u->gather_target = best;
                        u->gather_rtype = 0;
                        u->drop_target = kNullRef;
                        --cur[over];
                        ++cur[0];
                        break; // one move per pass -- gentle, no thrash
                    }
                }
            }
        }
    }

    // Fishing boats (naval/water plans): point any idle one at the nearest
    // fish shoal. unit_behavior re-seeks a fresh shoal on its own once one is
    // fished out (its is_ai fishing-boat branch), so a one-time assignment is
    // enough. Kept entirely out of the land-quota system above.
    if (td.ai_map_derive && td.ai_plan.can_fish) {
        double R = std::hypot(world.px_w, world.px_h);
        for (auto ref : world.active_units) {
            Unit* u = world.get(ref);
            if (!u || !u->common.alive || u->common.team != team || u->name != "fishing boat") continue;
            if (u->gather_target.valid() || u->carry > 0) continue;
            EntityRef fish = world.nearest(u->common.x, u->common.y, R, [&](EntityRef r, EntityCommon&) {
                Resource* rr = world.get_resource(r);
                return rr && rr->common.alive && rr->name == "fish";
            });
            if (fish.valid()) u->gather_target = fish;
        }
    }
}

void Control::ai_build(int team, World& world, EntityRef base, const std::vector<std::string>& names) {
    if (!base.valid()) return;
    Team& td = teams[team];
    const std::string& strat = td.strategy;
    int cap = td.cap, pop = td.pop;
    int house_count = static_cast<int>(std::count(names.begin(), names.end(), "house"));

    // Foundations already placed but not yet raised. Walls are excluded: a
    // choke line is dozens of segments raised by one chaining villager (see
    // ai_build_walls), so counting them would freeze the whole build order for
    // as long as the wall takes.
    //
    // pending_houses is the important one. Control::recompute only adds pop cap
    // for a house that is COMPLETE, so while a house foundation stands unbuilt
    // the team still reads as pop-blocked -- and `cap - pop <= 1` below fired
    // again on the very next ai_tick (every 0.05s on Hard), and again, and
    // again. That is exactly the reported bug: five house foundations dropped in
    // under a second, none of them finished, all of them redundant.
    //
    // NOT A RULES CHANGE. The pop-cap MECHANIC is untouched: recompute still
    // credits complete houses only, so a foundation gives the AI no population
    // room, exactly as it gives the player none. cap_projected below is a local
    // planning figure used ONLY by this function's "should I queue another
    // house?" predicates -- the mechanic answers "how much room do I have right
    // now", the build order needs "is more housing already on the way", and
    // answering the second with the first is what ordered five houses.
    int pending_total = 0, pending_houses = 0;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive || b->common.team != team || b->complete) continue;
        if (b->name == "palisade" || b->name == "iron wall") continue;
        // A foundation that has been making no progress for a while no longer
        // holds a work-in-progress slot -- see the watchdog in ai_tick. Without
        // this the cap turns one stuck foundation into a permanent build-order
        // deadlock. It still counts toward cap_projected: it IS still going to
        // be a house, and the watchdog will clear it if it never gets raised.
        if (b->ai_stall_t < 15.0) ++pending_total;
        if (b->name == "house") ++pending_houses;
    }
    int house_cap = (td.civ == 3 ? 5 : 4);
    int cap_projected = std::min(max_pop, cap + pending_houses * house_cap);

    // ---- PROMOTED (was ai_variant == 10, see variant_has): pop-cap headroom -
    // Every housing gate below carried its own hard-coded ceiling on the house
    // COUNT -- 8 for `low_house`, 12 for `pop_capped`, 10 for the lazy
    // catch-all -- so the loosest of them stopped the team at 12 houses no
    // matter what the game's population cap was. Twelve houses is 48 pop, plus
    // 8 per town centre: a one-base team is hard-capped at 56 population, and
    // even three bases only reach 72.
    //
    // That is not a throttle, it is a wall, and the AI spends the whole match
    // against it. The arena runs at --max-pop 200, and the per-match logs end
    // with the same handful of numbers over and over: pop=56, pop=64, pop=72.
    // Its own villager goal (AiPlan::vil_goal, up to 80) is on its own already
    // larger than the cap it allows itself, so a team that boomed to goal would
    // have no population left for an army at all -- which is exactly what peak
    // army (23-29) against peak villagers (up to 57) says is happening.
    //
    // Nothing about the ceiling was load-bearing: a house is 40 wood, and these
    // teams finish matches sitting on 8-18k banked wood, so ~18 more of them is
    // rounding error against a stockpile they never spend. Houses are also
    // food/wood dropoffs and are placed spread out (kHouseGap), so the extra
    // ones shorten carry trips rather than sprawling uselessly.
    //
    // So: derive the limit instead of hard-coding it -- exactly enough houses
    // to lift the cap to max_pop, given the town centres this team has. The
    // staggered URGENCY (headroom <= 1 / <= 2 / <= 5) is untouched and still
    // decides how eagerly each gate fires; only the arbitrary count ceiling
    // goes. The 30 clamp is a runaway guard, not a target: it is already past
    // what a 200-pop game needs.
    int house_limit_8 = 8, house_limit_10 = 10, house_limit_12 = 12;
    if (variant_has(td.ai_variant, 10) && td.ai_map_derive) {
        int base_count = static_cast<int>(std::count(names.begin(), names.end(), "base"));
        int from_bases = 8 * std::max(1, base_count);
        int need = (std::max(0, max_pop - from_bases) + house_cap - 1) / house_cap;
        house_limit_8 = house_limit_10 = house_limit_12 = std::clamp(need, 1, 30);
    }

    // Headroom of 2, not 1. With foundations now counted (above) this fires
    // once per genuinely-needed house instead of once per tick, so it can
    // afford to fire slightly EARLIER -- and it has to, because a house takes
    // real time to raise and a town centre that reaches the cap before the
    // house lands just idles. (Measured: at <= 1 the allied 20-min run lost
    // 5 villagers and gained 98s of idle TC purely to pop-blocking.)
    bool low_house = cap_projected < max_pop && cap_projected - pop <= 2 && house_count < house_limit_8;

    auto has = [&](const std::string& n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    auto count = [&](const std::string& n) {
        return static_cast<int>(std::count(names.begin(), names.end(), n));
    };
    int barracks_count = count("barracks");
    // ---- production saturation: back-pressure builds another building -------
    // "If the AI ever has more than 2 units in the queue it should make another
    // one of those production buildings."
    //
    // ai_train caps each building's queue at 2, so a queue never physically
    // exceeds that -- the observable signal for the same condition is that EVERY
    // building of a type is sitting at its cap while the team can still afford
    // to build. That means production capacity, not resources, is now the limit,
    // and it is the exact state that leaves resources banked: the AI wants more
    // units, has the money, and has nowhere to make them.
    //
    // Returns true when every completed building of `n` is queue-saturated (and
    // at least one exists). Deliberately checks completed buildings only -- one
    // still being raised is capacity already on the way.
    auto saturated = [&](const std::string& n) {
        int seen = 0;
        for (auto ref : world.active_buildings) {
            Building* b = world.get_building(ref);
            if (!b || !b->common.alive || b->common.team != team || !b->complete || b->name != n)
                continue;
            ++seen;
            if (b->queue.size() < 2) return false; // this one still has room
        }
        return seen > 0;
    };
    // Count only LIVE, non-exhausted farms -- an exhausted farm still exists as
    // a building (it gets re-sown), so counting by name alone would think the
    // food economy is fine while every farm is actually spent.
    //
    // free_farms is the same set narrowed to farms nobody is working -- the
    // number of farms that exist purely as spare capacity. A farm is placed
    // COMPLETE (World::place_building skips the construction phase for it), so
    // an unworked farm is not a foundation waiting on a builder; it is 40 wood
    // producing literally nothing. This is what the AI kept doing: nine farms,
    // two farmers. The build order below now gates on this rather than on the
    // farm count alone.
    int farm_count = 0, free_farms = 0;
    Building* spent_farm = nullptr;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive || b->common.team != team || b->name != "farm") continue;
        if (b->exhausted) {
            if (!spent_farm) spent_farm = b; // re-sow candidate, see below
            continue;
        }
        ++farm_count;
        if (b->complete && !b->occupied_by.valid()) ++free_farms;
    }

    // Farms are the ONLY renewable food source (berries deplete within minutes),
    // so after the early berries run out a team's entire food income is however
    // many WORKED farms it has.
    //
    // The count has to track the villagers actually on food, not the size of
    // the workforce. A farm feeds exactly one villager (Building::occupied_by
    // -- a second one sent to the same farm bounces off to another, see
    // update_gather), so a farm past the number of food villagers produces
    // nothing at all: it's 40 wood and a build slot for zero income. The
    // previous target was `civs/2 + 2` on the assumption that about half the
    // workforce ends up on food, but nothing enforces that split -- when the
    // resource weights put villagers on wood/oil/iron instead, the farm count
    // kept climbing with the total workforce anyway. Observed in a real game:
    // ~10 farms with ONE of them occupied.
    //
    // Tying it to the live food workforce makes it self-correcting in both
    // directions -- villagers move onto food, farms follow; they move off,
    // farms stop being built -- and berry gatherers count as food workers, so
    // the transition off a drying vein still ramps farms up ahead of demand.
    // ore_workers = villagers on oil or iron, the two carry types a house will
    // NOT accept (World::nearest_dropoff) -- they can only unload at the base or
    // a refinery. It drives the refinery timing below.
    int cur_civs = 0, food_workers = 0, ore_workers = 0;
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (!u || !u->common.alive || u->common.team != team || u->name != "civilian") continue;
        ++cur_civs;
        // What this villager is on RIGHT NOW: a farm is always food; a
        // resource carries its own rtype; gather_rtype is the fallback for
        // one between targets (see update_gather's re-seek).
        int rt = -1;
        if (u->gather_target.kind == EntityKind::Building) rt = 0;
        else if (Resource* r = world.get_resource(u->gather_target)) rt = r->res.rtype;
        if (rt < 0) rt = u->gather_rtype;
        if (rt == 0) ++food_workers;
        else if (rt == 2 || rt == 3) ++ore_workers;
    }
    // +1 spare so a villager switching to food always has a free farm to walk
    // onto rather than waiting for one to be built. It used to be +2, but the
    // real limiter is now the free_farms gate at the candidate push below --
    // food_workers counts BERRY gatherers as food workers, so a berry-fed
    // opening with 7 villagers on food read as "I want 9 farms" and built all
    // nine, while the resource quotas kept those same villagers on the berries
    // and only a couple ever walked onto a farm.
    //
    // ---- CANDIDATE (feature 23): the farm ceiling IS a workforce ceiling -----
    // The 24 below is the other half of the villager-saturation problem, and it
    // is the half that causes it. A farm feeds exactly one villager, so 24 farms
    // is 24 employable food workers -- and the food quota is 0.35-0.55 of the
    // workforce, so on a 200-pop map with a villager goal of 80 the AI intends
    // to put ~30-44 villagers on food and has room for 24 of them. The rest are
    // bought, walk to the food quota, find nothing, and stand there.
    //
    // Nothing made 24 the right number; it is simply where the clamp was put.
    // Raising it costs 40 wood per extra farm out of stockpiles these teams end
    // matches sitting on, and the free-farm gate at the build-order push below
    // (free_farms < 2) already refuses to build a farm nobody will work -- so
    // this raises the ceiling without loosening what fills it.
    const int farm_cap = (variant_has(td.ai_variant, 23) && td.ai_map_derive) ? 40 : 24;
    int desired_farms;
    if (td.ai_map_derive) {
        desired_farms = std::clamp(food_workers + 1, 2, farm_cap);
    } else {
        desired_farms = 3 + 2 * td.era; // campaign AI: authored behaviour, unchanged
    }
    // Starvation target -- a couple MORE spares than usual, because when a
    // team is actually starving the problem may be that its existing farms
    // turned out unreachable or unworkable rather than that it has too few.
    // Still anchored to the food workforce, so this can't degenerate into the
    // old workforce-scaled sprawl either.
    int emergency_farms = std::clamp(food_workers + 4, 4, farm_cap);
    // Prefer BERRIES first, farms later: while a decent berry vein is still up
    // near home, don't build farms at all -- gather the free berries for food
    // (the town centre keeps churning villagers off that). Only once the berries
    // are running out (few nodes left near base) does the AI transition to farms.
    // This is what the player wants: no frantic early farm-spam, berries drive
    // the opening economy. Count live berries within ~18 tiles of the base.
    int berries_near = 0;
    if (base.valid()) {
        Building* bb = world.get_building(base);
        double bx0 = bb ? bb->common.x : 0.0, by0 = bb ? bb->common.y : 0.0;
        double R2 = (18.0 * TILE) * (18.0 * TILE);
        for (auto ref : world.active_resources) {
            Resource* r = world.get_resource(ref);
            if (!r || !r->common.alive || r->name != "berry") continue;
            double dx = r->common.x - bx0, dy = r->common.y - by0;
            if (dx * dx + dy * dy <= R2) ++berries_near;
        }
    }
    // Transition to farms BEFORE the vein is exhausted (avoid a food gap), and
    // always once the economy has grown past the opening -- berries alone can't
    // sustain a big workforce. So: farms once the berry nodes near home are
    // thinning (<5 left) OR the villager count is past ~10. Still no farms at
    // tick zero with a full vein, which is the "berries first" the player wants.
    // FOOD EMERGENCY -- keyed on STARVATION, not on berry existence. A base can
    // have berries nearby that are actually UNREACHABLE (across water/blocked),
    // which fools a berry-count gate into thinking food is fine while the town
    // centre idles out and the economy freezes at a handful of villagers. So:
    // if food is critically low with a real workforce and no working farm, a
    // farm becomes the single highest priority -- ai_build_spot plants it on the
    // base's own reachable land, and a worked farm is guaranteed reachable food.
    // Keep firing while genuinely starving, NOT just until the first farm exists
    // (the old `farm_count == 0` gate): a bad spawn can build one farm that ends
    // up unreachable/unworked and produce zero food, and the team -- with
    // thousands of banked wood -- would then never build another, freezing at
    // its starting villagers (a villager costs 50 food, so 0 food means the town
    // centre can't even train the villager that would dig it out). While starved,
    // keep planting farms (40 wood each) on the base's own reachable land up to
    // the normal desired count, so the food workforce has real, near-home
    // targets the rebalance pass can put villagers on.
    // The free_farms < 3 term keeps even the emergency honest: if three farms
    // are already standing unworked, the team is not short of farms, it is short
    // of villagers ON them, and a fourth empty one changes nothing.
    bool food_emergency = td.ai_map_derive && td.res["food"] < 100.0 && cur_civs >= 6 &&
                          farm_count < emergency_farms && free_farms < 3;
    // Transition to farms BEFORE the vein is exhausted (avoid a food gap), once
    // the economy grows past the opening, OR in a food emergency. Still no farms
    // at tick zero with a full reachable vein -- the "berries first" the player
    // wants -- but a starving base always falls through to farms.
    bool need_farms = berries_near < 5 || cur_civs >= 10 || food_emergency;
    // Food economy at risk: pushes farms to the very top of the build order
    // below. Skirmish-only, so campaign AI keeps its exact old build priority.
    // Uses the emergency target too: at under 200 food the risk is a food
    // economy about to stall, which is exactly when a spare farm or two is
    // worth the wood.
    bool food_at_risk = td.ai_map_derive && need_farms && td.res["food"] < 200 &&
                        farm_count < emergency_farms && free_farms < 3;
    // Fortress only has anything to train for the two civs whose unique
    // roster lives there (Germany's waffen SS, Ottoman's janissary/camel
    // corps -- see PRODUCTION["fortress"] and the CIV_ONLY_UNITS/
    // OTTOMAN_ONLY gates in control.cpp); every other civ would just be
    // paying for a building with an empty unit list, so this is checked
    // BEFORE fortress ever enters the priority chain below.
    bool fortress_useful = !available_units("fortress", team).empty();
    // "Good amount of resources" gate for war-era base expansion -- a 50%
    // margin over the 275 wood / 100 iron cost (not just bare affordability),
    // so a second base never cannibalizes an economy that's still catching
    // up on its basics. Kept modest deliberately: iron in particular is
    // heavily contested by factory/airbase/shipyard unit production (light
    // tank 110, heavy tank 200, artillery 100, fighter 120, bomber 350 --
    // see catalog.json's units table) once those buildings exist, so a
    // stricter flat threshold (originally 200 iron, ~2x cost) turned out to
    // almost never survive a single ai_tick cycle in real self-play --
    // confirmed via tournament: 0 of 24 matches ever built a second base,
    // including one that reached War Era with a strong economy. A live-
    // stockpile gate at all is inherently in tension with a continuously-
    // spending military queue; this is the least-strict version of it that
    // still means something.
    bool flush_for_expansion = td.res["wood"] >= 400 && td.res["iron"] >= 150;
    // ---- THE BANK IS THE SYMPTOM (feature 34) -------------------------------
    // The tempo doctrine (see ai_economy) removed every throttle on SPENDING and
    // the AI still finishes matches on five figures of banked resources. That is
    // because spending was never the binding constraint: production CAPACITY is.
    // ai_train queues at most 2 deep per building, so a team with two barracks
    // and a factory can have at most six units in flight no matter what it owns,
    // and the moment all three are full the income has nowhere to go again.
    //
    // Every existing rule that adds a production building is reactive -- it
    // waits for `saturated` (every building of that type at its queue cap) and
    // then adds exactly one. That is a treadmill: capacity only ever grows one
    // step behind demand, and it stops entirely whenever a tick happens to catch
    // a queue with one slot free.
    //
    // `flush` is the direct read of the failure instead: a stockpile this size
    // means the team has already bought everything it knows how to buy. When it
    // is true, capacity is what is missing, and the answer is more buildings --
    // spread out, which is why feature 34 also turns the placement gap back on
    // for production buildings (see ai_build_spot).
    bool flush = td.ai_map_derive && (td.res["food"] > 1500.0 || td.res["wood"] > 1500.0 ||
                                      td.res["iron"] > 1200.0 || td.res["oil"] > 1200.0);
    bool pop_capped = cap_projected < max_pop && cap_projected - pop <= 1 && house_count < house_limit_12;

    // Muscateer-rush build order, in priority order. Housing and farms are
    // interleaved with military so the rush never chokes on pop cap or food;
    // extra barracks widen muscateer throughput; refinery/factory back the
    // oil economy and later-age siege. Academy/fortress widen the roster past
    // pure muscateer-line infantry so civ identity (cavalry/swordsman for
    // everyone, camel/waffen/janissary for the civs that have them) actually
    // shows up in what the AI fields, not just tanks/muscateers regardless
    // of civ.
    //
    // Every condition that's currently true gets a slot in `candidates`, in
    // this priority order -- NOT just the first one, unlike the old if/else
    // chain. That chain stopped dead the instant its single top pick turned
    // out unaffordable or physically unplaceable (no valid ai_build_spot),
    // which silently blocked every lower-priority item forever -- including
    // any war-era base expansion, confirmed via tournament metrics where
    // bases_built stayed exactly 1 across 40 team-instances even in matches
    // that reached War Era with resources to spare (farms are the likely
    // culprit: desired_farms climbs to 7-9 on the default water=true map,
    // where buildable land near the base runs out). The loop below tries
    // each candidate in order and only stops at the first one that's both
    // affordable and has a real spot, so a stuck top-priority item just gets
    // skipped instead of jamming the whole build order.
    std::vector<std::string> candidates;
    // ABSOLUTE TOP, above even the food emergency: being attacked with no army
    // to answer with and nowhere to make one. Starving is survivable; having
    // the base taken apart while the build order works through farms is not.
    // Only fires when there is genuinely no military production on the map for
    // this team -- once a barracks exists, ai_train's raised floor takes over
    // and this stops competing with the rest of the build order.
    if (td.ai_retaliate && !has("barracks") && !has("academy") && !has("fortress"))
        candidates.push_back("barracks");
    if (food_emergency) candidates.push_back("farm");                   // ABSOLUTE TOP: starving, TC idling out
    if (pop_capped) candidates.push_back("house");                      // nothing else builds past pop cap
    if (food_at_risk) candidates.push_back("farm");                     // a starved food economy is fatal
    // ---- AGE PREREQUISITES FIRST (tempo doctrine, see ai_economy) ----------
    // Industrial needs TWO of {barracks, academy, market, refinery, shipyard}
    // (World::can_age_up). Those buildings used to be scattered down the order
    // -- the barracks behind a 7-villager gate, the market near the bottom, and
    // the second one only pushed for "economic" plans -- so a team could reach
    // its villager target with the resources for an age-up and no legal way to
    // spend them, which is the "banks resources, never advances" behaviour.
    //
    // The age is the biggest single power spike in the game and Victorian is the
    // weakest era, so while the prerequisite is missing it outranks everything
    // except starving and being pop-blocked. Barracks first of the two: it is
    // also where the army comes from, which is the other half of the same
    // complaint. Both are cheap (150 and 100 wood).
    if (td.ai_map_derive && td.era == 0 && !world.can_age_up(team)) {
        if (!has("barracks")) candidates.push_back("barracks");
        if (!has("market")) candidates.push_back("market");
        if (!has("academy")) candidates.push_back("academy");
    }
    // The FIRST barracks. An aggressive plan opens with one -- that IS the
    // rush, and it's supposed to. Every other plan (i.e. the default) now
    // waits until an opening economy actually exists.
    //
    // This push was previously unconditional, sitting 4th in the order above
    // houses, the academy and every farm, so EVERY skirmish AI regardless of
    // its map-derived plan put up a barracks inside the first minute and
    // started making muscateers off a three-villager economy. ai_train's
    // boom-phase hold only ever capped the RATE at the defensive floor; it
    // never stopped the building itself going up first and soaking the
    // opening's wood, which is the behaviour that reads as "it always
    // rushes".
    //
    // The gate opens on any of: a real workforce, reaching Industrial, or a
    // genuine attack near home -- so an economy-first plan still can't be
    // caught defenceless by someone else's rush.
    //
    // The threat term needs TWO enemy soldiers, not one. Every team starts
    // with a cavalry and ai_scout sends a spare unit sweeping the map in era
    // 0, so a single enemy unit within the 24-tile radius is far more often
    // a scout wandering past than an attack -- and at `> 0` that lone scout
    // tripped the gate and bought the instant barracks straight back.
    //
    // ai_retaliate opens it on a SINGLE attacker, which the bare ai_threat term
    // deliberately won't: that flag additionally requires actual damage taken,
    // so the lone-scout false positive the >= 2 threshold exists to suppress
    // can't reach it.
    //
    // The `ai_threat >= 2` head-count is now backed up by the actual read of
    // what those bodies are DOING (Team::ai_read): a classified Raid or Push
    // opens the gate on its own -- including while the force is still crossing
    // the map, which is the only point at which building a barracks can still
    // matter -- while a Scout explicitly does NOT, however many times it wanders
    // back and forth through the radius.
    using ThreatKind = Team::EnemyRead::Threat;
    bool real_attack = td.ai_read.threat == ThreatKind::Raid ||
                       td.ai_read.threat == ThreatKind::Push;
    bool rush_plan = !td.ai_map_derive || td.ai_plan.playstyle == "aggressive";
    // The workforce gate was 12 villagers, which measured out at 4-10 SIM-MINUTES
    // before a non-aggressive plan had any military building at all -- so for most
    // of the opening there was nowhere to spend on units even when ai_train wanted
    // to, and the AI read as a free win in live play. 7 is still "an opening
    // economy exists" (past the first houses/farms, off the three-villager start
    // the old unconditional push was criticised for) but early enough that the
    // barracks is up while the boom-ramp trickle (kBoomMilShare) still has most of
    // era 0 left to run. Campaign AI is unaffected -- rush_plan short-circuits it.
    bool army_time = rush_plan || cur_civs >= 7 || td.era >= 1 || td.ai_threat >= 2 ||
                     td.ai_retaliate || real_attack;
    if (army_time && !has("barracks")) candidates.push_back("barracks"); // core: muscateers
    // Shipyards only go up on a map that can actually support a navy: one that
    // has fish to work, or where water covers more than 20% of the terrain.
    // Team::strategy alone ("navy" on every want_water map -- scenario.cpp)
    // said nothing about either, so a map with a single coastal strip and no
    // shoals got the full five-dock naval build-out and the wood that paid for
    // it bought nothing: no fishing boats to feed the economy, and warships
    // with no water worth contesting. AiPlan::naval_viable is the whole-map
    // measurement of both (ai_assess_map).
    //
    // Navy plans build MULTIPLE shipyards (up to 5) as the economy grows, so
    // fishing + warship production scales instead of bottlenecking on one dock.
    // The first goes up immediately; each extra waits for era 1 and a wood
    // surplus so it ramps rather than spamming five at once.
    //
    // The one case that overrides the gate is a team whose enemy is across
    // WATER with no land route: a transport is the only way its army ever
    // reaches the enemy, so it gets exactly ONE dock to build that boat even
    // on a map the gate calls dry (a narrow river splitting an otherwise
    // land map) -- never the naval build-out.
    bool naval_map = !td.ai_map_derive || td.ai_plan.naval_viable;
    if (strat == "navy" && naval_map) {
        int syc = count("shipyard");
        if (syc < 1) candidates.push_back("shipyard");
        else if (syc < 5 && td.era >= 1 && td.res["wood"] > 400.0) candidates.push_back("shipyard");
    } else if (!td.ai_plan.land_to_enemy && count("shipyard") < 1) {
        candidates.push_back("shipyard"); // stranded: needs a transport or it can never attack
    }
    // Early pop headroom. ONE house up front, not two: a house is +4 pop on top
    // of the base's 8, the opening only trains villagers, and the second one was
    // routinely placed before the first was even raised. The `low_house` rule
    // above (now foundation-aware) adds the next one exactly when the cap is
    // actually being approached, which is what "as new houses are needed" means.
    if (house_count < 1) candidates.push_back("house");
    if (low_house) candidates.push_back("house");                       // urgent: near pop cap
    // ---- PROMOTED (was ai_variant == 16): build housing AHEAD of the cap ----
    // Housing was only ever REACTIVE. The three gates that ask for a house fire
    // at 1, 2 and 5 population of headroom, and the widest of those three sits
    // at the very BOTTOM of this priority list -- below farms, towers, the
    // academy, extra barracks, the refinery, the market and every era
    // prerequisite. Since the loop below places the first affordable candidate
    // and stops, a house at the bottom is only ever built in a tick where the
    // AI wants nothing else at all.
    //
    // Both halves of that are wrong for a building whose entire job is to be
    // ready BEFORE it is needed. Five population is a little over one villager's
    // train time at a working economy, and a house still has to be placed,
    // walked to and raised after that -- so the town centre reliably hits the
    // cap and idles anyway, which is the reported "it doesn't make houses
    // proactively". The pop cap is also uniquely blocking: at the cap NOTHING
    // trains, so a missing house stalls the economy AND the army at once, which
    // no other building on this list does.
    //
    // So: a wider margin (kProactiveHeadroom, ~2 houses of slack), placed HIGH
    // in the order -- above the economy build-out, below only the genuine
    // emergencies. Bounded by the same house_limit as every other gate, so it
    // stops entirely once the cap can reach max_pop; it builds housing sooner,
    // never more of it.
    // Widened from 8 with the tempo doctrine (see ai_economy). Under constant
    // villager AND military production, population is consumed several times
    // faster than it was under the old throttle, so a margin that was
    // comfortable before now evaporates while the house is still being raised --
    // and hitting the cap stops BOTH production lines at once.
    //
    // It scales with the number of production buildings for exactly that reason:
    // the drain is one pop per building per train cycle, so a team with six
    // production lines eats headroom six times faster than one with a single
    // barracks. This is also the most likely answer to "the AI clearly has a lot
    // of resources banked up" -- a pop-blocked team cannot spend, whatever it
    // has, because every queue it owns is refusing to start.
    int prod_lines = count("barracks") + count("academy") + count("factory") +
                     count("airbase") + count("fortress") + count("base");
    int kProactiveHeadroom = std::clamp(6 + 2 * prod_lines, 12, 30);
    if (variant_has(td.ai_variant, 16) && td.ai_map_derive && cap_projected < max_pop &&
        cap_projected - pop <= kProactiveHeadroom && house_count < house_limit_10)
        candidates.push_back("house");
    // Economic plans raise their academy during the boom (before the mass-farm
    // build-out), so the OIL-FREE food->army outlet is ready the moment the army
    // phase begins; a food-flush economy adds a 2nd to widen the pipeline. It's
    // also a 2nd age-qualifying building. (Plan 1: convert food into cavalry/
    // swordsman on oil-poor maps.)
    bool econ_plan = td.ai_map_derive && td.ai_plan.playstyle != "aggressive";
    bool land_to_enemy = !td.ai_map_derive || td.ai_plan.land_to_enemy;
    if (econ_plan && !has("academy")) candidates.push_back("academy");
    if (econ_plan && td.res["food"] > 500.0 && count("academy") < 2) candidates.push_back("academy");
    // Era 0: SECURE the second age-qualifying building. Industrial wants 2 of
    // {barracks, academy, market, refinery, shipyard} (World::can_age_up),
    // and refinery is itself era-1-gated -- so with the opening barracks now
    // deferred, an economic plan's academy is only one of the two and the
    // delayed barracks would quietly BECOME its age gate. The market is the
    // natural partner: cheap, non-military, and its trade covers resource
    // shortfalls. Aging dominates everything downstream, so this is worth a
    // high slot rather than the market's usual spot near the bottom.
    if (econ_plan && td.era == 0 && !world.can_age_up(team) && !has("market"))
        candidates.push_back("market");
    // Once at Industrial, SECURE the 2 War prerequisites early -- factory +
    // university (or an airbase on a pure island). They were buildable but sat
    // at the bottom of the order behind endless farms/houses, so economic teams
    // sat on huge stockpiles (26k+ food) stuck at Industrial purely for lack of
    // these two buildings. Prioritising them here is what actually unlocks War.
    // No longer gated on econ_plan. War needs TWO of {factory, university,
    // airbase} and nothing else will do, so for ANY plan these are the only
    // things standing between Industrial and the next age -- and the player's
    // report is that the AI sits in Industrial too long with resources banked,
    // which is precisely what a missing prerequisite looks like from outside.
    // An aggressive plan wants the factory as much as anyone: it is where tanks
    // and AA come from.
    if (td.era >= 1 && td.era < 2 && !world.can_age_up(team)) {
        if (land_to_enemy && !has("factory")) candidates.push_back("factory");
        if (!has("university")) candidates.push_back("university");
        if (!land_to_enemy && !has("airbase")) candidates.push_back("airbase");
        // Third option: if the factory or university can't be placed at all, the
        // airbase is the other qualifying building, and it is worth having in
        // its own right once aircraft are on the table.
        if (!has("airbase")) candidates.push_back("airbase");
    }
    // Once at War, SECURE the Scientific prerequisite the same way. can_age_up
    // (Scientific) needs 2 of {base,fortress} (the start base counts as 1) OR a
    // lone fortress -- so a 2nd base is the cheapest unlock AND doubles as the
    // resource expansion the user wants; a fortress is the fallback if a base
    // can't be placed. Without this, era-2 teams sat on 7k+ oil unable to
    // advance for lack of the building, not the resources.
    // ---- CANDIDATE (feature 21): EVERY plan needs the age, not just economic
    // ones ---------------------------------------------------------------------
    // The Industrial->War push directly above was deliberately un-gated from
    // econ_plan ("No longer gated on econ_plan... for ANY plan these are the only
    // things standing between Industrial and the next age"). This one, the very
    // next tier up, still is -- so an aggressive map-plan reaching War era has NO
    // path to Scientific at all except the expansion base further down, which is
    // behind flush_for_expansion (400 wood AND 150 iron banked simultaneously,
    // which the comment there records as almost never surviving a single tick
    // against a continuously-spending military queue).
    //
    // The reasoning that un-gated the tier below applies here word for word: the
    // Scientific gate is 2 of {base, fortress} and nothing else will do, so while
    // it is unmet the resources have nowhere to go and the team sits in War era
    // with a stockpile -- exactly the shape of the original complaint. An
    // aggressive plan wants the fortress as much as anyone; it is also where the
    // civ-unique elites and the ballistic missile come from.
    bool age_prereq_push = econ_plan || (variant_has(td.ai_variant, 21) && td.ai_map_derive);
    if (age_prereq_push && td.era >= 2 && !world.can_age_up(team)) {
        if (count("base") < 3) candidates.push_back("base");
        if (!has("fortress")) candidates.push_back("fortress");
    }
    // Farms only once berries are drying up (need_farms) -- until then the free
    // berry vein feeds the economy and farms would just be premature.
    // ...and only while there isn't already spare farm capacity standing idle.
    // One unworked farm ahead of demand is the buffer a villager switching to
    // food walks onto; a second is already speculative. Past that, more farms
    // add zero income (a farm feeds exactly one villager -- Building::
    // occupied_by), so they are pure wasted wood and wasted build slots. The
    // gate is self-correcting in both directions: as villagers occupy farms,
    // free_farms drops and the next farm goes up immediately.
    if ((need_farms || !td.ai_map_derive) && farm_count < desired_farms && free_farms < 2)
        candidates.push_back("farm"); // sustainable food (replaces depleting berries)
    // A defensive map-plan puts up an extra tower (usually behind its choke
    // wall); everyone else keeps the single minimal watchtower.
    int tower_goal = (td.ai_map_derive && td.ai_plan.playstyle == "defensive") ? 2 : 1;
    // ---- CANDIDATE (feature 24): static defence that scales with the game ----
    // ONE tower, for the whole match, at every era. A tower is the only thing
    // this team owns that defends home while the army is away on a push -- it
    // costs no population, it never gets pulled into the offensive rally, and it
    // cannot be lured off by a scout the way ai_defend_civilians' villagers can.
    // Meanwhile the thing it has to survive grows every era: era 0 raids are a
    // cavalry, era 2 raids are tanks.
    //
    // Not open-ended. Towers do not win games -- the arena's own lesson is that
    // an economic/defensive investment has to be paid for out of tempo -- so this
    // is a small ramp (2 at Victorian up to 5 at Scientific) and it still sits in
    // the same slot in the build order, below the age prerequisites and the food
    // economy, not above them.
    if (variant_has(td.ai_variant, 24) && td.ai_map_derive)
        tower_goal = std::clamp(2 + td.era + (td.ai_plan.playstyle == "defensive" ? 1 : 0), 2, 5);
    if (count("tower") < tower_goal) candidates.push_back("tower");     // defense once economy's up
    if (!has("academy")) candidates.push_back("academy");               // civ variety: swordsman/cavalry/camel
    // Also army_time-gated: with no barracks yet this condition is true too,
    // so leaving it open would just build the first one here and walk
    // straight around the gate above.
    // Widen a production line the moment it is the bottleneck (see `saturated`).
    // These sit high in the order on purpose: an idle barracks is a building
    // that will be used the instant it exists, and every tick spent saturated is
    // resources accumulating with nowhere to go.
    // Caps widened with feature 34: the old 6/4/4/3 were sized against a
    // reactive treadmill that never got near them anyway (matches typically end
    // on 2-3 barracks). A team with 1500 banked wood is not being protected by a
    // ceiling of six, it is being stopped by one.
    const bool cap34 = variant_has(td.ai_variant, 34) && td.ai_map_derive;
    const int kMaxBarracks = cap34 ? 10 : 6;
    const int kMaxAcademy = cap34 ? 6 : 4;
    const int kMaxFactory = cap34 ? 6 : 4;
    const int kMaxAirbase = cap34 ? 4 : 3;
    if (army_time && saturated("barracks") && barracks_count < kMaxBarracks)
        candidates.push_back("barracks");
    if (saturated("academy") && count("academy") < kMaxAcademy) candidates.push_back("academy");
    if (saturated("factory") && count("factory") < kMaxFactory) candidates.push_back("factory");
    if (saturated("airbase") && count("airbase") < kMaxAirbase) candidates.push_back("airbase");
    // ---- STOP HOARDING (feature 34) -----------------------------------------
    // Not gated on `saturated` at all -- that is the reactive rule above, and by
    // the time it fires the resources are already sitting idle. A stockpile this
    // deep is a standing instruction to widen every line the team is allowed to
    // have. One building per call still, so this ramps rather than carpeting the
    // map, and it switches itself off the moment the bank comes down.
    if (cap34 && flush) {
        if (army_time && barracks_count < kMaxBarracks) candidates.push_back("barracks");
        if (count("academy") < kMaxAcademy) candidates.push_back("academy");
        if (td.era >= 1 && land_to_enemy && count("factory") < kMaxFactory)
            candidates.push_back("factory");
        if (td.era >= 1 && count("airbase") < kMaxAirbase) candidates.push_back("airbase");
        if (td.era >= 2 && fortress_useful && count("fortress") < 2) candidates.push_back("fortress");
    }
    if (army_time && barracks_count < 2) candidates.push_back("barracks"); // second muscateer line
    // Refinery: no longer era-1-gated. Nothing in the catalog gates it, and the
    // era-0 resource weights already put 15-35% of the workforce on oil -- with
    // the base as their only legal dropoff (a house refuses oil/iron), so every
    // one of those villagers walked the full distance home on every trip while
    // the AI waited for Industrial to build the one building that would fix it.
    // 80 wood, placed ON the patch by ai_build_spot. Gated on miners existing so
    // an opening with nobody on ore still doesn't spend the wood.
    if (!has("refinery") && (td.era >= 1 || ore_workers >= 3)) candidates.push_back("refinery");
    if (!has("market")) candidates.push_back("market");                 // trade to cover shortfalls
    // Expand to a second/third base once at War era with a healthy surplus
    // banked -- more pop cap (+8 each, see the has_base recompute above), a
    // second dropoff, and a forward foothold for training. Sits below the
    // core economy so it never steals from a team still building its
    // basics, but above the late-game military tech below.
    // ---- expand the ECONOMY, not just the age requirement -------------------
    // The only two things that ever asked for a second town centre were the
    // Scientific-age prerequisite and flush_for_expansion (400 wood AND 150
    // iron banked at the same instant, which a continuously-spending military
    // queue almost never leaves standing). So a team could sit on a picked-clean
    // home patch for the rest of the match with villagers walking further and
    // further for every load.
    //
    // An expansion is worth it on its own terms: it is pop cap, a second
    // villager line (every town centre trains -- feature 11), and above all a
    // dropoff next to fresh resources, which is what ai_build_spot now aims it
    // at. Gate it on having the workforce to justify one rather than on a
    // stockpile that never survives a tick -- afford() below still has the
    // final say, so this asks at a sensible moment instead of never.
    //
    // Era 2 is not a choice: BUILDING_ERA puts the buildable base at War era.
    bool expand_economy = td.ai_map_derive && td.era >= 2 && cur_civs >= 20;
    if (td.era >= 2 && (flush_for_expansion || expand_economy) && count("base") < 4)
        candidates.push_back("base");
    if (!has("fortress") && td.era >= 2 && fortress_useful) candidates.push_back("fortress"); // civ-unique elites
    // Lower-priority fallback for the factory/university/airbase (non-economic
    // plans, or an economic plan that already has the 2 War-prereqs and just
    // wants the extra production). land_to_enemy is computed above.
    // Factory only when its tanks can actually reach the enemy overland (any
    // map with a land route, water or not) -- on a pure island an airbase's
    // planes are the useful War-prereq instead. University is always worth it.
    if (!has("factory") && td.era >= 1 && land_to_enemy) candidates.push_back("factory"); // siege + War prereq
    if (!has("university") && td.era >= 1) candidates.push_back("university"); // upgrades + War prereq
    if (!has("airbase") && td.era >= 1 && !land_to_enemy) candidates.push_back("airbase"); // island: planes + War prereq
    if (strat == "navy" && !has("airbase") && td.era >= 2) candidates.push_back("airbase");
    // CANDIDATE (ai_variant == 2): an airbase on a LAND map too.
    //
    // Every airbase condition above needs either !land_to_enemy (island) or a
    // navy map, so on an ordinary land map the AI never builds one AT ALL --
    // measured across 384 self-play games on Ostland, which also explains the
    // zero bombers. That silently deletes a whole branch of the game from the
    // AI's repertoire: fighters, bombers, heavy bombers, jets, and the atomic
    // bomb (an airbase tech) -- and, by never fielding aircraft, it also makes
    // the player's AA guns and flak towers pointless. Gated to War era so it
    // comes after the factory rather than competing with it.
    if (td.ai_variant == 2 && !has("airbase") && td.era >= 2 && land_to_enemy)
        candidates.push_back("airbase");
    // ---- CANDIDATE (feature 22): an AIR FORCE on a land map ------------------
    // Variant 2 above is the same observation and was never measured; this is it
    // with the piece that was missing. One airbase is not an air force: aircraft
    // burn fuel and fly home to refuel (aircraft_behavior), so a single base far
    // from the front means a plane spends most of the match in transit, and a
    // bomber only earns its 350 iron by making repeated passes.
    //
    // The airbase IS already reachable on a land map in one narrow window -- as
    // the "third option" War prerequisite -- but that push stops the instant
    // can_age_up goes true, which the factory and university satisfy on their
    // own. So in practice a land map builds factory + university and the airbase
    // never happens, which is why 384 self-play games on Ostland produced zero
    // bombers. This asks for it in its own right, at War era so it comes after
    // the factory rather than competing with it, and asks for a second one so
    // there is somewhere to rearm near the fighting.
    if (variant_has(td.ai_variant, 22) && td.ai_map_derive && td.era >= 2 && land_to_enemy &&
        count("airbase") < 2)
        candidates.push_back("airbase");
    if (barracks_count < 3 && td.era >= 1) candidates.push_back("barracks"); // third line, mass muscateers
    if (cap_projected < max_pop && cap_projected - pop <= 5 && house_count < house_limit_10)
        candidates.push_back("house");

    // ---- work-in-progress cap -------------------------------------------
    // ai_build places at most one building per call, but on Hard it is called
    // every 0.05s -- so "one per call" is no throttle at all, and the AI could
    // (and did) lay five foundations inside a second, then leave them all
    // half-built because the villagers to raise them didn't exist. Cap how many
    // foundations may be open at once, scaled to the workforce that has to raise
    // them: ~1 per 5 villagers, 1 minimum, 3 maximum. Farms are unaffected --
    // World::place_building spawns them already complete, so they are never
    // foundations (their over-building is handled by the free-farm gate above).
    // Housing is the ONE exemption. Pop-blocking freezes villager production
    // outright, so refusing a house because a barracks happens to be half-built
    // is strictly worse than the extra foundation -- and the cap_projected
    // accounting above is what actually bounds houses now, since each pending
    // one immediately satisfies the very check that would have asked for the
    // next. Everything else waits its turn.
    int wip_limit = std::clamp(cur_civs / 5, 1, 3);
    // Feature 34: the cap is sized to the workforce that has to RAISE the
    // foundations, which is the right constraint when resources are tight. A
    // team sitting on four figures of everything is not short of builders'
    // wages, it is short of buildings, and one-at-a-time is how the build order
    // stays permanently behind. Still bounded -- half the workforce cannot end
    // up on construction -- but it lets several go up at once while the bank is
    // deep, and reverts the moment it is not.
    if (variant_has(td.ai_variant, 34) && td.ai_map_derive && flush)
        wip_limit = std::clamp(cur_civs / 4, 2, 6);
    bool wip_full = pending_total >= wip_limit;

    // ---- re-sow a spent farm instead of placing a new one elsewhere -------
    // Only when the build order actually wants a farm (so this rides the same
    // need_farms / desired_farms / free_farms gating as a new placement, and
    // can't quietly re-sow a field nobody will work).
    //
    // An exhausted farm keeps its tile forever, and that tile is PRIME: farms
    // are deliberately placed flush against a dropoff (ai_build_spot packs the
    // base's own surroundings first), so a spent farm is squatting on some of
    // the best ground the team has. A fresh farm can't use it -- footprint_clear
    // rejects the occupied tile -- so every replacement lands further out, each
    // cycle lengthening the carry trip, which is what the player saw.
    //
    // Re-sowing costs a flat 40 WOOD (never food) -- the same resow price the
    // human pays, whether by right-clicking a dead farm or via the market's
    // auto-replant toggle (see unit_behavior.cpp's replant path), and the same
    // price as the initial farm build. Exempt from the WIP cap because it places
    // no foundation (a farm is never under construction) and frees a build slot
    // rather than taking one.
    constexpr double kFarmResowWood = 40.0;
    if (spent_farm && td.ai_map_derive && td.res["wood"] >= kFarmResowWood &&
        std::find(candidates.begin(), candidates.end(), "farm") != candidates.end()) {
        td.res["wood"] -= kFarmResowWood;
        spent_farm->amount = spent_farm->max_farm_food;
        spent_farm->exhausted = false;
        world.events.push(
            {EventType::Sound, "build", spent_farm->common.x, spent_farm->common.y, 400, kNullRef, ""});
        return; // one build action per call, same as a placement
    }

    // Housing's exemption from the work-in-progress cap widens with feature 16
    // for the same reason it moves up the priority list: a proactive house that
    // waits behind a half-raised barracks lands after the cap has already been
    // hit, which is exactly the stall being fixed.
    bool house_exempt = pop_capped || (variant_has(td.ai_variant, 16) && td.ai_map_derive &&
                                       cap_projected < max_pop &&
                                       cap_projected - pop <= kProactiveHeadroom);
    // ---- CANDIDATE (feature 21): an age prerequisite jumps the WIP queue ------
    // Housing is currently the only thing exempt from the work-in-progress cap,
    // on the argument that pop-blocking freezes BOTH production lines at once so
    // the extra foundation is cheaper than the stall. A missing age prerequisite
    // is the same argument one level up and considerably worse: while it is
    // absent the age itself is illegal (World::enqueue rejects it), so the single
    // biggest power spike in the game is unavailable however much is banked --
    // and unlike a pop stall, nothing about it self-resolves.
    //
    // The cap is 1-3 foundations, sized to the workforce, and it is FIFO by
    // priority order, so a prerequisite asked for on the same tick as a farm and
    // a house simply waits. Waiting is what the tempo doctrine exists to stop.
    // The names come straight from World::can_age_up's own tiers rather than a
    // second copy of the rule, and the whole thing switches off the moment the
    // gate is satisfied.
    std::set<std::string> age_prereq_names;
    if (variant_has(td.ai_variant, 21) && td.ai_map_derive && td.era < 3 && !world.can_age_up(team)) {
        if (td.era == 0) age_prereq_names = {"barracks", "academy", "market", "refinery", "shipyard"};
        else if (td.era == 1) age_prereq_names = {"factory", "university", "airbase"};
        else age_prereq_names = {"base", "fortress"};
    }
    for (const auto& want : candidates) {
        if (wip_full && !(want == "house" && house_exempt) && !age_prereq_names.count(want)) continue;
        if (!civ_has(want, td.civ) || !afford(want, team)) continue;
        auto spot = ai_build_spot(team, world, base, want);
        if (!spot) continue;
        // Stop only on a placement that actually HAPPENED. place_building has
        // its own rejections that ai_build_spot cannot see -- it re-checks
        // affordability and footprint_clear (a unit may have wandered onto the
        // spot in between), and for team 0 it additionally requires the
        // footprint to be explored. Returning unconditionally meant one
        // candidate that found a spot but failed to place jammed the entire
        // build order behind it on EVERY tick, forever: measured a team sitting
        // at 8 pop with 1500 banked wood, zero houses and zero age-qualifying
        // buildings for a whole match, re-picking the same doomed spot 20 times
        // a second. Same failure the ordered-candidate loop was introduced to
        // fix, one layer further down.
        if (world.place_building(want, team, spot->first, spot->second).valid()) {
            return; // one successful placement per call, same as before
        }
    }
}

// Two-pass placement. Pass 1 insists every new building leave a one-tile lane
// around itself; pass 2 (only reached when the first found nowhere on the whole
// map sweep) drops that and allows the old flush placement, so a base that
// genuinely has no room left is never stopped from building at all.
//
// Why: footprint_clear only rejects an OVERLAP, so the AI would weld farms,
// houses and military buildings edge-to-edge into one solid slab. That reads as
// "the AI keeps its base packed tight", and the real cost is that its own units
// can't get through -- gatherers walking out to a woodline, or an army trying to
// muster, path into a dead end and stall. The lane requirement fixes the cause
// (no walkable space) rather than the symptom, and as the core fills up the ring
// sweep naturally has to go further out to satisfy it, which is the "spread out
// once it gets packed" behaviour.
std::optional<std::pair<double, double>> Control::ai_build_spot(int team, World& world, EntityRef base,
                                                                 const std::string& name) {
    // CANDIDATE (ai_variant == 1), NOT promoted -- it loses its A/B.
    //
    // What it fixes, measured over 64 matches on identical seeds: buildings with
    // no walkable lane on their tightest side fall from 39.1 per team (79.8% of
    // the base) to 10.3 (21.2%). That is the "packed base, units get stuck"
    // complaint, and the fix is real. Note the MEAN building gap barely moves
    // (3.19 -> 3.29 tiles) -- a base can average a healthy gap and still contain
    // welded pockets, and it is the pocket that traps a unit, so judge this by
    // the tight-pair count, not the mean.
    //
    // What it costs: three separate 64-match head-to-heads against baseline put
    // the candidate at 40.6% (always-on), then 36.7% (this version, 95% CI
    // 26.0-49.0), with avg era 2.23 vs 2.44 and peak army 28.9 vs 33.1 both
    // times. Longer walks delay the age-up, and on these maps era is the
    // strongest single predictor of winning. Deferring the rule until the base
    // is actually packed did NOT recover it.
    //
    // So this trades self-play strength for not jamming itself -- a judgement
    // call about how the AI should FEEL to play against, which a win-rate A/B
    // cannot make. Left behind the flag until someone decides that trade.
    // Farms are exempt: they're walk-through, so no unit can ever be trapped by
    // one, and they're the building a base has most of -- spacing them out only
    // lengthens the carry walk (see footprint_clear_gap).
    //
    // The lane is also only demanded ONCE THE BASE IS ACTUALLY PACKED. Applying
    // it from the first building lost a 64-match A/B outright (40.6% win rate,
    // and both avg era and peak army down): early on there is no congestion to
    // relieve, so all the rule does is push the opening buildings further apart
    // and lengthen every walk during the exact phase where economy decides the
    // game. Congestion is a late-base problem, so this is a late-base rule.
    //
    // ---- ...and the same rule, for PRODUCTION only, under feature 34 --------
    // Feature 34 asks for a lot more barracks/academies/factories, and welding
    // ten of them into the slab described above would be a worse base than the
    // three it replaces: production buildings are exactly the ones units have to
    // walk OUT of, and a new unit that spawns into a sealed pocket never reaches
    // the muster point at all.
    //
    // Scoped deliberately narrower than variant 1, which lost its A/B by
    // spacing EVERYTHING and lengthening every villager's walk during the
    // economy phase that decides the game. Farms, houses, towers, walls and the
    // refinery are untouched here -- they keep their tight, dropoff-hugging
    // placement -- and the packed-base precondition still applies, so nothing
    // changes at all until the core is actually congested.
    static const std::set<std::string> kSpacedBuildings = {"barracks", "academy", "factory",
                                                           "airbase", "fortress"};
    bool space_it = (teams[team].ai_variant == 1 && name != "farm") ||
                    (variant_has(teams[team].ai_variant, 34) && teams[team].ai_map_derive &&
                     kSpacedBuildings.count(name) != 0);
    if (space_it) {
        Building* hb = base.valid() ? world.get_building(base) : nullptr;
        int near_home = 0;
        if (hb) {
            for (auto ref : world.active_buildings) {
                Building* nb = world.get_building(ref);
                if (!nb || !nb->common.alive || nb->common.team != team || !nb->solid) continue;
                if (std::hypot(nb->common.x - hb->common.x, nb->common.y - hb->common.y) <= 8.0 * TILE)
                    ++near_home;
            }
        }
        if (near_home >= kAiPackedCount) {
            if (auto spaced = ai_build_spot_gap(team, world, base, name, kAiBuildGap)) return spaced;
        }
    }
    return ai_build_spot_gap(team, world, base, name, 0.0);
}

std::optional<std::pair<double, double>> Control::ai_build_spot_gap(int team, World& world, EntityRef base,
                                                                    const std::string& name, double gap) {
    Building* b = world.get_building(base);
    if (!b) return std::nullopt;
    if (name == "farm") {
        // Place farms flush against a dropoff for gathering efficiency -- a
        // villager working the farm then has the shortest possible walk back
        // (the base and houses are dropoffs). Preference order: pack the base's
        // own surroundings FIRST (probing a few rings out so many farms fit
        // around it), and only once those are full spill over to ringing
        // houses; falls through to the generic placement below if all are taken.
        std::vector<Building*> bases_a, houses_a;
        for (auto ref : world.active_buildings) {
            Building* a = world.get_building(ref);
            if (!a || !a->common.alive || a->common.team != team) continue;
            if (a->name == "base") bases_a.push_back(a);
            else if (a->name == "house") houses_a.push_back(a);
        }
        for (std::vector<Building*>* group : {&bases_a, &houses_a}) {
            for (Building* a : *group) {
                double edge = std::max(a->foot_w, a->foot_h) * 0.5;
                for (int ring = 1; ring <= 3; ++ring) {
                    double off = edge + ring * TILE;
                    for (int k = 0; k < 8; ++k) {
                        double ang = k * M_PI / 4.0;
                        auto p = world.snap(name, a->common.x + std::cos(ang) * off,
                                            a->common.y + std::sin(ang) * off);
                        if (world.footprint_clear_gap(name, p.first, p.second, gap)) return p;
                    }
                }
            }
        }
        // Cramped home: sweep FURTHER out around the base (up to ~16 tiles, 12
        // directions) so a food-starved base on a tight spawn can always plant a
        // farm somewhere rather than giving up when its immediate ring is full.
        for (int ring = 3; ring <= 16; ++ring) {
            for (int k = 0; k < 12; ++k) {
                double ang = k * 6.28318 / 12.0;
                auto p = world.snap(name, b->common.x + std::cos(ang) * ring * TILE,
                                    b->common.y + std::sin(ang) * ring * TILE);
                if (world.footprint_clear_gap(name, p.first, p.second, gap)) return p;
            }
        }
    }
    if (name == "shipyard") {
        std::optional<std::pair<double, double>> best;
        for (int r = 3; r < 26; ++r) {
            for (int k = 0; k < 24; ++k) { // 24 rays around the circle -- angle resolution, not tile-size-related
                double a = k * M_PI / 12.0;
                auto p = world.snap(name, b->common.x + std::cos(a) * r * TILE,
                                    b->common.y + std::sin(a) * r * TILE);
                if (!world.footprint_clear_gap(name, p.first, p.second, gap)) continue;
                int tx = static_cast<int>(std::floor(p.first / TILE));
                int ty = static_cast<int>(std::floor(p.second / TILE));
                bool near_land = false;
                for (int dx : {-2, -1, 1, 2}) {
                    for (int dy : {-2, -1, 1, 2}) {
                        int nx = tx + dx, ny = ty + dy;
                        if (nx >= 0 && nx < world.cols && ny >= 0 && ny < world.rows &&
                            world.terrain[nx][ny] != WATER) {
                            near_land = true;
                        }
                    }
                }
                if (near_land) return p;
                if (!best) best = p;
            }
        }
        return best;
    }
    if (name == "house") {
        // Houses are food+wood dropoffs, so place them to serve BOTH: prefer a
        // spot by the nearby berry vein (fast food dropoff for the opening
        // economy), and among the ring around it favour the spot closest to a
        // woodline so one house covers berries AND wood. Once the berries near
        // home are gone, fall back to the woodline, then the generic ring.
        //
        // SPREAD. Every term above is deterministic, so consecutive houses all
        // resolved to the same anchor and the same best-of-ring answer, each one
        // landing in the last one's footprint shadow -- the reported "five
        // houses all right next to each other", which is worthless for
        // gathering since a second dropoff two tiles from the first saves nobody
        // any walking. Candidate spots within kHouseGap of an existing house are
        // now rejected, and an anchor that is already served by a house is
        // skipped entirely so the next house goes to the NEXT resource cluster.
        const double kHouseGap = 5.0 * TILE;
        std::vector<std::pair<double, double>> houses;
        for (auto ref : world.active_buildings) {
            Building* h = world.get_building(ref);
            if (h && h->common.alive && h->common.team == team && h->name == "house")
                houses.push_back({h->common.x, h->common.y});
        }
        auto clear_of_houses = [&](double x, double y, double gap) {
            for (auto& [hx, hy] : houses)
                if (std::hypot(x - hx, y - hy) < gap) return false;
            return true;
        };
        double map_diag = std::hypot(world.px_w, world.px_h);
        // Pick the nearest resource of this type that does NOT already have a
        // house sitting on it -- that is what "spread to another resource" means
        // in practice: each new house claims an unserved patch.
        auto unserved = [&](double radius, auto&& pred) {
            return world.nearest(b->common.x, b->common.y, radius, [&](EntityRef r, EntityCommon& c) {
                return pred(r) && clear_of_houses(c.x, c.y, 6.0 * TILE);
            });
        };
        EntityRef wood = unserved(map_diag, [&](EntityRef r) {
            Resource* rr = world.get_resource(r);
            return rr && rr->common.alive && rr->res.rtype == 1; // WOOD
        });
        EntityCommon* wc = world.common(wood);
        EntityRef berry = unserved(15.0 * TILE, [&](EntityRef r) {
            Resource* rr = world.get_resource(r);
            return rr && rr->common.alive && rr->name == "berry";
        });
        if (EntityCommon* bc = world.common(berry)) {
            std::optional<std::pair<double, double>> best;
            double bestd = 1e18;
            for (int ring : {2, 3, 4}) {
                for (int k = 0; k < 8; ++k) {
                    double a = k * M_PI / 4.0;
                    auto p = world.snap(name, bc->x + std::cos(a) * ring * TILE,
                                        bc->y + std::sin(a) * ring * TILE);
                    if (!world.footprint_clear_gap(name, p.first, p.second, gap)) continue;
                    if (!clear_of_houses(p.first, p.second, kHouseGap)) continue;
                    double d = wc ? std::hypot(p.first - wc->x, p.second - wc->y) : 0.0;
                    if (d < bestd) { bestd = d; best = p; }
                }
            }
            if (best) return *best;
        }
        if (wc) {
            for (int ring : {2, 3, 4}) {
                for (int k = 0; k < 8; ++k) {
                    double a = k * M_PI / 4.0;
                    auto p = world.snap(name, wc->x + std::cos(a) * ring * TILE,
                                        wc->y + std::sin(a) * ring * TILE);
                    if (world.footprint_clear_gap(name, p.first, p.second, gap) &&
                        clear_of_houses(p.first, p.second, kHouseGap))
                        return p;
                }
            }
        }
        // Generic ring, still honouring the spacing. Falls through to the
        // unspaced sweep below only if a packed base leaves nowhere else.
        for (int ring = 3; ring <= 24; ++ring) {
            for (int k = 0; k < 16; ++k) {
                double a = k * M_PI / 8.0;
                auto p = world.snap(name, b->common.x + std::cos(a) * ring * 48,
                                    b->common.y + std::sin(a) * ring * 48);
                if (world.footprint_clear_gap(name, p.first, p.second, gap) &&
                    clear_of_houses(p.first, p.second, kHouseGap))
                    return p;
            }
        }
    }
    if (name == "base") {
        // ---- expansions go TO the resources, not next to the old town ------
        // The generic ring below starts a few tiles further out for a base and
        // is otherwise blind, so an expansion landed wherever there happened to
        // be a gap -- usually just outside the existing base, sharing the same
        // exhausted woodline. That is a second town centre, not an expansion.
        //
        // Anchor it on the richest patch of resources that is genuinely AWAY
        // from everything the team already owns: villagers trained there walk
        // to fresh wood/ore instead of back across the map, and the base itself
        // is their dropoff. Falls through to the generic sweep if the map has
        // nowhere that qualifies (a small or picked-clean map), so this can
        // never block an expansion the build order has decided it wants.
        constexpr double kMinFromOwnBase = 20.0 * TILE; // far enough to be new ground
        constexpr double kMaxFromHome = 60.0 * TILE;    // near enough to defend/reach
        constexpr double kClusterR = 8.0 * TILE;        // what counts as "the same patch"
        std::vector<std::pair<double, double>> own_bases;
        for (auto ref : world.active_buildings) {
            Building* ob = world.get_building(ref);
            if (ob && ob->common.alive && ob->common.team == team && ob->name == "base")
                own_bases.push_back({ob->common.x, ob->common.y});
        }
        EntityRef anchor = kNullRef;
        double anchor_score = 0.0;
        for (auto rref : world.active_resources) {
            Resource* r = world.get_resource(rref);
            if (!r || !r->common.alive || r->name == "fish") continue;
            // Wood is what an expansion is usually for, but ore and oil are
            // worth just as much once the near patches are gone -- weight wood
            // slightly ahead rather than excluding the others.
            double type_w = (r->res.rtype == 1) ? 1.25 : 1.0;
            bool too_close = false;
            for (auto& [bx2, by2] : own_bases) {
                if (std::hypot(r->common.x - bx2, r->common.y - by2) < kMinFromOwnBase) {
                    too_close = true;
                    break;
                }
            }
            if (too_close) continue;
            if (std::hypot(r->common.x - b->common.x, r->common.y - b->common.y) > kMaxFromHome) continue;
            // How rich is this patch? Count live neighbours of any gatherable
            // type -- a lone tree is not worth a 275-wood town centre.
            int near_count = 0;
            for (auto oref : world.grid.query(r->common.x, r->common.y, kClusterR)) {
                Resource* o = world.get_resource(oref);
                if (o && o->common.alive && o->name != "fish") ++near_count;
            }
            double score = near_count * type_w;
            if (score > anchor_score) {
                anchor_score = score;
                anchor = rref;
            }
        }
        if (EntityCommon* ac = anchor.valid() ? world.common(anchor) : nullptr) {
            // Sit a few tiles off the patch itself so the base does not try to
            // occupy the very tiles the villagers need to stand on.
            for (int ring = 3; ring <= 10; ++ring) {
                for (int k = 0; k < 12; ++k) {
                    double a = k * M_PI / 6.0;
                    auto p = world.snap(name, ac->x + std::cos(a) * ring * TILE,
                                        ac->y + std::sin(a) * ring * TILE);
                    if (world.footprint_clear_gap(name, p.first, p.second, gap)) return p;
                }
            }
        }
    }
    if (name == "refinery") {
        // The refinery is the ONLY dropoff for oil and iron besides the base
        // (World::nearest_dropoff: a house refuses carry types 2 and 3), so its
        // entire purpose is to shorten the haul from an ore/oil patch. It was
        // falling through to the generic ring below and getting planted a few
        // tiles from the base -- i.e. right next to the dropoff it was supposed
        // to replace -- while the miners kept walking the full distance back
        // from the oil. Anchor it on the patch the team is actually working.
        //
        // Prefer the resource type with the most villagers on it right now, so
        // the refinery follows the mining rather than a guess. Ties and the
        // no-miners case fall back to the nearest oil, then the nearest iron.
        int on[4] = {0, 0, 0, 0};
        for (auto ref : world.active_units) {
            Unit* u = world.get(ref);
            if (!u || !u->common.alive || u->common.team != team || !u->is_gatherer) continue;
            Resource* r = world.get_resource(u->gather_target);
            int rt = r ? r->res.rtype : u->gather_rtype;
            if (rt >= 0 && rt < 4) ++on[rt];
        }
        double map_diag = std::hypot(world.px_w, world.px_h);
        std::vector<int> order = (on[3] > on[2]) ? std::vector<int>{3, 2} : std::vector<int>{2, 3};
        for (int rtype : order) {
            EntityRef ore = world.nearest(b->common.x, b->common.y, map_diag,
                                          [&](EntityRef r, EntityCommon&) {
                                              Resource* rr = world.get_resource(r);
                                              return rr && rr->common.alive && rr->res.rtype == rtype;
                                          });
            EntityCommon* oc = world.common(ore);
            if (!oc) continue;
            // Already covered? A refinery within ~8 tiles of this patch means the
            // haul is short and a second one there is wasted wood.
            bool covered = false;
            for (auto ref : world.active_buildings) {
                Building* rb = world.get_building(ref);
                if (rb && rb->common.alive && rb->common.team == team && rb->name == "refinery" &&
                    std::hypot(rb->common.x - oc->x, rb->common.y - oc->y) < 8.0 * TILE) {
                    covered = true;
                    break;
                }
            }
            if (covered) continue;
            for (int ring : {2, 3, 4, 5}) {
                for (int k = 0; k < 12; ++k) {
                    double a = k * M_PI / 6.0;
                    auto p = world.snap(name, oc->x + std::cos(a) * ring * TILE,
                                        oc->y + std::sin(a) * ring * TILE);
                    if (world.footprint_clear_gap(name, p.first, p.second, gap)) return p;
                }
            }
        }
    }
    // ---- CANDIDATE (feature 24): towers face the way the enemy comes ---------
    // A tower had no placement rule at all, so it fell through to the generic
    // ring sweep below -- which starts at ring 3 and takes the FIRST clear angle,
    // and the angle loop starts at k=0, i.e. due east. Every tower a team builds
    // therefore lands on the same side of its base regardless of where the enemy
    // actually is, and on half of all maps that side is the back.
    //
    // A tower is a fixed emplacement with an 8-ish tile arc; which side of the
    // base it sits on is most of its value. The attack axis is already known --
    // ai_read_enemy resolves the nearest hostile base every AI tick, and this
    // runs from ai_build in the same tick -- so aim at it, sweeping outward from
    // the axis so a second and third tower spread across the approach instead of
    // stacking. Falls through to the generic sweep if nothing on the axis is
    // clear, and does nothing at all before an enemy base has been seen.
    if (name == "tower" && variant_has(teams[team].ai_variant, 24) && teams[team].ai_map_derive) {
        const Team::EnemyRead& rd = teams[team].ai_read;
        if (rd.base_known) {
            double axis = std::atan2(rd.base_y - b->common.y, rd.base_x - b->common.x);
            for (int ring = 5; ring <= 12; ++ring) {
                for (int k = 0; k < 9; ++k) {
                    // 0, +-0.3, +-0.6, ... radians off the attack axis (~+-69 deg)
                    double spread = ((k + 1) / 2) * 0.3 * ((k % 2) ? 1.0 : -1.0);
                    double a = axis + spread;
                    auto p = world.snap(name, b->common.x + std::cos(a) * ring * TILE,
                                        b->common.y + std::sin(a) * ring * TILE);
                    if (world.footprint_clear_gap(name, p.first, p.second, gap)) return p;
                }
            }
        }
    }
    // Generic placement: spiral outward in rings, 16 angles per ring for a fine
    // sweep. The ring range runs WIDE (out to ~24 tiles) because the big
    // prerequisite buildings -- factory/university (War), base/fortress
    // (Scientific) -- are 64x64 and a mature base is packed solid with farms,
    // houses, walls, barracks, academy, market, etc.; a short ring-3..7 search
    // finds no clear 2x2 spot and the age-unlocking building silently fails to
    // place (measured: era-1 teams stuck with only 1 of 2 War-prereqs). A base
    // in particular wants to sit further out anyway (expansion toward more room
    // / resources), so search it from further out first.
    int ring_lo = (name == "base") ? 6 : 3;
    // Congestion drift: once the home cluster is dense, start the sweep further
    // out rather than hunting for the last crack in the middle of it. Without
    // this the search always begins at ring 3 and every building that CAN still
    // squeeze into the core does, so the core just keeps getting denser and the
    // lanes through it keep getting scarcer. Counting buildings within 8 tiles
    // of the town centre, each group of 4 pushes the start one ring out, capped
    // so a mature base still builds within reach of its own defences.
    // Part of the same unpromoted candidate as the build gap above, so it is
    // flagged together with it rather than shipping on its own.
    if (teams[team].ai_variant == 1) {
        int near_home = 0;
        for (auto ref : world.active_buildings) {
            Building* nb = world.get_building(ref);
            if (!nb || !nb->common.alive || nb->common.team != team) continue;
            if (std::hypot(nb->common.x - b->common.x, nb->common.y - b->common.y) <= 8.0 * TILE)
                ++near_home;
        }
        ring_lo += std::min(6, near_home / 4);
    }
    for (int ring = ring_lo; ring <= 24; ++ring) {
        for (int k = 0; k < 16; ++k) {
            double a = k * M_PI / 8.0;
            auto p = world.snap(name, b->common.x + std::cos(a) * ring * 48, b->common.y + std::sin(a) * ring * 48);
            if (world.footprint_clear_gap(name, p.first, p.second, gap)) return p;
        }
    }
    return std::nullopt;
}

void Control::ai_train(int team, World& world, const std::vector<EntityRef>& team_buildings,
                       const std::vector<EntityRef>& force) {
    Team& td = teams[team];
    int era = td.era;
    const std::string& strat = td.strategy;
    // Defensive floor: always keep at least this many military, exempt from
    // every hold below, so the team is never left defenceless. With the tempo
    // doctrine (see ai_economy) production no longer stops at all, so this now
    // only matters for the handful of units made before the age hold can bind.
    int mil_floor = 4;
    // Retaliation: a fixed 2-5 unit floor is not a defence, it's a speed bump --
    // whatever the attacker brought walks through it and the AI goes right back
    // to booming. While ai_retaliate holds, the floor is instead SIZED TO THE
    // ATTACK (every enemy soldier near home, plus two), so everything below
    // treats production as un-throttled until the team can actually answer.
    // Capped so a doomed base doesn't try to out-produce an entire army, and it
    // clears itself the moment the shooting stops (ai_under_fire runs out) or
    // the army reaches kDefencelessArmy. The cap is 10, not 20: this floor is
    // exempt from the food throttle, so a big number here is an "all food into
    // units" instruction that the age-up never recovers from.
    if (td.ai_retaliate) mil_floor = std::max(mil_floor, std::min(10, td.ai_threat + 2));
    bool below_floor = static_cast<int>(force.size()) < mil_floor;
    // Train a military unit. Under the tempo doctrine (see ai_economy) there is
    // no income fraction and no accrued budget any more: if the team can afford
    // the unit, it buys the unit. World::enqueue does the affording, so this is
    // now almost a straight passthrough.
    //
    // The single hold is the age-up (Team::ai_banking, set only while the next
    // age is genuinely within reach). It is checked PER RESOURCE against what
    // that specific age actually costs -- the Victorian->Industrial step is paid
    // in food, Scientific wants 800 oil -- so a unit is only ever refused when
    // buying it would eat into the particular resource the pending age is short
    // of. A tank cannot delay a food age-up and is never held for one.
    //
    // Below the defensive floor nothing is held at all: being unable to defend
    // the base outranks aging up.
    std::unordered_map<std::string, double> age_cost;
    if (td.era < 3) {
        for (auto& [k, v] : cost_of(AGE_ITEMS[td.era], team)) age_cost[k] = v;
    }
    auto train_mil = [&](EntityRef bref, const std::string& unit) {
        // Villager reserve (Team::ai_vil_reserve) -- same shape as the age hold
        // below, one resource instead of several: don't buy a unit with the food
        // the town centre needs for its next villager. Below the defensive floor
        // nothing is held, for the same reason nothing is held for an age-up.
        if (td.ai_vil_reserve > 0.0 && !below_floor) {
            auto c = cost_of(unit, team);
            auto food_cost = c.find("food");
            if (food_cost != c.end() && food_cost->second > 0.0) {
                auto have = td.res.find("food");
                double stock = have == td.res.end() ? 0.0 : have->second;
                if (stock - food_cost->second < td.ai_vil_reserve) return;
            }
        }
        if (td.ai_banking && !below_floor && !age_cost.empty()) {
            for (auto& [k, v] : cost_of(unit, team)) {
                auto need = age_cost.find(k);
                if (need == age_cost.end()) continue;
                auto have = td.res.find(k);
                double stock = have == td.res.end() ? 0.0 : have->second;
                if (stock - v < need->second) return; // would eat the age-up
            }
        }
        world.enqueue(bref, unit);
    };
    // Floating food -> convert the surplus into OIL-FREE academy units (cavalry
    // 80 food / swordsman 50 food, no oil at all). On oil-poor land maps a food-
    // rich boom otherwise stalls on the oil-gated muscateer (40 food + 20 oil);
    // this lets it field an army from the resource it has in surplus. Widens the
    // academy queue so the conversion runs fast when food is clearly spare.
    // ---- PROMOTED (was ai_variant == 6): counter-composition ----------------
    // The AI has never had ANY notion of what it is fighting. It builds the
    // same infantry line regardless of the enemy's army, and in this game's
    // damage model that is close to fatal, because damage is
    //     max(attack - (melee ? target.armor : target.pierce), 1)
    // with no rock-paper-scissors table to soften it. A rifleman (attack 6)
    // against a heavy tank (pierce 6, 300 hp) deals literally 1 damage: 300
    // hits to kill one. A swordsman2 (attack 8) against any tank (armor 7-8)
    // is the same story. Artillery (attack 20) deals 14 to that same tank.
    // Over the 600-match arena the AI produced 60,581 riflemen and 199 heavy
    // tanks, so the counter it needed was one it almost never built.
    //
    // So: profile what the enemy actually fields, and pick the unit whose
    // EFFECTIVE damage per resource spent is highest against that profile,
    // using the sim's own damage formula rather than a hand-written table that
    // could drift away from it.
    //
    // ---- CANDIDATE (feature 20): the enemy is a DISTRIBUTION, not a mean -----
    // The scorer below averages his armour and pierce and scores against the
    // average. In a max(attack - armour, 1) model that is the wrong statistic,
    // and wrong in the direction that matters.
    //
    // Take the exact case the comment above is about. Thirty riflemen (pierce 1)
    // and six heavy tanks (pierce 6, 300 hp): mean pierce 1.8, so a rifleman
    // "effectively" deals 4.2 and looks like a perfectly good answer. Against the
    // actual army it deals 5 to thirty of them and ONE to the other six, and one
    // damage against 300 hit points is not a worse trade, it is no trade -- 300
    // shots, during which the tank kills everything it can see. Averaging is what
    // makes a blind spot look like a small penalty.
    //
    // So keep him as a histogram of (armour, pierce, air) classes and score
    // against every class he actually fields, with anything under 2 effective
    // damage contributing zero rather than a token 1. That also replaces the two
    // crude air_frac multipliers below with the real engagement filters, one
    // class at a time: melee cannot reach aircraft, AA mounts fire only at them.
    //
    // Distinct classes are few (armour/pierce are small integers and a roster is
    // ~40 units), so the linear search is cheaper than hashing and this stays a
    // once-per-call cost, like the profile it replaces.
    struct EnemyClass {
        int armor = 0, pierce = 0;
        bool is_air = false;
        int count = 0;
    };
    std::vector<EnemyClass> e_classes;
    const bool dist_counter = variant_has(td.ai_variant, 20);
    double e_armor = 0.0, e_pierce = 0.0;
    int e_units = 0, e_air = 0;
    for (auto ref : world.active_units) {
        Unit* eu = world.get(ref);
        if (!eu || !eu->common.alive || eu->common.team < 0 || allied(eu->common.team, team)) continue;
        if (eu->is_gatherer) continue;
        e_armor += eu->armor;
        e_pierce += eu->pierce;
        ++e_units;
        if (eu->common.is_air) ++e_air;
        if (dist_counter) {
            bool known = false;
            for (auto& c : e_classes) {
                if (c.armor == eu->armor && c.pierce == eu->pierce && c.is_air == eu->common.is_air) {
                    ++c.count;
                    known = true;
                    break;
                }
            }
            if (!known) e_classes.push_back(EnemyClass{eu->armor, eu->pierce, eu->common.is_air, 1});
        }
    }
    const double mean_armor = e_units ? e_armor / e_units : 0.0;
    const double mean_pierce = e_units ? e_pierce / e_units : 0.0;
    const double air_frac = e_units ? static_cast<double>(e_air) / e_units : 0.0;
    // The toughest thing he fields IN NUMBERS -- the class the production choice
    // has to keep an answer to. A single captured tank is not a composition
    // problem; a sixth of his army being tanks is. Below kHardShare it is left to
    // the per-class scoring above, which already prices it correctly.
    constexpr double kHardShare = 0.15;
    int hard_armor = 0, hard_pierce = 0;
    bool hard_air = false, have_hard = false;
    if (dist_counter && e_units > 0) {
        double toughest = 0.0;
        for (const auto& c : e_classes) {
            if (static_cast<double>(c.count) / e_units < kHardShare) continue;
            double h = std::max(c.armor, c.pierce);
            if (h > toughest) {
                toughest = h;
                hard_armor = c.armor;
                hard_pierce = c.pierce;
                hard_air = c.is_air;
                have_hard = true;
            }
        }
    }

    // ---- anti-air, on sight of aircraft ------------------------------------
    // The counter-composition scorer already knows AA exists, but it weights it
    // by the enemy's AIR FRACTION, so a player with three bombers among thirty
    // ground units barely moves the score and the AA never gets built in time.
    // Aircraft are not a fraction of a threat, they are a different threat: the
    // ground army cannot shoot back at them at all (unit_behavior's melee/air
    // filter, and every non-AA building only fires at ground -- see
    // building_behavior's `b.is_aa != e->is_air`).
    //
    // So: as soon as ANY enemy aircraft is seen, put a couple of AA mounts in
    // production, and scale to the size of the air force -- but stop at four.
    // They are 60 oil + 80 iron each and can only shoot at aircraft, so past a
    // handful they are dead weight against the ground army that is also coming.
    // Being units rather than towers, they walk with the offensive push as well
    // as covering home, which is the "accompany the riflemen or defend, or
    // both" the player asked for -- no extra plumbing needed, they simply join
    // `force` like anything else.
    int own_aa = 0;
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (u && u->common.alive && u->common.team == team && u->is_aa) ++own_aa;
    }
    constexpr int kMaxAA = 4;
    int want_aa = e_air > 0 ? std::clamp(e_air + 1, 2, kMaxAA) : 0;

    // ---- enemy fortresses: a siege problem, not a target ---------------------
    // A fortress is 2000 hp, attack 5, RANGE 8 tiles. Every mainline unit this
    // AI builds is inside that: muscateer 3, rifleman 4, cavalry melee. Sending
    // them at it is feeding it. Three things can hurt it without being hit
    // back -- artillery (range 10), the ballistic missile (range 16), and ANY
    // aircraft, because a fortress is not an AA structure and cannot fire at air
    // at all. So seeing one is a production instruction: build the siege that
    // answers it. The targeting half is in ai_siege.
    bool enemy_fortress = false;
    for (auto ref : world.active_buildings) {
        Building* eb = world.get_building(ref);
        if (eb && eb->common.alive && eb->name == "fortress" && eb->common.team >= 0 &&
            !allied(eb->common.team, team)) {
            enemy_fortress = true;
            break;
        }
    }
    // Damage-per-second per unit of resource, against the CURRENT enemy mix.
    // Dividing by cost is what keeps this from always answering "artillery":
    // a cheap unit that still hurts the enemy beats an expensive one that hurts
    // it slightly more. Melee is scaled down by the enemy's air fraction
    // because melee units cannot reach aircraft at all (unit_behavior.cpp's
    // `!(melee && c.is_air)` target filter), so against an air-heavy enemy a
    // melee unit's real contribution is only against the part it can touch.
    //
    // Both the catalog handle and the per-unit result are hoisted/cached: the
    // enemy profile is fixed for the whole call, so a given unit name always
    // scores the same here, and without the cache this ran a JSON object lookup
    // plus a cost_of() map build for every candidate unit at EVERY production
    // building on every AI tick. That showed up as a ~5x slowdown in wall-clock
    // simulation speed during the A/B, which is cost the real game pays too, not
    // just the harness.
    const auto& unit_cat = data_.catalog().at("units");
    std::unordered_map<std::string, double> score_cache;
    // What fraction of the enemy can a given unit actually SHOOT? Raw attack is
    // meaningless against targets a unit may not engage, and two whole classes
    // here are restricted:
    //   * melee cannot reach aircraft at all (unit_behavior.cpp's
    //     `!(melee && c.is_air)` filter);
    //   * AA mounts fire ONLY at aircraft (Unit::is_aa, set for these three names
    //     in world.cpp; update_combat drops any non-air target for them).
    // Without the AA term the flak gun's attack 50 and the AA gun's 50 made them
    // the highest-scoring "answer" to an enemy that had no aircraft whatsoever --
    // a 6-match check built 49 AA guns that could not fire a shot all game.
    // Hoisted out of the scorer so the hard-class test below applies the same
    // two rules rather than a second copy of them.
    static const std::set<std::string> kAaMounts = {"aa gun", "flak", "aircraft carrier2"};
    auto counter_score = [&](const std::string& name) -> double {
        auto cached = score_cache.find(name);
        if (cached != score_cache.end()) return cached->second;
        double score = -1.0;
        if (unit_cat.contains(name)) {
            const auto& s = unit_cat.at(name);
            double atk = s.value("attack", 0.0);
            if (atk > 0.0) { // transports, fishing boats: not an answer to anything
                bool melee = s.value("melee", false);
                bool aa = kAaMounts.count(name) != 0;
                // catalog reload is a GML frame count
                double reload = std::max(1.0, s.value("reload", 30.0)) / 60.0;
                double cost = 0.0;
                for (auto& [k, v] : cost_of(name, team)) cost += v;
                cost = std::max(20.0, cost);
                if (dist_counter && e_units > 0) {
                    // CANDIDATE (feature 20): expected damage against the army he
                    // actually has, class by class, normalised back to a
                    // per-enemy-unit figure so it stays comparable to the mean
                    // form below. A class this unit may not engage contributes
                    // nothing; so does one it can only scratch.
                    double dmg = 0.0;
                    for (const auto& c : e_classes) {
                        if (melee && c.is_air) continue;
                        if (aa && !c.is_air) continue;
                        double eff = atk - (melee ? c.armor : c.pierce);
                        if (eff < 2.0) continue; // 1 damage is not a trade at all
                        dmg += eff * c.count;
                    }
                    score = dmg / e_units / reload / cost;
                } else {
                    double eff = std::max(atk - (melee ? mean_armor : mean_pierce), 1.0);
                    score = eff / reload / cost;
                    if (aa) score *= air_frac;
                    else if (melee) score *= (1.0 - air_frac);
                }
            }
        }
        score_cache.emplace(name, score);
        return score;
    };
    // CANDIDATE (feature 20): can this unit hurt the hard class at all? Same two
    // engagement filters and the same "under 2 damage is nothing" rule as the
    // scorer, asked about one class instead of averaged over all of them.
    auto hurts_hard = [&](const std::string& name) -> bool {
        if (!have_hard || !unit_cat.contains(name)) return false;
        const auto& s = unit_cat.at(name);
        double atk = s.value("attack", 0.0);
        if (atk <= 0.0) return false;
        bool melee = s.value("melee", false);
        bool aa = kAaMounts.count(name) != 0;
        if (melee && hard_air) return false;
        if (aa && !hard_air) return false;
        return atk - (melee ? hard_armor : hard_pierce) >= 2.0;
    };
    // Best available answer at this building, or "" to leave the existing
    // hand-written choice alone (no enemy seen yet -> nothing to counter, so
    // the tuned default opening is still the right call).
    auto best_counter = [&](const std::vector<std::string>& units) -> std::string {
        if (!variant_has(td.ai_variant, 6) || e_units == 0) return std::string();
        std::string best;
        double best_s = 0.0;
        std::string best_answer; // best among those that can hurt the hard class
        double best_answer_s = 0.0;
        for (const auto& u : units) {
            double s = counter_score(u);
            if (s > best_s) { best_s = s; best = u; }
            if (s > 0.0 && s > best_answer_s && hurts_hard(u)) { best_answer_s = s; best_answer = u; }
        }
        // ---- CANDIDATE (feature 20): never field an army with no answer ------
        // Efficiency and sufficiency are different questions and this scorer only
        // asks the first. Against 30 riflemen and 6 heavy tanks, more riflemen is
        // genuinely the most damage per resource -- and it still loses, because
        // the six tanks are unkillable by everything bought with that reasoning
        // and go on killing until the match ends.
        //
        // So when the efficient pick has no answer to the hard class, buy the
        // best thing at this building that does, even though it prices worse.
        // Deliberately not a quota or a ratio: production alternates naturally as
        // the enemy composition shifts under it (buy the answer, his tank share
        // stops being the problem, the efficient pick comes back), so the fleet
        // self-balances without a target mix to tune. If nothing here can touch
        // it -- a barracks against heavy armour -- this changes nothing and the
        // factory/fortress branches are where the answer has to come from.
        if (have_hard && !best_answer.empty() && !hurts_hard(best)) return best_answer;
        return best;
    };
    // ---- BUILD THE WHOLE ROSTER (feature 35) --------------------------------
    // Every picker above answers "what is the single best unit here", and then
    // every building of that type queues it, every tick, for the rest of the
    // match. That is why the arena produced 60,581 riflemen and 199 heavy tanks
    // off rosters carrying a dozen units each, and why a civ's unique unit --
    // the entire point of picking that civ -- essentially never appeared.
    //
    // A single answer is also just wrong here. The best unit per resource is a
    // ratio, and this game's damage model (max(attack - armour, 1)) means the
    // cheap efficient unit and the expensive one that can actually punch through
    // are answers to DIFFERENT halves of an enemy army; an army of only the
    // first has no answer to armour, an army of only the second is tiny.
    //
    // So: rank the roster exactly as before, keep everything still competitive,
    // and rotate across that shortlist. Two things are force-included whatever
    // they price at -- the civ's UNIQUE units (a Tiger is why you picked
    // Germany, and it is priced like a Tiger) and whatever `must` names, which
    // the factory uses for the heavy tank slot.
    //
    // The rotation is (building id + units produced so far), so it is
    // deterministic, it advances as production actually completes rather than on
    // a timer, and two factories standing side by side build different things in
    // the same tick instead of the same thing twice.
    constexpr double kRosterBand = 0.35; // keep anything within ~a third of the best
    auto roster_pick = [&](const std::vector<std::string>& units, const Building& bl,
                           const std::string& must) -> std::string {
        if (!variant_has(td.ai_variant, 35) || units.empty()) return std::string();
        std::vector<std::pair<std::string, double>> ranked;
        double top = 0.0;
        for (const auto& u : units) {
            if (kAaMounts.count(u)) continue; // anti-air has its own rule (want_aa)
            double s = counter_score(u);
            if (s <= 0.0) continue; // cannot hurt anything he currently fields
            ranked.push_back({u, s});
            top = std::max(top, s);
        }
        if (ranked.empty()) return std::string();
        std::vector<std::string> shortlist;
        for (const auto& [u, s] : ranked)
            if (s >= top * kRosterBand || UNIQUE_UNITS.count(u) || u == must) shortlist.push_back(u);
        if (shortlist.empty()) return std::string();
        std::sort(shortlist.begin(), shortlist.end()); // stable order for the rotation
        uint32_t rot = bl.common.id + static_cast<uint32_t>(td.military_units_created);
        return shortlist[rot % shortlist.size()];
    };

    // Surplus food -> oil-free academy units. The boom ramp used to be excluded
    // outright, which is the other half of the "AI sits on a stockpile it never
    // spends" report: an era-0 economic plan with 900 banked food had BOTH this
    // and train_mil's hard stop switched off, so the pile just grew. It now
    // applies during the boom too, at a higher bar (600 vs 400) -- past the ~500
    // age cost, so converting the excess can't rob the age-up the way spending
    // from 400 would.
    bool floating_food = td.res["food"] > (td.ai_boom_phase ? 600.0 : 400.0);
    // Per-building queue depth. Normally deliberately shallow (see the barracks
    // comment below) so pre-paid production doesn't freeze resources the build
    // order needs. Under retaliation one deeper: liquidity stops mattering when
    // the base is being taken apart, and the extra slot is what keeps the
    // building producing across the gaps between AI decision ticks.
    //
    // ---- ...and DEEPER while the bank is deep (feature 34) ------------------
    // The shallow queue exists to keep resources liquid for the build order. A
    // team sitting on four figures of everything has no liquidity problem -- it
    // has the opposite one -- and a queue that empties between AI decision ticks
    // is a production building standing idle for up to a second on Normal, every
    // cycle, across every building it owns. Same threshold as ai_build's `flush`
    // (recomputed rather than passed, so the two passes cannot drift apart on
    // resources that change between them).
    bool flush_train = variant_has(td.ai_variant, 34) && td.ai_map_derive &&
                       (td.res["food"] > 1500.0 || td.res["wood"] > 1500.0 ||
                        td.res["iron"] > 1200.0 || td.res["oil"] > 1200.0);
    // ---- NEVER QUEUE MORE THAN ONE ON DECK (2026-08-15) ---------------------
    // Two: the one being trained, plus one waiting. Nothing deeper, ever --
    // including under retaliation and including a full bank, both of which used
    // to raise it.
    //
    // Depth buys a human player something real: they cannot be at the barracks
    // the instant a unit pops, so a queue keeps the building busy while their
    // attention is elsewhere. The AI has no such gap. It re-evaluates every
    // building on its own cadence (0.05s on Hard, 1s on Normal) and requeues as
    // soon as a slot frees, so the building is never idle for longer than one
    // decision tick whatever the depth is.
    //
    // The cost, though, is real and compounding: World::enqueue PRE-PAYS. Every
    // queued item's cost is deducted the moment it is queued, so a deep queue
    // across six production buildings is hundreds of food and oil frozen in
    // buildings instead of sitting in the bank -- which is exactly the resource
    // the age-up reserve above is trying to accumulate. Deep queues and "why
    // won't it advance" are the same problem seen from two ends.
    (void)flush_train;
    const size_t mil_qcap = 2;
    for (auto ref : team_buildings) {
        Building* b = world.get_building(ref);
        if (!b) continue;
        if (b->name == "barracks") {
            // Mass muscateers -- the whole point of the rush. Fall back to
            // whatever the barracks can build if muscateer has been upgraded
            // out (available_units resolves the upgrade chain / civ swaps).
            // Keep the per-barracks queue shallow so resources stay liquid
            // for farms/houses/age-ups rather than sunk into a huge pre-paid
            // backlog. The reserved throttle (train_mil) governs the rate.
            if (b->queue.size() >= mil_qcap) continue;
            auto units = available_units("barracks", team);
            std::string inf = "muscateer";
            if (std::find(units.begin(), units.end(), inf) == units.end() && !units.empty()) {
                inf = units.front();
            }
            // Mix in siege when there's spare oil+iron to spend: the barracks
            // now trains the field cannon (artillery1) / artillery, so the AI
            // keeps some bombardment behind its infantry wave. Self-regulates --
            // building one drains the surplus, so it reverts to infantry.
            std::string siege;
            for (const char* s : {"artillery", "artillery1"}) {
                if (std::find(units.begin(), units.end(), s) != units.end()) { siege = s; break; }
            }
            // ---- what to train: three layers, outermost wins -----------------
            //   1. the PICK -- feature 35's roster rotation, else feature 6's
            //      single best counter, else the hand-written default above;
            //   2. the surplus siege buy;
            //   3. a hostile fortress (just below), which beats everything.
            //
            // Layers 1 and 2 used to run the other way round, and had done since
            // feature 6 was promoted to baseline: best_counter returns non-empty
            // whenever feature 6 is on AND any hostile military unit exists, and
            // every team starts with a cavalry, so that was true from tick zero
            // of every match. The surplus-siege mix was therefore DEAD CODE and
            // has never once fired in a real game.
            //
            // It is not redundant with the counter, either. It answers a
            // different question: not "what is the most damage per resource"
            // (artillery never wins that -- it is priced for what it does to
            // BUILDINGS, which the scorer does not look at) but "we are sitting
            // on 300 iron and 160 oil we have no other use for, put bombardment
            // behind the wave". A deliberate strategic rule should override the
            // efficiency default, not the other way round.
            //
            // `deliberate_first` keeps the exact legacy order when both features
            // are off, so `--candidate-variant 98` really is the old behaviour.
            bool surplus_siege = !siege.empty() && td.res["iron"] >= 300 && td.res["oil"] >= 160;
            bool deliberate_first = variant_has(td.ai_variant, 20) || variant_has(td.ai_variant, 35);
            if (!deliberate_first && surplus_siege) inf = siege; // legacy order, see above
            std::string pick = roster_pick(units, *b, std::string());
            if (pick.empty()) pick = best_counter(units); // variant 6
            if (!pick.empty()) inf = pick;
            if (deliberate_first && surplus_siege) inf = siege;
            // A hostile fortress overrides both of the above. It is the one
            // structure the mainline army genuinely cannot fight (see
            // enemy_fortress), and the barracks is where the answer is trained.
            // The surplus gate is dropped to bare affordability here: waiting
            // for 300 iron / 160 oil is what left the AI walking riflemen into
            // it instead. Only the true artillery (range 10) outranges the
            // fortress's 8 -- artillery1 is range 8 and merely trades -- so this
            // also makes the artillery upgrade worth having, see ai_research.
            if (enemy_fortress && !siege.empty()) inf = siege;
            train_mil(ref, inf);
        } else if (b->name == "academy") {
            // Cavalry-first mobile harassment to round out the muscateer
            // line, EXCEPT camel civs (Ottoman and whoever else CAMEL_CIVS
            // grows to cover), which lean into their own signature unit --
            // CAMEL_UNITS covers both "camel" and its "camel corps" upgrade
            // name, so this still recognizes it post-upgrade. Same banking
            // discipline as the barracks, but a DEEPER queue when food is
            // floating so a big food economy converts into cavalry/swordsman
            // fast (Plan 1: oil-free army from the food surplus).
            // The academy used to go one deeper while food was floating. Same
            // reasoning as mil_qcap above applies -- a deeper queue does not
            // make it produce faster, it just freezes the surplus it was
            // reacting to. Converting spare food into cavalry is still what
            // happens; it happens one unit at a time, at the same rate.
            if (b->queue.size() >= mil_qcap) continue;
            auto units = available_units("academy", team);
            if (units.empty()) continue;
            std::string camel;
            for (auto& unit : units) {
                if (CAMEL_UNITS.count(unit)) { camel = unit; break; }
            }
            std::string want = camel;
            if (want.empty()) {
                want = "cavalry";
                if (std::find(units.begin(), units.end(), want) == units.end()) want = units.front();
            }
            std::string apick = roster_pick(units, *b, std::string()); // feature 35
            if (apick.empty()) apick = best_counter(units);            // variant 6
            if (!apick.empty()) want = apick;
            // ---- CANDIDATE (feature 20): give the camel civs their unit back --
            // Same dead-code problem as the barracks: the counter pick overwrites
            // this unconditionally, so since feature 6 shipped an Ottoman academy
            // has trained whatever scored best and never a camel. That is a
            // CIV-IDENTITY decision rather than an efficiency one -- ai_build's
            // own comment says the academy exists "so civ identity actually shows
            // up in what the AI fields" -- and it is the whole reason CAMEL_CIVS
            // and CAMEL_UNITS exist in the first place.
            //
            // Put it back in front, with one exception: if the enemy is fielding
            // something in numbers that a camel cannot hurt, the counter's answer
            // wins. Identity is worth a little efficiency, not a lost match.
            if (variant_has(td.ai_variant, 20) && !camel.empty() && (!have_hard || hurts_hard(camel)))
                want = camel;
            train_mil(ref, want);
        } else if (b->name == "fortress") {
            // Only ever queued when ai_build's fortress_useful check found
            // something trainable here for this civ (Germany's waffen SS or
            // Ottoman's janissary/camel corps line) -- so `units` is never
            // empty in practice, but check anyway rather than assume it.
            if (b->queue.size() >= mil_qcap) continue;
            auto units = available_units("fortress", team);
            if (!units.empty()) train_mil(ref, units.back());
        } else if (b->name == "factory" && era >= 1) {
            // Siege first (artillery) once available -- the later-age punch
            // behind the muscateer wave; otherwise the best tank on hand.
            // Tanks/artillery are oil+iron heavy with ~no food cost, so they
            // slip past train_mil's food throttle -- cap the queue shallow so
            // they don't lock up oil/iron, and HOLD entirely while banking for
            // an age (otherwise the factory drains the very oil Scientific's
            // 800-oil cost needs).
            // The blanket `ai_banking` hold that used to sit here is gone: it
            // stopped the factory outright whenever an age was being saved for,
            // whatever that age was actually short of. train_mil now applies the
            // hold per resource, so a tank is only refused when it would eat
            // into the specific resource the pending age needs.
            if (b->queue.size() >= mil_qcap) continue;
            auto units = available_units("factory", team);
            // ---- FIX (feature 35): the tank slot, RESOLVED -------------------
            // The line below used to read
            //     want = td.tech.count("heavy tank upgrade") ? "heavy tank"
            //                                                : "light tank";
            // which hardcodes two catalog names and is simply wrong for any civ
            // whose heavy tier is not literally called "heavy tank". Germany's
            // tank slot substitutes to the TIGER (CIV_UNIT_SUB {2,"tank"}) and
            // upgrades on to the Tiger II, and civ_has EXCLUDES Germany from the
            // generic heavy tank outright -- so a German AI has spent every
            // match asking its factory for a unit that is not in its own roster,
            // which is why nobody has ever seen it field a Tiger.
            //
            // available_units has already done the entire resolution (upgrade
            // chain, civ substitution, chain again, then every civ/era/tech
            // gate), so the heavy slot is just the costliest non-AA thing it
            // returned. Population cost is the ranking that survives a catalog
            // edit: light tank 2, tank/heavy tank 4, Tiger/Tiger II 5.
            std::string heaviest;
            for (const auto& u : units) {
                if (kAaMounts.count(u)) continue;
                if (heaviest.empty() || pop_cost(u) > pop_cost(heaviest)) heaviest = u;
            }
            std::string want;
            if (std::find(units.begin(), units.end(), "artillery") != units.end()) want = "artillery";
            else if (variant_has(td.ai_variant, 35) && !heaviest.empty()) want = heaviest;
            else want = td.tech.count("heavy tank upgrade") ? "heavy tank" : "light tank";
            // Feature 35 rotates the whole factory roster -- light AND heavy
            // armour, plus the civ unique -- with the heavy slot force-included
            // so it can never be priced out of the shortlist by a cheaper tank.
            std::string fpick = roster_pick(units, *b, heaviest);
            if (fpick.empty()) fpick = best_counter(units); // variant 6
            if (!fpick.empty()) want = fpick;
            // ---- CANDIDATE (feature 20): the factory needs the fortress rule --
            // The barracks got one (see enemy_fortress there) and this did not,
            // which leaves a gap rather than a duplicate: once "artillery
            // upgrade" has converted the barracks' field cannon, or on a civ
            // whose barracks roster never carried one, the FACTORY is the only
            // place artillery is trained -- and artillery's range 10 is the only
            // ground answer to a fortress's range 8 (see ai_siege). Without this
            // such a team walks tanks into the arc forever, which is precisely
            // the behaviour ai_siege exists to prevent and cannot, because it can
            // only pull units out, never build the thing that should be there.
            if (variant_has(td.ai_variant, 20) && enemy_fortress &&
                std::find(units.begin(), units.end(), "artillery") != units.end())
                want = "artillery";
            // Anti-air takes the slot outright while we are short of the goal --
            // ahead of the counter-composition pick, which is a per-resource
            // efficiency score and will not react fast enough to a small air
            // force (see want_aa above). The factory is where AA is trained
            // (PRODUCTION["factory"]); flak is the upgraded form of the same
            // unit and counts toward the same goal via Unit::is_aa.
            if (own_aa < want_aa) {
                for (const char* aa : {"flak", "aa gun"}) {
                    if (std::find(units.begin(), units.end(), aa) != units.end()) {
                        want = aa;
                        ++own_aa; // this pass may visit several factories
                        break;
                    }
                }
            }
            train_mil(ref, want);
        } else if (b->name == "airbase" && era >= 1) {
            if (b->queue.size() >= mil_qcap) continue; // per-resource age hold lives in train_mil
            // Bombers were gated on `strat == "navy"`, i.e. on the map's
            // want_water flag -- so on any LAND map the airbase queued fighters
            // and nothing else, for the whole match. Measured over 320 self-play
            // games on Ostland: 530 team-instances reached War era and built
            // ZERO bombers between them, while fighters piled up with no enemy
            // aircraft to contest. Air superiority is only worth buying once
            // there is something to be superior to; what actually converts an
            // air force into a won game is bombing the enemy's buildings.
            //
            // Keep a couple of fighters as escort/interception, then bomb. The
            // enemy-aircraft term means a side that DOES face planes still
            // answers them rather than blindly building bombers.
            int own_fighters = 0, enemy_air = 0;
            for (auto r2 : world.active_units) {
                Unit* au = world.get(r2);
                if (!au || !au->common.alive || !au->common.is_air) continue;
                if (au->common.team == team) { if (au->base_attack > 0 && !au->is_bomber) ++own_fighters; }
                else if (!allied(au->common.team, team)) ++enemy_air;
            }
            // `>=`, not `>`. With the same AI on both sides the fighter counts
            // are symmetric, so a strict `>` is essentially never satisfied and
            // the airbase falls back to fighters forever -- the exact bug this
            // block replaced, reintroduced one comparison lower down (measured:
            // 2 bombers in 64 games). At `>=` a side only stops bombing when it
            // is genuinely behind in the air and needs interceptors.
            // A navy map keeps its EXACT previous behaviour (straight to bombers
            // at War era) -- that path was never broken and regressing it was
            // not the point of this change. The escort-then-bomb rule is purely
            // additive, for the land-map airbases that could not exist before.
            // ---- CANDIDATE (feature 22): one escort, not two -----------------
            // Two fighters before the first bomber is a long wait when the whole
            // air force is one or two airbases deep: it is two full train cycles
            // of an expensive unit whose only job is to fight aircraft the enemy
            // may not have, and the `own_fighters >= enemy_air` term underneath
            // already handles the case where he does. What converts an air force
            // into a won game is bombing his buildings, and every tick spent
            // building the escort for that is a tick the bomber is not flying.
            //
            // Kept at one rather than zero on purpose: a lone unescorted bomber
            // meeting a single fighter dies for 350 iron, so the escort rule
            // still exists -- it just stops being the thing that delays the whole
            // air game.
            const int escort = variant_has(td.ai_variant, 22) ? 1 : 2;
            bool want_bomber = (strat == "navy" && era >= 2) ||
                               (era >= 2 && own_fighters >= escort && own_fighters >= enemy_air);
            train_mil(ref, want_bomber ? "bomber" : "fighter");
        } else if (b->name == "shipyard") {
            // Land-blocked (enemy across water): make sure ONE transport ship
            // exists to ferry the land army over -- it's the only way ground
            // troops reach the enemy at all (ai_amphibious drives the boat run).
            // Only once there's a real land army to ship, so it isn't built idly.
            if (td.ai_map_derive && !td.ai_plan.land_to_enemy) {
                bool have_transport = false;
                for (auto r2 : world.active_units) {
                    Unit* s = world.get(r2);
                    if (s && s->common.alive && s->common.team == team && s->transport_cap > 0) {
                        have_transport = true;
                        break;
                    }
                }
                int land_army = 0;
                for (auto r2 : force) {
                    Unit* fu = world.get(r2);
                    if (fu && !fu->common.is_ship && !fu->common.is_air) ++land_army;
                }
                if (!have_transport && land_army >= 3 && b->queue.empty()) {
                    world.enqueue(ref, "transport ship");
                    continue;
                }
            }
            // Fishing boats first on a water/naval plan -- they feed the food
            // boom -- until the fleet goal is met; warships only once the boom
            // ramp is over (train_mil holds them during it).
            if (td.ai_map_derive && td.ai_plan.can_fish) {
                int boats = 0;
                for (auto r2 : world.active_units) {
                    Unit* fu = world.get(r2);
                    if (fu && fu->common.alive && fu->common.team == team && fu->name == "fishing boat")
                        ++boats;
                }
                if (boats < td.ai_plan.fish_goal) {
                    if (b->queue.empty()) world.enqueue(ref, "fishing boat");
                    continue;
                }
            }
            // Warships need water worth contesting. The shipyard gate in
            // ai_build already asks that question (AiPlan::naval_viable) before
            // putting a DOCK up -- but the dock raised by the "stranded, needs a
            // transport" branch just above bypasses that gate by design, and
            // then fell through to here and built a battle fleet anyway. On the
            // 600-match Ostland arena that meant ~7 warships per dock-owning
            // team on maps averaging 0.49% water: ships that can't reach a
            // target, paid for with the army that could (dock owners produced
            // 69.6 military to their opponents' 106.5, and went 4-117-21).
            //
            // So: on a map the plan calls non-naval, the stranded dock builds
            // its transport and then stops. Anything that still has enemy SHIPS
            // to fight keeps building warships regardless of the map verdict --
            // being out-shipped on a lake is its own way to lose.
            bool enemy_ships = false;
            for (auto r2 : world.active_units) {
                Unit* s = world.get(r2);
                if (s && s->common.alive && s->common.is_ship && s->base_attack > 0 &&
                    s->common.team >= 0 && !allied(s->common.team, team)) {
                    enemy_ships = true;
                    break;
                }
            }
            if (td.ai_map_derive && !td.ai_plan.naval_viable && !enemy_ships) continue;
            std::string ship = (era >= 2) ? (td.tech.count("battleship upgrade") ? "battleship" : "destroyer")
                               : (era >= 1) ? "destroyer"
                                            : "frigate";
            train_mil(ref, ship);
        }
    }
}

// ---- CANDIDATE (ai_variant == 12): target priority + focus fire ------------
//
// Everything the AI does with its army stops at "walk at the enemy base": the
// offensive push sets Unit::rally and nothing else. WHAT each unit shoots is
// then decided entirely by update_combat's auto-acquire, which takes the
// NEAREST hostile entity of any kind -- a farm, a palisade segment, a house,
// a villager and a heavy tank are all identical to it.
//
// Two things follow, and both show up in the arena data.
//
// First, an army that arrives at a base spends itself on the base's outskirts.
// A mature base is mostly farms and houses (the AI builds far more of those
// than anything else), so those are what is nearest to a force coming in, and
// those are what it chews on while the defenders shoot back. Farms are 200 hp
// of pure decoration -- razing every one of them changes nothing about who wins.
//
// Second, and worse: there is no concentration at all. Twelve units within
// range of a fight pick twelve independent nearest targets, so twelve enemies
// each take one unit's damage. Nothing in this game degrades as it loses HP --
// a unit at 1 hp deals exactly the same damage as a unit at full -- so spread
// damage is close to no damage at all, and the only thing that reduces incoming
// fire is a target actually dying. That is the mechanism behind the arena's
// most striking number: each side produces ~117 military units and loses ~113
// of them (96%) while peak army size never exceeds ~31. Armies are not being
// destroyed in battles, they are dissolving into each other.
//
// So: rank what is in reach, and allocate.
//
//   RANK -- by what killing it is worth DIVIDED BY how long this particular
//   unit would take to kill it, not by how close it is. Anything that shoots
//   outranks anything that does not; a villager outranks a building (economy
//   damage compounds, a farm does not); production outranks decoration. The
//   per-unit time term is what stops the ranking becoming its own trap -- see
//   the scoring comment below for why raw value alone is actively harmful here.
//
//   ALLOCATE -- assign units to the top target until the damage committed to it
//   would kill it (its "pool"), then move to the next one. That is focus fire
//   with a bound on overkill in one rule: enough guns to kill it, no more.
//
// Deliberately does NOT set Unit::forced. A forced unit chases its target
// across the map regardless of range and ignores everything else on the way,
// which turns one exposed villager into an army-wide wild goose chase. Without
// it, update_combat KEEPS a still-valid assigned target (it only re-acquires
// once the target is dead or past acq * 1.6) and quietly falls back to its own
// auto-acquire otherwise -- so this steers the army without ever taking the
// safety rails off. The whole allocation is recomputed from scratch each AI
// tick; the iteration order is stable, so a unit whose situation hasn't changed
// keeps the same target instead of thrashing.
//
// ---- PROMOTED, 2026-08-14 (feature 31), on a player report rather than a win
// rate ----------------------------------------------------------------------
// This measured 50.0% as variant 12 (192 matches, CI 42.9-57.1) and was left
// off on that basis. It ships now for a reason the A/B could not see: in
// self-play BOTH sides chew on farms and BOTH sides spread their damage, so the
// two errors cancel and the win rate says nothing at all. Against a human who
// does neither, they do not cancel -- and the report is exactly the symptom,
// "the AI locks on to buildings instead of shooting my units".
//
// Shipping it also brings two rules the 50% version did not have, and they are
// most of the point: the target list is collected UNITS FIRST so buildings
// cannot crowd it out, and no building is even a candidate for a unit that has
// an enemy soldier in reach. See the two `unit_in_reach` blocks below.
void Control::ai_focus_fire(int team, World& world, const std::vector<EntityRef>& force) {
    Team& td = teams[team];
    if (!(variant_has(td.ai_variant, 12) || variant_has(td.ai_variant, 31)) || force.empty()) return;

    // How much of a target's remaining HP one assigned unit is counted as
    // claiming. Not a prediction of the fight -- just the unit that saturation
    // is measured in. Four seconds is long enough that a squad's worth of
    // damage saturates a unit-sized target and short enough that a building
    // still soaks the whole force, which is the behaviour we want in each case.
    constexpr double kFocusWindow = 4.0;

    struct Target {
        EntityRef ref;
        double x = 0.0, y = 0.0;
        double hp = 0.0;     // what is actually left of it
        double pool = 0.0;   // that HP minus what assigned units have claimed
        double value = 0.0;  // what killing it is worth
        int armor = 0, pierce = 0;
        bool is_air = false;
        bool is_unit = false;
    };
    std::vector<Target> targets;
    // Cap the list so a fight in the middle of a mature base can't turn this
    // into an O(force * every building on the map) sweep.
    constexpr size_t kMaxTargets = 64;

    auto acq_radius = [](const Unit& u) {
        // Ground units auto-acquire out to whichever is greater of sight and
        // weapon range (update_combat); aircraft judge by sight alone
        // (aircraft_behavior). Match each so an assignment is one the unit's
        // own combat code will actually hold on to rather than drop next tick.
        return u.common.is_air ? u.sight_px : std::max(u.sight_px, u.range_px);
    };

    // Two passes, UNITS FIRST (feature 31). The list is capped, and it used to be
    // filled in whatever order the force happened to sweep -- so an army arriving
    // at the edge of a mature base, which is mostly farms and houses, could fill
    // all 64 slots with buildings before it ever reached the defenders standing
    // behind them. The units-before-buildings rule below would then find no unit
    // in the list and correctly conclude there was nothing better to shoot,
    // having never looked. Collecting every reachable unit before any building
    // removes the bias at its source; buildings still fill whatever is left.
    for (int pass = 0; pass < 2; ++pass) {
    for (auto uref : force) {
        Unit* u = world.get(uref);
        if (!u || !u->common.alive || u->attack <= 0.0) continue;
        if (targets.size() >= kMaxTargets) break;
        for (auto tref : world.grid.query(u->common.x, u->common.y, acq_radius(*u))) {
            EntityCommon* c = world.common(tref);
            if (!c || !c->alive || c->team < 0 || allied(c->team, team)) continue;
            if (c->kind != EntityKind::Unit && c->kind != EntityKind::Building) continue;
            if (variant_has(td.ai_variant, 31) &&
                (c->kind == EntityKind::Unit) != (pass == 0))
                continue; // pass 0 collects units, pass 1 buildings
            bool dup = false;
            for (const auto& t : targets)
                if (t.ref == tref) { dup = true; break; }
            if (dup) continue;
            Target t;
            t.ref = tref;
            t.x = c->x;
            t.y = c->y;
            t.hp = c->hp;
            t.pool = c->hp;
            t.is_air = c->is_air;
            if (Unit* eu = world.get(tref)) {
                t.is_unit = true;
                t.armor = eu->armor;
                t.pierce = eu->pierce;
                // Anything that shoots is the reason our units are dying, so it
                // comes first, hardest-hitting first. A gatherer sits just below
                // that and above every building: killing the workforce is what
                // actually costs an opponent the game, and a villager dies to a
                // couple of volleys.
                t.value = eu->is_gatherer ? 55.0 : 100.0 + eu->attack * 2.0;
            } else if (Building* eb = world.get_building(tref)) {
                t.armor = eb->armor;
                t.pierce = eb->pierce;
                const std::string& n = eb->name;
                t.value = (n == "base")                                  ? 120.0 // the win condition
                          : (n == "tower")                               ? 90.0  // shoots back
                          : (n == "barracks" || n == "academy" || n == "factory" ||
                             n == "airbase" || n == "shipyard" || n == "fortress" ||
                             n == "university" || n == "refinery" || n == "market")
                              ? 30.0                                             // production
                              : 4.0; // farms, houses, walls: decoration
            } else {
                continue;
            }
            if (t.pool <= 0.0) continue;
            targets.push_back(t);
            if (targets.size() >= kMaxTargets) break;
        }
    }
    // Without feature 31 there is only ever one pass: the single loop this
    // replaced, byte for byte.
    if (!variant_has(td.ai_variant, 31)) break;
    }
    if (targets.empty()) return;

    for (auto uref : force) {
        Unit* u = world.get(uref);
        if (!u || !u->common.alive || u->attack <= 0.0) continue;
        // Leave anything already spoken for by a more specific system alone:
        // a ballistic launcher is driven by the attack-ground logic in ai_tick,
        // a boarding/carried unit belongs to ai_amphibious, and `forced` only
        // ever comes from an explicit order.
        if (u->is_ballistic || u->forced || u->carrier.valid() || u->load_target.valid()) continue;
        if (td.ai_plan.scout.valid() && uref == td.ai_plan.scout) continue;

        double acq = acq_radius(*u);
        // ---- UNITS BEFORE BUILDINGS (feature 31) ---------------------------
        // The value/time-to-kill ranking below is a good tie-breaker and a bad
        // priority. It is a ratio, so a farm -- 200 hp, value 4, dies in three
        // volleys -- can out-score a rifleman that is actively shooting back,
        // and a mature base is mostly farms and houses, so that is what an
        // arriving wave finds nearest and what it stands there demolishing
        // while the defenders kill it. Razing every farm on the map wins
        // nothing; the win condition is a base, and the thing stopping us
        // reaching it is his army.
        //
        // So this is a hard tier, not a weight: if ANYTHING this unit is
        // allowed to shoot at is a unit, no building is a candidate at all.
        // Per-unit, not per-army -- a unit with no enemy soldier in reach
        // should absolutely keep hitting the building in front of it.
        bool unit_in_reach = false;
        if (variant_has(td.ai_variant, 31)) {
            for (const auto& t : targets) {
                if (!t.is_unit || t.pool <= 0.0) continue;
                if (u->melee && t.is_air) continue;
                if (u->is_aa && !t.is_air) continue;
                if (!u->is_aa && u->is_bomber && t.is_air) continue;
                if (std::hypot(t.x - u->common.x, t.y - u->common.y) > acq) continue;
                unit_in_reach = true;
                break;
            }
        }
        Target* best = nullptr;
        double best_score = 0.0;
        for (auto& t : targets) {
            if (t.pool <= 0.0) continue;
            if (unit_in_reach && !t.is_unit) continue; // feature 31: units first, always
            // Respect the engagement restrictions update_combat would enforce
            // anyway, so we never hand a unit a target it will drop on sight:
            // melee cannot reach aircraft, AA mounts fire ONLY at aircraft, and
            // a bomber cannot bomb a plane.
            if (u->melee && t.is_air) continue;
            if (u->is_aa && !t.is_air) continue;
            if (!u->is_aa && u->is_bomber && t.is_air) continue;
            double d = std::hypot(t.x - u->common.x, t.y - u->common.y);
            if (d > acq) continue;
            // VALUE PER SECOND OF THIS UNIT'S FIRE, not raw value. Ranking on
            // value alone is a trap in this damage model: it reads
            // max(attack - (melee ? armor : pierce), 1) -- see World::hurt's
            // callers -- so a rifleman (attack 6) shooting a heavy tank
            // (pierce 6, 300 hp) does literally 1 damage a shot and needs 300
            // of them. A heavy tank is the highest-value thing on the field and
            // very nearly the worst possible use of a rifleman's time; a pure
            // value ranking would send every rifleman we own at it while the
            // enemy's riflemen -- which they CAN kill, and which are what is
            // killing them -- go unshot.
            //
            // Dividing by time-to-kill fixes both directions at once. It is the
            // same insight as the counter-composition production rule (feature
            // 6), applied to a single engagement instead of a build queue: shoot
            // whatever converts this unit's damage into dead enemy fastest,
            // weighted by how much that enemy mattered. It also finishes wounded
            // targets off for free, since a target's remaining HP is exactly
            // what "time to kill" is measured against.
            double eff = std::max(u->attack - (u->melee ? t.armor : t.pierce), 1.0);
            double dps = eff / std::max(0.1, u->reload);
            double score = t.value * dps / std::max(1.0, t.hp);
            // Distance only separates targets that are otherwise equal (two
            // farms, two riflemen), so it never overrides the ranking itself.
            score -= d * 1e-6;
            if (score > best_score) { best_score = score; best = &t; }
        }
        if (!best) continue;
        u->attack_target = best->ref;
        // Debit what this unit is expected to contribute over the window, so a
        // unit that can barely scratch this target claims almost none of it and
        // the next one is sent elsewhere.
        double best_eff = std::max(u->attack - (u->melee ? best->armor : best->pierce), 1.0);
        best->pool -= best_eff / std::max(0.1, u->reload) * kFocusWindow;
    }
}

// ---- fortress doctrine ----------------------------------------------------
// "Don't attack a fortress unless you can do it without being hit -- outrange
// it, or bomb it." The three facts that make this decidable rather than a
// guess, all read straight from the catalog and the sim:
//
//   * a fortress has attack 5 at RANGE 8 tiles and 2000 hp;
//   * it is not an AA structure, and building_behavior drops any target whose
//     is_air does not match (`b.is_aa != e->is_air`), so it cannot fire at
//     aircraft AT ALL -- a bomber is not merely favoured against it, it is
//     completely immune;
//   * a unit closes only to its own weapon range (update_combat's `reach`), so
//     anything with range > 8 tiles shoots it from outside its arc and takes
//     nothing back. Artillery is range 10, the ballistic missile 16. The
//     mainline infantry the AI mass-produces is 3-4, i.e. deep inside it.
//
// So the rule is simply: whoever outranges it or flies gets sent at it, and
// everyone else is pulled out of the radius and left to do something useful.
void Control::ai_siege(int team, World& world, const std::vector<EntityRef>& force) {
    // Fortress range plus a tile of margin -- the line a short-ranged unit must
    // not be standing inside. kFortressReach is deliberately derived from the
    // building's own stat rather than hard-coded per unit, so a catalog change
    // to the fortress moves this with it.
    struct Fort { EntityRef ref; double x, y; };
    std::vector<Fort> forts;
    double fort_range = 8.0 * TILE;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive || b->common.team < 0 || allied(b->common.team, team)) continue;
        if (b->name != "fortress") continue;
        forts.push_back({ref, b->common.x, b->common.y});
        if (b->range_px > 0.0) fort_range = std::max(fort_range, b->range_px);
    }
    if (forts.empty()) return;
    // One tile of margin normally; TWO once feature 33 is on, so the line a unit
    // must not cross sits outside the fortress's reach rather than on it -- a
    // unit that only turns around at range+1 has already been fired on.
    const double danger =
        fort_range + (variant_has(teams[team].ai_variant, 33) ? 2.0 : 1.0) * TILE;

    for (auto uref : force) {
        Unit* u = world.get(uref);
        if (!u || !u->common.alive || u->attack <= 0.0) continue;
        if (u->carrier.valid() || u->load_target.valid()) continue;

        // Nearest fortress to this unit, and how far outside its arc we are.
        const Fort* near = nullptr;
        double nd = 1e18;
        for (const auto& f : forts) {
            double d = std::hypot(f.x - u->common.x, f.y - u->common.y);
            if (d < nd) { nd = d; near = &f; }
        }
        if (!near) continue;

        // Can this unit hurt it for free? Aircraft always can (it cannot shoot
        // back). A ground unit can only if its weapon genuinely reaches further
        // than the fortress's does.
        bool safe = u->common.is_air || (!u->melee && u->range_px > fort_range + 1.0);
        if (safe) {
            // Only commit the ones that are actually in the area -- a fresh
            // artillery piece at home should keep following the normal push
            // until it arrives, not turn and walk at the fortress alone.
            // A ballistic launcher is left alone entirely: ai_tick drives it via
            // attack_ground, which is its own (deploy-then-fire) mechanism.
            if (nd < fort_range * 3.0 && !u->is_ballistic && !u->attack_ground.has_value()) {
                // ---- NOT ALONE (feature 32) ----------------------------------
                // `forced` is a commitment: it makes this unit chase the
                // fortress regardless of range or sight and ignore everything
                // on the way, including whatever is shooting it. Outranging a
                // fortress means nothing against the infantry guarding it, and
                // a lone artillery piece walking into a defended base is the
                // most expensive unit in the army thrown away. So the siege only
                // takes the job with a line in front of it; otherwise it stays
                // on the normal push, which (feature 32, ai_tick) is itself now
                // holding it back until escorts arrive.
                bool escorted = true;
                if (variant_has(teams[team].ai_variant, 32)) {
                    int friends = 0;
                    for (auto oref : force) {
                        Unit* o = world.get(oref);
                        if (!o || !o->common.alive || oref == uref) continue;
                        if (o->range_px >= kSiegeRange) continue; // another gun is not cover
                        if (std::hypot(o->common.x - u->common.x, o->common.y - u->common.y) <=
                            kSquadRadius)
                            ++friends;
                    }
                    escorted = friends >= 2;
                }
                if (escorted) {
                    u->attack_target = near->ref;
                    u->forced = true; // hold it: this is the job
                }
            }
            continue;
        }

        // Not safe. Drop the fortress if this unit has picked it up (auto-acquire
        // takes the NEAREST hostile of any kind, so a fortress on the approach is
        // exactly what a rifleman walks into), and step back out of its arc.
        bool was_engaging = false;
        if (u->attack_target.valid()) {
            Building* t = world.get_building(u->attack_target);
            if (t && t->name == "fortress") {
                u->attack_target = kNullRef;
                u->forced = false;
                was_engaging = true;
            }
        }
        // ---- OUT OF THE ARC ON CONTACT (feature 33) ------------------------
        // The old rule pulled back only a unit that was ACTUALLY shooting at the
        // fortress, to avoid fighting the offensive push (which re-rallies it
        // toward the objective the next tick, so it dithers on the doorstep).
        // That trade is wrong, and the report says so plainly: waiting until a
        // unit has picked the fortress as its target means waiting until it is
        // already deep inside a 2000-hp emplacement's arc taking fire, and
        // "arrived, shot at, then walked back out" is just a slower way of
        // losing the unit.
        //
        // So any unit that cannot hurt it and is inside `danger` leaves, whether
        // it had engaged or not. Note `danger` is now the fortress's reach plus
        // TWO tiles (below), so the back-out starts BEFORE the unit is in range
        // rather than once it is -- "the moment it is in sight" is not literally
        // implementable, because a fortress outranges the sight of everything
        // that fears it (range 8 vs sight 5-6): the unit is being shot before it
        // can see what is shooting.
        //
        // The dithering the old comment warned about is real and is accepted:
        // the army now bounces along the edge of the arc instead of walking into
        // it. That IS the doctrine ("everyone else is pulled out of the radius")
        // -- artillery and aircraft are what kill the fortress, and this batch
        // (feature 32/35) is also what finally gets them built.
        bool backout = variant_has(teams[team].ai_variant, 33) ? (nd < danger)
                                                               : (was_engaging && nd < danger);
        if (backout) {
            // A rally alone does NOT get a unit out. update_combat drives an
            // assigned attack_target ahead of any rally and closes to weapon
            // range for it, so a rifleman shooting a villager that happens to be
            // standing under the fortress simply stays there and dies with a
            // retreat order it never executes. Drop any target that is itself
            // inside the arc -- nothing in there is worth the trade -- and leave
            // targets outside it alone, since walking out to fight those is the
            // move anyway.
            if (variant_has(teams[team].ai_variant, 33) && u->attack_target.valid()) {
                if (EntityCommon* tc = world.common(u->attack_target)) {
                    if (std::hypot(tc->x - near->x, tc->y - near->y) < danger) {
                        u->attack_target = kNullRef;
                        u->forced = false;
                    }
                }
            }
            // Retreat directly away from the fortress to just outside its reach.
            double dx = u->common.x - near->x, dy = u->common.y - near->y;
            double len = std::hypot(dx, dy);
            if (len < 1e-6) { dx = 1.0; dy = 0.0; len = 1.0; }
            u->rally = Vec2{near->x + dx / len * (danger + 2.0 * TILE),
                            near->y + dy / len * (danger + 2.0 * TILE)};
        }
    }
}

void Control::ai_amphibious(int team, World& world, EntityRef base,
                            const std::vector<EntityRef>& force) {
    Team& td = teams[team];
    // Only when the enemy is genuinely across water AND we've massed an army.
    if (td.ai_plan.land_to_enemy || !td.ai_committed) return;
    Building* mb = base.valid() ? world.get_building(base) : nullptr;
    if (!mb) return;
    double hx = mb->common.x, hy = mb->common.y;

    // Nearest enemy base (the invasion objective).
    double ex = 0, ey = 0, ebest = 1e18;
    bool have_enemy = false;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive || b->common.team < 0 || allied(b->common.team, team)) continue;
        double d = (b->common.x - hx) * (b->common.x - hx) + (b->common.y - hy) * (b->common.y - hy);
        if (d < ebest) { ebest = d; ex = b->common.x; ey = b->common.y; have_enemy = true; }
    }
    if (!have_enemy) return;

    // Troops already landed on the enemy side (closer to the enemy than home)
    // press the attack -- the offensive push skips land units on a water map,
    // so this is what keeps a landed wave moving onto the objective.
    for (auto uref : force) {
        Unit* u = world.get(uref);
        if (!u || u->common.is_ship || u->common.is_air || u->carrier.valid() || u->load_target.valid())
            continue;
        double dh = std::hypot(u->common.x - hx, u->common.y - hy);
        double de = std::hypot(u->common.x - ex, u->common.y - ey);
        if (de < dh && !u->attack_target.valid() && !u->rally.has_value())
            u->rally = Vec2{ex, ey};
    }

    // Resolve our transport (validate the tracked one, else adopt any own boat).
    Unit* ship = world.get(td.ai_transport);
    if (!ship || !ship->common.alive || ship->transport_cap <= 0 || ship->common.team != team) {
        td.ai_transport = kNullRef;
        for (auto ref : world.active_units) {
            Unit* s = world.get(ref);
            if (s && s->common.alive && s->common.team == team && s->transport_cap > 0) {
                td.ai_transport = ref;
                ship = s;
                break;
            }
        }
    }
    if (!ship) return; // ai_train is queuing one at the shipyard

    double used = 0.0;
    for (auto c : ship->cargo)
        if (Unit* cu = world.get(c)) used += transport_cost(cu->name);
    double cap = ship->transport_cap;
    int loadable = 0, boarding = 0;
    for (auto uref : force) {
        Unit* u = world.get(uref);
        if (!u || u->common.is_ship || u->common.is_air || u->carrier.valid()) continue;
        if (u->load_target == td.ai_transport) { ++boarding; continue; }
        if (u->load_target.valid()) continue;
        double dh = std::hypot(u->common.x - hx, u->common.y - hy);
        double de = std::hypot(u->common.x - ex, u->common.y - ey);
        if (dh <= de) ++loadable; // still home-side, can be loaded
    }

    // Enemy shore: nearest LAND tile to the enemy base that touches water, plus
    // the adjacent water tile the transport approaches through.
    double dropx = ex, dropy = ey, apprx = ex, appry = ey;
    bool shore = false;
    int etx = static_cast<int>(ex / TILE), ety = static_cast<int>(ey / TILE);
    static const int dxs4[4] = {1, -1, 0, 0}, dys4[4] = {0, 0, 1, -1};
    for (int r = 1; r < 60 && !shore; ++r) {
        int steps = std::max(8, r * 6);
        for (int a = 0; a < steps && !shore; ++a) {
            double ang = a * 2.0 * M_PI / steps;
            int tx = etx + static_cast<int>(std::lround(std::cos(ang) * r));
            int ty = ety + static_cast<int>(std::lround(std::sin(ang) * r));
            if (tx < 0 || tx >= world.cols || ty < 0 || ty >= world.rows) continue;
            if (world.terrain[tx][ty] == WATER) continue; // want land
            for (int d = 0; d < 4 && !shore; ++d) {
                int wx = tx + dxs4[d], wy = ty + dys4[d];
                if (wx < 0 || wx >= world.cols || wy < 0 || wy >= world.rows) continue;
                if (world.terrain[wx][wy] != WATER) continue;
                dropx = (tx + 0.5) * TILE; dropy = (ty + 0.5) * TILE;
                apprx = (wx + 0.5) * TILE; appry = (wy + 0.5) * TILE;
                shore = true;
            }
        }
    }
    if (!shore) return;

    bool has_cargo = used > 0.5;
    bool ready = has_cargo && (used >= std::min(cap, 6.0) || (loadable == 0 && boarding == 0));
    if (!ready) {
        // LOAD near home. If the (empty) boat drifted out to the enemy after a
        // prior run, sail it home first so land troops can actually reach it.
        double dhome = std::hypot(ship->common.x - hx, ship->common.y - hy);
        if (!has_cargo && dhome > 14.0 * TILE && !ship->rally.has_value())
            ship->rally = Vec2{hx, hy};
        for (auto uref : force) {
            Unit* u = world.get(uref);
            if (!u || u->common.is_ship || u->common.is_air || u->carrier.valid()) continue;
            if (u->load_target.valid()) continue;
            double dh = std::hypot(u->common.x - hx, u->common.y - hy);
            double de = std::hypot(u->common.x - ex, u->common.y - ey);
            if (de < dh) continue; // already across -- handled above
            if (used + transport_cost(u->name) > cap) break;
            u->load_target = td.ai_transport;
            u->rally.reset();
            u->attack_target = kNullRef;
            u->forced = false;
            used += transport_cost(u->name);
        }
        return;
    }

    // CROSS + UNLOAD: sail to the shore approach, disgorge onto the enemy beach,
    // then send the landed troops at the enemy base.
    double dship = std::hypot(ship->common.x - dropx, ship->common.y - dropy);
    if (dship <= 4.8 * TILE) {
        std::vector<EntityRef> landed = ship->cargo;
        if (world.unload_transport(td.ai_transport, dropx, dropy)) {
            for (auto cref : landed)
                if (Unit* cu = world.get(cref)) cu->rally = Vec2{ex, ey};
        } else if (!ship->rally.has_value()) {
            ship->rally = Vec2{apprx, appry};
        }
    } else if (!ship->rally.has_value()) {
        ship->rally = Vec2{apprx, appry};
    }
}

void Control::ai_defend(int team, World& world, const std::vector<EntityRef>& team_buildings,
                        const std::vector<EntityRef>& force) {
    // A hostile unit landing this close to one of our buildings counts as a
    // raid worth responding to; a friendly military unit within the WIDER
    // recall radius of THAT SAME building gets pulled back to meet it. Units
    // beyond the recall radius are left alone on purpose -- a unit already
    // deep in enemy territory (mid-offense) is worth more finishing what it
    // started than walking all the way home for a raid it can't reach in
    // time anyway, and that's exactly the "don't recall the ones already
    // attacking far forward" balance requested for this feature.
    constexpr double kDefendRadius = 10.0 * TILE;
    constexpr double kRecallRadius = 16.0 * TILE;
    EntityRef threat = kNullRef;
    double threat_d2 = kDefendRadius * kDefendRadius;
    double bx = 0.0, by = 0.0; // position of the specific building under threat
    bool threat_on_base = false; // ...and whether that building is a town centre
    for (auto bref : team_buildings) {
        Building* b = world.get_building(bref);
        if (!b || !b->common.alive) continue;
        EntityRef nearby = world.nearest(b->common.x, b->common.y, kDefendRadius, [&](EntityRef, EntityCommon& c) {
            return c.alive && c.team >= 0 && c.kind == EntityKind::Unit && !allied(c.team, team);
        });
        if (!nearby.valid()) continue;
        EntityCommon* nc = world.common(nearby);
        double d2 = (nc->x - b->common.x) * (nc->x - b->common.x) + (nc->y - b->common.y) * (nc->y - b->common.y);
        if (d2 < threat_d2) {
            threat_d2 = d2;
            threat = nearby;
            bx = b->common.x;
            by = b->common.y;
            threat_on_base = (b->name == "base");
        }
    }
    if (!threat.valid()) return;
    EntityCommon* tc = world.common(threat);
    if (!tc) return;
    // ---- CANDIDATE (feature 24): come home when the TOWN CENTRE is being hit -
    // The 16-tile recall is the right rule for a raid on a farm: a unit already
    // deep in enemy territory is worth more finishing what it started than
    // walking all the way home for something it cannot reach in time.
    //
    // It is the wrong rule for the town centre, because losing the last base is
    // not a loss of value, it is THE loss condition (Control::check_win). A team
    // whose army is away on a push and whose base is being dismantled currently
    // recalls only whatever happens to be within 16 tiles of it -- typically
    // nothing, since the push is why the base is undefended -- and loses the game
    // while winning the fight at the other end of the map. That is the "I trade
    // his base for mine and he gets there first" outcome, decided by nobody.
    //
    // Widened, not made unbounded, and only while the base is ACTUALLY TAKING
    // DAMAGE (ai_under_fire, stamped by World::hurt) rather than merely having
    // something hostile near it -- a scout wandering past a town centre must not
    // be able to pull an entire committed push back across the map, which is
    // exactly the failure mode the bare-proximity checks elsewhere in this file
    // keep running into.
    double recall = kRecallRadius;
    if (variant_has(teams[team].ai_variant, 24) && threat_on_base &&
        teams[team].ai_under_fire > 0.0)
        recall = 40.0 * TILE;
    for (auto uref : force) {
        Unit* u = world.get(uref);
        if (!u) continue;
        double dx = u->common.x - bx, dy = u->common.y - by;
        // Unconditional overwrite (even of an existing rally toward the
        // forward objective) -- a unit that hasn't actually left the home
        // area yet should peel off for the closer emergency first.
        if (dx * dx + dy * dy <= recall * recall) u->rally = Vec2{tc->x, tc->y};
    }
}

void Control::ai_defend_civilians(int team, World& world) {
    // Threat/group radii scale with a villager's own sight (bigger sight
    // techs/civ bonuses widen both), rather than a flat pixel constant.
    // Anyone found is handled as a GROUP, not independently, so a cluster of
    // villagers reaches the same fight-or-flee decision together instead of
    // splitting (one fighting while its neighbour flees the same threat).
    std::vector<EntityRef> civs;
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (u && u->common.alive && u->common.team == team && u->is_gatherer) civs.push_back(ref);
    }
    std::vector<bool> handled(civs.size(), false);

    // Home anchor + leash: villagers only fight NEAR their base and let a
    // retreating threat go, returning to work -- so an early scout/raider can't
    // kite the whole villager line away from the economy. (Only AI civilians
    // are driven here; a `forced` villager attack always originates from this
    // function, so it's safe to clear.)
    double bx = 0, by = 0;
    bool have_base = false;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (b && b->common.alive && b->common.team == team && b->name == "base") {
            bx = b->common.x;
            by = b->common.y;
            have_base = true;
            break;
        }
    }
    constexpr double kEngageLeash = 11.0 * TILE; // only start a fight this close to home
    constexpr double kDropLeash = 15.0 * TILE;   // drop pursuit once the target passes this
    // Disengage stale pursuit: a villager still chasing a target that has died,
    // vanished, or retreated past the leash drops it and returns to work (its
    // gather_target was left intact, so the gather dispatch resumes it).
    if (have_base) {
        for (auto ref : civs) {
            Unit* u = world.get(ref);
            if (!u || !u->forced || !u->attack_target.valid()) continue;
            EntityCommon* atc = world.common(u->attack_target);
            bool drop = !atc || !atc->alive ||
                        (atc->x - bx) * (atc->x - bx) + (atc->y - by) * (atc->y - by) >
                            kDropLeash * kDropLeash;
            if (drop) {
                u->forced = false;
                u->attack_target = kNullRef;
            }
        }
    }

    for (size_t i = 0; i < civs.size(); ++i) {
        if (handled[i]) continue;
        Unit* u = world.get(civs[i]);
        if (!u) continue;
        double threat_radius = u->sight_px;
        EntityRef hostile = world.nearest(u->common.x, u->common.y, threat_radius, [&](EntityRef, EntityCommon& c) {
            return c.alive && c.team >= 0 && c.kind == EntityKind::Unit && !allied(c.team, team);
        });
        if (!hostile.valid()) continue;
        EntityCommon* hc = world.common(hostile);
        if (!hc) continue;

        double group_radius = u->sight_px * 2.5;
        std::vector<size_t> group;
        for (size_t j = 0; j < civs.size(); ++j) {
            if (handled[j]) continue;
            Unit* ou = world.get(civs[j]);
            if (!ou) continue;
            double dx = ou->common.x - u->common.x, dy = ou->common.y - u->common.y;
            if (dx * dx + dy * dy <= group_radius * group_radius) group.push_back(j);
        }

        // How much actual MILITARY muscle is backing this threat, counted
        // near the threat itself (not just the one nearest hostile) --
        // another villager poking around is always fair game, but a real
        // soldier needs the group to genuinely outnumber it, not just match
        // it 1-for-1, before committing to a fight instead of running.
        int hostile_military = 0;
        for (auto eref : world.active_units) {
            Unit* eu = world.get(eref);
            if (!eu || !eu->common.alive || eu->common.team < 0 || allied(eu->common.team, team)) continue;
            double dx = eu->common.x - hc->x, dy = eu->common.y - hc->y;
            if (!eu->is_gatherer && dx * dx + dy * dy <= threat_radius * threat_radius) ++hostile_military;
        }
        // Only commit to a FIGHT when the threat is near home and winnable;
        // a threat out past the leash (or if we have no base) means flee/return
        // to work instead of chasing it across the map.
        bool near_home = have_base && (hc->x - bx) * (hc->x - bx) + (hc->y - by) * (hc->y - by) <=
                                          kEngageLeash * kEngageLeash;
        bool can_win = near_home &&
                       ((hostile_military == 0) || (static_cast<int>(group.size()) >= hostile_military * 2));

        double cx = 0.0, cy = 0.0;
        for (size_t j : group) {
            if (EntityCommon* c = world.common(civs[j])) { cx += c->x; cy += c->y; }
        }
        if (!group.empty()) { cx /= group.size(); cy /= group.size(); }
        // Truly alone (no one nearby to rally with) -- run for the nearest
        // dropoff instead of just standing there, so a lone villager still
        // gets somewhere safer rather than "retreating" to its own position.
        EntityRef fallback = (group.size() <= 1) ? world.nearest_dropoff(team, u->common.x, u->common.y, 0) : kNullRef;
        EntityCommon* fc = fallback.valid() ? world.common(fallback) : nullptr;

        for (size_t j : group) {
            Unit* gu = world.get(civs[j]);
            if (!gu) continue;
            // Pull this villager off whatever it was doing (matches the
            // reset ai_economy's own builder-reassignment uses) -- build_
            // target is checked ahead of the gather/combat split, so leaving
            // it set would silently block forced/rally from ever taking
            // effect. gather_target is deliberately left alone: once the
            // fight/flee resolves (forced clears or rally is reached), the
            // gather dispatch picks it back up and the villager just quietly
            // resumes the same job it had before, no extra bookkeeping needed.
            gu->build_target = kNullRef;
            // Repair outranks the gather/combat split too (update_gather checks
            // repair_target before it ever looks at forced/rally), so a villager
            // left mid-repair would stand at the damaged wall being shot instead
            // of fighting or fleeing. Only ai_variant 13 ever sets this, so
            // clearing it unconditionally is a no-op everywhere else.
            gu->repair_target = kNullRef;
            gu->move_goal.reset();
            gu->path.clear();
            gu->approach_prev_pos.reset();
            gu->approach_progress_check_t = 0.0;
            gu->approach_target.reset();
            if (can_win) {
                gu->forced = true;
                gu->attack_target = hostile;
            } else if (fc) {
                gu->rally = Vec2{fc->x, fc->y};
            } else {
                gu->rally = Vec2{cx, cy};
            }
            handled[j] = true;
        }
    }
}

void Control::ai_research(int team, World& world, const std::vector<EntityRef>& team_buildings) {
    Team& td = teams[team];
    // Keep a working buffer so research never starves civilian/muscateer/age
    // production: muscateers need food+oil and civilians need food, so hold
    // back until both are comfortably stocked. One research per tick, the
    // cheapest useful one available -- economical upgrades (gather/farm/
    // cheap combat buffs) naturally come first, expensive niche techs last.
    // Difficulty tilts how big that buffer needs to be: Hard spends into
    // research eagerly (reaching upgrades sooner), Easy holds a bigger
    // reserve and lags on tech -- same greedy-cheapest-first logic either
    // way, just how soon it's allowed to fire.
    double food_reserve = 200, oil_reserve = 60;
    if (td.difficulty == 0) { food_reserve = 300; oil_reserve = 100; }
    else if (td.difficulty >= 2) { food_reserve = 120; oil_reserve = 30; }
    if (td.res["food"] < food_reserve || td.res["oil"] < oil_reserve) return;

    // Cheapest-first alone never buys the expensive UNIT-UNLOCKING upgrades:
    // there is always some cheaper tech left in the list, so the greedy pick
    // takes that instead, forever. Measured over 320 self-play games: 335
    // team-instances reached the Scientific era and produced TWO heavy tanks
    // between them, because "heavy tank upgrade" was never once the cheapest
    // option -- so ai_train kept falling through to its light-tank default and
    // late-era teams fought with early-era units.
    //
    // These upgrades are what an era is actually FOR, so they get first refusal
    // as a tier of their own; everything else still goes cheapest-first behind
    // them. They're only considered when affordable, so this delays them rather
    // than letting one starve the economy.
    static const std::set<std::string> kUnlockTier = {
        "heavy tank upgrade", "heavy bomber upgrade", "battleship upgrade", "assault rifle",
        "jet engine", "jet fighter", "elite waffen", "cavalry3", "swordsman2", "royal marine",
    };
    // ---- PROMOTED (was ai_variant == 8): buy VALUE, not the cheapest thing ---
    // kUnlockTier above is a hand-maintained list, which is a symptom: the real
    // problem is that the picker has no notion of what a tech is WORTH, so any
    // upgrade it forgets to list is one the AI will never buy. Everything past
    // the list is still chosen purely on price.
    //
    // A unit upgrade converts the ENTIRE standing army of the source type at
    // once (Control::research walks active_units applying the delta), so its
    // value scales with how many of that unit we actually field, times how much
    // better the replacement is. That makes it directly computable from the
    // catalog and the live army, with no list to maintain: 30 riflemen upgrading
    // to infantrymen (+2 attack each) is worth real money, the same upgrade with
    // two riflemen on the field is not.
    //
    // Counting our own army once here rather than per candidate tech.
    std::unordered_map<std::string, int> own_army;
    if (variant_has(td.ai_variant, 8)) {
        for (auto ref : world.active_units) {
            Unit* u = world.get(ref);
            if (!u || !u->common.alive || u->common.team != team || u->is_gatherer) continue;
            ++own_army[u->name];
        }
    }
    // Value per resource spent. Non-upgrade techs (economy, flat combat buffs)
    // keep a modest flat value so they still get bought when nothing better is
    // on offer -- tuned to sit near a mid-size unit upgrade so eco tech isn't
    // starved out entirely, which would be the mirror of the current bug.
    auto tech_value = [&](const std::string& key, int total) -> double {
        double worth = 40.0; // baseline for a non-upgrade tech
        auto om = UPGRADE_MAP.find(key);
        if (om != UPGRADE_MAP.end()) {
            const auto& cat = data_.catalog().at("units");
            const std::string& dst = om->second.second;
            double gain = 0.0;
            if (cat.contains(dst)) {
                double a1 = cat.at(dst).value("attack", 0.0), h1 = cat.at(dst).value("max_life", 0.0);
                double a0 = 0.0, h0 = 0.0;
                if (om->second.first && cat.contains(*om->second.first)) {
                    a0 = cat.at(*om->second.first).value("attack", 0.0);
                    h0 = cat.at(*om->second.first).value("max_life", 0.0);
                }
                // Attack weighted above hit points: in a max(attack - armor, 1)
                // model an attack point is worth far more than a hit point once
                // the enemy has any armour at all.
                gain = std::max(0.0, (a1 - a0) * 10.0 + (h1 - h0) * 0.5);
            }
            int have = om->second.first ? own_army.count(*om->second.first) ? own_army[*om->second.first] : 0
                                        : 0;
            // +4 notional future units: an unlock we field none of yet is still
            // worth something, or nothing would ever bootstrap a new line.
            worth = gain * (have + 4);
        }
        return worth / std::max(1, total);
    };

    std::set<std::string> seen;
    std::string best, best_unlock, best_value;
    double best_value_score = 0.0;
    int best_cost = 1 << 30, best_unlock_cost = 1 << 30;
    for (auto ref : team_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->complete) continue;
        if (!seen.insert(b->name).second) continue; // one lookup per building type
        for (const auto& key : available_techs(b->name, team)) {
            if (td.tech.count(key)) continue;
            auto cost = cost_of(key, team);
            if (cost.empty()) { cost["iron"] = 75; cost["oil"] = 50; }
            bool ok = true;
            int total = 0;
            for (auto& [k, v] : cost) {
                auto it = td.res.find(k);
                if ((it == td.res.end() ? 0.0 : it->second) < v) { ok = false; break; }
                total += v;
            }
            if (!ok) continue;
            if (variant_has(td.ai_variant, 8)) {
                double v = tech_value(key, total);
                if (v > best_value_score) { best_value_score = v; best_value = key; }
                continue; // variant 8 ignores both price-ordered tiers entirely
            }
            if (kUnlockTier.count(key)) {
                if (total < best_unlock_cost) { best_unlock_cost = total; best_unlock = key; }
            } else if (total < best_cost) {
                best_cost = total;
                best = key;
            }
        }
    }
    if (variant_has(td.ai_variant, 8)) best = best_value;
    else if (!best_unlock.empty()) best = best_unlock;
    if (!best.empty()) research(best, team, world);
}

void Control::grant_era_techs(int team, World& world) {
    if (team < 0 || team >= static_cast<int>(teams.size())) return;
    // available_techs already applies every gate that matters -- the civ
    // exclusions, CIV_UPGRADE_OWNER ownership, the era ceiling and the
    // TECH_PREREQ chain -- so this needs no second copy of those rules, and a
    // civ that cannot legally have radar still will not get it.
    //
    // Repeated passes because prerequisites form chains: granting tier 1 is what
    // makes tier 2 appear in available_techs. Bounded rather than while(true) --
    // the tree is a handful deep and a fixed bound cannot hang the tick if a
    // data file ever contains a cycle.
    for (int pass = 0; pass < 8; ++pass) {
        bool granted = false;
        for (const auto& [bname, keys] : building_techs_) {
            (void)keys;
            for (const auto& key : available_techs(bname, team)) {
                // Never the age advances themselves -- Hardest arrives at each
                // era fully teched, it does not skip to the last one.
                if (std::find(AGE_ITEMS.begin(), AGE_ITEMS.end(), key) != AGE_ITEMS.end()) continue;
                if (teams[team].tech.count(key)) continue;
                apply_research(key, team, world);
                granted = true;
            }
        }
        if (!granted) break;
    }
}

bool Control::repair_tick(EntityRef building_ref, int team, double dt, World& world) {
    Building* b = world.get_building(building_ref);
    if (!b || !b->complete || b->common.hp >= b->common.max_hp) return false;
    // Same per-second rate a builder constructs this same building at (see
    // unit_behavior.cpp's build_target branch: +5/+1/+3 construction-%
    // per builder's reload cycle for house&refinery/base&fortress/
    // everything else, which grows hp by (full_max_hp-40) over a full
    // 0->100% construction run -- this is that same total HP gain divided
    // by that same total time, i.e. the equivalent steady HP/sec rate).
    // Used to be a flat max_hp/8 (full HP in 8 seconds regardless of
    // building type or repairer count), 2.5x-12.5x faster than building
    // the same structure from scratch would have been.
    double rate = (b->name == "house" || b->name == "refinery")   ? 5.0
                  : (b->name == "base" || b->name == "fortress") ? 1.0
                                                                  : 3.0;
    double hp_per_sec = (b->full_max_hp - 40.0) * rate * world.build_speed / 100.0;
    double gain = std::min(hp_per_sec * dt, b->common.max_hp - b->common.hp);
    double frac = gain / b->common.max_hp;
    auto base = cost_of(b->name, team);
    Team& td = teams[team];
    std::unordered_map<std::string, double> cost;
    for (auto& [k, v] : base) cost[k] = v * 0.5 * frac;
    for (auto& [k, v] : cost) {
        auto it = td.res.find(k);
        if ((it == td.res.end() ? 0.0 : it->second) < v) {
            if (team == 0) {
                world.events.push({EventType::Warn, "", 0, 0, 0, kNullRef, "You need more resources!"});
            }
            return false;
        }
    }
    for (auto& [k, v] : cost) td.res[k] -= v;
    b->common.hp = std::min(b->common.max_hp, b->common.hp + gain);
    return true;
}

namespace {
constexpr double kCommodityFee = 0.30;
}

double Control::trade_quote(const std::string& action, const std::string& res, int team) const {
    const Team& td = teams[team];
    double fee = kCommodityFee - (td.tech.count("trade agreement") ? 0.15 : 0.0);
    // Joseph Stalin: no market trading fee.
    if (bonuses_.leader_name(td.civ, td.leader) == "Joseph Stalin") fee = 0.0;
    double rate = res == "food" ? (action == "buy" ? td.trade.food_buy : td.trade.food_sell)
                  : res == "wood" ? (action == "buy" ? td.trade.wood_buy : td.trade.wood_sell)
                                  : (action == "buy" ? td.trade.iron_buy : td.trade.iron_sell);
    return std::round(rate * (action == "buy" ? (1.0 + fee) : (1.0 - fee)));
}

bool Control::trade(const std::string& action, const std::string& res, int team, World& world) {
    Team& td = teams[team];
    double* buy = res == "food" ? &td.trade.food_buy : res == "wood" ? &td.trade.wood_buy : &td.trade.iron_buy;
    double* sell = res == "food" ? &td.trade.food_sell : res == "wood" ? &td.trade.wood_sell : &td.trade.iron_sell;

    if (action == "sell") {
        if (td.res[res] < 100) return false;
        double gain = trade_quote("sell", res, team);
        td.res[res] -= 100;
        td.res["oil"] += gain;
        *sell = std::max(20.0, *sell - 3);
        *buy = std::min(999.0, *buy + 3);
    } else {
        double cost = trade_quote("buy", res, team);
        if (td.res["oil"] < cost) return false;
        td.res["oil"] -= cost;
        td.res[res] += 100;
        *buy = std::min(999.0, *buy + 3);
        *sell = std::max(20.0, *sell - 3);
    }
    if (team == 0) world.events.push({EventType::Sound, "trade", 0, 0, 0, kNullRef, ""});
    return true;
}

bool Control::research(const std::string& key, int team, World& world) {
    Team& td = teams[team];
    const auto& techs = data_.techs();
    if (techs.contains(key)) {
        if (!can_research(key, team)) return false;
        if (techs.at(key).contains("cost")) {
            // cost_of carries the civ + leader research discounts (George VI,
            // Stalin, Goering) so this deduction matches what enqueue charged.
            for (auto& [k, v] : cost_of(key, team)) td.res[k] -= v;
        }
        td.tech.insert(key);
        // apply_tech_delta keys on the UNDERSCORED tech name (e.g.
        // "assault_rifle"); single-word catalog techs match either way, but a
        // multi-word one like "assault rifle" would silently apply no effect
        // if passed with its spaces -- underscore it here just like the
        // non-catalog path (apply_research) already does.
        std::string underscored = key;
        std::replace(underscored.begin(), underscored.end(), ' ', '_');
        for (auto ref : world.active_units) {
            Unit* u = world.get(ref);
            if (u && u->common.alive && u->common.team == team) bonuses_.apply_tech_delta(*u, underscored);
        }
        if (team == 0) {
            world.events.push({EventType::Sound, "research", 0, 0, 0, kNullRef, ""});
            world.events.push({EventType::Notify, "research_complete", 0, 0, 0, kNullRef, key});
        }
        return true;
    }
    auto era = TECH_ERA.find(key);
    if (td.tech.count(key) || (era != TECH_ERA.end() ? era->second : 0) > td.era) return false;
    auto cost = cost_of(key, team);
    if (cost.empty()) { cost["iron"] = 75; cost["oil"] = 50; }
    for (auto& [k, v] : cost) {
        auto it = td.res.find(k);
        if ((it == td.res.end() ? 0.0 : it->second) < v) return false;
    }
    for (auto& [k, v] : cost) td.res[k] -= v;
    apply_research(key, team, world);
    return true;
}

void Control::apply_research(const std::string& key, int team, World& world) {
    Team& td = teams[team];
    if (td.tech.count(key)) return;
    td.tech.insert(key);

    auto om = UPGRADE_MAP.find(key);
    if (om != UPGRADE_MAP.end() && om->second.first) {
        const std::string& old_name = *om->second.first;
        const std::string& new_name = om->second.second;
        for (auto ref : std::vector<EntityRef>(world.active_units)) {
            Unit* u = world.get(ref);
            if (u && u->common.alive && u->common.team == team && u->name == old_name) {
                world.transform_unit(ref, new_name);
            }
        }
        for (auto ref : world.active_buildings) {
            Building* b = world.get_building(ref);
            if (b && b->common.team == team) {
                for (auto& q : b->queue) if (q == old_name) q = new_name;
            }
        }
    }
    auto bom = BUILDING_UPGRADE_MAP.find(key);
    if (bom != BUILDING_UPGRADE_MAP.end()) {
        const std::string& old_name = bom->second.first;
        const std::string& new_name = bom->second.second;
        // Only already-COMPLETE buildings convert immediately -- a
        // foundation still under construction finishes as the old form and
        // gets converted right at completion instead (see
        // building_behavior.cpp's construction-complete check), same
        // "training queue keeps the old name, converts on completion" idea
        // UPGRADE_MAP's unit case doesn't need since units train instantly
        // relative to a tick.
        for (auto ref : std::vector<EntityRef>(world.active_buildings)) {
            Building* b = world.get_building(ref);
            if (b && b->common.alive && b->common.team == team && b->name == old_name && b->complete) {
                world.transform_building(ref, new_name);
            }
        }
    }
    std::string underscored = key;
    std::replace(underscored.begin(), underscored.end(), ' ', '_');
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (u && u->common.alive && u->common.team == team) bonuses_.apply_tech_delta(*u, underscored);
    }
    // Building-HP upgrades apply retroactively to EXISTING buildings too (the
    // unit deltas above never touched buildings, so a researched Steel Frame
    // only ever helped buildings raised AFTER it -- the player asked for
    // standing buildings to gain the HP and fill up as well). Steel Frame =
    // +30% building HP; raise the max and top finished buildings up to it,
    // exactly matching how a freshly-built one comes out under the tech.
    if (key == "steel frame" || key == "steel_frame") {
        for (auto ref : world.active_buildings) {
            Building* b = world.get_building(ref);
            if (!b || !b->common.alive || b->common.team != team || b->name == "base") continue;
            b->common.max_hp = std::round(b->common.max_hp * 1.30);
            b->full_max_hp = std::round(b->full_max_hp * 1.30);
            if (b->complete) b->common.hp = b->common.max_hp; // fill finished buildings to the new full
        }
    }
    if (team == 0) {
        world.events.push({EventType::Sound, "research", 0, 0, 0, kNullRef, ""});
        world.events.push({EventType::Notify, "research_complete", 0, 0, 0, kNullRef, key});
    }
}

} // namespace ww::sim
