#include "sim/scenario.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_set>

namespace ww::sim {

namespace {

const std::unordered_map<int, std::string> CIV_BASE = {
    {0, "spr_uk_base"}, {1, "spr_capitol"}, {2, "spr_nazi_base"},
    {3, "spr_soviet_base"}, {4, "spr_japan_base"}, {5, "spr_italy_base"},
    {6, "spr_france_base"}, {7, "spr_china_base"}, {8, "spr_ottoman_base"},
};

// Where each team's starting base goes: teams are spread evenly around an
// ellipse inscribed in the map's usable LAND, at a randomly rotated
// starting angle, so which side or corner a given player opens on changes
// every match.
//
// This used to be a fixed 8-entry table of map fractions, with no random
// input at all -- team 0 started in the top-left and team 1 in the
// bottom-right of literally every skirmish, so the human knew exactly
// where the enemy was before scouting a single tile. It also capped every
// spawn at x <= 0.62 of the width regardless of map type, leaving the
// eastern third of a land map permanently unused.
//
// EVEN angular spacing (rather than n independently-random points) is what
// keeps this fair: every team sits the same distance from the middle and
// from its neighbours, so a roll can't hand one player a roomy corner and
// wedge two others together. Only the ROTATION and the ring's radius are
// random -- deliberately no per-team angular wobble, which would make one
// player's spawn measurably closer to the enemy than another's.
std::vector<std::pair<double, double>> spawn_points(const World& world, int n, Rng& rng) {
    n = std::max(1, std::min(n, 8));
    constexpr double kTau = 6.283185307179586;

    // MEASURE the usable land rather than assuming a fraction of the map.
    // Several map types flood a whole edge -- the "random" water toggle and
    // normandy put an open sea in the east, stalingrad the Volga, guam is
    // an island ringed by ocean -- and the coastline's exact position is
    // itself randomised per match (see World's terrain generation), so any
    // hardcoded fraction would either waste half the map or drop a base in
    // the sea. An interior lake never widens the box, so this stays tight.
    int lx0 = world.cols, lx1 = -1, ly0 = world.rows, ly1 = -1;
    for (int tx = 0; tx < world.cols; ++tx) {
        for (int ty = 0; ty < world.rows; ++ty) {
            if (world.terrain[tx][ty] == WATER) continue;
            lx0 = std::min(lx0, tx); lx1 = std::max(lx1, tx);
            ly0 = std::min(ly0, ty); ly1 = std::max(ly1, ty);
        }
    }
    if (lx1 < lx0 || ly1 < ly0) { lx0 = ly0 = 0; lx1 = world.cols - 1; ly1 = world.rows - 1; }
    // Inset a couple of tiles: a base needs its own footprint plus a build
    // ring, and the caller clamps to 120px from the map edge anyway.
    double x0 = (lx0 + 2) * double(TILE), x1 = (lx1 - 1) * double(TILE);
    double y0 = (ly0 + 2) * double(TILE), y1 = (ly1 - 1) * double(TILE);
    if (x1 <= x0) { x0 = 0.0; x1 = world.px_w; }
    if (y1 <= y0) { y0 = 0.0; y1 = world.px_h; }
    double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;

    // The minimum gap any two bases must end up with, as a fraction of the
    // land's smaller dimension. Tightens as the field fills up -- eight
    // teams simply can't sit as far apart as two can.
    double span = std::min(x1 - x0, y1 - y0);
    double min_sep = span * (n <= 2 ? 0.55 : n <= 4 ? 0.38 : 0.22);

    std::vector<std::pair<double, double>> best;
    double best_sep = -1.0;
    // A layout only misses the separation floor on a lopsided map (a narrow
    // strip of land on a water map), where the ellipse is far flatter than
    // it is tall and some rotations bunch teams along its short axis --
    // re-rolling finds one that fits. Bounded, and the roomiest attempt
    // wins outright if none clears the bar, so this always returns a
    // layout rather than looping.
    for (int attempt = 0; attempt < 16; ++attempt) {
        // How far out the ring sits, as a fraction of the land's half-
        // extent: well under 1.0 so a base is never jammed against the
        // coast or the map edge, and jittered so the spread isn't a
        // constant either.
        double fill = rng.uniform(0.74, 0.94);
        double rx = (x1 - x0) * 0.5 * fill, ry = (y1 - y0) * 0.5 * fill;
        double a0 = rng.uniform(0.0, kTau);
        double step = kTau / n;
        std::vector<std::pair<double, double>> pts;
        for (int i = 0; i < n; ++i) {
            double a = a0 + step * i;
            pts.emplace_back(cx + std::cos(a) * rx, cy + std::sin(a) * ry);
        }
        double worst = 1e18;
        for (size_t i = 0; i < pts.size(); ++i) {
            for (size_t j = i + 1; j < pts.size(); ++j) {
                worst = std::min(worst, std::hypot(pts[i].first - pts[j].first,
                                                   pts[i].second - pts[j].second));
            }
        }
        if (worst > best_sep) { best_sep = worst; best = pts; }
        if (worst >= min_sep) break; // single-team games trivially pass (worst stays 1e18)
    }
    return best;
}

int64_t tile_key(int tx, int ty) { return (static_cast<int64_t>(tx) << 32) ^ static_cast<uint32_t>(ty); }

// "Normal" map size (kMapSizeValues[1] in menu_controller.cpp -> cols=80) --
// the density every area-scaled scatter count below is calibrated against,
// so a Normal map's counts stay BYTE-IDENTICAL to before this existed; only
// other sizes change. Every count that's meant to read as a roughly uniform
// density across the whole map (forests, berries, neutral ore, deer, fish,
// themed decoration) must scale with AREA (cols*cols, maps are square), not
// with cols alone -- cols-alone growth is one dimension while the map's
// area grows with the square of it, so a bigger map would end up with the
// same features spread thinner (visibly "stretched out") instead of just
// having proportionally more of them at the same spacing.
constexpr int kRefCols = 80;
int area_scaled(int count_at_ref, int floor_n, int cols) {
    return std::max(floor_n, count_at_ref * cols * cols / (kRefCols * kRefCols));
}

// Grant a team its civ's free starting techs (civs.json "freeTech", e.g.
// France's free Irrigation). Defined in the data but previously never
// applied -- called right after a team's civ is assigned, before any unit or
// building spawns, so the tech's effects (farm food bonus, etc.) take hold.
void grant_free_techs(Team& t, const Bonuses& bonuses) {
    const nlohmann::json& e = bonuses.civ_effects(t.civ);
    auto it = e.find("freeTech");
    if (it != e.end() && it->is_array()) {
        for (const auto& tech : *it) t.tech.insert(tech.get<std::string>());
    }
}

// Playstyle archetypes an AI opponent can be dealt in a skirmish (see
// Team::ai_behavior's branches in control_ai.cpp: attack threshold and
// villager-target tuning per preset). Randomized per team below so a
// skirmish doesn't always play out as the same fixed "balanced" script --
// campaign levels bypass this entirely (new_from_level sets ai_behavior
// straight from the level's own per-player data, never from this table).
const std::vector<std::string>& skirmish_ai_behaviors() {
    static const std::vector<std::string> v = {"balanced", "rusher", "aggressive", "defensive", "passive"};
    return v;
}

// ---- Archipelago (island) map sculpting -------------------------------------
// Stamp a blobby island of radius `r` TILES centred at tile (cxt,cyt), writing
// terrain value `land`. The coastline wobbles via two low-frequency sine terms
// (same idea as guam) so islands don't read as perfect circles.
void stamp_island(World& w, double cxt, double cyt, double r, Rng& rng, int land) {
    double p1 = rng.uniform(0, 6.2831853), p2 = rng.uniform(0, 6.2831853);
    int x0 = std::max(0, static_cast<int>(std::floor(cxt - r - 4)));
    int x1 = std::min(w.cols - 1, static_cast<int>(std::ceil(cxt + r + 4)));
    int y0 = std::max(0, static_cast<int>(std::floor(cyt - r - 4)));
    int y1 = std::min(w.rows - 1, static_cast<int>(std::ceil(cyt + r + 4)));
    for (int x = x0; x <= x1; ++x)
        for (int y = y0; y <= y1; ++y) {
            double dx = x - cxt, dy = y - cyt;
            double ang = std::atan2(dy, dx);
            double rr = r + 3.0 * std::sin(ang * 3.0 + p1) + 2.0 * std::sin(ang * 5.0 + p2);
            if (dx * dx + dy * dy <= rr * rr) w.terrain[x][y] = land;
        }
}

// Carve the islands for the archipelago map types. One big MAIN island is
// stamped at each spawn point so every base lands on solid, defensible ground;
// a set of smaller NEUTRAL islands is scattered through the map's middle to
// fight over. `pts` are the (already edge-clamped) spawn centres in pixels.
// Returns the neutral-island centres (pixels) so the caller can seed them with
// the contested oil/iron/wood the map is all about.
std::vector<std::pair<double, double>> sculpt_island_map(
        World& w, const std::vector<std::pair<double, double>>& pts, Rng& rng) {
    const bool pacific = w.map_type == "pacific islands";
    const double cols = w.cols;
    int n = static_cast<int>(pts.size());
    // Shrink every island as the player count climbs so the spawns (spread around
    // an ellipse) keep a good belt of ocean between them instead of merging into a
    // ring of touching land. The spawns sit sin(pi/n) of the ellipse chord apart,
    // so scaling the radius by that keeps the land-to-water ratio between adjacent
    // islands roughly constant. sin(pi/2)=1, so 1v1/1-player sizes are unchanged;
    // 4 players -> ~0.71x, 8 players -> ~0.38x.
    const double PI = 3.14159265358979;
    double shrink = std::sin(PI / std::max(2, n));
    // Main-island radius as a fraction of map width: Santa Cruz spawns own a big
    // chunk each (~a quarter of the map at 1v1), Pacific's are tighter and more naval.
    double main_r = cols * (pacific ? 0.18 : 0.25) * shrink;
    for (auto& [px, py] : pts) stamp_island(w, px / TILE, py / TILE, main_r, rng, 1); // lush grass

    // Neutral islands: Santa Cruz gets several small ones (3 for 1v1); Pacific a
    // single larger-but-still-tiny one for 1v1 (~5% of the map). Both scale UP in
    // count with player count (more to fight over) but each shrinks like the mains.
    int n_neutral = pacific ? std::max(1, n / 2) : (n + 1);
    double neutral_r = cols * (pacific ? 0.12 : 0.06) * shrink;
    std::vector<std::pair<double, double>> centers;
    double mcx = w.px_w * 0.5, mcy = w.px_h * 0.5;
    for (int i = 0; i < n_neutral; ++i) {
        double bx = mcx, by = mcy;
        for (int attempt = 0; attempt < 40; ++attempt) {
            // Somewhere in the central ~third of the map, angle staggered so a
            // batch of neutrals spreads out instead of clumping in one spot.
            double a = rng.uniform(0, 6.2831853) + i * (6.2831853 / std::max(1, n_neutral));
            double rad = rng.uniform(0.0, 0.30) * cols * TILE;
            bx = mcx + std::cos(a) * rad;
            by = mcy + std::sin(a) * rad;
            bool ok = true;
            for (auto& [px, py] : pts) // clear of every main island (spawn)
                if (std::hypot(bx - px, by - py) < (main_r + neutral_r + 6.0) * TILE) { ok = false; break; }
            for (auto& [ox, oy] : centers) // clear of the other neutrals
                if (std::hypot(bx - ox, by - oy) < (2.0 * neutral_r + 6.0) * TILE) { ok = false; break; }
            if (ok) break;
        }
        stamp_island(w, bx / TILE, by / TILE, neutral_r, rng, 1);
        centers.emplace_back(bx, by);
    }

    // Beach rim: any land tile touching open water becomes beach sand so every
    // island has a proper coastline. Single pass, land->beach only (beach is
    // still non-water), so it can't cascade inland.
    std::vector<std::pair<int, int>> rim;
    static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
    for (int x = 0; x < w.cols; ++x)
        for (int y = 0; y < w.rows; ++y) {
            if (w.terrain[x][y] == WATER) continue;
            for (int d = 0; d < 4; ++d) {
                int nx = x + dx4[d], ny = y + dy4[d];
                if (nx < 0 || nx >= w.cols || ny < 0 || ny >= w.rows || w.terrain[nx][ny] == WATER) {
                    rim.emplace_back(x, y);
                    break;
                }
            }
        }
    for (auto& [x, y] : rim) w.terrain[x][y] = 11; // beach sand
    return centers;
}

} // namespace

std::unique_ptr<World> new_skirmish(const DataStore& data, const Bonuses& bonuses, Control& control,
                                    Rng& rng, EventBus& events, const SkirmishSettings& settings) {
    int n = settings.n_players;
    int cols = settings.map_size * 2;
    auto world = std::make_unique<World>(data, bonuses, control, rng, events, cols, cols,
                                         settings.map_type, settings.water);

    if (settings.reveal_mode == 2) {
        world->reveal_all = true;
    } else if (settings.reveal_mode == 1) {
        // "No fog": start every tile at "explored" (1) instead of
        // "unexplored" (0) -- update_fog only ever upgrades a cell to
        // visible (2) or fades a visible one back down to explored (1), it
        // never re-drops to 0, so this alone keeps the whole map at least
        // explored (terrain/buildings visible, units only in actual sight
        // range) for the rest of the match with no other change needed.
        for (auto& col : world->fog) std::fill(col.begin(), col.end(), 1);
    }

    std::vector<int> civs = settings.civs;
    if (civs.empty()) {
        civs.push_back(0);
        static const int defaults[3] = {2, 3, 4};
        for (int i = 0; i < std::min(n - 1, 3); ++i) civs.push_back(defaults[i]);
    }
    if (settings.enemy_civ && n > 1) {
        civs.resize(n, 0);
        civs[1] = *settings.enemy_civ;
    }

    for (int i = 0; i < n; ++i) {
        Team& t = control.teams[i];
        t.civ = (i < static_cast<int>(civs.size())) ? civs[i] : (i + 1);
        grant_free_techs(t, bonuses);
        t.leader = (i < static_cast<int>(settings.leaders.size()))
                       ? settings.leaders[i]
                       : (i == 0) ? settings.leader : (i == 1) ? settings.enemy_leader : 0;
        t.is_ai = settings.spectator || (i != 0);
        t.difficulty = settings.difficulty;
        // Random playstyle per AI opponent (team 0 is the human slot -- or,
        // in a spectator match, is never actually driven by ai_tick anyway,
        // see its team==1 start there -- so it's left at the "default"
        // member-initializer either way).
        if (i != 0) {
            const auto& behaviors = skirmish_ai_behaviors();
            t.ai_behavior = behaviors[rng.below(static_cast<uint32_t>(behaviors.size()))];
            // Skirmish AI derives its Victorian playstyle from the actual map
            // (ai_assess_map overrides ai_behavior on its first tick); the
            // random roll above is just the pre-assessment fallback. Campaign
            // AI (new_from_level) never sets this, keeping its authored logic.
            t.ai_map_derive = true;
        }
        t.strategy = world->want_water ? "navy" : "land";
        if (i < static_cast<int>(settings.colours.size())) t.colour = settings.colours[i];
        else if (i == 0 && settings.player_colour) t.colour = *settings.player_colour;
        else if (i == 1 && settings.enemy_colour) t.colour = *settings.enemy_colour;
        if (i < static_cast<int>(settings.allies.size())) t.ally = settings.allies[i];
        // Deathmatch: every team starts loaded with resources (matches
        // Other_4.gml's game_mode==1 food/wood/oil/iron=20000 set -- our
        // Team::res default of 200/200/100/100 is a smaller baseline than
        // GML's own Standard-mode 100/0/50/0, so this targets the same
        // "effectively unlimited" spirit rather than exact GML parity).
        if (settings.deathmatch) {
            t.res["food"] = 20000;
            t.res["wood"] = 20000;
            t.res["oil"] = 20000;
            t.res["iron"] = 20000;
        }
    }
    // Construction/training/research all run through this single
    // multiplier in the C++ port (see building_behavior.cpp's `percent +=
    // ... * world.build_speed` and unit_behavior.cpp's construction
    // increment) -- GML instead had two independently-tuned game_mode==1
    // paths (builder-tick construction: flat +25 vs per-building-type
    // 1/3/5; production queue: base=25 vs 4, or 10 vs 1 for age-advances),
    // averaging out to roughly 6-10x faster. One flat multiplier can't
    // reproduce each ratio exactly, so this picks a single representative
    // value in that range.
    if (settings.deathmatch) world->build_speed = 8.0;

    auto pts = spawn_points(*world, n, rng);
    // Archipelago maps started as open ocean (see World's ctor). Now that the
    // spawn ellipse is known, carve a main island under each spawn plus neutral
    // islands to contest. Clamp the spawns to the SAME edge margin the base loop
    // below uses, so each island's centre and its base target line up exactly.
    std::vector<std::pair<double, double>> neutral_islands;
    if (world->map_type == "santa cruz islands" || world->map_type == "pacific islands") {
        for (auto& p : pts) {
            p.first = std::max(120.0, std::min(world->px_w - 120, p.first));
            p.second = std::max(120.0, std::min(world->px_h - 120, p.second));
        }
        neutral_islands = sculpt_island_map(*world, pts, rng);
    }
    std::vector<std::pair<double, double>> bases;
    for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
        double sx = pts[i].first, sy = pts[i].second;
        sx = std::max(120.0, std::min(world->px_w - 120, sx));
        sy = std::max(120.0, std::min(world->px_h - 120, sy));
        double cx = world->px_w * 0.5, cy = world->px_h * 0.5;
        // Failsafe: a base must sit on solid ground with NO water within 7 tiles
        // (so the base, its starting villagers -- which spawn ~5 tiles away --
        // and its build radius are never in/next to the sea). Nudge toward the
        // map centre until that holds; keep the least-watery spot if the search
        // runs out (tiny/island maps).
        auto water_near = [&](double px, double py) {
            int worst = 0;
            for (int dx = -7; dx <= 7; ++dx)
                for (int dy = -7; dy <= 7; ++dy)
                    if (world->is_water(px + dx * TILE, py + dy * TILE)) ++worst;
            return worst;
        };
        // Reachability: how much CONNECTED open (non-water) land a base can
        // actually reach from a spot -- a capped flood-fill. A base needs a
        // decent contiguous pocket to build farms/houses in and to reach its
        // resources; a boxed-in spawn (tiny island / thin peninsula) scores low
        // here and starves (the diagnosed "0 farms, 7 villagers" collapse), so
        // it gets nudged toward the map centre to find a bigger landmass.
        auto open_land = [&](double px, double py) {
            int stx = static_cast<int>(px / TILE), sty = static_cast<int>(py / TILE);
            if (stx < 0 || stx >= world->cols || sty < 0 || sty >= world->rows) return 0;
            if (world->terrain[stx][sty] == WATER) return 0;
            std::unordered_set<int64_t> seen;
            std::vector<std::pair<int, int>> stk;
            stk.push_back({stx, sty});
            seen.insert(tile_key(stx, sty));
            int count = 0;
            static const int dxs[4] = {1, -1, 0, 0}, dys[4] = {0, 0, 1, -1};
            while (!stk.empty() && count < 160) { // cap: 160 reachable tiles = plenty of room
                auto [tx, ty] = stk.back();
                stk.pop_back();
                ++count;
                for (int d = 0; d < 4; ++d) {
                    int nx = tx + dxs[d], ny = ty + dys[d];
                    if (nx < 0 || nx >= world->cols || ny < 0 || ny >= world->rows) continue;
                    if (world->terrain[nx][ny] == WATER) continue;
                    int64_t k = tile_key(nx, ny);
                    if (seen.count(k)) continue;
                    seen.insert(k);
                    stk.push_back({nx, ny});
                }
            }
            return count;
        };
        // A good spot has a large open pocket AND no water within the 7-tile
        // build ring; open land dominates the score, water breaks ties.
        auto score = [&](double px, double py) { return open_land(px, py) * 1000 - water_near(px, py); };
        auto is_good = [&](double px, double py) {
            return open_land(px, py) >= 140 && water_near(px, py) == 0;
        };
        int guard = 0, best_score = score(sx, sy);
        double best_sx = sx, best_sy = sy;
        while (!is_good(best_sx, best_sy) && guard < 400) {
            sx += (sx < cx) ? TILE : -TILE;
            sy += (sy < cy) ? TILE : -TILE;
            sx = std::max(120.0, std::min(world->px_w - 120, sx));
            sy = std::max(120.0, std::min(world->px_h - 120, sy));
            int s = score(sx, sy);
            if (s > best_score) { best_score = s; best_sx = sx; best_sy = sy; }
            ++guard;
        }
        sx = best_sx;
        sy = best_sy;
        // Grid-align like a player-placed building would (world.snap) --
        // spawn_building itself doesn't snap (place_building snaps
        // explicitly before calling it), so without this the starting
        // base lands whereever the water-avoidance nudging above happened
        // to leave it, not necessarily on the 32px grid.
        std::tie(sx, sy) = world->snap("base", sx, sy);
        EntityRef base_ref = world->spawn_building("base", i, sx, sy);
        if (Building* b = world->get_building(base_ref)) {
            auto it = CIV_BASE.find(control.teams[i].civ);
            b->sprite = (it != CIV_BASE.end()) ? it->second : "spr_uk_base";
        }
        bases.emplace_back(sx, sy);

        int n_civ = 3 + (control.teams[i].civ == 7 ? 1 : 0);
        for (int v = 0; v < n_civ; ++v) {
            auto [cx2, cy2] = world->clear_point_near(sx - 80 + v * 50, sy + 150, TILE, false);
            world->spawn_unit("civilian", i, cx2, cy2);
        }
        auto [scx, scy] = world->clear_point_near(sx + 140, sy - 40, TILE, false);
        world->spawn_unit("cavalry", i, scx, scy);
    }

