// Headless sim-only CLI (no SDL). Three modes:
//   --seed N --players N ...        runs a real (RNG-driven) scenario
//                                    and prints per-tick checksums --
//                                    proves the sim runs deterministically
//                                    with no rendering/audio dependency.
//   --fixture <commands.csv> ...     builds the fixed, RNG-free "fixture 1"
//                                    scenario (see
//                                    game/tests/golden/fixture1.md),
//                                    replays a scripted command log, and
//                                    prints CSV state snapshots in the same
//                                    format as the retired Python
//                                    prototype's golden_record.py, for
//                                    diffing against its recordings.
//   --tournament N ...               AI self-play arena: N 1v1 matches,
//                                    both teams AI-controlled, one side
//                                    tagged Team::ai_variant = 0 (baseline)
//                                    and the other = --candidate-variant
//                                    (default 1) -- see control_ai.cpp's
//                                    ai_variant branches. Alternates which
//                                    team index carries the candidate each
//                                    match to cancel spawn-point asymmetry,
//                                    prints a per-match line plus a final
//                                    win-rate/metrics summary, and can also
//                                    write a full per-match CSV (--out).
//   --arena N ...                    Civ/leader balance run: N 1v1 matches with
//                                    the SAME shipped AI on both sides and a
//                                    randomly drawn civ + leader per side, so
//                                    the only thing varying is the matchup.
//                                    Reports win rate by civ, by leader and by
//                                    map-derived AI strategy (each with a 95%
//                                    Wilson interval, since a 1000-match run
//                                    only puts ~35 games in each of the 27
//                                    civ/leader cells), plus the produced-unit
//                                    breakdown. Honours --max-pop, --map-size,
//                                    --map-type, --difficulty, --ticks, --out,
//                                    --jobs, --seed, --verbose. Contrast with
//                                    --tournament, which holds the matchup
//                                    EQUAL and varies the AI instead.
#include "sim/command.h"
#include "sim/match.h"
#include "sim/world.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace ww::sim;

// Multiplayer self-test modes, kept in their own translation unit so the socket
// headers stay out of this one (see nettest.cpp).
int run_nettest(int turns_to_play);
int run_upnp_probe();

namespace {
uint64_t parse_u64(const char* s) { return std::strtoull(s, nullptr, 10); }

void run_checksum_mode(uint64_t seed, int players, int ticks, double dt, int print_every) {
    SkirmishSettings settings;
    settings.n_players = players; // civs left empty -> new_skirmish's built-in default assignment
    if (std::getenv("WW_LAND")) settings.water = false; // opt-in land map (AI/economy testing)
    if (const char* m = std::getenv("WW_MAP")) settings.map_type = m; // themed map testing

    Match match(seed, settings);
    std::printf("tick,checksum\n");
    std::printf("%llu,%llu\n", 0ull, static_cast<unsigned long long>(match.checksum()));
    for (int t = 1; t <= ticks; ++t) {
        match.step(dt);
        match.events().clear(); // headless: no client to consume them
        if (t % print_every == 0 || t == ticks) {
            std::printf("%d,%llu\n", t, static_cast<unsigned long long>(match.checksum()));
        }
    }
#ifdef WW_AI_DUMP
    {
        World& world = match.world();
        Control& control = match.control();
        std::map<int, std::map<std::string, int>> units_by_team, blds_by_team;
        for (auto ref : world.active_units) {
            Unit* u = world.get(ref);
            if (u && u->common.alive && u->common.team >= 0) units_by_team[u->common.team][u->name]++;
        }
        for (auto ref : world.active_buildings) {
            Building* b = world.get_building(ref);
            if (b && b->common.alive && b->common.team >= 0) blds_by_team[b->common.team][b->name]++;
        }
        for (int tm = 0; tm < control.n; ++tm) {
            Team& td = control.teams[tm];
            std::printf("TEAM %d ai=%d era=%d techs=%d | res food=%.0f wood=%.0f oil=%.0f iron=%.0f\n",
                        tm, (int)td.is_ai, td.era, (int)td.tech.size(), td.res["food"], td.res["wood"],
                        td.res["oil"], td.res["iron"]);
            std::printf("  buildings:");
            for (auto& [n, c] : blds_by_team[tm]) std::printf(" %s=%d", n.c_str(), c);
            std::printf("\n  units:");
            for (auto& [n, c] : units_by_team[tm]) std::printf(" %s=%d", n.c_str(), c);
            std::printf("\n");
        }
    }
#endif
}

// Direct port of golden_record.py's build_fixture() -- see
// game/tests/golden/fixture1.md for why spawn order/coordinates
// matter (ids must line up, and every spawn must clear the base's
// footprint).
std::unique_ptr<World> build_fixture1(const DataStore& data, const Bonuses& bonuses, Control& control,
                                      Rng& rng, EventBus& events) {
    control.teams[0].civ = 0;
    control.teams[1].civ = 2;
    control.teams[0].is_ai = false;
    control.teams[1].is_ai = false;

    auto world = std::make_unique<World>(data, bonuses, control, rng, events, 60, 60, "random",
                                         /*water=*/false);
    world->spawn_building("base", 0, 600, 600);  // id 1
    world->spawn_unit("civilian", 0, 750, 600);   // id 2
    world->spawn_resource("tree", 790, 600);      // id 3
    world->spawn_unit("rifleman", 0, 600, 750);   // id 4
    world->spawn_building("base", 1, 1200, 600);  // id 5
    world->spawn_unit("rifleman", 1, 1200, 750);  // id 6
    world->prime();
    return world;
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) out.push_back(field);
    return out;
}

std::map<int, std::vector<Command>> load_commands(const std::string& path) {
    std::map<int, std::vector<Command>> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto p = split_csv(line);
        int tick = std::atoi(p[0].c_str());
        const std::string& kind = p[1];
        Command cmd;
        if (kind == "move") {
            cmd = MoveCommand{static_cast<uint32_t>(std::atoi(p[2].c_str())), std::atof(p[3].c_str()),
                              std::atof(p[4].c_str())};
        } else if (kind == "gather") {
            cmd = GatherCommand{static_cast<uint32_t>(std::atoi(p[2].c_str())),
                                static_cast<uint32_t>(std::atoi(p[3].c_str()))};
        } else if (kind == "attack") {
            cmd = AttackCommand{static_cast<uint32_t>(std::atoi(p[2].c_str())),
                                static_cast<uint32_t>(std::atoi(p[3].c_str()))};
        } else if (kind == "place") {
            cmd = PlaceBuildingCommand{std::atoi(p[2].c_str()), p[3], std::atof(p[4].c_str()),
                                      std::atof(p[5].c_str())};
        } else if (kind == "enqueue") {
            cmd = EnqueueCommand{static_cast<uint32_t>(std::atoi(p[2].c_str())), p[3]};
        } else if (kind == "research") {
            cmd = ResearchCommand{std::atoi(p[2].c_str()), p[3]};
        } else if (kind == "trade") {
            cmd = TradeCommand{std::atoi(p[2].c_str()), p[3], p[4]};
        } else {
            std::fprintf(stderr, "unknown command: %s\n", kind.c_str());
            continue;
        }
        out[tick].push_back(cmd);
    }
    return out;
}

const int TRACKED_IDS[] = {2, 4, 6};

std::string snapshot(int tick, World& world, Control& control) {
    std::ostringstream out;
    out << tick;
    for (int t = 0; t < 2; ++t) {
        Team& team = control.teams[t];
        auto res = [&](const char* k) { auto it = team.res.find(k); return it == team.res.end() ? 0.0 : it->second; };
        out << "," << std::fixed << std::setprecision(3) << res("food") << "," << res("wood") << ","
            << res("oil") << "," << res("iron") << "," << team.era << "," << team.pop;
    }
    for (int id : TRACKED_IDS) {
        EntityRef ref = world.find_by_id(static_cast<uint32_t>(id));
        Unit* u = world.get(ref);
        if (!u) {
            out << ",dead,dead,dead,0";
        } else {
            out << "," << std::fixed << std::setprecision(3) << u->common.x << "," << u->common.y << ","
                << u->common.hp << "," << (u->common.alive ? 1 : 0);
        }
    }
    return out.str();
}

void run_fixture_mode(const std::string& commands_path, int ticks, double dt, int snapshot_every) {
    DataStore data(WW_DATA_DIR);
    Bonuses bonuses(data);
    Control control(data, bonuses, 2, 100);
    Rng rng(1); // fixture spawns are fixed/RNG-free; nothing in fixture 1 draws from this
    EventBus events;
    auto world = build_fixture1(data, bonuses, control, rng, events);

    auto commands = load_commands(commands_path);

    std::printf("%s\n", snapshot(0, *world, control).c_str());
    for (int t = 1; t <= ticks; ++t) {
        auto it = commands.find(t - 1);
        if (it != commands.end()) {
            for (auto& cmd : it->second) apply_command(*world, cmd);
        }
        world->update(dt);
        events.clear();
        if (t % snapshot_every == 0 || t == ticks) {
            std::printf("%s\n", snapshot(t, *world, control).c_str());
        }
    }
}

// ---- AI self-play arena (--tournament) ----

