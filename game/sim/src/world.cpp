#include "sim/world.h"

#include "sim/behavior.h"

#include <algorithm>
#include <cmath>

namespace ww::sim {

namespace {
int64_t tile_key(int tx, int ty) {
    return (static_cast<int64_t>(tx) << 32) ^ static_cast<uint32_t>(ty);
}
} // namespace

const std::unordered_map<std::string, ResourceKindInfo>& resource_kinds() {
    static const std::unordered_map<std::string, ResourceKindInfo> kinds = {
        {"tree", {"spr_tree", 1, 100}},
        {"palm", {"spr_palmtree", 1, 100}},
        {"berry", {"spr_berry_bush", 0, 150}},
        {"oil", {"spr_oil_pool", 2, 800}},
        {"iron", {"spr_iron_ore", 3, 800}},
        {"fish", {"spr_fish", 0, 250}},
    };
    return kinds;
}

const std::unordered_map<std::string, DecorationKindInfo>& decoration_kinds() {
    // Mirrors editor/src/editor.cpp's kTerrainKinds
    // decoration entries exactly (name and passability both) -- see
    // DecorationKindInfo's header comment on why `passable` isn't sourced
    // from the original game. Frame is only ever non-zero for the 5
    // destroyed-building variants (spr_house_destroyed's subimages).
    static const std::unordered_map<std::string, DecorationKindInfo> kinds = {
        {"rubble", {"spr_rubble1", 0, true}},
        {"brown stone", {"spr_stone1", 0, false}},
        {"grey stone", {"spr_stone2", 0, false}},
        {"stones", {"spr_stones", 0, false}},
        {"big tree", {"spr_tree1", 0, false}},
        {"oriental tree", {"spr_tree2", 0, false}},
        {"crate", {"spr_crate", 0, false}},
        {"pebbles", {"spr_rocks", 0, true}},
        {"destroyed building 1", {"spr_house_destroyed", 0, false}},
        {"destroyed building 2", {"spr_house_destroyed", 1, false}},
        {"destroyed building 3", {"spr_house_destroyed", 2, false}},
        {"destroyed building 4", {"spr_house_destroyed", 3, false}},
        {"destroyed building 5", {"spr_house_destroyed", 4, false}},
        {"hedge", {"spr_hedge", 0, true}},
        {"fence", {"spr_fence1", 0, false}},
        {"fence2", {"spr_fence2", 0, false}},
        {"fence3", {"spr_fence3", 0, false}},
        {"orange rock", {"spr_cliff1", 0, false}},
        {"yellow rock", {"spr_cliff2", 0, false}},
        {"brown rock", {"spr_cliff3", 0, false}},
        {"grey rock", {"spr_cliff4", 0, false}},
        {"big rubble", {"spr_96_rubble", 0, true}},
        {"small rubble", {"spr_64_rubble", 0, true}},
        {"aa gun rubble", {"spr_aa_gun_rubble", 0, true}},
        {"b29 rubble", {"spr_b29_rubble", 0, true}},
        {"biplane rubble", {"spr_biplane_rubble", 0, true}},
        {"bomber rubble", {"spr_bomber_rubble", 0, true}},
        {"bush", {"spr_bush", 0, true}},
        {"fighter rubble", {"spr_fighter_rubble", 0, true}},
        {"flak rubble", {"spr_flak_rubble", 0, true}},
        {"heavy bomber rubble", {"spr_heavy_bomber_rubble", 0, true}},
        {"heavy tank rubble", {"spr_heavy_tank_rubble", 0, true}},
        {"jet fighter rubble", {"spr_jet_fighter_rubble", 0, true}},
        {"light tank rubble", {"spr_light_tank_rubble", 0, true}},
    };
    return kinds;
}

std::pair<int, int> building_wh(const std::string& name) {
    // Native pixel footprint of each building, taken directly from its
    // sprite's actual fw/fh (assets/manifest.json), NOT a
    // "grid squares" count multiplied out by some tile constant -- that
    // scheme (previously N * BTILE, BTILE=24) assumed every building's
    // footprint was a multiple of 24px, but most sprites here are true
    // 64x64 (house/farm/barracks/factory/refinery/university/market/
    // academy/shipyard), not 72x72 as 3*24 would give. That mismatch, on
    // top of snap() quantizing position to a *different* grid than the
    // footprint's own units, meant two same-size buildings could never be
    // placed with their edges exactly flush -- the nearest reachable
    // snapped spot was always either a few px of overlap (rejected, drawn
    // red) or an unwanted gap. Every value below is an exact multiple of
    // TILE (32), so snapping position to the TILE grid now always lands
    // footprint edges exactly flush between adjacent buildings.
    static const std::unordered_map<std::string, std::pair<int, int>> sizes = {
        {"base", {96, 96}}, {"fortress", {96, 96}},
        {"house", {64, 64}}, {"farm", {64, 64}}, {"barracks", {64, 64}}, {"factory", {64, 64}},
        {"refinery", {64, 64}}, {"university", {64, 64}}, {"market", {64, 64}}, {"academy", {64, 64}},
        {"shipyard", {64, 64}},
        {"airbase", {96, 64}}, {"nuclear reactor", {96, 64}},
        // tower/aa tower (flak tower)/outpost are tall, thin spires --
        // footprint (collision, placement, click/selection hitbox -- see
        // GameClient::select_at and right_click_order) is just their base
        // tile, one TILE square, not the full sprite height. The sprite
        // itself draws at its own native (taller) manifest size regardless
        // (see game_client.cpp's building draw pass, which reads atlas
        // meta, not foot_w/foot_h), so this only shrinks what blocks
        // movement/placement and what you have to click.
        {"tower", {32, 32}}, {"aa tower", {32, 32}},
        {"outpost", {32, 32}},
        {"palisade", {32, 32}}, {"iron wall", {32, 32}},
    };
    auto it = sizes.find(name);
    return it == sizes.end() ? std::make_pair(64, 64) : it->second;
}

// How far BELOW a building's own world anchor (common.x/y -- also where its
// sprite is drawn, see game_client.cpp's building draw pass) the footprint
// rectangle (building_wh) is actually centered. Zero for almost everything
// -- footprint == full sprite bounds, so it's already centered exactly on
// the anchor. tower/aa tower/outpost are the exception (see building_wh's
// comment): their footprint is only the base tile of a much taller sprite,
// and that sprite's own art anchors near its base already (assets/
// manifest.json: spr_tower/spr_aa_tower's oy=64 of fh=96, spr_outpost's
// oy=32 of fh=64 -- both put exactly 32px of the sprite BELOW the anchor),
// so nudging the footprint down by half its own height lines it up with
// that same bottom 32px instead of straddling the sprite's visual middle.
double footprint_dy(const std::string& name) {
    static const std::unordered_set<std::string> kBaseAnchored = {"tower", "aa tower", "outpost"};
    return kBaseAnchored.count(name) ? 16.0 : 0.0;
}

// spawn_building_shot's "fire from near the top of the tower" muzzle offset
// used to just be foot_h*0.35, back when foot_h was every building's own
// true sprite height. Now that tower/aa tower's foot_h is their small base
// footprint instead (see building_wh), they need their REAL (taller)
// sprite height here specifically -- every other defensive building
// (fortress, base) still has foot_h == sprite height, so this is a no-op
// for them.
double muzzle_visual_h(const std::string& name, double foot_h) {
    if (name == "tower" || name == "aa tower") return 96.0;
    return foot_h;
}

// ---- construction / terrain ----

World::World(const DataStore& data_, const Bonuses& bonuses_, Control& control_, Rng& rng_,
             EventBus& events_, int cols_, int rows_, const std::string& map_type_, bool water)
    : cols(cols_), rows(rows_), px_w(cols_ * TILE), px_h(rows_ * TILE), map_type(map_type_),
      control(control_), data(data_), bonuses(bonuses_), rng(rng_), events(events_) {
    // guam is a true island (ocean all around -> navy). Every other named
    // theme map is a land map (any water on them is internal lakes/oases/a
    // river, generated below, not an open sea). "random" honours the toggle.
    if (map_type == "guam" || map_type == "normandy" || map_type == "santa cruz islands" ||
        map_type == "pacific islands") {
        want_water = true; // island / amphibious landing beach -> naval AI
    } else if (map_type == "ostland" || map_type == "negev desert" || map_type == "stalingrad" ||
               map_type == "ardennes" || map_type == "arabia" || map_type == "arena") {
        want_water = false;
    } else {
        want_water = water;
    }

    terrain.assign(cols, std::vector<int>(rows, 0));
    fog.assign(cols * kFogSubdiv, std::vector<int>(rows * kFogSubdiv, 0));

    auto blob = [&](int val, int n, int size) {
        for (int i = 0; i < n; ++i) {
            int cx = rng.randrange(cols), cy = rng.randrange(rows);
            for (int s = 0; s < size; ++s) {
                if (cx >= 0 && cx < cols && cy >= 0 && cy < rows) terrain[cx][cy] = val;
                cx += rng.choice3();
                cy += rng.choice3();
            }
        }
    };

    if (map_type == "arabia") {
        for (auto& col : terrain) std::fill(col.begin(), col.end(), 3);
        blob(0, std::max(6, cols / 3), 20);
        return;
    }
    if (map_type == "arena") {
        int m = std::max(3, cols / 10);
        for (int x = 0; x < cols; ++x) {
            for (int y = 0; y < rows; ++y) {
                if (x < m || y < m || x >= cols - m || y >= rows - m) terrain[x][y] = 3;
            }
        }
        blob(1, std::max(3, cols / 6), 18);
        return;
    }
    // Terrain palette (see client's terrain_sprite): 0 green grass, 1 bright
    // grass, 2 WATER, 3 sand-dirt, 4 dirt, 5 gravel/rock, 6 dark dirt/mud,
    // 7 urban ruin, 8 snow, 9 snowy grass, 10 desert sand, 11 beach sand.
    // Archipelago maps (Santa Cruz / Pacific islands): start as OPEN OCEAN. The
    // actual islands can't be carved here -- their positions must line up with
    // the player spawns, which aren't known until new_skirmish computes them --
    // so scenario.cpp::sculpt_island_map() stamps the land afterward, once the
    // spawn ellipse is decided. spawn_points sees all-water and falls back to
    // the full-map ellipse, giving evenly-spread island centres to build on.
    if (map_type == "santa cruz islands" || map_type == "pacific islands") {
        for (auto& col : terrain) std::fill(col.begin(), col.end(), WATER);
        return;
    }
    if (map_type == "guam") {
        int ring = std::min(20, cols / 3);
        for (auto& col : terrain) std::fill(col.begin(), col.end(), 11); // beach sand base
        blob(1, std::max(5, cols / 3), 22); // lush tropical grass interior
        blob(3, std::max(3, cols / 5), 14); // sandier inland patches
        double p1 = rng.uniform(0, 6.28), p2 = rng.uniform(0, 6.28);
        for (int x = 0; x < cols; ++x) {
            for (int y = 0; y < rows; ++y) {
                int wl = ring + static_cast<int>(4 * std::sin(y / 6.0 + p1)) + rng.choice3();
                int wr = ring + static_cast<int>(4 * std::sin(y / 5.0 + p2)) + rng.choice3();
                int wt = ring + static_cast<int>(4 * std::sin(x / 6.0 + p2)) + rng.choice3();
                int wb = ring + static_cast<int>(4 * std::sin(x / 5.0 + p1)) + rng.choice3();
                if (x < wl || y < wt || x >= cols - wr || y >= rows - wb) terrain[x][y] = WATER;
            }
        }
        return;
    }
    if (map_type == "negev desert") {
        // Endless pale desert: sand everywhere, rocky gravel outcrops, and a
        // couple of small oasis pools (the scenario rings them with palms).
        for (auto& col : terrain) std::fill(col.begin(), col.end(), 10); // desert sand
        blob(5, std::max(4, cols / 4), 14);                              // rocky outcrops
        blob(3, std::max(2, cols / 8), 10);                              // lighter dune sand
        int n_oasis = rng.randint(2, 3);
        for (int i = 0; i < n_oasis; ++i) blob(WATER, 1, 9); // small pools
        return;
    }
    if (map_type == "stalingrad") {
        // Open steppe: dry grassland dotted with dirt and dusty patches, crossed
        // by one MAIN river (the Volga) whose course is randomised every match.
        for (auto& col : terrain) std::fill(col.begin(), col.end(), 0); // green grass base
        blob(1, std::max(5, cols / 3), 22);                             // bright grass swathes
        blob(4, std::max(4, cols / 4), 16);                             // dry dirt
        blob(3, std::max(3, cols / 6), 12);                             // dusty/sandy patches
        // A meandering river from the top edge to the bottom edge at a random
        // column, a few tiles wide. One FORD (a land crossing) is left so the
        // river never fully bisects this land (no-naval) map -- units can still
        // reach the far bank on foot there.
        double rx = cols * rng.uniform(0.32, 0.68);       // mean column
        double drift = rng.uniform(-0.14, 0.14) * cols;   // net top->bottom drift
        double amp = rng.uniform(4.0, 9.0);               // meander amplitude
        double period = rng.uniform(8.0, 16.0);
        double phase = rng.uniform(0.0, 6.28);
        int half_w = rng.randint(1, 2);                   // river is 2*half_w+1 tiles wide
        int ford_y = rng.randint(rows / 4, rows * 3 / 4); // a crossing point
        for (int y = 0; y < rows; ++y) {
            if (std::abs(y - ford_y) <= 2) continue; // leave the ford as dry land
            double t = y / static_cast<double>(std::max(1, rows - 1));
            int cx = static_cast<int>(rx + drift * t + amp * std::sin(y / period + phase)) + rng.choice3();
            for (int x = cx - half_w; x <= cx + half_w; ++x)
                if (x >= 0 && x < cols) terrain[x][y] = WATER;
        }
        return;
    }
    if (map_type == "ardennes") {
        // Snow-dusted forest country: snowy grass base, bare snow patches and
        // frozen mud; the scenario blankets it in dense forest.
        for (auto& col : terrain) std::fill(col.begin(), col.end(), 9); // snowy grass
        blob(8, std::max(6, cols / 3), 22);                             // bare snow
        blob(6, std::max(3, cols / 6), 14);                             // frozen mud
        return;
    }
    if (map_type == "normandy") {
        // Amphibious landing: green bocage inland, a band of beach sand at the
        // waterline, and the sea (the Channel) filling the eastern edge.
        for (auto& col : terrain) std::fill(col.begin(), col.end(), 0); // green grass
        blob(1, std::max(4, cols / 4), 18);                            // meadow patches
        blob(4, std::max(3, cols / 6), 10);                            // dirt/mud tracks
        int coast = static_cast<int>(cols * 0.72);
        double phase = rng.uniform(0, 6.28);
        for (int y = 0; y < rows; ++y) {
            int edge = coast + static_cast<int>(4 * std::sin(y / 6.0 + phase)) + rng.choice3();
            edge = std::max(static_cast<int>(cols * 0.55), std::min(cols - 1, edge));
            for (int x = std::max(0, edge - 3); x < edge; ++x)
                if (terrain[x][y] != WATER) terrain[x][y] = 11; // beach sand strip
            for (int x = edge; x < cols; ++x) terrain[x][y] = WATER; // the Channel
        }
        return;
    }

    blob(1, std::max(4, cols / 3), 22);
    blob(3, std::max(3, cols / 5), 16);
    if (map_type == "ostland") {
        // Baltic plains: lush green base + bright meadows + some dirt, dotted
        // with several irregular lakes.
        blob(1, std::max(6, cols / 2), 26);
        blob(4, std::max(3, cols / 6), 12);
        int n_lakes = rng.randint(3, 4);
        for (int i = 0; i < n_lakes; ++i) blob(WATER, 1, 40);
        return;
    }

    if (want_water) {
        int coast = static_cast<int>(cols * rng.uniform(0.62, 0.80));
        int amp = rng.randint(2, 6);
        int period = rng.randint(5, 11);
        double phase = rng.uniform(0, 6.28);
        for (int y = 0; y < rows; ++y) {
            int edge = coast + static_cast<int>(amp * std::sin(y / double(period) + phase)) + rng.choice3();
            edge = std::max(static_cast<int>(cols * 0.5), std::min(cols - 2, edge));
            for (int x = edge; x < cols; ++x) terrain[x][y] = WATER;
        }
    }
}

int World::tile_at(double px, double py) const {
    int tx = static_cast<int>(std::floor(px / TILE)), ty = static_cast<int>(std::floor(py / TILE));
    if (tx >= 0 && tx < cols && ty >= 0 && ty < rows) return terrain[tx][ty];
    return -1;
}

int World::fog_at(double px, double py) const {
    int tx = static_cast<int>(std::floor(px / kFogTilePx)), ty = static_cast<int>(std::floor(py / kFogTilePx));
    if (tx >= 0 && tx < static_cast<int>(fog.size()) && ty >= 0 &&
        ty < static_cast<int>(fog[0].size())) {
        return fog[tx][ty];
    }
    return 0;
}

// Direct port of game/world.py's World.update_fog: 0 unexplored, 1
// explored, 2 visible. Every previously-visible cell fades to explored,
// then each of `player`'s alive units/buildings re-reveals a circular
// (not square) radius around itself, in fog cells (see kFogSubdiv).
void World::update_fog(int player) {
    int fcols = static_cast<int>(fog.size()), frows = static_cast<int>(fog[0].size());
    if (reveal_all) {
        for (auto& col : fog) std::fill(col.begin(), col.end(), 2);
        return;
    }
    for (auto& col : fog) {
        for (auto& v : col) {
            if (v == 2) v = 1;
        }
    }
    auto reveal_around = [&](double ex, double ey, double sight_px) {
        // Same "+1 tile of padding, minimum 3 tiles" shape as the Python
        // original, just expressed in fog cells instead of terrain tiles.
        int sight = std::max(3 * kFogSubdiv, static_cast<int>(sight_px / kFogTilePx) + kFogSubdiv);
        int cx = static_cast<int>(std::floor(ex / kFogTilePx)), cy = static_cast<int>(std::floor(ey / kFogTilePx));
        int s2 = sight * sight;
        for (int tx = std::max(0, cx - sight); tx < std::min(fcols, cx + sight + 1); ++tx) {
            int dx = tx - cx;
            for (int ty = std::max(0, cy - sight); ty < std::min(frows, cy + sight + 1); ++ty) {
                int dy = ty - cy;
                if (dx * dx + dy * dy <= s2) fog[tx][ty] = 2;
            }
        }
    };
    for (auto ref : active_units) {
        Unit* u = units.get(ref);
        // Garrisoned passengers don't reveal fog on their own -- their
        // transport does (carrier.valid() -> skip).
        if (u && u->common.alive && u->common.team == player && !u->carrier.valid()) {
            reveal_around(u->common.x, u->common.y, u->sight_px);
        }
    }
    // Buildings have no per-catalog sight stat yet -- matches
    // game/world.py's catalog default (buildings.get(name, {}).get("sight", 5)).
    constexpr double kBuildingSightPx = 5.0 * TILE;
    // Radar tech grants +3 tiles LOS to the watchtower/base line (see the tech).
    bool has_radar = player >= 0 && player < static_cast<int>(control.teams.size()) &&
                     control.teams[player].tech.count("radar") > 0;
    static const std::unordered_set<std::string> kRadarBuildings = {"tower", "aa tower", "outpost", "base", "fortress"};
    for (auto ref : active_buildings) {
        Building* b = buildings.get(ref);
        // A still-under-construction FOUNDATION grants no vision -- only a
        // finished building reveals fog (matches the user's request).
        if (b && b->common.alive && b->complete && b->common.team == player) {
            // Buildings reveal at least the baseline radius, but a building
            // with a larger catalog sight (e.g. the outpost's 7 tiles) reveals
            // out to its own sight instead.
            double px = std::max(kBuildingSightPx, b->sight_px);
            if (has_radar && kRadarBuildings.count(b->name)) px += 3.0 * TILE;
            reveal_around(b->common.x, b->common.y, px);
        }
    }
    // Campaign line-of-sight boxes revealed by fired WakeTriggers -- re-stamped to
    // visible every tick so they stay lit even with no unit nearby (see world.h
    // reveal_areas; kFogSubdiv fog cells per tile).
    for (auto& r : reveal_areas) {
        for (int tx = std::max(0, r.tx * kFogSubdiv); tx < std::min(fcols, (r.tx + r.tw) * kFogSubdiv); ++tx)
            for (int ty = std::max(0, r.ty * kFogSubdiv); ty < std::min(frows, (r.ty + r.th) * kFogSubdiv); ++ty)
                fog[tx][ty] = 2;
    }
}

bool World::is_water(double px, double py) const { return tile_at(px, py) == WATER; }

bool World::passable(bool is_air, bool is_ship, double px, double py, bool phase_trees) const {
    int t = tile_at(px, py);
    if (t < 0) return false;
    if (is_air) return true;
    // Buildings block via a precise rectangle check against
    // solid_building_rects_ (world.h has the full rationale) rather than
    // the coarser per-TILE occupied_ set, so units can walk right up to a
    // building's true pixel edge instead of stopping at whichever whole
    // 32px tile happens to touch it.
    for (const auto& r : solid_building_rects_) {
        if (px >= r[0] && px <= r[2] && py >= r[1] && py <= r[3]) return false;
    }
    int tx = static_cast<int>(std::floor(px / TILE)), ty = static_cast<int>(std::floor(py / TILE));
    auto key = tile_key(tx, ty);
    // Resources (trees/ore/etc.) block ground movement the same way solid
    // buildings do -- units path around them, not through them. Depleted
    // resources drop out of resource_tiles_ on the next rebuild_occupied(),
    // so gathering a tile clear immediately reopens it. Li Zongren's infantry
    // (phase_trees) walk straight through TREES/palms -- but ore, berries and
    // everything else still block them.
    if (resource_tiles_.count(key) && !(phase_trees && tree_tiles_.count(key))) return false;
    // Same treatment as resources -- an impassable Decoration (see
    // decoration_kinds()) blocks the tile it sits on; a passable one
    // (pebbles, rubble, hedge, etc.) never entered decoration_tiles_ in the
    // first place, see rebuild_occupied().
    if (decoration_tiles_.count(key)) return false;
    if (is_ship) return t == WATER;
    return t != WATER;
}

bool World::passable_planning(bool is_air, bool is_ship, double px, double py, bool phase_trees) const {
    if (!passable(is_air, is_ship, px, py, phase_trees)) return false;
    if (is_air) return true; // aircraft ignore ground obstacles entirely
    for (const auto& r : pending_foundation_rects_) {
        if (px >= r[0] && px <= r[2] && py >= r[1] && py <= r[3]) return false;
    }
    return true;
}

std::pair<double, double> World::nearest_passable(double cx, double cy, bool is_air, bool is_ship) const {
    if (passable(is_air, is_ship, cx, cy)) return {cx, cy};
    int ctx = static_cast<int>(std::floor(cx / TILE)), cty = static_cast<int>(std::floor(cy / TILE));
    for (int r = 1; r <= 12; ++r) {
        for (int dx = -r; dx <= r; ++dx) {
            for (int dy = -r; dy <= r; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue; // ring border only
                int tx = ctx + dx, ty = cty + dy;
                if (tx < 0 || tx >= cols || ty < 0 || ty >= rows) continue;
                double px = tx * TILE + TILE / 2.0, py = ty * TILE + TILE / 2.0;
                if (passable(is_air, is_ship, px, py)) return {px, py};
            }
        }
    }
    return {cx, cy}; // nothing found within range; caller's give-up logic handles it
}

std::vector<std::pair<int, int>> World::building_tile_list(const std::string& name, double cx,
                                                             double cy) const {
    auto [w, h] = building_wh(name); // already native pixels, see building_wh's comment
    double hw = w / 2.0, hh = h / 2.0;
    double fcy = cy + footprint_dy(name); // see footprint_dy's comment
    std::vector<std::pair<int, int>> out;
    int tx0 = static_cast<int>(std::floor((cx - hw) / TILE));
    int tx1 = static_cast<int>(std::floor((cx + hw - 1) / TILE));
    int ty0 = static_cast<int>(std::floor((fcy - hh) / TILE));
    int ty1 = static_cast<int>(std::floor((fcy + hh - 1) / TILE));
    for (int tx = tx0; tx <= tx1; ++tx) {
        for (int ty = ty0; ty <= ty1; ++ty) out.emplace_back(tx, ty);
    }
    return out;
}

void World::rebuild_building_tiles() {
    // Building-derived collision sets: rebuilt EVERY tick (buildings are few --
    // tens -- and change often: foundations flip blocks_movement on the first
    // hammer blow, completions, deaths). Split out from rebuild_occupied so bulk
    // placement (populate_stress_test) can refresh footprint_clear after each
    // building WITHOUT the far more expensive resource-tile/spatial-grid rebuild.
    occupied_.clear();
    solid_building_rects_.clear();
    all_building_rects_.clear();
    pending_foundation_rects_.clear();
    for (auto ref : active_buildings) {
        Building* b = buildings.get(ref);
        if (!b || !b->common.alive) continue;
        // blocks_movement(), not the raw `solid` flag: an unstarted
        // foundation is walk-through until its first hammer blow (see
        // Building::blocks_movement). It still goes into
        // all_building_rects_ below, so you STILL can't place a second
        // building on top of one -- it just doesn't obstruct units.
        if (b->blocks_movement()) {
            for (auto [tx, ty] : building_tile_list(b->name, b->common.x, b->common.y)) {
                occupied_.insert(tile_key(tx, ty));
            }
        }
        double hw = b->foot_w / 2.0, hh = b->foot_h / 2.0;
        double fcy = b->common.y + footprint_dy(b->name); // see footprint_dy's comment
        all_building_rects_.push_back({b->common.x - hw, fcy - hh, b->common.x + hw, fcy + hh});
        if (b->blocks_movement()) {
            auto rect = all_building_rects_.back();
            // Walls: inflate the COLLISION rect a few px so DIAGONALLY-adjacent
            // segments (which only touch at a corner) overlap and seal the gap a
            // unit could otherwise slip through. Placement still uses the
            // un-inflated 32x32 footprint (all_building_rects_/footprint_clear),
            // so adjacent segments still snap flush.
            if (b->name == "palisade" || b->name == "iron wall") {
                constexpr double m = 4.0;
                rect = {rect[0] - m, rect[1] - m, rect[2] + m, rect[3] + m};
            }
            solid_building_rects_.push_back(rect);
        } else if (b->solid) {
            // Solid by type but not started yet: walk-through for now, and
            // about to become a wall. Planning routes around it (see
            // passable_planning) even though movement doesn't.
            pending_foundation_rects_.push_back(all_building_rects_.back());
        }
    }
}

void World::rebuild_occupied() {
    rebuild_building_tiles();
    // Resource + decoration tiles are STATIC: resources change only on gather-
    // out/spawn (both flag static_grid_dirty_) and decorations are map-gen only,
    // so re-hashing all ~1200 of them every tick was pure waste -- by far the
    // dominant per-tick sim cost on the current larger maps. Rebuild them only
    // when that set changed. The flag is read (not reset) here; rebuild_spatial_
    // grid() -- always called immediately after this, in update() and prime() --
    // consumes the same flag and resets it, so both static layers stay in sync.
    if (static_grid_dirty_) {
        resource_tiles_.clear();
        tree_tiles_.clear();
        decoration_tiles_.clear();
        for (auto ref : active_resources) {
            Resource* r = resources.get(ref);
            if (!r || !r->common.alive) continue;
            auto key = tile_key(static_cast<int>(std::floor(r->common.x / TILE)),
                                static_cast<int>(std::floor(r->common.y / TILE)));
            resource_tiles_.insert(key);
            if (r->name == "tree" || r->name == "palm") tree_tiles_.insert(key);
        }
        for (auto& dec : decorations) {
            auto it = decoration_kinds().find(dec.kind);
            if (it == decoration_kinds().end() || it->second.passable) continue;
            decoration_tiles_.insert(tile_key(static_cast<int>(std::floor(dec.x / TILE)),
                                              static_cast<int>(std::floor(dec.y / TILE))));
        }
    }
}

void World::rebuild_spatial_grid() {
    // DYNAMIC layer (things that MOVE) -- rebuilt every tick.
    grid.clear();
    for (auto ref : active_units) {
        Unit* u = units.get(ref);
        // Garrisoned passengers (inside a transport) are off the board: not
        // in the grid, so they can't be targeted, shot, or collided with.
        if (u && u->common.alive && !u->carrier.valid()) grid.insert(ref, u->common.x, u->common.y);
    }
    for (auto ref : active_deer) {
        Deer* d = deer.get(ref);
        if (d && d->common.alive) grid.insert(ref, d->common.x, d->common.y);
    }
    // STATIC layer (buildings + resources -- never move) -- rebuilt ONLY when
    // that set changes (a building or resource spawned or died). Re-hashing the
    // ~1500 map resources every tick was by far the dominant sim cost on the
    // current larger maps; they change rarely, so this is a big win.
    if (static_grid_dirty_) {
        static_grid_dirty_ = false;
        grid.clear_static();
        for (auto ref : active_buildings) {
            Building* b = buildings.get(ref);
            if (b && b->common.alive) grid.insert_static(ref, b->common.x, b->common.y);
        }
        for (auto ref : active_resources) {
            Resource* r = resources.get(ref);
            if (r && r->common.alive) grid.insert_static(ref, r->common.x, r->common.y);
        }
    }
}

std::pair<double, double> World::snap(const std::string& name, double cx, double cy) const {
    auto [w, h] = building_wh(name); // already native pixels, see building_wh's comment
    // Every building's footprint is an exact multiple of TILE (see
    // building_wh), so snapping its edge to the TILE grid always lands two
    // same- or different-sized buildings' edges exactly flush when placed
    // side by side -- no remainder/overlap possible.
    double hw = w / 2.0, hh = h / 2.0;
    int tx = static_cast<int>(std::floor((cx - hw) / TILE));
    int ty = static_cast<int>(std::floor((cy - hh) / TILE));
    return {tx * double(TILE) + hw, ty * double(TILE) + hh};
}

bool World::footprint_clear(const std::string& name, double cx, double cy,
                            const std::vector<EntityRef>* exclude) const {
    // Precise rectangle overlap against every existing building, not the
    // coarser building_tiles_ TILE set -- see all_building_rects_'s
    // comment for why a tile-based check alone can reject a placement
    // that has a genuine gap to its neighbors (e.g. two houses placed
    // right next to each other could still share a whole 32px terrain
    // tile that only partially touches each footprint).
    auto [w, h] = building_wh(name); // already native pixels, see building_wh's comment
    double hw = w / 2.0, hh = h / 2.0;
    double fcy = cy + footprint_dy(name); // see footprint_dy's comment
    double x0 = cx - hw, x1 = cx + hw, y0 = fcy - hh, y1 = fcy + hh;
    for (const auto& r : all_building_rects_) {
        if (x0 < r[2] && x1 > r[0] && y0 < r[3] && y1 > r[1]) return false;
    }
    for (auto [tx, ty] : building_tile_list(name, cx, cy)) {
        auto key = tile_key(tx, ty);
        if (resource_tiles_.count(key)) return false;
        if (!(tx >= 0 && tx < cols && ty >= 0 && ty < rows)) return false;
        bool water = terrain[tx][ty] == WATER;
        if (name == "shipyard") {
            if (!water) return false;
        } else if (water) {
            return false;
        }
    }
    if (name == "shipyard") {
        // Shore rule: the footprint sits on water (above, so ships launch), but
        // it must ALSO have at least one adjacent LAND tile -- a shipyard is a
        // coastal dock hugging the shore, not a platform floating in open ocean.
        // (Villagers build it from the neighbouring land; ships sail off the
        // water it occupies.)
        bool touches_land = false;
        for (auto [tx, ty] : building_tile_list(name, cx, cy)) {
            for (int dx = -1; dx <= 1 && !touches_land; ++dx) {
                for (int dy = -1; dy <= 1 && !touches_land; ++dy) {
                    int nx = tx + dx, ny = ty + dy;
                    if (nx < 0 || nx >= cols || ny < 0 || ny >= rows) continue;
                    if (terrain[nx][ny] != WATER) touches_land = true;
                }
            }
            if (touches_land) break;
        }
        if (!touches_land) return false;
    }
    // A unit standing where you're trying to build blocks placement --
    // you have to move it out of the way first -- for every building
    // EXCEPT a farm, which units can freely walk over/through (see
    // spawn_building's b.solid), so one standing there is never a real
    // spatial conflict.
    if (name != "farm") {
        for (auto ref : active_units) {
            const Unit* u = units.get(ref);
            if (!u || !u->common.alive) continue;
            if (exclude && std::find(exclude->begin(), exclude->end(), ref) != exclude->end())
                continue; // this unit is one of the build crew -- it steps off
            if (u->common.x > x0 && u->common.x < x1 && u->common.y > y0 && u->common.y < y1) {
                return false;
            }
        }
    }
    return true;
}

bool World::footprint_clear_gap(const std::string& name, double cx, double cy, double gap,
                                const std::vector<EntityRef>* exclude) const {
    if (!footprint_clear(name, cx, cy, exclude)) return false;
    if (gap <= 0.0) return true;
    // Same rectangle test footprint_clear runs, but with the candidate's own
    // footprint inflated by `gap` on every side, so a spot only passes when it
    // leaves that much walkable space to its neighbours. Nothing else is
    // re-checked -- a spot that got here already satisfied the terrain, shore
    // and standing-unit rules at its true size.
    // Only SOLID buildings need a lane kept around them. A farm is walk-through
    // (spawn_building: `b.solid = (name != "farm")`), so it can never box a unit
    // in -- and farms are the most numerous thing a mature base owns, so
    // demanding clearance around them buys no passability at all while pushing
    // every farm further from the town centre and lengthening the carry walk.
    // Hence this iterates live buildings and skips the non-solid ones rather
    // than using the cached all_building_rects_, which doesn't record solidity.
    auto [w, h] = building_wh(name);
    double hw = w / 2.0 + gap, hh = h / 2.0 + gap;
    double fcy = cy + footprint_dy(name);
    double x0 = cx - hw, x1 = cx + hw, y0 = fcy - hh, y1 = fcy + hh;
    for (auto ref : active_buildings) {
        const Building* b = buildings.get(ref);
        if (!b || !b->common.alive || !b->solid) continue;
        auto r = footprint_rect(*b);
        if (x0 < r[2] && x1 > r[0] && y0 < r[3] && y1 > r[1]) return false;
    }
    return true;
}

bool World::footprint_explored(const std::string& name, double cx, double cy) const {
    // Buildable in visible (2) or explored/"light" fog (1); blocked only in
    // unexplored/"dark" fog (0), where the player has no idea what's there
    // yet. world.fog only tracks team 0's vision (world.h's comment), so
    // this check only makes sense -- and is only applied -- for the local
    // player's own placements, never the AI's.
    for (auto [tx, ty] : building_tile_list(name, cx, cy)) {
        if (tx < 0 || tx >= static_cast<int>(fog.size()) || ty < 0 ||
            ty >= static_cast<int>(fog[0].size()) || fog[tx][ty] == 0) {
            return false;
        }
    }
    return true;
}

bool World::at_dropoff(const Building& b, double ux, double uy) const {
    // Small buffer (not 0) rather than an exact touch: step_toward moves in
    // discrete per-tick steps, so a unit walking up to the edge generally
    // lands a few px short of/past it rather than exactly on it. This used
    // to be foot_w/h*0.5 + TILE*2 (a 2-tile buffer, ~64px at the current
    // TILE=32) -- units could gather/build/repair from way outside the
    // building's actual footprint. Now that passable() stops units at the
    // building's real geometric edge (solid_building_rects_), a small
    // fixed buffer is all that's needed.
    double hw = b.foot_w * 0.5 + 8.0, hh = b.foot_h * 0.5 + 8.0;
    double fcy = b.common.y + footprint_dy(b.name); // see footprint_dy's comment
    return std::abs(ux - b.common.x) < hw && std::abs(uy - fcy) < hh;
}

std::array<double, 4> World::footprint_rect(const Building& b) const {
    double hw = b.foot_w * 0.5, hh = b.foot_h * 0.5;
    double fcy = b.common.y + footprint_dy(b.name); // see footprint_dy's comment
    return {b.common.x - hw, fcy - hh, b.common.x + hw, fcy + hh};
}

bool World::inside_footprint(const Building& b, double px, double py) const {
    auto r = footprint_rect(b);
    // STRICT interior, exactly like footprint_clear's unit test: a builder
    // parked right ON the edge -- which is precisely where
    // advance_to_building walks it (perimeter_candidates' points all sit on
    // the footprint boundary) -- counts as beside the building, not on it,
    // so it never blocks its own foundation.
    return px > r[0] && px < r[2] && py > r[1] && py < r[3];
}

std::vector<EntityRef> World::units_on_footprint(const Building& b) const {
    std::vector<EntityRef> out;
    auto r = footprint_rect(b);
    // Grid radius: half the footprint's diagonal at most, and foot_w/foot_h
    // are each >= that half-extent, so the larger of the two always covers
    // the whole rect from its centre.
    double reach = std::max(b.foot_w, b.foot_h);
    for (auto ref : grid.query((r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5, reach)) {
        if (ref.kind != EntityKind::Unit) continue;
        const Unit* u = units.get(ref);
        // Aircraft fly over a footprint rather than standing on it, so they
        // never obstruct construction. Garrisoned passengers are off the
        // board entirely and aren't in the grid to begin with (see
        // rebuild_spatial_grid).
        if (!u || !u->common.alive || u->common.is_air) continue;
        if (inside_footprint(b, u->common.x, u->common.y)) out.push_back(ref);
    }
    return out;
}

std::pair<double, double> World::point_off_footprint(const Building& b, double px, double py,
                                                      bool is_air, bool is_ship) const {
    auto r = footprint_rect(b);
    // Leave by whichever edge is nearest -- the shortest way out is also
    // the least likely to cross the rest of a crowded base -- with a small
    // margin so the unit ends up unambiguously OUTSIDE rather than sitting
    // exactly on the boundary.
    constexpr double kMargin = 6.0;
    double out_x = r[0] - kMargin, out_y = py, best = px - r[0];
    if (r[2] - px < best) { best = r[2] - px; out_x = r[2] + kMargin; out_y = py; }
    if (py - r[1] < best) { best = py - r[1]; out_x = px; out_y = r[1] - kMargin; }
    if (r[3] - py < best) { best = r[3] - py; out_x = px; out_y = r[3] + kMargin; }
    return nearest_passable(out_x, out_y, is_air, is_ship);
}

std::pair<double, double> World::clear_point_near(double cx, double cy, double dist,
                                                   bool ship) const {
    auto ok = [&](double px, double py) {
        int tx = static_cast<int>(std::floor(px / TILE)), ty = static_cast<int>(std::floor(py / TILE));
        if (!(tx >= 0 && tx < cols && ty >= 0 && ty < rows)) return false;
        if (occupied_.count(tile_key(tx, ty))) return false;
        return (terrain[tx][ty] == WATER) == ship;
    };
    if (ok(cx, cy)) return {cx, cy};
    for (double r : {dist, dist * 2}) {
        for (int ang : {90, 135, 45, 180, 0, 225, 315, 270}) {
            double px = cx + std::cos(ang * M_PI / 180.0) * r;
            double py = cy + std::sin(ang * M_PI / 180.0) * r;
            if (ok(px, py)) return {px, py};
        }
    }
    return {cx, cy + dist};
}

// ---- generic entity resolution ----

EntityCommon* World::common(EntityRef r) {
    switch (r.kind) {
        case EntityKind::Unit: { auto* e = units.get(r); return e ? &e->common : nullptr; }
        case EntityKind::Building: { auto* e = buildings.get(r); return e ? &e->common : nullptr; }
        case EntityKind::Resource: { auto* e = resources.get(r); return e ? &e->common : nullptr; }
        case EntityKind::Deer: { auto* e = deer.get(r); return e ? &e->common : nullptr; }
        case EntityKind::Projectile: { auto* e = projectiles.get(r); return e ? &e->common : nullptr; }
        default: return nullptr;
    }
}

void World::hurt(EntityRef target, double dmg, bool shake) {
    // Every point of damage anything takes refreshes its owner's "under fire"
    // clock -- the one unambiguous signal that a team is being ATTACKED rather
    // than merely being walked past (see Team::ai_under_fire). Stamped here, at
    // the single chokepoint every damage source already funnels through, so
    // projectiles, melee, bombs, fire DoT and blasts all count without each
    // call site having to remember to report it.
    //
    // The same chokepoint records WHERE the team was last hit, which is what the
    // "you are under attack" alert needs -- but it deliberately does NOT emit the
    // alert from here. hurt() is the damage primitive: it runs from inside
    // projectile impacts, melee swings, splash loops, fire damage-over-time and
    // the AI's own foundation watchdog, several frames deep. Pushing a
    // heap-allocating SimEvent from that depth was measurably unsafe -- it
    // produced intermittent heap corruption in long headless runs (100% on some
    // builds, 0% on others from identical source, which is the signature of the
    // layout-sensitive UB it was tripping). Stamping two doubles and a flag
    // allocates nothing and cannot.
    //
    // The event itself is raised once per tick from Control::update_ai, which is
    // an ordinary top-level per-tick hook that already emits events safely.
    constexpr double kUnderFireWindow = 20.0; // seconds
    auto mark_under_fire = [&](int team, double wx, double wy) {
        if (team < 0 || team >= static_cast<int>(control.teams.size())) return;
        Team& t = control.teams[team];
        t.ai_under_fire = kUnderFireWindow;
        t.warn_x = wx;
        t.warn_y = wy;
        t.warn_pending = true;
    };
    if (Unit* u = get(target)) {
        if (u->diving) return; // already spiralling down
        mark_under_fire(u->common.team, u->common.x, u->common.y);
        u->common.hp -= dmg;
        if (shake) u->hit_timer = 0.35;
        u->common.dmg_flash = 1.0; // float the HP bar for ~1s
        if (u->common.hp <= 0) {
            u->common.hp = 0;
            if (u->common.is_air) u->diving = true;
            else u->common.alive = false;
        }
    } else if (Building* b = get_building(target)) {
        mark_under_fire(b->common.team, b->common.x, b->common.y);
        b->common.hp -= dmg;
        // `shake` is false for continuous fire DoT so a burning building doesn't
        // judder the whole time it's alight (hit_timer drives the shudder).
        if (shake) b->hit_timer = 0.35;
        b->common.dmg_flash = 1.0; // float the HP bar for ~1s
        if (b->common.hp <= 0) {
            b->common.hp = 0;
            b->common.alive = false;
        }
    }
}

double World::armor_of(EntityRef r) {
    if (Unit* u = get(r)) return u->armor;
    if (Building* b = get_building(r)) return b->armor;
    return 0;
}
double World::pierce_of(EntityRef r) {
    if (Unit* u = get(r)) return u->pierce;
    if (Building* b = get_building(r)) return b->pierce;
    return 0;
}
double World::foot_px_of(EntityRef r) {
    if (Building* b = get_building(r)) return b->foot_px;
    return 0;
}

EntityRef World::nearest(double x, double y, double radius,
                          const std::function<bool(EntityRef, EntityCommon&)>& pred) {
    // Reuse one buffer across every nearest() call this process makes -- combat
    // target-acquisition alone fires this thousands of times a step in a dense
    // battle, and a fresh heap vector per call was pure malloc churn. Same
    // entities/order as grid.query(), so determinism is untouched. nearest()
    // never nests another query, so a single shared buffer is safe.
    static thread_local std::vector<EntityRef> buf;
    grid.query_into(x, y, radius, buf);
    EntityRef best = kNullRef;
    double bestd = radius * radius;
    for (auto ref : buf) {
        EntityCommon* c = common(ref);
        if (!c || !pred(ref, *c)) continue;
        double dx = c->x - x, dy = c->y - y;
        double d = dx * dx + dy * dy;
        if (d < bestd) { bestd = d; best = ref; }
    }
    return best;
}

EntityRef World::nearest_dropoff(int team, double x, double y, int carry_type, EntityRef exclude, bool naval) {
    EntityRef best = kNullRef;
    double bestd = 1e18;
    for (auto ref : active_buildings) {
        Building* b = buildings.get(ref);
        // COMPLETE only. `is_dropoff` is stamped at spawn_building time from
        // the name alone, so a house/refinery FOUNDATION carried it from the
        // instant it was placed -- and nothing here checked, so villagers
        // happily banked resources into a building that did not exist yet.
        // The client's own right-click picker has always required `complete`
        // (see GameClient's dropoff_building), so this was a sim-side-only
        // hole: it fed the automatic delivery every gatherer uses, which in
        // practice meant the AI got free drop-offs the player could not use.
        if (!b || b->common.team != team || !b->is_dropoff || !b->common.alive || !b->complete)
            continue;
        // `exclude` lets a carrier that has repeatedly failed to reach one
        // drop-off ask for the next-best instead of being handed the same
        // unreachable building forever (see update_gather's delivery ladder).
        if (exclude.valid() && ref == exclude) continue;
        // A NAVAL carrier (fishing boat) can only unload at a dock: the SHIPYARD
        // or the base -- never a land house/refinery (a boat can't sail to one).
        // Conversely a LAND gatherer never routes to a water shipyard.
        if (naval) {
            if (b->name != "base" && b->name != "shipyard") continue;
        } else {
            if (b->name == "shipyard") continue;
            // Resource-type dropoff rules: the refinery only accepts OIL (2) and
            // IRON (3); the house only accepts FOOD (0) and WOOD (1) -- oil/iron
            // need a refinery (or the base). The base accepts everything.
            if (b->name == "refinery" && carry_type >= 0 && carry_type != 2 && carry_type != 3) continue;
            if (b->name == "house" && (carry_type == 2 || carry_type == 3)) continue;
        }
        double dx = b->common.x - x, dy = b->common.y - y;
        double d = dx * dx + dy * dy;
        if (d < bestd) { bestd = d; best = ref; }
    }
    return best;
}

bool World::can_age_up(int team) {
    if (team < 0 || team >= static_cast<int>(control.teams.size())) return false;
    const Team& t = control.teams[team];
    // Number of this team's COMPLETED buildings whose name is in `names`.
    auto count = [&](const std::unordered_set<std::string>& names) {
        int n = 0;
        for (auto ref : active_buildings) {
            Building* b = get_building(ref);
            if (!b || !b->common.alive || !b->complete || b->common.team != team) continue;
            if (names.count(b->name)) ++n;
        }
        return n;
    };
    switch (t.era) {
    case 0: {
        // -> Industrial: any 2 real era-0 structures. The always-present town
        // centre (base), population housing (house), food (farm), the walls
        // (palisade / iron wall), the cheap scout tower (outpost) and the
        // defensive tower don't count -- the gate is about committing to a
        // production/economy tech base.
        static const std::unordered_set<std::string> req = {
            "barracks", "academy", "market", "refinery", "shipyard"};
        return count(req) >= 2;
    }
    case 1: {
        // -> War: any 2 buildings first unlocked in the Industrial era. The
        // aa tower is a defensive structure and doesn't count (same as the
        // plain tower is excluded from the Industrial tier).
        static const std::unordered_set<std::string> req = {
            "factory", "university", "airbase"};
        return count(req) >= 2;
    }
    case 2: {
        // -> Scientific: any 2 War-era buildings (an expansion base and/or a
        // fortress -- the starting base counts as one), OR a single fortress
        // on its own as the direct military route.
        static const std::unordered_set<std::string> war = {"base", "fortress"};
        static const std::unordered_set<std::string> fort = {"fortress"};
        return count(war) >= 2 || count(fort) >= 1;
    }
    default:
        return true; // already at the last era -- nothing left to gate
    }
}

EntityRef World::next_wall_segment(int team, const std::string& name, double x, double y, double radius,
                                   EntityRef exclude) {
    double r2 = radius * radius;
    EntityRef best_any = kNullRef, best_free = kNullRef;
    double bd_any = 1e18, bd_free = 1e18;
    for (auto ref : active_buildings) {
        if (ref == exclude) continue;
        Building* b = get_building(ref);
        if (!b || !b->common.alive || b->complete) continue;
        if (b->common.team != team || b->name != name) continue;
        double dx = b->common.x - x, dy = b->common.y - y;
        double d2 = dx * dx + dy * dy;
        if (d2 > r2) continue;
        if (d2 < bd_any) { bd_any = d2; best_any = ref; }
        // Prefer a segment no other living unit is already building.
        bool taken = false;
        for (auto uref : active_units) {
            Unit* ou = units.get(uref);
            if (ou && ou->common.alive && ou->build_target == ref) { taken = true; break; }
        }
        if (!taken && d2 < bd_free) { bd_free = d2; best_free = ref; }
    }
    return best_free.valid() ? best_free : best_any;
}

bool World::enqueue(EntityRef building_ref, const std::string& item, bool priority) {
    Building* b = get_building(building_ref);
    if (!b || !b->complete) return false;
    Team& team = control.teams[b->common.team];

    if (control.is_tech(item)) {
        if (team.tech.count(item)) return false;
        // A tech is TEAM-wide, so having it queued anywhere on the team already
        // is enough to refuse -- this used to check only THIS building's queue,
        // which let the same tech be paid for once per building: select three
        // universities and click a tech and activate_card_button enqueues it at
        // each of them, charging three times for one team-wide effect (the 2nd
        // and 3rd completions hit the `team.tech.count` guard in Control::
        // research and silently evaporate). Also what keeps the hidden-while-
        // researching tech icon honest for the paths that don't go through the
        // command card, e.g. the shipyard's other-page hotkey.
        for (auto ref : active_buildings) {
            const Building* ob = get_building(ref);
            if (!ob || !ob->common.alive || ob->common.team != b->common.team) continue;
            if (std::find(ob->queue.begin(), ob->queue.end(), item) != ob->queue.end()) return false;
        }
        if (!control.afford(item, b->common.team)) {
            if (b->common.team == 0) {
                events.push({EventType::Sound, "error", b->common.x, b->common.y, 0, kNullRef, ""});
                events.push({EventType::Warn, "", 0, 0, 0, kNullRef, "You need more resources!"});
            }
            return false;
        }
        control.pay(item, b->common.team);
        b->queue.push_back(item);
        return true;
    }

    bool is_age = std::find(AGE_ITEMS.begin(), AGE_ITEMS.end(), item) != AGE_ITEMS.end();
    if (is_age) {
        // Campaign whole-level age cap (Team::max_era): once a team has reached
        // the cap it cannot enqueue any further age-up. Covers human and AI
        // (both route through here). max_era < 0 means "no cap".
        if (team.max_era >= 0 && team.era >= team.max_era) return false;
        int idx = static_cast<int>(std::find(AGE_ITEMS.begin(), AGE_ITEMS.end(), item) - AGE_ITEMS.begin());
        bool any_age_in_queue = std::any_of(b->queue.begin(), b->queue.end(), [](const std::string& q) {
            return std::find(AGE_ITEMS.begin(), AGE_ITEMS.end(), q) != AGE_ITEMS.end();
        });
        bool already_queued = std::find(b->queue.begin(), b->queue.end(), item) != b->queue.end();
        if (idx < team.era || already_queued || any_age_in_queue) return false;
        // Building prerequisite for the next age (see World::can_age_up): gates
        // human and AI alike, since both route their age-ups through here.
        if (!can_age_up(b->common.team)) {
            if (b->common.team == 0) {
                events.push({EventType::Sound, "error", b->common.x, b->common.y, 0, kNullRef, ""});
                events.push({EventType::Warn, "", 0, 0, 0, kNullRef,
                             "You need more buildings to advance to the next era!"});
            }
            return false;
        }
    }
    if (!control.afford(item, b->common.team)) {
        if (b->common.team == 0) {
            events.push({EventType::Sound, "error", b->common.x, b->common.y, 0, kNullRef, ""});
            events.push({EventType::Warn, "", 0, 0, 0, kNullRef, "You need more resources!"});
        }
        return false;
    }
    if (!is_age && team.pop + pop_cost(item) > team.cap) {
        if (b->common.team == 0) {
            events.push({EventType::Sound, "pop_full", b->common.x, b->common.y, 0, kNullRef, ""});
            events.push({EventType::Warn, "", 0, 0, 0, kNullRef, "You need to build more houses!"});
        }
        return false;
    }
    control.pay(item, b->common.team);
    if (priority && !b->queue.empty()) {
        b->queue.insert(b->queue.begin(), item);
        b->percent = 0.0;
        b->acc = 0.0;
    } else {
        b->queue.push_back(item);
    }
    return true;
}

bool World::cancel_queue(EntityRef building_ref, int index) {
    Building* b = get_building(building_ref);
    if (!b || index < 0 || index >= static_cast<int>(b->queue.size())) return false;
    std::string item = b->queue[index];
    Team& team = control.teams[b->common.team];
    for (auto& [k, v] : control.cost_of(item, b->common.team)) team.res[k] += v;
    b->queue.erase(b->queue.begin() + index);
    if (index == 0) {
        b->percent = 0.0;
        b->acc = 0.0;
    }
    return true;
}

EntityRef World::find_by_id(uint32_t id) {
    for (auto ref : active_units) if (get(ref) && get(ref)->common.id == id) return ref;
    for (auto ref : active_buildings) if (get_building(ref) && get_building(ref)->common.id == id) return ref;
    for (auto ref : active_resources) if (get_resource(ref) && get_resource(ref)->common.id == id) return ref;
    for (auto ref : active_deer) if (get_deer(ref) && get_deer(ref)->common.id == id) return ref;
    for (auto ref : active_projectiles) if (get_projectile(ref) && get_projectile(ref)->common.id == id) return ref;
    return kNullRef;
}

void World::order_move(EntityRef ref, double x, double y, bool from_queue, double group_speed_px) {
    Unit* u = get(ref);
    if (!u) return;
    if (!from_queue) { u->order_queue.clear(); u->active_queue_watch = kNullRef; u->queue_active = false; }
    u->group_speed_px = group_speed_px;
    // If the exact clicked point is unreachable (inside a building/solid
    // resource), redirect to the nearest reachable point next to it --
    // otherwise the unit just walks up to the obstacle and slides along
    // its edge forever (see update_unit's give-up check for the other
    // half of this fix, covering points that only became unreachable
    // after the order was issued, e.g. another unit already standing
    // there).
    auto [tx, ty] = nearest_passable(x, y, u->common.is_air, u->common.is_ship);
    u->move_goal = Vec2{tx, ty};
    u->attack_target = kNullRef;
    u->gather_target = kNullRef;
    // A plain move order is an explicit "stop gathering" -- clearing just
    // gather_target isn't enough, since gather_rtype (set by order_gather,
    // used by update_gather to re-seek the same resource type if the exact
    // target disappears) would otherwise survive the move. Once the unit
    // arrived and went idle, THAT re-seek logic saw an empty gather_target,
    // still-set gather_rtype, and happily walked the unit right back to
    // its old job -- looking like the move order had been silently ignored.
    u->gather_rtype = -1;
    u->drop_target = kNullRef;
    u->build_target = kNullRef;
    u->repair_target = kNullRef;
    u->load_target = kNullRef; // explicit move cancels a pending transport board
    u->rally.reset();
    u->hold.reset();
    u->forced = false;
    u->need_path = true;
    u->path.clear();
    u->path_i = 0;
    u->progress_check_t = 0.0;
    u->last_goal_dist = -1.0;
    u->stall_strikes = 0;
}

bool World::unload_transport(EntityRef ship_ref, double wx, double wy) {
    Unit* ship = get(ship_ref);
    if (!ship || !ship->common.alive || ship->transport_cap <= 0 || ship->cargo.empty())
        return false;

    // The ship must be hugging the coast: some land tile within ~1 tile of it.
    bool near_land = false;
    for (int dy = -1; dy <= 1 && !near_land; ++dy)
        for (int dx = -1; dx <= 1 && !near_land; ++dx)
            if (tile_at(ship->common.x + dx * TILE, ship->common.y + dy * TILE) >= 0 &&
                !is_water(ship->common.x + dx * TILE, ship->common.y + dy * TILE))
                near_land = true;
    if (!near_land) return false;

    // The drop point must be a shoreline: on land, and touching an ocean tile
    // (so troops can't be teleported into the dry middle of an island).
    if (tile_at(wx, wy) < 0 || is_water(wx, wy)) return false;
    bool touches_ocean = false;
    for (int dy = -1; dy <= 1 && !touches_ocean; ++dy)
        for (int dx = -1; dx <= 1 && !touches_ocean; ++dx)
            if ((dx || dy) && is_water(wx + dx * TILE, wy + dy * TILE)) touches_ocean = true;
    if (!touches_ocean) return false;

    // And it has to be within reach of the ship -- no cross-map drops.
    if (std::hypot(wx - ship->common.x, wy - ship->common.y) > 5.0 * TILE) return false;

    // Disgorge a SQUAD at a time, not the whole hold. The old code fanned every
    // passenger out on a ring whose radius grew with the index
    // ((0.6 + 0.5*i)*TILE) -- so a full transport (cap 50) flung its last troops
    // ~25 tiles away and nearest_passable scattered them across the map. Now we
    // drop at most kUnloadBatch per call on a TIGHT ring and leave the rest
    // aboard; the player re-clicks (and the AI re-issues while it's still hugging
    // the beach) to land the next batch, so a landing arrives as clustered waves.
    constexpr int kUnloadBatch = 10;
    int count = std::min<int>(kUnloadBatch, static_cast<int>(ship->cargo.size()));
    for (int i = 0; i < count; ++i) {
        Unit* cu = get(ship->cargo[i]);
        if (!cu) continue;
        double ang = i * 2.399963;               // golden-angle spread
        double rad = (i == 0) ? 0.0 : (0.5 + 0.3 * i) * TILE; // tight: ~0.8..3.2 tiles
        auto [tx, ty] = nearest_passable(wx + std::cos(ang) * rad, wy + std::sin(ang) * rad,
                                         /*is_air=*/false, /*is_ship=*/false);
        cu->carrier = kNullRef;
        cu->common.alive = true;
        cu->common.x = tx;
        cu->common.y = ty;
        cu->move_goal.reset();
        cu->path.clear();
        cu->path_i = 0;
        cu->hold.reset();
        cu->attack_target = kNullRef;
    }
    ship->cargo.erase(ship->cargo.begin(), ship->cargo.begin() + count);
    if (ship->cargo.empty()) ship->unload_point.reset();
    return true;
}

void World::order_gather(EntityRef ref, EntityRef target, bool from_queue) {
    Unit* u = get(ref);
    if (!u) return;
    if (!from_queue) { u->order_queue.clear(); u->active_queue_watch = kNullRef; u->queue_active = false; }
    // Resolve the NEW target's resource type up front so a reassignment to
    // a different type (e.g. wood -> food) takes effect immediately: drop
    // whatever they were carrying and clear any pending drop_target, rather
    // than letting update_gather's "carry full / already have a drop_target"
    // branch keep forcing them to finish delivering the OLD resource to a
    // dropoff first -- which is what made switching a loaded villager to a
    // different resource look like it was silently refusing to take effect.
    // Same-type reassignment (e.g. one wood tree to another) keeps the
    // carry as-is, same as before.
    int new_rtype = -1;
    if (target.kind == EntityKind::Resource) {
        if (Resource* r = get_resource(target)) new_rtype = r->res.rtype;
    } else if (target.kind == EntityKind::Building) {
        new_rtype = 0; // the only Building gather_target is a farm, food-only
    }
    if (u->carry > 0 && new_rtype >= 0 && u->carry_type != new_rtype) {
        u->carry = 0;
        u->drop_target = kNullRef;
    }
    u->gather_rtype = new_rtype;
    u->gather_target = target;
    u->attack_target = kNullRef;
    u->move_goal.reset();
    u->build_target = kNullRef;
    u->forced = false;
    u->gather_progress_check_t = 0.0;
    u->gather_last_dist = -1.0;
}

void World::order_attack(EntityRef ref, EntityRef target, bool from_queue) {
    Unit* u = get(ref);
    if (!u) return;
    if (!from_queue) { u->order_queue.clear(); u->active_queue_watch = kNullRef; u->queue_active = false; }
    u->attack_target = target;
    u->forced = true;
    u->move_goal.reset();
    u->rally.reset();
    u->gather_target = kNullRef;
    u->gather_rtype = -1; // see order_move's comment: an explicit new order supersedes gathering entirely
    u->build_target = kNullRef;
}

bool World::queue_order(EntityRef ref, QueuedOrder order) {
    Unit* u = get(ref);
    if (!u) return false;
    if (u->order_queue.size() >= kMaxQueuedOrders) return false; // ignore the extra click
    u->order_queue.push_back(order);
    return true;
}

void World::add_resource(int team, int rtype, double amount) {
    static const char* names[4] = {"food", "wood", "oil", "iron"};
    if (team >= 0 && static_cast<size_t>(team) < control.teams.size()) {
        control.teams[team].res[names[rtype]] += amount;
        // AI-comparison metric: raw resources actually harvested, distinct
        // from the fluctuating res[] stockpile above -- see Team::
        // total_gathered's comment. This is the ONLY place gathered income
        // lands (trade converts existing resources instead, see Control::
        // trade), so it's the single correct choke point for this counter.
        control.teams[team].total_gathered[names[rtype]] += amount;
    }
}

// ---- spawning ----

Unit World::make_unit(const std::string& name_in, int team, double x, double y) const {
    std::string name = name_in;
    auto sub = CIV_UNIT_SUB.find({control.teams[team].civ, name});
    if (sub != CIV_UNIT_SUB.end()) name = sub->second;

    Unit u;
    if (!data.catalog().at("units").contains(name)) return u; // caller checks catalog first
    const auto& stats = data.catalog().at("units").at(name);

    u.common.kind = EntityKind::Unit;
    u.common.team = team;
    u.common.x = x;
    u.common.y = y;
    u.common.max_hp = stats.value("max_life", 1.0);
    u.common.hp = u.common.max_hp;
    u.common.is_air = stats.value("aerial", false);
    u.common.is_ship = stats.value("ship", false);
    u.common.is_solid = true;
    u.name = name;
    u.sprite = stats.value("sprite_index", "");
    // Civ-specific unit skins (game/control.py CIV_FIGHTER/CIV_BOMBER, extended
    // to jets and to the Soviet tank): SAME stats, a nation's own art. Only the
    // DRAWN sprite changes -- u.name stays the generic type ("fighter"/"tank"),
    // so behaviour and the death-wreck lookup (falls back to spr_<name>_rubble/
    // destroyed) still resolve. Civs with no listed skin keep the catalog's
    // generic sprite_index.
    {
        static const std::map<std::pair<int, std::string>, std::string> kCivSkin = {
            {{0, "fighter"}, "spr_spitfire"},     {{1, "fighter"}, "spr_mustang"},
            {{2, "fighter"}, "spr_messerschmitt"}, {{4, "fighter"}, "spr_zero"},
            {{0, "jet fighter"}, "spr_meteor"},   {{1, "jet fighter"}, "spr_shooting_star"},
            {{2, "jet fighter"}, "spr_me262"},    {{4, "jet fighter"}, "spr_white_zero"},
            {{0, "bomber"}, "spr_lancaster_bomber"},
            {{0, "heavy bomber"}, "spr_lancaster_bomber"},
            {{3, "tank"}, "spr_tank_soviet"},           // Soviet Union: reskinned base War-era tank
            {{3, "heavy tank"}, "spr_heavy_tank_soviet"}, // Soviet Union: reskinned heavy tank (upgrade)
        };
        auto skin = kCivSkin.find({control.teams[team].civ, name});
        if (skin != kCivSkin.end()) u.sprite = skin->second;
    }
    u.armor = stats.value("armor", 0);
    u.pierce = stats.value("pierce", 0);
    // catalog "speed" is GML's raw px/step value (game/scripts/get_stats.gml,
    // e.g. civilian=1, cavalry=1.5). Converting to px/sec requires the
    // actual GML room_speed -- assets/gmk/objects/control/Step.gml:501 sets
    // it to 60 unconditionally during normal play (only 600 under the
    // Ctrl+Space turbo cheat), not the 30fps both this port and
    // game/entity.py's ROOM_SPEED assumed, which made every unit move at
    // half its true GameMaker speed.
    u.speed_px = stats.value("speed", 1.0) * 60.0;
    // Same 30-vs-60 room_speed correction as speed_px just above -- "reload"
    // is also a raw GML frame count (civilian=60, i.e. one gather tick/sec),
    // so converting to seconds must divide by the real 60fps rate too. This
    // was still dividing by 30, making every unit's attack AND gather tick
    // take twice as long in wall-clock time as the original GML (half rate).
    u.reload = stats.value("reload", 30.0) / 60.0;
    // catalog "range"/"sight" are in GML tiles; TILE now equals GML's own
    // 32px grid, so this is a straight tile->px conversion (previously a
    // bare 32.0 literal, since it was already correct independent of
    // TILE's old, mismatched 24 value).
    u.range_px = stats.value("range", 0.0) * TILE;
    u.min_range_px = stats.value("min_range", 0.0) * TILE; // artillery etc. can't hit point-blank
    u.accuracy = stats.value("accuracy", 1.0);             // <1 scatters shells (artillery)
    // A unit must always SEE at least as far as it can SHOOT. Four catalog
    // entries ship with range > sight (artillery 10/6, artillery1 8/6,
    // infantryman 5/3, rifleman 4/3) -- and rifleman + infantryman are far and
    // away the most-produced military units, so this was the common case, not a
    // corner one. update_combat already auto-acquires out to max(sight, range)
    // (see unit_behavior.cpp), but sight_px alone drives the fog reveal in
    // reveal_fog() and the AI's threat/group radii in control_ai.cpp, so those
    // units shot at things the player could not see and the AI did not count as
    // threats. Clamping here fixes every consumer at once and leaves units whose
    // sight already exceeds their range (scouts, bombers, carriers) untouched.
    u.sight_px = std::max(stats.value("sight", 5.0), stats.value("range", 0.0)) * TILE;
    u.attack = stats.value("attack", 0.0);
    u.melee = stats.value("melee", false);
    u.is_bomber = (name == "bomber" || name == "heavy bomber" || name == "b29");
    // AA (air-only) firers: the AA gun, its Flak upgrade, and the Supercarrier
    // (its single deck gun only ever engages aircraft). The base aircraft
    // carrier has no weapon at all (attack 0 -> update_combat skips it).
    u.is_aa = (name == "aa gun" || name == "flak" || name == "aircraft carrier2");
    u.is_ballistic = (name == "ballistic missile");
    if (u.is_ballistic) { u.packed = true; u.pack_t = 0.0; u.pack_target = true; } // spawns mobile/packed
    static const std::set<std::string> mechanical_names = {
        "light tank", "tank", "heavy tank", "tiger tank", "tiger2 tank", "ballistic missile",
        "artillery", "artillery1", "aa gun", "flak", "nuclear reactor"};
    u.mechanical = u.common.is_air || u.common.is_ship || mechanical_names.count(name);
    u.clip_max = stats.value("clip_size", 0);
    u.clip_ammo = u.clip_max;
    u.clip_reload = stats.value("clip_reload", 0.0) / 60.0;
    u.fuel_max = stats.value("fuel_max", 100.0);
    u.fuel = u.fuel_max;
    u.fuel_latency = stats.value("fuel_latency", 27.5);
    u.turn_speed = stats.value("turn_speed", 2.0);
    u.bullet_sprite = stats.value("bullet_sprite", "spr_bullet");
    u.is_gatherer = (name == "civilian" || name == "fishing boat");
    u.max_carry = stats.value("max_carry", 10.0);
    u.transport_cap = stats.value("capacity", 0);          // >0 -> amphibious troop carrier
    u.air_capacity = stats.value("air_capacity", 0);        // >0 -> aircraft carrier (mobile airbase)
    u.is_carrier = u.air_capacity > 0;
    // Isoroku Yamamoto: aircraft carriers hold +1 plane.
    if (u.is_carrier && bonuses.leader_name(control.teams[team].civ, control.teams[team].leader) ==
                            "Isoroku Yamamoto")
        u.air_capacity += 1;

    u.base_attack = u.attack; // snapshot before civ/tech bonuses (UI shows base+delta)
    const Team& t = control.teams[team];
    bonuses.apply_unit(u, t.civ, t.leader, t.era, t.tech);
    return u;
}

EntityRef World::spawn_unit(const std::string& name, int team, double x, double y) {
    if (!data.catalog().at("units").contains(name)) {
        // still need to check the civ-substituted name, so just delegate
        auto sub = CIV_UNIT_SUB.find({control.teams[team].civ, name});
        if (sub == CIV_UNIT_SUB.end() || !data.catalog().at("units").contains(sub->second)) {
            return kNullRef;
        }
    }
    Unit u = make_unit(name, team, x, y);
    u.common.id = next_id++;
    uint32_t slot = units.insert(std::move(u));
    EntityRef ref{EntityKind::Unit, slot, units.generation_of(slot)};
    active_units.push_back(ref);
    return ref;
}

void World::transform_unit(EntityRef ref, const std::string& new_name) {
    Unit* u = get(ref);
    if (!u || !data.catalog().at("units").contains(new_name)) return;
    double frac = u->common.hp / std::max(1.0, u->common.max_hp);
    uint32_t id = u->common.id;

    Unit nu = make_unit(new_name, u->common.team, u->common.x, u->common.y);
    nu.common.id = id;
    nu.common.hp = std::max(1.0, nu.common.max_hp * frac);
    *u = std::move(nu);
}

void World::transform_building(EntityRef ref, const std::string& new_name) {
    Building* b = get_building(ref);
    if (!b || !data.catalog().at("buildings").contains(new_name)) return;
    const auto& stats = data.catalog().at("buildings").at(new_name);
    double frac = b->common.hp / std::max(1.0, b->common.max_hp);
    b->name = new_name;
    b->sprite = stats.value("sprite_index", b->sprite);
    b->armor = stats.value("armor", b->armor);
    b->pierce = stats.value("pierce", b->pierce);
    b->attack = stats.value("attack", 0.0);
    b->range_px = stats.value("range", 0.0) * TILE;
    b->reload = stats.value("reload", 30.0) / 60.0;
    b->sight_px = stats.value("sight", 5.0) * TILE;
    b->is_aa = (new_name == "aa tower" || new_name == "aa gun tower");
    b->common.max_hp = stats.value("max_life", b->common.max_hp);
    b->common.hp = std::max(1.0, b->common.max_hp * frac);
    const Team& t = control.teams[b->common.team];
    bonuses.apply_building(*b, t.civ, t.leader, t.tech);
    // full_max_hp after the bonus, so an upgraded building tops up to its
    // bonused HP (see spawn_building for the same fix).
    b->full_max_hp = b->common.max_hp;
}

EntityRef World::spawn_building(const std::string& name, int team, double x, double y,
                                 bool constructing) {
    if (!data.catalog().at("buildings").contains(name)) return kNullRef;
    const auto& stats = data.catalog().at("buildings").at(name);

    Building b;
    b.common.id = next_id++;
    b.common.kind = EntityKind::Building;
    b.common.team = team;
    b.common.x = x;
    b.common.y = y;
    b.common.max_hp = stats.value("max_life", 1.0);
    b.common.hp = b.common.max_hp;
    b.name = name;
    b.sprite = stats.value("sprite_index", "");
    // The outpost is era-skinned: the Victorian-era watchtower in the early
    // ages, the War-era one from the War era (2) on.
    if (name == "outpost") {
        b.sprite = control.teams[team].era >= 2 ? "spr_outpost_war_era" : "spr_outpost_victorian_era";
    }
    // The base/town-centre has no generic sprite -- it's civ-specific, so a
    // player-BUILT base (base is buildable in the War era) gets the owner's own
    // capitol sprite, same map the starting base uses (scenario.cpp CIV_BASE).
    if (name == "base") {
        static const char* kCivBase[9] = {
            "spr_uk_base",     "spr_capitol",   "spr_nazi_base",
            "spr_soviet_base", "spr_japan_base", "spr_italy_base",
            "spr_france_base", "spr_china_base", "spr_ottoman_base"};
        int civ = control.teams[team].civ;
        b.sprite = (civ >= 0 && civ < 9) ? kCivBase[civ] : "spr_uk_base";
    }
    b.armor = stats.value("armor", 0);
    b.pierce = stats.value("pierce", 0);
    // Defensive-structure combat stats (tower/fortress/aa tower have these in
    // the catalog; every other building leaves attack at 0 == doesn't shoot).
    b.attack = stats.value("attack", 0.0);
    b.range_px = stats.value("range", 0.0) * TILE;
    b.reload = stats.value("reload", 30.0) / 60.0;
    b.sight_px = stats.value("sight", 5.0) * TILE;
    b.is_aa = (name == "aa tower" || name == "aa gun tower");
    b.complete = !constructing;
    b.construction = constructing ? 0.0 : 100.0;
    b.is_dropoff = (name == "base" || name == "house" || name == "refinery" || name == "shipyard");
    auto [w, h] = building_wh(name); // already native pixels, see building_wh's comment
    b.size_w = w; b.size_h = h;
    b.foot_w = w; b.foot_h = h;
    b.foot_px = std::max(b.foot_w, b.foot_h);
    // Farms are the one building units can walk straight over/through
    // (matches the original GML catalog's "solid": false for farm) -- a
    // villager working one walks to its exact centre (see update_gather's
    // is_farm reach), and units passing by never have to route around it.
    b.solid = (name != "farm");
    b.common.is_solid = b.solid;
    b.build_radius = b.foot_px * 0.5 + 12;
    b.gather_x = x;
    b.gather_y = y + b.foot_h * 0.5 + 20;
    if (name == "farm") {
        b.max_farm_food = 200;
        b.amount = b.max_farm_food;
    }

    const Team& t = control.teams[team];
    bonuses.apply_building(b, t.civ, t.leader, t.tech);
    // full_max_hp (the HP a finished building tops up to) and the current HP
    // must be set AFTER apply_building so they include the civ/tech HP bonuses
    // (Soviet +50%, Steel Frame +30%). Previously full_max_hp was captured
    // before the bonus, so a constructed Soviet/steel-frame building completed
    // to its un-bonused base HP and never showed full.
    b.full_max_hp = b.common.max_hp;
    b.common.hp = constructing ? 40.0 : b.common.max_hp;

    uint32_t slot = buildings.insert(std::move(b));
    EntityRef ref{EntityKind::Building, slot, buildings.generation_of(slot)};
    active_buildings.push_back(ref);
    static_grid_dirty_ = true; // rebuild the grid's static layer next tick
    // AI-comparison metric: every placement counts, including the starting
    // base (spawned complete, never touches building_behavior.cpp's
    // construction-finish branch) -- see Team::bases_built's comment.
    if (team >= 0 && static_cast<size_t>(team) < control.teams.size()) {
        control.teams[team].buildings_built++; // every building, any type (stats screen)
        if (name == "base") control.teams[team].bases_built++;
        else if (name == "shipyard") control.teams[team].shipyards_built++;
        else if (name == "airbase") control.teams[team].airbases_built++;
    }
    return ref;
}

EntityRef World::place_building(const std::string& name, int team, double x, double y,
                                const std::vector<EntityRef>& builders, bool assign_builders) {
    if (!data.catalog().at("buildings").contains(name) || !control.afford(name, team)) {
        return kNullRef;
    }
    auto [sx, sy] = snap(name, x, y);
    if (!footprint_clear(name, sx, sy, &builders)) return kNullRef;
    // Fog only tracks the local player's (team 0) vision, so only team 0's
    // own placements are restricted by it -- the AI has no such concept.
    //
    // ...but ONLY when team 0 really is that local human. Whenever team 0 is
    // AI-driven -- a spectator match ("Start Stress Test", which sets every
    // team's is_ai including 0's) or a self-play arena/tournament match -- the
    // fog is nobody's vision, and gating team 0's placements on it silently
    // handicapped that one team: it could not put a building on any tile it
    // had not personally explored, while team 1 built anywhere. Since the AI
    // barely scouts, that is most of the map. Measured over 320 arena matches
    // with identical AI on both sides, team 0 won 38% overall and just 25% of
    // the matches that ended in an actual base destruction, with a 24% smaller
    // peak army -- while reaching Industrial at the same time as team 1, i.e.
    // the gap opened in expansion and army production, exactly where a blocked
    // place_building bites. Self-play numbers gathered before this are skewed.
    if (team == 0 && !control.teams[0].is_ai && !footprint_explored(name, sx, sy)) return kNullRef;
    control.pay(name, team);

    // Farms are now built like any other building: a foundation goes down and
    // the selected villager(s) construct it, THEN the builder seamlessly starts
    // farming it once complete (see unit_behavior.cpp's build-complete path).
    bool is_farm = (name == "farm");
    EntityRef ref = spawn_building(name, team, sx, sy, /*constructing=*/true);
    Building* b = get_building(ref);
    if (!b) return kNullRef;
    if (b->sprite.empty()) {
        static const std::unordered_map<std::string, std::string> default_sprites = {{"base", "spr_uk_base"}};
        auto it = default_sprites.find(name);
        if (it != default_sprites.end()) b->sprite = it->second;
    }
    // Only a building that actually blocks movement right now goes into
    // occupied_ ahead of the next rebuild_occupied() -- a fresh foundation
    // is walk-through until someone starts building it (see
    // Building::blocks_movement), so it must NOT close these tiles off.
    if (b->blocks_movement()) {
        for (auto [tx, ty] : building_tile_list(name, sx, sy)) occupied_.insert(tile_key(tx, ty));
    }

    // No need to clear units off the footprint here: footprint_clear()
    // above already rejected the placement if any unit was standing on it
    // (except for a farm, which units pass through freely either way).
    // Units CAN wander onto the foundation afterwards now that it's
    // walk-through, which is exactly why construction only starts once
    // unit_behavior.cpp sees the footprint clear again.

    // Assign the explicitly-passed builders (the player's actual selection,
    // if any were gatherers) as this foundation's construction crew -- ALL
    // of them, not just one -- or, for a farm, as its gather crew (update_gather's
    // occupied_by check then keeps only one of them actually working the
    // farm; the rest redirect to another free farm nearby or idle beside
    // this one -- see unit_behavior.cpp). Only falls back to auto-picking
    // the nearest idle civilian if none of the explicit builders qualified
    // (or none were passed at all, e.g. AI/scripted placement).
    auto assign = [&](Unit* c) {
        // Farms build like everything else now -- route the villager to BUILD
        // the farm foundation; it auto-switches to farming on completion.
        c->build_target = ref;
        c->gather_target = kNullRef;
        c->gather_rtype = -1; // see World::order_move's comment
        c->attack_target = kNullRef;
        c->move_goal.reset();
        c->path.clear();
        c->approach_prev_pos.reset();
        c->approach_progress_check_t = 0.0;
        c->approach_target.reset();
        c->approach_replans = 0; // fresh assignment -> a fresh replan budget
        c->order_queue.clear();
        c->active_queue_watch = kNullRef;
        c->queue_active = false;
    };
    // Shift-queued placement wants the foundation with no crew committed now --
    // the caller queues a Build order onto the selected villagers instead.
    if (!assign_builders) return ref;
    bool any_assigned = false;
    for (auto uref : builders) {
        Unit* c = get(uref);
        if (!c || !c->common.alive || c->common.team != team || !c->is_gatherer) continue;
        assign(c);
        any_assigned = true;
    }
    if (!any_assigned) {
        EntityRef best = kNullRef;
        double bestd = 1e18;
        for (auto uref : active_units) {
            Unit* u = get(uref);
            if (!u || u->common.team != team || u->name != "civilian" || !u->common.alive) continue;
            if (u->gather_target.valid() || u->build_target.valid()) continue;
            double dx = u->common.x - sx, dy = u->common.y - sy;
            double d = dx * dx + dy * dy;
            if (d < bestd) { bestd = d; best = uref; }
        }
        if (Unit* c = get(best)) assign(c);
    }
    return ref;
}

EntityRef World::spawn_resource(const std::string& kind, double x, double y) {
    auto it = resource_kinds().find(kind);
    if (it == resource_kinds().end()) return kNullRef;
    Resource r;
    r.common.id = next_id++;
    r.common.kind = EntityKind::Resource;
    r.common.team = -1;
    // Snap to the tile centre so the logical position matches exactly where
    // the sprite draws (the renderer already tile-snaps every resource) and
    // where it blocks movement -- otherwise the click/gather hitbox sits a few
    // px off the visible node. See game_client.cpp's resource draw.
    r.common.x = (std::floor(x / TILE) + 0.5) * TILE;
    r.common.y = (std::floor(y / TILE) + 0.5) * TILE;
    r.common.max_hp = it->second.amount;
    r.common.hp = it->second.amount;
    r.name = kind;
    r.sprite = it->second.sprite;
    r.res.rtype = it->second.rtype;
    r.res.amount = it->second.amount;
    r.res.start_amount = it->second.amount;

    uint32_t slot = resources.insert(std::move(r));
    EntityRef ref{EntityKind::Resource, slot, resources.generation_of(slot)};
    active_resources.push_back(ref);
    static_grid_dirty_ = true; // rebuild the grid's static layer next tick
    return ref;
}

EntityRef World::spawn_deer(double x, double y) {
    Deer d;
    d.common.id = next_id++;
    d.common.kind = EntityKind::Deer;
    d.common.team = -1;
    d.common.x = x; d.common.y = y;
    d.res.rtype = 0; // FOOD
    d.res.amount = 150;
    d.res.start_amount = 150;

    uint32_t slot = deer.insert(std::move(d));
    EntityRef ref{EntityKind::Deer, slot, deer.generation_of(slot)};
    active_deer.push_back(ref);
    return ref;
}

EntityRef World::spawn_projectile(Unit& shooter, EntityRef target, std::optional<Vec2> origin,
                                  std::optional<Vec2> target_pos) {
    bool lob = shooter.name == "artillery" || shooter.name == "artillery1" || shooter.name == "heavy artillery";
    // The ballistic missile flies like the Yamato's rocket (homing straight-line
    // travel with an obj_missile height arc, see kShipArc below), NOT a lob.
    bool ballistic = shooter.name == "ballistic missile";
    Projectile p;
    p.common.id = next_id++;
    p.common.kind = EntityKind::Projectile;
    p.common.team = shooter.common.team;
    Vec2 org = origin.value_or(Vec2{shooter.common.x, shooter.common.y});
    p.common.x = org.x; p.common.y = org.y;
    p.name = shooter.name;
    // Direct port of obj_missile's own default sprite (spr_missile) for
    // every unit that fires one in the original (battleship/yamato/
    // destroyer/frigate/torpedo boat/artillery) -- these were all using
    // `shooter.bullet_sprite` (a plain small-bullet sprite, "spr_bullet"/
    // "spr_bullet_large") instead of a proper shell sprite. Yamato alone
    // randomly swaps to spr_missile_hot or spr_torpedo per shot (obj_missile/
    // Step.gml's "if origin_name=yamato" sprite-swap), which is what gives
    // its salvo a visibly different, more varied look from the battleship's.
    static const std::set<std::string> kMissileUnits = {
        "battleship", "yamato", "destroyer", "frigate", "torpedo boat", "artillery", "artillery1",
        "heavy artillery"};
    if (kMissileUnits.count(shooter.name)) {
        if (shooter.name == "yamato") {
            p.sprite = rng.uniform(0, 1) < 0.5 ? "spr_torpedo" : "spr_missile_hot";
        } else {
            p.sprite = "spr_missile";
        }
    } else {
        p.sprite = shooter.bullet_sprite;
    }
    p.pow = shooter.attack;
    p.target = target;
    p.homing = !lob;
    p.lob = lob;
    p.big = shooter.common.is_ship;
    // (The Soviet unique tech "420mm mortar" was reworked into the economic
    // "5-Year Plan" -- it no longer boosts artillery blast.)
    // Ballistic missile: blast radius is 2x an artillery shell's, and it hits
    // buildings for +50 (its anti-fortification role). Both consumed by
    // projectile_behavior.cpp (splash radius = 48/40 * splash_mult).
    if (shooter.name == "ballistic missile") {
        p.splash_mult = 2.0;
        p.bonus_vs_building = 50.0;
        p.launch_facing = (shooter.facing < 0) ? -1 : 1; // missile leans to the launcher's side
    }
    // Tank line: a small direct-fire splash sized by the unit's catalog
    // blast_radius (in tiles). The round still hits its primary target for full
    // damage; blast_px only drives the modest half-power splash on neighbours
    // (see the LAND_SHELL_UNITS branch in projectile_behavior.cpp).
    {
        const auto& units = data.catalog().at("units");
        if (units.contains(shooter.name))
            p.blast_px = units.at(shooter.name).value("blast_radius", 0.0) * TILE;
    }
    // ---- Per-leader projectile bonuses.
    if (shooter.common.team >= 0 && shooter.common.team < 8) {
        const Team& st = control.teams[shooter.common.team];
        std::string sld = bonuses.leader_name(st.civ, st.leader);
        // Chiang Kai-Shek: infantry deal +2 damage to tanks.
        if (sld == "Chiang Kai-Shek" && INFANTRY.count(shooter.name)) {
            if (Unit* tu = get(target); tu && TANK.count(tu->name)) p.pow += 2.0;
        }
        // Harry S. Truman: bombers land a 25% bigger blast (the +25% damage is
        // already on the bomber's attack via apply_unit -> p.pow above).
        if (sld == "Harry S. Truman" &&
            (shooter.name == "bomber" || shooter.name == "heavy bomber" || shooter.name == "b29")) {
            p.splash_mult = (p.splash_mult > 0.0 ? p.splash_mult : 1.0) * 1.25;
        }
    }
    // Radar: this team's SHIPS fire with 100% accuracy -- their shells keep
    // homing until they connect rather than arcing out when a target flees.
    if (shooter.common.is_ship && shooter.common.team >= 0 && shooter.common.team < 8 &&
        control.teams[shooter.common.team].tech.count("radar")) {
        p.radar_guided = true;
    }
    // Waffen SS (and its Elite upgrade): +5 damage on a direct hit against an
    // enemy villager -- a specialist anti-eco raider. Applied at impact.
    if (shooter.name == "waffen" || shooter.name == "elite waffen") {
        p.bonus_vs_civilian = 5.0;
    }
    // Royal Marine (UK unique) and its Elite form: bonus damage on a direct hit
    // against enemy cavalry (+5 / +7). Applied at impact.
    if (shooter.name == "royal marine") p.bonus_vs_cavalry = 5.0;
    else if (shooter.name == "elite royal marine") p.bonus_vs_cavalry = 7.0;
    p.speed = 8.0 * 30.0;
    p.life = 4.0;
    if (ballistic) p.life = 9.0; // launch hold + slow accel + a long-range arc
    // Aircraft fire from altitude: seed the bullet's visual height to the
    // plane's current lift (Unit::height, 0..64) so tracer rounds leave the
    // nose at the same height the sprite is drawn at, instead of appearing to
    // come off the ground. z is purely visual for a homing bullet (no arc
    // physics touches it), so the shot still tracks its ground target.
    if (shooter.common.is_air) p.z = shooter.height;

    EntityCommon* tgt = common(target);
    double tx = target_pos ? target_pos->x : (tgt ? tgt->x : p.common.x);
    double ty = target_pos ? target_pos->y : (tgt ? tgt->y : p.common.y);
    // Artillery scatter: a low-accuracy shell lands somewhere in a circle around
    // the aim point (50% accuracy -> up to ~1 tile off), so it often misses a
    // point target outright but still bombards the area.
    if ((lob || ballistic) && shooter.accuracy < 1.0) {
        double miss = (1.0 - shooter.accuracy) * 2.0 * TILE;
        double a = rng.uniform(0, 2 * M_PI), r = rng.uniform(0, miss);
        tx += std::cos(a) * r;
        ty += std::sin(a) * r;
    }
    // Naval accuracy: without Radar, each ship shell has a per-shot chance to
    // go wide (based on the ship's accuracy stat -- frigates are the least
    // accurate). A missed shot scatters its aim point and stops homing, so it
    // arcs off past the target. Radar-guided shots (p.radar_guided) always home
    // true -- that team's ships fire with 100% accuracy.
    if (shooter.common.is_ship) {
        double acc = p.radar_guided ? 1.0 : shooter.accuracy;
        if (rng.uniform(0, 1) >= acc) {
            double a = rng.uniform(0, 2 * M_PI), r = rng.uniform(0.6 * TILE, 1.6 * TILE);
            tx += std::cos(a) * r;
            ty += std::sin(a) * r;
            p.homing = false; // fly to the wrong point instead of tracking the target
        }
    }
    double d = std::hypot(tx - p.common.x, ty - p.common.y);
    if (d < 1e-9) d = 1;
    p.vx = (tx - p.common.x) / d;
    p.vy = (ty - p.common.y) / d;
    p.angle = std::atan2(p.vy, p.vx);
    // Aircraft tracers: the bullet is drawn lifted by z (the plane's altitude)
    // but streaks toward a GROUND target, so its sprite must point steeper
    // (angled down toward the target) than the flat ground vector, and it flies
    // STRAIGHT rather than homing so that angle holds for the whole streak.
    if (shooter.common.is_air && !p.bomb) {
        p.homing = false;
        p.angle = std::atan2((ty - p.common.y) + shooter.height, tx - p.common.x);
    }
    if (p.lob) {
        p.sx = p.common.x; p.sy = p.common.y;
        p.tx = tx; p.ty = ty;
        p.total = d;
        p.progress = 0.0;
        p.speed = 6.0 * 30.0;
    } else {
        // Direct port of obj_unit/Step.gml's per-ship inst.speeds/inst.accel
        // (both GML per-frame units, i.e. assuming 60fps) -- this is what
        // drives obj_missile/Step.gml's height/up_speed arc (see update_
        // projectile), and also corrects a shot's actual travel speed to
        // match its ship type instead of every homing shot flying at the
        // same flat default. speeds is px/frame -> *60 for our px/sec.
        static const std::unordered_map<std::string, std::pair<double, double>> kShipArc = {
            {"yamato", {8.0, -0.25}},   {"battleship", {6.0, -0.15}}, {"destroyer", {6.0, -0.09}},
            {"frigate", {5.0, -0.08}}, {"torpedo boat", {6.0, -0.09}},
            // Ballistic missile reuses the Yamato rocket's flight arc, 33% slower
            // (speed 8 -> 5.33; arc accel dialled down to keep the apex height).
            {"ballistic missile", {5.33, -0.111}},
        };
        if (auto it = kShipArc.find(shooter.name); it != kShipArc.end()) {
            p.speed = it->second.first * 60.0;
            p.arc_accel = it->second.second;
            p.arc_t = 60.0 * (d / p.speed);
        }
    }

    uint32_t slot = projectiles.insert(std::move(p));
    EntityRef ref{EntityKind::Projectile, slot, projectiles.generation_of(slot)};
    active_projectiles.push_back(ref);
    return ref;
}

EntityRef World::spawn_bomb(Unit& shooter, EntityRef target, bool nuke, std::optional<Vec2> target_pos) {
    Projectile p;
    p.common.id = next_id++;
    p.common.kind = EntityKind::Projectile;
    p.common.team = shooter.common.team;
    p.common.x = shooter.common.x; p.common.y = shooter.common.y;
    p.name = shooter.name;
    p.sprite = nuke ? "spr_atom_bomb" : "spr_bomb"; // atomic bomb has its own falling-bomb sprite
    p.pow = shooter.attack;
    p.target = target;
    p.homing = false;
    p.lob = true;   // falls from altitude rather than flying straight at the target
    p.big = true;   // full mushroom, not the half-size tank/artillery one
    p.bomb = true;
    p.nuke = nuke;
    p.speed = 4.5 * 30.0;

    EntityCommon* tgt = common(target);
    double tx = target_pos ? target_pos->x : (tgt ? tgt->x : p.common.x);
    double ty = target_pos ? target_pos->y : (tgt ? tgt->y : p.common.y);
    double d = std::hypot(tx - p.common.x, ty - p.common.y);
    if (d < 1e-9) d = 1;
    p.vx = (tx - p.common.x) / d;
    p.vy = (ty - p.common.y) / d;
    p.angle = std::atan2(p.vy, p.vx);
    p.sx = p.common.x; p.sy = p.common.y;
    p.tx = tx; p.ty = ty;
    p.total = d;
    p.progress = 0.0;
    p.z = 60.0;

    uint32_t slot = projectiles.insert(std::move(p));
    EntityRef ref{EntityKind::Projectile, slot, projectiles.generation_of(slot)};
    active_projectiles.push_back(ref);
    return ref;
}

EntityRef World::spawn_building_shot(Building& shooter, EntityRef target) {
    Projectile p;
    p.common.id = next_id++;
    p.common.kind = EntityKind::Projectile;
    p.common.team = shooter.common.team;
    p.common.x = shooter.common.x;
    // muzzle near the top of the tower
    p.common.y = shooter.common.y - muzzle_visual_h(shooter.name, shooter.foot_h) * 0.35;
    p.name = shooter.name; // not in the ship/land-shell splash sets -> plain direct hit
    p.sprite = shooter.is_aa ? "spr_bullet_large" : "spr_bullet";
    p.pow = shooter.attack;
    p.target = target;
    p.homing = true;
    p.lob = false;
    p.speed = 8.0 * 30.0;
    p.life = 3.0;

    EntityCommon* tgt = common(target);
    double tx = tgt ? tgt->x : p.common.x;
    double ty = tgt ? tgt->y : p.common.y;
    double d = std::hypot(tx - p.common.x, ty - p.common.y);
    if (d < 1e-9) d = 1;
    p.vx = (tx - p.common.x) / d;
    p.vy = (ty - p.common.y) / d;
    p.angle = std::atan2(p.vy, p.vx);
    p.sx = p.common.x; p.sy = p.common.y;
    p.tx = tx; p.ty = ty;

    uint32_t slot = projectiles.insert(std::move(p));
    EntityRef ref{EntityKind::Projectile, slot, projectiles.generation_of(slot)};
    active_projectiles.push_back(ref);
    return ref;
}

// ---- main per-tick update ----

void World::update(double dt) {
    rally_astar_budget_ = 12; // spread a mass army-commit's pathfinding across steps (see world.h)
    rebuild_occupied();
    rebuild_spatial_grid();
    control.update_ai(dt, *this);

    for (auto ref : active_units) {
        Unit* u = units.get(ref);
        if (u && u->common.alive) update_unit(ref, *u, dt, *this);
    }

    // Fire hazard damage-over-time: every active fire patch hurts any
    // non-tank, non-air ground unit standing in it (see FireHazard's
    // comment on why tanks are exempt and why the damage numbers are new
    // rather than ported). Run right after units update, so a unit that
    // just walked out of a fire this tick doesn't still get burned, and
    // before the death sweep below, so a unit fire kills this tick gets
    // its death FX/sound for free same as any other kill.
    //
    // Overlapping flames stack in space (a nuke/reactor drops ~14 at once), but
    // an entity standing in several is only burned ONCE per tick -- otherwise a
    // unit in a dense firestorm took N x the rate and melted instantly. These
    // per-tick "already burned" sets enforce one flame per entity; membership is
    // tested (not iterated), so the fixed (fire order, then query order) apply
    // order stays deterministic. (dt is fixed, so rate x dt is a fixed tick hit.)
    std::unordered_set<uint32_t> burned_units, burned_buildings;
    for (auto it = fires.begin(); it != fires.end();) {
        it->timer -= dt;
        if (it->timer <= 0.0) {
            it = fires.erase(it);
            continue;
        }
        for (auto ref : grid.query(it->x, it->y, kFireRadius)) {
            if (ref.kind == EntityKind::Unit) {
                Unit* u = units.get(ref);
                if (!u || !u->common.alive || u->common.is_air || TANK.count(u->name)) continue;
                if (!burned_units.insert(ref.slot).second) continue; // already burned by another flame
                hurt(ref, kFireDamagePerSecond * dt, /*shake=*/false);
            } else if (ref.kind == EntityKind::Building) {
                // Fire also chews on any building it's sitting against, same
                // rate, until it burns out. No shudder (shake=false) -- a
                // burning building shouldn't judder for the whole fire.
                Building* b = buildings.get(ref);
                if (!b || !b->common.alive) continue;
                if (!burned_buildings.insert(ref.slot).second) continue; // one flame per building
                hurt(ref, kFireDamagePerSecond * dt, /*shake=*/false);
            }
        }
        ++it;
    }

    // Map-authored message triggers (editor's Events
    // tab): one-shot, unlike the fire sweep above -- the first living unit
    // (any team) found within range fires the Notify event and marks it
    // used, so it never fires again for the rest of the match.
    for (auto& trig : message_triggers) {
        if (trig.triggered) continue;
        // Fires only when one of the PLAYER's (team 0) units is standing directly
        // on the marker's own tile -- not merely near it. Query a tile-radius then
        // require an exact tile match.
        int mtx = static_cast<int>(trig.x / TILE), mty = static_cast<int>(trig.y / TILE);
        for (auto ref : grid.query(trig.x, trig.y, TILE)) {
            Unit* u = units.get(ref);
            if (!u || !u->common.alive || u->common.team != 0) continue;
            if (static_cast<int>(u->common.x / TILE) != mtx || static_cast<int>(u->common.y / TILE) != mty) continue;
            trig.triggered = true;
            events.push({EventType::Notify, "map_message", trig.x, trig.y, 0, kNullRef, trig.text});
            events.push({EventType::Sound, "chat", 0, 0, 0, kNullRef, ""}); // activation blip (snd_chat)
            break;
        }
    }

    // ---- Campaign triggers: objective tracking + spawn / resource / wake ----
    // Local player is always team 0. All one-shot and deterministic (checksum-safe).
    auto player_in_rect = [&](const TriggerRect& r) -> bool {
        if (r.tw <= 0 || r.th <= 0) return false;
        for (auto ref : active_units) {
            Unit* u = units.get(ref);
            if (!u || !u->common.alive || u->common.team != 0) continue;
            int tx = static_cast<int>(u->common.x / TILE), ty = static_cast<int>(u->common.y / TILE);
            if (tx >= r.tx && tx < r.tx + r.tw && ty >= r.ty && ty < r.ty + r.th) return true;
        }
        return false;
    };

    // Objective completion (gates spawn triggers). kill_units -> all target refs
    // dead; move_to_area -> a team-0 unit inside the area. protect_unit deferred.
    for (auto& ow : objective_watches) {
        if (cleared_objectives.count(ow.id)) continue;
        bool met = false;
        if (ow.type == "kill_units") {
            if (!ow.targets.empty()) {
                met = true;
                for (auto ref : ow.targets) {
                    Unit* u = units.get(ref);
                    if (u && u->common.alive) { met = false; break; }
                }
            }
        } else if (ow.type == "move_to_area") {
            met = player_in_rect({ow.tx, ow.ty, ow.tw, ow.th});
        }
        if (met) cleared_objectives.insert(ow.id);
    }

    // Spawn triggers: fire on the gating objective clearing, or (no objective) a
    // team-0 unit entering the box; creates `count` units tiled across the box.
    for (auto& s : spawn_triggers) {
        if (s.fired) continue;
        bool go = s.objective_id.empty() ? player_in_rect(s.box)
                                         : cleared_objectives.count(s.objective_id) > 0;
        if (!go) continue;
        s.fired = true;
        int bw = std::max(1, s.box.tw), bh = std::max(1, s.box.th);
        for (int i = 0; i < s.count; ++i) {
            int bx = s.box.tx + (i % bw);
            int by = s.box.ty + ((i / bw) % bh);
            spawn_unit(s.unit, s.player, (bx + 0.5) * TILE, (by + 0.5) * TILE);
        }
        if (!s.text.empty()) {
            double cx = (s.box.tx + bw / 2.0) * TILE, cy = (s.box.ty + bh / 2.0) * TILE;
            events.push({EventType::Notify, "map_message", cx, cy, 0, kNullRef, s.text});
        }
    }

    // Resource pickups: a team-0 unit touching the tile grants resources once,
    // shows the message, and removes the marker (fired -> client stops drawing).
    for (auto& p : resource_pickups) {
        if (p.fired) continue;
        // Collected only when a PLAYER (team 0) unit stands directly on the
        // marker's own tile (not merely nearby).
        int ptx = static_cast<int>(p.x / TILE), pty = static_cast<int>(p.y / TILE);
        bool touched = false;
        for (auto ref : grid.query(p.x, p.y, TILE)) {
            Unit* u = units.get(ref);
            if (!u || !u->common.alive || u->common.team != 0) continue;
            if (static_cast<int>(u->common.x / TILE) == ptx && static_cast<int>(u->common.y / TILE) == pty) { touched = true; break; }
        }
        if (!touched) continue;
        p.fired = true;
        events.push({EventType::Sound, "chat", 0, 0, 0, kNullRef, ""}); // activation blip (snd_chat)
        if (p.food) add_resource(0, 0, p.food);
        if (p.wood) add_resource(0, 1, p.wood);
        if (p.oil) add_resource(0, 2, p.oil);
        if (p.iron) add_resource(0, 3, p.iron);
        if (!p.text.empty())
            events.push({EventType::Notify, "map_message", p.x, p.y, 0, kNullRef, p.text});
    }

    // Wake triggers: a team-0 unit entering the tripwire wakes the dormant group
    // (they charge the target via `rally`), reveals the sight boxes, shows text.
    for (auto& w : wake_triggers) {
        if (w.fired) continue;
        bool tripped = false;
        for (auto& tr : w.trips) if (player_in_rect(tr)) { tripped = true; break; }
        if (!tripped) continue; // a team-0 unit must enter ANY one of the trip boxes
        w.fired = true;
        int i = 0;
        for (auto ref : w.group) {
            Unit* u = units.get(ref);
            if (!u || !u->common.alive) continue;
            u->dormant = false;
            u->hold = std::nullopt;
            double ang = i * 2.399963229, rad = 8.0 + 3.0 * i; // golden-angle fan
            u->rally = Vec2{w.target_x + std::cos(ang) * rad, w.target_y + std::sin(ang) * rad};
            ++i;
        }
        for (auto& r : w.los) reveal_areas.push_back(r);
        if (!w.text.empty())
            events.push({EventType::Notify, "map_message", w.target_x, w.target_y, 0, kNullRef, w.text});
    }

    for (auto ref : active_buildings) {
        Building* b = buildings.get(ref);
        if (b && b->common.alive) update_building(ref, *b, dt, *this);
    }
    // NOTE: resources are NOT iterated per-tick anymore. update_resource() only
    // mirrored res.amount into common.hp/max_hp and flagged a depleted node dead
    // -- both now happen at the gather site (unit_behavior.cpp), which is the one
    // place amount ever changes. Iterating all ~1500 nodes every tick just to
    // touch the handful being gathered was the single largest per-tick sim cost
    // on the current larger maps (measured ~200us/tick in the -O0 client build).
    for (auto ref : active_deer) {
        Deer* d = deer.get(ref);
        if (d && d->common.alive) update_deer(ref, *d, dt, *this);
    }

    for (auto ref : active_projectiles) {
        Projectile* p = projectiles.get(ref);
        if (p && p->common.alive) update_projectile(ref, *p, dt, *this);
    }

    control.recompute(*this);
    control.check_win(*this);
    control.update_metrics(dt, *this);

    // death bookkeeping: emit events, then compact each active list
    // (stable filter, preserving spawn order -- see the determinism
    // note on iteration-order dependence).
    auto sweep = [&](std::vector<EntityRef>& active, auto&& slotmap, auto&& on_death) {
        for (auto ref : active) {
            auto* e = slotmap.get(ref);
            if (e && !e->common.alive) on_death(*e);
        }
        active.erase(std::remove_if(active.begin(), active.end(),
                                     [&](EntityRef r) {
                                         auto* e = slotmap.get(r);
                                         bool dead = !e || !e->common.alive;
                                         if (dead && e) slotmap.erase(r.slot);
                                         return dead;
                                     }),
                     active.end());
    };
    sweep(active_units, units, [&](Unit& u) {
        // Cosmetic death event: carries the entity state the client needs to
        // pick the right corpse/wreck sprite (spr_<name>_dead/_rubble/_sink)
        // -- see GameClient::spawn_death_effect.
        SimEvent fx{EventType::Effect, u.common.is_air ? "death_air" : "death_unit",
                    u.common.x, u.common.y};
        // Carry the SPRITE base (minus the "spr_" prefix), not the catalog
        // name: upgraded units share a base sprite (e.g. "elite waffen" and
        // "waffen" both draw spr_waffen), so the corpse must key off the
        // sprite -> spr_waffen_dead, not a non-existent spr_elite_waffen_dead.
        fx.text = (u.sprite.rfind("spr_", 0) == 0) ? u.sprite.substr(4) : u.sprite;
        fx.alt = u.name; // fallback: generic type -> spr_<type>_rubble for skinned planes
        fx.team = u.common.team;
        fx.flip = u.facing < 0;
        fx.mechanical = u.mechanical;
        fx.deleted = u.deleted; // player-deleted -> no explosion boom (esp. sinking boats)
        if (u.is_ballistic) {
            // Distinct wreck sprite per state -- the client grounds
            // spr_ballistic_missile[_unpacked]_destroyed for these sentinels.
            fx.text = (u.packed || u.pack_t > 0.0) ? "ballistic_missile_destroyed"
                                                   : "ballistic_missile_unpacked_destroyed";
            // A destroyed launcher's warhead cooks off in a damaging blast
            // (friendly units included; tapering to the rim). Inlined here since
            // splash_damage is file-local to projectile_behavior.cpp.
            if (!u.deleted) {
                double x = u.common.x, y = u.common.y;
                // The warhead cooks off: just the small artillery-style explosion
                // (not the huge blast/mushroom), plus the sound, so a destroyed
                // launcher reads as a detonation without a giant fireball.
                events.push({EventType::Effect, "spr_explosion", x, y, 1.0, kNullRef, ""});
                events.push({EventType::Sound, "explosion", x, y, 200, kNullRef, ""});
                const double half_box = 72.0, base_dmg = 90.0;
                for (auto ref2 : grid.query(x, y, half_box * 1.2)) {
                    EntityCommon* e = common(ref2);
                    if (!e || !e->alive || e->team < 0 || e->is_air) continue;
                    if (e->kind != EntityKind::Unit && e->kind != EntityKind::Building) continue;
                    double dx = e->x - x, dy = e->y - y;
                    if (std::abs(dx) > half_box || std::abs(dy) > half_box) continue;
                    double falloff = std::max(0.0, 1.0 - std::hypot(dx, dy) / half_box);
                    if (e->kind == EntityKind::Building) {
                        if (Building* ob = get_building(ref2)) ob->big_death = true;
                    }
                    hurt(ref2, std::max(1.0, base_dmg * falloff));
                }
                fires.push_back({x, y, 5.0});
            }
        }
        events.push(fx);
        // Direct port of Audio.death_sound's dispatch (game/audio.py),
        // resolved here (not client-side) since Unit fields are needed
        // and the entity is about to be erased.
        std::string sound;
        if (u.common.is_air) sound = "plane_crash";
        else if (u.common.is_ship) sound = "ship_sink";
        else if (u.mechanical) sound = "explosion";
        else if (u.name == "camel" || u.name == "camel corps") sound = "camel_death";
        else if (u.name == "cavalry" || u.name == "cavalry2" || u.name == "cavalry3") sound = "cavalry_death";
        else sound = "male_death";
        events.push({EventType::Sound, sound, u.common.x, u.common.y, 200, kNullRef, ""});
        // A crashing plane also gets the explosion boom on impact (game/
        // world.py plays plane_crash AND explosion for a downed aircraft).
        if (u.common.is_air) {
            events.push({EventType::Sound, "explosion", u.common.x, u.common.y, 150, kNullRef, ""});
        }
        // AI-comparison metric -- excludes a player's own deliberate delete
        // (not a combat loss); see Team::units_lost's comment.
        if (!u.deleted && u.common.team >= 0 && u.common.team < 8) {
            control.teams[u.common.team].units_lost++;
            // Post-game stats: log this combat death (where/when/whose) for the
            // Largest Battle tab. Deletes are excluded (same as units_lost).
            if (control.combat_log.size() < Control::kCombatLogCap) {
                control.combat_log.push_back({static_cast<float>(u.common.x),
                                              static_cast<float>(u.common.y), control.stats_elapsed,
                                              u.common.team, /*is_building=*/false, u.name});
            }
        }
    });
    sweep(active_buildings, buildings, [&](Building& b) {
        static_grid_dirty_ = true; // a building died -> rebuild the static grid layer
        SimEvent fx{EventType::Effect, "death_building", b.common.x, b.common.y};
        fx.text = b.name;
        fx.team = b.common.team;
        fx.shell_kill = b.big_death;
        fx.deleted = b.deleted;
        fx.foot = b.foot_w;
        events.push(fx);
        if (!b.deleted && b.common.team >= 0 && b.common.team < 8) {
            control.teams[b.common.team].buildings_lost++;
            if (control.combat_log.size() < Control::kCombatLogCap) {
                control.combat_log.push_back({static_cast<float>(b.common.x),
                                              static_cast<float>(b.common.y), control.stats_elapsed,
                                              b.common.team, /*is_building=*/true, b.name});
            }
        }
        // Building collapse (game/world.py _make_rubble's audio.play(
        // "building_destroyed")) -- a withering farm or a cancelled
        // foundation just poofs, so no collapse sound for those.
        if (b.name != "farm" && !b.deleted) {
            events.push({EventType::Sound, "building_destroyed", b.common.x, b.common.y, 200, kNullRef, ""});
        }
        // NUCLEAR MELTDOWN: a destroyed reactor (killed, not player-deleted)
        // detonates a wide radioactive blast -- a mushroom cloud, the client
        // white-flash/shake cue, a heavy area hit (friendly units included), and
        // a lingering firestorm. Same shape as an atomic bomb, keyed off the
        // reactor's death rather than a projectile.
        if (b.name == "nuclear reactor" && !b.deleted) {
            double x = b.common.x, y = b.common.y;
            events.push({EventType::Effect, "spr_explosion_mushroom", x, y, 4.0, kNullRef, ""});
            events.push({EventType::Effect, "nuke_flash", x, y, 240.0, kNullRef, ""});
            events.push({EventType::Sound, "explosion", x, y, 0, kNullRef, ""});
            // Wide meltdown blast: hurt every nearby ground unit/building
            // (friendly included), damage tapering to 0 at the rim -- the same
            // shape splash_damage uses for a nuke, inlined here since that
            // helper is file-local to projectile_behavior.cpp.
            const double half_box = 240.0, base_dmg = 850.0;
            for (auto ref2 : grid.query(x, y, half_box * 1.2)) {
                EntityCommon* e = common(ref2);
                if (!e || !e->alive || e->team < 0 || e->is_air) continue;
                if (e->kind != EntityKind::Unit && e->kind != EntityKind::Building) continue;
                double dx = e->x - x, dy = e->y - y;
                if (std::abs(dx) > half_box || std::abs(dy) > half_box) continue;
                double falloff = std::max(0.0, 1.0 - std::hypot(dx, dy) / half_box);
                if (e->kind == EntityKind::Building) {
                    if (Building* ob = get_building(ref2)) ob->big_death = true;
                }
                hurt(ref2, std::max(1.0, base_dmg * falloff));
            }
            for (int i = 0; i < 14; ++i) {
                double a = i * 0.4487989, r = 25.0 + (i % 4) * 55.0;
                double fx2 = x + std::cos(a) * r, fy2 = y + std::sin(a) * r;
                events.push({EventType::Effect, "spr_flame", fx2, fy2, 0, kNullRef, ""});
                fires.push_back({fx2, fy2, 10.0});
            }
        }
    });
    sweep(active_resources, resources, [&](Resource&) { static_grid_dirty_ = true; });
    sweep(active_deer, deer, [](Deer&) {});
    sweep(active_projectiles, projectiles, [](Projectile&) {});

    update_fog(fog_player); // normally team 0; the POV cheat repoints it (see world.h)
}

} // namespace ww::sim