    std::unordered_set<int64_t> used;
    // Keep a slightly bigger clear ring right around each base (nothing spawns
    // within this) so the start isn't cramped -- resources sit a touch further
    // out on average, leaving room to build.
    constexpr double NEAR = 6.0 * TILE;

    auto place = [&](const std::string& kind, double px, double py, bool want_water = false) -> bool {
        if (px < 0 || py < 0 || px >= world->px_w || py >= world->px_h) return false;
        if (world->is_water(px, py) != want_water) return false;
        int64_t key = tile_key(static_cast<int>(std::floor(px / TILE)), static_cast<int>(std::floor(py / TILE)));
        if (used.count(key)) return false;
        for (auto& [bx, by] : bases) {
            if (std::abs(px - bx) < NEAR && std::abs(py - by) < NEAR) return false;
        }
        used.insert(key);
        if (kind == "deer") world->spawn_deer(px, py);
        else world->spawn_resource(kind, px, py);
        return true;
    };

    std::vector<std::pair<double, double>> ore_centers;
    auto cluster = [&](const std::string& kind, int n_nodes, double spread,
                       std::optional<double> cx_in = std::nullopt,
                       std::optional<double> cy_in = std::nullopt, bool ww = false) {
        double cx = cx_in ? *cx_in : rng.uniform(0, world->px_w);
        double cy = cy_in ? *cy_in : rng.uniform(0, world->px_h);
        if (kind == "oil" || kind == "iron") {
            for (int t = 0; t < 20; ++t) {
                bool ok = true;
                for (auto& [ox, oy] : ore_centers) {
                    if (!(std::abs(cx - ox) > 10 * TILE || std::abs(cy - oy) > 10 * TILE)) { ok = false; break; }
                }
                if (ok) break;
                cx = rng.uniform(0, world->px_w);
                cy = rng.uniform(0, world->px_h);
            }
            ore_centers.emplace_back(cx, cy);
        }
        int placed = 0;
        for (int i = 0; i < n_nodes * 3; ++i) {
            if (place(kind, cx + rng.uniform(-spread, spread), cy + rng.uniform(-spread, spread), ww))
                ++placed;
        }
        return placed;
    };