struct TeamMetrics {
    int era = 0;
    int pop = 0;
    double score = 0.0;
    double food_gathered = 0.0, wood_gathered = 0.0, oil_gathered = 0.0, iron_gathered = 0.0;
    double idle_tc_seconds = 0.0, idle_villager_seconds = 0.0;
    int military_units_created = 0;
    int units_lost = 0, buildings_lost = 0;
    bool had_base = false; // still alive (has_base) at the point this was captured
    int peak_army_size = 0, peak_vil_count = 0;
    int bases_built = 0, shipyards_built = 0, airbases_built = 0;
    // Filled by run_one_match's per-tick era watch (not capture_metrics): the
    // sim-time this team first reached Industrial (era>=1), and how many
    // civilians it had at that exact moment. -1 time = never advanced.
    double industrial_time_s = -1.0;
    double war_time_s = -1.0;        // sim-time reached War (era 2), -1 = never
    double sci_time_s = -1.0;        // sim-time reached Scientific (era 3), -1 = never
    int industrial_vils = 0;
    double industrial_idle_tc = 0.0; // idle town-centre time accumulated by the age-up moment
    // Live count of age-qualifying buildings (barracks/academy/market/refinery/
    // shipyard -- the Industrial prerequisite set, see World::can_age_up) at
    // match end. <2 means the team physically couldn't age no matter how much
    // food it banked -- the "boomed but never advanced" diagnosis.
    int age_qual = 0;
    int farms_live = 0, farms_exhausted = 0; // farm buildings the team ended with
    int fishing_boats = 0;                   // live fishing boats at match end (naval-food usage)
    int war_bldgs = 0;                       // live factory+university+airbase (War prereqs)
    double food_now = 0.0, oil_now = 0.0;    // CURRENT stockpile at match end (not lifetime gathered)
    int can_age_next = 0;                     // 1 if can_age_up(next era) is satisfied right now
    int age_in_queue = 0;                     // 1 if an AGE_ITEM is sitting in a base queue at match end
    int base_qlen = 0;                        // longest base production queue at match end
};

TeamMetrics capture_metrics(const Team& t) {
    TeamMetrics m;
    m.era = t.era;
    m.pop = t.pop;
    m.score = t.score;
    auto res = [&](const char* k) {
        auto it = t.total_gathered.find(k);
        return it == t.total_gathered.end() ? 0.0 : it->second;
    };
    m.food_gathered = res("food");
    m.wood_gathered = res("wood");
    m.oil_gathered = res("oil");
    m.iron_gathered = res("iron");
    auto now = [&](const char* k) {
        auto it = t.res.find(k);
        return it == t.res.end() ? 0.0 : it->second;
    };
    m.food_now = now("food");
    m.oil_now = now("oil");
    m.idle_tc_seconds = t.idle_tc_seconds;
    m.idle_villager_seconds = t.idle_villager_seconds;
    m.military_units_created = t.military_units_created;
    m.units_lost = t.units_lost;
    m.buildings_lost = t.buildings_lost;
    m.had_base = t.has_base;
    m.peak_army_size = t.peak_army_size;
    m.peak_vil_count = t.peak_vil_count;
    m.bases_built = t.bases_built;
    m.shipyards_built = t.shipyards_built;
    m.airbases_built = t.airbases_built;
    return m;
}

struct MatchResult {
    int ticks_used = 0;
    bool timed_out = false;
    std::optional<int> winner_variant; // 0 = baseline won, candidate_variant = candidate won, unset = draw/timeout
    TeamMetrics baseline, candidate;
};

// One full 1v1 match: team `cand_team` (0 or 1) is tagged ai_variant =
// candidate_variant, the other stays ai_variant = 0 (baseline). Both teams
// are forced AI-controlled (team 0 is human-only by convention elsewhere in
// the sim, not here) and given identical difficulty/civs/resources -- the
// ONLY thing ever allowed to differ between the two sides is ai_variant,
// so a win margin actually reflects the candidate logic, not a resource or
// difficulty edge (see the "no cheating with extra resources" design goal).
// --dump-maps <dir>: when set, run_one_match writes a BMP of any match where a
// team is still stuck at era 0 (never reached Industrial), so the pathological
// spawns can be eyeballed. Empty = disabled.
static std::string g_dump_map_dir;
// --trace-seed <seed>: emit a per-5-min trajectory (gathered food/wood, villager
// food/wood assignment split, farms building) for the match with this seed. 0 =
// disabled.
static uint64_t g_trace_seed = 0;

// Minimal 24-bit BMP writer (no libs). `rgb` is W*H*3, row 0 = TOP; BMP stores
// rows bottom-up as BGR, each row zero-padded to a 4-byte boundary.
static void write_bmp(const std::string& path, int W, int H, const std::vector<uint8_t>& rgb) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    int rowbytes = W * 3;
    int pad = (4 - (rowbytes % 4)) % 4;
    int imgsize = (rowbytes + pad) * H;
    auto u16 = [&](int v) { f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF)); };
    auto u32 = [&](int v) {
        f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF));
        f.put((char)((v >> 16) & 0xFF)); f.put((char)((v >> 24) & 0xFF));
    };
    f.put('B'); f.put('M'); u32(54 + imgsize); u32(0); u32(54);
    u32(40); u32(W); u32(H); u16(1); u16(24); u32(0); u32(imgsize); u32(2835); u32(2835); u32(0); u32(0);
    char padbytes[4] = {0, 0, 0, 0};
    for (int y = H - 1; y >= 0; --y) {
        for (int x = 0; x < W; ++x) {
            int idx = (y * W + x) * 3;
            f.put((char)rgb[idx + 2]); f.put((char)rgb[idx + 1]); f.put((char)rgb[idx + 0]); // BGR
        }
        if (pad) f.write(padbytes, pad);
    }
}

// Render the whole map: water/land terrain, every resource (berry=red,
// wood=dark-green, oil=black, iron=steel, fish=cyan, deer=orange), and every
// building (the STUCK team's base = red, a healthy base = white, farms = lime,
// other buildings = gold). One terrain tile -> an SC x SC pixel block.
static void dump_map_bmp(Match& match, Control& control, const std::string& path) {
    World& world = match.world();
    const int SC = 5;
    int W = world.cols * SC, H = world.rows * SC;
    std::vector<uint8_t> buf(static_cast<size_t>(W) * H * 3, 0);
    auto put = [&](int tx, int ty, uint8_t r, uint8_t g, uint8_t b, int span) {
        for (int dy = 0; dy < span; ++dy)
            for (int dx = 0; dx < span; ++dx) {
                int px = tx * SC + dx, py = ty * SC + dy;
                if (px < 0 || py < 0 || px >= W || py >= H) continue;
                int idx = (py * W + px) * 3;
                buf[idx] = r; buf[idx + 1] = g; buf[idx + 2] = b;
            }
    };
    for (int x = 0; x < world.cols; ++x)
        for (int y = 0; y < world.rows; ++y) {
            if (world.terrain[x][y] == WATER) put(x, y, 60, 120, 200, SC);
            else put(x, y, 120, 165, 95, SC);
        }
    for (auto ref : world.active_resources) {
        Resource* r = world.get_resource(ref);
        if (!r || !r->common.alive) continue;
        int tx = (int)(r->common.x / TILE), ty = (int)(r->common.y / TILE);
        if (r->name == "berry") put(tx, ty, 220, 40, 40, SC);
        else if (r->name == "oil") put(tx, ty, 25, 25, 25, SC);
        else if (r->name == "iron") put(tx, ty, 160, 160, 180, SC);
        else if (r->name == "fish") put(tx, ty, 40, 220, 230, SC);
        else put(tx, ty, 25, 90, 25, SC); // tree/palm = wood
    }
    for (auto ref : world.active_deer) {
        EntityCommon* c = world.common(ref);
        if (!c || !c->alive) continue;
        put((int)(c->x / TILE), (int)(c->y / TILE), 230, 140, 40, SC);
    }
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive) continue;
        int tx = (int)(b->common.x / TILE), ty = (int)(b->common.y / TILE);
        if (b->name == "base") {
            bool stuck = control.teams[b->common.team].era == 0;
            if (stuck) put(tx - 1, ty - 1, 255, 40, 40, SC * 3);
            else put(tx - 1, ty - 1, 255, 255, 255, SC * 3);
        } else if (b->name == "farm") {
            put(tx, ty, 180, 240, 90, SC);
        } else {
            put(tx, ty, 235, 200, 60, SC);
        }
    }
    write_bmp(path, W, H, buf);
}

// Map-thumbnail preview: the terrain + resources of a freshly-generated match,
// with each team's STARTING base drawn as a big block in its side colour (team 0
// blue = the human, team 1 red = the enemy). Used to bake the menu's bg_map_<key>
// minimap thumbnails (see --preview-map).
static void dump_preview_bmp(Match& match, Control& control, const std::string& path) {
    World& world = match.world();
    const int SC = 5;
    int W = world.cols * SC, H = world.rows * SC;
    std::vector<uint8_t> buf(static_cast<size_t>(W) * H * 3, 0);
    auto put = [&](int tx, int ty, uint8_t r, uint8_t g, uint8_t b, int span) {
        for (int dy = 0; dy < span; ++dy)
            for (int dx = 0; dx < span; ++dx) {
                int px = tx * SC + dx, py = ty * SC + dy;
                if (px < 0 || py < 0 || px >= W || py >= H) continue;
                int idx = (py * W + px) * 3;
                buf[idx] = r; buf[idx + 1] = g; buf[idx + 2] = b;
            }
    };
    for (int x = 0; x < world.cols; ++x)
        for (int y = 0; y < world.rows; ++y) {
            if (world.terrain[x][y] == WATER) put(x, y, 55, 105, 185, SC);
            else put(x, y, 118, 162, 92, SC);
        }
    for (auto ref : world.active_resources) {
        Resource* r = world.get_resource(ref);
        if (!r || !r->common.alive) continue;
        int tx = (int)(r->common.x / TILE), ty = (int)(r->common.y / TILE);
        if (r->name == "berry") put(tx, ty, 175, 65, 190, SC);   // purple, not red (red = enemy base)
        else if (r->name == "oil") put(tx, ty, 20, 20, 20, SC);
        else if (r->name == "iron") put(tx, ty, 165, 165, 185, SC);
        else if (r->name == "fish") put(tx, ty, 45, 215, 225, SC);
        else put(tx, ty, 30, 95, 30, SC); // tree/palm = wood
    }
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive || b->name != "base") continue;
        int tx = (int)(b->common.x / TILE), ty = (int)(b->common.y / TILE);
        // team 0 = blue (you), team 1 = red (enemy), others cyan/yellow just in case.
        if (b->common.team == 0) put(tx - 2, ty - 2, 40, 90, 245, SC * 5);
        else if (b->common.team == 1) put(tx - 2, ty - 2, 235, 45, 45, SC * 5);
        else put(tx - 2, ty - 2, 235, 210, 60, SC * 5);
    }
    (void)control;
    write_bmp(path, W, H, buf);
}

