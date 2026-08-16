#include "sim/stress_scenario.h"

#include <cmath>
#include <optional>
#include <utility>

namespace ww::sim {

namespace {

// Nearest-water search, expanding one tile-ring (Chebyshev distance) at a
// time and checking every tile on that ring -- NOT an angular/ray search
// like Control::ai_build_spot's shipyard case, which samples only 24 fixed
// directions per radius and can step clean over a narrow strait or bay
// once the ring is wide enough that adjacent rays are many tiles apart.
// Only runs once per team here, so an exhaustive per-tile ring scan (up to
// the full map size) costs nothing that matters.
std::optional<std::pair<double, double>> find_water_near(const World& world, double cx, double cy) {
    int ctx = static_cast<int>(std::floor(cx / TILE));
    int cty = static_cast<int>(std::floor(cy / TILE));
    int max_r = std::max(world.cols, world.rows);
    for (int r = 0; r <= max_r; ++r) {
        for (int dx = -r; dx <= r; ++dx) {
            for (int dy = -r; dy <= r; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue; // ring border only
                int tx = ctx + dx, ty = cty + dy;
                if (tx < 0 || tx >= world.cols || ty < 0 || ty >= world.rows) continue;
                if (world.terrain[tx][ty] != WATER) continue;
                double px = tx * TILE + TILE / 2.0, py = ty * TILE + TILE / 2.0;
                if (world.passable(/*is_air=*/false, /*is_ship=*/true, px, py)) return std::make_pair(px, py);
            }
        }
    }
    return std::nullopt;
}

struct RosterEntry {
    const char* name;
    double fraction; // share of the per-team pop-cost budget (see populate_stress_test); sums to 1.0
    bool is_ship;
};

// Cost-weighted (see POP_COST, control.cpp) mix of civilians + infantry +
// cavalry + tanks/artillery (land), ships (skipped entirely if no water is
// reachable near the base -- see find_water_near), and planes (just need
// an airbase to exist, placed below, not any special positioning).
// Fractions of the pop-cost budget, not unit counts directly -- see
// populate_stress_test, which converts each into an actual count from
// control.max_pop so the roster actually lands near whatever pop target
// is configured (previously a fixed unit-count table that summed to only
// ~180 pop even when max_pop was 200).
const RosterEntry kRoster[] = {
    {"civilian", 0.12, false},  {"rifleman", 0.22, false}, {"cavalry", 0.05, false},
    {"light tank", 0.10, false}, {"heavy tank", 0.10, false}, {"artillery", 0.05, false},
    {"frigate", 0.05, true},     {"destroyer", 0.06, true},   {"battleship", 0.12, true},
    {"fighter", 0.08, false},    {"bomber", 0.05, false},
};

const char* kOneOffBuildings[] = {
    "barracks", "academy", "factory", "fortress", "market", "refinery", "university", "airbase", "shipyard",
};

constexpr int kHousesPerTeam = 20;

} // namespace

void populate_stress_test(World& world, Control& control, Rng& rng) {
    // Target pop-cost budget per team: control.max_pop already reflects
    // whatever the player configured (the menu's Stress Test toggle
    // defaults it to 200, but this scales down cleanly if they picked a
    // smaller Max Pop instead), less a small allowance for new_skirmish's
    // own starting civilians/cavalry (already placed before this runs).
    double target_pop = std::max(20.0, static_cast<double>(control.max_pop) - 4.0);

    for (int team = 0; team < control.n; ++team) {
        EntityRef base_ref = kNullRef;
        for (auto ref : world.active_buildings) {
            Building* b = world.get_building(ref);
            if (b && b->common.alive && b->common.team == team && b->name == "base") {
                base_ref = ref;
                break;
            }
        }
        Building* base = world.get_building(base_ref);
        if (!base) continue; // team has no base (shouldn't happen post-new_skirmish) -- nothing to build around
        // Cache the base's position up front -- `base` is a raw pointer into
        // World::buildings' backing SlotMap (sim/include/sim/entity_common.h),
        // and every spawn_building call below can insert into that same
        // SlotMap and reallocate it, silently invalidating `base` (an
        // intermittent use-after-free/segfault depending on heap layout, not
        // a reliably-reproducing one). Everything after this point uses
        // these plain doubles instead of dereferencing `base` again.
        double base_x = base->common.x, base_y = base->common.y;

        // One of every production building (ai_build_spot already knows to
        // search for open water next to land for "shipyard" specifically), plus
        // a stack of houses. After each placement refresh only the BUILDING
        // collision sets (rebuild_building_tiles) so the next ai_build_spot's
        // footprint_clear sees it -- NOT a full world.prime(), which re-hashed
        // every one of the ~1500 map resources into the grid + tile sets on each
        // of the ~230 placements (an ~800ms freeze at 8 players, big enough to
        // trip a GPU driver-timeout crash). One real prime() runs at the end.
        for (const char* name : kOneOffBuildings) {
            if (auto spot = control.ai_build_spot(team, world, base_ref, name)) {
                world.spawn_building(name, team, spot->first, spot->second);
            }
            world.rebuild_building_tiles();
        }
        for (int i = 0; i < kHousesPerTeam; ++i) {
            if (auto spot = control.ai_build_spot(team, world, base_ref, "house")) {
                world.spawn_building("house", team, spot->first, spot->second);
            }
            world.rebuild_building_tiles();
        }

        std::optional<std::pair<double, double>> water_spot = find_water_near(world, base_x, base_y);

        for (const auto& entry : kRoster) {
            int count = std::max(
                1, static_cast<int>(std::lround(entry.fraction * target_pop / pop_cost(entry.name))));
            for (int i = 0; i < count; ++i) {
                if (entry.is_ship) {
                    if (!water_spot) continue; // landlocked team on this map -- skip naval units entirely
                    double jx = water_spot->first + rng.uniform(-150.0, 150.0);
                    double jy = water_spot->second + rng.uniform(-150.0, 150.0);
                    auto [sx, sy] = world.nearest_passable(jx, jy, /*is_air=*/false, /*is_ship=*/true);
                    world.spawn_unit(entry.name, team, sx, sy);
                } else {
                    double a = rng.uniform(0.0, 2.0 * M_PI);
                    double r = 80.0 + rng.uniform(0.0, 350.0);
                    double jx = base_x + std::cos(a) * r;
                    double jy = base_y + std::sin(a) * r;
                    auto [sx, sy] = world.clear_point_near(jx, jy, TILE, false);
                    world.spawn_unit(entry.name, team, sx, sy);
                }
            }
        }
    }
    world.prime();
}

} // namespace ww::sim