    // Oil/iron in TIGHT packed pods: 3-4 nodes crammed into adjacent tiles (a
    // 2x2-ish blob) rather than a loose scatter, so a "deposit" reads as one
    // dense clump right next to itself. Honours the 10-tile ore-separation and
    // base-proximity rules via place()/ore_centers.
    auto tight_ore = [&](const std::string& kind) {
        double cx = rng.uniform(0, world->px_w), cy = rng.uniform(0, world->px_h);
        for (int t = 0; t < 20; ++t) {
            bool ok = true;
            for (auto& [ox, oy] : ore_centers)
                if (!(std::abs(cx - ox) > 8 * TILE || std::abs(cy - oy) > 8 * TILE)) { ok = false; break; }
            if (ok) break;
            cx = rng.uniform(0, world->px_w);
            cy = rng.uniform(0, world->px_h);
        }
        ore_centers.emplace_back(cx, cy);
        int want = rng.randint(3, 4), placed = 0;
        for (int dy = 0; dy <= 1 && placed < want; ++dy)
            for (int dx = 0; dx <= 1 && placed < want; ++dx)
                if (place(kind, cx + dx * TILE, cy + dy * TILE)) ++placed;
    };

    const std::string& mt = world->map_type;
    // Palms on the tropical island and the desert (oases); ordinary trees
    // everywhere else.
    std::string tree_kind = (mt == "guam" || mt == "negev desert") ? "palm" : "tree";
    // Guaranteed home resource PODS: rolled once for the whole map so every base
    // gets the SAME supply (no per-player imbalance). Each player reliably opens
    // with a sizeable berry vein, a large + small oil pod and a large + small
    // iron pod, spread to different sides of the base so they don't pile up on
    // one side. Pods are tight clusters, so a villager working one has minimal
    // walking and the deposit is dense.
    int oil_large = rng.randint(4, 6);
    int iron_large = rng.randint(4, 6);
    for (auto& [bx, by] : bases) {
        for (int i = 0; i < 14; ++i) {
            double a = rng.uniform(0, 6.283), r = rng.uniform(8, 11) * TILE;
            place(tree_kind, bx + std::cos(a) * r, by + std::sin(a) * r);
        }
        // Tight pod of `count` nodes near the base: pick a non-water centre in
        // the 7-12 tile band around `ang`, then pack nodes outward in rings.
        auto place_pod = [&](const std::string& kind, int count, double ang) {
            double cx = bx, cy = by;
            for (int attempt = 0; attempt < 20; ++attempt) {
                double a = ang + rng.uniform(-0.5, 0.5), rad = rng.uniform(7, 12) * TILE;
                cx = bx + std::cos(a) * rad;
                cy = by + std::sin(a) * rad;
                if (!world->is_water(cx, cy)) break;
            }
            int placed = 0;
            for (int ring = 0; ring <= 4 && placed < count; ++ring) {
                int spokes = ring == 0 ? 1 : ring * 6;
                for (int k = 0; k < spokes && placed < count; ++k) {
                    double sa = k * 6.283 / spokes;
                    if (place(kind, cx + std::cos(sa) * ring * TILE, cy + std::sin(sa) * ring * TILE))
                        ++placed;
                }
            }
        };
        double a0 = rng.uniform(0, 6.283);
        place_pod("berry", 6, a0);               // sizeable starting food vein
        place_pod("oil", oil_large, a0 + 1.3);   // large oil pod (4-6)
        place_pod("oil", 2, a0 + 2.6);           // small oil pod
        place_pod("iron", iron_large, a0 + 3.9); // large iron pod (4-6)
        place_pod("iron", 2, a0 + 5.2);          // small iron pod
        // TWO dense tree CLUSTERS (woodlines) close to home, on roughly
        // opposite sides, both within 7 tiles -- so every player/AI reliably
        // starts with at least two workable wood sources nearby, not just one.
        // Each retries a few centres so water/occupied ground can't leave a
        // start short of a second woodline.
        {
            double a0 = rng.uniform(0, 6.283);
            for (int w = 0; w < 2; ++w) {
                double a = a0 + w * 3.14159 + rng.uniform(-0.6, 0.6);
                int planted = 0, guard = 0;
                while (planted == 0 && guard++ < 8) {
                    double r = rng.uniform(7.5, 9.0) * TILE;
                    double cx = bx + std::cos(a) * r, cy = by + std::sin(a) * r;
                    planted = cluster(tree_kind, rng.randint(10, 16), 1.5 * TILE, cx, cy);
                    a += rng.uniform(-0.5, 0.5); // wiggle the angle on a failed try
                }
            }
        }
    }