// --preview-map <path> (+ WW_MAP): generate one skirmish and immediately dump a
// starting-position minimap thumbnail. No ticks are run, so bases/resources are
// exactly the opening layout.
static void run_preview_mode(uint64_t seed, int players, const std::string& out) {
    SkirmishSettings settings;
    settings.n_players = players;
    if (const char* m = std::getenv("WW_MAP")) settings.map_type = m;
    if (std::getenv("WW_LAND")) settings.water = false;
    Match match(seed, settings);
    dump_preview_bmp(match, match.control(), out);
}

// Per-villager end-state trace for a team stuck at era 0: what is each civilian
// actually doing (target kind/type, carry, path length, stall strikes) and how
// far is the nearest farm / berry. Distinguishes "mis-assigned" from "assigned
// but can't path there" (high stall_strikes / empty path).
static void trace_stuck_villagers(Match& match, Control& control, uint64_t seed, int team) {
    World& world = match.world();
    Building* homebase = nullptr;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (b && b->common.alive && b->common.team == team && b->name == "base") { homebase = b; break; }
    }
    auto nearest_of = [&](double x, double y, auto pred) {
        double best = 1e18;
        for (auto ref : world.active_resources) {
            Resource* r = world.get_resource(ref);
            if (!r || !r->common.alive || !pred(r)) continue;
            double d = std::hypot(r->common.x - x, r->common.y - y);
            if (d < best) best = d;
        }
        return best;
    };
    auto nearest_farm = [&](double x, double y) {
        double best = 1e18;
        for (auto ref : world.active_buildings) {
            Building* b = world.get_building(ref);
            if (!b || !b->common.alive || !b->complete || b->common.team != team) continue;
            if (b->name != "farm" || b->exhausted) continue;
            double d = std::hypot(b->common.x - x, b->common.y - y);
            if (d < best) best = d;
        }
        return best;
    };
    std::fprintf(stderr, "=== STUCK team %d, seed %llu (base tile %d,%d) ===\n", team,
                 (unsigned long long)seed, homebase ? (int)(homebase->common.x / TILE) : -1,
                 homebase ? (int)(homebase->common.y / TILE) : -1);
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (!u || !u->common.alive || u->common.team != team || u->name != "civilian") continue;
        const char* tgt = "none";
        int tt_tx = -1, tt_ty = -1;
        double tdist = -1;
        if (u->gather_target.valid()) {
            if (u->gather_target.kind == EntityKind::Building) {
                tgt = "farm";
                if (EntityCommon* c = world.common(u->gather_target)) {
                    tt_tx = (int)(c->x / TILE); tt_ty = (int)(c->y / TILE);
                    tdist = std::hypot(c->x - u->common.x, c->y - u->common.y) / TILE;
                }
            } else if (Resource* r = world.get_resource(u->gather_target)) {
                static const char* rn[4] = {"berry", "wood", "oil", "iron"};
                tgt = (r->res.rtype >= 0 && r->res.rtype < 4) ? rn[r->res.rtype] : "res";
                tt_tx = (int)(r->common.x / TILE); tt_ty = (int)(r->common.y / TILE);
                tdist = std::hypot(r->common.x - u->common.x, r->common.y - u->common.y) / TILE;
            }
        }
        std::fprintf(stderr,
                     "  vil tile(%3d,%3d) carry=%.0f(t%d) target=%-5s@(%3d,%3d) d=%.1ft path=%zu need_path=%d "
                     "stall=%d build=%d | nearestFarm=%.1ft nearestBerry=%.1ft nearestWood=%.1ft\n",
                     (int)(u->common.x / TILE), (int)(u->common.y / TILE), u->carry, u->carry_type, tgt,
                     tt_tx, tt_ty, tdist, u->path.size(), (int)u->need_path, u->stall_strikes,
                     (int)u->build_target.valid(), nearest_farm(u->common.x, u->common.y) / TILE,
                     nearest_of(u->common.x, u->common.y, [](Resource* r) { return r->name == "berry"; }) / TILE,
                     nearest_of(u->common.x, u->common.y, [](Resource* r) { return r->res.rtype == 1; }) / TILE);
    }
}

MatchResult run_one_match(uint64_t seed, int cand_team, int candidate_variant, int max_ticks, double dt,
                          int difficulty, int civ_a, int civ_b, bool allied, const std::string& map_type,
                          int max_pop, int map_size) {
    SkirmishSettings settings;
    settings.n_players = 2;
    settings.difficulty = difficulty;
    settings.civs = {civ_a, civ_b};
    settings.max_pop = max_pop;
    settings.map_size = map_size;
    // Same reason the arena sets this: only team 0 is fog-restricted when
    // placing buildings (World::place_building), which handicaps whichever side
    // it is. --tournament alternates cand_team every match so the effect
    // cancels on AVERAGE, but it still adds variance the A/B has to see
    // through, and a candidate that changes BUILD PLACEMENT interacts with it
    // directly. Revealed removes it outright.
    settings.reveal_mode = 2;
    if (!map_type.empty()) settings.map_type = map_type;
    Match match(seed, settings);
    Control& control = match.control();
    control.teams[0].is_ai = true; // team 0 is human-only by SkirmishSettings default; both sides are AI here
    control.teams[1].is_ai = true;
    // new_skirmish only turns on the map-derived skirmish AI for teams i!=0 (in
    // a real game team 0 is the human). The tournament makes team 0 an AI too,
    // so opt it in as well -- otherwise team 0 silently runs the fallback
    // (non-map-derive) economy and every measured number is half polluted.
    control.teams[0].ai_map_derive = true;
    control.teams[1].ai_map_derive = true;
    control.teams[0].ai_variant = (cand_team == 0) ? candidate_variant : 0;
    control.teams[1].ai_variant = (cand_team == 1) ? candidate_variant : 0;
    // Allied mode (--allied): put both AIs on the SAME team so they never fight
    // -- a pure economic-development test (how consistently/fast they reach an
    // age with no combat pressure). check_win flips game_over immediately (one
    // alliance left standing), so the run loop must NOT break on it in this mode.
    if (allied) {
        control.teams[0].ally = 0;
        control.teams[1].ally = 0;
    }

    // Per-team watch for the first Industrial (era>=1) advance: record the
    // sim-time and the team's live civilian count at that exact tick.
    int adv_tick[2] = {-1, -1};      // first tick each team reached Industrial (era>=1)
    int adv_war[2] = {-1, -1};       // ...War (era>=2)
    int adv_sci[2] = {-1, -1};       // ...Scientific (era>=3)
    int adv_vils[2] = {0, 0};
    double adv_idle[2] = {0.0, 0.0}; // idle_tc accumulated up to the industrial age-up moment
    auto count_vils = [&](int team) {
        int n = 0;
        for (auto ref : match.world().active_units) {
            Unit* u = match.world().get(ref);
            if (u && u->common.alive && u->common.team == team && u->name == "civilian") ++n;
        }
        return n;
    };

    MatchResult result;
    for (int t = 0; t < max_ticks; ++t) {
        match.step(dt);
        match.events().clear();
        result.ticks_used = t + 1;
        for (int tm = 0; tm < 2; ++tm) {
            int era = control.teams[tm].era;
            if (adv_tick[tm] < 0 && era >= 1) {
                adv_tick[tm] = t + 1;
                adv_vils[tm] = count_vils(tm);
                adv_idle[tm] = control.teams[tm].idle_tc_seconds; // idle TC time up to aging
            }
            if (adv_war[tm] < 0 && era >= 2) adv_war[tm] = t + 1;
            if (adv_sci[tm] < 0 && era >= 3) adv_sci[tm] = t + 1;
        }
        if (g_trace_seed != 0 && seed == g_trace_seed && (t % 6000) == 0) {
            for (int tm = 0; tm < 2; ++tm) {
                int nfood = 0, nwood = 0, noil = 0, niron = 0, nbuild = 0, nidle = 0, nciv = 0;
                for (auto ref : match.world().active_units) {
                    Unit* u = match.world().get(ref);
                    if (!u || !u->common.alive || u->common.team != tm || u->name != "civilian") continue;
                    ++nciv;
                    if (u->build_target.valid()) { ++nbuild; continue; }
                    int rt = -1;
                    if (u->gather_target.valid()) {
                        if (u->gather_target.kind == EntityKind::Building) rt = 0;
                        else if (Resource* r = match.world().get_resource(u->gather_target)) rt = r->res.rtype;
                    }
                    if (rt < 0) rt = u->gather_rtype;
                    if (rt == 0) ++nfood; else if (rt == 1) ++nwood; else if (rt == 2) ++noil;
                    else if (rt == 3) ++niron; else ++nidle;
                }
                auto g = [&](const char* k) {
                    auto it = control.teams[tm].total_gathered.find(k);
                    return it == control.teams[tm].total_gathered.end() ? 0.0 : it->second;
                };
                std::fprintf(stderr,
                             "[t=%5d %2dmin] team%d era=%d civ=%d gath(f/w)=%.0f/%.0f split f=%d w=%d o=%d "
                             "i=%d build=%d idle=%d food=%.0f\n",
                             t, t / 1200, tm, control.teams[tm].era, nciv, g("food"), g("wood"), nfood, nwood,
                             noil, niron, nbuild, nidle, control.teams[tm].res["food"]);
            }
        }
        if (!allied && control.game_over) break; // allied: run the full cap (game_over fires at once)
    }
    result.timed_out = !control.game_over;

    int base_team = cand_team == 0 ? 1 : 0;
    result.baseline = capture_metrics(control.teams[base_team]);
    result.candidate = capture_metrics(control.teams[cand_team]);
    auto age_qual_count = [&](int team) {
        static const std::set<std::string> qual = {"barracks", "academy", "market", "refinery", "shipyard"};
        int n = 0;
        for (auto ref : match.world().active_buildings) {
            Building* b = match.world().get_building(ref);
            if (b && b->common.alive && b->complete && b->common.team == team && qual.count(b->name)) ++n;
        }
        return n;
    };
    auto set_adv = [&](TeamMetrics& m, int team) {
        m.industrial_time_s = adv_tick[team] >= 0 ? adv_tick[team] * dt : -1.0;
        m.war_time_s = adv_war[team] >= 0 ? adv_war[team] * dt : -1.0;
        m.sci_time_s = adv_sci[team] >= 0 ? adv_sci[team] * dt : -1.0;
        m.industrial_vils = adv_vils[team];
        m.industrial_idle_tc = adv_idle[team];
        m.age_qual = age_qual_count(team);
        for (auto ref : match.world().active_buildings) {
            Building* b = match.world().get_building(ref);
            if (!b || !b->common.alive || b->common.team != team || b->name != "farm") continue;
            if (b->exhausted) ++m.farms_exhausted; else ++m.farms_live;
        }
        for (auto ref : match.world().active_units) {
            Unit* u = match.world().get(ref);
            if (u && u->common.alive && u->common.team == team && u->name == "fishing boat")
                ++m.fishing_boats;
        }
        for (auto ref : match.world().active_buildings) {
            Building* b = match.world().get_building(ref);
            if (!b || !b->common.alive || !b->complete || b->common.team != team) continue;
            if (b->name == "factory" || b->name == "university" || b->name == "airbase") ++m.war_bldgs;
        }
        m.can_age_next = match.world().can_age_up(team) ? 1 : 0;
        static const std::set<std::string> age_items = {"industrial", "war", "scientific"};
        for (auto ref : match.world().active_buildings) {
            Building* b = match.world().get_building(ref);
            if (!b || !b->common.alive || b->common.team != team) continue;
            if (b->name != "base" && b->name != "fortress") continue;
            m.base_qlen = std::max(m.base_qlen, (int)b->queue.size());
            for (auto& q : b->queue)
                if (age_items.count(q)) m.age_in_queue = 1;
        }
    };
    set_adv(result.baseline, base_team);
    set_adv(result.candidate, cand_team);
    // Snapshot the map of any spawn where a team never left the Victorian age --
    // the pathological "boxed-in / wood-only pocket" cases -- so they can be
    // eyeballed. Filename carries the seed so it's reproducible.
    if (!g_dump_map_dir.empty() && (control.teams[0].era == 0 || control.teams[1].era == 0)) {
        char p[600];
        std::snprintf(p, sizeof(p), "%s/failmap_seed%llu.bmp", g_dump_map_dir.c_str(),
                      (unsigned long long)seed);
        dump_map_bmp(match, control, p);
        if (control.teams[0].era == 0) trace_stuck_villagers(match, control, seed, 0);
        if (control.teams[1].era == 0) trace_stuck_villagers(match, control, seed, 1);
    }
    if (!result.timed_out && control.winner) {
        result.winner_variant = (*control.winner == cand_team) ? candidate_variant : 0;
    }
    // Timed-out/no-winner tie-break: a side's own `score` already reflects
    // its production (+10/unit trained, Team::score), and the OPPONENT's
    // own losses are this side's kills -- combine into one "power" figure
    // per side rather than leaving an inconclusive match a hard draw.
    if (!result.winner_variant) {
        double cand_power =
            result.candidate.score + result.baseline.units_lost * 10.0 + result.baseline.buildings_lost * 20.0;
        double base_power =
            result.baseline.score + result.candidate.units_lost * 10.0 + result.candidate.buildings_lost * 20.0;
        if (result.candidate.had_base && !result.baseline.had_base) result.winner_variant = candidate_variant;
        else if (result.baseline.had_base && !result.candidate.had_base) result.winner_variant = 0;
        else if (cand_power > base_power * 1.05) result.winner_variant = candidate_variant; // clear enough margin
        else if (base_power > cand_power * 1.05) result.winner_variant = 0;
        // else: genuinely too close to call -- stays a draw (nullopt)
    }
    return result;
}

void print_metrics_row(std::FILE* f, const char* label, const TeamMetrics& m) {
    char ind[64];
    if (m.industrial_time_s >= 0.0) {
        int secs = static_cast<int>(m.industrial_time_s);
        std::snprintf(ind, sizeof(ind), "industrial@%d:%02d(vils=%d,idleTC=%.0fs,warbldgs=%d)", secs / 60,
                      secs % 60, m.industrial_vils, m.industrial_idle_tc, m.war_bldgs);
    } else {
        std::snprintf(ind, sizeof(ind), "industrial@never(agebldgs=%d,farms=%d/exh%d)", m.age_qual,
                      m.farms_live, m.farms_exhausted);
    }
    std::fprintf(f, "  %-10s era=%d pop=%d score=%.0f now(food/oil)=%.0f/%.0f canage=%d ageQ=%d qlen=%d "
                    "gathered(food/wood/oil/iron)=%.0f/%.0f/%.0f/%.0f "
                    "idle_tc=%.1fs idle_vil=%.1fs mil_created=%d units_lost=%d buildings_lost=%d alive=%d "
                    "peak_army=%d peak_vil=%d bases=%d shipyards=%d airbases=%d %s\n",
                 label, m.era, m.pop, m.score, m.food_now, m.oil_now, m.can_age_next, m.age_in_queue, m.base_qlen,
                 m.food_gathered, m.wood_gathered, m.oil_gathered, m.iron_gathered,
                 m.idle_tc_seconds, m.idle_villager_seconds, m.military_units_created, m.units_lost,
                 m.buildings_lost, m.had_base ? 1 : 0, m.peak_army_size, m.peak_vil_count, m.bases_built,
                 m.shipyards_built, m.airbases_built, ind);
}

// Deterministic per-match setup derived purely from the match index, so a
// worker thread can rebuild it from `i` alone (no shared state) -- and the
// results are identical whatever order/parallelism the matches run in.
struct MatchPlan {
    uint64_t seed;
    int cand_team, civ_a, civ_b;
};
static MatchPlan plan_match(int i, uint64_t seed_start, bool vary_civs) {
    MatchPlan p;
    p.seed = seed_start + static_cast<uint64_t>(i);
    p.cand_team = i % 2; // alternate sides to cancel spawn-point asymmetry
    p.civ_a = 0;
    p.civ_b = 2;
    if (vary_civs) {
        p.civ_a = i % 9;
        p.civ_b = (i + 3) % 9;
        if (p.civ_b == p.civ_a) p.civ_b = (p.civ_b + 1) % 9;
    }
    return p;
}