    // Archipelago maps: guarantee every NEUTRAL island a contested cache of oil,
    // iron and wood -- the whole point of these maps is fighting over the middle.
    // Ring-packed like the home pods (place() enforces non-water + no overlap),
    // NOT via cluster(), whose ore-separation rule would relocate the deposit off
    // the island. Neutrals sit far from any base, so base-proximity never trips.
    if (!neutral_islands.empty()) {
        auto seed_pod = [&](const std::string& kind, int count, double cx, double cy) {
            int placed = 0;
            for (int ring = 0; ring <= 4 && placed < count; ++ring) {
                int spokes = ring == 0 ? 1 : ring * 6;
                for (int k = 0; k < spokes && placed < count; ++k) {
                    double sa = k * 6.2831853 / spokes;
                    if (place(kind, cx + std::cos(sa) * ring * TILE, cy + std::sin(sa) * ring * TILE))
                        ++placed;
                }
            }
        };
        for (auto& [ncx, ncy] : neutral_islands) {
            seed_pod("oil", rng.randint(3, 4), ncx, ncy);
            seed_pod("iron", rng.randint(3, 4), ncx, ncy);
            for (int t = 0, want = rng.randint(8, 12); t < want; ++t) {
                double a = rng.uniform(0, 6.2831853), r = rng.uniform(0, 3.0) * TILE;
                place(tree_kind, ncx + std::cos(a) * r, ncy + std::sin(a) * r);
            }
        }
    }

    // Fewer, DENSER forests: more nodes packed into a tighter spread so each
    // reads as a near-solid blob the pathfinder can cleanly box around, rather
    // than a scatter of single trees with diagonal gaps units get wedged in.
    // Forest DENSITY is themed per map: the Ardennes is smothered in trees, the
    // desert and the ruined city are nearly bare.
    // Forests scale with map AREA (area_scaled -- same density on every map
    // size, just proportionally more of them on a bigger one) and each
    // forest's shape VARIES: some are near-round blobs, others long belts
    // (an elongated spread on one axis), so the woods don't all read the same.
    int forest_cnt = area_scaled(26, 6, cols);
    if (mt == "ardennes") forest_cnt = area_scaled(106, 16, cols);                // dense forest
    else if (mt == "negev desert" || mt == "stalingrad") forest_cnt = area_scaled(10, 3, cols); // sparse
    for (int i = 0; i < forest_cnt; ++i) {
        int nodes = rng.randint(14, 26);
        // ~40% of forests are stretched into a belt: pick a longer node count
        // and lay them along a line by clustering around a stretched centre.
        if (rng.uniform(0, 1) < 0.4) {
            double cx = rng.uniform(0, world->px_w), cy = rng.uniform(0, world->px_h);
            bool horiz = rng.uniform(0, 1) < 0.5;
            for (int k = 0; k < nodes * 3; ++k) {
                double ex = cx + rng.uniform(-1, 1) * (horiz ? 5.0 : 1.5) * TILE;
                double ey = cy + rng.uniform(-1, 1) * (horiz ? 1.5 : 5.0) * TILE;
                place(tree_kind, ex, ey);
            }
        } else {
            cluster(tree_kind, nodes, 2 * TILE);
        }
    }