void run_tournament_mode(int n_matches, int max_ticks, double dt, uint64_t seed_start, int difficulty,
                         int candidate_variant, bool vary_civs, const std::string& csv_path, int jobs,
                         bool allied, const std::string& map_type, int max_pop, int map_size) {
    // Each match is fully self-contained (its own Match/World/Control/Rng,
    // seeded from the match index) with no shared mutable state, so running
    // them across threads is a pure wall-clock speedup -- byte-identical
    // results, just computed concurrently. Printing/aggregation still happens
    // sequentially in match order afterward, so output is unchanged from the
    // single-threaded version.
    std::vector<MatchResult> results(n_matches);
    std::atomic<int> next_idx{0};
    auto worker = [&]() {
        int i;
        while ((i = next_idx.fetch_add(1)) < n_matches) {
            MatchPlan p = plan_match(i, seed_start, vary_civs);
            results[i] = run_one_match(p.seed, p.cand_team, candidate_variant, max_ticks, dt, difficulty,
                                       p.civ_a, p.civ_b, allied, map_type, max_pop, map_size);
        }
    };
    int nthreads = std::max(1, std::min(jobs, n_matches));
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    std::ofstream csv;
    if (!csv_path.empty()) {
        csv.open(csv_path);
        csv << "match,seed,cand_team,winner,ticks,timed_out,"
               "cand_era,cand_pop,cand_score,cand_food,cand_wood,cand_oil,cand_iron,cand_idle_tc,"
               "cand_idle_vil,cand_mil_created,cand_units_lost,cand_buildings_lost,cand_alive,"
               "cand_peak_army,cand_peak_vil,cand_bases,cand_shipyards,cand_airbases,"
               "base_era,base_pop,base_score,base_food,base_wood,base_oil,base_iron,base_idle_tc,"
               "base_idle_vil,base_mil_created,base_units_lost,base_buildings_lost,base_alive,"
               "base_peak_army,base_peak_vil,base_bases,base_shipyards,base_airbases\n";
    }

    int candidate_wins = 0, baseline_wins = 0, draws = 0;
    // Industrial-consistency aggregates across ALL team-instances (both sides;
    // identical logic today), for the "how often / how fast / how big" summary.
    int ind_reached = 0, ind_total = 0;
    double ind_time_sum = 0.0;
    int ind_vils_sum = 0;
    double ind_idle_sum = 0.0; // idle TC up to age-up, for the reached teams
    double idle_tc_sum = 0.0;  // avg idle town-centre time across all instances (whole match)
    int fished_teams = 0, fishing_sum = 0; // how many team-instances fished, and total boats
    int war_reached = 0, sci_reached = 0;
    double war_time_sum = 0.0, sci_time_sum = 0.0;
    for (int i = 0; i < n_matches; ++i) {
        MatchPlan p = plan_match(i, seed_start, vary_civs);
        uint64_t seed = p.seed;
        int cand_team = p.cand_team;
        const MatchResult& r = results[i];

        const char* outcome = "draw";
        if (r.winner_variant) {
            outcome = (*r.winner_variant == candidate_variant) ? "candidate" : "baseline";
            if (*r.winner_variant == candidate_variant) ++candidate_wins; else ++baseline_wins;
        } else {
            ++draws;
        }
        std::printf("match %d/%d  seed=%llu  cand_team=%d  ticks=%d%s  winner=%s\n", i + 1, n_matches,
                    static_cast<unsigned long long>(seed), cand_team, r.ticks_used,
                    r.timed_out ? " (timed out)" : "", outcome);
        print_metrics_row(stdout, "candidate", r.candidate);
        print_metrics_row(stdout, "baseline", r.baseline);
        for (const TeamMetrics* m : {&r.candidate, &r.baseline}) {
            ++ind_total;
            idle_tc_sum += m->idle_tc_seconds;
            if (m->fishing_boats > 0) ++fished_teams;
            fishing_sum += m->fishing_boats;
            if (m->industrial_time_s >= 0.0) {
                ++ind_reached;
                ind_time_sum += m->industrial_time_s;
                ind_vils_sum += m->industrial_vils;
                ind_idle_sum += m->industrial_idle_tc;
            }
            if (m->war_time_s >= 0.0) { ++war_reached; war_time_sum += m->war_time_s; }
            if (m->sci_time_s >= 0.0) { ++sci_reached; sci_time_sum += m->sci_time_s; }
        }

        if (csv.is_open()) {
            auto row = [&](const TeamMetrics& m) {
                csv << m.era << "," << m.pop << "," << m.score << "," << m.food_gathered << "," << m.wood_gathered
                    << "," << m.oil_gathered << "," << m.iron_gathered << "," << m.idle_tc_seconds << ","
                    << m.idle_villager_seconds << "," << m.military_units_created << "," << m.units_lost << ","
                    << m.buildings_lost << "," << (m.had_base ? 1 : 0) << "," << m.peak_army_size << ","
                    << m.peak_vil_count << "," << m.bases_built << "," << m.shipyards_built << ","
                    << m.airbases_built;
            };
            csv << (i + 1) << "," << seed << "," << cand_team << "," << outcome << "," << r.ticks_used << ","
                << (r.timed_out ? 1 : 0) << ",";
            row(r.candidate);
            csv << ",";
            row(r.baseline);
            csv << "\n";
        }
    }

    std::printf("\n==== tournament summary (%d matches) ====\n", n_matches);
    std::printf("candidate (variant %d): %d wins (%.1f%%)\n", candidate_variant, candidate_wins,
                100.0 * candidate_wins / n_matches);
    std::printf("baseline  (variant 0): %d wins (%.1f%%)\n", baseline_wins, 100.0 * baseline_wins / n_matches);
    std::printf("draws/inconclusive: %d (%.1f%%)\n", draws, 100.0 * draws / n_matches);
    std::printf("reached Industrial: %d/%d team-instances (%.1f%%)", ind_reached, ind_total,
                ind_total ? 100.0 * ind_reached / ind_total : 0.0);
    if (ind_reached > 0) {
        int avg_s = static_cast<int>(ind_time_sum / ind_reached);
        std::printf("  | avg advance %d:%02d, avg villagers %.1f, avg idleTC-to-age %.0fs", avg_s / 60,
                    avg_s % 60, static_cast<double>(ind_vils_sum) / ind_reached, ind_idle_sum / ind_reached);
    }
    if (ind_total > 0) std::printf("  | avg idle_tc(match) %.0fs", idle_tc_sum / ind_total);
    std::printf("\n");
    if (ind_total > 0)
        std::printf("fished: %d/%d team-instances (%.0f%%), %.1f boats avg (all), %.1f avg (fishers)\n",
                    fished_teams, ind_total, 100.0 * fished_teams / ind_total,
                    static_cast<double>(fishing_sum) / ind_total,
                    fished_teams ? static_cast<double>(fishing_sum) / fished_teams : 0.0);
    if (ind_total > 0) {
        auto age_line = [&](const char* name, int reached, double time_sum) {
            std::printf("reached %s: %d/%d (%.1f%%)", name, reached, ind_total,
                        100.0 * reached / ind_total);
            if (reached > 0) {
                int a = static_cast<int>(time_sum / reached);
                std::printf("  | avg advance %d:%02d", a / 60, a % 60);
            }
            std::printf("\n");
        };
        age_line("War", war_reached, war_time_sum);
        age_line("Scientific", sci_reached, sci_time_sum);
    }
    if (!csv_path.empty()) std::printf("per-match CSV written to %s\n", csv_path.c_str());
}

// ---- civ / leader balance arena (--arena) ----
//
// Unlike --tournament (which pits ai_variant 0 against a candidate variant and
// deliberately holds civ/difficulty EQUAL so the AI logic is the only variable),
// the arena runs the SAME shipped AI on both sides and randomises the matchup
// instead: each side draws its own civ and leader. The AI logic being identical
// is the whole point -- any spread that shows up in the win rates is then a
// property of the civ/leader bonuses (or of the map/spawn), not of two different
// brains playing.

struct ArenaSide {
    int civ = 0, leader = 0;
    std::string playstyle;      // AiPlan::playstyle -- the map-derived strategy
    int era = 0, pop = 0;
    double score = 0.0;
    int military_created = 0, units_lost = 0, buildings_lost = 0;
    int peak_army = 0, peak_vil = 0;
    int shipyards_built = 0, bases_built = 0, airbases_built = 0;
    int fishing_boats = 0;
    // Base compactness at match end: how many buildings the team still holds and
    // the mean distance (in tiles) from each to its NEAREST other own building.
    // This is the number the build-spacing change is supposed to move -- a base
    // welded edge-to-edge scores near the sum of two half-footprints (~2 tiles),
    // a base with walkable lanes scores meaningfully higher.
    int buildings_end = 0;
    double avg_nn_tiles = 0.0;
    // Buildings whose nearest own neighbour is under 3 tiles centre-to-centre.
    // Two 2-tile buildings placed flush sit exactly 2 tiles apart, and 3 tiles
    // is flush-plus-one-lane, so this counts the ones with NO walkable gap on
    // their tightest side. The mean above can't see this: a base can average a
    // healthy gap while still having a welded pocket that traps units, and it
    // is the pocket, not the average, that strands a villager.
    int tight_nn = 0;
    double industrial_s = -1.0, war_s = -1.0, sci_s = -1.0;
    bool alive = false;
    std::map<std::string, int> produced; // unit name -> finished-training count
};

struct ArenaResult {
    uint64_t seed = 0;
    int ticks = 0;
    bool timed_out = false;
    bool decided_in_sim = false; // true = a real Control::winner, false = scored on the tie-break
    int winner = -1;             // team index 0/1, or -1 for a draw
    ArenaSide side[2];
    // Map-level facts behind the shipyard gate (AiPlan::naval_viable). Both
    // teams measure the same map, so these are per-MATCH, read off team 0's
    // plan. Reported so it's visible how often the gate actually binds rather
    // than being taken on faith.
    double water_frac = 0.0;
    bool has_fish = false;
    bool naval_viable = false;
    int strategy_navy = 0; // Team::strategy == "navy" (the want_water map flag)
};

// SplitMix64 -- a self-contained PRNG for drawing the matchup. Deliberately NOT
// the sim's Rng: that one belongs to the match and drawing from it here would
// shift every terrain/resource roll downstream, so identical seeds would stop
// producing identical maps.
static uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct ArenaPlan {
    uint64_t seed;
    int civ[2], leader[2];
};

// Everything about match `i` derives from `i` alone (map seed AND matchup), so a
// worker thread rebuilds it with no shared state and the entire 1000-match run
// reproduces byte-for-byte from --seed regardless of --jobs.
static ArenaPlan plan_arena(int i, uint64_t seed_start, int n_civs, const std::vector<int>& leaders_per_civ) {
    ArenaPlan p;
    p.seed = seed_start + static_cast<uint64_t>(i);
    uint64_t s = p.seed * 0x2545F4914F6CDD1Dull + 0x9E3779B97F4A7C15ull;
    for (int k = 0; k < 2; ++k) {
        p.civ[k] = static_cast<int>(splitmix64(s) % static_cast<uint64_t>(n_civs));
        int nl = std::max(1, leaders_per_civ[p.civ[k]]);
        p.leader[k] = static_cast<int>(splitmix64(s) % static_cast<uint64_t>(nl));
    }
    return p;
}