    int berry_cnt = area_scaled(11, 3, cols);
    if (mt == "negev desert" || mt == "stalingrad") berry_cnt = area_scaled(5, 1, cols);
    for (int i = 0; i < berry_cnt; ++i) cluster("berry", rng.randint(2, 4), 2 * TILE);

    // Neutral oil/iron: tight packed pods scattered over the map to fight over,
    // scaled with map area (more pods, not sparser ones, on a bigger map). The
    // desert is extra oil-rich.
    int ore_pods = area_scaled(5, 3, cols);
    int oil_cnt = (mt == "negev desert") ? ore_pods + rng.randint(2, 4) : ore_pods + rng.randint(0, 1);
    for (int i = 0; i < oil_cnt; ++i) tight_ore("oil");
    for (int i = 0, cnt = ore_pods + rng.randint(0, 1); i < cnt; ++i) tight_ore("iron");

    // Small neutral oil wells out in contested territory, kept well away from
    // every home base (>=12 tiles) -- extra oil to expand toward and fight over
    // once the depleting home wells run dry, and a reason to control the map's
    // middle. 1-2 nodes each (a small well, not a full pod).
    int neutral_oil_wells = area_scaled(6, 3, cols);
    for (int i = 0; i < neutral_oil_wells; ++i) {
        for (int t = 0; t < 40; ++t) {
            double px = rng.uniform(0, world->px_w), py = rng.uniform(0, world->px_h);
            bool far = true;
            for (auto& [bx, by] : bases) {
                if (std::hypot(px - bx, py - by) < 12.0 * TILE) { far = false; break; }
            }
            if (!far) continue;
            int want = rng.randint(1, 2), placed = 0;
            for (int dx = 0; dx <= 1 && placed < want; ++dx)
                if (place("oil", px + dx * TILE, py)) ++placed;
            if (placed > 0) break;
        }
    }

    // Deer roam the green/forest maps; none in the desert or the frozen ruins.
    // Cluster COUNT scales with area same as everything else above -- the
    // (lo, hi) random range itself is area_scaled so it still reads as "2 to
    // 4ish" at Normal but grows into "5 to 10ish" on a Huge map instead of
    // staying flat while the map around it gets much bigger.
    if (mt != "negev desert" && mt != "stalingrad") {
        int deer_lo = area_scaled(2, 1, cols), deer_hi = area_scaled(4, 2, cols);
        for (int i = 0, cnt = rng.randint(deer_lo, deer_hi); i < cnt; ++i) {
            cluster("deer", rng.randint(2, 5), 2.5 * TILE);
        }
    }

    // Themed ground clutter (cosmetic, walkable): bomb rubble across Stalingrad,
    // rock/pebble scatter across the Negev.
    auto scatter_decor = [&](const std::vector<std::string>& kinds, int n) {
        for (int i = 0; i < n; ++i) {
            double rx = rng.uniform(0, world->px_w), ry = rng.uniform(0, world->px_h);
            if (world->is_water(rx, ry)) continue;
            world->decorations.push_back({rx, ry, kinds[rng.randrange(static_cast<int>(kinds.size()))]});
        }
    };
    if (mt == "stalingrad") scatter_decor({"rubble", "big rubble", "small rubble"}, area_scaled(80, 10, cols));
    else if (mt == "negev desert") scatter_decor({"pebbles", "stones"}, area_scaled(40, 6, cols));
    else if (mt == "normandy") scatter_decor({"hedge", "fence"}, area_scaled(80, 10, cols)); // bocage hedgerows