ArenaResult run_arena_match(const ArenaPlan& plan, int max_ticks, double dt, int difficulty, int max_pop,
                            int map_size, const std::string& map_type) {
    SkirmishSettings settings;
    settings.n_players = 2;
    settings.difficulty = difficulty;
    settings.max_pop = max_pop;
    settings.map_size = map_size;
    settings.deathmatch = false; // "Standard" game mode
    settings.civs = {plan.civ[0], plan.civ[1]};
    settings.leaders = {plan.leader[0], plan.leader[1]};
    // Fog is only ever TRACKED for team 0 (it is the local player's), and
    // World::place_building refuses a team-0 placement whose footprint is still
    // unexplored -- a restriction no other team is under. In a real game that is
    // correct; in AI-vs-AI self-play it silently handicaps whichever side lands
    // on team 0. Measured across 320 matches before this was set: team 0 won
    // 38.1% vs team 1's 61.9%, with matching age-up times but a 23% smaller
    // peak army -- i.e. the sides diverged after the opening, exactly where
    // building placement starts to matter. Revealing the map equalises them
    // without giving either side information the AI logic actually reads
    // (nothing in control_ai.cpp consults fog).
    settings.reveal_mode = 2; // Revealed
    if (!map_type.empty()) settings.map_type = map_type;

    Match match(plan.seed, settings);
    Control& control = match.control();
    // new_skirmish leaves team 0 human-controlled and only opts teams i!=0 into
    // the map-derived skirmish AI. Both sides here must be the SAME brain, so
    // team 0 gets both flags explicitly -- otherwise it silently runs the
    // fallback economy and every civ that lands on team 0 is measured unfairly.
    for (int t = 0; t < 2; ++t) {
        control.teams[t].is_ai = true;
        control.teams[t].ai_map_derive = true;
        control.teams[t].ai_variant = 0; // shipped baseline logic on both sides
    }

    int adv_ind[2] = {-1, -1}, adv_war[2] = {-1, -1}, adv_sci[2] = {-1, -1};
    ArenaResult r;
    r.seed = plan.seed;
    for (int t = 0; t < max_ticks; ++t) {
        match.step(dt);
        match.events().clear();
        r.ticks = t + 1;
        for (int tm = 0; tm < 2; ++tm) {
            int era = control.teams[tm].era;
            if (adv_ind[tm] < 0 && era >= 1) adv_ind[tm] = t + 1;
            if (adv_war[tm] < 0 && era >= 2) adv_war[tm] = t + 1;
            if (adv_sci[tm] < 0 && era >= 3) adv_sci[tm] = t + 1;
        }
        if (control.game_over) break;
    }
    r.timed_out = !control.game_over;

    for (int tm = 0; tm < 2; ++tm) {
        const Team& td = control.teams[tm];
        ArenaSide& s = r.side[tm];
        s.civ = plan.civ[tm];
        s.leader = plan.leader[tm];
        s.playstyle = td.ai_plan.assessed ? td.ai_plan.playstyle : std::string("unassessed");
        s.era = td.era;
        s.pop = td.pop;
        s.score = td.score;
        s.military_created = td.military_units_created;
        s.units_lost = td.units_lost;
        s.buildings_lost = td.buildings_lost;
        s.peak_army = td.peak_army_size;
        s.peak_vil = td.peak_vil_count;
        s.shipyards_built = td.shipyards_built;
        s.bases_built = td.bases_built;
        s.airbases_built = td.airbases_built;
        s.alive = td.has_base;
        s.industrial_s = adv_ind[tm] >= 0 ? adv_ind[tm] * dt : -1.0;
        s.war_s = adv_war[tm] >= 0 ? adv_war[tm] * dt : -1.0;
        s.sci_s = adv_sci[tm] >= 0 ? adv_sci[tm] * dt : -1.0;
        for (const auto& [name, n] : td.units_created_by_name) s.produced[name] = n;
    }
    for (auto ref : match.world().active_units) {
        Unit* u = match.world().get(ref);
        if (u && u->common.alive && u->name == "fishing boat" && u->common.team >= 0 && u->common.team < 2)
            ++r.side[u->common.team].fishing_boats;
    }
    for (int tm = 0; tm < 2; ++tm) {
        std::vector<std::pair<double, double>> pts;
        for (auto ref : match.world().active_buildings) {
            Building* b = match.world().get_building(ref);
            if (b && b->common.alive && b->common.team == tm) pts.push_back({b->common.x, b->common.y});
        }
        r.side[tm].buildings_end = static_cast<int>(pts.size());
        if (pts.size() >= 2) {
            double sum = 0.0;
            int tight = 0;
            for (size_t i = 0; i < pts.size(); ++i) {
                double best = 1e18;
                for (size_t j = 0; j < pts.size(); ++j) {
                    if (i == j) continue;
                    best = std::min(best, std::hypot(pts[i].first - pts[j].first,
                                                     pts[i].second - pts[j].second));
                }
                sum += best;
                if (best < 3.0 * TILE) ++tight;
            }
            r.side[tm].avg_nn_tiles = sum / pts.size() / TILE;
            r.side[tm].tight_nn = tight;
        }
    }
    {
        const Team& t0 = control.teams[0];
        r.water_frac = t0.ai_plan.map_water_frac;
        r.has_fish = t0.ai_plan.map_has_fish;
        r.naval_viable = t0.ai_plan.naval_viable;
        r.strategy_navy = t0.strategy == "navy" ? 1 : 0;
    }

    if (!r.timed_out && control.winner && *control.winner >= 0 && *control.winner < 2) {
        r.winner = *control.winner;
        r.decided_in_sim = true;
        return r;
    }
    // Same tie-break --tournament uses for an undecided match: a side still
    // holding a base beats one that isn't, otherwise compare "power" (own
    // production score + the damage inflicted, which in a 1v1 IS the opponent's
    // own losses). Under 5% apart stays a genuine draw rather than being called.
    double p0 = r.side[0].score + r.side[1].units_lost * 10.0 + r.side[1].buildings_lost * 20.0;
    double p1 = r.side[1].score + r.side[0].units_lost * 10.0 + r.side[0].buildings_lost * 20.0;
    if (r.side[0].alive && !r.side[1].alive) r.winner = 0;
    else if (r.side[1].alive && !r.side[0].alive) r.winner = 1;
    else if (p0 > p1 * 1.05) r.winner = 0;
    else if (p1 > p0 * 1.05) r.winner = 1;
    return r;
}

// win/loss/draw tally for one grouping key (a civ, a civ+leader, a playstyle).
struct Record {
    int games = 0, wins = 0, losses = 0, draws = 0;
    double era_sum = 0.0, industrial_sum = 0.0;
    int industrial_n = 0;
    long long mil_created = 0, peak_army = 0;
    // Win rate counting a draw as half a win each way. With ~1/9 of matches
    // being civ mirrors (both sides random and independent) and a real draw
    // rate on top, ignoring draws entirely would distort the ordering.
    double win_pct() const { return games ? 100.0 * (wins + 0.5 * draws) / games : 0.0; }
};

static void add_record(Record& rec, const ArenaResult& r, int tm) {
    ++rec.games;
    if (r.winner == tm) ++rec.wins;
    else if (r.winner < 0) ++rec.draws;
    else ++rec.losses;
    rec.era_sum += r.side[tm].era;
    if (r.side[tm].industrial_s >= 0) { rec.industrial_sum += r.side[tm].industrial_s; ++rec.industrial_n; }
    rec.mil_created += r.side[tm].military_created;
    rec.peak_army += r.side[tm].peak_army;
}

static std::string mmss(double secs) {
    if (secs < 0) return "never";
    int s = static_cast<int>(secs);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

// Wilson score interval, 95% -- a sample-size-aware band on the true win rate.
// With 1000 matches spread over 27 civ/leader combinations some cells only hold
// ~35 games, where a raw 60% means very little; printing the interval keeps a
// small-sample outlier from reading as a balance finding.
//
// `wins` is the DRAW-ADJUSTED win count (wins + draws/2), i.e. the same
// quantity Record::win_pct reports. Feeding it raw wins while printing the
// adjusted rate puts the interval below its own point estimate on any row with
// draws -- and roughly a quarter of these matches draw.
static void wilson95(double wins, int games, double& lo, double& hi) {
    if (games <= 0) { lo = hi = 0.0; return; }
    const double z = 1.959964;
    double p = wins / games, n = games;
    double denom = 1.0 + z * z / n;
    double centre = p + z * z / (2 * n);
    double margin = z * std::sqrt(p * (1 - p) / n + z * z / (4 * n * n));
    lo = 100.0 * (centre - margin) / denom;
    hi = 100.0 * (centre + margin) / denom;
}

void run_arena_mode(int n_matches, int max_ticks, double dt, uint64_t seed_start, int difficulty, int max_pop,
                    int map_size, const std::string& map_type, const std::string& csv_path, int jobs,
                    bool verbose) {
    // Civ/leader names straight from data/civs.json, so the report reads as
    // "United Kingdom / Winston Churchill" rather than index numbers, and so
    // the draw automatically covers however many civs the data file defines.
    DataStore data(WW_DATA_DIR);
    const auto& civs_json = data.civs().at("civs");
    int n_civs = static_cast<int>(civs_json.size());
    std::vector<std::string> civ_names(n_civs);
    std::vector<std::vector<std::string>> leader_names(n_civs);
    std::vector<int> leaders_per_civ(n_civs, 1);
    for (int c = 0; c < n_civs; ++c) {
        civ_names[c] = civs_json[c].value("name", "civ" + std::to_string(c));
        if (civs_json[c].contains("leaders")) {
            for (const auto& l : civs_json[c].at("leaders"))
                leader_names[c].push_back(l.value("name", "leader"));
        }
        if (leader_names[c].empty()) leader_names[c].push_back("(default)");
        leaders_per_civ[c] = static_cast<int>(leader_names[c].size());
    }

    std::printf("arena: %d matches, 1v1 %s, map_size=%d, max_pop=%d, Standard, difficulty=%d,\n"
                "       random civ+leader per side, identical AI both sides, cap %d ticks (%d sim-min)\n\n",
                n_matches, map_type.empty() ? "random" : map_type.c_str(), map_size, max_pop, difficulty,
                max_ticks, static_cast<int>(max_ticks * dt / 60));
    std::fflush(stdout);

    std::vector<ArenaResult> results(n_matches);
    std::atomic<int> next_idx{0}, done{0};
    auto worker = [&]() {
        int i;
        while ((i = next_idx.fetch_add(1)) < n_matches) {
            results[i] = run_arena_match(plan_arena(i, seed_start, n_civs, leaders_per_civ), max_ticks, dt,
                                         difficulty, max_pop, map_size, map_type);
            int d = done.fetch_add(1) + 1;
            // Progress to stderr so a redirected stdout stays a clean report.
            if (d % 10 == 0 || d == n_matches)
                std::fprintf(stderr, "\r  %d/%d matches complete", d, n_matches), std::fflush(stderr);
        }
    };
    int nthreads = std::max(1, std::min(jobs, n_matches));
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
    std::fprintf(stderr, "\n");

    // Two CSVs: one row per match, plus a long-format one for the produced-unit
    // counts (variable width per match, so it can't live in the wide table).
    // Both are what a sharded run -- N single-threaded processes over disjoint
    // seed ranges -- gets merged from, so everything the summary computes is
    // recoverable from the files alone.
    std::ofstream csv, ucsv;
    if (!csv_path.empty()) {
        csv.open(csv_path);
        csv << "match,seed,ticks,sim_minutes,timed_out,decided_in_sim,winner_team,"
               "water_frac,has_fish,naval_viable,strategy_navy,"
               "civ_a,leader_a,style_a,era_a,score_a,mil_a,lost_a,peak_army_a,alive_a,shipyards_a,"
               "industrial_a,bldgs_a,nngap_a,tight_a,"
               "civ_b,leader_b,style_b,era_b,score_b,mil_b,lost_b,peak_army_b,alive_b,shipyards_b,"
               "industrial_b,bldgs_b,nngap_b,tight_b\n";
        ucsv.open(csv_path + ".units.csv");
        ucsv << "match,seed,side,civ,leader,unit,count\n";
    }

    std::map<int, Record> by_civ;
    std::map<std::pair<int, int>, Record> by_leader;
    std::map<std::string, Record> by_style;
    std::map<std::string, long long> produced_total;
    std::map<int, std::map<std::string, long long>> produced_by_civ;
    Record by_side[2];
    int decided = 0, timed_out = 0, mirrors = 0;
    long long tick_sum = 0;
    int shipyard_teams = 0, fishing_teams = 0;
    long long shipyard_total = 0;
    int era_hist[4] = {0, 0, 0, 0};
    int navy_maps = 0, viable_maps = 0, fish_maps = 0, gated_maps = 0;
    double water_sum = 0.0;

    for (int i = 0; i < n_matches; ++i) {
        const ArenaResult& r = results[i];
        if (r.decided_in_sim) ++decided;
        if (r.timed_out) ++timed_out;
        if (r.side[0].civ == r.side[1].civ) ++mirrors;
        tick_sum += r.ticks;
        water_sum += r.water_frac;
        if (r.strategy_navy) ++navy_maps;
        if (r.naval_viable) ++viable_maps;
        if (r.has_fish) ++fish_maps;
        // The gate BINDING means: the map told the AI to play navy, but the
        // map has neither fish nor real water -- exactly the case the change
        // suppresses. If this is 0, the new rule changed nothing on this map.
        if (r.strategy_navy && !r.naval_viable) ++gated_maps;
        for (int tm = 0; tm < 2; ++tm) {
            const ArenaSide& s = r.side[tm];
            add_record(by_civ[s.civ], r, tm);
            add_record(by_leader[{s.civ, s.leader}], r, tm);
            add_record(by_style[s.playstyle], r, tm);
            add_record(by_side[tm], r, tm);
            for (const auto& [name, n] : s.produced) {
                produced_total[name] += n;
                produced_by_civ[s.civ][name] += n;
            }
            if (s.shipyards_built > 0) ++shipyard_teams;
            shipyard_total += s.shipyards_built;
            if (s.fishing_boats > 0) ++fishing_teams;
            if (s.era >= 0 && s.era < 4) ++era_hist[s.era];
        }
        if (verbose) {
            std::printf("match %d/%d seed=%llu ticks=%d%s winner=%s | A %s/%s (%s) vs B %s/%s (%s)\n", i + 1,
                        n_matches, static_cast<unsigned long long>(r.seed), r.ticks,
                        r.timed_out ? " (timed out)" : "",
                        r.winner < 0 ? "draw" : (r.winner == 0 ? "A" : "B"), civ_names[r.side[0].civ].c_str(),
                        leader_names[r.side[0].civ][r.side[0].leader].c_str(), r.side[0].playstyle.c_str(),
                        civ_names[r.side[1].civ].c_str(),
                        leader_names[r.side[1].civ][r.side[1].leader].c_str(), r.side[1].playstyle.c_str());
        }
        if (csv.is_open()) {
            auto side_cols = [&](const ArenaSide& s) {
                csv << ",\"" << civ_names[s.civ] << "\",\"" << leader_names[s.civ][s.leader] << "\","
                    << s.playstyle << "," << s.era << "," << s.score << "," << s.military_created << ","
                    << s.units_lost << "," << s.peak_army << "," << (s.alive ? 1 : 0) << ","
                    << s.shipyards_built << "," << s.industrial_s << "," << s.buildings_end << ","
                    << s.avg_nn_tiles << "," << s.tight_nn;
            };
            csv << (i + 1) << "," << r.seed << "," << r.ticks << "," << (r.ticks * dt / 60.0) << ","
                << (r.timed_out ? 1 : 0) << "," << (r.decided_in_sim ? 1 : 0) << "," << r.winner << ","
                << r.water_frac << "," << (r.has_fish ? 1 : 0) << "," << (r.naval_viable ? 1 : 0) << ","
                << r.strategy_navy;
            side_cols(r.side[0]);
            side_cols(r.side[1]);
            csv << "\n";
            for (int tm = 0; tm < 2; ++tm) {
                const ArenaSide& s = r.side[tm];
                for (const auto& [name, n] : s.produced) {
                    ucsv << (i + 1) << "," << r.seed << "," << tm << ",\"" << civ_names[s.civ] << "\",\""
                         << leader_names[s.civ][s.leader] << "\",\"" << name << "\"," << n << "\n";
                }
            }
        }
    }

    auto rule = [&](const char* title) { std::printf("\n==== %s ====\n", title); };

    rule("run overview");
    std::printf("matches            : %d\n", n_matches);
    std::printf("decided in-sim     : %d (%.1f%%)   scored on tie-break: %d\n", decided,
                100.0 * decided / n_matches, n_matches - decided);
    std::printf("hit the time cap   : %d (%.1f%%)\n", timed_out, 100.0 * timed_out / n_matches);
    std::printf("draws              : %d (%.1f%%)\n", by_side[0].draws, 100.0 * by_side[0].draws / n_matches);
    std::printf("civ mirror matches : %d (%.1f%%)  -- both sides drew the same civ\n", mirrors,
                100.0 * mirrors / n_matches);
    std::printf("avg match length   : %s sim-time\n", mmss(tick_sum * dt / n_matches).c_str());
    std::printf("side bias (team 0) : %.1f%% win rate over %d games -- 50%% means spawn side is fair\n",
                by_side[0].win_pct(), by_side[0].games);
    std::printf("era reached (team-instances): Victorian %d, Industrial %d, War %d, Scientific %d\n",
                era_hist[0], era_hist[1], era_hist[2], era_hist[3]);
    std::printf("shipyards          : %d/%d team-instances built one (%.1f%%), %lld total\n", shipyard_teams,
                2 * n_matches, 100.0 * shipyard_teams / (2 * n_matches), shipyard_total);
    std::printf("fishing            : %d/%d team-instances ended with boats (%.1f%%)\n", fishing_teams,
                2 * n_matches, 100.0 * fishing_teams / (2 * n_matches));

    rule("shipyard gate (fish present OR map water > 20%)");
    std::printf("avg map water          : %.1f%% of tiles\n", 100.0 * water_sum / n_matches);
    std::printf("maps with fish         : %d/%d (%.1f%%)\n", fish_maps, n_matches,
                100.0 * fish_maps / n_matches);
    std::printf("maps passing the gate  : %d/%d (%.1f%%)\n", viable_maps, n_matches,
                100.0 * viable_maps / n_matches);
    std::printf("maps flagged navy      : %d/%d (%.1f%%)  -- Team::strategy from the want_water map flag\n",
                navy_maps, n_matches, 100.0 * navy_maps / n_matches);
    std::printf("maps the gate BLOCKS   : %d/%d (%.1f%%)  -- navy-flagged but dry: the new rule's effect\n",
                gated_maps, n_matches, 100.0 * gated_maps / n_matches);

    // ---- per-civ ----
    rule("win rate by civilisation");
    std::printf("%-20s %6s %6s %6s %6s %8s %16s %8s %9s\n", "civ", "games", "W", "L", "D", "win%",
                "95% CI", "avg era", "industrial");
    std::vector<std::pair<int, Record>> civ_rows(by_civ.begin(), by_civ.end());
    std::sort(civ_rows.begin(), civ_rows.end(),
              [](const auto& a, const auto& b) { return a.second.win_pct() > b.second.win_pct(); });
    for (const auto& [c, rec] : civ_rows) {
        double lo, hi;
        wilson95(rec.wins + 0.5 * rec.draws, rec.games, lo, hi);
        char ci[32];
        std::snprintf(ci, sizeof(ci), "%.1f-%.1f", lo, hi);
        std::printf("%-20s %6d %6d %6d %6d %7.1f%% %16s %8.2f %9s\n", civ_names[c].c_str(), rec.games,
                    rec.wins, rec.losses, rec.draws, rec.win_pct(), ci, rec.era_sum / std::max(1, rec.games),
                    mmss(rec.industrial_n ? rec.industrial_sum / rec.industrial_n : -1.0).c_str());
    }

    // ---- per-leader ----
    rule("win rate by leader");
    std::printf("%-20s %-24s %6s %6s %6s %6s %8s %16s\n", "civ", "leader", "games", "W", "L", "D", "win%",
                "95% CI");
    std::vector<std::pair<std::pair<int, int>, Record>> lead_rows(by_leader.begin(), by_leader.end());
    std::sort(lead_rows.begin(), lead_rows.end(),
              [](const auto& a, const auto& b) { return a.second.win_pct() > b.second.win_pct(); });
    for (const auto& [key, rec] : lead_rows) {
        double lo, hi;
        wilson95(rec.wins + 0.5 * rec.draws, rec.games, lo, hi);
        char ci[32];
        std::snprintf(ci, sizeof(ci), "%.1f-%.1f", lo, hi);
        std::printf("%-20s %-24s %6d %6d %6d %6d %7.1f%% %16s\n", civ_names[key.first].c_str(),
                    leader_names[key.first][key.second].c_str(), rec.games, rec.wins, rec.losses, rec.draws,
                    rec.win_pct(), ci);
    }

    // ---- per-strategy ----
    // The AI picks its playstyle from the map around its own base
    // (ai_assess_map), so this is "which map read wins", not a setting anyone
    // chose -- the styles are not drawn uniformly and the sample sizes differ.
    rule("win rate by AI strategy (map-derived playstyle)");
    std::printf("%-14s %6s %6s %6s %6s %8s %16s %11s %10s\n", "strategy", "games", "W", "L", "D", "win%",
                "95% CI", "avg mil made", "peak army");
    std::vector<std::pair<std::string, Record>> style_rows(by_style.begin(), by_style.end());
    std::sort(style_rows.begin(), style_rows.end(),
              [](const auto& a, const auto& b) { return a.second.win_pct() > b.second.win_pct(); });
    for (const auto& [name, rec] : style_rows) {
        double lo, hi;
        wilson95(rec.wins + 0.5 * rec.draws, rec.games, lo, hi);
        char ci[32];
        std::snprintf(ci, sizeof(ci), "%.1f-%.1f", lo, hi);
        std::printf("%-14s %6d %6d %6d %6d %7.1f%% %16s %11.1f %10.1f\n", name.c_str(), rec.games, rec.wins,
                    rec.losses, rec.draws, rec.win_pct(), ci,
                    static_cast<double>(rec.mil_created) / std::max(1, rec.games),
                    static_cast<double>(rec.peak_army) / std::max(1, rec.games));
    }

    // ---- production ----
    rule("units produced (all matches, both sides)");
    std::vector<std::pair<std::string, long long>> prod(produced_total.begin(), produced_total.end());
    std::sort(prod.begin(), prod.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    long long prod_sum = 0;
    for (const auto& [n, c] : prod) prod_sum += c;
    std::printf("%-24s %10s %8s %10s\n", "unit", "total", "share", "per match");
    for (const auto& [name, count] : prod) {
        std::printf("%-24s %10lld %7.1f%% %10.2f\n", name.c_str(), count,
                    prod_sum ? 100.0 * count / prod_sum : 0.0, static_cast<double>(count) / n_matches);
    }
    if (!prod.empty()) std::printf("\nmost-produced unit overall: %s\n", prod.front().first.c_str());
    for (const auto& [name, count] : prod) {
        if (name == "civilian" || name == "fishing boat") continue;
        std::printf("most-produced military unit: %s (%lld, %.2f per match)\n", name.c_str(), count,
                    static_cast<double>(count) / n_matches);
        break;
    }

    rule("most-produced military unit, per civ");
    std::printf("%-20s %-22s %10s\n", "civ", "top military unit", "total");
    for (int c = 0; c < n_civs; ++c) {
        std::vector<std::pair<std::string, long long>> pc(produced_by_civ[c].begin(), produced_by_civ[c].end());
        std::sort(pc.begin(), pc.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& [name, count] : pc) {
            if (name == "civilian" || name == "fishing boat") continue;
            std::printf("%-20s %-22s %10lld\n", civ_names[c].c_str(), name.c_str(), count);
            break;
        }
    }

    if (!csv_path.empty()) std::printf("\nper-match CSV written to %s\n", csv_path.c_str());
}

} // namespace

int main(int argc, char** argv) {
    uint64_t seed = 1;
    int players = 2;
    int ticks = -1;        // -1 == "not overridden"; each mode picks its own default below
    double dt = 1.0 / 20.0;
    int print_every = -1;  // -1 == "not overridden"
    std::string fixture_path;

    int tournament_matches = -1;
    int candidate_variant = 1;
    int tournament_difficulty = 1; // Normal by default -- both sides always get the SAME difficulty
    bool vary_civs = false;
    std::string tournament_csv;
    int jobs = 0; // 0 == auto (hardware_concurrency); parallel matches, identical results
    bool allied = false; // --allied: both AIs on the same team (pure economy test, no combat)
    std::string map_type; // empty => default "random"; else e.g. "guam"/"arabia"/"normandy"
    std::string preview_out; // --preview-map <path>: dump a starting-position minimap thumbnail

    int arena_matches = -1;   // --arena N: civ/leader balance run (see run_arena_mode)
    int arena_max_pop = 200;  // --max-pop: the menu's four choices are 50/100/150/200
    int arena_map_size = 64;  // --map-size: menu "Normal" (Tiny 48 / Normal 64 / Large 80 / Huge 96)
    bool arena_verbose = false; // --verbose: one line per match on stdout as well as the CSV

    // Multiplayer self-tests (see nettest.cpp). --nettest runs BOTH lockstep
    // peers in this process over a real loopback socket and asserts the two
    // independently-simulated worlds stay identical; --upnp just probes the
    // router and reports whether hosting will work on this network.
    int nettest_turns = -1;
    bool upnp_probe = false;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--seed")) seed = parse_u64(next());
        else if (!std::strcmp(argv[i], "--players")) players = std::atoi(next());
        else if (!std::strcmp(argv[i], "--ticks")) ticks = std::atoi(next());
        else if (!std::strcmp(argv[i], "--dt")) dt = std::atof(next());
        else if (!std::strcmp(argv[i], "--print-every") || !std::strcmp(argv[i], "--snapshot-every")) {
            print_every = std::atoi(next());
        } else if (!std::strcmp(argv[i], "--fixture")) {
            fixture_path = next();
        } else if (!std::strcmp(argv[i], "--tournament")) {
            tournament_matches = std::atoi(next());
        } else if (!std::strcmp(argv[i], "--candidate-variant")) {
            candidate_variant = std::atoi(next());
        } else if (!std::strcmp(argv[i], "--difficulty")) {
            tournament_difficulty = std::atoi(next());
        } else if (!std::strcmp(argv[i], "--vary-civs")) {
            vary_civs = true;
        } else if (!std::strcmp(argv[i], "--out")) {
            tournament_csv = next();
        } else if (!std::strcmp(argv[i], "--jobs")) {
            jobs = std::atoi(next());
        } else if (!std::strcmp(argv[i], "--allied")) {
            allied = true;
        } else if (!std::strcmp(argv[i], "--map-type")) {
            map_type = next();
        } else if (!std::strcmp(argv[i], "--dump-maps")) {
            g_dump_map_dir = next();
        } else if (!std::strcmp(argv[i], "--preview-map")) {
            preview_out = next();
        } else if (!std::strcmp(argv[i], "--trace-seed")) {
            g_trace_seed = parse_u64(next());
        } else if (!std::strcmp(argv[i], "--nettest")) {
            nettest_turns = std::atoi(next());
            if (nettest_turns <= 0) nettest_turns = 60;
        } else if (!std::strcmp(argv[i], "--upnp")) {
            upnp_probe = true;
        } else if (!std::strcmp(argv[i], "--arena")) {
            arena_matches = std::atoi(next());
        } else if (!std::strcmp(argv[i], "--max-pop")) {
            arena_max_pop = std::atoi(next());
        } else if (!std::strcmp(argv[i], "--map-size")) {
            arena_map_size = std::atoi(next());
        } else if (!std::strcmp(argv[i], "--verbose")) {
            arena_verbose = true;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return 1;
        }
    }

    if (!preview_out.empty()) {
        run_preview_mode(seed, players < 0 ? 2 : players, preview_out);
        return 0;
    }
    if (arena_matches > 0) {
        if (jobs <= 0) {
            unsigned hc = std::thread::hardware_concurrency();
            jobs = hc > 0 ? static_cast<int>(hc) : 4;
        }
        // Default cap: 48000 ticks = 40 simulated minutes at the standard dt.
        run_arena_mode(arena_matches, ticks < 0 ? 48000 : ticks, dt, seed, tournament_difficulty,
                       arena_max_pop, arena_map_size, map_type, tournament_csv, jobs, arena_verbose);
    } else if (tournament_matches > 0) {
        // Default cap: 60 simulated minutes per match (most games decide
        // well before this; ends early the instant Control::game_over
        // flips regardless), at the same fixed dt every other mode uses.
        if (jobs <= 0) {
            unsigned hc = std::thread::hardware_concurrency();
            jobs = hc > 0 ? static_cast<int>(hc) : 4;
        }
        run_tournament_mode(tournament_matches, ticks < 0 ? 72000 : ticks, dt, seed, tournament_difficulty,
                            candidate_variant, vary_civs, tournament_csv, jobs, allied, map_type,
                            arena_max_pop, arena_map_size);
    } else if (nettest_turns > 0) {
        return run_nettest(nettest_turns);
    } else if (upnp_probe) {
        return run_upnp_probe();
    } else if (!fixture_path.empty()) {
        run_fixture_mode(fixture_path, ticks < 0 ? 300 : ticks, dt, print_every < 0 ? 20 : print_every);
    } else {
        run_checksum_mode(seed, players, ticks < 0 ? 1200 : ticks, dt, print_every < 0 ? 60 : print_every);
    }
    return 0;
}