    // Fish shoals on ANY open water -- oceans, lakes, rivers, oases. place()
    // only ever lands them on water tiles (want_water=true arg), so a land-only
    // map simply ends up with none. Scaled with area like everything else --
    // a bigger map has proportionally more open water to fill.
    int shoals = area_scaled((mt == "guam") ? 30 : 20, 10, cols);
    for (int i = 0; i < shoals; ++i) {
        place("fish", rng.uniform(0, world->px_w), rng.uniform(0, world->px_h), true);
    }

    world->prime();
    return world;
}

std::unique_ptr<World> new_from_level(const DataStore& data, const Bonuses& bonuses, Control& control,
                                      Rng& rng, EventBus& events, const ww::campaign::Level& level) {
    int cols = std::max(1, level.grid_size);
    auto world = std::make_unique<World>(data, bonuses, control, rng, events, cols, cols, "random",
                                         /*water=*/false);
    // A campaign level's grid is flat grass except where explicitly
    // painted (editor's own "absent tile = grass"
    // convention) -- wipe out whatever random blob pattern the World
    // constructor just generated before applying the level's own tiles.
    for (auto& col : world->terrain) std::fill(col.begin(), col.end(), 0);

    // Starting fog-of-war state chosen in the campaign editor (Level::
    // reveal_mode), same semantics as new_skirmish's SkirmishSettings path:
    // 2 = fully revealed, 1 = whole map pre-explored (No fog), 0 = normal fog.
    if (level.reveal_mode == 2) {
        world->reveal_all = true;
    } else if (level.reveal_mode == 1) {
        for (auto& col : world->fog) std::fill(col.begin(), col.end(), 1);
    }

    for (size_t i = 0; i < level.players.size() && i < 8; ++i) {
        Team& t = control.teams[i];
        t.civ = level.players[i].civ;
        grant_free_techs(t, bonuses);
        // Campaign AI controls: per-player behaviour preset + the level's
        // bespoke AI profile (see control_ai.cpp's ai_manage branch). Skirmish
        // never sets these, so its AI is unaffected.
        t.ai_behavior = level.players[i].ai_behavior.empty() ? "default"
                                                             : level.players[i].ai_behavior;
        t.ai_profile = level.ai_profile;
        t.max_era = level.max_age; // whole-level age cap (-1 = none), enforced in World::enqueue
        // Whole-level STARTING age: begin every player already advanced to this
        // era. Setting Team::era is exactly the state reaching that age via
        // age-up produces (building_behavior assigns era the same way), so units/
        // buildings of that era become immediately buildable. Never start above
        // the cap if one is set.
        if (level.start_age > 0) {
            t.era = level.start_age;
            // The age cap can never sit BELOW the starting era -- a level has to
            // be able to begin in the age it's configured to start in, so raise
            // the cap to match rather than dragging the start back down to it.
            if (t.max_era >= 0 && t.max_era < t.era) t.max_era = t.era;
        }
        // Starting stockpile: the per-player resources set in the editor become
        // each team's opening stockpile -- for the human AND every AI. Without
        // this they'd all start at the default 200/200/100/100.
        t.res["food"] = level.players[i].food;
        t.res["wood"] = level.players[i].wood;
        t.res["oil"] = level.players[i].oil;
        t.res["iron"] = level.players[i].iron;
        t.ally = level.players[i].team;
        // Slot 0 is always the campaign's own locked human player (see
        // campaign_data.h's LevelPlayer comment) -- every other slot is AI,
        // same convention new_skirmish uses for team index != 0.
        t.is_ai = (i != 0);
        t.strategy = world->want_water ? "navy" : "land";
    }

    for (auto& t : level.terrain) {
        if (t.tx < 0 || t.tx >= cols || t.ty < 0 || t.ty >= cols) continue;
        // Base ground textures -> real terrain ids (see terrain_sprite(),
        // game_client.cpp): water and sand map to the sim's own pre-
        // existing ids; the other 4 editor-only base kinds (dirt/gravel/
        // brown dirt/pavement) got new ids 4-7 specifically so they'd have
        // somewhere to go.
        if (t.base == "water") world->terrain[t.tx][t.ty] = WATER;
        else if (t.base == "sand") world->terrain[t.tx][t.ty] = 3;
        else if (t.base == "dirt") world->terrain[t.tx][t.ty] = 4;
        else if (t.base == "gravel") world->terrain[t.tx][t.ty] = 5;
        else if (t.base == "brown dirt") world->terrain[t.tx][t.ty] = 6;
        else if (t.base == "pavement") world->terrain[t.tx][t.ty] = 7;

        double cx = (t.tx + 0.5) * TILE, cy = (t.ty + 0.5) * TILE;
        if (t.resource == "deer") {
            world->spawn_deer(cx, cy);
        } else if (t.resource == "flames") {
            // A hand-placed flame is a real hazard, not decoration -- same
            // per-second damage/radius a shell-impact fire uses (see
            // World::kFireDamagePerSecond/kFireRadius), just a much longer
            // duration since nothing will naturally re-ignite it the way a
            // fresh shell impact would.
            world->fires.push_back({cx, cy, 60.0});
        } else if (!t.resource.empty() && resource_kinds().count(t.resource)) {
            world->spawn_resource(t.resource, cx, cy);
        } else if (!t.resource.empty() && decoration_kinds().count(t.resource)) {
            // Static clutter/obstacle -- see World::decorations and
            // decoration_kinds(). Passability is handled generically by
            // rebuild_occupied()/passable(), not here.
            world->decorations.push_back({cx, cy, t.resource});
        }
    }

    // Placed units carry an optional stable id (PlacedEntity::id, assigned by
    // the editor when an objective captures them); map it to the spawned unit so
    // objective-completion tracking (below) can watch the right units.
    std::unordered_map<std::string, EntityRef> placed_unit_by_id;
    for (auto& u : level.units) {
        if (u.player_index < 0 || u.player_index >= static_cast<int>(level.players.size())) continue;
        EntityRef ref = world->spawn_unit(u.type, u.player_index, (u.tx + 0.5) * TILE, (u.ty + 0.5) * TILE);
        if (!u.id.empty()) placed_unit_by_id[u.id] = ref;
    }
    for (auto& b : level.buildings) {
        if (b.player_index < 0 || b.player_index >= static_cast<int>(level.players.size())) continue;
        // (tx,ty) is the footprint's top-left tile (matches
        // editor's own placement convention) -- centre =
        // tx*TILE + w/2, NOT (tx+0.5)*TILE, same reasoning as that
        // project's own editor.cpp comment on the "encroaches on 9 cells
        // instead of 4" bug for even-tile-count buildings.
        auto [bw, bh] = building_wh(b.type);
        double cx = b.tx * TILE + bw / 2.0, cy = b.ty * TILE + bh / 2.0;
        EntityRef ref = world->spawn_building(b.type, b.player_index, cx, cy);
        if (b.type == "base") {
            if (Building* bld = world->get_building(ref)) {
                auto it = CIV_BASE.find(control.teams[b.player_index].civ);
                bld->sprite = (it != CIV_BASE.end()) ? it->second : "spr_uk_base";
            }
        }
    }

    // Campaign map triggers (editor's Events tab). `message` becomes a
    // World::MessageTrigger as before; `resources`/`spawn`/`dormant` build the
    // trigger-state vectors on World (see world.h), checked each tick in
    // World::update. `gate` is still schema-only (not consumed). tx == -1 means
    // "never placed on the map", excluded before the bounds check.
    auto tile_in_bounds = [&](int tx, int ty) { return tx >= 0 && tx < cols && ty >= 0 && ty < cols; };
    for (auto& e : level.events) {
        if (e.type == "message") {
            if (e.tx < 0 || !tile_in_bounds(e.tx, e.ty)) continue;
            world->message_triggers.push_back({(e.tx + 0.5) * TILE, (e.ty + 0.5) * TILE, e.text, false});
        } else if (e.type == "resources") {
            if (e.tx < 0 || !tile_in_bounds(e.tx, e.ty)) continue;
            World::ResourcePickup p;
            p.x = (e.tx + 0.5) * TILE;
            p.y = (e.ty + 0.5) * TILE;
            p.food = e.res_food;
            p.wood = e.res_wood;
            p.oil = e.res_oil;
            p.iron = e.res_iron;
            p.text = e.text;
            world->resource_pickups.push_back(std::move(p));
        } else if (e.type == "spawn") {
            if (e.area_tw <= 0 || e.area_th <= 0 || e.spawn_unit.empty() || e.spawn_count <= 0) continue;
            World::SpawnTrigger s;
            s.box = {e.area_tx, e.area_ty, e.area_tw, e.area_th};
            s.unit = e.spawn_unit;
            s.count = e.spawn_count;
            s.player = e.spawn_player;
            s.objective_id = e.unlock_objective_id;
            s.text = e.text;
            world->spawn_triggers.push_back(std::move(s));
        } else if (e.type == "dormant") {
            World::WakeTrigger w;
            // Tripwire boxes: prefer the trip_areas list; fall back to the single
            // area2_* for older campaigns that predate multiple trip boxes.
            if (!e.trip_areas.empty()) {
                for (auto& r : e.trip_areas)
                    if (r.tw > 0 && r.th > 0) w.trips.push_back({r.tx, r.ty, r.tw, r.th});
            } else if (e.area2_tw > 0 && e.area2_th > 0) {
                w.trips.push_back({e.area2_tx, e.area2_ty, e.area2_tw, e.area2_th});
            }
            if (w.trips.empty()) continue; // no tripwire -> nothing to wake
            // Charge target: the centre of the first trip box.
            w.target_x = (w.trips[0].tx + w.trips[0].tw / 2.0) * TILE;
            w.target_y = (w.trips[0].ty + w.trips[0].th / 2.0) * TILE;
            for (auto& r : e.los_areas) w.los.push_back({r.tx, r.ty, r.tw, r.th});
            w.text = e.text;
            // Freeze every unit whose tile sits in the "units" box (area_*).
            for (auto ref : world->active_units) {
                Unit* u = world->units.get(ref);
                // Skip units already claimed by an earlier dormant event, so a
                // unit can belong to only ONE dormant group -- overlapping "units"
                // boxes stay independent (tripping one never wakes another's units).
                if (!u || !u->common.alive || u->dormant) continue;
                int tx = static_cast<int>(u->common.x / TILE);
                int ty = static_cast<int>(u->common.y / TILE);
                if (tx < e.area_tx || tx >= e.area_tx + e.area_tw) continue;
                if (ty < e.area_ty || ty >= e.area_ty + e.area_th) continue;
                u->dormant = true;
                w.group.push_back(ref);
            }
            world->wake_triggers.push_back(std::move(w));
        }
    }

    // Objective-completion watches, resolved to live unit refs -- gates spawn
    // triggers whose unlock_objective_id points at one of these. (protect_unit
    // isn't evaluated mid-match; kill_units/move_to_area are, see World::update.)
    for (auto& obj : level.objectives) {
        World::ObjectiveWatch w;
        w.id = obj.id;
        w.type = obj.type;
        w.tx = obj.area_tx;
        w.ty = obj.area_ty;
        w.tw = obj.area_tw;
        w.th = obj.area_th;
        for (auto& uid : obj.target_unit_ids) {
            auto it = placed_unit_by_id.find(uid);
            if (it != placed_unit_by_id.end()) w.targets.push_back(it->second);
        }
        world->objective_watches.push_back(std::move(w));
    }

    world->prime();
    return world;
}

} // namespace ww::sim
