#include "game_client.h"

#include "hud/item_tooltips.h"
#include "menu/civ_data.h"
#include "net/session.h"
#include "sim/command.h"
#include "sim/control.h"
#include "sim/stress_scenario.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

using namespace ww::sim;

namespace {
// ---- Per-plane propeller placement -------------------------------------
// Planes are drawn at a FIXED art orientation (only mirrored by facing, never
// rotated), so a propeller sits at a fixed sprite pixel, not along the flight
// heading. Each entry is a prop's offset from the sprite's pivot (ox,oy) in
// native sprite px: dx is mirrored with facing, dy is not. `scale` sizes the
// (93px) spr_propeller down to the engine, `behind` draws it under the
// airframe (for engines whose disc faces away from the viewer) instead of over
// it. Single-engine fighters get one nose prop; 4-engine heavy bombers get one
// per nacelle. Jets/ohka have none. TUNE these dx/dy by eye in-game -- render
// a sprite with a pixel grid to read positions (pivot listed in the manifest).
struct PropSpec { double dx, dy, scale; bool behind; };
const std::vector<PropSpec>& plane_props(const std::string& sprite) {
    static const std::unordered_map<std::string, std::vector<PropSpec>> table = {
        // single-engine fighters: nose spinner (measured off the sprite grid)
        {"spr_spitfire", {{5, 17, 0.22, false}}},
        {"spr_messerschmitt", {{17, 14, 0.22, false}}},
        {"spr_mustang", {{5, 17, 0.22, false}}},
        {"spr_zero", {{14, 15, 0.22, false}}},
        {"spr_fighter", {{5, 17, 0.22, false}}},
        {"spr_biplane", {{5, 17, 0.22, false}}},
        // 4-engine heavy bombers: one prop per wing nacelle (estimated -- tune)
        {"spr_b29", {{55, -13, 0.16, false}, {30, 7, 0.16, false}, {-8, 22, 0.16, false},
                     {-32, 40, 0.16, false}}},
        {"spr_bomber", {{36, 8, 0.15, false}, {21, 18, 0.15, false}, {-14, 18, 0.15, false},
                        {-29, 26, 0.15, false}}},
        {"spr_heavy_bomber", {{36, 8, 0.15, false}, {21, 18, 0.15, false}, {-14, 18, 0.15, false},
                              {-29, 26, 0.15, false}}},
        {"spr_lancaster_bomber", {{36, 8, 0.15, false}, {21, 18, 0.15, false},
                                  {-14, 18, 0.15, false}, {-29, 26, 0.15, false}}},
        // jets (meteor/me262/shooting_star/white_zero/jet_fighter) and the
        // rocket-powered ohka have no propeller.
    };
    static const std::vector<PropSpec> empty;
    auto it = table.find(sprite);
    return it != table.end() ? it->second : empty;
}
// The forward gun muzzle / nose point for a fighter, same fixed sprite pixel as
// its nose propeller -- so bullets leave the nose, not the sprite centre. dx is
// mirrored by facing. Returns {0,0} (centre) for planes not listed.
std::pair<double, double> plane_nose(const std::string& sprite) {
    const auto& props = plane_props(sprite);
    if (props.empty()) return {0.0, 0.0};
    return {props.front().dx, props.front().dy}; // the (first) nose prop's offset
}
// Multi-frame effect sprite (explosions etc.) playback rate: each GML
// object sets its own image_speed, and the room actually runs at
// room_speed=60 (assets/gmk/objects/control/Step.gml:501 -- see world.cpp's
// speed_px comment for the same 30-vs-60 correction), so the true frame
// rate is image_speed*60. Used both to size a multi-frame effect's
// lifetime (see drain_events) and to step its displayed frame (see
// draw()) -- previously every effect was stretched/squashed to a fixed
// 0.4s regardless of its real frame count, which made a 9-frame explosion
// flicker through its whole animation in under half a second. Every ported
// effect object's own image_speed differs (obj_explosion_large: 0.13;
// obj_explosion/obj_explosion_effect/obj_shockwave: 0.2; obj_flame/
// obj_fire_cloud: 0.1), so this is keyed per sprite instead of one flat
// rate -- using a single constant for all of them (as before) played
// everything at obj_explosion_large's rate regardless of its own speed.
double effect_fps(const std::string& sprite) {
    if (sprite == "spr_flame" || sprite == "spr_fire_cloud") return 6.0;      // 0.1 * 60
    if (sprite == "spr_explosion_large" || sprite == "spr_explosion_mushroom") return 7.8; // 0.13 * 60
    return 12.0; // spr_explosion / spr_shockwave: 0.2 * 60
}

// Splits Control::BUILDABLE into the two categories the "build eco
// building" / "build military building" buttons filter by.
const std::set<std::string>& eco_buildings() {
    static const std::set<std::string> s = {"house", "farm",     "refinery",        "market",
                                            "university", "nuclear reactor", "base"};
    return s;
}
const std::set<std::string>& military_buildings() {
    static const std::set<std::string> s = {"barracks", "factory",  "airbase",  "academy", "shipyard",
                                            "tower",    "fortress", "outpost",  "palisade", "iron wall"};
    return s;
}

// Which shipyard page (0 = passive dock, 1 = warships) a unit/tech belongs
// on -- hoisted out of draw_command_card so handle_hotkey can also consult
// it (an item's hotkey should work even while the OTHER page is showing,
// see its "Next Page" auto-switch fallback).
const std::set<std::string>& ship_passive_units() {
    static const std::set<std::string> s = {"fishing boat", "transport ship"};
    return s;
}
const std::set<std::string>& ship_passive_techs() {
    static const std::set<std::string> s = {"hydrodynamics", "naval armour"};
    return s;
}

struct GridPos { int col, row; };

// FIXED command-card slot for each building type, keyed by name so a
// building's icon always lands in the same place regardless of which
// OTHER buildings happen to be era-unlocked yet (previously a flat index
// into Control::available_buildings()'s filtered list -- reported as
// "building icons change locations depending on what age I'm in", since
// a newly-unlocked building could only ever land at the END of whatever
// was already shown, shifting nothing, but a building unlocking in the
// MIDDLE of the canonical order would shift every later one). A building
// not yet unlocked just leaves its slot empty rather than something else
// sliding into it.
const std::vector<std::pair<std::string, GridPos>>& military_building_layout() {
    static const std::vector<std::pair<std::string, GridPos>> v = {
        {"barracks", {0, 0}}, {"academy", {1, 0}}, {"shipyard", {2, 0}}, {"tower", {3, 0}},
        {"outpost", {4, 0}},  {"factory", {0, 1}}, {"airbase", {1, 1}},  {"fortress", {2, 1}},
        {"palisade", {3, 1}}, {"iron wall", {4, 1}},
    };
    return v;
}
const std::vector<std::pair<std::string, GridPos>>& eco_building_layout() {
    static const std::vector<std::pair<std::string, GridPos>> v = {
        {"house", {0, 0}}, {"farm", {1, 0}}, {"refinery", {2, 0}}, {"market", {3, 0}},
        {"university", {0, 1}}, {"nuclear reactor", {1, 1}}, {"base", {2, 1}},
    };
    return v;
}

// Upper bound on how many training-queue buttons a building could ever
// show at once -- Control::available_units() only ever DROPS raw
// PRODUCTION entries (era/civ gating, or an obsolete tier collapsing into
// its upgraded name), never adds beyond that count. Used to reserve
// leading command-card slots for units so a tech's fixed position (below)
// can never land on top of one.
int unit_slot_reserve(const std::string& building) {
    auto it = PRODUCTION.find(building);
    return it == PRODUCTION.end() ? 0 : static_cast<int>(it->second.size());
}

// Hand-placed tech slots for the two buildings whose research the user
// specifically wants at fixed spots (grid is 1-indexed col/row in the
// original request -- (5,2) etc -- converted to 0-indexed here). Every
// OTHER building's techs fall back to their own static index within
// Control::building_techs() (see draw_command_card) -- that list itself
// never reorders, unlike the "currently researchable" filtered list that
// used to drive positions directly (finishing one tech removed it from
// that filtered list, shifting every later tech up a slot -- reported as
// "techs move after I research something").
const std::unordered_map<std::string, GridPos>* tech_position_overrides(const std::string& building) {
    static const std::unordered_map<std::string, GridPos> factory = {
        // Techs live on row 1, clear of the row-0 unit slots. Germany's
        // factory fills all five row-0 slots (light tank, tank, tiger tank
        // [heavy-tank substitute], artillery, aa gun), so the old diesel-engine
        // slot at (4,0) collided with the aa gun. Pushed right on row 1 here so
        // diesel engine and the aa gun never overlap for the Germans.
        {"diesel engine", {2, 1}},
        {"blowback reload", {3, 1}},
        {"heavy tank upgrade", {4, 1}}, // tank -> heavy tank (USA/Soviet); Germany uses Tiger II instead
        {"flak upgrade", {1, 1}},       // aa gun -> flak gateway; free row-1 cell, clear of the others
        // NOTE: the Tiger II (tiger2 tank upgrade) is intentionally NOT pinned
        // here -- it's placed dynamically directly under whichever slot the
        // Tiger tank occupies (see tech_parent_units), so it always reads as
        // "upgrade the tank above me" regardless of the German roster layout.
    };
    static const std::unordered_map<std::string, GridPos> airbase = {
        {"fighter upgrade", {0, 1}},
        // Jet Fighter Upgrade shares Upgrade Fighter's cell: it now REQUIRES
        // Upgrade Fighter (control.cpp TECH_PREREQ), so exactly one of the two
        // is ever researchable at a time and the slot reads as a single
        // "upgrade the fighter line" rung -- the same one-slot-per-chain
        // treatment the barracks firearm chain and the shipyard's frigate line
        // already get. It used to sit alone at (4,0).
        {"jet fighter upgrade", {0, 1}},
        {"heavy bomber upgrade", {1, 1}},
        {"steel plane armor", {2, 1}},
        {"composite plane armor", {3, 1}},
        // Without an explicit slot this fell back to a computed index that, past
        // the 7-wide airbase unit reserve, landed on row 2 -- below the 2-row
        // command card, i.e. invisible. Col 4 of each row is free for a building
        // selection (col4/row0 is the delete button, which only shows for a UNIT
        // selection, never a building).
        {"atomic bomb", {4, 1}},
    };
    // Market techs now live on their OWN command-card page (market_page_ == 1),
    // with the buy/sell trade table on page 0 -- so techs no longer need to
    // dodge the trade icons and just use their natural static-index slots
    // (indices 0-6 map to distinct cells (0,0)..(1,1)). No overrides needed.
    // University trains no units (unit_slot_reserve is 0 for it), so its
    // static-index fallback alone already fills the whole 2x5 grid (10
    // techs, indices 0-9 -> every one of (0,0)..(4,1)) with zero slack.
    // Synthetic Fuel is pinned onto Gasoline's own slot (see item_hotkeys.
    // cpp's "university" group -- they're now one aliased hotkey/tier, only
    // one ever available at once, same idea as the barracks firearm chain),
    // which is what actually frees (3,1) -- Synthetic Fuel's OLD static-
    // index slot -- for Flak Tower Upgrade, the 11th tech.
    //
    // Radar is the 12th tech, and with all 10 cells reserved by static
    // indices there was no home for it. The fix reuses the exact
    // gasoline/synthetic-fuel trick on the OTHER two prerequisite chains:
    // Fracking requires Electric Drill and Beneficiation requires Smelting
    // (control.cpp TECH_PREREQS), so a chain's two members are NEVER
    // available at the same time. Pinning each upgrade onto its
    // prerequisite's cell (fracking -> electric drill's (2,0), beneficiation
    // -> smelting's (1,1)) frees each upgrade's OLD static slot -- (3,0) and
    // (1,0) -- and Radar takes (1,0). Net: 9 distinct cells, one spare, no
    // two icons ever superimpose.
    static const std::unordered_map<std::string, GridPos> university = {
        {"gasoline", {4, 0}},
        {"synthetic fuel", {4, 0}},
        {"flak tower upgrade", {3, 1}},
        {"electric drill", {2, 0}},
        {"fracking", {2, 0}},      // shares Electric Drill's slot (chain)
        {"smelting", {1, 1}},
        {"beneficiation", {1, 1}}, // shares Smelting's slot (chain)
        {"radar", {1, 0}},         // freed by Beneficiation moving onto Smelting
    };
    // Barracks: Binoculars has no parent unit and its static-index fallback
    // (past the 4 unit slots) computed row 2, off the 2-row card -- pin it to the
    // one free row-1 cell (2,1), clear of the rifle/infantry/artillery upgrades.
    static const std::unordered_map<std::string, GridPos> barracks = {
        // (4,1): the far-right row-1 cell, clear of the infantry upgrade (rifle
        // unit's column), artillery upgrade (artillery's column), and the firearm
        // chain (artillery column + 1) -- which previously all crowded (2,1).
        {"binoculars", {4, 1}},
    };
    // Fortress: the per-civ UNIQUE tech (only ever one is available, since each
    // is gated to a single civ via CIV_UPGRADE_OWNER) gets the free bottom-right
    // cell, clear of the waffen/janissary unit-upgrade slots at (0,1)/(1,1).
    static const std::unordered_map<std::string, GridPos> fortress = {
        {"emergency fighter program", {4, 1}},
        {"meiji restoration", {4, 1}},
        {"420mm mortar", {4, 1}},
        {"naval hegemony", {4, 1}}, // UK unique; shares the civ-unique cell (one per civ)
        // NOTE: royal marine upgrade is NOT pinned here -- it's placed directly
        // under the Royal Marine unit via tech_parent_units (like elite waffen /
        // royal janissary upgrades), landing at (0,1) beneath the unique unit.
        {"conscription", {3, 1}}, // all civs; distinct cell from the civ unique tech (4,1)
    };
    if (building == "factory") return &factory;
    if (building == "airbase") return &airbase;
    if (building == "university") return &university;
    if (building == "barracks") return &barracks;
    if (building == "fortress") return &fortress;
    return nullptr;
}

// An individual-unit upgrade tech should sit directly BELOW the unit it
// improves. This maps a tech to the candidate units it belongs under (first one
// actually shown wins) -- e.g. the whole barracks firearm chain (bolt-action ->
// semi-automatic -> assault, only one available at a time) sits under whatever
// rifle unit is on the card. Returns null for techs that aren't unit-specific.
const std::vector<std::string>* tech_parent_units(const std::string& tech) {
    static const std::unordered_map<std::string, std::vector<std::string>> m = {
        {"bolt action rifle", {"infantryman", "rifleman", "muscateer"}},
        {"semi automatic rifle", {"infantryman", "rifleman", "muscateer"}},
        {"assault rifle", {"infantryman", "rifleman", "muscateer"}},
        {"rifleman upgrade", {"muscateer"}},
        {"infantryman upgrade", {"rifleman", "muscateer"}},
        {"swordsman2 upgrade", {"swordsman"}},
        {"cavalry2 upgrade", {"cavalry"}},
        {"cavalry3 upgrade", {"cavalry2", "cavalry"}},
        {"camel corps upgrade", {"camel", "camel corps"}},
        {"elite waffen upgrade", {"waffen", "elite waffen"}},
        {"royal janissary upgrade", {"janissary", "royal janissary"}},
        {"royal marine upgrade", {"royal marine", "elite royal marine"}},
        {"destroyer upgrade", {"torpedo boat", "destroyer"}},
        {"battleship upgrade", {"battleship"}},
        {"torpedo boat upgrade", {"frigate", "torpedo boat"}},
        {"tiger2 tank upgrade", {"tiger tank", "heavy tank"}}, // King Tiger sits under the Tiger I
        {"artillery upgrade", {"artillery1", "artillery"}},    // sits under the field cannon
        {"heavy artillery upgrade", {"artillery", "artillery1"}}, // same slot as Artillery Upgrade
    };
    auto it = m.find(tech);
    return it == m.end() ? nullptr : &it->second;
}

// Generic (team-neutral -- these sprites have only 1-2 frames, not the 8
// per-team-colour frames a finished building's sprite has) scaffolding
// shown in place of a building's real sprite while its foundation is under
// construction. Direct port of obj_building's Draw event (assets/gmk):
// tower/aa tower always get the small placeholder regardless of their
// actual 64x96 footprint, everything else is picked by footprint size
// (96x96 base/fortress, 96x64 airbase/nuclear reactor, else the general
// 64x64 one). spr_2x2construction has 2 frames -- the GML picked a frame
// via `construction/100` (effectively always frame 0 for a 2-frame sprite
// outside of rounding edge cases), so instead this switches at the
// halfway point for a bit of visible progression.
std::pair<std::string, int> foundation_sprite(const Building& b) {
    if (b.name == "tower" || b.name == "aa tower") return {"spr_2x2small_construction", 0};
    // Any 1-tile (32x32) building -- palisade, outpost, and any other 1x1 --
    // uses the 1x1 scaffold, picked by footprint so it isn't limited to a
    // hardcoded name list.
    if (b.foot_w == 32 && b.foot_h == 32) return {"spr_1x1construction", 0};
    if (b.foot_w == 96 && b.foot_h == 96) return {"spr_3x3construction", 0};
    if (b.foot_w == 96 && b.foot_h == 64) return {"spr_3x2construction", 0};
    return {"spr_2x2construction", b.construction >= 50.0 ? 1 : 0};
}

// Greedy word-wrap for the command-card tooltip's description text (see
// draw_item_tooltip) -- the original's draw_text_ext(desc, 12, 380) let
// GameMaker wrap for it; TextRenderer has no such built-in, so this
// measures word-by-word and breaks at max_w, matching that behaviour.
std::vector<std::string> wrap_text(TextRenderer& text, const std::string& s, int size, int max_w) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        std::string candidate = line.empty() ? word : line + " " + word;
        int w, h;
        text.measure(candidate, size, w, h);
        if (w > max_w && !line.empty()) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') { flush_word(); }
        else word += c;
    }
    flush_word();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

SkirmishSettings default_settings() {
    SkirmishSettings s;
    s.n_players = 2;
    // Perf-testing hook: WW_STRESS_PLAYERS lets a WW_SKIP_MENU + WW_STRESS_TEST
    // run reproduce the 8-player, ~1000-unit battle the menu's Stress Test button
    // creates (the env path otherwise defaults to 2 players / ~130 units).
    if (const char* p = std::getenv("WW_STRESS_PLAYERS")) s.n_players = std::max(2, std::min(8, std::atoi(p)));
    s.map_size = 64; // "Normal" (kMapSizeValues[1]) -- bigger default, room to develop
    s.water = true;
    s.map_type = "random";
    if (const char* m = SDL_getenv("WW_MAP")) {
        s.map_type = m; // test override (skip-menu maps)
        s.reveal_mode = 2; // reveal the whole map so a WW_SHOT shows all terrain
    }
    return s; // civs left empty -> new_skirmish assigns team0=UK(0), team1=Germany(2)
}

const char* terrain_sprite(int tid) {
    switch (tid) {
        case 0: return "spr_green_grass";
        case 1: return "spr_bright_grass";
        case 2: return "spr_water";
        case 3: return "spr_sand_dirt";
        // 4-7: the editor's other ground-texture base kinds (worldwar_
        // campaign_editor's kTerrainKinds) -- new_from_level (scenario.cpp)
        // is the only thing that ever writes these into World::terrain, no
        // random-map generator produces them.
        case 4: return "spr_dirt";
        case 5: return "spr_gravel_dirt";
        case 6: return "spr_dark_dirt";
        case 7: return "spr_urban";
        // Themed-map terrain added for the ostland/negev/guam/stalingrad/
        // ardennes maps (see World's map generators): snow + snowy grass for
        // the winter maps, desert/beach sand for the desert and island.
        case 8: return "spr_snow";
        case 9: return "spr_snow_grass";
        case 10: return "spr_desert_sand";
        case 11: return "spr_beach_sand";
        default: return "spr_green_grass";
    }
}

double dist2(double x0, double y0, double x1, double y1) {
    double dx = x1 - x0, dy = y1 - y0;
    return dx * dx + dy * dy;
}

// SDL2 has no ellipse primitive -- approximate with a line-segment
// polygon (28 segments is smooth enough at typical selection-ring sizes).
void draw_ellipse(SDL_Renderer* renderer, int cx, int cy, int rx, int ry, SDL_Color color) {
    constexpr int kSegments = 28;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    int prev_x = cx + rx, prev_y = cy;
    for (int i = 1; i <= kSegments; ++i) {
        double theta = 2.0 * M_PI * i / kSegments;
        int x = cx + static_cast<int>(rx * std::cos(theta));
        int y = cy + static_cast<int>(ry * std::sin(theta));
        SDL_RenderDrawLine(renderer, prev_x, prev_y, x, y);
        prev_x = x; prev_y = y;
    }
}

// Fills `rect` with `color`, optionally rounding each of its 4 corners
// independently via a pixel-stepped quarter-circle cut (hard-edged rows,
// no anti-aliasing -- a classic retro "rounded rect", not a blurred one).
// Used for the fog-of-war overlay: only a tile's outward-facing (convex)
// corners get rounded, so interior tiles stay plain squares and keep
// tiling together seamlessly -- see draw()'s fog pass for the neighbor
// check that decides which corners to pass as true here.
void fill_rounded_tile(SDL_Renderer* renderer, SDL_Rect rect, int radius, SDL_Color color,
                       bool round_tl, bool round_tr, bool round_bl, bool round_br) {
    radius = std::min({radius, rect.w / 2, rect.h / 2});
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_Rect mid{rect.x, rect.y + radius, rect.w, rect.h - 2 * radius};
    if (mid.h > 0) SDL_RenderFillRect(renderer, &mid);
    for (int i = 0; i < radius; ++i) {
        int dy = radius - i;
        int dx = static_cast<int>(std::lround(std::sqrt(static_cast<double>(radius * radius - dy * dy))));
        int inset = radius - dx;
        int left_top = round_tl ? inset : 0, right_top = round_tr ? inset : 0;
        int left_bot = round_bl ? inset : 0, right_bot = round_br ? inset : 0;
        SDL_Rect top{rect.x + left_top, rect.y + i, rect.w - left_top - right_top, 1};
        SDL_Rect bot{rect.x + left_bot, rect.y + rect.h - 1 - i, rect.w - left_bot - right_bot, 1};
        if (top.w > 0) SDL_RenderFillRect(renderer, &top);
        if (bot.w > 0) SDL_RenderFillRect(renderer, &bot);
    }
}

// Draws a filled quarter-circle bulge into one corner of `rect` -- the
// exact mirror shape of what fill_rounded_tile's cut removes, used to
// round CONCAVE fog corners: where a visible tile sits with fog on two
// adjacent (orthogonal) sides, that tile's own corner is a sharp inward
// point unless something fills it back in. Same hard-edged stepped rows,
// no anti-aliasing.
void fill_corner_bulge(SDL_Renderer* renderer, SDL_Rect rect, int radius, SDL_Color color, bool top,
                       bool left) {
    radius = std::min({radius, rect.w, rect.h});
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    for (int i = 0; i < radius; ++i) {
        int dy = radius - i;
        int dx = static_cast<int>(std::lround(std::sqrt(static_cast<double>(radius * radius - dy * dy))));
        int inset = radius - dx;
        if (inset <= 0) continue;
        int row_y = top ? (rect.y + i) : (rect.y + rect.h - 1 - i);
        int col_x = left ? rect.x : (rect.x + rect.w - inset);
        SDL_Rect px{col_x, row_y, inset, 1};
        SDL_RenderFillRect(renderer, &px);
    }
}
// Test/verification hook: WW_SEED=<n> forces the match seed so the scenario
// (map + unit placement) is reproducible across runs -- lets a WW_SHOT
// capture do a clean before/after of the SAME battlefield. Unset => the perf
// counter, i.e. a fresh random map every launch (the normal behaviour).
uint64_t match_seed() {
    if (const char* s = SDL_getenv("WW_SEED")) return static_cast<uint64_t>(std::strtoull(s, nullptr, 10));
    return static_cast<uint64_t>(SDL_GetPerformanceCounter());
}

// Solves the assignment problem (minimum-cost perfect matching on an n x n
// cost matrix) exactly: returns, for each row i, the column assigned to it,
// such that the sum of cost[i][result[i]] over all i is minimal. Standard
// O(n^3) Hungarian/Kuhn-Munkres algorithm -- used by formation_slots to
// assign units to formation slots by total travel distance (see its own
// comment for why a positional heuristic wasn't good enough). 1-indexed
// internally (u/v/p/way keep a dummy 0th slot) purely because that's the
// natural indexing for this algorithm's potential-adjustment step; the
// public interface here is plain 0-indexed in and out.
std::vector<int> hungarian_assignment(const std::vector<std::vector<double>>& cost) {
    int n = static_cast<int>(cost.size());
    constexpr double kInf = 1e18;
    std::vector<double> u(n + 1, 0.0), v(n + 1, 0.0);
    std::vector<int> p(n + 1, 0), way(n + 1, 0);
    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(n + 1, kInf);
        std::vector<bool> used(n + 1, false);
        do {
            used[j0] = true;
            int i0 = p[j0], j1 = -1;
            double delta = kInf;
            for (int j = 1; j <= n; ++j) {
                if (used[j]) continue;
                double cur = cost[i0 - 1][j - 1] - u[i0] - v[j];
                if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                if (minv[j] < delta) { delta = minv[j]; j1 = j; }
            }
            for (int j = 0; j <= n; ++j) {
                if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
                else { minv[j] -= delta; }
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }
    std::vector<int> result(n);
    for (int j = 1; j <= n; ++j) {
        if (p[j] > 0) result[p[j] - 1] = j - 1;
    }
    return result;
}
} // namespace

GameClient::GameClient(SDL_Renderer* renderer, Audio& audio, const std::string& asset_dir,
                       const std::string& data_dir, int view_w, int view_h)
    : GameClient(renderer, audio, asset_dir, data_dir, view_w, view_h, default_settings()) {}

GameClient::GameClient(SDL_Renderer* renderer, Audio& audio, const std::string& asset_dir,
                       const std::string& data_dir, int view_w, int view_h,
                       const ww::sim::SkirmishSettings& settings)
    : match_(match_seed(), settings, data_dir),
      cam_(view_w, view_h, match_.world().px_w, match_.world().px_h), atlas_(renderer, asset_dir),
      audio_(audio),
      // Windows-only for now (matches the project's current toolchain
      // assumptions, see memory) -- a bundled .ttf would be needed for
      // cross-platform builds. Back to MS Serif (serife.fon, the classic
      // Windows bitmap font used in the original GameMaker project), now
      // synthesized bold -- there's no separate bold .fon for it, so this
      // is FreeType's algorithmic embolden (see TextRenderer's `bold`
      // param). segoeui.ttf stays as the last-resort fallback.
      text_(renderer, "C:\\Windows\\Fonts\\serife.fon", "C:\\Windows\\Fonts\\segoeui.ttf", 0, true),
      // Same MS Serif, regular weight -- used for the HP/armor/attack/carry
      // stat numbers in the unit info panel so they read lighter/smaller
      // than the (now bold) unit/building name next to them.
      text_regular_(renderer, "C:\\Windows\\Fonts\\serife.fon", "C:\\Windows\\Fonts\\segoeui.ttf", 0, false),
      view_w_(view_w), view_h_(view_h) {
    post_construct();
}

GameClient::GameClient(SDL_Renderer* renderer, Audio& audio, const std::string& asset_dir,
                       const std::string& data_dir, int view_w, int view_h,
                       const ww::campaign::Level& level, const std::string& campaign_name)
    : match_(match_seed(), level, data_dir),
      cam_(view_w, view_h, match_.world().px_w, match_.world().px_h), atlas_(renderer, asset_dir),
      audio_(audio),
      text_(renderer, "C:\\Windows\\Fonts\\serife.fon", "C:\\Windows\\Fonts\\segoeui.ttf", 0, true),
      text_regular_(renderer, "C:\\Windows\\Fonts\\serife.fon", "C:\\Windows\\Fonts\\segoeui.ttf", 0, false),
      view_w_(view_w), view_h_(view_h), campaign_name_(campaign_name), campaign_level_id_(level.id) {
    // Campaign custom-unit sprites: register a single static frame for each
    // custom unit that has an uploaded sprite, so the atlas can draw it. The PNG
    // is loaded from the campaign's own sprites folder (data/campaigns/sprites/
    // <sprite>.png) -- the campaign editor writes it there, so a campaign carries
    // its custom art with it (no copying into the game's asset tree needed).
    for (const auto& cu : level.custom_units) {
        if (cu.sprite.empty() || cu.sprite_w <= 0 || cu.sprite_h <= 0) continue;
        std::string sprite_path = data_dir + "/campaigns/sprites/" + cu.sprite + ".png";
        atlas_.register_sprite(cu.sprite,
                               {cu.sprite_w / 2, cu.sprite_h / 2, cu.sprite_w, cu.sprite_h, 1},
                               sprite_path);
    }
    // Cache the shown objectives (id + name) for the top-right tracker overlay;
    // hidden ones never appear (same rule the briefing screen uses).
    for (const auto& obj : level.objectives) {
        if (obj.hidden) continue;
        campaign_objectives_.push_back({obj.id, obj.name.empty() ? "(objective)" : obj.name});
    }
    post_construct();
}

// ---- the command sink ------------------------------------------------------
// See GameClient::issue in the header for why every player action routes
// through here. The single-player path is a straight apply_command, which is
// byte-for-byte what the input handlers used to do inline.
void GameClient::issue(const ww::sim::Command& cmd) {
    if (net_) {
        net_->submit(cmd);
        return; // executes on both machines in kInputDelay turns' time
    }
    ww::sim::apply_command(match_.world(), cmd);
}

void GameClient::set_session(ww::net::Session* s, int local_team) {
    net_ = s;
    local_team_ = local_team;
}

std::vector<uint32_t> GameClient::selected_ids() const {
    std::vector<uint32_t> ids;
    ids.reserve(selected_.size());
    for (auto ref : selected_) {
        if (const ww::sim::EntityCommon* c = const_cast<GameClient*>(this)->match_.world().common(ref))
            if (c->alive) ids.push_back(c->id);
    }
    return ids;
}

// Lockstep replacement for update()'s fixed-timestep accumulator. The sim runs
// in whole TURNS, and only once the session confirms both players' commands for
// that turn are in hand -- so the two machines execute the identical sequence.
// A missing packet stalls the game rather than letting one side run ahead,
// because a side that runs ahead is a desync.
bool GameClient::step_networked() {
    net_->poll();
    for (int guard = 0; guard < ww::net::kTicksPerTurn; ++guard) {
        ww::net::TurnState st = net_->begin_turn(match_.checksum(), turn_commands_);
        if (st == ww::net::TurnState::Stopped) {
            if (net_->status() == ww::net::Status::Desync) {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "Desync at turn %llu (yours %llu, theirs %llu). The match cannot "
                              "continue.",
                              static_cast<unsigned long long>(net_->desync_turn()),
                              static_cast<unsigned long long>(net_->local_checksum()),
                              static_cast<unsigned long long>(net_->remote_checksum()));
                net_error_ = buf;
            } else if (net_error_.empty()) {
                net_error_ = net_->error().empty() ? "the match ended" : net_->error();
            }
            return false;
        }
        if (st != ww::net::TurnState::Run) return true; // waiting on the peer
        // Both players' commands, in the session's fixed order. Applying them
        // in a different order on the two machines would desync just as surely
        // as dropping one, which is why the ordering lives in the session.
        for (const auto& c : turn_commands_) ww::sim::apply_command(match_.world(), c);
        for (int t = 0; t < ww::net::kTicksPerTurn; ++t) {
            snapshot_positions();
            match_.step(kFixedDt);
            drain_events();
        }
        net_->end_turn();
        return true; // one turn per frame keeps the pace at real time
    }
    return true;
}

void GameClient::post_construct() {
    settings_.load();
    SDL_ShowCursor(SDL_DISABLE); // replaced by the custom spr_mouse cursor, drawn in draw()
    cam_.zoom = kDefaultZoom;
    for (auto ref : match_.world().active_buildings) {
        Building* b = match_.world().get_building(ref);
        if (b && b->common.team == 0 && b->name == "base") {
            cam_.center_on(b->common.x, b->common.y);
            break;
        }
    }
    // Swap the menu's looping title theme for the in-game shuffled playlist
    // (main.py: stop_music(); play_ingame() on match start).
    audio_.stop_music();
    audio_.play_ingame();
}

void GameClient::populate_stress_test() {
    ww::sim::populate_stress_test(match_.world(), match_.control(), match_.rng());
}

void GameClient::update(double real_dt) {
    // Chat bar pauses the whole game (sim tick, camera edge-pan, effect/
    // notification aging alike) -- Enter opens it, see handle_event.
    if (paused_) return;
    audio_.update_music(); // roll on to the next in-game track when one ends
    auto vr = cam_.visible_rect();
    audio_.set_listener(vr.x, vr.y, vr.w, vr.h);

    // AoE-style edge-pan: scroll the camera when the cursor sits near a
    // window edge, on top of arrow-key panning (app.cpp). Skipped while
    // drag-selecting (an edge-hugging drag should extend the marquee, not
    // also scroll the map under it) and when the window doesn't have mouse
    // focus (mouse_pos_ is only updated on MOUSEMOTION, so a stale
    // in-window position could otherwise cause phantom panning once the
    // cursor/window loses focus, e.g. after alt-tabbing away).
    if (!dragging_ && (SDL_GetMouseFocus() || SDL_getenv("WW_TEST_MOUSEPOS"))) {
        int margin = ui(kEdgePanMarginNative);
        // Ramps from 0 speed at the zone's inner boundary (dist == margin)
        // to full kEdgePanSpeed right at the screen edge (dist == 0).
        auto edge_factor = [margin](int dist) {
            return std::clamp(1.0 - static_cast<double>(dist) / margin, 0.0, 1.0);
        };
        double dx = 0, dy = 0;
        if (mouse_pos_.x < margin) dx -= kEdgePanSpeed * edge_factor(mouse_pos_.x) * real_dt;
        else if (mouse_pos_.x > view_w_ - margin) dx += kEdgePanSpeed * edge_factor(view_w_ - mouse_pos_.x) * real_dt;
        if (mouse_pos_.y < margin) dy -= kEdgePanSpeed * edge_factor(mouse_pos_.y) * real_dt;
        else if (mouse_pos_.y > view_h_ - margin) dy += kEdgePanSpeed * edge_factor(view_h_ - mouse_pos_.y) * real_dt;
        if (dx != 0 || dy != 0) cam_.pan(dx, dy);
    }

    render_clock_ += real_dt;
    if (nuke_flash_t_ > 0.0) nuke_flash_t_ = std::max(0.0, nuke_flash_t_ - real_dt);
    if (nuke_shake_t_ > 0.0) nuke_shake_t_ = std::max(0.0, nuke_shake_t_ - real_dt);
    // Networked match: the lockstep session owns the clock, not this
    // accumulator. Both machines must execute the same turns in the same order,
    // which means the sim can only advance when the peer's commands for the
    // next turn are in hand -- see step_networked().
    if (net_) {
        step_networked();
        return;
    }
    accumulator_ += real_dt;
    // Spiral-of-death guard. The fixed-timestep loop below runs one sim step per
    // kFixedDt of accumulated real time -- but if a single frame is slow (a heavy
    // battle's pathfinding, an OS hitch, an alt-tab), real_dt balloons and the
    // loop schedules MANY catch-up steps at once. Those steps make the frame even
    // slower, which inflates the next real_dt, which schedules even more steps...
    // a runaway that locks the game up (the crash log showed update time going
    // 83 -> 166 -> 334 -> 1180 -> 3993 ms over five frames, entity counts flat).
    // Cap the catch-up: run at most a few steps per frame and DROP any remaining
    // backlog, so under extreme load the sim briefly runs slow-motion instead of
    // freezing. Determinism is untouched -- every executed step is still exactly
    // kFixedDt (the golden checksum drives match_.step directly, not this loop).
    // 3 (not more): a huge-battle sim step can be ~35ms at ~1000 units, so 5
    // bunched catch-up steps made ~175ms hitches; 3 caps the worst felt stutter
    // at ~105ms while only ever engaging under extreme load (normal play does
    // <=1 step/frame and never reaches the cap, so it's unaffected).
    constexpr int kMaxStepsPerFrame = 3;
    // ALSO cap the catch-up by WALL-CLOCK time, not just a step count. In a
    // ~1000-unit battle a single sim step is inherently ~60-120ms, so 3 bunched
    // catch-up steps stacked into ~300-375ms freezes (seen in a stress-test perf
    // log). This budget stops the loop once the frame has already spent too long
    // stepping, so a heavy frame runs ONE big step (~one step's worth of stutter)
    // instead of three. Normal play's steps are ~5ms, so all three fit under the
    // budget and it's unaffected. Client-side pacing only -- every executed step
    // is still exactly kFixedDt, so the golden checksum is untouched.
    constexpr double kMaxUpdateMs = 90.0;
    int steps = 0;
    double spent_ms = 0.0;
    while (accumulator_ >= kFixedDt && steps < kMaxStepsPerFrame) {
        snapshot_positions(); // capture pre-tick positions for render interpolation
        auto step_t0 = std::chrono::steady_clock::now();
        match_.step(kFixedDt);
        auto step_t1 = std::chrono::steady_clock::now();
        drain_events();
        accumulator_ -= kFixedDt;
        ++steps;

        double this_ms = std::chrono::duration<double, std::milli>(step_t1 - step_t0).count();
        tick_ms_history_.push_back(this_ms);
        while (tick_ms_history_.size() > 20) tick_ms_history_.pop_front(); // ~1s at the fixed 20Hz rate
        spent_ms += this_ms;
        if (spent_ms >= kMaxUpdateMs) break; // over the time budget -> stop catching up this frame
    }
    if (accumulator_ >= kFixedDt) accumulator_ = 0.0; // hit a cap -> discard the un-runnable backlog
    // Campaign-level game over: no victory/defeat screen (see the
    // "minimal, no banner" decision) -- record a win, if this was one,
    // then drop straight back to the menu via the same quit_to_menu_ flag
    // the pause menu's own "Quit Game" button already uses, so nothing
    // else about the return-to-menu path needs to change. Skirmish
    // matches (campaign_level_id_ empty) are untouched -- Control::
    // game_over/winner are still set for them (check_win doesn't know or
    // care what started the match), this just never acts on it.
    if (!game_over_handled_ && match_.control().game_over) {
        const Control& control = match_.control();
        // control.winner is nullopt when NO team has a base -- not just
        // "every base was destroyed in a real battle", but also the
        // degenerate case of a level whose buildings haven't been placed
        // in the editor yet (see check_win, sim/src/control_ai.cpp: an
        // empty `alive` list still satisfies "at most one alliance
        // standing"). check_win runs every tick, so an author testing a
        // buildings-less WIP level would otherwise get bounced straight
        // back to the menu on literally the first frame -- nobody won or
        // lost anything, so only a REAL winner ends the match here.
        if (control.winner.has_value()) {
            game_over_handled_ = true;
            bool team0_won = control.teams[0].ally == control.teams[*control.winner].ally;
            // Record a campaign win as before.
            if (team0_won && !campaign_level_id_.empty()) {
                settings_.completed_campaign_levels.insert(campaign_level_key(campaign_name_, campaign_level_id_));
                settings_.save();
            }
            // Raise the on-screen VICTORY/DEFEAT banner (both skirmish AND
            // campaign now) -- the player then chooses to keep watching the map
            // or go to the statistics screen. Plays the win/lose sting once.
            game_over_banner_open_ = true;
            game_over_won_ = team0_won;
            audio_.play(team0_won ? "victory" : "lose", 0, 0, false);
        }
    }
    // Cosmetic on-hit / battle-damage particles, generated client-side off
    // each building's live hit_timer/hp -- a direct port of game/entity.py's
    // Building.update (buildings only, matching the reference: units flash
    // but don't shed debris). Client-side RNG only; never touches the sim.
    if (!paused_) {
        auto rnd = []() { return static_cast<double>(std::rand()) / RAND_MAX; };
        for (auto ref : match_.world().active_buildings) {
            ww::sim::Building* b = match_.world().get_building(ref);
            if (!b || !b->common.alive || !b->complete) continue;
            // Struck this instant: shed a few debris fragments that arc out
            // and fall (rate tuned so ~0.25/frame at 60fps == the port's
            // per-update 0.25 chance).
            if (b->hit_timer > 0 && rnd() < 15.0 * real_dt) {
                double a = rnd() * 6.2831853;
                double spd = 60.0 + rnd() * 80.0;
                ClientEffect d;
                d.sprite = "spr_debris";
                d.x = b->common.x + (rnd() - 0.5) * b->foot_w / 2.0;
                d.y = b->common.y + (rnd() - 0.5) * b->foot_h / 2.0;
                d.lifetime = 0.6;
                d.fade = 0.3;
                d.vx = std::cos(a) * spd;
                d.vy = std::sin(a) * spd - 40.0; // upward bias
                d.gravity = 380.0;
                effects_.push_back(std::move(d));
            }
            // Heavily damaged: smoke + glowing embers rise from the structure.
            if (b->common.hp < b->common.max_hp * 0.4) {
                double ox = (rnd() - 0.5) * b->foot_w * (2.0 / 3.0);
                if (rnd() < 7.0 * real_dt) {
                    ClientEffect s;
                    s.sprite = "spr_smoke";
                    s.x = b->common.x + ox;
                    s.y = b->common.y + (rnd() - 0.5) * b->foot_h / 4.0;
                    s.lifetime = 1.0;
                    s.fade = 0.5;
                    s.vy = -30.0;
                    s.draw_scale = 0.7; // smoke 30% smaller
                    effects_.push_back(std::move(s));
                }
                if (rnd() < 6.0 * real_dt) {
                    // Burning building: the RED flame frame of spr_burn_effect
                    // (frame 0; frame 1 is yellow, 2 is smoke) held fixed, at
                    // half size, mirrored horizontally on a 2s cadence like the
                    // ocean so it flickers side to side. Sits in place.
                    ClientEffect e;
                    e.sprite = "spr_burn_effect";
                    e.x = b->common.x + ox;
                    e.y = b->common.y + (rnd() - 0.5) * b->foot_h / 4.0;
                    e.lifetime = 0.5;
                    e.fade = 0.25;
                    e.frame = 0;            // red flame
                    e.fixed_frame = true;   // don't animate through yellow/smoke
                    e.draw_scale = 0.5;     // 50% smaller
                    e.flip = (static_cast<int>(render_clock_ / 2.0) & 1) != 0;
                    effects_.push_back(std::move(e));
                }
            }
        }
    }
    for (auto& fx : effects_) {
        fx.t += real_dt;
        if (fx.vx != 0.0 || fx.vy != 0.0 || fx.gravity != 0.0) { // ballistic debris/embers
            fx.x += fx.vx * real_dt;
            fx.y += fx.vy * real_dt;
            fx.vy += fx.gravity * real_dt;
        }
    }
    effects_.erase(std::remove_if(effects_.begin(), effects_.end(),
                                  [](const ClientEffect& e) { return e.t >= e.lifetime; }),
                   effects_.end());
    // obj_smoke's per-frame updates (vspeed += 0.005, y += vspeed,
    // swell += 0.008, image_alpha -= 0.006), scaled by how many 60fps
    // GML-frames this real-time tick represents -- same *60 convention as
    // the ship-shell arc (world.cpp/projectile_behavior.cpp).
    double smoke_frames = 60.0 * real_dt;
    for (auto& s : smoke_) {
        s.vy += 0.005 * smoke_frames;
        s.y += s.vy * smoke_frames;
        s.scale += 0.008 * smoke_frames;
        s.alpha -= 0.006 * smoke_frames;
    }
    smoke_.erase(std::remove_if(smoke_.begin(), smoke_.end(),
                                [](const ClientSmoke& s) { return s.alpha <= 0.0; }),
                smoke_.end());
    for (auto& fl : target_flashes_) fl.t += real_dt;
    target_flashes_.erase(std::remove_if(target_flashes_.begin(), target_flashes_.end(),
                                         [](const TargetFlash& f) { return f.t >= f.lifetime; }),
                          target_flashes_.end());
    for (auto& n : notifications_) n.t += real_dt;
    notifications_.erase(std::remove_if(notifications_.begin(), notifications_.end(),
                                        [](const ClientNotification& n) { return n.t >= n.lifetime; }),
                         notifications_.end());
    for (auto& c : chat_log_) c.t += real_dt;
    chat_log_.erase(std::remove_if(chat_log_.begin(), chat_log_.end(),
                                   [](const ClientChatMessage& c) { return c.t >= c.lifetime; }),
                    chat_log_.end());
    for (auto& p : minimap_pings_) p.t += real_dt;
    minimap_pings_.erase(std::remove_if(minimap_pings_.begin(), minimap_pings_.end(),
                                        [](const MinimapPing& p) { return p.t >= p.lifetime; }),
                         minimap_pings_.end());
}

void GameClient::snapshot_positions() {
    prev_unit_pos_.clear();
    for (auto ref : match_.world().active_units) {
        if (Unit* u = match_.world().get(ref)) prev_unit_pos_[u->common.id] = {u->common.x, u->common.y};
    }
    prev_proj_pos_.clear();
    for (auto ref : match_.world().active_projectiles) {
        if (Projectile* p = match_.world().get_projectile(ref)) {
            prev_proj_pos_[p->common.id] = {p->common.x, p->common.y};
        }
    }
}

void GameClient::drain_events() {
    for (const auto& ev : match_.events().events()) {
        switch (ev.type) {
            case EventType::Sound:
                if (ev.key == "building_ready") {
                    audio_.building_sound(ev.text);
                } else {
                    bool has_pos = ev.x != 0.0 || ev.y != 0.0;
                    audio_.play(ev.key, ev.x, ev.y, has_pos);
                }
                break;
            case EventType::Effect:
                if (ev.key == "spr_smoke") {
                    // obj_missile/Step.gml creates 3 of these per shot,
                    // each with its own random upward drift speed
                    // (random_range(-3,-1)) -- a real drifting/fading
                    // particle, not a generic frame-stepped ClientEffect
                    // (see ClientSmoke's comment).
                    double vy = -1.0 - (std::rand() % 2000) / 1000.0;
                    smoke_.push_back({ev.x, ev.y, vy});
                } else if (ev.key == "spr_water_splash") {
                    // Cannonball splashdown: no fiery explosion over water, a
                    // blue-tinted shockwave ring that swells and fades instead
                    // (reuses spr_shockwave -- no dedicated splash sprite).
                    ClientEffect s;
                    s.sprite = "spr_shockwave";
                    s.x = ev.x;
                    s.y = ev.y;
                    s.lifetime = 0.5;
                    s.fade = 0.5;
                    s.draw_scale = 1.4;
                    s.tint = {90, 160, 230, 255};
                    effects_.push_back(s);
                } else if (ev.key == "nuke_flash") {
                    // Atomic detonation cue (not a sprite): a full-screen white
                    // flash, a heavy screen shake, and a billowing rising smoke
                    // cloud over the blast. ev.pitch carries the blast radius.
                    nuke_flash_t_ = 0.7;   // seconds of white-out fade
                    nuke_shake_t_ = 1.1;   // seconds of camera shake
                    double R = ev.pitch > 0 ? ev.pitch : 260.0;
                    // A dense mushroom of rising smoke puffs across the blast.
                    for (int i = 0; i < 26; ++i) {
                        double a = i * 0.618 * 6.283185;
                        double r = (i % 5) * R * 0.16;
                        double px = ev.x + std::cos(a) * r;
                        double py = ev.y + std::sin(a) * r - (i % 4) * 8.0; // stack upward
                        double vy = -0.6 - (std::rand() % 2200) / 1000.0;
                        smoke_.push_back({px, py, vy, 1.4 + (i % 3) * 0.5}); // big billowing puffs
                    }
                } else if (ev.key.rfind("death_", 0) == 0) {
                    // Corpse / vehicle wreck / building rubble (+ its
                    // explosion or collapse boom) -- resolved to a real
                    // sprite by spawn_death_effect, since the sim only knows
                    // the entity name/state, not which sprite variants exist.
                    spawn_death_effect(ev);
                } else {
                    // Multi-frame effects live exactly as long as their
                    // real animation takes at their own effect_fps;
                    // single-frame ones (sparks, slices) keep the old
                    // snappy 0.4s.
                    const auto* m = atlas_.meta(ev.key);
                    double lifetime = (m && m->frames > 1) ? m->frames / effect_fps(ev.key) : 0.4;
                    ClientEffect fx{ev.key, ev.x, ev.y, 0.0, lifetime, 0};
                    // The atomic mushroom is drawn much larger than its native
                    // size (ev.pitch carries the requested scale, e.g. 4x).
                    if (ev.key == "spr_explosion_mushroom" && ev.pitch > 0.0) fx.draw_scale = ev.pitch;
                    // ...and lingers a few seconds LONGER than its raw animation
                    // so the atomic cloud hangs in the air. The frame logic in
                    // draw() spreads the animation across this whole lifetime, so
                    // the cloud keeps billowing the entire time (rather than
                    // freezing on the last frame); it stays solid, then fades out
                    // over the final ~1.2s.
                    if (ev.key == "spr_explosion_mushroom") {
                        fx.lifetime += 2.5;
                        fx.fade = 1.2;
                    }
                    effects_.push_back(fx);
                }
                break;
            case EventType::Notify: {
                // Sim decides WHAT happened (key) and hands over the raw
                // payload (text); formatting the actual sentence is the
                // client's job, same split of responsibility as Sound's
                // key -> sound-file mapping above.
                std::string line;
                if (ev.key == "age_advance") {
                    static const char* kEraNames[4] = {"Victorian Era", "Industrial Era", "War Era",
                                                       "Scientific Era"};
                    int era_idx = std::clamp(match_.control().teams[0].era, 0, 3);
                    line = std::string("Advanced to the ") + kEraNames[era_idx] + ".";
                } else if (ev.key == "research_complete" || ev.key == "building_ready" ||
                          ev.key == "unit_created") {
                    std::string name = ev.text;
                    if (!name.empty()) name[0] = std::toupper(static_cast<unsigned char>(name[0]));
                    const char* suffix = ev.key == "research_complete" ? " research complete."
                                        : ev.key == "building_ready"   ? " built."
                                                                       : " created.";
                    line = name + suffix;
                } else if (ev.key == "map_message") {
                    // editor's Events tab: author-written
                    // text, shown verbatim (no generated suffix, unlike the
                    // cases above) -- see World::message_triggers.
                    line = ev.text;
                } else if (ev.key == "under_attack") {
                    // The original's attack warning. The sim raises this for
                    // every team (World::hurt, rate-limited per team); showing
                    // it is a per-viewer decision, so the filter lives here --
                    // a spectator watching two AIs shouldn't be told that one
                    // of them is under attack, and in a future network match
                    // each peer filters to its own local_team_.
                    if (ev.team != local_team_ || spectator_) break;
                    line = "You are under attack!";
                    audio_.play("warning", 0, 0, false);
                    // ...and a marker on the minimap where it happened, so the
                    // line answers "where" as well as "what". A raid on an
                    // outlying woodline reads identically to one on the town
                    // centre without it.
                    minimap_pings_.push_back({ev.x, ev.y, 0.0, 6.0});
                } else if (ev.key == "team_age_advance") {
                    // Every team's age-up, named, in the chat feed (the sim
                    // stamps whose it was; see building_behavior.cpp). Aging is
                    // the biggest single power spike in the game and there was
                    // no way to see one happen anywhere but your own base.
                    static const char* kEraNames[4] = {"Victorian Era", "Industrial Era", "War Era",
                                                       "Scientific Era"};
                    auto& tms = match_.control().teams;
                    if (ev.team < 0 || ev.team >= static_cast<int>(tms.size())) break;
                    int era_idx = std::clamp(tms[ev.team].era, 0, 3);
                    // Civ name straight out of civs.json, the same source the
                    // setup screen and the headless arena report both use.
                    std::string who = "Player " + std::to_string(ev.team + 1);
                    const auto& civs = match_.data().civs().at("civs");
                    int civ = tms[ev.team].civ;
                    if (civ >= 0 && civ < static_cast<int>(civs.size()))
                        who = civs[civ].value("name", who);
                    if (ev.team == local_team_ && !spectator_) who = "You";
                    chat_log_.push_back(
                        {who + (who == "You" ? " have" : " has") + " advanced to the " +
                             kEraNames[era_idx] + ".",
                         0.0, 10.0});
                    break; // chat feed, not the event-log stack
                }
                if (!line.empty()) notifications_.push_back({line, 0.0, 5.0});
                break;
            }
            case EventType::MusicStop:
                audio_.stop_music(); // sim signals game over (control_ai.cpp check_win)
                break;
            default:
                break; // Warn/Victory/Defeat: no HUD banner yet (documented deferral)
        }
    }
    match_.events().clear();
}

void GameClient::spawn_death_effect(const ww::sim::SimEvent& ev) {
    auto exists = [&](const std::string& s) { return !s.empty() && atlas_.meta(s) != nullptr; };
    // Sprite keys use underscores; a couple of catalog names have spaces
    // ("camel corps") -- match _make_corpse's name.replace(" ", "_").
    std::string n = ev.text;
    for (char& c : n) if (c == ' ') c = '_';
    std::string n2 = ev.alt; // fallback base (generic unit type), for skinned planes
    for (char& c : n2) if (c == ' ') c = '_';
    // Returns spr_<primary-base>_<suffix> if it exists, else spr_<fallback-
    // base>_<suffix>, else "" -- e.g. a spitfire-skinned fighter has no
    // spr_spitfire_rubble but resolves to spr_fighter_rubble via the fallback.
    auto pick = [&](const std::string& suffix) -> std::string {
        for (const std::string& base : {n, n2}) {
            if (base.empty()) continue;
            std::string s = "spr_" + base + "_" + suffix;
            if (exists(s)) return s;
        }
        return "";
    };

    // A persistent, on-the-ground corpse/wreck/rubble pile (drawn under
    // units & buildings). foot_px > 0 scales it to a building's footprint.
    auto ground = [&](const std::string& spr, int frame, double lifetime, double fade, double foot_px) {
        if (!exists(spr)) return;
        // A single-frame wreck/rubble sprite is team-agnostic -> clamp any
        // requested team frame back to 0. A multi-frame one (e.g. the
        // 8-colour spr_artillery_rubble) uses the dying unit's team colour.
        const auto* fm = atlas_.meta(spr);
        if (fm && frame >= fm->frames) frame = 0;
        ClientEffect e;
        e.sprite = spr;
        e.x = ev.x;
        e.y = ev.y;
        e.lifetime = lifetime;
        e.fade = fade;
        e.fixed_frame = true;
        e.ground = true;
        e.frame = frame;
        e.flip = ev.flip;
        e.scale = foot_px;
        effects_.push_back(std::move(e));
    };
    // A transient on-top explosion (frame-stepped for its whole animation).
    auto boom = [&](const std::string& spr) {
        if (!exists(spr)) return;
        const auto* m = atlas_.meta(spr);
        double lifetime = (m && m->frames > 1) ? m->frames / effect_fps(spr) : 0.4;
        effects_.push_back({spr, ev.x, ev.y, 0.0, lifetime, 0});
    };
    // Corpse/wreck colour frame is the dying unit's team COLOUR (its chosen
    // palette index 0-7), not the raw team index -- a gray team (colour 6)
    // must leave a gray corpse, not a blue (frame-0) one. Single-frame shared
    // wrecks just clamp this back to 0 inside ground().
    const auto& death_teams = match_.control().teams;
    int death_col = (ev.team >= 0 && ev.team < static_cast<int>(death_teams.size()))
                        ? death_teams[ev.team].colour
                        : 0;

    if (ev.key == "death_building") {
        if (n == "farm") { // withers to its dead-farm frame, no boom
            ground("spr_farm", 1, 6.0, 3.0, ev.foot);
            return;
        }
        if (ev.deleted) { // cancelled foundation -> just a dust poof
            boom("spr_dust_poof");
            return;
        }
        // Every destroyed building leaves footprint-sized rubble: its own
        // spr_<name>_rubble if it has one, else the generic 96/64 pile.
        bool big = ev.foot >= 96.0;
        std::string generic = big ? "spr_96_rubble" : "spr_64_rubble";
        std::string own = pick("rubble");
        std::string rubble = !own.empty() ? own : exists(generic) ? generic : "spr_64_rubble";
        ground(rubble, 0, 45.0, 8.0, ev.foot);
        boom(ev.shell_kill ? "spr_explosion_large" : "spr_explosion");
        return;
    }

    // death_air: planes crash & leave wreckage (spr_<skin>_rubble, or the
    // generic spr_<type>_rubble for a civ-skinned airframe).
    if (ev.key == "death_air") {
        boom("spr_explosion_large");
        ground(pick("rubble"), death_col, 20.0, 5.0, 0.0);
        return;
    }

    // Ballistic missile: a distinct destroyed-launcher wreck per state (the sim
    // sets ev.text to the sentinel; spr_<n> is the wreck art). The damaging
    // blast's explosion visual is emitted separately by the sim's death sweep.
    if (n == "ballistic_missile_destroyed" || n == "ballistic_missile_unpacked_destroyed") {
        ground("spr_" + n, death_col, 20.0, 5.0, 0.0);
        return;
    }

    // Artillery leaves its own destroyed-piece wreck (per-team coloured): the
    // tier-2 gun ("artillery") and tier-1 field cannon ("artillery1") each get
    // their own art (spr_artillery2/1_destroyed), replacing the old rubble.
    if (n == "artillery" || n == "artillery1" || n == "heavy_artillery") {
        ground(n == "heavy_artillery" ? "spr_heavy_artillery_destroyed"
               : n == "artillery"     ? "spr_artillery2_destroyed"
                                      : "spr_artillery1_destroyed",
               death_col, 20.0, 5.0, 0.0);
        boom("spr_explosion");
        return;
    }

    // death_unit (ground): ships sink, tanks/artillery leave a wreck,
    // infantry/cavalry leave a coloured corpse -- else just burst.
    // The transport ship and both aircraft-carrier tiers have no sink sprite of
    // their own, so they borrow the battleship's sinking animation (per request).
    std::string sink_spr = pick("sink");
    if (sink_spr.empty() && (n == "transport_ship" || n.rfind("aircraftcarrier", 0) == 0))
        sink_spr = "spr_battleship_sink";
    if (std::string sink = sink_spr; !sink.empty()) { // ships
        // Sinking: show the ship's single-frame sink sprite and simply fade it
        // out (no shrink, no drift). A player-DELETED boat skips the explosion.
        ClientEffect e;
        e.sprite = sink;
        e.x = ev.x;
        e.y = ev.y;
        e.lifetime = 6.0;
        e.fade = 3.5;
        e.fixed_frame = true;
        e.ground = true;
        e.flip = ev.flip;
        effects_.push_back(std::move(e));
        if (!ev.deleted) boom("spr_explosion");
        return;
    }
    std::string wreck = pick("rubble");
    if (wreck.empty() && ev.mechanical) wreck = "spr_aa_gun_rubble"; // fallback: shared AA-gun wreck
    if (exists(wreck)) {
        // Team frame for a colour-per-team wreck (spr_artillery_rubble);
        // clamped to 0 by ground() for the single-frame shared wrecks.
        ground(wreck, death_col, 20.0, 5.0, 0.0);
        boom("spr_explosion");
    } else if (std::string dead = pick("dead"); !dead.empty()) {
        ground(dead, death_col, 12.0, 3.0, 0.0);
    } else {
        boom("spr_explosion");
    }
}

void GameClient::handle_event(const SDL_Event& ev) {
    if (stats_open_) {
        // The post-game stats screen is modal: it eats all input except the
        // cursor position, its tab clicks, and Return-to-Menu.
        if (ev.type == SDL_MOUSEMOTION) {
            mouse_pos_ = {ev.motion.x, ev.motion.y};
        } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
            handle_stats_click(ev.button.x, ev.button.y);
        } else if (ev.type == SDL_KEYDOWN &&
                   (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_RETURN)) {
            quit_to_menu_ = true; // Esc/Enter = leave to menu
        }
        return;
    }
    // Victory/Defeat banner is NON-modal: a click on one of its two buttons is
    // consumed, but anything else (map pan/zoom, selection) still works so the
    // player can keep looking around while it's up.
    if (game_over_banner_open_ && ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        if (handle_game_over_click(ev.button.x, ev.button.y)) return;
    }
    if (chat_open_) {
        // Swallows everything else (clicks, hotkeys) while typing --
        // otherwise e.g. typing "blitz" would also fire the 'b' barracks
        // hotkey on every keystroke. MOUSEMOTION is handled first and
        // falls through to the shared update below rather than being
        // swallowed too -- the OS cursor is hidden (SDL_ShowCursor(
        // SDL_DISABLE) at construction) and replaced by a custom sprite
        // drawn at mouse_pos_ (see draw()), so if this branch didn't
        // update mouse_pos_ here, that sprite would just freeze in place
        // for as long as the chat bar stayed open.
        if (ev.type == SDL_MOUSEMOTION) {
            mouse_pos_ = {ev.motion.x, ev.motion.y};
        } else if (ev.type == SDL_TEXTINPUT) {
            chat_input_ += ev.text.text;
        } else if (ev.type == SDL_KEYDOWN) {
            if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER) {
                submit_chat();
            } else if (ev.key.keysym.sym == SDLK_ESCAPE) {
                chat_open_ = false;
                paused_ = false;
                chat_input_.clear();
                SDL_StopTextInput();
            } else if (ev.key.keysym.sym == SDLK_BACKSPACE && !chat_input_.empty()) {
                chat_input_.pop_back();
            }
        }
        return;
    }
    if (pause_menu_open_) {
        // Swallows everything else while open, same reasoning (and same
        // MOUSEMOTION exception, same underlying frozen-cursor bug) as the
        // chat bar above. Escape resumes without needing to hit the button
        // again; clicks are hit-tested against last frame's rects (see
        // draw_pause_menu), same "layout then hit-test" pattern the
        // command card uses.
        if (ev.type == SDL_MOUSEMOTION) {
            mouse_pos_ = {ev.motion.x, ev.motion.y};
        } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
            handle_pause_menu_click(ev.button.x, ev.button.y);
        } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
            pause_menu_open_ = false;
            paused_ = false;
        }
        return;
    }
    if (ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER)) {
        open_chat();
        return;
    }
    if (ev.type == SDL_MOUSEMOTION) {
        mouse_pos_ = {ev.motion.x, ev.motion.y};
        // Click-and-drag the minimap viewport: as long as the button is
        // still down from a press that started on the minimap, keep
        // following the cursor instead of only jumping once on mousedown.
        if (minimap_dragging_) jump_minimap_to(mouse_pos_.x, mouse_pos_.y);
        // AoE-style middle-mouse-button drag-to-pan: the world point under
        // the cursor at mousedown stays under the cursor as you drag (pan
        // by the NEGATIVE of the mouse's own movement, so dragging right
        // reveals map to the left, same as grabbing and dragging a map).
        if (mid_dragging_) {
            cam_.pan(-(mouse_pos_.x - mid_drag_last_.x), -(mouse_pos_.y - mid_drag_last_.y));
            mid_drag_last_ = mouse_pos_;
        }
        // Right-drag past the 6px click threshold arms formation mode (same
        // threshold the left box-select uses); the ghost preview then tracks
        // the cursor until release.
        if (rdrag_start_) {
            int ddx = mouse_pos_.x - rdrag_start_->x, ddy = mouse_pos_.y - rdrag_start_->y;
            if (ddx * ddx + ddy * ddy > 36) r_dragging_ = true;
        }
    } else if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
        mouse_pos_ = {ev.button.x, ev.button.y};
    }
    if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        // Manual, stricter double-click: two presses within kDoubleClickMs and
        // on nearly the same spot (SDL's own ev.button.clicks used a too-lenient
        // OS interval that made ordinary quick clicks select-all).
        Uint32 now = SDL_GetTicks();
        int ddx = ev.button.x - last_left_click_pos_.x, ddy = ev.button.y - last_left_click_pos_.y;
        double_click_ = (now - last_left_click_ms_) <= kDoubleClickMs && (ddx * ddx + ddy * ddy) <= 36;
        last_left_click_ms_ = now;
        last_left_click_pos_ = {ev.button.x, ev.button.y};
        handle_left_down(ev.button.x, ev.button.y);
    } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
        handle_left_up(ev.button.x, ev.button.y);
    } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_RIGHT) {
        // With a unit selection, defer the right-click: a drag becomes a
        // formation (handled on button-up), a plain click issues the normal
        // order. Everything else (building rally, armed attack-move, cancel
        // placement) keeps acting immediately on press as before.
        //
        // Spectator mode (Start Stress Test preview) drops this branch
        // entirely -- right-click is how every move/attack/gather order
        // gets issued, and a pure spectator shouldn't be able to trigger
        // any of them (see set_spectator()'s comment). rdrag_start_ simply
        // never gets set, so the MOUSEBUTTONUP branch below stays a no-op
        // too without needing its own guard.
        if (orders_locked()) {
            // no-op
        } else if (formation_drag_eligible()) {
            rdrag_start_ = SDL_Point{ev.button.x, ev.button.y};
            r_dragging_ = false;
        } else {
            rdrag_start_.reset();
            handle_right_down(ev.button.x, ev.button.y);
        }
    } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_RIGHT) {
        if (rdrag_start_) {
            if (r_dragging_) {
                double ax, ay, bx, by;
                cam_.screen_to_world(rdrag_start_->x, rdrag_start_->y, ax, ay);
                cam_.screen_to_world(ev.button.x, ev.button.y, bx, by);
                issue_formation(ax, ay, bx, by);
            } else {
                handle_right_down(ev.button.x, ev.button.y); // no drag -> normal order
            }
            rdrag_start_.reset();
            r_dragging_ = false;
        }
    } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_MIDDLE) {
        mid_dragging_ = true;
        mid_drag_last_ = {ev.button.x, ev.button.y};
    } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_MIDDLE) {
        mid_dragging_ = false;
    } else if (ev.type == SDL_MOUSEWHEEL) {
        double factor = ev.wheel.y > 0 ? 1.1 : (ev.wheel.y < 0 ? 1.0 / 1.1 : 1.0);
        // cam_.x/y is the world point at the viewport's top-left corner, so
        // changing zoom alone leaves THAT point fixed on screen -- zoom
        // visibly expands from the top-left. Re-centering on the world
        // point that was at screen-center before the zoom keeps that point
        // fixed instead, so zoom reads as centered on the middle of the
        // screen.
        double cx, cy;
        cam_.screen_to_world(view_w_ / 2, view_h_ / 2, cx, cy);
        cam_.zoom = std::clamp(cam_.zoom * factor, 0.4, 2.5);
        cam_.center_on(cx, cy);
    } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
        // AoE-style Escape: cancel an in-progress building placement or an
        // armed attack-move first, else deselect (does NOT quit -- that's
        // Alt+F4 now).
        if (!placing_.empty()) { placing_.clear(); wall_dragging_ = false; wall_drag_start_.reset(); }
        else if (attack_ground_armed_) attack_ground_armed_ = false;
        else if (attack_move_armed_) attack_move_armed_ = false;
        else if (!selected_.empty()) selected_.clear();
    } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F1) {
        // Fixed debug key: toggles the per-team resource readout in the
        // bottom-right scoreboard (draw_score_hud) -- for seeing what the AI is
        // hoarding/spending.
        show_res_debug_ = !show_res_debug_;
    } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F3) {
        // Fixed debug key, not part of the rebindable Settings scheme --
        // toggles the perf overlay (draw_perf_overlay) in any match, not
        // just a stress test.
        show_perf_overlay_ = !show_perf_overlay_;
    } else if (ev.type == SDL_KEYDOWN && !orders_locked()) {
        // Every hotkey here trains/builds/researches/arms an order mode --
        // blocked in spectator mode for the same reason right-click is
        // (see above). Escape/F3 above have their own branches and stay
        // available (Escape's normal "clear selection" effect is harmless
        // for a spectator; ending the preview itself is handled by app.cpp
        // watching for the same keypress independently of this function).
        handle_hotkey(ev.key.keysym.sym);
    }
}

static bool can_attack_ground(const ww::sim::Unit* u); // defined near issue_attack_ground

void GameClient::handle_hotkey(SDL_Keycode key) {
    // Every Settings binding is a Hotkey{key, ctrl} now -- the player can
    // put Ctrl on ANY rebindable slot (or take it off building_keys'
    // historically-always-Ctrl map-select), so matching one means checking
    // BOTH the base key and whether Ctrl is currently held, exactly --
    // a bare binding must NOT also fire while Ctrl happens to be down,
    // since that combo might be bound to something else entirely.
    bool ctrl_held = (SDL_GetModState() & KMOD_CTRL) != 0;
    // The delete-unit button (5th slot, top row -- see draw_command_card)
    // deliberately does NOT use its slot's QWERT hotkey (T); it's bound to
    // the Delete key instead, direct port of the original's dedicated
    // vk_delete check (assets/gmk/objects/control/Step.gml) rather than the
    // command-card's generic per-slot letter scheme. Fires unconditionally
    // on keypress (not gated on the button actually being drawn this frame,
    // same as the original's control.delete_command flag) -- delete_one_
    // selected_unit() itself is a no-op with nothing owned/alive selected.
    if (key == SDLK_DELETE) {
        delete_one_selected();
        return;
    }
    // Formation-shape hotkeys: Z = column, X = box, C = staggered, V = split.
    // Sets formation_type_ for the next right-drag / group move, identical to
    // the command-card formation buttons (see activate_card_button). Only while
    // a group (2+) is selected, so these keys stay free otherwise; fires
    // regardless of unit type so it works for artillery/air groups whose
    // formation button cell is claimed by land/attack-ground (see the command
    // card layout). Never with Ctrl held (that's reserved for map-select).
    if (!ctrl_held && selected_.size() >= 2) {
        const char* shape = nullptr;
        if (key == SDLK_z) shape = "column";
        else if (key == SDLK_x) shape = "box";
        else if (key == SDLK_c) shape = "staggered";
        else if (key == SDLK_v) shape = "split";
        if (shape) {
            formation_type_ = shape;
            return;
        }
    }
    if (key == settings_.idle_villager_key.key && ctrl_held == settings_.idle_villager_key.ctrl) {
        // Cycles one idle villager at a time (center camera + select, like
        // the building hotkeys below); Shift+<key> selects all of them at
        // once instead.
        if (SDL_GetModState() & KMOD_SHIFT) select_all_idle_villagers();
        else cycle_idle_villager();
        return;
    }
    Hotkey base_hk = settings_.building_key("base");
    if (key == base_hk.key) {
        // The base/TC key fires regardless of whether Ctrl happens to be
        // held -- it predates the general building map-select scheme just
        // below, so it's kept working without needing the modifier for
        // muscle memory. Shift+<key> (with or without Ctrl) selects every
        // base at once instead of cycling.
        if (SDL_GetModState() & KMOD_SHIFT) select_all_of_building_type("base");
        else cycle_to_building_type("base");
        return;
    }
    if (key >= SDLK_1 && key <= SDLK_9) {
        int group = static_cast<int>(key - SDLK_1) + 1;
        if (ctrl_held) assign_command_group(group);
        else select_command_group(group);
        return;
    }
    // Cycles through (centers camera on, and selects) the player's own
    // buildings of the mapped type; +Shift selects all of them at once.
    // Bindings come from settings_ (Options > Hotkeys), not a fixed table,
    // so a player's rebinding -- including dropping or adding Ctrl -- takes
    // effect immediately.
    for (auto& [name, bound] : settings_.building_keys) {
        if (bound.key != key || bound.ctrl != ctrl_held || name == "base") continue; // base already handled above
        if (SDL_GetModState() & KMOD_SHIFT) select_all_of_building_type(name);
        else cycle_to_building_type(name);
        return;
    }
    // T arms artillery attack-ground when an artillery piece is selected;
    // otherwise it falls through (T isn't otherwise bound to anything).
    if (key == SDLK_t) {
        for (auto ref : selected_) {
            Unit* u = match_.world().get(ref);
            if (u && u->common.team == 0 && u->common.alive && can_attack_ground(u)) {
                attack_ground_armed_ = true;
                return;
            }
        }
    }
    // While a villager's build-category list is open, this building's own
    // construction key (Villager Commands on the Hotkeys screen, settings_.
    // construction_keys -- a SEPARATE binding from building_keys' Ctrl+
    // <letter> map-select, defaulting to its slot in the eco/military build
    // list) constructs a NEW one of whichever building is currently offered
    // in the open list.
    if (!building_category_.empty()) {
        // Two passes, not one: settings_.construction_keys lists every eco
        // entry before every military one (default_construction_keys_grid),
        // so a single combined loop that fell back to the other page the
        // instant the FIRST same-key entry wasn't on the current page would
        // shadow a perfectly valid current-page building just because it
        // happened to be listed later -- e.g. Military page open, W bound
        // to both "farm" (eco) and "barracks" (military): farm's entry came
        // first, wasn't on the current page, and the old code jumped
        // straight to eco and built farm without ever looking at barracks.
        // Pass 1 checks every same-key entry against the CURRENTLY open
        // page first (also noting whether the key is bound to anything at
        // all, so a match here-but-unavailable still consumes the press
        // below instead of falling through to the item-hotkey handling),
        // so a valid current-page building always wins regardless of table
        // order.
        bool key_bound = false;
        for (auto& [name, bound] : settings_.construction_keys) {
            if (bound.key != key || bound.ctrl != ctrl_held) continue;
            key_bound = true;
            for (auto& btn : card_buttons_) {
                if (btn.kind == "build" && btn.item == name) { activate_card_button(btn); return; }
            }
        }
        if (key_bound) {
            // Pass 2: nothing on the current page matched -- if the same
            // key is bound to a building on the OTHER page instead (and
            // it's actually buildable right now), flip to that page and
            // start placing it in one press, rather than making the player
            // hit Next Page first and press the key again.
            for (auto& [name, bound] : settings_.construction_keys) {
                if (bound.key != key || bound.ctrl != ctrl_held) continue;
                const std::string other_category = building_category_ == "eco" ? "military" : "eco";
                const auto& other_allowed =
                    other_category == "eco" ? eco_buildings() : military_buildings();
                if (other_allowed.count(name)) {
                    auto avail = match_.control().available_buildings(0);
                    if (std::find(avail.begin(), avail.end(), name) != avail.end()) {
                        building_category_ = other_category;
                        activate_card_button({{}, "build", name, 0, 0});
                        return;
                    }
                }
            }
            return; // matched a construction key, just not one available anywhere right now
        }
    }
    // Dedicated, non-positional action keys (Options > Hotkeys) -- looked
    // up by CardButton::kind directly, independent of whatever grid slot
    // that kind's button happens to occupy. build_eco/build_military/
    // attack_move and the 4 formation keys vary by hotkey preset (Grid's
    // Q/W/A/S/D/F/G happen to already coincide with those buttons' natural
    // command-card slots; Classic uses AoE-style B/V/Q plus real AoE2's own
    // formation keys, plus a made-up one for Split); the other five carry
    // forward their old fixed slot as a sensible default regardless of
    // preset (see Settings::apply_preset).
    struct Dedicated { Hotkey key; const char* kind; };
    const Dedicated dedicated[] = {
        {settings_.build_eco_key, "build_eco"},         {settings_.build_military_key, "build_military"},
        {settings_.attack_move_key, "attack_move"},
        {settings_.formation_column_key, "formation_column"},
        {settings_.formation_box_key, "formation_box"},
        {settings_.formation_stagger_key, "formation_stagger"},
        {settings_.formation_split_key, "formation_split"},
        {settings_.land_key, "land"},                    {settings_.unload_key, "unload"},
        {settings_.shipyard_page_key, "shipyard_page"},  {settings_.build_nuke_key, "build_nuke"},
        {settings_.build_back_key, "build_back"},
    };
    for (auto& d : dedicated) {
        if (key != d.key.key || ctrl_held != d.key.ctrl) continue;
        for (auto& btn : card_buttons_) {
            if (btn.kind == d.kind) { activate_card_button(btn); return; }
        }
        // This candidate matches the key but isn't available right now --
        // shipyard_page/build_nuke/build_back default to the SAME key
        // (they only ever apply in mutually exclusive contexts: a shipyard
        // selected, an airbase selected, or a civilian's build list open),
        // so keep checking the rest of `dedicated` for one that IS active
        // instead of giving up on the first key match, which would let
        // whichever kind happens to be listed first permanently shadow
        // the others.
    }
    // Per-item train/tech/trade hotkeys (Options > Hotkeys) -- looked up by
    // the item's own catalog identity (btn.kind + btn.item), fixed
    // regardless of which slot it happens to render into this frame -- e.g.
    // "fishing boat" is always its own key at the shipyard regardless of
    // research order or which shipyard page is showing. This is the last
    // resolution step: every CardButton::kind is handled by exactly one of
    // the checks above (Delete/idle/base/Ctrl+letter/T/building mnemonic/
    // dedicated action keys) or this one -- there's no positional grid
    // fallback left (see Settings, which dropped grid_keys entirely).
    for (auto& btn : card_buttons_) {
        if (btn.kind != "train" && btn.kind != "tech" && btn.kind != "trade") continue;
        Hotkey hk = settings_.item_key(btn.kind, btn.item);
        if (hk.key != key || hk.ctrl != ctrl_held) continue;
        activate_card_button(btn);
        return;
    }
    // Not on the currently-shown shipyard page -- if it's a unit/tech
    // that's genuinely trainable/researchable right now but sitting on
    // the OTHER page, flip pages and fire it in one press, same "Next
    // Page" auto-switch idea as the villager build-category case above.
    if (Building* b = selected_.empty() ? nullptr : match_.world().get_building(selected_[0])) {
        if (b->name == "shipyard" && b->common.team == 0 && b->complete) {
            for (auto& name : match_.control().available_units("shipyard", 0)) {
                bool passive = ship_passive_units().count(name) > 0;
                if ((shipyard_page_ == 0) == passive) continue; // already showing on this page
                Hotkey hk = settings_.item_key("train", name);
                if (hk.key != key || hk.ctrl != ctrl_held) continue;
                shipyard_page_ = passive ? 0 : 1;
                activate_card_button({{}, "train", name, 0, 0});
                return;
            }
            for (auto& name : match_.control().available_techs("shipyard", 0)) {
                bool passive = ship_passive_techs().count(name) > 0;
                if ((shipyard_page_ == 0) == passive) continue;
                Hotkey hk = settings_.item_key("tech", name);
                if (hk.key != key || hk.ctrl != ctrl_held) continue;
                shipyard_page_ = passive ? 0 : 1;
                activate_card_button({{}, "tech", name, 0, 0});
                return;
            }
        }
    }
}

void GameClient::activate_card_button(const CardButton& btn) {
    World& world = match_.world();
    audio_.play("click", 0, 0, false); // UI select/click feedback for every command-card button
    if (btn.kind == "train" || btn.kind == "tech") {
        for (auto ref : selected_) {
            Building* b = world.get_building(ref);
            if (b && b->common.team == local_team_)
                issue(ww::sim::EnqueueCommand{b->common.id, btn.item});
        }
    } else if (btn.kind == "build") {
        placing_ = btn.item;
    } else if (btn.kind == "attack_move") {
        attack_move_armed_ = true;
    } else if (btn.kind == "attack_ground") {
        attack_ground_armed_ = true; // next right-click sets the bombard point
    } else if (btn.kind == "build_nuke") {
        // Build one atomic bomb into the selected airbase's stockpile (200 iron,
        // matching the tech cost). A landing heavy bomber/b29 auto-loads one.
        Building* ab = world.get_building(selected_.empty() ? kNullRef : selected_[0]);
        Team& t0 = match_.control().teams[0];
        double iron = t0.res.count("iron") ? t0.res["iron"] : 0.0;
        if (ab && ab->name == "airbase" && ab->common.team == 0 && ab->complete) {
            if (iron >= 200.0) {
                t0.res["iron"] -= 200.0;
                ab->nuke_count++;
                audio_.play("build", 0, 0, false);
            } else {
                world.events.push({EventType::Warn, "", 0, 0, 0, kNullRef, "You need more resources!"});
            }
        }
    } else if (btn.kind == "toggle_park_planes") {
        // Toggle whether newly-built planes park at this airbase (if a slot is
        // free) instead of taking off immediately (see Building::park_new_planes).
        Building* ab = world.get_building(selected_.empty() ? kNullRef : selected_[0]);
        if (ab && ab->name == "airbase" && ab->common.team == 0)
            ab->park_new_planes = !ab->park_new_planes;
    } else if (btn.kind == "pack" || btn.kind == "unpack") {
        // Ballistic missile: begin the pack/unpack transition on every selected
        // launcher currently settled in the opposite state. Packed = mobile
        // (can't fire); unpacked = deployed (can't move). Each toggle takes 5s.
        bool want_packed = (btn.kind == "pack");
        bool any = false;
        for (auto ref : selected_) {
            Unit* u = world.get(ref);
            if (u && u->common.alive && u->common.team == 0 && u->is_ballistic &&
                u->pack_t <= 0.0 && u->packed != want_packed) {
                u->pack_target = want_packed;
                u->pack_t = 5.0; // 5-second deploy/stow, matches the sim gate
                u->move_goal.reset();
                u->attack_target = kNullRef;
                u->attack_ground.reset();
                u->rally.reset();
                any = true;
            }
        }
        if (any) audio_.play("build", 0, 0, false);
    } else if (btn.kind == "replant") {
        // Toggle the team's auto-replant policy (see Team::replant). A team
        // policy is world state like any other, so it travels as a command.
        issue(ww::sim::TeamToggleCommand{local_team_, "replant"});
    } else if (btn.kind == "build_eco" || btn.kind == "build_military") {
        std::string cat = btn.kind == "build_eco" ? "eco" : "military";
        building_category_ = (building_category_ == cat) ? "" : cat; // click again to clear
    } else if (btn.kind == "trade") {
        // Market buy/sell -- btn.item is e.g. "buy food" / "sell iron". Routes
        // through Control::trade (oil is the currency); it warns + refuses if
        // the player can't afford it. Plays the market/trade sfx on success.
        auto sp = btn.item.find(' ');
        if (sp != std::string::npos) {
            std::string action = btn.item.substr(0, sp);
            std::string res = btn.item.substr(sp + 1);
            bool ok = match_.control().trade(action, res, 0, world);
            if (!ok) {
                world.events.push({EventType::Warn, "", 0, 0, 0, kNullRef, "You need more resources!"});
                audio_.play("error", 0, 0, false);
            }
        }
    } else if (btn.kind == "shipyard_page") {
        shipyard_page_ = shipyard_page_ == 0 ? 1 : 0; // flip passive <-> warships
    } else if (btn.kind == "market_page") {
        market_page_ = market_page_ == 0 ? 1 : 0; // flip trade table <-> techs
    } else if (btn.kind == "build_back") {
        // Alternates between the two build categories rather than
        // returning to the default menu -- lets the player flip between
        // eco/military building lists without needing to re-click the
        // (now-hidden) category button.
        building_category_ = (building_category_ == "eco") ? "military" : "eco";
    } else if (btn.kind == "delete") {
        delete_one_selected();
    } else if (btn.kind == "land") {
        // "Go to nearest available landing place": each selected aircraft flies
        // to the NEAREST friendly landing site that still has a free slot and
        // lands there to refuel/rearm -- one click, no base-picking. A landing
        // site is an AIRBASE (capacity 5) or an AIRCRAFT CARRIER (a mobile sea
        // airbase, capacity = its air_capacity -- see aircraft_behavior.cpp).
        World& world = match_.world();
        auto landed_at = [&](uint32_t site_id) {
            int n = 0;
            for (auto pref : world.active_units) {
                Unit* p = world.get(pref);
                if (p && p->common.alive && p->common.is_air && p->landed &&
                    p->home_id == site_id)
                    ++n;
            }
            return n;
        };
        bool any = false;
        for (auto sref : selected_) {
            Unit* u = world.get(sref);
            if (!u || u->common.team != 0 || !u->common.is_air || !u->common.alive) continue;
            uint32_t best_id = 0;
            double best_d = 1e30;
            bool found = false;
            for (auto bref : world.active_buildings) {
                Building* b = world.get_building(bref);
                if (!b || !b->common.alive || b->common.team != 0 || b->name != "airbase" ||
                    !b->complete)
                    continue;
                if (landed_at(b->common.id) >= 5) continue; // no free slot at this airbase
                double d = std::hypot(b->common.x - u->common.x, b->common.y - u->common.y);
                if (d < best_d) { best_d = d; best_id = b->common.id; found = true; }
            }
            for (auto cref : world.active_units) {
                Unit* c = world.get(cref);
                if (!c || !c->common.alive || c->common.team != 0 || !c->is_carrier) continue;
                if (landed_at(c->common.id) >= c->air_capacity) continue; // carrier deck full
                double d = std::hypot(c->common.x - u->common.x, c->common.y - u->common.y);
                if (d < best_d) { best_d = d; best_id = c->common.id; found = true; }
            }
            if (found) {
                u->land_order = true;
                u->stationed = true; // garrison there until re-ordered
                u->land_target_id = best_id;
                u->attack_target = kNullRef;
                u->rally.reset();
                u->forced = false;
                any = true;
            }
        }
        if (any) audio_.play("plane_move", 0, 0, false);
        land_armed_ = false;
        attack_move_armed_ = false;
        attack_ground_armed_ = false;
    } else if (btn.kind == "unload") {
        // Arm "pick a shoreline" mode: the next left-click on land next to the
        // selected transport disgorges its cargo there (World::unload_transport
        // validates the boat is coast-hugging and the point is shoreline).
        unload_armed_ = true;
        land_armed_ = false;
        attack_move_armed_ = false;
        attack_ground_armed_ = false;
    } else if (btn.kind == "formation_column") {
        formation_type_ = "column";
    } else if (btn.kind == "formation_box") {
        formation_type_ = "box";
    } else if (btn.kind == "formation_stagger") {
        formation_type_ = "staggered";
    } else if (btn.kind == "formation_split") {
        formation_type_ = "split";
    }
}

void GameClient::open_chat() {
    chat_open_ = true;
    paused_ = true;
    chat_input_.clear();
    SDL_StartTextInput();
}

void GameClient::submit_chat() {
    std::string msg = chat_input_;
    chat_open_ = false;
    paused_ = false;
    chat_input_.clear();
    SDL_StopTextInput();
    if (msg.empty()) return;

    std::string trimmed = msg;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
    std::string cmd = trimmed;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Debug/cheat commands -- consumed here rather than broadcast as a
    // literal chat line (classic RTS convention: typing a cheat code isn't
    // something the other players see). Both are team-0 (you) only, per
    // the request that these are single-player debugging aids, not
    // something to hand every AI team for free.
    Team& team0 = match_.control().teams[0];
    if (cmd == "blitz") {
        issue(ww::sim::TeamToggleCommand{local_team_, "blitz"});
        chat_log_.push_back(
            {std::string("[Cheat] Blitz mode ") + (team0.blitz ? "ON." : "OFF."), 0.0, 10.0});
        return;
    }
    if (cmd == "stockpile") {
        team0.res["food"] += 5000.0;
        team0.res["wood"] += 5000.0;
        team0.res["oil"] += 5000.0;
        team0.res["iron"] += 5000.0;
        chat_log_.push_back({"[Cheat] Stockpile: +5000 of all resources.", 0.0, 10.0});
        return;
    }
    // ---- "pov <n>" / "pov" -------------------------------------------------
    // Watch another team's economy live: their resources, population, era and
    // -- by selecting one of their buildings -- their production queues, with
    // the global queue in the corner listing everything they have in build.
    //
    // Purely a viewport change. The AI keeps running exactly as it was: nothing
    // is paused, no state is touched, and orders_locked() refuses every command
    // while it is active, so watching an AI cannot perturb the thing being
    // watched. "pov" with no argument (or your own team number) returns.
    if (cmd == "pov" || cmd.rfind("pov ", 0) == 0) {
        std::string arg = (cmd.size() > 4) ? cmd.substr(4) : std::string();
        while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
        int n = local_team_;
        if (!arg.empty() && arg != "me" && arg != "off") n = std::atoi(arg.c_str());
        int nplayers = match_.control().n;
        if (n < 0 || n >= nplayers) {
            chat_log_.push_back({"[Cheat] pov: team must be 0-" + std::to_string(nplayers - 1) +
                                     ". \"pov\" alone returns to your own view.",
                                 0.0, 10.0});
            return;
        }
        // Moves the HUD, the fog AND the camera -- see set_pov.
        set_pov(n);
        if (pov_active()) {
            const Team& t = match_.control().teams[n];
            std::string who = "Team " + std::to_string(n + 1);
            const auto& civs = match_.data().civs().at("civs");
            if (t.civ >= 0 && t.civ < static_cast<int>(civs.size()))
                who = civs[t.civ].value("name", who);
            chat_log_.push_back({"[Cheat] Watching " + who + " (team " + std::to_string(n + 1) +
                                     "). Orders are disabled. Type \"pov\" to go back.",
                                 0.0, 10.0});
        } else {
            chat_log_.push_back({"[Cheat] Back to your own view.", 0.0, 10.0});
        }
        return;
    }

    // Ordinary chat, broadcast to all other players -- there's no
    // multiplayer transport yet, so "everyone" is just you for now, but
    // the display already does what a real chat feed needs once one
    // exists.
    chat_log_.push_back({"You: " + msg, 0.0, 10.0});
}

void GameClient::delete_one_selected() {
    World& world = match_.world();
    // Peel off own units first (one per press, in selection order).
    for (size_t i = 0; i < selected_.size(); ++i) {
        Unit* u = world.get(selected_[i]);
        if (u && u->common.team == local_team_ && u->common.alive) {
            // The removal (and, for buildings below, the queue + pro-rata
            // refunds) now lives in apply_command, so a network match performs
            // the identical change -- including the identical resource totals --
            // on both machines. See the command sink.
            issue(ww::sim::DeleteCommand{{u->common.id}});
            selected_.erase(selected_.begin() + i);
            return;
        }
    }
    // Then own BUILDINGS (gmk: Delete removes any own building, houses
    // included). A finished building collapses into rubble via its normal
    // death FX; an unbuilt foundation is cancelled with just a dust poof
    // (deleted => no rubble/explosion -- see World's death sweep).
    for (size_t i = 0; i < selected_.size(); ++i) {
        Building* b = world.get_building(selected_[i]);
        if (b && b->common.team == local_team_ && b->common.alive) {
            issue(ww::sim::DeleteCommand{{b->common.id}});
            selected_.erase(selected_.begin() + i);
            return;
        }
    }
}

void GameClient::cycle_to_building_type(const std::string& name) {
    World& world = match_.world();
    std::vector<EntityRef> matches;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (b && b->common.alive && b->common.team == 0 && b->name == name) matches.push_back(ref);
    }
    if (matches.empty()) return;
    // Stable order across calls (spawn order), not insertion order into
    // active_buildings (which can shuffle as dead entries get swept out).
    std::sort(matches.begin(), matches.end(), [&](EntityRef a, EntityRef b) {
        return world.get_building(a)->common.id < world.get_building(b)->common.id;
    });
    size_t& i = building_cycle_i_[name];
    i %= matches.size();
    Building* b = world.get_building(matches[i]);
    cam_.center_on(b->common.x, b->common.y);
    selected_ = {matches[i]};
    play_selection_building_sound(selected_); // same identity cue a click gives
    i = (i + 1) % matches.size();
}

void GameClient::select_all_of_building_type(const std::string& name) {
    World& world = match_.world();
    std::vector<EntityRef> matches;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (b && b->common.alive && b->common.team == 0 && b->name == name) matches.push_back(ref);
    }
    if (!matches.empty()) {
        selected_ = std::move(matches);
        play_selection_building_sound(selected_); // all one type -> that type's clip
    }
}

std::set<std::string> GameClient::techs_in_progress(int team) {
    World& world = match_.world();
    const Control& ctrl = match_.control();
    std::set<std::string> out;
    for (auto ref : world.active_buildings) {
        const Building* b = world.get_building(ref);
        if (!b || !b->common.alive || b->common.team != team) continue;
        // A building's queue mixes units, age-ups and techs; only the techs
        // matter here. is_tech() is the same test World::enqueue used to decide
        // this item was research in the first place.
        for (const auto& item : b->queue)
            if (ctrl.is_tech(item)) out.insert(item);
    }
    return out;
}

bool GameClient::is_idle_civilian(const Unit& u) const {
    return u.name == "civilian" && u.common.team == 0 && u.common.alive && !u.gather_target.valid() &&
          !u.build_target.valid() && !u.repair_target.valid() && !u.drop_target.valid() &&
          !u.attack_target.valid() && !u.move_goal && !u.rally;
}

void GameClient::cycle_idle_villager() {
    World& world = match_.world();
    std::vector<EntityRef> idle;
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (u && is_idle_civilian(*u)) idle.push_back(ref);
    }
    if (idle.empty()) return;
    std::sort(idle.begin(), idle.end(), [&](EntityRef a, EntityRef b) {
        return world.get(a)->common.id < world.get(b)->common.id;
    });
    idle_cycle_i_ %= idle.size();
    Unit* u = world.get(idle[idle_cycle_i_]);
    cam_.center_on(u->common.x, u->common.y);
    selected_ = {idle[idle_cycle_i_]};
    idle_cycle_i_ = (idle_cycle_i_ + 1) % idle.size();
}

void GameClient::select_all_idle_villagers() {
    World& world = match_.world();
    std::vector<EntityRef> idle;
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (u && is_idle_civilian(*u)) idle.push_back(ref);
    }
    if (!idle.empty()) selected_ = std::move(idle);
}

void GameClient::assign_command_group(int group) {
    if (selected_.empty()) return;
    World& world = match_.world();
    std::vector<EntityRef> members;
    for (auto ref : selected_) {
        if (ref.kind != EntityKind::Unit && ref.kind != EntityKind::Building) continue;
        EntityCommon* c = world.common(ref);
        if (c && c->team == 0 && c->alive) members.push_back(ref);
    }
    // A unit can only ever belong to ONE group at a time, so reassigning it
    // here must also strip any stale membership in every OTHER group --
    // without this, a unit moved from group 2 into group 1 stayed listed in
    // BOTH, and since group_of() reports the lowest-numbered match, later
    // touching the other group could make it look like this reassignment
    // had silently failed, or that the unit's group number "jumped" on its
    // own once the stale entry surfaced again.
    for (int g = 1; g <= 9; ++g) {
        if (g == group) continue;
        auto& v = command_groups_[g];
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](EntityRef r) {
                                   return std::find(members.begin(), members.end(), r) != members.end();
                               }),
               v.end());
    }
    // Overwriting the target slot IS "clear the group, then reassign" --
    // whatever was in it before is simply gone from the vector.
    command_groups_[group] = std::move(members);
}

void GameClient::select_command_group(int group) {
    World& world = match_.world();
    auto& members = command_groups_[group];
    members.erase(std::remove_if(members.begin(), members.end(),
                                 [&](EntityRef r) {
                                     EntityCommon* c = world.common(r);
                                     return !c || !c->alive;
                                 }),
                 members.end());
    if (members.empty()) return;
    // A second press of the SAME digit shortly after the first centers the
    // camera on the group's first member instead of re-selecting it (the
    // original's own equivalent -- control/Step.gml's per-instance
    // view_xview snap inside a `with(obj_entity)` loop -- ended up centering
    // on whichever matching instance happened to iterate last, not a clean
    // "first member" rule).
    bool double_tap =
        last_group_key_ == group && match_.tick() - last_group_tick_ <= kGroupDoubleTapTicks;
    last_group_key_ = group;
    last_group_tick_ = match_.tick();
    if (double_tap) {
        if (EntityCommon* c = world.common(members.front())) cam_.center_on(c->x, c->y);
    } else {
        selected_ = members;
        // Same identity cue a single click on a building gives (see select_at),
        // extended to a whole group: one clip, for the building type the group
        // holds most of. Only on the branch that actually (re)selects -- the
        // double-tap above just moves the camera and stays quiet.
        play_selection_building_sound(selected_);
    }
}

void GameClient::play_selection_building_sound(const std::vector<EntityRef>& refs) {
    World& world = match_.world();
    // Tally by building NAME, not by ref: "most of" is a question about types.
    // Insertion order is kept alongside the counts so the winner among equal
    // counts is drawn from a stable, deterministic candidate list -- iterating
    // an unordered_map would make the tie-break depend on hash order rather
    // than on the coin flip below.
    std::vector<std::pair<std::string, int>> tally;
    for (auto ref : refs) {
        if (ref.kind != EntityKind::Building) continue;
        Building* b = world.get_building(ref);
        if (!b || b->common.team != 0 || !b->common.alive) continue;
        auto it = std::find_if(tally.begin(), tally.end(),
                               [&](const auto& e) { return e.first == b->name; });
        if (it == tally.end()) tally.push_back({b->name, 1});
        else ++it->second;
    }
    if (tally.empty()) return; // units-only group: nothing to announce
    int best = 0;
    for (const auto& [name, n] : tally) best = std::max(best, n);
    // Every type tied on the top count is an equally likely winner: two types
    // at 1 each is the 50/50 the design calls for, three at 1 each is 1/3, and
    // a clear majority collapses to a single candidate and always wins.
    std::vector<const std::string*> winners;
    for (const auto& [name, n] : tally) {
        if (n == best) winners.push_back(&name);
    }
    audio_.building_sound(*winners[std::rand() % winners.size()]);
}

int GameClient::group_of(EntityRef ref) const {
    for (int g = 1; g <= 9; ++g) {
        const auto& v = command_groups_[g];
        if (std::find(v.begin(), v.end(), ref) != v.end()) return g;
    }
    return 0;
}

// Looks up an item's real icon_sprite from the catalog (units/buildings/
// techs) instead of guessing "spr_<item>_icon" -- that guess happens to
// match most items' actual sprite name but not all (e.g. "uniform"'s
// catalog key doesn't match its real icon sprite, spr_leather_icon,
// leaving the button blank). Falls back to the old guessed name if the
// item isn't found in the catalog at all.
std::string GameClient::item_icon(const std::string& item) {
    World& world = match_.world();
    for (const char* section : {"units", "buildings", "techs"}) {
        auto& sec = world.data.catalog().at(section);
        if (sec.contains(item)) {
            std::string ic = sec.at(item).value("icon_sprite", "");
            if (!ic.empty()) return ic;
            break;
        }
    }
    std::string fallback = "spr_" + item + "_icon";
    for (auto& ch : fallback) if (ch == ' ') ch = '_';
    return fallback;
}

// ---- global production queue -------------------------------------------
// Everything the viewed team currently has in production, gathered from every
// building into one strip under the resource bar. Answers "what am I actually
// making right now" without clicking through buildings one at a time -- and,
// with the pov cheat, does the same for an AI.
//
// Progress is drawn as a light grey wipe descending over the icon: empty at 0%,
// covering the whole icon at 100%, at which point the item completes and leaves
// the queue on its own. Only the FRONT item of each building is being worked
// on (Building::percent tracks exactly that one); the rest are queued behind it
// and correctly read as 0.
void GameClient::draw_global_queue(SDL_Renderer* renderer) {
    global_queue_h_ = 0;
    World& world = match_.world();
    const int team = view_team();

    struct Entry {
        std::string item;
        double pct;
    };
    std::vector<Entry> entries;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive || !b->complete) continue;
        if (b->common.team != team || b->queue.empty()) continue;
        for (size_t i = 0; i < b->queue.size(); ++i)
            entries.push_back({b->queue[i], i == 0 ? b->percent : 0.0});
    }

    const int icon = ui(28), gap = ui(3);
    const int x0 = ui(4), y0 = top_bar_height() + ui(4);

    // The POV banner rides along with this strip: it is the one piece of HUD
    // that has to be impossible to miss, because every number on screen belongs
    // to somebody else while it is up.
    int y = y0;
    if (pov_active()) {
        const Team& t = match_.control().teams[team];
        std::string who = "TEAM " + std::to_string(team + 1);
        const auto& civs = match_.data().civs().at("civs");
        if (t.civ >= 0 && t.civ < static_cast<int>(civs.size())) {
            std::string n = civs[t.civ].value("name", who);
            for (auto& ch : n) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            who = n;
        }
        std::string line = "WATCHING " + who + "  --  type \"pov\" to return";
        int tw, th;
        text_.measure(line, ui(12), tw, th);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 120, 20, 20, 200);
        SDL_Rect box{x0, y, tw + ui(10), th + ui(4)};
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 235, 90, 90, 255);
        SDL_RenderDrawRect(renderer, &box);
        text_.draw(line, x0 + ui(5), y + ui(2), {255, 235, 235, 255}, ui(12));
        y += box.h + ui(3);
        global_queue_h_ = y - y0;
    }
    if (entries.empty()) return;

    // Bounded so a late-game economy with a dozen production buildings cannot
    // run the strip off the side of the screen; the overflow is counted instead.
    const size_t kMax = 14;
    size_t shown = std::min(entries.size(), kMax);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (size_t i = 0; i < shown; ++i) {
        SDL_Rect cell{x0 + static_cast<int>(i) * (icon + gap), y, icon, icon};
        atlas_.draw_stretched("spr_button", cell);
        int pad = ui(3);
        SDL_Rect ir{cell.x + pad, cell.y + pad, cell.w - 2 * pad, cell.h - 2 * pad};
        atlas_.draw_in_rect(ir, item_icon(entries[i].item), item_icon_frame(entries[i].item),
                            /*pad=*/0);
        double pct = std::clamp(entries[i].pct, 0.0, 100.0);
        if (pct > 0.0) {
            int h = static_cast<int>(ir.h * pct / 100.0 + 0.5);
            if (h > 0) {
                SDL_SetRenderDrawColor(renderer, 210, 210, 210, 150);
                SDL_Rect wipe{ir.x, ir.y, ir.w, h};
                SDL_RenderFillRect(renderer, &wipe);
            }
        }
    }
    if (entries.size() > shown) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "+%d", static_cast<int>(entries.size() - shown));
        text_.draw(buf, x0 + static_cast<int>(shown) * (icon + gap) + ui(2), y + icon / 2 - ui(6),
                   {255, 255, 255, 255}, ui(12));
    }
    global_queue_h_ = (y + icon) - y0;
}

// Move the entire viewpoint to `team`: the HUD (view_team()), the fog, and the
// camera. Passing local_team_ returns to your own view and restores everything
// exactly as it was.
//
// The fog is SAVED and RESTORED rather than just repointed. The grid is shared
// and "explored" is sticky, so simply handing it to the AI and taking it back
// would leave every cell the AI had seen permanently marked explored on your
// map -- terrain you never scouted, revealed for the rest of the match. That is
// a gameplay change, not a debug view, so your grid is copied out on the way in
// and copied back on the way out.
void GameClient::set_pov(int team) {
    World& world = match_.world();
    bool going_away = (team != local_team_);

    if (going_away && !pov_has_backup_) {
        // Leaving your own view for the first time -- stash what is yours.
        pov_fog_backup_ = world.fog;
        pov_cam_x_ = cam_.x;
        pov_cam_y_ = cam_.y;
        pov_has_backup_ = true;
    }

    pov_team_ = going_away ? team : -1;
    selected_.clear(); // a selection made in someone else's view means nothing here

    if (!going_away && pov_has_backup_) {
        world.fog = pov_fog_backup_; // your map, exactly as you left it
        cam_.x = pov_cam_x_;
        cam_.y = pov_cam_y_;
        pov_has_backup_ = false;
        pov_fog_backup_.clear();
        pov_fog_backup_.shrink_to_fit();
    }

    // Rebuild fog for whoever we are now watching, immediately rather than on
    // the next tick, so the switch is visible on this very frame.
    world.fog_player = view_team();
    world.update_fog(world.fog_player);

    if (going_away) {
        // Put the camera where that team lives: its town centre, else any
        // building it owns, else any unit. A team with nothing left keeps the
        // camera where it was rather than jumping to the origin.
        double hx = 0.0, hy = 0.0;
        bool found = false;
        for (auto ref : world.active_buildings) {
            Building* b = world.get_building(ref);
            if (!b || !b->common.alive || b->common.team != team) continue;
            hx = b->common.x;
            hy = b->common.y;
            found = true;
            if (b->name == "base") break; // prefer the town centre
        }
        if (!found) {
            for (auto ref : world.active_units) {
                Unit* u = world.get(ref);
                if (!u || !u->common.alive || u->common.team != team) continue;
                hx = u->common.x;
                hy = u->common.y;
                found = true;
                break;
            }
        }
        if (found) cam_.center_on(hx, hy);
    }
    cam_.clamp();
}

int GameClient::item_icon_frame(const std::string& item) {
    // See the header for why techs are pinned to frame 0.
    if (match_.control().is_tech(item)) return 0;
    return match_.control().teams[view_team()].colour;
}

std::vector<std::pair<double, double>> GameClient::wall_line(double wx0, double wy0, double wx1,
                                                            double wy1) {
    World& world = match_.world();
    // Snap both endpoints to the wall's own 32x32 tile grid, then work in
    // integer cell indices. snap() returns the segment CENTRE (tile*32 + 16
    // for a one-tile building), so recovering the cell index is (centre-16)/32.
    auto [ax, ay] = world.snap(placing_, wx0, wy0);
    auto [bx, by] = world.snap(placing_, wx1, wy1);
    int cx0 = static_cast<int>(std::lround((ax - 16.0) / TILE));
    int cy0 = static_cast<int>(std::lround((ay - 16.0) / TILE));
    int cx1 = static_cast<int>(std::lround((bx - 16.0) / TILE));
    int cy1 = static_cast<int>(std::lround((by - 16.0) / TILE));
    // Rasterize the cell path by linear interpolation with rounding: with
    // steps == the longer axis span, consecutive cells differ by at most 1 in
    // each axis, so the run is fully connected. It comes out as a pure
    // orthogonal line when one axis is fixed, a clean 45-degree diagonal when
    // the spans are equal, and a staircase of the two in between -- covering
    // both the orthogonal and diagonal drags the wall tool is meant to build.
    int span = std::max(std::abs(cx1 - cx0), std::abs(cy1 - cy0));
    std::vector<std::pair<double, double>> out;
    int prev_cx = 0, prev_cy = 0;
    bool have_prev = false;
    for (int i = 0; i <= span; ++i) {
        double t = span == 0 ? 0.0 : static_cast<double>(i) / span;
        int cx = cx0 + static_cast<int>(std::lround((cx1 - cx0) * t));
        int cy = cy0 + static_cast<int>(std::lround((cy1 - cy0) * t));
        if (have_prev && cx == prev_cx && cy == prev_cy) continue; // rounding can repeat a cell
        prev_cx = cx;
        prev_cy = cy;
        have_prev = true;
        out.emplace_back(cx * double(TILE) + 16.0, cy * double(TILE) + 16.0);
    }
    return out;
}

void GameClient::jump_minimap_to(int mx, int my) {
    // Clamped so dragging past the minimap's edge (button still held)
    // pins the camera at that edge of the map instead of extrapolating to
    // a nonsensical world position outside [0, px_w/px_h].
    int cx = std::clamp(mx, minimap_rect_.x, minimap_rect_.x + minimap_rect_.w);
    int cy = std::clamp(my, minimap_rect_.y, minimap_rect_.y + minimap_rect_.h);
    World& world = match_.world();
    double wx = (cx - minimap_rect_.x) * world.px_w / static_cast<double>(minimap_rect_.w);
    double wy = (cy - minimap_rect_.y) * world.px_h / static_cast<double>(minimap_rect_.h);
    cam_.center_on(wx, wy);
}

void GameClient::handle_left_down(int mx, int my) {
    SDL_Point mp0{mx, my};
    if (SDL_PointInRect(&mp0, &pause_button_rect_)) {
        pause_menu_open_ = true;
        paused_ = true;
        return;
    }
    if (my > view_h_ - panel_height()) { // command-card / HUD panel area consumes the click
        SDL_Point mp{mx, my};
        if (SDL_PointInRect(&mp, &minimap_rect_)) {
            minimap_dragging_ = true;
            jump_minimap_to(mx, my);
            return;
        }
        // Spectator mode (Start Stress Test preview): card_buttons_/queue_
        // buttons_ are still populated and drawn normally (selection works,
        // so the command card shows whatever the selected unit/building
        // would show a real player) but MUST NOT be activatable -- every
        // one of them trains/builds/researches/cancels production, which a
        // pure spectator shouldn't be able to do (see set_spectator()'s
        // comment). The minimap-drag case above is unaffected -- it's just
        // camera movement, not a command.
        if (!orders_locked()) {
            // Idle-villager button: same action as idle_villager_key's
            // plain (non-Shift) press -- cycle_idle_villager() centers the
            // camera on and selects the next idle civilian. Gated on
            // spectator_ same as everything else in this block -- it's a
            // "select my own villagers" shortcut, and nothing is "mine" in
            // a pure spectator preview (see set_spectator()'s comment).
            if (SDL_PointInRect(&mp0, &idle_button_rect_)) {
                cycle_idle_villager();
                return;
            }
            for (auto& btn : card_buttons_) {
                SDL_Point p{mx, my};
                if (SDL_PointInRect(&p, &btn.rect)) {
                    activate_card_button(btn);
                    return;
                }
            }
            for (auto& qbtn : queue_buttons_) {
                SDL_Point p{mx, my};
                if (SDL_PointInRect(&p, &qbtn.rect)) {
                    if (ww::sim::EntityCommon* qc = match_.world().common(qbtn.building))
                        issue(ww::sim::CancelQueueCommand{qc->id, qbtn.index});
                    return;
                }
            }
        }
        return;
    }
    double wx, wy;
    cam_.screen_to_world(mx, my, wx, wy);
    if (!placing_.empty()) {
        // Walls support AoE-style click-and-drag: the press only ARMS the
        // drag here; handle_left_up() lays the whole line of segments (or a
        // single one, for a press with no drag). See wall_line() / the drag
        // ghost preview in draw().
        if (is_wall(placing_)) {
            wall_drag_start_ = SDL_Point{mx, my};
            wall_dragging_ = true;
            return;
        }
        bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        // Pass the actual selection as the construction crew -- this used
        // to always auto-pick whichever idle civilian happened to be
        // nearest, ignoring selection entirely and only ever assigning
        // one builder even when several villagers were selected.
        //
        // Shift = AoE-style shift-queue: place the foundation but DON'T pull
        // the selected villagers off what they're doing. Instead append a
        // Build order to each one's order_queue (assign_builders=false stops
        // place_building committing a crew now), so they finish their current
        // tasks and build this in turn. Shift also keeps placement mode armed
        // so you can drop a whole chain of queued buildings in one go.
        // place_building() no-ops (refunds nothing, spawns nothing) on an
        // unaffordable or blocked spot, so a shift-click on an invalid tile
        // just does nothing rather than erroring.
        // Placement AND the shift-queued build order travel as ONE command:
        // the order refers to the foundation this placement creates, and under
        // lockstep that foundation does not exist yet when the command is
        // issued. See PlaceBuildingCommand::queue_build.
        ww::sim::PlaceBuildingCommand pc;
        pc.team = local_team_;
        pc.name = placing_;
        pc.x = wx;
        pc.y = wy;
        pc.builder_ids = selected_ids();
        pc.assign_builders = !shift;
        pc.queue_build = shift;
        issue(pc);
        if (!shift) placing_.clear();
        return;
    }
    // Attack-ground: while armed (T / the button), a LEFT-click sets the
    // bombard point (right-click cancels -- see handle_right_down).
    if (attack_ground_armed_) {
        issue_attack_ground(wx, wy);
        return;
    }
    // Return-to-base: while armed (Land button), a LEFT-click on one of your
    // own airbases sends the selected aircraft there to land. A click that
    // misses every airbase is swallowed but keeps the mode armed (right-click
    // cancels -- see handle_right_down), so a stray click never loses the mode.
    if (land_armed_) {
        World& world = match_.world();
        // A landing site under the click: your own airbase OR aircraft carrier
        // (both refuel/rearm parked planes -- see aircraft_behavior.cpp's
        // find_landing_site, which matches this id across buildings AND units).
        std::optional<uint32_t> site_id;
        for (auto ref : world.active_buildings) {
            Building* b = world.get_building(ref);
            if (!b || !b->common.alive || b->common.team != 0 || b->name != "airbase" || !b->complete) {
                continue;
            }
            if (std::abs(wx - b->common.x) <= b->foot_w * 0.5 + 8 &&
                std::abs(wy - b->common.y) <= b->foot_h * 0.5 + 8) {
                site_id = b->common.id;
                break;
            }
        }
        if (!site_id) {
            for (auto ref : world.active_units) {
                Unit* c = world.get(ref);
                if (!c || !c->common.alive || c->common.team != 0 || !c->is_carrier) continue;
                if (std::abs(wx - c->common.x) <= 90.0 && std::abs(wy - c->common.y) <= 60.0) {
                    site_id = c->common.id;
                    break;
                }
            }
        }
        if (site_id) {
            bool any = false;
            for (auto sref : selected_) {
                Unit* u = world.get(sref);
                if (u && u->common.team == 0 && u->common.is_air && u->common.alive) {
                    u->land_order = true;
                    u->stationed = true; // garrison here until a new order (see Unit::stationed)
                    u->land_target_id = *site_id;
                    u->attack_target = kNullRef;
                    u->rally.reset();
                    u->forced = false;
                    any = true;
                }
            }
            if (any) audio_.play("plane_move", 0, 0, false);
            land_armed_ = false;
        }
        return;
    }
    // Unload: while armed (transport Unload button), a LEFT-click on a
    // shoreline tile next to the selected transport disgorges its cargo.
    // World::unload_transport rejects invalid spots (boat not coast-hugging,
    // point not shoreline) -- a rejected click keeps the mode armed and warns.
    if (unload_armed_) {
        World& world = match_.world();
        EntityRef unloaded = kNullRef;
        for (auto sref : selected_) {
            Unit* u = world.get(sref);
            if (!u || u->common.team != 0 || u->transport_cap <= 0 || u->cargo.empty()) continue;
            if (world.unload_transport(sref, wx, wy)) {
                unloaded = sref;
                break; // one transport unloads per click
            }
        }
        if (unloaded.valid()) {
            // A click now lands only a squad (~10), not the whole hold, so keep
            // the Unload mode armed while troops remain aboard -- the player can
            // keep clicking the beach to land successive waves without re-pressing
            // the Unload key. Disarm once the transport is empty.
            Unit* su = world.get(unloaded);
            unload_armed_ = (su && !su->cargo.empty());
            audio_.play("click", wx, wy, true);
            effects_.push_back({"spr_arrow", wx, wy, 0.0, 1.0, 0});
        } else {
            world.events.push({EventType::Warn, "", 0, 0, 0, kNullRef,
                               "Move the ship beside the shore to unload!"});
        }
        return;
    }
    // Attack-move fires on either mouse button while armed, and must NOT
    // fall through to the drag-select below -- otherwise the same click
    // that issues the order would also start a marquee-select box. Once
    // this consumes the click, attack_move_armed_ is cleared, so the next
    // left-click behaves as a normal click/drag again.
    if (attack_move_armed_) {
        issue_attack_move(wx, wy);
        return;
    }
    drag_start_ = SDL_Point{mx, my};
    dragging_ = true;
}

void GameClient::handle_left_up(int mx, int my) {
    minimap_dragging_ = false;
    // Wall drag release: lay every segment along the drawn line. Each segment
    // is an independent place_building() call, so it pays per segment and
    // simply stops spawning once the player runs out of resources (or hits a
    // blocked/unexplored cell) -- the same no-op-on-invalid behaviour a
    // single placement already has. A press with no real drag collapses to a
    // one-cell line, i.e. a single segment at the cursor.
    if (wall_dragging_ && wall_drag_start_) {
        wall_dragging_ = false;
        double wx0, wy0, wx1, wy1;
        cam_.screen_to_world(wall_drag_start_->x, wall_drag_start_->y, wx0, wy0);
        cam_.screen_to_world(mx, my, wx1, wy1);
        World& world = match_.world();
        std::string wall = placing_;
        std::vector<uint32_t> crew = selected_ids();
        for (auto [sx, sy] : wall_line(wx0, wy0, wx1, wy1)) {
            // One command per segment, matching the one-place_building-per-
            // segment behaviour this always had (each segment is paid for
            // separately, and an unaffordable one simply stops the run).
            ww::sim::PlaceBuildingCommand pc;
            pc.team = local_team_;
            pc.name = wall;
            pc.x = sx;
            pc.y = sy;
            pc.builder_ids = crew;
            issue(pc);
        }
        // Point each selected villager at the segment nearest IT; from there
        // the sim auto-chains it along the rest of the wall (World::
        // next_wall_segment), so the whole drawn line builds from one drag with
        // the crew spread out instead of every segment needing its own order.
        double R = std::hypot(world.px_w, world.px_h);
        for (auto sref : selected_) {
            Unit* u = world.get(sref);
            if (!u || u->common.team != 0 || !u->is_gatherer) continue;
            EntityRef seg = world.next_wall_segment(0, wall, u->common.x, u->common.y, R, kNullRef);
            if (!seg.valid()) continue;
            u->build_target = seg;
            u->gather_target = kNullRef;
            u->move_goal.reset();
            u->path.clear();
            u->approach_prev_pos.reset();
            u->approach_target.reset();
            u->approach_progress_check_t = 0.0;
        }
        wall_drag_start_.reset();
        // Same shift-to-keep-placing convention as a normal building drop.
        if (!(SDL_GetModState() & KMOD_SHIFT)) placing_.clear();
        return;
    }
    if (!dragging_ || !drag_start_) { dragging_ = false; return; }
    dragging_ = false;
    int dx = mx - drag_start_->x, dy = my - drag_start_->y;
    bool ctrl_held = (SDL_GetModState() & KMOD_CTRL) != 0;
    if (dx * dx + dy * dy > 36) {
        double wx0, wy0, wx1, wy1;
        cam_.screen_to_world(drag_start_->x, drag_start_->y, wx0, wy0);
        cam_.screen_to_world(mx, my, wx1, wy1);
        box_select(std::min(wx0, wx1), std::min(wy0, wy1), std::max(wx0, wx1), std::max(wy0, wy1));
    } else {
        double wx, wy;
        cam_.screen_to_world(mx, my, wx, wy);
        select_at(wx, wy, ctrl_held, double_click_);
    }
    // Selection acknowledgement (session.py's LMB-up handler). Buildings/enemy/
    // resource acks already fired inside select_at, so this only covers an
    // own-unit selection.
    play_select_ack();
    drag_start_.reset();
    double_click_ = false;
}

void GameClient::play_select_ack() {
    if (selected_.empty()) return;
    Unit* u = match_.world().get(selected_[0]); // null for a building ref
    if (!u || u->common.team != 0) return;
    const std::string& n = u->name;
    if (n == "camel" || n == "camel corps") audio_.play("camel", 0, 0, false);
    else if (n == "cavalry" || n == "cavalry2" || n == "cavalry3") audio_.play("cavalry_move", 0, 0, false);
    else if (n == "artillery1" || n == "artillery" || n == "heavy artillery") audio_.play("artillery_move", 0, 0, false); // snd_tank_move2
    else if (n == "yamato") audio_.play("ship_horn", 0, 0, false);
    else if (u->common.is_ship) audio_.play("ship_select", 0, 0, false);
    else if (u->common.is_air) { /* airbase units: no voice line (engine plays in flight) */ }
    else if (u->mechanical) audio_.play("tank", 0, 0, false); // factory vehicles: engine, not a voice
    else audio_.voice("me", match_.control().teams[0].civ); // military + villager infantry
}

void GameClient::play_order_ack(bool attack) {
    if (selected_.empty()) return;
    Unit* u = match_.world().get(selected_[0]);
    if (!u || u->common.team != 0) return;
    int civ = match_.control().teams[0].civ;
    const std::string& n = u->name;
    if (n == "camel" || n == "camel corps") audio_.play("camel", 0, 0, false);
    else if (n == "cavalry" || n == "cavalry2" || n == "cavalry3") audio_.play("cavalry_move", 0, 0, false);
    else if (n == "artillery1" || n == "artillery" || n == "heavy artillery") audio_.play("artillery_move", 0, 0, false); // snd_tank_move2
    else if (u->common.is_air) { /* plane_move engine handled by the caller */ }
    else if (u->common.is_ship) { /* ships: no order voice */ }
    else if (u->mechanical) audio_.play("tank", 0, 0, false); // ground vehicles: engine, not a voice
    else if (attack) { if (!audio_.voice("attack", civ)) audio_.voice("me", civ); }
    else audio_.voice("me", civ); // military + villager infantry
}

void GameClient::handle_right_down(int mx, int my) {
    double wx, wy;
    SDL_Point mp{mx, my};
    if (SDL_PointInRect(&mp, &minimap_rect_)) {
        // A minimap click's world point comes from the minimap's own pixel
        // mapping (same one jump_minimap_to's camera-jump uses), NOT
        // cam_.screen_to_world -- mx/my here are minimap-panel screen
        // coordinates, unrelated to the main viewport's camera transform,
        // so screen_to_world would resolve them to a essentially arbitrary
        // world point instead of the spot the player actually clicked.
        int cx = std::clamp(mx, minimap_rect_.x, minimap_rect_.x + minimap_rect_.w);
        int cy = std::clamp(my, minimap_rect_.y, minimap_rect_.y + minimap_rect_.h);
        World& world = match_.world();
        wx = (cx - minimap_rect_.x) * world.px_w / static_cast<double>(minimap_rect_.w);
        wy = (cy - minimap_rect_.y) * world.px_h / static_cast<double>(minimap_rect_.h);
    } else {
        cam_.screen_to_world(mx, my, wx, wy);
    }
    if (!placing_.empty()) { placing_.clear(); return; }
    if (attack_ground_armed_) { attack_ground_armed_ = false; return; } // right-click cancels aiming
    if (land_armed_) { land_armed_ = false; return; } // right-click cancels airbase picking
    if (unload_armed_) { unload_armed_ = false; return; } // right-click cancels unload aiming
    if (attack_move_armed_) {
        issue_attack_move(wx, wy);
        return;
    }
    bool shift_held = (SDL_GetModState() & KMOD_SHIFT) != 0;
    right_click_order(wx, wy, shift_held);
}

// Artillery, bombers and the ohka can be given a standing attack-ground order.
static bool can_attack_ground(const ww::sim::Unit* u) {
    return u && (u->name == "artillery" || u->name == "artillery1" || u->name == "heavy artillery" ||
                 u->is_bomber || u->name == "ohka" || u->is_ballistic);
}

void GameClient::issue_attack_ground(double wx, double wy) {
    World& world = match_.world();
    bool any = false;
    for (auto ref : selected_) {
        Unit* u = world.get(ref);
        if (u && u->common.team == local_team_ && u->common.alive && can_attack_ground(u)) {
            // The state change itself now lives in apply_command (see the
            // command sink) so both machines in a network match perform it
            // identically. One command per unit rather than a batch, because
            // the eligibility test above filters the selection.
            issue(ww::sim::AttackGroundCommand{{u->common.id}, wx, wy});
            any = true;
        }
    }
    // No cannon SFX on PLACING the target -- the cannon "shoot" sound now plays
    // only when the artillery actually fires its shell (see unit_behavior.cpp).
    attack_ground_armed_ = false;
}

void GameClient::issue_attack_move(double wx, double wy) {
    // AoE-style attack-move: sets a rally point each selected own unit
    // marches toward, engaging any enemy it sights along the way
    // (Unit::rally is already consumed that way in unit_behavior.cpp).
    World& world = match_.world();
    bool moved_aircraft = false, any_ordered = false;
    for (auto ref : selected_) {
        Unit* u = world.get(ref);
        if (!u || u->common.team != local_team_) continue;
        // See the command sink: the mutation lives in apply_command now, so a
        // network match performs the identical change on both machines.
        issue(ww::sim::RallyCommand{{u->common.id}, wx, wy});
        if (u->common.is_air) moved_aircraft = true;
        any_ordered = true;
    }
    if (moved_aircraft) audio_.play("plane_move", wx, wy, true); // see right_click_order's comment
    // Attack-move ack: infantry shout, vehicles rev, planes/ships quiet
    // (see play_order_ack). Local player only (team 0).
    if (any_ordered) play_order_ack(/*attack=*/true);
    attack_move_armed_ = false;
}

const std::string& GameClient::animated_sprite(const Unit& u) const {
    if (u.is_ballistic) {
        // Packed/stowed launcher (also shown mid pack-or-unpack transition);
        // deployed shows the missile on the rail when a shot is chambered
        // (reload done) and the empty rail while it reloads. All four sprites
        // share the same 112x112 frame + origin, so the meta-based sizing done
        // off u.sprite stays correct regardless of which one is returned.
        static const std::string kPacked = "spr_ballistic_missile_packed";
        static const std::string kLoaded = "spr_ballistic_missile_unpacked_loaded";
        static const std::string kUnloaded = "spr_ballistic_missile_unpacked_unloaded";
        if (u.packed || u.pack_t > 0.0) return kPacked;
        return u.reload_timer > 0.0 ? kUnloaded : kLoaded;
    }
    if (u.working) {
        if (u.name == "civilian") {
            static const std::string kWorking1 = "spr_civilian_working1", kWorking2 = "spr_civilian_working2";
            return u.swing_down ? kWorking2 : kWorking1;
        }
        if (u.name == "fishing boat") {
            static const std::string kFishWorking = "spr_fishing_boat_working";
            return kFishWorking;
        }
    }
    return u.sprite;
}

void GameClient::select_at(double wx, double wy, bool add_to_selection, bool double_click) {
    building_category_.clear();
    World& world = match_.world();
    EntityRef best = kNullRef;
    // Not 40*40 -- that would silently cap buildings the same way the old
    // circular building hit-test used to (see below): with `best_d` seeded
    // at 1600, any candidate more than 40px from its own center could never
    // win even if its own radius/rectangle comfortably covered the click.
    // Units/resources are unaffected either way since their own r*r (see
    // `consider`) is always <= this anyway.
    double best_d = std::numeric_limits<double>::max();

    auto consider = [&](EntityRef ref, double x, double y, double radius, bool exact = false) {
        double d = dist2(wx, wy, x, y);
        double r = exact ? radius : std::max(radius, 40.0);
        if (d < r * r && d < best_d) { best_d = d; best = ref; }
    };
    for (auto ref : world.active_units) {
        if (Unit* u = world.get(ref)) {
            // Air units are DRAWN lifted by their `height` (see the draw offset
            // `sy -= u->height * zoom`), so hit-test where the sprite ACTUALLY
            // appears -- not the ground point ~64px below it, which made flying
            // planes feel like they had no hitbox. Aircraft also get a bigger
            // pick radius (their sprites are small and move fast).
            double hy = u->common.y - (u->common.is_air ? u->height : 0.0);
            if (u->common.is_air && u->landed) {
                // A plane PARKED on a carrier (or airbase) is a small deck icon
                // sitting on top of the big ship -- give it a tight, EXACT hitbox
                // so you have to click the icon itself; otherwise the click falls
                // through to the carrier beneath it (double-click still grabs all
                // planes of that type). No 40px floor, else it would swallow the
                // whole deck and you could never select the ship.
                consider(ref, u->common.x, hy, 14.0, /*exact=*/true);
            } else if (u->is_carrier) {
                // Big ship: make its whole deck clickable so it wins over the
                // empty deck between parked planes.
                consider(ref, u->common.x, hy, 84.0);
            } else {
                consider(ref, u->common.x, hy, u->common.is_air ? 50.0 : 20.0);
            }
        }
    }
    // Buildings hit-test against their TRUE rectangular footprint, not a
    // circle -- a circle sized off foot_px (== max(foot_w, foot_h), see
    // World::place_building) either misses the corners of any non-square
    // building or, along its shorter axis, extends past the real edge.
    // Clicking anywhere inside the building's actual rectangle should hit
    // it; center-distance only breaks ties against overlapping units/
    // resources/buildings, same as the circular candidates above.
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b) continue;
        double hw = std::max(b->foot_w * 0.5, 20.0), hh = std::max(b->foot_h * 0.5, 20.0);
        // Same bottom-anchored footprint the sim itself collides against
        // (see World::footprint_dy) -- tower/aa tower/outpost's footprint
        // is only their base tile, not centered on common.y like every
        // other building's.
        double fcy = b->common.y + footprint_dy(b->name);
        if (wx < b->common.x - hw || wx > b->common.x + hw || wy < fcy - hh || wy > fcy + hh) {
            continue;
        }
        double d = dist2(wx, wy, b->common.x, fcy);
        if (d < best_d) { best_d = d; best = ref; }
    }
    for (auto ref : world.active_resources) {
        if (Resource* r = world.get_resource(ref)) consider(ref, r->common.x, r->common.y, 20);
    }

    if (!best.valid()) {
        if (!add_to_selection) selected_.clear();
        return;
    }
    // AoE-style double-click: double-clicking one of your own units selects
    // every unit of that same type currently on-screen, instead of just the
    // one clicked. Doesn't apply to buildings/resources or add_to_selection
    // (ctrl-click), which have their own single-target semantics.
    if (double_click && !add_to_selection) {
        if (Unit* u = world.get(best); u && u->common.team == 0) {
            select_all_of_type_in_viewport(u->name, 0);
            return;
        }
    }
    if (add_to_selection) {
        auto it = std::find(selected_.begin(), selected_.end(), best);
        if (it != selected_.end()) selected_.erase(it);
        else selected_.push_back(best);
    } else {
        // Only a click that actually CHANGES the selection to this building
        // (it wasn't already the one selected) plays its identity sound --
        // clicking an already-selected building again stays silent instead
        // of replaying the clip every time.
        bool already_selected = selected_.size() == 1 && selected_[0] == best;
        selected_ = {best};
        // Direct port of the original's Mouse_53 handler (assets/gmk/
        // objects/control/Mouse_53.gml): a plain click that selects a
        // building plays that building's own identity sound (snd_base,
        // snd_barracks, ...) -- the SAME clip Audio::building_sound already
        // plays once when that building type finishes construction (see
        // drain_events's "building_ready" case), just also fired here on
        // selection. Ctrl-click (add_to_selection) doesn't trigger it,
        // matching the original's `not keyboard_check(vk_control)` guard.
        if (!already_selected) {
            if (Building* b = world.get_building(best)) audio_.building_sound(b->name);
        }
    }
}

void GameClient::select_all_of_type_in_viewport(const std::string& name, int team) {
    World& world = match_.world();
    double wx0, wy0, wx1, wy1;
    cam_.screen_to_world(0, 0, wx0, wy0);
    cam_.screen_to_world(view_w_, view_h_, wx1, wy1);
    selected_.clear();
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (u && u->name == name && u->common.team == team && u->common.alive &&
            u->common.x >= wx0 && u->common.x <= wx1 && u->common.y >= wy0 && u->common.y <= wy1) {
            selected_.push_back(ref);
        }
    }
}

void GameClient::box_select(double wx0, double wy0, double wx1, double wy1) {
    building_category_.clear();
    World& world = match_.world();
    selected_.clear();
    // A unit joins the drag-select as soon as ANY part of its hitbox
    // (same 20px half-size select_at's own circular unit radius uses)
    // overlaps the drawn rectangle -- a plain center-point-inside test
    // (the old behaviour) misses a unit whose sprite is clearly inside
    // the box but whose exact center happens to fall just outside it,
    // which read as the drag feeling stricter than it looked.
    constexpr double kUnitHalf = 20.0;
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (!u || u->common.team != 0 || !u->common.alive) continue;
        // Air units are drawn lifted by `height`, so test the box against where
        // the sprite appears (same reason as select_at), else a marquee over a
        // flying plane misses it.
        double uy = u->common.y - (u->common.is_air ? u->height : 0.0);
        if (u->common.x + kUnitHalf >= wx0 && u->common.x - kUnitHalf <= wx1 &&
            uy + kUnitHalf >= wy0 && uy - kUnitHalf <= wy1) {
            selected_.push_back(ref);
        }
    }
}

void GameClient::right_click_order(double wx, double wy, bool shift_queue) {
    World& world = match_.world();
    if (selected_.empty()) return;

    // A right-click move/attack un-garrisons any stationed plane in the
    // selection so it leaves the airbase to carry out the order (see
    // Unit::stationed). Harmless for non-air units, which never set it.
    {
        std::vector<uint32_t> ids;
        for (auto ref : selected_)
            if (Unit* su = world.get(ref); su && su->common.alive) ids.push_back(su->common.id);
        if (!ids.empty()) issue(ww::sim::LaunchCommand{std::move(ids)});
    }

    // Classify what's at the click point: nearest enemy, then resource/farm,
    // then damaged own building, then unbuilt own foundation, else ground.
    // Computed up front (not just for the unit-order path below) so the
    // building-rally branch just past it can also tell whether the click
    // landed on a resource/foundation worth rallying straight onto.
    EntityRef enemy = kNullRef, resource = kNullRef, damaged = kNullRef, foundation = kNullRef;
    EntityRef workable_farm = kNullRef, dropoff_building = kNullRef, transport = kNullRef,
              dead_farm = kNullRef;
    double best_enemy = 1e18, best_res = 1e18, best_dmg = 1e18, best_found = 1e18, best_farm = 1e18,
        best_drop = 1e18, best_transport = 1e18, best_dead = 1e18;
    // An own landing site (airbase or aircraft carrier) under the cursor: with
    // aircraft selected, right-clicking it lands them there (see the per-unit
    // order loop). id-based so it works for both a building and a unit carrier.
    uint32_t land_site_id = 0;
    EntityRef land_site_ref = kNullRef; // for the green "you're landing here" strobe
    bool have_land_site = false;
    double best_land = 1e18;

    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (!u || !u->common.alive) continue;
        double d = dist2(wx, wy, u->common.x, u->common.y);
        // Own transport ship under the cursor: land units board it (checked
        // before the enemy scan's team filter so a friendly ship wins).
        if (u->common.team == 0 && u->transport_cap > 0 && d < 40 * 40 && d < best_transport) {
            best_transport = d; transport = ref;
        }
        // Own aircraft carrier under the cursor: selected aircraft land on its
        // deck (a mobile sea airbase). Generous hitbox -- carriers are large.
        if (u->common.team == 0 && u->is_carrier && d < 90.0 * 90.0 && d < best_land) {
            best_land = d; land_site_id = u->common.id; land_site_ref = ref; have_land_site = true;
        }
        if (u->common.team == 0 || u->common.team < 0) continue;
        if (d < 30 * 30 && d < best_enemy) { best_enemy = d; enemy = ref; }
    }
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b) continue;
        // True rectangular footprint, not a circle sized off foot_px (==
        // max(foot_w, foot_h)) -- same fix as GameClient::select_at, so a
        // right-click near a non-square building's corner (repair/rally/
        // drop-off/foundation targeting) hits it just as reliably as a
        // left-click selection does.
        double hw = b->foot_w * 0.5, hh = b->foot_h * 0.5;
        // Same bottom-anchored footprint the sim itself collides against
        // (see World::footprint_dy) -- tower/aa tower/outpost's footprint
        // is only their base tile, not centered on common.y like every
        // other building's.
        double fcy = b->common.y + footprint_dy(b->name);
        if (wx < b->common.x - hw || wx > b->common.x + hw || wy < fcy - hh || wy > fcy + hh) {
            continue;
        }
        double d = dist2(wx, wy, b->common.x, fcy);
        // Tracked independently of the enemy/foundation/farm/damaged chain
        // below (not an else-if branch of it) so a drop-off building that's
        // ALSO damaged still classifies as both -- the per-unit loop only
        // acts on this one when the unit is actually carrying something,
        // and otherwise falls through to "damaged" (repair) as before.
        if (b->common.team == 0 && b->complete && b->is_dropoff && d < best_drop) {
            best_drop = d; dropoff_building = ref;
        }
        // Own complete airbase under the cursor: selected aircraft land on it
        // (the click is already inside this building's footprint here).
        if (b->common.team == 0 && b->complete && b->name == "airbase" && d < best_land) {
            best_land = d; land_site_id = b->common.id; land_site_ref = ref; have_land_site = true;
        }
        if (b->common.team != 0 && b->common.team >= 0 && d < best_enemy) { best_enemy = d; enemy = ref; }
        else if (b->common.team == 0 && !b->complete && d < best_found) { best_found = d; foundation = ref; }
        // An unoccupied own farm: right-clicking it sends a gatherer
        // straight to work it (checked before the generic "damaged"
        // fallback so a farm ready for a worker takes priority over a
        // repair order even if it's also below full HP).
        else if (b->common.team == 0 && b->complete && b->name == "farm" && !b->exhausted &&
                 !b->occupied_by.valid() && d < best_farm) {
            best_farm = d; workable_farm = ref;
        } else if (b->common.team == 0 && b->complete && b->name == "farm" && b->exhausted &&
                   d < best_dead) {
            best_dead = d; dead_farm = ref; // a dead farm: re-sow it (see below)
        } else if (b->common.team == 0 && b->complete && b->common.hp < b->common.max_hp && d < best_dmg) {
            best_dmg = d; damaged = ref;
        }
    }
    for (auto ref : world.active_resources) {
        Resource* r = world.get_resource(ref);
        if (!r || !r->common.alive) continue;
        double d = dist2(wx, wy, r->common.x, r->common.y);
        if (d < 30 * 30 && d < best_res) { best_res = d; resource = ref; }
    }

    // Buildings: right-click sets the rally point instead of a unit order
    // (buildings and units are never selected together in normal play --
    // box_select only ever picks up units, and a plain select_at replaces
    // the whole selection with one entity). World-side, gather_x/gather_y
    // already exist and are already consumed by building_behavior.cpp to
    // send freshly trained units there; this was just the missing input
    // that sets them (obj_building/Step.gml's rally-point right-click).
    // Landing the rally point on a live resource/workable farm/own
    // foundation also records that as rally_target -- building_behavior.cpp
    // sends a freshly-trained gatherer straight into that job (gather/
    // build) instead of just walking to the point and standing idle.
    if (Building* b0 = world.get_building(selected_[0]); b0 && b0->common.team == 0) {
        EntityRef rally_target =
            resource.valid() ? resource : workable_farm.valid() ? workable_farm : foundation;
        // Landing the rally point on a resource/workable farm/foundation
        // snaps the stored point to that entity's own center (rather than
        // wherever inside its hitbox was actually clicked) and flashes the
        // same green "targeted" strobe a villager sent to gather/build
        // there gets -- same target_flashes_ mechanism the unit-order path
        // below uses via its own push_flash lambda (defined further down,
        // past this branch's early return, so inlined here instead of
        // sharing it).
        double rx = wx, ry = wy;
        if (rally_target.valid()) {
            if (EntityCommon* c = world.common(rally_target)) { rx = c->x; ry = c->y; }
            target_flashes_.erase(std::remove_if(target_flashes_.begin(), target_flashes_.end(),
                                                 [&](const TargetFlash& f) { return f.ref == rally_target; }),
                                  target_flashes_.end());
            target_flashes_.push_back({rally_target, 0.0, 2.0});
        }
        for (auto ref : selected_) {
            Building* b = world.get_building(ref);
            if (!b || b->common.team != 0) continue;
            b->gather_x = rx;
            b->gather_y = ry;
            b->rally_set = true;
            b->rally_target = rally_target;
        }
        return;
    }

    // Re-sow a dead (exhausted) farm: right-click it with a villager selected to
    // pay 40 WOOD (seed costs wood, never food -- same price the auto-replant
    // toggle and the AI pay, see kFarmResowWood in unit_behavior.cpp and
    // control_ai.cpp) and refill it to full, then send a villager to work it
    // (the gmk farm re-sow -- see session.py). Consumes the click either way.
    if (dead_farm.valid()) {
        Building* df = world.get_building(dead_farm);
        EntityRef vill = kNullRef;
        for (auto uref : selected_) {
            Unit* u = world.get(uref);
            if (u && u->common.team == 0 && u->common.alive && u->is_gatherer &&
                !u->common.is_ship && !u->common.is_air) { vill = uref; break; }
        }
        if (df && vill.valid()) {
            Team& t0 = match_.control().teams[0];
            double wood = t0.res.count("wood") ? t0.res["wood"] : 0.0;
            if (wood >= 40.0) {
                t0.res["wood"] = wood - 40.0;
                df->exhausted = false;
                df->amount = df->max_farm_food;
                df->highlight = 1.3;
                if (Unit* u = world.get(vill))
                    apply_command(world, GatherCommand{u->common.id, world.common(dead_farm)->id});
                audio_.building_sound("farm"); // re-sown -- same cue as building a farm
            } else {
                world.events.push({EventType::Warn, "", 0, 0, 0, kNullRef, "You need more resources!"});
                audio_.play("error", 0, 0, false);
            }
        }
        return; // click handled
    }

    bool acted_attack = false, acted_gather = false, acted_build = false, acted_repair = false,
        acted_drop = false, acted_move = false, acted_board = false, moved_aircraft = false,
        acted_land = false;
    // Formation offset for a plain group move (see the final `else` branch
    // below): every selected unit's own personal target is (wx,wy) PLUS
    // that unit's own current offset from the centroid of the whole
    // selection, instead of the literal clicked point -- otherwise every
    // unit A*-paths to the exact same pixel and clumps/deadlocks fighting
    // over it. Direct port of the original GML's obj_unit/Step.gml "move
    // towards red arrow" handler (goto_x = waypoint.x + (self.x - avg_x));
    // aircraft are excluded there too (see the is_air check below), they
    // keep the literal clicked point. centroid_n <= 1 leaves centroid_x/y
    // unused (a single unit's offset from itself is zero anyway).
    double centroid_x = 0.0, centroid_y = 0.0;
    int centroid_n = 0;
    // Slowest non-air mover in the selection, so a group move (below) caps
    // every ground/naval unit to this pace instead of fast units outrunning
    // slow ones and scattering along the route -- see Unit::group_speed_px.
    // Aircraft never join this (they already keep their own literal target
    // and are excluded from the offset above them too).
    int ground_n = 0;
    double group_min_speed = 1e18;
    for (auto uref : selected_) {
        Unit* u = world.get(uref);
        if (!u || u->common.team != 0 || !u->common.alive) continue;
        centroid_x += u->common.x;
        centroid_y += u->common.y;
        ++centroid_n;
        if (!u->common.is_air) {
            ++ground_n;
            group_min_speed = std::min(group_min_speed, u->speed_px);
        }
    }
    if (centroid_n > 1) {
        centroid_x /= centroid_n;
        centroid_y /= centroid_n;
    }
    // Farthest any one of those per-unit centroid-offset targets is from
    // its own unit, used below (the plain-move branch) to scale each
    // unit's own pace by its own distance relative to this -- so units
    // with a short hop to their own target don't get there and just stand
    // around while a unit with a long hop straggles in, same reasoning as
    // issue_formation's own synchronized-arrival comment.
    double group_max_dist = 0.0;
    if (ground_n > 1) {
        for (auto uref : selected_) {
            Unit* u = world.get(uref);
            if (!u || u->common.team != 0 || !u->common.alive || u->common.is_air) continue;
            double tx = wx + (u->common.x - centroid_x), ty = wy + (u->common.y - centroid_y);
            group_max_dist = std::max(group_max_dist, std::hypot(tx - u->common.x, ty - u->common.y));
        }
    }
    // Gather-order spread: right-clicking one tree in a woodline with
    // several gatherers selected used to send every one of them to that
    // exact resource -- only ~2 can physically fit close enough to work it
    // (see World::nearest's use in unit_behavior.cpp's stall-based reseek,
    // which already corrects this reactively after a ~1.5s failed-approach
    // window). Assigning proactively here avoids that jostle/delay for the
    // common case entirely: gather_counts tracks how many gatherers are
    // aiming at each specific resource (seeded with whoever's ALREADY
    // working one, anywhere -- not just this selection, so the group won't
    // pile onto a tree some other villager is already on), and each unit
    // beyond a resource's cap gets redirected to the nearest same-type
    // (rtype, not name -- a tree and a palm both count as "wood" here too,
    // matching the sim's own reseek) alternative within the same 500px
    // range that reseek uses, falling back to the literal clicked resource
    // (today's behavior) if nothing else nearby is free either. Farms are
    // unaffected -- they already have hard exclusivity (Building::
    // occupied_by), so workable_farm's branch below doesn't need this.
    constexpr int kMaxGatherersPerResource = 2;
    std::vector<std::pair<EntityRef, int>> gather_counts;
    auto count_for = [&](EntityRef ref) -> int& {
        for (auto& [r, c] : gather_counts) {
            if (r == ref) return c;
        }
        gather_counts.push_back({ref, 0});
        return gather_counts.back().second;
    };
    if (resource.valid()) {
        for (auto uref2 : world.active_units) {
            Unit* eu = world.get(uref2);
            if (eu && eu->common.alive && eu->gather_target.valid()) ++count_for(eu->gather_target);
        }
    }
    auto pick_gather_target = [&](Unit* u) {
        Resource* r0 = world.get_resource(resource);
        if (!r0) return resource; // defensive -- resource.valid() implies this normally exists
        if (count_for(resource) < kMaxGatherersPerResource) {
            ++count_for(resource);
            return resource;
        }
        int want_rtype = r0->res.rtype;
        EntityRef alt = world.nearest(u->common.x, u->common.y, 500, [&](EntityRef ref, EntityCommon& c) {
            if (ref == resource || c.kind != EntityKind::Resource || !c.alive) return false;
            Resource* nr = world.get_resource(ref);
            return nr && nr->res.rtype == want_rtype && count_for(ref) < kMaxGatherersPerResource;
        });
        if (alt.valid()) {
            ++count_for(alt);
            return alt;
        }
        return resource; // nothing else nearby/free -- same fallback as before this change
    };
    for (auto uref : selected_) {
        Unit* u = world.get(uref);
        if (!u || u->common.team != 0 || !u->common.alive) continue;
        u->attack_ground.reset(); // any ordinary order cancels a standing bombard
        // Right-click an own airbase OR aircraft carrier with aircraft selected:
        // land there to refuel/rearm (same as the Land button, but at this
        // specific site) instead of flying a plain move order over it. The
        // stationed=true keeps them parked until re-ordered, so they show as
        // the small landed planes just like at an airbase.
        if (u->common.is_air && have_land_site) {
            u->order_queue.clear();
            u->active_queue_watch = kNullRef;
            u->queue_active = false;
            u->land_order = true;
            u->stationed = true;
            u->land_target_id = land_site_id;
            u->attack_target = kNullRef;
            u->rally.reset();
            u->forced = false;
            u->move_goal.reset();
            u->path.clear();
            moved_aircraft = true; // reuse the plane-move ack sound
            acted_land = true;     // strobe the airbase/carrier being landed on
            continue;
        }
        // Board a friendly transport: any land unit that isn't itself a ship
        // or aircraft (and not the transport being clicked) heads over to
        // garrison inside it.
        bool boardable = transport.valid() && uref != transport && !u->common.is_ship &&
                         !u->common.is_air && u->transport_cap == 0;
        if (boardable) {
            // Boarding a transport isn't one of the four shift-queueable
            // order kinds -- always immediate, and cancels any pending queue
            // like any other fresh order.
            u->order_queue.clear();
            u->active_queue_watch = kNullRef;
            u->queue_active = false;
            Unit* ship = world.get(transport);
            u->load_target = transport;
            u->attack_target = kNullRef;
            u->gather_target = kNullRef;
            u->gather_rtype = -1;
            u->build_target = kNullRef;
            u->repair_target = kNullRef;
            u->drop_target = kNullRef;
            // Send them to the shore next to the ship straight away; the sim
            // finishes the boarding once they're within reach (update_unit).
            if (ship) {
                auto [tx, ty] = world.nearest_passable(ship->common.x, ship->common.y,
                                                       /*is_air=*/false, /*is_ship=*/false);
                u->move_goal = Vec2{tx, ty};
                u->group_speed_px = -1.0; // fresh order, not a group move -- don't inherit an old cap
                u->need_path = true;
                u->path.clear();
                u->path_i = 0;
            }
            acted_board = true;
        } else if (enemy.valid()) {
            if (shift_queue) {
                // A full queue (kMaxQueuedOrders) silently eats the click --
                // no ack sound, no marker -- rather than replacing anything.
                if (world.queue_order(uref, {QueuedOrderKind::Attack, 0, 0, enemy})) acted_attack = true;
            } else {
                apply_command(world, AttackCommand{u->common.id, world.common(enemy)->id});
                acted_attack = true;
            }
        } else if (dropoff_building.valid() && u->carry > 0) {
            // Force a drop-off regardless of current carry (not just once
            // full): walk straight to the building and unload, then go
            // idle -- clearing gather_target here is what keeps them from
            // automatically resuming their old gather job afterward (see
            // update_gather's "no gather_target -> stays idle for a
            // non-AI team" branch). Not shift-queueable (out of scope) --
            // always immediate.
            u->order_queue.clear();
            u->active_queue_watch = kNullRef;
            u->queue_active = false;
            u->drop_target = dropoff_building;
            u->gather_target = kNullRef;
            u->gather_rtype = -1; // see World::order_move's comment
            u->attack_target = kNullRef;
            u->build_target = kNullRef;
            u->repair_target = kNullRef;
            u->move_goal.reset();
            u->path.clear();
            u->approach_prev_pos.reset();
            u->approach_progress_check_t = 0.0;
            u->approach_target.reset();
            acted_drop = true;
        } else if (resource.valid() && u->is_gatherer) {
            EntityRef target = pick_gather_target(u);
            if (shift_queue) {
                if (world.queue_order(uref, {QueuedOrderKind::Gather, 0, 0, target})) acted_gather = true;
            } else {
                apply_command(world, GatherCommand{u->common.id, world.common(target)->id});
                acted_gather = true;
            }
        } else if (workable_farm.valid() && u->is_gatherer) {
            if (shift_queue) {
                if (world.queue_order(uref, {QueuedOrderKind::Gather, 0, 0, workable_farm}))
                    acted_gather = true;
            } else {
                apply_command(world, GatherCommand{u->common.id, world.common(workable_farm)->id});
                acted_gather = true;
            }
        } else if (foundation.valid() && u->is_gatherer) {
            if (shift_queue) {
                if (world.queue_order(uref, {QueuedOrderKind::Build, 0, 0, foundation})) acted_build = true;
            } else {
                u->order_queue.clear();
                u->active_queue_watch = kNullRef;
                u->queue_active = false;
                if (ww::sim::EntityCommon* fc = world.common(foundation))
                    issue(ww::sim::AssignBuildCommand{{u->common.id}, fc->id});
                acted_build = true;
            }
        } else if (damaged.valid() && u->is_gatherer) {
            // Repair isn't shift-queueable (out of scope) -- always immediate.
            u->order_queue.clear();
            u->active_queue_watch = kNullRef;
            u->queue_active = false;
            if (ww::sim::EntityCommon* dc = world.common(damaged))
                issue(ww::sim::RepairCommand{{u->common.id}, dc->id});
            acted_repair = true;
        } else {
            double tx = wx, ty = wy;
            double gcap = -1.0;
            if (!u->common.is_air && centroid_n > 1) {
                tx = wx + (u->common.x - centroid_x);
                ty = wy + (u->common.y - centroid_y);
            }
            if (!u->common.is_air && ground_n > 1) {
                double d = std::hypot(tx - u->common.x, ty - u->common.y);
                gcap = (group_max_dist > 1.0) ? std::max(1.0, d * group_min_speed / group_max_dist)
                                              : group_min_speed;
            }
            if (shift_queue) {
                if (world.queue_order(uref, {QueuedOrderKind::Move, tx, ty, kNullRef})) {
                    acted_move = true;
                    if (u->common.is_air) moved_aircraft = true;
                }
            } else {
                apply_command(world, MoveCommand{u->common.id, tx, ty, gcap});
                acted_move = true;
                if (u->common.is_air) moved_aircraft = true;
            }
        }
    }
    // objects/obj_unit/Step.gml's "move towards red arrow" handler plays
    // snd_plane_backing (engine revving up) for aerial units on top of the
    // generic move acknowledgement -- civ voice lines for the generic case
    // aren't ported (see app.cpp's scope comment), so this is the one
    // move-order sound this port actually plays.
    if (moved_aircraft) audio_.play("plane_move", wx, wy, true);

    // Order acknowledgement (see play_order_ack): mounts play their animal
    // noise, ground vehicles rev, planes/ships stay quiet, and only infantry
    // use the civ voice. Only when an order actually landed on a selected unit.
    if (acted_attack || acted_gather || acted_build || acted_repair || acted_drop || acted_move ||
        acted_board) {
        play_order_ack(acted_attack);
    }

    // Visual feedback -- obj_unit/obj_building/obj_tree's "targeted"
    // strobe (a light-green marker pinned to the order's target) for any
    // order with an entity target, or the red spr_arrow waypoint for a
    // plain ground move. Mutually exclusive, same as the original's
    // separate order-type branches; only fires if at least one selected
    // unit actually took that branch (mirrors the original's `n>0` gate).
    auto push_flash = [&](EntityRef ref) {
        target_flashes_.erase(std::remove_if(target_flashes_.begin(), target_flashes_.end(),
                                             [&](const TargetFlash& f) { return f.ref == ref; }),
                              target_flashes_.end());
        target_flashes_.push_back({ref, 0.0, 2.0});
    };
    if (acted_land) {
        push_flash(land_site_ref); // green ring on the airbase/carrier being landed on
    } else if (acted_board) {
        push_flash(transport); // strobe the ship being boarded
    } else if (acted_attack) {
        push_flash(enemy);
    } else if (acted_drop) {
        push_flash(dropoff_building);
    } else if (acted_gather) {
        // Whichever of the two gather-order branches actually fired above
        // (a wild resource takes priority over an unoccupied farm when
        // both are somehow in range of the same click).
        push_flash(resource.valid() ? resource : workable_farm);
    } else if (acted_build) {
        push_flash(foundation);
    } else if (acted_repair) {
        push_flash(damaged);
    } else if (acted_move) {
        // Only one arrow marker at a time, same as the original destroying
        // any existing obj_waypoint before creating the new one.
        effects_.erase(std::remove_if(effects_.begin(), effects_.end(),
                                      [](const ClientEffect& e) { return e.sprite == "spr_arrow"; }),
                       effects_.end());
        effects_.push_back({"spr_arrow", wx, wy, 0.0, 1.0, 0});
    }
}

bool GameClient::formation_drag_eligible() {
    if (!placing_.empty() || attack_move_armed_ || attack_ground_armed_) return false;
    // Needs 2+ own living units: a single selected unit just gets an ordinary
    // move order on right-click, no formation drag (reference: len(movers)>1).
    // (get() is null for a building ref, so a building's right-click still
    // sets a rally point rather than starting a formation.)
    int own = 0;
    for (auto ref : selected_) {
        Unit* u = match_.world().get(ref);
        if (u && u->common.alive && u->common.team == 0 && ++own > 1) return true;
    }
    return false;
}

std::vector<std::pair<double, double>> GameClient::formation_slots(
    double ax, double ay, double bx, double by, const std::vector<ww::sim::EntityRef>& units) {
    int n = static_cast<int>(units.size());
    std::vector<std::pair<double, double>> out(n, {ax, ay});
    if (n == 0) return out;
    World& world = match_.world();
    // Minimum gap between neighbours -- kept above the sim's hard-block
    // distance (kSeparation, unit_behavior.cpp) so a settled rank never
    // finds its own slots mutually blocked.
    // The ACTUAL column gap grows past this as the player drags wider.
    constexpr double kMinSpacing = 32.0;
    // Staggered floors out at this wider gap so it reads as "a bit more
    // spread out" than the old shared kMinSpacing.
    constexpr double kStaggerSpacing = 44.0;
    // Row (single-file "column" internally) used to have NO floor at all (a
    // short drag with many units packed them closer than the sim's
    // hard-block distance, so they'd sit collapsed into each other until the
    // separation force suddenly shoved them apart the moment they stopped
    // moving). It floors at its own, smaller gap -- ~30% tighter than
    // staggered's, still comfortably above kSeparation (unit_behavior.cpp).
    constexpr double kRowSpacing = kStaggerSpacing * 0.7;
    double dx = bx - ax, dy = by - ay;
    double len = std::sqrt(dx * dx + dy * dy);
    double ux = 1.0, uy = 0.0; // front-rank axis (unit vector A->B)
    if (len > 1e-3) { ux = dx / len; uy = dy / len; }
    // (staggered) Columns = as many as fit along the drawn line at
    // >= kStaggerSpacing, capped at n. Then spread those columns EVENLY
    // across the full drawn length, so the player controls how wide the
    // formation stretches purely by how far they pull the cursor: a long
    // drag with few units => one wide, sparse rank; a short drag => tighter
    // columns that wrap into deeper ranks. The resulting col_gap is always
    // >= kStaggerSpacing (since cols-1 <= len/kStaggerSpacing).
    double px = -uy, py = ux; // perpendicular: the rank-stacking direction
    // Stack ranks toward the units' current centroid, so the formation builds
    // up behind the drawn front line (facing away from where they came from).
    double cx = 0.0, cy = 0.0;
    for (auto ref : units) {
        if (EntityCommon* c = world.common(ref)) { cx += c->x; cy += c->y; }
    }
    cx /= n; cy /= n;
    if (px * (cx - ax) + py * (cy - ay) < 0.0) { px = -px; py = -py; }

    // Shape per formation_type_: column/staggered are a direct port of
    // game/session.py's _drag_formation; box and split are local additions
    // (see their own comments below). Builds a plain, UNORDERED list of N
    // ideal positions first -- which specific unit ends up at which
    // position is decided afterward (see the assignment pass below), not
    // baked into the order these are generated in.
    std::vector<std::pair<double, double>> slots(n);
    if (formation_type_ == "column" || n <= 1) {
        // Straight rank(s) along the drawn line a -> b, spaced >= kRowSpacing.
        // The drawn length sets the row's WIDTH -- per_row is however many
        // fit at kRowSpacing within len, and anyone left over wraps into
        // additional straight ranks stacked behind (depth direction) --
        // EXCEPT when len is so short that per_row would fall below a
        // square-ish floor (ceil(sqrt(n)), same idea as staggered's floor):
        // uncapped, a short drag turns into one man directly behind the
        // last, a long thin column many ranks deep rather than a block. So
        // per_row never drops below that floor, and the row's width then
        // stretches to whatever that minimum per_row actually needs
        // (>= (per_row-1)*kRowSpacing) instead of literally squeezing into
        // the too-short drawn length.
        int min_per_row = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
        int per_row = std::clamp(static_cast<int>(len / kRowSpacing) + 1, min_per_row, n);
        double span_len = std::max(len, static_cast<double>(per_row - 1) * kRowSpacing);
        int span = per_row - 1;
        for (int k = 0; k < n; ++k) {
            int row = k / per_row, col = k % per_row;
            double t = span ? static_cast<double>(col) / span : 0.0;
            double depth = row * kRowSpacing;
            slots[k] = {ax + ux * span_len * t + px * depth, ay + uy * span_len * t + py * depth};
        }
    } else if (formation_type_ == "box") {
        // A HOLLOW square: units ring the perimeter with open ground in the
        // middle (an infantry square), not a filled grid -- a filled
        // rectangle of units is a "staggered"/block formation, not what
        // "box" means here. Side length sized so N units spaced evenly
        // around the ring land ~kMinSpacing apart (side = N*kMinSpacing/4,
        // one quarter of the ring per side); a longer drag can still make
        // it bigger (side = drawn length instead), same "player controls
        // size by how far they drag, with a sensible floor for a short
        // drag" convention the other shapes use.
        double side = std::max(len, n * kMinSpacing / 4.0);
        double perim = side * 4.0;
        double mid_x = ax + ux * len * 0.5, mid_y = ay + uy * len * 0.5;
        double hs = side * 0.5;
        auto corner_pt = [&](double lx, double ly) {
            return std::pair<double, double>{mid_x + ux * lx + px * ly, mid_y + uy * lx + py * ly};
        };
        std::pair<double, double> corners[4] = {corner_pt(-hs, -hs), corner_pt(hs, -hs),
                                                corner_pt(hs, hs), corner_pt(-hs, hs)};
        for (int k = 0; k < n; ++k) {
            double t = perim * static_cast<double>(k) / n; // evenly spaced around the ring
            int side_idx = std::min(3, static_cast<int>(t / side));
            double frac = (side > 0.0) ? (t - side_idx * side) / side : 0.0;
            auto& c0 = corners[side_idx];
            auto& c1 = corners[(side_idx + 1) % 4];
            slots[k] = {c0.first + (c1.first - c0.first) * frac, c0.second + (c1.second - c0.second) * frac};
        }
    } else if (formation_type_ == "staggered") {
        // staggered: units spread evenly across the drawn width, stacked in
        // depth, with every other rank offset by half a cell (a checkerboard
        // block that reads less like a rigid grid). per_row targets a
        // roughly SQUARE layout by default (ceil(sqrt(n))) regardless of how
        // short the drag was -- a short (or click-like, barely-past-the-drag-
        // threshold) drag used to force everything down to 1-2 columns
        // stacked many ranks deep, which read as scattered pairs rather than
        // a block. A longer drag still widens it into a shallower, wider
        // block exactly as before, since per_row also grows with the drawn
        // length past this square-ish floor.
        int min_per_row = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
        int per_row = std::clamp(static_cast<int>(len / kStaggerSpacing) + 1, min_per_row, n);
        // Once per_row is pushed above what the raw drag length would fit at
        // kStaggerSpacing (the short-drag case above), spread across the WIDTH
        // per_row actually needs instead of the literal (too-short) drag
        // length, so columns still end up at least kStaggerSpacing apart rather
        // than crammed into a few pixels.
        double span_len = std::max(len, static_cast<double>(per_row - 1) * kStaggerSpacing);
        for (int k = 0; k < n; ++k) {
            int row = k / per_row, col = k % per_row;
            int span = per_row - 1;
            double along = span ? span_len * (static_cast<double>(col) / span) : span_len * 0.5;
            if ((row % 2) && span) {
                along = std::min(span_len, along + (span_len / span) * 0.5);
            }
            double depth = row * kStaggerSpacing;
            slots[k] = {ax + ux * along + px * depth, ay + uy * along + py * depth};
        }
    } else {
        // split: a flanking maneuver -- half the units peel off to each END
        // of the drawn line (a for the left flank, b for the right) instead
        // of spreading across the middle like the other three shapes. Each
        // half forms its own small square-ish block (same per_row idea as
        // staggered) stacked back in the depth direction from its anchor
        // point, so the two flanks read as two separate approach groups
        // either side of whatever's at the drawn line.
        int left_n = (n + 1) / 2, right_n = n - left_n;
        auto place_block = [&](double anchor_x, double anchor_y, int count, int start_idx) {
            if (count <= 0) return;
            int per_row = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count)))));
            int span = per_row - 1;
            for (int k = 0; k < count; ++k) {
                int row = k / per_row, col = k % per_row;
                double along = span ? kStaggerSpacing * (static_cast<double>(col) - span / 2.0) : 0.0;
                double depth = row * kStaggerSpacing;
                slots[start_idx + k] = {anchor_x + ux * along + px * depth, anchor_y + uy * along + py * depth};
            }
        };
        place_block(ax, ay, left_n, 0);
        place_block(bx, by, right_n, left_n);
    }

    // Assign units to slots by solving the assignment problem exactly
    // (Hungarian algorithm, minimizing TOTAL travel distance across the
    // whole group) rather than any positional heuristic. Replaces two
    // earlier attempts that both turned out wrong: sorting units by
    // position along the drag axis and pairing them off in that order only
    // considered ONE axis, so a unit could get sent across the whole
    // formation while a much closer unit took its natural spot; a "greedily
    // claim the globally closest still-open (unit, slot) pair" heuristic
    // fixed some of that but can still starve one side of the formation of
    // any nearby slot by over-serving the other side first (measured on a
    // simple re-form-the-same-box-shifted-sideways case: greedy produced
    // ~1200px of total movement across 12 units where the optimal solution
    // -- every unit just steps sideways by the shift distance -- needs
    // only ~640px). The exact solution directly fixes both symptoms this
    // was chasing: a unit that's already close to some slot reliably gets
    // assigned to it, and re-forming a similar shape nearby lands each unit
    // on (near) its own already-current position instead of scrambling the
    // group. O(n^3) -- worth it at formation-sized N (tens of units, not
    // thousands).
    std::vector<std::vector<double>> cost(n, std::vector<double>(n, 0.0));
    for (int i = 0; i < n; ++i) {
        EntityCommon* c = world.common(units[i]);
        double ux_ = c ? c->x : ax, uy_ = c ? c->y : ay;
        for (int s = 0; s < n; ++s) {
            cost[i][s] = std::hypot(slots[s].first - ux_, slots[s].second - uy_);
        }
    }
    std::vector<int> assigned_slot = hungarian_assignment(cost);
    for (int i = 0; i < n; ++i) out[i] = slots[assigned_slot[i]];

    // A computed slot landing on impassable terrain (a tree, rock, water) is
    // left as-is here -- World::order_move already falls back to
    // nearest_passable for a single blocked destination (and the normal
    // A* routing gets a unit there in the first place), so a unit whose
    // exact slot is unreachable still ends up close by rather than stuck.
    // Not attempting anything smarter for the multi-unit case: an obstacle
    // sitting across part of a formation is expected to disrupt it while
    // units route around/into position, same as it would for a plain group
    // move -- the formation reasserting itself once everyone's through is
    // what resolve_overlap and each unit's own distinct slot already give,
    // not something this needs to solve up front.
    return out;
}

void GameClient::issue_formation(double ax, double ay, double bx, double by) {
    World& world = match_.world();
    std::vector<EntityRef> units;
    for (auto ref : selected_) {
        Unit* u = world.get(ref);
        if (u && u->common.alive && u->common.team == 0) units.push_back(ref); // aircraft included now
    }
    if (units.empty()) { right_click_order(ax, ay); return; }
    auto slots = formation_slots(ax, ay, bx, by, units);
    // Slowest mover's own top speed is the group's reference pace (see
    // Unit::group_speed_px). A single uniform cap at that pace isn't enough
    // on its own, though: units with a short hop to their own slot would
    // still reach it and just stand there while a unit with a long hop
    // straggles in -- the formation would only actually look like a
    // formation once everyone finally settled, not while walking there.
    // So each unit's speed is instead scaled by its own distance-to-slot
    // relative to the group's farthest traveler (capped at the reference
    // pace, i.e. the SLOWEST unit still never exceeds its natural speed):
    // every unit then takes the same total time (dist_i / speed_i ==
    // max_dist / group_min_speed for all i), so the shape holds together
    // throughout the walk instead of only converging at the very end.
    double group_min_speed = 1e18;
    for (auto ref : units) {
        if (Unit* u = world.get(ref)) group_min_speed = std::min(group_min_speed, u->speed_px);
    }
    double max_dist = 0.0;
    std::vector<double> dist(units.size(), 0.0);
    for (size_t i = 0; i < units.size(); ++i) {
        if (Unit* u = world.get(units[i])) {
            dist[i] = std::hypot(slots[i].first - u->common.x, slots[i].second - u->common.y);
            max_dist = std::max(max_dist, dist[i]);
        }
    }
    for (size_t i = 0; i < units.size(); ++i) {
        if (Unit* u = world.get(units[i])) {
            double gcap = -1.0;
            if (units.size() > 1) {
                gcap = (max_dist > 1.0) ? std::max(1.0, dist[i] * group_min_speed / max_dist)
                                        : group_min_speed;
            }
            apply_command(world, MoveCommand{u->common.id, slots[i].first, slots[i].second, gcap});
        }
    }
    // Move-order ack (a formation drag is still a move order) -- see
    // play_order_ack for the per-unit-type rules.
    play_order_ack(/*attack=*/false);
    // Red waypoint arrow at the front-line midpoint (same one-at-a-time marker
    // a plain group move drops -- see right_click_order's acted_move branch).
    effects_.erase(std::remove_if(effects_.begin(), effects_.end(),
                                  [](const ClientEffect& e) { return e.sprite == "spr_arrow"; }),
                   effects_.end());
    effects_.push_back({"spr_arrow", (ax + bx) / 2.0, (ay + by) / 2.0, 0.0, 1.0, 0});
}

void GameClient::draw_formation_preview(SDL_Renderer* renderer) {
    if (!r_dragging_ || !rdrag_start_) return;
    World& world = match_.world();
    std::vector<EntityRef> units;
    for (auto ref : selected_) {
        Unit* u = world.get(ref);
        if (u && u->common.alive && u->common.team == 0) units.push_back(ref); // aircraft included now
    }
    if (units.empty()) return;
    double ax, ay, bx, by;
    cam_.screen_to_world(rdrag_start_->x, rdrag_start_->y, ax, ay);
    cam_.screen_to_world(mouse_pos_.x, mouse_pos_.y, bx, by);
    auto slots = formation_slots(ax, ay, bx, by, units);
    // The drawn front line...
    SDL_SetRenderDrawColor(renderer, 60, 230, 60, 255);
    SDL_RenderDrawLine(renderer, rdrag_start_->x, rdrag_start_->y, mouse_pos_.x, mouse_pos_.y);
    // ...and a green ghost ellipse at every unit's future slot.
    for (auto& s : slots) {
        int sx, sy;
        cam_.world_to_screen(s.first, s.second, sx, sy);
        int rx = std::max(6, static_cast<int>(12 * cam_.zoom));
        int ry = std::max(3, rx / 2);
        draw_ellipse(renderer, sx, sy, rx, ry, {60, 230, 60, 255});
    }
}

void GameClient::draw(SDL_Renderer* renderer) {
    World& world = match_.world();

    SDL_SetRenderDrawColor(renderer, 28, 40, 24, 255);
    SDL_RenderClear(renderer);

    // Nuke screen shake: jolt the whole world render by a decaying jitter
    // (restored at the end of draw so it never accumulates). Offset is applied
    // in world units (÷zoom) so the on-screen amplitude is constant.
    double shake_save_x = cam_.x, shake_save_y = cam_.y;
    bool shaking = nuke_shake_t_ > 0.0;
    if (shaking) {
        double amp = (nuke_shake_t_ / 1.1) * 9.0 / std::max(0.1, cam_.zoom);
        cam_.x += amp * std::sin(render_clock_ * 63.0);
        cam_.y += amp * std::cos(render_clock_ * 51.0);
    }

    auto vr = cam_.visible_rect();
    int x0 = std::max(0, static_cast<int>(vr.x / TILE));
    int x1 = std::min(world.cols, static_cast<int>((vr.x + vr.w) / TILE) + 1);
    int y0 = std::max(0, static_cast<int>(vr.y / TILE));
    int y1 = std::min(world.rows, static_cast<int>((vr.y + vr.h) / TILE) + 1);
    for (int tx = x0; tx < x1; ++tx) {
        for (int ty = y0; ty < y1; ++ty) {
            // Size each tile from its own corner to the NEXT tile's corner
            // (rather than a single TILE*zoom rounded once for every tile)
            // so adjacent tiles' edges always meet exactly -- using one
            // fixed size for all tiles let per-tile rounding/truncation
            // drift by a pixel at some zoom levels, opening a 1px gap
            // (visible as a black seam, since it let the clear color show
            // through) between certain rows/columns.
            int sx, sy, sx1, sy1;
            cam_.world_to_screen(tx * TILE, ty * TILE, sx, sy);
            cam_.world_to_screen((tx + 1) * TILE, (ty + 1) * TILE, sx1, sy1);
            SDL_Rect dst{sx, sy, std::max(1, sx1 - sx), std::max(1, sy1 - sy)};
            int tid = world.terrain[tx][ty];
            // Ocean shimmer: water tiles mirror horizontally on a slow, per-
            // tile-staggered cadence (~every 2s, phase spread by tile coords)
            // so the surface reads as gently churning rather than a static
            // sheet. Purely cosmetic -- terrain data is untouched.
            bool flip = false;
            if (tid == 2) {
                double phase = render_clock_ * 0.5 + (tx * 7 + ty * 13) * 0.37;
                flip = (static_cast<int>(phase) & 1) != 0;
            }
            atlas_.draw_stretched(terrain_sprite(tid), dst, 0, flip);
        }
    }

    // Visibility culling: scenario maps can have hundreds of resource
    // nodes scattered across the whole map, and drawing every single one
    // every frame regardless of whether it's on-screen is wasted work --
    // skip anything well outside the camera's visible rect (margin covers
    // the largest sprites so nothing pops in/out at the edge).
    constexpr double kCullMargin = 100.0;
    auto in_view = [&](double x, double y) {
        return x >= vr.x - kCullMargin && x <= vr.x + vr.w + kCullMargin && y >= vr.y - kCullMargin &&
               y <= vr.y + vr.h + kCullMargin;
    };

    double alpha = std::clamp(accumulator_ / kFixedDt, 0.0, 1.0);
    auto interp = [&](uint32_t id, double cx, double cy, const std::unordered_map<uint32_t, Pos>& prev) {
        auto it = prev.find(id);
        if (it == prev.end()) return std::pair<double, double>{cx, cy};
        return std::pair<double, double>{it->second.x + (cx - it->second.x) * alpha,
                                         it->second.y + (cy - it->second.y) * alpha};
    };
    auto is_selected = [&](EntityRef ref) {
        return std::find(selected_.begin(), selected_.end(), ref) != selected_.end();
    };
    // Command-group badge: a small white number beside a SELECTED entity's
    // ellipse/rect if it belongs to one of the Ctrl+1-9 groups (see
    // assign_command_group/select_command_group) -- only checked for
    // already-selected entities (cheap: at most 9 small vectors, scanned
    // only for however many are currently selected), not drawn on every
    // grouped entity unconditionally.
    auto draw_group_badge = [&](EntityRef ref, int ex, int ey) {
        int group = group_of(ref);
        if (group <= 0) return;
        std::string s = std::to_string(group);
        text_.draw(s, ex, ey, {255, 255, 255, 255}, ui(11));
    };
    auto draw_health_bar = [&](int ex, int top_y, int bar_w, double frac) {
        constexpr int kBarH = 6;
        SDL_SetRenderDrawColor(renderer, 25, 25, 25, 220);
        SDL_Rect bg{ex - bar_w / 2, top_y, bar_w, kBarH};
        SDL_RenderFillRect(renderer, &bg);
        SDL_SetRenderDrawColor(renderer, 60, 225, 60, 255);
        SDL_Rect fg{ex - bar_w / 2, top_y, static_cast<int>(bar_w * frac), kBarH};
        SDL_RenderFillRect(renderer, &fg);
    };
    // All sprites here use a CENTER pivot (manifest ox/oy == fw/fh / 2), so
    // world_to_screen(x,y) lands at the sprite's vertical middle, not its
    // feet -- these offset by half the sprite's HEIGHT to reach its visual
    // base/top. Called BEFORE each entity's own sprite draw, so the sprite
    // naturally occludes the far/back half instead of the marker floating
    // on top of everything.
    // origin_y is the sprite's pivot Y (its manifest oy): the ellipse straddles
    // the sprite's VISUAL base = (footprint_h - origin_y) below the draw point,
    // so a centre-pivoted unit gets it at the feet while a base-offset sprite
    // (trees, tall props) gets it on the trunk instead of floating below it.
    auto draw_selection_ellipse = [&](int sx, int sy, double footprint_w, double footprint_h,
                                      double origin_y, double hp, double max_hp) {
        int rx = std::max(10, static_cast<int>(footprint_w * 0.55 * cam_.zoom));
        int ry = std::max(4, static_cast<int>(rx * 0.35));
        int base_below = static_cast<int>((footprint_h - origin_y) * cam_.zoom);
        int top_above = static_cast<int>(origin_y * cam_.zoom);
        int ex = sx, ey = sy + base_below - ry / 2;
        draw_ellipse(renderer, ex + 1, ey + 1, rx, ry, {0, 0, 0, 255});
        draw_ellipse(renderer, ex, ey, rx, ry, {255, 255, 255, 255});
        double frac = max_hp > 0 ? std::clamp(hp / max_hp, 0.0, 1.0) : 0.0;
        draw_health_bar(ex, sy - top_above - 12, std::max(24, rx * 2), frac);
    };
    auto draw_selection_rect = [&](int sx, int sy, double footprint_w, double footprint_h,
                                   double hp, double max_hp) {
        // A couple px of padding so the rectangle sits just outside the
        // sprite's edge rather than exactly tracing it (was 6 -- read as a
        // ~12px gap since it's added on every side).
        constexpr int kPad = 2;
        int half_w = std::max(8, static_cast<int>(footprint_w * 0.5 * cam_.zoom)) + kPad;
        int half_h = std::max(8, static_cast<int>(footprint_h * 0.5 * cam_.zoom)) + kPad;
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_Rect black{sx - half_w + 1, sy - half_h + 1, half_w * 2, half_h * 2};
        SDL_RenderDrawRect(renderer, &black);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_Rect white{sx - half_w, sy - half_h, half_w * 2, half_h * 2};
        SDL_RenderDrawRect(renderer, &white);
        double frac = max_hp > 0 ? std::clamp(hp / max_hp, 0.0, 1.0) : 0.0;
        draw_health_bar(sx, sy - half_h - 12, std::max(24, half_w * 2), frac);
    };

    // Persistent corpses, vehicle wrecks and building rubble: drawn on the
    // ground, UNDER every unit/building/resource (game/session.py "corpses +
    // rubble under everything"), so a fresh unit walking over a battlefield
    // steps on top of the bodies rather than being hidden behind them.
    for (auto& fx : effects_) {
        if (!fx.ground || !in_view(fx.x, fx.y)) continue;
        int sx, sy;
        cam_.world_to_screen(fx.x, fx.y, sx, sy);
        const auto* m = atlas_.meta(fx.sprite);
        // fx.scale (footprint px) fills a destroyed building's footprint,
        // exactly like the live building did; units draw at native size.
        double sc = (fx.scale > 0.0 && m && m->fw > 0) ? fx.scale * cam_.zoom / m->fw : cam_.zoom;
        if (fx.shrink > 0.0) sc *= 1.0 - fx.shrink * std::clamp(fx.t / fx.lifetime, 0.0, 1.0);
        Uint8 alpha = 255;
        if (fx.fade > 0.0) {
            double rem = fx.lifetime - fx.t;
            if (rem < fx.fade) alpha = static_cast<Uint8>(std::clamp(rem / fx.fade, 0.0, 1.0) * 255);
        }
        atlas_.draw(fx.sprite, sx, sy, fx.frame, sc, 0.0, fx.flip, alpha);
    }

    // Ground-clutter depth sort: resources, buildings, and units are all
    // drawn in ONE pass ordered by each entity's ground-contact world Y
    // (painter's algorithm), instead of three separate fixed-order passes
    // (all resources, then all buildings, then all units). With separate
    // passes, a unit standing north of (behind) a tall tree would still
    // always draw on top of it since units always drew last -- and trees
    // themselves weren't ordered against each other at all, so a tree
    // further south could render behind one further north. The sort key
    // is common.y + (sprite fh - oy): for a center-pivoted sprite (units,
    // buildings, round resources -- oy == fh/2) that's the sprite's visual
    // bottom edge; for a base-anchored sprite (trees -- oy near fh, close
    // to the trunk) it's ~= common.y itself, i.e. where it actually touches
    // the ground either way.
    struct DrawEntry {
        double base_y;
        std::function<void()> draw;
    };
    std::vector<DrawEntry> clutter;
    clutter.reserve(world.active_resources.size() + world.active_buildings.size() +
                    world.active_units.size());

    // Fog of war (game/world.py: World.fog, 0/1/2 = unexplored/explored/
    // visible). Own entities (team 0) are always drawn. Non-own entities
    // are hidden entirely on an unexplored tile; units additionally stay
    // hidden on a merely-explored (not currently visible) tile, while
    // buildings/resources persist once explored -- matches session.py's
    // draw() fog check.
    auto fog_hides = [&](int team, double x, double y, bool is_unit) {
        if (team == 0) return false;
        int f = world.fog_at(x, y);
        return f == 0 || (f == 1 && is_unit);
    };

    for (auto ref : world.active_resources) {
        Resource* r = world.get_resource(ref);
        if (!r || !r->common.alive || r->sprite.empty() || !in_view(r->common.x, r->common.y)) continue;
        if (fog_hides(r->common.team, r->common.x, r->common.y, /*is_unit=*/false)) continue;
        const auto* m = atlas_.meta(r->sprite);
        double fh = m ? m->fh : 32.0, oy = m ? m->oy : fh / 2.0;
        clutter.push_back({r->common.y + (fh - oy), [&, ref, r, m]() {
            // Snap each resource to its tile centre. Short resources (oil/iron/
            // berry, 1 tile) draw fit INSIDE a single tile so they never spill
            // onto neighbours. Tall ones (trees/palms, a 32x64 sprite) keep
            // their natural two-tile height, drawn on their own pivot so the
            // trunk BASE sits at the tile centre -- the canopy is allowed to
            // rise over the tile above, which reads correctly for a tree.
            double tcx = (std::floor(r->common.x / TILE) + 0.5) * TILE;
            double tcy = (std::floor(r->common.y / TILE) + 0.5) * TILE;
            int sx, sy;
            cam_.world_to_screen(tcx, tcy, sx, sy);
            bool tall = m && m->fh > TILE + 1;
            // obj_unit/Step.gml: once a tree/palm drops below 95% of its
            // starting amount, it switches to its "cut down" frame 1 for
            // the rest of its life (frame 0 = full tree). Other resource
            // kinds only have a single frame, so this is a no-op for them.
            int frame = 0;
            if (m && m->frames > 1 && r->res.start_amount > 0 &&
                r->res.amount < r->res.start_amount * 0.95) {
                frame = 1;
            }
            // Fish are an 8-frame swim animation whose FRAME 0 IS BLANK -- drawn
            // at frame 0 they render as nothing (this is why shoals were
            // invisible). Animate through the real frames (1..frames-1) on the
            // render clock so a shoal is both visible and gently swimming.
            if (m && r->name == "fish" && m->frames > 1) {
                frame = 1 + (static_cast<int>(render_clock_ * 4.0 +
                                              (r->common.x + r->common.y) * 0.05) %
                             (m->frames - 1));
            }
            if (tall) {
                if (is_selected(ref)) {
                    draw_selection_ellipse(sx, sy, m ? m->fw : 32, m ? m->fh : 32,
                                           m ? m->oy : 16.0, r->common.hp, r->common.max_hp);
                }
                atlas_.draw(r->sprite, sx, sy, frame, cam_.zoom); // native, base on tile centre
            } else {
                int ts = std::max(1, static_cast<int>(std::lround(TILE * cam_.zoom)));
                SDL_Rect dst{sx - ts / 2, sy - ts / 2, ts, ts};
                if (is_selected(ref)) {
                    // Drawn centred in a tile-sized rect, so pivot is ts/2.
                    draw_selection_ellipse(sx, sy, ts, ts, ts / 2.0, r->common.hp, r->common.max_hp);
                }
                atlas_.draw_in_rect(dst, r->sprite, frame, /*pad=*/0);
            }
        }});
    }
    // Persistent fire hazards (World::fires) -- distinct from the brief
    // cosmetic "spr_flame" Effect pushed alongside each one (see
    // spawn_missile_impact_fx): this drawing loop lasts exactly as long as
    // the hazard actually keeps burning units (its real gameplay duration),
    // not the Effect's few-frame cosmetic flash. Looping frame cycle rather
    // than effects_'s clamp-at-last-frame, since a fire should keep
    // flickering the whole time it exists rather than freeze on one frame.
    for (auto& fire : world.fires) {
        if (!in_view(fire.x, fire.y)) continue;
        const auto* m = atlas_.meta("spr_flame");
        clutter.push_back({fire.y, [&, m]() {
            int sx, sy;
            cam_.world_to_screen(fire.x, fire.y, sx, sy);
            int frame = (m && m->frames > 0)
                          ? static_cast<int>(fire.timer * effect_fps("spr_flame")) % m->frames
                          : 0;
            atlas_.draw("spr_flame", sx, sy, frame, cam_.zoom);
        }});
    }
    // Static decorations (World::decorations, see decoration_kinds()) --
    // terrain-like clutter authored via a campaign level's TerrainFeature::
    // resource, same visibility rule as the base terrain tiles themselves
    // (drawn unconditionally, no fog check): they're environmental set
    // dressing, not a team-owned entity a player could have "not
    // discovered yet" the way a unit/building/resource is.
    for (auto& dec : world.decorations) {
        if (!in_view(dec.x, dec.y)) continue;
        auto it = decoration_kinds().find(dec.kind);
        if (it == decoration_kinds().end()) continue;
        const std::string& sprite = it->second.sprite;
        int frame = it->second.frame;
        const auto* m = atlas_.meta(sprite);
        double fh = m ? m->fh : 32.0, oy = m ? m->oy : fh / 2.0;
        clutter.push_back({dec.y + (fh - oy), [&, sprite, frame]() {
            int sx, sy;
            cam_.world_to_screen(dec.x, dec.y, sx, sy);
            atlas_.draw(sprite, sx, sy, frame, cam_.zoom);
        }});
    }
    // Map-authored message triggers (World::message_triggers) -- same
    // "environmental set dressing, no fog check" treatment as the
    // decorations just above, drawn with the same spr_message icon the
    // campaign editor's Events tab uses so it's recognizable as the same
    // thing. Still drawn after it's fired once (`triggered`) -- there's no
    // in-match way to remove it, same as a decoration.
    for (auto& trig : world.message_triggers) {
        if (trig.triggered || !in_view(trig.x, trig.y)) continue; // vanish once activated, like resource pickups
        const auto* m = atlas_.meta("spr_message");
        double fh = m ? m->fh : 32.0, oy = m ? m->oy : fh / 2.0;
        clutter.push_back({trig.y + (fh - oy), [&]() {
            int sx, sy;
            cam_.world_to_screen(trig.x, trig.y, sx, sy);
            atlas_.draw("spr_message", sx, sy, 0, cam_.zoom);
        }});
    }
    // Map-authored resource pickups (World::resource_pickups) -- a yellow-tinted
    // marker the player walks a unit onto to collect. Unlike message triggers it
    // vanishes once collected (`fired`), because touching it removes it.
    for (auto& p : world.resource_pickups) {
        if (p.fired || !in_view(p.x, p.y)) continue;
        const auto* m = atlas_.meta("spr_message");
        double fh = m ? m->fh : 32.0, oy = m ? m->oy : fh / 2.0;
        clutter.push_back({p.y + (fh - oy), [this, px = p.x, py = p.y]() {
            int sx, sy;
            cam_.world_to_screen(px, py, sx, sy);
            atlas_.draw("spr_message", sx, sy, 0, cam_.zoom, 0.0, false, 255, {255, 230, 60, 255});
        }});
    }
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive || b->sprite.empty() || !in_view(b->common.x, b->common.y)) continue;
        if (fog_hides(b->common.team, b->common.x, b->common.y, /*is_unit=*/false)) continue;
        const auto* m = atlas_.meta(b->sprite);
        double fh = m ? m->fh : b->foot_h, oy = m ? m->oy : fh / 2.0;
        // The footprint's true CENTER (== common.x/y for most buildings,
        // but not tower/aa tower/outpost -- see World::footprint_dy):
        // selection rect/HP bar/icon/badge below deliberately track the
        // small base-tile footprint, not the visual sprite, so a tower's
        // selection box hugs its base the same way its click hit-test does.
        double foot_cy = b->common.y + footprint_dy(b->name);
        // Farms are the one building units actually stand ON (they're
        // walkable -- see spawn_building's b.solid), so the usual bottom-
        // edge y-sort key can't be trusted to keep a working villager drawn
        // on top of it: the villager walks to the farm's exact centre,
        // which sits ABOVE the farm sprite's own bottom edge, so the normal
        // key would draw the farm over them. Sort farms far below every
        // other key instead (mirrors the +1e9 trick aircraft use to always
        // draw above everything, just inverted) so a farm always renders
        // under any unit near/on it.
        double sort_key = (b->name == "farm") ? -1.0e9 : b->common.y + (fh - oy);
        clutter.push_back({sort_key, [&, ref, b, foot_cy]() {
            int sx, sy;
            cam_.world_to_screen(b->common.x, b->common.y, sx, sy);
            int ssx, ssy;
            cam_.world_to_screen(b->common.x, foot_cy, ssx, ssy);
            if (is_selected(ref)) {
                draw_selection_rect(ssx, ssy, b->foot_w, b->foot_h, b->common.hp, b->common.max_hp);
                int half_w = std::max(8, static_cast<int>(b->foot_w * 0.5 * cam_.zoom));
                int half_h = std::max(8, static_cast<int>(b->foot_h * 0.5 * cam_.zoom));
                draw_group_badge(ref, ssx + half_w + 2, ssy - half_h);
            }
            // Shudder when struck: a brief horizontal jitter that decays
            // with hit_timer (game/session.py's `shk` -- buildings only).
            int shk = 0;
            if (b->hit_timer > 0.0) {
                shk = static_cast<int>(std::sin(SDL_GetTicks() / 18.0) * 4.0 * cam_.zoom *
                                       (b->hit_timer / 0.35));
            }
            if (b->complete) {
                int frame = (b->common.team >= 0) ? world.control.teams[b->common.team].colour : 0;
                std::string bspr = b->sprite;
                // Houses re-skin to the owner's CURRENT age so every house on
                // the map updates when the player advances. Japan (civ 4) and
                // China (civ 7) use an Asian Victorian house in the first era.
                if (b->name == "house" && b->common.team >= 0) {
                    const auto& tm = world.control.teams[b->common.team];
                    int era = std::clamp(tm.era, 0, 3);
                    // Japan (civ 4) and China (civ 7) get the Asian house line;
                    // everyone else the default. Both are 8-frame team-coloured,
                    // so frame stays = team colour.
                    if (tm.civ == 4 || tm.civ == 7) {
                        static const char* kAsian[4] = {"spr_asian_house", "spr_asian_house1",
                                                        "spr_asian_house2", "spr_asian_house3"};
                        bspr = kAsian[era];
                    } else {
                        static const char* kHouse[4] = {"spr_house", "spr_house1",
                                                        "spr_house2", "spr_house3"};
                        bspr = kHouse[era];
                    }
                }
                // Germany (civ 2) fields its own barracks design.
                if (b->name == "barracks" && b->common.team >= 0 &&
                    world.control.teams[b->common.team].civ == 2 &&
                    atlas_.meta("spr_barracks_german")) {
                    bspr = "spr_barracks_german";
                }
                // Outpost re-skins to the owner's current age (war-era look from
                // the war era onward) so existing outposts update on age-up.
                if (b->name == "outpost" && b->common.team >= 0) {
                    bspr = world.control.teams[b->common.team].era >= 2 ? "spr_outpost_war_era"
                                                                        : "spr_outpost_victorian_era";
                }
                // The farm is a 2-frame sprite (not team-coloured): frame 0 is
                // the green, growing/harvestable field; frame 1 is the withered
                // "dead farm" shown once it's exhausted (0 food).
                if (b->name == "farm") frame = b->exhausted ? 1 : 0;
                atlas_.draw(bspr, sx + shk, sy, frame, cam_.zoom);
            } else if (b->name == "farm") {
                // A farm under construction shows the withered "dead farm"
                // sprite (spr_farm frame 1) -- a ploughed but unsown field --
                // instead of a generic scaffold, so it reads as a field being
                // prepared. It flips to the green frame 0 once complete (above).
                atlas_.draw(b->sprite, sx + shk, sy, 1, cam_.zoom);
            } else {
                auto [fspr, ffr] = foundation_sprite(*b);
                // tower/aa tower's own dedicated scaffold (spr_2x2small_
                // construction, see foundation_sprite) already sits low
                // enough to match their base-anchored footprint (World::
                // footprint_dy) -- its manifest oy/fh happens to put the
                // same amount below the anchor as their completed sprite
                // does. Outpost is ALSO base-anchored but falls through to
                // the generic, anchor-CENTERED spr_1x1construction (shared
                // with every other 1-tile building's foundation, which
                // isn't base-anchored), so its foundation renders
                // footprint_dy(name) px too high relative to where the
                // actual footprint -- and the completed building -- sits.
                // Nudge it down to match instead of drawing it centered.
                int fdy = 0;
                if (fspr == "spr_1x1construction" && footprint_dy(b->name) > 0.0) {
                    fdy = static_cast<int>(std::lround(footprint_dy(b->name) * cam_.zoom));
                }
                atlas_.draw(fspr, sx + shk, sy + fdy, ffr, cam_.zoom);
            }
            // Airbase nuke stockpile: an atomic-bomb icon + "xN" above the
            // airbase whenever it's holding at least one nuke (own team only).
            if (b->name == "airbase" && b->nuke_count > 0 && b->common.team == 0) {
                int isz = std::max(12, static_cast<int>(16 * cam_.zoom));
                int iy = ssy - static_cast<int>(b->foot_h * 0.5 * cam_.zoom) - isz - ui(4);
                SDL_Rect ir{ssx - isz, iy, isz, isz};
                atlas_.draw_in_rect(ir, "spr_atomic_bomb_icon", 0, /*pad=*/0);
                char cnt[8];
                std::snprintf(cnt, sizeof(cnt), "x%d", b->nuke_count);
                text_.draw(cnt, ssx + 2, iy + isz / 2 - ui(6), {255, 230, 60, 255}, ui(12));
            }
            // Damage flash: float an HP bar over a building hit in the last ~1s
            // (unless it's selected, which already shows one).
            if (b->common.dmg_flash > 0.0 && !is_selected(ref)) {
                double f = b->common.max_hp > 0 ? std::clamp(b->common.hp / b->common.max_hp, 0.0, 1.0) : 0.0;
                int bw = std::max(18, static_cast<int>(b->foot_w * 0.7 * cam_.zoom));
                int bh = std::max(3, static_cast<int>(4 * cam_.zoom));
                int by = ssy - static_cast<int>(b->foot_h * 0.5 * cam_.zoom) - static_cast<int>(7 * cam_.zoom);
                SDL_SetRenderDrawColor(renderer, 25, 25, 25, 220);
                SDL_Rect bg{ssx - bw / 2, by, bw, bh};
                SDL_RenderFillRect(renderer, &bg);
                SDL_Color col = f > 0.5 ? SDL_Color{60, 220, 60, 255}
                                : f > 0.25 ? SDL_Color{230, 200, 40, 255}
                                           : SDL_Color{220, 50, 50, 255};
                SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
                SDL_Rect fg{ssx - bw / 2, by, static_cast<int>(bw * f), bh};
                SDL_RenderFillRect(renderer, &fg);
            }
        }});
    }
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (!u || !u->common.alive || u->sprite.empty() || !in_view(u->common.x, u->common.y)) continue;
        if (u->carrier.valid()) continue; // garrisoned inside a transport: hidden
        if (fog_hides(u->common.team, u->common.x, u->common.y, /*is_unit=*/true)) continue;
        auto [rx, ry] = interp(u->common.id, u->common.x, u->common.y, prev_unit_pos_);
        const auto* m = atlas_.meta(u->sprite);
        double fh = m ? m->fh : 32.0, oy = m ? m->oy : fh / 2.0;
        // Aircraft always draw above every ground unit/building/resource
        // (session.py's draw sort: `(1 if is_air else 0, y)`) -- offsetting
        // the sort key by a constant far larger than any real base_y value
        // keeps them in their own trailing "layer" while still sorting
        // correctly against each other by y.
        double sort_key = ry + (fh - oy);
        if (u->common.is_air) sort_key += 1.0e9;
        // Vertical velocity sign this frame, purely for the climb/level
        // sprite swap below (objects/obj_unit/Step.gml: a distinct "_away"
        // sprite variant while vspeed<0) -- derived from the same prev-
        // tick snapshot used for render interpolation rather than a new
        // sim-side field, since it's cosmetic only.
        double dy = 0.0;
        if (u->common.is_air) {
            auto it = prev_unit_pos_.find(u->common.id);
            if (it != prev_unit_pos_.end()) dy = u->common.y - it->second.y;
        }
        clutter.push_back({sort_key, [&, ref, u, rx = rx, ry = ry, m, dy]() {
            int sx, sy;
            cam_.world_to_screen(rx, ry, sx, sy);
            int frame = (u->common.team >= 0) ? world.control.teams[u->common.team].colour : 0;

            if (u->landed) {
                // Parked at the airbase: small icon (fallback to the full
                // sprite if there's no dedicated "_small" variant), a blue
                // refuel gauge above it, and a plain selection ring
                // instead of the usual ellipse -- matches the original's
                // visually distinct "parked" look (session.py's landed
                // branch).
                std::string base = u->name;
                for (auto& c : base) if (c == ' ') c = '_';
                std::string small = "spr_" + base + "_small";
                // The heavy bomber has no dedicated "_small" parked sprite, so it
                // borrows the regular bomber's small top-down sprite.
                if (u->name == "heavy bomber") small = "spr_bomber_small";
                // Jet planes have no "_small" parked sprite yet -- borrow the
                // fighter's small top-down icon for now.
                if (u->name == "jet fighter" || u->name == "me262") small = "spr_fighter_small";
                if (!atlas_.meta(small)) small = u->sprite;
                if (is_selected(ref)) {
                    int r = static_cast<int>(13 * cam_.zoom);
                    draw_ellipse(renderer, sx, sy, r, r, {60, 255, 60, 255});
                }
                atlas_.draw(small, sx, sy, frame, cam_.zoom * 0.85, 0.0, u->facing < 0);
                int fw = static_cast<int>(18 * cam_.zoom);
                double ff = u->fuel_max > 0 ? std::clamp(u->fuel / u->fuel_max, 0.0, 1.0) : 0.0;
                int fy = sy - static_cast<int>(14 * cam_.zoom);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_Rect fbg{sx - fw / 2, fy, fw, 3};
                SDL_RenderFillRect(renderer, &fbg);
                SDL_SetRenderDrawColor(renderer, 70, 150, 255, 255);
                SDL_Rect ffg{sx - fw / 2, fy, static_cast<int>(fw * ff), 3};
                SDL_RenderFillRect(renderer, &ffg);
                return;
            }

            std::string spname = animated_sprite(*u);
            if (u->common.is_air && dy < -0.5) { // climbing -> distinct "_away" variant if one exists
                std::string away = spname + "_away";
                if (atlas_.meta(away)) spname = away;
            }
            // Built-in unit art faces RIGHT (mirrored when facing < 0). Uploaded
            // campaign custom sprites (name prefix "cu_") are authored facing LEFT,
            // so their mirror is inverted -- flip when facing right instead.
            bool body_flip = (u->facing < 0);
            if (spname.rfind("cu_", 0) == 0) body_flip = !body_flip;
            // The PACKED/mobile ballistic launcher sprite (also shown mid pack/
            // unpack) is authored facing LEFT -- opposite the deployed launcher's
            // right-facing art -- so its mirror is inverted, matching the packed-
            // sprite condition in animated_sprite(). (Deployed launcher unaffected.)
            if (u->is_ballistic && (u->packed || u->pack_t > 0.0)) body_flip = !body_flip;

            if (u->common.is_air) {
                // Shadow: the SAME sprite silhouette (not a plain ellipse),
                // frame 0 always, tinted black at ~50% alpha, drawn at the
                // true ground position -- direct port of the original's
                // draw_sprite_ext(sprite_index, 0, x, y, ..., c_black, 0.5).
                // The colored body then lifts by up to Unit::height (0..64
                // native px, i.e. up to a full sprite-height's worth) as it
                // ramps through takeoff/landing -- see aircraft_behavior.
                // cpp's comment on why the sim keeps common.x/y grounded
                // and only this render position rises.
                atlas_.draw(spname, sx, sy, /*frame=*/0, cam_.zoom, 0.0, body_flip, /*alpha=*/128,
                           {0, 0, 0, 255});
                sy -= static_cast<int>(u->height * cam_.zoom);
            }

            if (is_selected(ref)) {
                double efh = m ? m->fh : 32.0;
                // Ships fill their whole frame, so a feet-at-the-bottom ellipse
                // sits at the keel, well under the hull -- raise it toward the
                // waterline (~72% down) so it hugs the hull instead.
                double eoy = u->common.is_ship ? efh * 0.72 : (m ? m->oy : efh / 2.0);
                draw_selection_ellipse(sx, sy, m ? m->fw : 32, efh, eoy, u->common.hp, u->common.max_hp);
                int rx = std::max(10, static_cast<int>((m ? m->fw : 32) * 0.55 * cam_.zoom));
                draw_group_badge(ref, sx + rx, sy - rx);
            }

            // Propellers removed for now (user request) -- planes draw without
            // the spr_propeller overlay. plane_props/plane_muzzle stay defined
            // for the nose-muzzle geometry; just nothing draws a rotor.
            atlas_.draw(spname, sx, sy, frame, cam_.zoom, 0.0, body_flip);

            // Ballistic missile pack/unpack progress: a blue bar above the unit
            // that fills as it transitions toward the other state (0 -> 1 over
            // the 5s pack/unpack), so the player sees how close it is.
            if (u->is_ballistic && u->pack_t > 0.0) {
                double prog = std::clamp(1.0 - u->pack_t / 5.0, 0.0, 1.0);
                int bw = std::max(20, static_cast<int>((m ? m->fw : 48) * 0.5 * cam_.zoom));
                int bh = std::max(3, static_cast<int>(4 * cam_.zoom));
                int by = sy - static_cast<int>((m ? m->oy : 40) * cam_.zoom) - static_cast<int>(6 * cam_.zoom);
                SDL_SetRenderDrawColor(renderer, 10, 10, 20, 235);
                SDL_Rect bg{sx - bw / 2, by, bw, bh};
                SDL_RenderFillRect(renderer, &bg);
                SDL_SetRenderDrawColor(renderer, 70, 150, 255, 255);
                SDL_Rect fg{sx - bw / 2, by, static_cast<int>(bw * prog), bh};
                SDL_RenderFillRect(renderer, &fg);
                SDL_SetRenderDrawColor(renderer, 200, 220, 255, 255);
                SDL_RenderDrawRect(renderer, &bg);
            }

            // Carried-resource icon: a gatherer hauling a load back to a
            // dropoff shows a small resource icon over its head (spr_resource_
            // carry frame = carry_type) so you can see what it's carrying while
            // it walks. Only while it actually has something on it.
            // Damage flash: float a small HP bar above any unit that was hit in
            // the last ~1s (World::dmg_flash), even when it isn't selected, so
            // you can see what's taking fire. Selected units already show HP via
            // their selection ring, so skip the double-up there.
            if (u->common.dmg_flash > 0.0 && !is_selected(ref)) {
                double f = u->common.max_hp > 0 ? std::clamp(u->common.hp / u->common.max_hp, 0.0, 1.0) : 0.0;
                int bw = std::max(14, static_cast<int>((m ? m->fw : 32) * 0.7 * cam_.zoom));
                int bh = std::max(2, static_cast<int>(3 * cam_.zoom));
                int by = sy - static_cast<int>((m ? m->oy : 24) * cam_.zoom) - static_cast<int>(6 * cam_.zoom);
                SDL_SetRenderDrawColor(renderer, 25, 25, 25, 220);
                SDL_Rect bg{sx - bw / 2, by, bw, bh};
                SDL_RenderFillRect(renderer, &bg);
                SDL_Color col = f > 0.5 ? SDL_Color{60, 220, 60, 255}
                                : f > 0.25 ? SDL_Color{230, 200, 40, 255}
                                           : SDL_Color{220, 50, 50, 255};
                SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
                SDL_Rect fg{sx - bw / 2, by, static_cast<int>(bw * f), bh};
                SDL_RenderFillRect(renderer, &fg);
            }
            // Carried resource: shown ONLY while the villager is WALKING a load
            // back to a dropoff (carry > 0 and not actively harvesting) -- it
            // vanishes while it's working a resource. Drawn directly over the
            // arms (front-of-body, ~40% up), 33% bigger than the head marker.
            if (u->is_gatherer && u->carry > 0.0 && !u->working) {
                int ic = std::max(14, static_cast<int>(23 * cam_.zoom));
                int fsign = u->facing < 0 ? -1 : 1;
                int armx = sx + static_cast<int>(4 * cam_.zoom) * fsign;
                // Held a touch lower (nearer the hands) than before -- was 0.4.
                int army = sy - static_cast<int>((m ? m->fh * 0.28 : 9) * cam_.zoom);
                SDL_Rect crect{armx - ic / 2, army - ic / 2, ic, ic};
                atlas_.draw_in_rect(crect, "spr_resource_carry", u->carry_type, /*pad=*/0);
            }

            // Idle-villager marker: own civilians with no order of any kind
            // flash a red exclamation mark above their head, same idea as
            // an AoE-style idle-villager indicator -- makes economy
            // neglect visible without having to click through every
            // civilian. Same test the "." hotkey cycles/selects by (see
            // is_idle_civilian).
            if (is_idle_civilian(*u)) {
                atlas_.draw("spr_idle", sx, sy - static_cast<int>(20 * cam_.zoom), 0, cam_.zoom);
            }

            if (u->common.is_air && u->common.team == 0) {
                // Own-team-only: blue fuel gauge above the plane, plus a
                // pip per remaining bomb for bombers (a pip vanishes each
                // time one is dropped).
                int fw = static_cast<int>(22 * cam_.zoom);
                double ff = u->fuel_max > 0 ? std::clamp(u->fuel / u->fuel_max, 0.0, 1.0) : 0.0;
                int fy = sy - static_cast<int>(22 * cam_.zoom);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_Rect fbg{sx - fw / 2, fy, fw, 3};
                SDL_RenderFillRect(renderer, &fbg);
                SDL_Color col = ff > 0.25 ? SDL_Color{70, 150, 255, 255} : SDL_Color{60, 90, 200, 255};
                SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
                SDL_Rect ffg{sx - fw / 2, fy, static_cast<int>(fw * ff), 3};
                SDL_RenderFillRect(renderer, &ffg);
                if (u->is_bomber) {
                    if (u->nuke_loaded) {
                        // A single BLACK widget marks a nuke aboard (instead of
                        // the orange conventional-bomb pips).
                        int nw = std::max(5, static_cast<int>(7 * cam_.zoom));
                        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                        SDL_Rect pip{sx - nw / 2, fy - static_cast<int>(7 * cam_.zoom), nw, nw};
                        SDL_RenderFillRect(renderer, &pip);
                    } else {
                        for (int bi = 0; bi < u->clip_ammo; ++bi) {
                            SDL_SetRenderDrawColor(renderer, 250, 120, 40, 255);
                            SDL_Rect pip{sx - fw / 2 + bi * 5, fy - static_cast<int>(5 * cam_.zoom), 3, 3};
                            SDL_RenderFillRect(renderer, &pip);
                        }
                    }
                }
            }
        }});
    }
    std::stable_sort(clutter.begin(), clutter.end(),
                     [](const DrawEntry& a, const DrawEntry& b) { return a.base_y < b.base_y; });
    for (auto& entry : clutter) entry.draw();
    for (auto ref : world.active_projectiles) {
        Projectile* p = world.get_projectile(ref);
        if (!p || !p->common.alive || p->sprite.empty() || !in_view(p->common.x, p->common.y)) continue;
        auto [rx, ry] = interp(p->common.id, p->common.x, p->common.y, prev_proj_pos_);
        int sx, sy;
        cam_.world_to_screen(rx, ry, sx, sy);
        // Direct port of obj_missile/Draw.gml: while airborne (z/height >
        // 0 -- artillery/bomb lobs and the ship-shell arc both set this),
        // a flat black half-alpha shadow stays pinned to the true ground
        // position and the real sprite lifts up (and drifts slightly
        // toward the shooter, x -= z/4) above it. z was computed but never
        // actually drawn with before, so every lofted shot rendered
        // perfectly flat/linear regardless of its underlying arc.
        if (p->z > 0) {
            atlas_.draw(p->sprite, sx, sy, 0, cam_.zoom, p->angle, p->flip, 128, {0, 0, 0, 255});
        }
        int zx = sx - static_cast<int>(p->z / 4.0 * cam_.zoom);
        int zy = sy - static_cast<int>(p->z * cam_.zoom);
        // Fighter / jet-fighter tracer rounds render fully black (user request);
        // everything else keeps its natural sprite colour.
        SDL_Color ptint = (p->name == "fighter" || p->name == "jet fighter")
                              ? SDL_Color{0, 0, 0, 255}
                              : SDL_Color{255, 255, 255, 255};
        atlas_.draw(p->sprite, zx, zy, 0, cam_.zoom, p->angle, p->flip, 255, ptint);
    }

    for (auto& fx : effects_) {
        if (fx.ground || !in_view(fx.x, fx.y)) continue; // ground corpses/rubble drawn earlier, under entities
        // spr_arrow (the plain-move waypoint marker) is player-order UI, not
        // a combat effect happening in the world -- drawn in its own pass
        // after the fog overlay instead (see below), same reasoning as the
        // target-flash/rally-point/queued-waypoint markers just past it.
        if (fx.sprite == "spr_arrow") continue;
        int sx, sy;
        cam_.world_to_screen(fx.x, fx.y, sx, sy);
        // Step through the sprite's frames at its own effect_fps instead of
        // freezing on fx.frame (always 0) -- explosions/shockwaves/flames
        // are genuine multi-frame animations that never actually animated
        // before. Single-frame effect sprites are unaffected (frames=1
        // clamps this to 0). Debris/embers hold a fixed frame while they arc.
        int frame = fx.frame;
        if (!fx.fixed_frame) {
            if (const auto* m = atlas_.meta(fx.sprite); m && m->frames > 1) {
                if (fx.sprite == "spr_explosion_mushroom") {
                    // The atomic cloud lingers a few seconds LONGER than its raw
                    // animation (lifetime was extended below). Spread its frames
                    // across that whole lifetime so the cloud keeps billowing/
                    // rising the entire time, instead of racing through the
                    // animation and then FREEZING on the last frame for 2s.
                    double prog = fx.lifetime > 0.0 ? fx.t / fx.lifetime : 1.0;
                    frame = std::min(m->frames - 1, static_cast<int>(prog * m->frames));
                } else {
                    frame = std::min(m->frames - 1, static_cast<int>(fx.t * effect_fps(fx.sprite)));
                }
            }
        }
        Uint8 alpha = 255;
        if (fx.fade > 0.0) {
            double rem = fx.lifetime - fx.t;
            if (rem < fx.fade) alpha = static_cast<Uint8>(std::clamp(rem / fx.fade, 0.0, 1.0) * 255);
        }
        atlas_.draw(fx.sprite, sx, sy, frame, cam_.zoom * fx.draw_scale, 0.0, fx.flip, alpha, fx.tint);
    }

    // obj_smoke puffs: direct port of its own Draw.gml
    // (draw_sprite_ext(sprite_index, image_index, x, y, swell, swell, 0,
    // c_white, image_alpha)) -- scale grows and alpha fades as they rise,
    // unlike every other effect above (see ClientSmoke's comment).
    for (auto& s : smoke_) {
        if (!in_view(s.x, s.y)) continue;
        int sx, sy;
        cam_.world_to_screen(s.x, s.y, sx, sy);
        Uint8 alpha = static_cast<Uint8>(std::clamp(s.alpha, 0.0, 1.0) * 255);
        atlas_.draw("spr_smoke", sx, sy, 0, s.scale * cam_.zoom, 0.0, false, alpha);
    }

    // Fog-of-war darkening overlay (World::fog -- 0/1/2 =
    // unexplored/explored/visible), drawn after all entities/effects so it
    // darkens everything uniformly, same ordering as session.py's fog
    // overlay pass. One rect per TILE-px tile (kFogSubdiv back at 1, matching
    // game/world.py's grid exactly).
    //
    // Drawn as TWO independent rounded layers, not one: an outer grey
    // layer (fog!=2, i.e. explored-or-unexplored) rounded against fully
    // visible, then a solid-black layer (fog==0, unexplored only) rounded
    // against "at least explored" and painted on top, fully opaque, so it
    // overwrites the grey tint wherever it applies with its own separately
    // rounded silhouette. Without this split, unexplored tiles only ever
    // rounded where they happened to touch visible directly (rare, since
    // there's normally an explored ring in between) -- the far more common
    // unexplored-vs-explored boundary stayed a sharp, unrounded staircase.
    //
    // Each layer's corner rounding uses a marching-squares-style check
    // over the 4 tiles touching each vertex (self + the 2 orthogonal
    // neighbors sharing that corner + the diagonal neighbor): a corner
    // only rounds when EXACTLY 1 of those 4 is on the "dark" side of that
    // layer's threshold (convex -- cut into the lone dark tile) or EXACTLY
    // 3 are (concave -- bulge into the lone light tile). That count is a
    // vertex-level invariant (identical no matter which of the 4 tiles
    // computes it), so exactly one tile ever rounds a given vertex within
    // a layer -- never a cut and a bulge colliding at the same point
    // (checking only the 2 orthogonal neighbors, as an earlier version
    // did, missed this and let diagonal/"saddle" patterns produce
    // colliding curves -- the earlier "bleb" artifact).
    //
    // Hard-edged rows throughout (fill_rounded_tile/fill_corner_bulge), no
    // blur -- stays retro.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    int fcols = static_cast<int>(world.fog.size()), frows = static_cast<int>(world.fog[0].size());
    int fx0 = std::max(0, static_cast<int>(vr.x / kFogTilePx));
    int fx1 = std::min(fcols, static_cast<int>((vr.x + vr.w) / kFogTilePx) + 1);
    int fy0 = std::max(0, static_cast<int>(vr.y / kFogTilePx));
    int fy1 = std::min(frows, static_cast<int>((vr.y + vr.h) / kFogTilePx) + 1);
    constexpr int kFogCornerRadius = kFogTilePx / 3;
    auto draw_fog_layer = [&](auto&& is_dark, SDL_Color color) {
        for (int tx = fx0; tx < fx1; ++tx) {
            for (int ty = fy0; ty < fy1; ++ty) {
                bool self_dark = is_dark(tx, ty);
                auto corner = [&](int cdx, int cdy) {
                    int n = (self_dark ? 1 : 0) + (is_dark(tx + cdx, ty) ? 1 : 0) +
                            (is_dark(tx, ty + cdy) ? 1 : 0) + (is_dark(tx + cdx, ty + cdy) ? 1 : 0);
                    return self_dark ? (n == 1) : (n == 3);
                };
                bool tl = corner(-1, -1), tr = corner(1, -1), bl = corner(-1, 1), br = corner(1, 1);
                if (!self_dark && !tl && !tr && !bl && !br) continue;
                int sx, sy, sx1, sy1;
                cam_.world_to_screen(tx * kFogTilePx, ty * kFogTilePx, sx, sy);
                cam_.world_to_screen((tx + 1) * kFogTilePx, (ty + 1) * kFogTilePx, sx1, sy1);
                SDL_Rect dst{sx, sy, std::max(1, sx1 - sx), std::max(1, sy1 - sy)};
                if (self_dark) {
                    fill_rounded_tile(renderer, dst, kFogCornerRadius, color, tl, tr, bl, br);
                } else {
                    if (tl) fill_corner_bulge(renderer, dst, kFogCornerRadius, color, true, true);
                    if (tr) fill_corner_bulge(renderer, dst, kFogCornerRadius, color, true, false);
                    if (bl) fill_corner_bulge(renderer, dst, kFogCornerRadius, color, false, true);
                    if (br) fill_corner_bulge(renderer, dst, kFogCornerRadius, color, false, false);
                }
            }
        }
    };
    auto in_bounds = [&](int tx, int ty) { return tx >= 0 && tx < fcols && ty >= 0 && ty < frows; };
    draw_fog_layer(
        [&](int tx, int ty) { return in_bounds(tx, ty) && world.fog[tx][ty] != 2; },
        SDL_Color{8, 10, 14, 130});
    draw_fog_layer(
        [&](int tx, int ty) { return in_bounds(tx, ty) && world.fog[tx][ty] == 0; },
        SDL_Color{8, 10, 14, 255});

    // Everything below is player-order UI, not something happening in the
    // (possibly unseen) world -- drawn AFTER the fog overlay so it stays
    // visible over fog/darkness instead of being darkened along with
    // everything else, unlike the ordinary world effects_ pass above.

    // The plain-move waypoint arrow (pushed into effects_ as "spr_arrow",
    // skipped in the main effects_ loop above -- see that loop's comment).
    for (auto& fx : effects_) {
        if (fx.sprite != "spr_arrow" || !in_view(fx.x, fx.y)) continue;
        int sx, sy;
        cam_.world_to_screen(fx.x, fx.y, sx, sy);
        Uint8 alpha = 255;
        if (fx.fade > 0.0) {
            double rem = fx.lifetime - fx.t;
            if (rem < fx.fade) alpha = static_cast<Uint8>(std::clamp(rem / fx.fade, 0.0, 1.0) * 255);
        }
        atlas_.draw(fx.sprite, sx, sy, fx.frame, cam_.zoom * fx.draw_scale, 0.0, fx.flip, alpha, fx.tint);
    }

    // Right-click order feedback: a strobing lime marker pinned to the
    // order's target (obj_unit/obj_building/obj_tree's "targeted"
    // countdown + sin(control.age) flicker in the original -- ported as a
    // fixed on/off duty cycle here). Buildings get a rectangle outline
    // (obj_building/Draw.gml), resources a black-then-lime double ellipse
    // (obj_tree/Draw.gml et al), units a lime-only ellipse (obj_unit/
    // Draw.gml). Tracks the target by ref so it follows a moving unit and
    // disappears the instant the target dies, same as the original.
    for (auto& fl : target_flashes_) {
        if (std::fmod(fl.t, 0.3) >= 0.15) continue; // strobe off-phase
        EntityCommon* c = world.common(fl.ref);
        if (!c || !c->alive || !in_view(c->x, c->y)) continue;
        int sx, sy;
        cam_.world_to_screen(c->x, c->y, sx, sy);
        if (fl.ref.kind == EntityKind::Building) {
            Building* fb = world.get_building(fl.ref);
            constexpr int kPad = 4;
            // Outlines the same base-anchored collision footprint the
            // selection rect uses (see World::footprint_dy) -- a tower's
            // repair/rally flash hugs its base tile, not its tall sprite.
            int fsx, fsy;
            cam_.world_to_screen(fb->common.x, fb->common.y + footprint_dy(fb->name), fsx, fsy);
            int hw = std::max(8, static_cast<int>(fb->foot_w * 0.5 * cam_.zoom)) + kPad;
            int hh = std::max(8, static_cast<int>(fb->foot_h * 0.5 * cam_.zoom)) + kPad;
            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
            SDL_Rect r{fsx - hw, fsy - hh, hw * 2, hh * 2};
            SDL_RenderDrawRect(renderer, &r);
        } else {
            std::string sprite =
                fl.ref.kind == EntityKind::Unit ? world.get(fl.ref)->sprite : world.get_resource(fl.ref)->sprite;
            const auto* m = atlas_.meta(sprite);
            int rx = std::max(10, static_cast<int>((m ? m->fw : 32) * 0.5 * cam_.zoom));
            int ry = std::max(5, static_cast<int>(rx * 0.5));
            if (fl.ref.kind == EntityKind::Resource) {
                draw_ellipse(renderer, sx + 1, sy + 1, rx, ry, {0, 0, 0, 255});
            }
            draw_ellipse(renderer, sx, sy, rx, ry, {0, 255, 0, 255});
        }
    }

    // Building rally point: a persistent spr_waypoint marker while its
    // owning building stays selected (obj_show_gather_point -- self-
    // destroys on deselect in the original; here that's just "don't draw
    // it" since there's no separate instance to manage). Only shown once
    // the player has actually right-clicked a rally point (rally_set),
    // not at the building's default just-below-the-footprint gather spot.
    for (auto ref : selected_) {
        Building* rb = world.get_building(ref);
        if (!rb || rb->common.team != 0 || !rb->rally_set) continue;
        if (!in_view(rb->gather_x, rb->gather_y)) continue;
        int sx, sy;
        cam_.world_to_screen(rb->gather_x, rb->gather_y, sx, sy);
        atlas_.draw("spr_waypoint", sx, sy, 0, cam_.zoom);
    }

    // Shift-queued order waypoints: a persistent yellow line + spr_waypoint
    // marker chain for every selected own unit that still has something
    // queued (order_queue non-empty) OR whose current order came from the
    // queue (queue_active -- covers the LAST step: once it's popped,
    // order_queue itself goes empty while that step is still in flight).
    // Checking order_queue directly (not just queue_active) matters
    // because a shift-click can queue up BEHIND a plain, non-queued order
    // that's already in flight (e.g. an ordinary right-click move you
    // shift-click a follow-up onto before it arrives) -- that pending
    // entry has to show immediately, not stay invisible until the
    // in-flight order happens to finish.
    //
    // The chain's first link is whatever order is CURRENTLY executing
    // (move_goal/build_target/gather_target/attack_target), queued or not
    // -- showing where the unit is headed right now alongside what's
    // queued after that gives the player the whole plan, not just the
    // part that hasn't started. A Move step's point is fixed; Gather/
    // Attack/Build steps track their target's CURRENT position, so the
    // chain follows a moving target the same way the live order would.
    for (auto ref : selected_) {
        Unit* qu = world.get(ref);
        if (!qu || qu->common.team != 0 || (qu->order_queue.empty() && !qu->queue_active)) continue;
        double px = qu->common.x, py = qu->common.y;
        double ctx = px, cty = py;
        bool have_current = true;
        if (qu->move_goal) {
            ctx = qu->move_goal->x;
            cty = qu->move_goal->y;
        } else if (EntityRef cur = qu->build_target.valid()   ? qu->build_target
                                    : qu->gather_target.valid() ? qu->gather_target
                                    : qu->attack_target.valid() ? qu->attack_target
                                                                 : kNullRef;
                   cur.valid()) {
            EntityCommon* c = world.common(cur);
            if (!c) { have_current = false; }
            else { ctx = c->x; cty = c->y; }
        } else {
            have_current = false; // between orders this tick -- nothing to draw yet
        }
        auto draw_link = [&](double fx, double fy, double tx, double ty) {
            if (in_view(fx, fy) || in_view(tx, ty)) {
                int sx0, sy0, sx1, sy1;
                cam_.world_to_screen(fx, fy, sx0, sy0);
                cam_.world_to_screen(tx, ty, sx1, sy1);
                SDL_SetRenderDrawColor(renderer, 255, 230, 60, 200);
                SDL_RenderDrawLine(renderer, sx0, sy0, sx1, sy1);
            }
            if (in_view(tx, ty)) {
                int sx, sy;
                cam_.world_to_screen(tx, ty, sx, sy);
                atlas_.draw("spr_waypoint", sx, sy, 0, cam_.zoom * 0.7);
            }
        };
        if (have_current) {
            draw_link(px, py, ctx, cty);
            px = ctx;
            py = cty;
        }
        for (auto& qo : qu->order_queue) {
            double tx = qo.x, ty = qo.y;
            if (qo.kind != QueuedOrderKind::Move) {
                EntityCommon* tc = world.common(qo.target);
                if (!tc || !tc->alive) break; // target's gone -- rest of the chain is unreachable to draw
                tx = tc->x;
                ty = tc->y;
            }
            draw_link(px, py, tx, ty);
            px = tx;
            py = ty;
        }
    }

    // Artillery bombardment target: a persistent red target marker at each
    // selected artillery's standing attack_ground point.
    for (auto ref : selected_) {
        Unit* au = world.get(ref);
        if (!au || au->common.team != 0 || !au->attack_ground) continue;
        if (!in_view(au->attack_ground->x, au->attack_ground->y)) continue;
        int sx, sy;
        cam_.world_to_screen(au->attack_ground->x, au->attack_ground->y, sx, sy);
        atlas_.draw("spr_attack_target_icon", sx, sy, 0, cam_.zoom);
    }

    if (!placing_.empty()) {
        // Ghost snaps to the same grid-aligned spot World::place_building
        // would actually use (world.snap), and checks the same validity
        // rules (world.footprint_clear + world.footprint_explored -- can't
        // place in unexplored/dark fog, but explored/"light" fog is fine)
        // so the preview never lies about where the building will land or
        // whether the click will work -- tinted red when it won't.
        std::string ghost_spr = atlas_.meta("spr_" + placing_) ? "spr_" + placing_ : placing_;
        int era0 = std::clamp(match_.control().teams[0].era, 0, 3);
        int civ0 = match_.control().teams[0].civ;
        if (placing_ == "outpost") {
            // Era-skinned building -> ghost the era-appropriate sprite.
            ghost_spr = era0 >= 2 ? "spr_outpost_war_era" : "spr_outpost_victorian_era";
        }
        if (placing_ == "house") {
            // Houses change look per age -- ghost the era skin (Asian line for
            // Japan/China) so the preview matches what actually gets built.
            if (civ0 == 4 || civ0 == 7) {
                static const char* kAsian[4] = {"spr_asian_house", "spr_asian_house1",
                                                "spr_asian_house2", "spr_asian_house3"};
                ghost_spr = kAsian[era0];
            } else {
                static const char* kHouse[4] = {"spr_house", "spr_house1",
                                                "spr_house2", "spr_house3"};
                ghost_spr = kHouse[era0];
            }
        }
        if (placing_ == "barracks" && civ0 == 2 && atlas_.meta("spr_barracks_german")) {
            ghost_spr = "spr_barracks_german"; // Germany's own barracks skin
        }
        if (placing_ == "base") {
            static const char* kCivBase[9] = {
                "spr_uk_base",    "spr_capitol",     "spr_nazi_base",
                "spr_soviet_base", "spr_japan_base", "spr_italy_base",
                "spr_france_base", "spr_china_base", "spr_ottoman_base"};
            if (civ0 >= 0 && civ0 < 9 && atlas_.meta(kCivBase[civ0])) ghost_spr = kCivBase[civ0];
        }
        if (is_wall(placing_)) {
            // "spr_iron wall" doesn't exist and palisade's real sprite was
            // swapped -- so read the actual in-world sprite straight from the
            // catalog (spr_grey_bricks / spr_wood_icon) rather than guessing.
            const auto& bs = world.data.catalog().at("buildings");
            if (bs.contains(placing_)) {
                std::string ic = bs.at(placing_).value("sprite_index", "");
                if (!ic.empty()) ghost_spr = ic;
            }
        }
        // Segment centres to preview: the whole drag line while a wall drag is
        // in progress, otherwise just the single snapped cell under the cursor.
        std::vector<std::pair<double, double>> ghosts;
        if (is_wall(placing_) && wall_dragging_ && wall_drag_start_) {
            double wx0, wy0, wx1, wy1;
            cam_.screen_to_world(wall_drag_start_->x, wall_drag_start_->y, wx0, wy0);
            cam_.screen_to_world(mouse_pos_.x, mouse_pos_.y, wx1, wy1);
            ghosts = wall_line(wx0, wy0, wx1, wy1);
        } else {
            double wx, wy;
            cam_.screen_to_world(mouse_pos_.x, mouse_pos_.y, wx, wy);
            auto [sx, sy] = world.snap(placing_, wx, wy);
            ghosts.emplace_back(sx, sy);
        }
        for (auto [sx, sy] : ghosts) {
            // Exclude the selected build crew so a villager standing where you're
            // placing (common for a big footprint like a fortress) doesn't red-X
            // the ghost -- matches place_building's own footprint_clear call.
            bool valid = world.footprint_clear(placing_, sx, sy, &selected_) &&
                         world.footprint_explored(placing_, sx, sy);
            int gx, gy;
            cam_.world_to_screen(sx, sy, gx, gy);
            SDL_Color tint = valid ? SDL_Color{255, 255, 255, 255} : SDL_Color{255, 90, 90, 255};
            // Team-colour frame so the ghost previews the player's actual colour
            // (single-frame civ/wall sprites just clamp back to frame 0).
            atlas_.draw(ghost_spr, gx, gy, match_.control().teams[0].colour, cam_.zoom, 0.0, false, 160,
                        tint);
        }
    }

    // drag-select marquee: a black rectangle offset 1px right/down under a
    // white rectangle, so the outline stays visible against any background.
    if (dragging_ && drag_start_) {
        int mx = mouse_pos_.x, my = mouse_pos_.y;
        int ddx = mx - drag_start_->x, ddy = my - drag_start_->y;
        // Same 6px-radius threshold handle_left_up() uses to distinguish a
        // box-select from a plain click -- without this, a stationary click
        // still draws a zero-size (or 1px, once black/white are offset)
        // marquee box for a single frame.
        if (ddx * ddx + ddy * ddy > 36) {
            int rx0 = std::min(drag_start_->x, mx), rx1 = std::max(drag_start_->x, mx);
            int ry0 = std::min(drag_start_->y, my), ry1 = std::max(drag_start_->y, my);
            SDL_Rect black{rx0 + 1, ry0 + 1, rx1 - rx0, ry1 - ry0};
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderDrawRect(renderer, &black);
            SDL_Rect white{rx0, ry0, rx1 - rx0, ry1 - ry0};
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDrawRect(renderer, &white);
        }
    }
    draw_formation_preview(renderer);

    draw_hud(renderer);
    draw_global_queue(renderer);
    draw_objectives(renderer);
    draw_score_hud(renderer);
    draw_notifications(renderer);
    draw_chat(renderer);
    draw_pause_button(renderer);
    draw_command_card(renderer);
    if (show_perf_overlay_) draw_perf_overlay(renderer);
    if (spectator_) draw_spectator_stats(renderer);
    if (paused_ && !pause_menu_open_) {
        std::string label = "PAUSED";
        int size = ui(20);
        int tw, th;
        text_.measure(label, size, tw, th);
        text_.draw(label, (view_w_ - tw) / 2, top_bar_height() + ui(4), {255, 230, 60, 255}, size);
    }
    // Nuke white-out flash over the whole scene (under the pause/stats menus so
    // those stay readable), fading with nuke_flash_t_.
    if (nuke_flash_t_ > 0.0) {
        Uint8 a = static_cast<Uint8>(std::min(1.0, nuke_flash_t_ / 0.7) * 235);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, a);
        SDL_Rect full{0, 0, view_w_, view_h_};
        SDL_RenderFillRect(renderer, &full);
    }
    // Restore the camera from the nuke shake so the offset never accumulates.
    if (shaking) {
        cam_.x = shake_save_x;
        cam_.y = shake_save_y;
    }

    if (game_over_banner_open_) draw_game_over_banner(renderer);
    if (pause_menu_open_) draw_pause_menu(renderer);
    if (stats_open_) draw_stats_screen(renderer);

    // Custom cursor, drawn last so it's always on top of the HUD; OS cursor
    // is hidden once at construction. While attack-move is armed (button
    // clicked, waiting for the target right-click) it swaps to the original
    // GMK "patrol" cursor (attack_move_armed_ IS the ported `patrol` flag
    // from objects/control/Step.gml, which does the same swap) so the click
    // has a visible effect even before the follow-up right-click. Otherwise,
    // hovering a visible (non-fogged) enemy unit swaps it to the attack
    // cursor -- objects/control/Step.gml's mouse_sprite logic, which checks
    // purely "is there an enemy obj_unit under the cursor", independent of
    // what's currently selected.
    const char* cursor = "spr_mouse";
    if (attack_ground_armed_ || unload_armed_) {
        // Red target follows the cursor while aiming a bombardment OR aiming a
        // transport unload (click a shoreline to disgorge the cargo) -- the
        // player needs the same "pick a spot" feedback for both.
        cursor = "spr_attack_target_icon";
    } else if (attack_move_armed_) {
        cursor = "spr_patrol_icon";
    } else if (mouse_pos_.y < view_h_ - panel_height()) {
        double hwx, hwy;
        cam_.screen_to_world(mouse_pos_.x, mouse_pos_.y, hwx, hwy);
        if (world.fog_at(hwx, hwy) != 0) {
            for (auto ref : world.active_units) {
                Unit* hu = world.get(ref);
                if (hu && hu->common.alive && hu->common.team != 0 && hu->common.team >= 0 &&
                    dist2(hwx, hwy, hu->common.x, hu->common.y) < 20 * 20) {
                    cursor = "spr_attack_icon";
                    break;
                }
            }
        }
    }
    atlas_.draw(cursor, mouse_pos_.x, mouse_pos_.y, 0, kUiScale);
}

void GameClient::draw_hud(SDL_Renderer* renderer) {
    // view_team(), not 0: this is the team whose economy the HUD describes.
    // Hardcoding 0 was also wrong in multiplayer -- the joiner is team 1 and
    // was shown the host's resources, population and era.
    Team& team = match_.control().teams[view_team()];
    int tbar_h = top_bar_height(); // 1 unit (32 native px) tall
    SDL_SetRenderDrawColor(renderer, 10, 12, 18, 230);
    SDL_Rect bar{0, 0, view_w_, tbar_h};
    SDL_RenderFillRect(renderer, &bar);

    auto res = [&](const char* k) { auto it = team.res.find(k); return it == team.res.end() ? 0.0 : it->second; };
    // Based on the original GML layout (assets/gmk/objects/control/Draw.gml
    // lines 601-630) at the 640-native-px HUD width: each resource is a
    // fixed cell (32px icon + number slot starting right after it),
    // population starts one cell-gap after the last resource, and the era
    // name/icons are positioned by fraction of the bar width. The original
    // used a 32px-wide number slot (64px cells); widened to 40px (72px
    // cells) since 4-digit resource counts slightly overran 32px.
    constexpr int kNumSlot = 40;
    constexpr int kCellW = 32 + kNumSlot;
    // 16 exactly matches one of serife.fon's embedded bitmap strikes (see
    // TextRenderer::font's kStrikes) -- other sizes snap to the nearest
    // strike anyway, so this picks the crispest match for this bar height.
    int text_size = ui(16);

    auto draw_icon_num = [&](int native_x, const char* icon_name, int frame, const std::string& s) {
        SDL_Rect icon_rect{ui(native_x), 0, tbar_h, tbar_h};
        atlas_.draw_in_rect(icon_rect, icon_name, frame, /*pad=*/0);
        int tw, th;
        text_.measure(s, text_size, tw, th);
        text_.draw(s, ui(native_x + 32), (tbar_h - th) / 2, {255, 255, 255, 255}, text_size);
    };

    struct ResEntry { int frame; const char* key; };
    ResEntry entries[4] = {{0, "food"}, {1, "wood"}, {2, "oil"}, {3, "iron"}};
    for (int i = 0; i < 4; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f", res(entries[i].key));
        draw_icon_num(i * kCellW, "spr_resources", entries[i].frame, buf);
    }

    char pop_buf[16];
    std::snprintf(pop_buf, sizeof(pop_buf), "%d/%d", team.pop, team.cap);
    draw_icon_num(4 * kCellW + 32, "spr_population_icon", 0, pop_buf);

    static const char* kEraNames[4] = {"Victorian Era", "Industrial Era", "War Era", "Scientific Era"};
    int era_idx = std::clamp(team.era, 0, 3);
    std::string era_name = kEraNames[era_idx];
    int etw, eth;
    text_.measure(era_name, text_size, etw, eth);
    // +32 native px on top of the original fraction-based placement, to
    // widen the gap between the population text and the era section.
    int era_gap_extra = ui(32);
    int era_icon1_x = static_cast<int>(view_w_ * 0.6) + era_gap_extra;
    int era_text_x = static_cast<int>(view_w_ * 0.65) + era_gap_extra;
    SDL_Rect era_rect1{era_icon1_x, 0, tbar_h, tbar_h};
    atlas_.draw_in_rect(era_rect1, "spr_era_icon", era_idx, /*pad=*/0);
    text_.draw(era_name, era_text_x, (tbar_h - eth) / 2, {255, 255, 255, 255}, text_size);
    SDL_Rect era_rect2{era_text_x + etw, 0, tbar_h, tbar_h};
    atlas_.draw_in_rect(era_rect2, "spr_era_icon", era_idx, /*pad=*/0);

    // bottom panel background -- tan, matching the reference design (was black)
    constexpr SDL_Color kPanelColor{213, 185, 172, 255};
    int panel_top = view_h_ - panel_height();
    SDL_SetRenderDrawColor(renderer, kPanelColor.r, kPanelColor.g, kPanelColor.b, kPanelColor.a);
    SDL_Rect panel{0, panel_top, view_w_, panel_height()};
    SDL_RenderFillRect(renderer, &panel);

    // Stats column starts well clear of the command-card buttons, which all
    // live on the left side of the panel now (see draw_command_card).
    int col_x = stats_col_x();
    if (selected_.size() > 1) {
        // Multi-select: the flag still shows normally (same as a single
        // selection, keyed off selected_[0]'s team), but instead of one
        // unit's detailed stats (which would arbitrarily just describe
        // selected_[0]), the space to its right shows one icon per distinct
        // unit type in the selection with a count badge -- e.g. 3 civilians
        // + 1 cavalry draws the civilian icon+"3" then the cavalry icon+"1"
        // beside it, in first-seen order.
        World& world = match_.world();
        int list_x = col_x;
        if (EntityCommon* c0 = world.common(selected_[0]); c0 && c0->team >= 0) {
            SDL_Rect flag_rect{col_x, panel_top + ui(2), ui(48), ui(32)};
            atlas_.draw_in_rect(flag_rect, "spr_flags_mini", match_.control().teams[c0->team].civ, /*pad=*/0);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderDrawRect(renderer, &flag_rect);
            list_x = col_x + ui(55);
        }
        std::vector<std::pair<std::string, int>> counts;
        for (auto ref : selected_) {
            Unit* u = world.get(ref);
            if (!u) continue;
            auto it = std::find_if(counts.begin(), counts.end(),
                                   [&](auto& p) { return p.first == u->name; });
            if (it != counts.end()) ++it->second;
            else counts.emplace_back(u->name, 1);
        }
        int icon_size = ui(kUnit);
        int ix = list_x;
        for (auto& [name, n] : counts) {
            if (ix + icon_size > queue_col_x()) break; // don't run into the production-queue column
            SDL_Rect rect{ix, panel_top + ui(2), icon_size - ui(4), icon_size - ui(4)};
            atlas_.draw_stretched("spr_button", rect);
            int ipad = ui(3);
            SDL_Rect icon_rect{rect.x + ipad, rect.y + ipad, rect.w - 2 * ipad, rect.h - 2 * ipad};
            // Player-colour frame so an 8-frame team-coloured unit icon matches
            // the colour the player chose (draw_in_rect clamps single-frame
            // icons to 0, so mixed selections stay correct).
            atlas_.draw_in_rect(icon_rect, item_icon(name), item_icon_frame(name), /*pad=*/0);
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", n);
            int tw, th;
            int badge_size = ui(11);
            text_.measure(buf, badge_size, tw, th);
            text_.draw(buf, rect.x + rect.w - tw - ui(2), rect.y + rect.h - th - ui(1),
                      {255, 255, 255, 255}, badge_size);
            ix += icon_size;
        }
    } else if (!selected_.empty()) {
        World& world = match_.world();
        EntityRef ref = selected_[0];
        EntityCommon* c = world.common(ref);
        Unit* u = world.get(ref);
        Building* b = world.get_building(ref);
        Resource* r = world.get_resource(ref);
        if (c) {
            std::string name = u ? u->name : b ? b->name : r ? r->name : "";
            int text_x = col_x;

            // Row 1: flag (per-civ, spr_flags_mini frame = civ id) + name.
            // spr_flags_mini is natively 48x32 -- the previous spr_flags
            // (96x64, meant for the bigger civ-select screens) had to be
            // squeezed down into the 45x32 box, blurring it and leaving the
            // border around the box rather than the smaller letterboxed
            // sprite actually drawn inside it. At native size there's
            // nothing to scale, so the 1px border now hugs the real pixels.
            if ((u || b) && c->team >= 0) {
                SDL_Rect flag_rect{col_x, panel_top + ui(2), ui(48), ui(32)};
                atlas_.draw_in_rect(flag_rect, "spr_flags_mini", match_.control().teams[c->team].civ, /*pad=*/0);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderDrawRect(renderer, &flag_rect);
                text_x = col_x + ui(55);
            }
            std::string display_name = name;
            // Prefer the catalog's own display string (e.g. "swordsman2" ->
            // "Swordmastery", "heavy tank" -> "Heavy Tank") over the raw key.
            for (const char* section : {"units", "buildings"}) {
                auto& sec = match_.world().data.catalog().at(section);
                if (sec.contains(name)) {
                    std::string d = sec.at(name).value("display", "");
                    if (!d.empty()) display_name = d;
                    break;
                }
            }
            if (!display_name.empty()) display_name[0] = std::toupper(display_name[0]);
            // Per-civ historical name for the "base" building, matching the
            // original GameMaker source (assets/gmk/scripts/get_stats.gml's
            // per-civ `display` strings), keyed by civ id in the same order
            // as scenario.cpp's CIV_BASE sprite table.
            if (b && name == "base" && c->team >= 0) {
                static const char* kBaseNames[9] = {
                    "Westminster Palace", "US Capitol", "Reich Chancellery", "Kremlin",
                    "Imperial Palace", "Palazzo della Civilta", "Hotel de Ville",
                    "Great Hall of the People", "Sultan Ahmed Mosque",
                };
                int civ = match_.control().teams[c->team].civ;
                if (civ >= 0 && civ < 9) display_name = kBaseNames[civ];
            }
            // Shrink to fit before the production-queue grid's left edge --
            // most names (units, most buildings) never come close, but a
            // few of the per-civ base names ("Great Hall of the People")
            // are long enough to run into the queue otherwise.
            int name_max_w = queue_col_x() - text_x - ui(6);
            int name_size = ui(13);
            int nmw, nmh;
            text_.measure(display_name, name_size, nmw, nmh);
            while (nmw > name_max_w && name_size > ui(8)) {
                --name_size;
                text_.measure(display_name, name_size, nmw, nmh);
            }
            text_.draw(display_name, text_x, panel_top + ui(1), {0, 0, 0, 255}, name_size);

            if (r) {
                // Resource node: resource-type icon (spr_resources frame =
                // rtype) + the remaining amount as a number, and a health-style
                // bar showing amount / start_amount.
                int rt = std::clamp(r->res.rtype, 0, 3);
                SDL_Rect ic{text_x, panel_top + ui(14), ui(20), ui(20)};
                atlas_.draw_in_rect(ic, "spr_resources", rt, /*pad=*/0);
                char num[16];
                std::snprintf(num, sizeof(num), "%.0f", r->res.amount);
                text_regular_.draw(num, text_x + ui(24), panel_top + ui(16), {0, 0, 0, 255}, ui(12));
                double frac = r->res.start_amount > 0
                                  ? std::clamp(r->res.amount / r->res.start_amount, 0.0, 1.0)
                                  : 0.0;
                int bar_x = text_x + ui(24), bar_y = panel_top + ui(29), bar_w = ui(70);
                SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
                SDL_Rect bbg{bar_x, bar_y, bar_w, ui(6)};
                SDL_RenderFillRect(renderer, &bbg);
                SDL_SetRenderDrawColor(renderer, 90, 200, 90, 255);
                SDL_Rect bfg{bar_x, bar_y, static_cast<int>(bar_w * frac), ui(6)};
                SDL_RenderFillRect(renderer, &bfg);
            } else {
                // Row 2: HP bar + "x/y" text. Anchored to text_x (right of the
                // flag), not col_x -- the flag is now tall enough that anything
                // drawn at col_x would render underneath/through it. 25%
                // shorter than the original 110 native px, for units and
                // buildings alike (both read as too wide at the original size).
                double frac = c->max_hp > 0 ? std::clamp(c->hp / c->max_hp, 0.0, 1.0) : 0.0;
                int hp_bar_w = ui(110.0 * 0.75);
                SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
                SDL_Rect hp_bg{text_x, panel_top + ui(18), hp_bar_w, ui(7)};
                SDL_RenderFillRect(renderer, &hp_bg);
                SDL_SetRenderDrawColor(renderer, 60, 225, 60, 255);
                SDL_Rect hp_fg{text_x, panel_top + ui(18), static_cast<int>(hp_bar_w * frac), ui(7)};
                SDL_RenderFillRect(renderer, &hp_fg);
                char hp_buf[24];
                std::snprintf(hp_buf, sizeof(hp_buf), "%.0f/%.0f", c->hp, c->max_hp);
                text_regular_.draw(hp_buf, text_x + hp_bar_w + ui(4), panel_top + ui(16), {0, 0, 0, 255}, ui(11));
            }

            // Row 3/4: attack, then armor(pierce) directly below it -- two
            // separate rows 12 native px apart (matching the original GML's
            // control/Draw.gml: sword icon+text at y+40, armor icon+text at
            // y+52), not side-by-side on one row. Side-by-side was cramped
            // enough for the icon/text of one stat to run into the other.
            // spr_sword_icon/spr_armor_icon are 11x11 native with a
            // top-left pivot, so at this 1.3x draw scale they render
            // ~14px tall -- icon and text are both top-left anchored at
            // the same row y (no extra per-element offset needed), and
            // rows are spaced ui(16) apart so the ~14px-tall icons in
            // adjacent rows can't touch.
            int stat_x = text_x;
            int stat_y = panel_top + ui(34);
            if (u) {
                atlas_.draw("spr_sword_icon", stat_x + ui(6), stat_y, 0, 1.3 * kUiScale);
                // Show base attack plus any upgrade bonus as "base+delta"
                // (e.g. scout cavalry with the first refinery upgrade -> "3+1")
                // so the player can see how much a tech added.
                double atk_delta = u->attack - u->base_attack;
                char atk_buf[24];
                if (atk_delta > 0.5)
                    std::snprintf(atk_buf, sizeof(atk_buf), "%.0f+%.0f", u->base_attack, atk_delta);
                else
                    std::snprintf(atk_buf, sizeof(atk_buf), "%.0f", u->attack);
                int atk_x = stat_x + ui(20);
                text_regular_.draw(atk_buf, atk_x, stat_y, {0, 0, 0, 255}, ui(11));
                // Attack range, in tiles, for every combat unit (melee shows 1).
                // Positioned right AFTER the attack text (measured) rather than a
                // fixed column, so a wide "base+delta" like "200+50" (e.g. a
                // Germany ballistic missile) can't run into the range readout.
                if (u->range_px > 1.0 || !u->melee) {
                    int aw = 0, ah = 0;
                    text_regular_.measure(atk_buf, ui(11), aw, ah);
                    char rng_buf[16];
                    std::snprintf(rng_buf, sizeof(rng_buf), "R:%.0f", std::max(1.0, u->range_px / 32.0));
                    text_regular_.draw(rng_buf, atk_x + aw + ui(9), stat_y, {0, 0, 0, 255}, ui(11));
                }

                int armor_y = stat_y + ui(16);
                atlas_.draw("spr_armor_icon", stat_x + ui(6), armor_y, 0, 1.3 * kUiScale);
                char armor_buf[24];
                std::snprintf(armor_buf, sizeof(armor_buf), "%d(%d)", u->armor, u->pierce);
                text_regular_.draw(armor_buf, stat_x + ui(20), armor_y, {0, 0, 0, 255}, ui(11));

                // Movement speed, directly UNDER the armour row (spr_speed_icon +
                // the unit's speed in the catalog's own relative units, i.e.
                // speed_px / 60 -- reflects live tech/bonus modifiers).
                int speed_y = armor_y + ui(16);
                SDL_Rect spd_ic{stat_x + ui(4), speed_y, ui(14), ui(14)};
                atlas_.draw_in_rect(spd_ic, "spr_speed_icon", 0, /*pad=*/0);
                char spd_buf[24];
                std::snprintf(spd_buf, sizeof(spd_buf), "%.1f", u->speed_px / 60.0);
                text_regular_.draw(spd_buf, stat_x + ui(20), speed_y, {0, 0, 0, 255}, ui(11));

                // Blast radius (area-of-effect weapons only -- artillery, ships,
                // bombers, the ballistic missile): the splash radius in tiles,
                // a red attack-ground target icon + number, pushed to UNDER the
                // speed row (so it appears only for AoE units, below speed).
                double blast = 0.0;
                {
                    const auto& units = match_.world().data.catalog().at("units");
                    if (units.contains(u->name)) blast = units.at(u->name).value("blast_radius", 0.0);
                }
                // 420mm Mortar (Soviet unique tech) gives this team's artillery a
                // +33% blast radius -- the same 1.33x the sim applies to the shell's
                // splash (see World::spawn_projectile) -- so the stat reflects the
                // live, researched value rather than the base catalog number.
                if (blast > 0.0 && (u->name == "artillery" || u->name == "artillery1")) {
                    const auto& teams = match_.control().teams;
                    if (u->common.team >= 0 && u->common.team < static_cast<int>(teams.size()) &&
                        teams[u->common.team].tech.count("420mm mortar")) {
                        blast *= 1.33;
                    }
                }
                if (blast > 0.0) {
                    int blast_y = speed_y + ui(16);
                    SDL_Rect tgt{stat_x + ui(4), blast_y, ui(14), ui(14)};
                    atlas_.draw_in_rect(tgt, "spr_attack_target_icon", 0, /*pad=*/0);
                    char blast_buf[24];
                    std::snprintf(blast_buf, sizeof(blast_buf), "%g", std::round(blast * 100.0) / 100.0);
                    text_regular_.draw(blast_buf, stat_x + ui(20), blast_y, {0, 0, 0, 255}, ui(11));
                }
            } else if (b && !b->queue.empty()) {
                // Buildings don't get an attack/armor readout -- that whole
                // block is unused space for them, so production progress
                // (bar + bold "%" text) goes right under the HP bar in its
                // place instead of being squeezed below both rows (which
                // pushed the "%" text far enough down to run past the
                // panel's bottom edge, ui(34)+ui(16) from panel_top left no
                // room for a 13px-tall text line within the 64px-tall
                // panel). More visible than the tiny strip that used to sit
                // under the command-card's first queue icon (see
                // draw_command_card, which no longer draws that strip now
                // that this covers it).
                SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
                SDL_Rect prog_bg{text_x, stat_y, ui(110), ui(8)};
                SDL_RenderFillRect(renderer, &prog_bg);
                double pfrac = std::clamp(b->percent / 100.0, 0.0, 1.0);
                SDL_SetRenderDrawColor(renderer, 220, 30, 30, 255);
                SDL_Rect prog_fg{text_x, stat_y, static_cast<int>(ui(110) * pfrac), ui(8)};
                SDL_RenderFillRect(renderer, &prog_fg);
                char pct_buf[8];
                std::snprintf(pct_buf, sizeof(pct_buf), "%.0f%%", b->percent);
                text_.draw(pct_buf, text_x, stat_y + ui(10), {215, 20, 20, 255}, ui(13));
            } else if (b && b->attack > 0.0) {
                // Defensive structure (tower/fortress/aa tower): attack + range
                // on the top row, armor(pierce) below -- same readout as a
                // combat unit so a player can see what a tower brings.
                atlas_.draw("spr_sword_icon", stat_x + ui(6), stat_y, 0, 1.3 * kUiScale);
                char atk_buf[16];
                std::snprintf(atk_buf, sizeof(atk_buf), "%.0f", b->attack);
                text_regular_.draw(atk_buf, stat_x + ui(20), stat_y, {0, 0, 0, 255}, ui(11));
                char rng_buf[16];
                std::snprintf(rng_buf, sizeof(rng_buf), "R:%.0f", std::max(1.0, b->range_px / 32.0));
                text_regular_.draw(rng_buf, stat_x + ui(48), stat_y, {0, 0, 0, 255}, ui(11));
                int armor_y = stat_y + ui(16);
                atlas_.draw("spr_armor_icon", stat_x + ui(6), armor_y, 0, 1.3 * kUiScale);
                char armor_buf[24];
                std::snprintf(armor_buf, sizeof(armor_buf), "%d(%d)", b->armor, b->pierce);
                text_regular_.draw(armor_buf, stat_x + ui(20), armor_y, {0, 0, 0, 255}, ui(11));
            } else if (b && b->name == "farm") {
                if (b->exhausted) {
                    // Dead farm: prompt the player to re-sow it (right-click
                    // with a villager, 40 wood).
                    text_regular_.draw("Depleted - re-sow (40 wood)", stat_x, stat_y + ui(2),
                                       {180, 60, 40, 255}, ui(11));
                } else {
                    // Farms are food sources -- food icon + number + a health-
                    // style bar of food left, same readout as a resource node.
                    SDL_Rect ic{stat_x, stat_y, ui(18), ui(18)};
                    atlas_.draw_in_rect(ic, "spr_resources", 0 /*food*/, /*pad=*/0);
                    char num[16];
                    std::snprintf(num, sizeof(num), "%.0f", b->amount);
                    text_regular_.draw(num, stat_x + ui(22), stat_y + ui(2), {0, 0, 0, 255}, ui(11));
                    double frac = b->max_farm_food > 0 ? std::clamp(b->amount / b->max_farm_food, 0.0, 1.0) : 0.0;
                    SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
                    SDL_Rect bbg{stat_x + ui(22), stat_y + ui(13), ui(70), ui(6)};
                    SDL_RenderFillRect(renderer, &bbg);
                    SDL_SetRenderDrawColor(renderer, 90, 200, 90, 255);
                    SDL_Rect bfg{stat_x + ui(22), stat_y + ui(13), static_cast<int>(ui(70) * frac), ui(6)};
                    SDL_RenderFillRect(renderer, &bfg);
                }
            }

            // Carry (gatherers only): spr_resource_carry frame = carry_type.
            // Matches the original GML (control/Draw.gml:569-571): icon/text
            // at text_x+148/+180, y+32 -- well clear to the right of the
            // attack/armor block (which starts at text_x, y+40) so it
            // doesn't run into it. The previous view_w_-relative anchor put
            // it only ~48px right of the stat block at this canvas width,
            // which collided with the armor text.
            if (u && u->is_gatherer) {
                int carry_x = text_x + ui(148);
                int carry_y = panel_top + ui(32);
                atlas_.draw("spr_resource_carry", carry_x + ui(10), carry_y + ui(8), u->carry_type,
                           0.8 * kUiScale);
                char carry_buf[16];
                std::snprintf(carry_buf, sizeof(carry_buf), "%.0f", u->carry);
                text_regular_.draw(carry_buf, carry_x + ui(22), carry_y, {0, 0, 0, 255}, ui(11));
            }

            // Transport ship: hold gauge (pop used / capacity) and a tally of
            // the units aboard, one icon per type with an "xN" count.
            if (u && u->transport_cap > 0) {
                std::map<std::string, int> tally; // unit name -> how many aboard
                double used = 0.0; // fractional: Royal Marines take half a slot each
                for (auto cref : u->cargo) {
                    Unit* cu = match_.world().get(cref);
                    if (!cu) continue;
                    tally[cu->name]++;
                    used += ww::sim::transport_cost(cu->name);
                }
                int cx = text_x + ui(148);
                int cy = panel_top + ui(2);
                char hold_buf[24];
                std::snprintf(hold_buf, sizeof(hold_buf), "Hold %g/%d", used, u->transport_cap);
                text_regular_.draw(hold_buf, cx, cy, {0, 0, 0, 255}, ui(11));
                int icon = ui(18), gap = ui(1);
                int ix = cx, iy = cy + ui(16);
                int row_left = cx, row_right = cx + ui(120);
                for (auto& [cname, cnt] : tally) {
                    SDL_Rect ir{ix, iy, icon, icon};
                    atlas_.draw_in_rect(ir, item_icon(cname), item_icon_frame(cname), /*pad=*/0);
                    char nb[8];
                    std::snprintf(nb, sizeof(nb), "%d", cnt);
                    text_regular_.draw(nb, ix + icon - ui(6), iy + icon - ui(11),
                                       {255, 240, 60, 255}, ui(10));
                    ix += icon + gap;
                    if (ix + icon > row_right) { ix = row_left; iy += icon + gap; } // wrap
                }
            }
        }
    }

    draw_minimap(renderer);
    draw_idle_button(renderer);
}

void GameClient::draw_idle_button(SDL_Renderer* renderer) {
    World& world = match_.world();
    int idle_count = 0;
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (u && is_idle_civilian(*u)) ++idle_count;
    }

    // Just left of the minimap (drawn immediately before this, so minimap_
    // rect_ is already this frame's), vertically centered against it.
    int size = ui(32);
    int bx = minimap_rect_.x - ui(4) - size;
    int by = minimap_rect_.y + (minimap_rect_.h - size) / 2;
    idle_button_rect_ = SDL_Rect{bx, by, size, size};

    const char* sprite = idle_count > 0 ? "spr_idle_icon" : "spr_idle_icon_none";
    if (atlas_.meta(sprite)) {
        atlas_.draw_in_rect(idle_button_rect_, sprite, /*frame=*/0, /*pad=*/0);
    } else {
        // Defensive fallback so the button stays visible/clickable even if
        // the sprite's ever missing, instead of silently disappearing.
        SDL_Color c = idle_count > 0 ? SDL_Color{200, 60, 60, 255} : SDL_Color{90, 90, 90, 255};
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
        SDL_RenderFillRect(renderer, &idle_button_rect_);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawRect(renderer, &idle_button_rect_);
    }

    // Count badge -- small white-on-black chip centered on the button's
    // bottom-right corner (so it overlaps both the button and the space
    // past it), same "icon + overlapping count" idea as the airbase nuke
    // stockpile badge above. Only shown with idle > 0 -- the grey/red
    // sprite swap already communicates zero on its own.
    if (idle_count > 0) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", idle_count);
        int size_px = ui(11);
        int tw, th;
        text_.measure(buf, size_px, tw, th);
        int pad = ui(2);
        SDL_Rect chip{idle_button_rect_.x + idle_button_rect_.w - tw / 2 - pad,
                     idle_button_rect_.y + idle_button_rect_.h - th / 2 - pad, tw + pad * 2, th + pad * 2};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, &chip);
        text_.draw(buf, chip.x + pad, chip.y + pad, {255, 255, 255, 255}, size_px);
    }
}

void GameClient::draw_objectives(SDL_Renderer* renderer) {
    if (campaign_objectives_.empty()) return; // skirmish / no shown objectives
    const auto& cleared = match_.world().cleared_objectives;
    int size = ui(11), pad = ui(5), line_h = ui(15);
    int maxw = 0, tw, th;
    text_.measure("Objectives", size, tw, th);
    maxw = tw;
    for (auto& kv : campaign_objectives_) {
        text_.measure(kv.second, size, tw, th);
        maxw = std::max(maxw, tw);
    }
    int panel_w = maxw + pad * 2;
    int panel_h = pad * 2 + line_h * static_cast<int>(campaign_objectives_.size() + 1);
    int px = view_w_ - panel_w - ui(4); // top-RIGHT corner, under the resource bar
    int py = top_bar_height() + ui(4);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 10, 12, 18, 190);
    SDL_Rect bg{px, py, panel_w, panel_h};
    SDL_RenderFillRect(renderer, &bg);
    SDL_SetRenderDrawColor(renderer, 120, 120, 130, 200);
    SDL_RenderDrawRect(renderer, &bg);
    int ty = py + pad;
    text_.draw("Objectives", px + pad, ty, {255, 220, 60, 255}, size);
    ty += line_h;
    for (auto& kv : campaign_objectives_) {
        bool done = cleared.count(kv.first) > 0;
        // Completed -> green with a strikethrough line; pending -> plain white.
        SDL_Color col = done ? SDL_Color{90, 220, 90, 255} : SDL_Color{230, 230, 230, 255};
        text_.draw(kv.second, px + pad, ty, col, size);
        if (done) {
            text_.measure(kv.second, size, tw, th);
            int ly = ty + th / 2;
            SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
            SDL_RenderDrawLine(renderer, px + pad, ly, px + pad + tw, ly);
        }
        ty += line_h;
    }
}

void GameClient::draw_notifications(SDL_Renderer* renderer) {
    // Oldest at top, newest appended below -- same stacking order as the
    // original's ds_list (objects/control/Draw.gml draws index 0 first,
    // i.e. topmost). Left-aligned near the top of the game view, just
    // under the top resource bar, plain white with no background box or
    // fade (matching the original -- see ClientNotification's comment on
    // what IS simplified: independent per-line timers instead of one
    // shared countdown).
    int x = ui(32);
    // Offset by the global production strip above (0 when it is empty), so
    // the two never overlap in the same corner.
    int y = top_bar_height() + ui(8) + global_queue_h_;
    // Every built-in notification (era advance, unit/building/tech
    // complete) is a short one-liner well under this width -- the wrap
    // only ever actually engages for a "map_message" trigger's free-typed
    // text (editor's Events tab), which has no length
    // limit of its own beyond the editor's 100-char field.
    int max_w = view_w_ - x - ui(32);
    for (const auto& n : notifications_) {
        for (auto& line : wrap_text(text_, n.text, ui(13), max_w)) {
            text_.draw(line, x, y, {255, 255, 255, 255}, ui(13));
            y += ui(20);
        }
    }
}

void GameClient::draw_chat(SDL_Renderer* renderer) {
    // AoE-style chat: a scrolling log bottom-left (most recent line closest
    // to the input bar), with the input bar itself only drawn while
    // chat_open_ (Enter to open, see handle_event/open_chat).
    int row_h = ui(16);
    int input_h = ui(20);
    int x = ui(8);
    int bottom_y = view_h_ - panel_height() - ui(4);
    int log_bottom = chat_open_ ? bottom_y - input_h - ui(4) : bottom_y;

    int n = static_cast<int>(chat_log_.size());
    for (int i = 0; i < n; ++i) {
        int y = log_bottom - (n - i) * row_h;
        text_.draw(chat_log_[i].text, x, y, {255, 255, 255, 255}, ui(12));
    }

    if (chat_open_) {
        SDL_Rect box{x, bottom_y - input_h, ui(300), input_h};
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 190);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawRect(renderer, &box);
        // Trailing "_" as a simple fixed caret -- no blink, but enough to
        // show where typed text is going.
        std::string display = "> " + chat_input_ + "_";
        text_.draw(display, x + ui(4), bottom_y - input_h + ui(3), {255, 255, 0, 255}, ui(13));
    }
}

void GameClient::draw_pause_button(SDL_Renderer* renderer) {
    int size = ui(32);
    pause_button_rect_ = SDL_Rect{view_w_ - size - ui(4), ui(4), size, size};
    atlas_.draw_in_rect(pause_button_rect_, "spr_pausemenu", /*frame=*/0, /*pad=*/0);
}

void GameClient::draw_pause_menu(SDL_Renderer* renderer) {
    // Full-screen dim, then a centered panel with Resume/Quit Game --
    // Quit Game is the only other option for now (per the request), but
    // Resume is basic table-stakes for a pause menu (there has to be SOME
    // way back into the match besides Escape).
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
    SDL_Rect dim{0, 0, view_w_, view_h_};
    SDL_RenderFillRect(renderer, &dim);

    int panel_w = ui(220), panel_h = ui(160);
    SDL_Rect panel{(view_w_ - panel_w) / 2, (view_h_ - panel_h) / 2, panel_w, panel_h};
    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 235);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &panel);

    std::string title = "PAUSED";
    int tsize = ui(20), tw, th;
    text_.measure(title, tsize, tw, th);
    text_.draw(title, panel.x + (panel.w - tw) / 2, panel.y + ui(14), {255, 230, 60, 255}, tsize);

    auto draw_menu_button = [&](SDL_Rect& rect, int y_offset, const std::string& label, SDL_Color colour) {
        rect = SDL_Rect{panel.x + ui(20), panel.y + y_offset, panel.w - ui(40), ui(32)};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, colour.r, colour.g, colour.b, 255);
        SDL_RenderDrawRect(renderer, &rect);
        int lsize = ui(14), lw, lh;
        text_.measure(label, lsize, lw, lh);
        text_.draw(label, rect.x + (rect.w - lw) / 2, rect.y + (rect.h - lh) / 2, colour, lsize);
    };
    draw_menu_button(pause_resume_rect_, ui(56), "Resume", {255, 255, 255, 255});
    draw_menu_button(pause_quit_rect_, ui(100), "Quit Game", {255, 90, 90, 255});
}

namespace {
const char* const kStatsTabs[] = {"Score",     "Military",  "Largest Battle", "Economy",
                                  "Technology", "Society",  "Timeline"};
constexpr int kStatsTabCount = 7;
std::string fmt_mmss(double s) {
    if (s < 0) return "--:--";
    int t = static_cast<int>(s);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
    return buf;
}
std::string fmt_k(double v) {
    char buf[24];
    if (v >= 10000) std::snprintf(buf, sizeof(buf), "%.1fk", v / 1000.0);
    else std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(v + 0.5));
    return buf;
}
} // namespace

void GameClient::draw_stats_screen(SDL_Renderer* renderer) {
    stats_tab_rects_.clear();
    const int W = view_w_, H = view_h_;
    // Parchment-ish full-screen panel.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 10, 8, 6, 235);
    SDL_Rect full{0, 0, W, H};
    SDL_RenderFillRect(renderer, &full);
    SDL_Rect panel{ui(10), ui(8), W - ui(20), H - ui(16)};
    SDL_SetRenderDrawColor(renderer, 54, 42, 28, 255); // aged parchment brown
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 150, 120, 74, 255);
    SDL_RenderDrawRect(renderer, &panel);

    const SDL_Color kInk{235, 224, 198, 255};   // parchment ink
    const SDL_Color kInkDim{178, 158, 120, 255};
    const SDL_Color kGold{255, 214, 96, 255};

    // ---- header: result + title + clock ----
    int hy = panel.y + ui(6);
    if (stats_.decided) {
        std::string res = stats_.team0_won ? "VICTORY" : "DEFEAT";
        SDL_Color rc = stats_.team0_won ? SDL_Color{120, 230, 120, 255} : SDL_Color{235, 96, 96, 255};
        int rs = ui(20), rw, rh;
        text_.measure(res, rs, rw, rh);
        text_.draw(res, panel.x + ui(12), hy, rc, rs);
    }
    {
        std::string title = "Statistics";
        int ts = ui(18), tw, th;
        text_.measure(title, ts, tw, th);
        text_.draw(title, panel.x + (panel.w - tw) / 2, hy, kGold, ts);
        std::string clk = "Time  " + fmt_mmss(stats_.elapsed_s);
        int cw, ch;
        text_.measure(clk, ui(13), cw, ch);
        text_.draw(clk, panel.x + panel.w - cw - ui(12), hy + ui(3), kInkDim, ui(13));
    }

    // ---- tab row ----
    int tab_y = panel.y + ui(30);
    int tab_h = ui(20);
    int tab_w = (panel.w - ui(16)) / kStatsTabCount;
    for (int i = 0; i < kStatsTabCount; ++i) {
        SDL_Rect tr{panel.x + ui(8) + i * tab_w, tab_y, tab_w - ui(3), tab_h};
        bool active = (stats_tab_ == i);
        SDL_SetRenderDrawColor(renderer, active ? 96 : 40, active ? 76 : 32, active ? 44 : 22, 255);
        SDL_RenderFillRect(renderer, &tr);
        SDL_SetRenderDrawColor(renderer, active ? 255 : 120, active ? 214 : 100, active ? 96 : 64, 255);
        SDL_RenderDrawRect(renderer, &tr);
        int fs = ui(11), fw, fh;
        text_.measure(kStatsTabs[i], fs, fw, fh);
        if (fw > tr.w - 4) { fs = ui(9); text_.measure(kStatsTabs[i], fs, fw, fh); }
        text_.draw(kStatsTabs[i], tr.x + (tr.w - fw) / 2, tr.y + (tr.h - fh) / 2, active ? kGold : kInkDim, fs);
        stats_tab_rects_.push_back({tr, i});
    }

    // ---- content area ----
    SDL_Rect body{panel.x + ui(10), tab_y + tab_h + ui(8), panel.w - ui(20),
                  (panel.y + panel.h) - (tab_y + tab_h) - ui(44)};
    const auto& teams = stats_.teams;
    auto team_col = [&](int idx) -> SDL_Color {
        int c = std::clamp(teams[idx].colour, 0, 7);
        return ww::menu::team_colours()[c];
    };

    // Generic table: a name column (team leaders, coloured) + labelled stat
    // columns. `cols` supplies {header, value(teamIndex)}.
    auto draw_table = [&](const std::vector<std::string>& headers,
                          const std::vector<std::function<std::string(int)>>& cells) {
        int n = static_cast<int>(teams.size());
        int name_w = ui(96);
        int ncol = static_cast<int>(headers.size());
        int col_w = ncol > 0 ? (body.w - name_w) / ncol : 0;
        int row_h = std::max(ui(14), std::min(ui(22), (body.h - ui(20)) / std::max(1, n + 1)));
        int y = body.y;
        // header row
        for (int c = 0; c < ncol; ++c) {
            int hx = body.x + name_w + c * col_w;
            text_regular_.draw(headers[c], hx, y, kInkDim, ui(11));
        }
        SDL_SetRenderDrawColor(renderer, 120, 100, 64, 255);
        SDL_RenderDrawLine(renderer, body.x, y + row_h - ui(3), body.x + body.w, y + row_h - ui(3));
        y += row_h;
        for (int i = 0; i < n; ++i) {
            SDL_Color tc = team_col(i);
            std::string nm = teams[i].name;
            if (teams[i].winner) nm += "  *";
            text_.draw(nm, body.x, y + ui(2), tc, ui(12));
            for (int c = 0; c < ncol; ++c) {
                int hx = body.x + name_w + c * col_w;
                text_regular_.draw(cells[c](i), hx, y + ui(2), kInk, ui(12));
            }
            y += row_h;
        }
    };

    switch (stats_tab_) {
        case 0: // Score
            draw_table({"Score", "Result", "Era", "Military", "Killed"},
                       {[&](int i) { return std::to_string(teams[i].score); },
                        [&](int i) {
                            return stats_.decided ? (teams[i].winner ? "Victor" : "Defeated") : "Undecided";
                        },
                        [&](int i) {
                            static const char* e[4] = {"Victorian", "Industrial", "War", "Scientific"};
                            return std::string(e[std::clamp(teams[i].era, 0, 3)]);
                        },
                        [&](int i) { return std::to_string(teams[i].military_created); },
                        [&](int i) { return std::to_string(teams[i].units_killed); }});
            break;
        case 1: // Military
            draw_table({"Trained", "Peak army", "Killed", "Lost", "Razed", "Bld lost"},
                       {[&](int i) { return std::to_string(teams[i].military_created); },
                        [&](int i) { return std::to_string(teams[i].peak_army); },
                        [&](int i) { return std::to_string(teams[i].units_killed); },
                        [&](int i) { return std::to_string(teams[i].units_lost); },
                        [&](int i) { return std::to_string(teams[i].buildings_razed); },
                        [&](int i) { return std::to_string(teams[i].buildings_lost); }});
            break;
        case 2: { // Largest Battle -- Wikipedia infobox style
            const auto& b = stats_.battle;
            int y = body.y + ui(2);
            if (!b.valid) {
                text_.draw("No major battle took place this match.", body.x, y, kInkDim, ui(14));
                break;
            }
            // Title + summary line.
            text_.draw(b.name, body.x, y, kGold, ui(18));
            y += ui(22);
            std::string sub = "Part of the war  \x95  " + fmt_mmss(b.t_start) + "-" + fmt_mmss(b.t_end);
            text_regular_.draw(sub, body.x, y, kInkDim, ui(11));
            y += ui(16);
            // Result + territory (the infobox "Result" row).
            text_regular_.draw("Result: ", body.x, y, kInkDim, ui(12));
            {
                int lw, lh;
                text_regular_.measure("Result: ", ui(12), lw, lh);
                SDL_Color oc = b.winner_team >= 0 ? SDL_Color{120, 230, 120, 255} : SDL_Color{210, 200, 150, 255};
                std::string res = b.outcome;
                if (b.winner_team >= 0 && b.winner_team < (int)teams.size())
                    res += " - " + teams[b.winner_team].name;
                text_.draw(res, body.x + lw, y, oc, ui(12));
            }
            y += ui(16);
            text_regular_.draw(b.territory, body.x, y, kInkDim, ui(11));
            y += ui(18);

            // Two faction columns side by side (flag + leader + order of battle).
            int ncol = std::min<int>(2, (int)b.sides.size());
            if (ncol == 0) break;
            int gap = ui(14);
            int col_w = (body.w - gap) / std::max(1, ncol);
            int top = y;
            for (int si = 0; si < ncol; ++si) {
                const auto& s = b.sides[si];
                int cx = body.x + si * (col_w + gap);
                int cy = top;
                SDL_Color tc = ww::menu::team_colours()[std::clamp(s.colour, 0, 7)];
                // Flag + leader header.
                SDL_Rect fr{cx, cy, ui(30), ui(20)};
                if (atlas_.meta("spr_flags_mini"))
                    atlas_.draw_in_rect(fr, "spr_flags_mini", ww::menu::kCivFlagFrame[std::clamp(s.civ, 0, 8)], 0);
                text_.draw(s.leader_name, cx + ui(36), cy, tc, ui(13));
                cy += ui(20);
                text_regular_.draw(std::string(si == 0 ? "Attacker" : "Defender") + "  \x95  " +
                                       std::to_string(s.total_involved) + " engaged",
                                   cx, cy, kInkDim, ui(10));
                cy += ui(16);
                // Column headers.
                text_regular_.draw("Unit", cx + ui(20), cy, kInkDim, ui(10));
                text_regular_.draw("Eng", cx + col_w - ui(66), cy, kInkDim, ui(10));
                text_regular_.draw("Lost", cx + col_w - ui(34), cy, kInkDim, ui(10));
                cy += ui(13);
                int shown = 0;
                for (const auto& g : s.groups) {
                    if (shown++ >= 7) break; // cap rows to fit
                    std::string icon = item_icon(g.unit);
                    SDL_Rect ir{cx, cy, ui(16), ui(16)};
                    if (atlas_.meta(icon)) atlas_.draw_in_rect(ir, icon, 0, 0);
                    text_regular_.draw(g.unit, cx + ui(20), cy + ui(2), kInk, ui(10));
                    text_regular_.draw(std::to_string(g.involved), cx + col_w - ui(66), cy + ui(2), kInk, ui(10));
                    SDL_Color lc = g.casualties > 0 ? SDL_Color{235, 140, 130, 255} : kInkDim;
                    text_regular_.draw(std::to_string(g.casualties), cx + col_w - ui(34), cy + ui(2), lc, ui(10));
                    cy += ui(16);
                }
                // Side totals.
                SDL_SetRenderDrawColor(renderer, 120, 100, 64, 255);
                SDL_RenderDrawLine(renderer, cx, cy + ui(1), cx + col_w - gap, cy + ui(1));
                cy += ui(4);
                text_regular_.draw("Casualties: " + std::to_string(s.total_casualties) +
                                       "   Kills: " + std::to_string(s.kills),
                                   cx, cy, kInk, ui(11));
            }
            break;
        }
        case 3: // Economy
            draw_table({"Food", "Wood", "Oil", "Iron", "Built", "Peak vil"},
                       {[&](int i) { return fmt_k(teams[i].gathered[0]); },
                        [&](int i) { return fmt_k(teams[i].gathered[1]); },
                        [&](int i) { return fmt_k(teams[i].gathered[2]); },
                        [&](int i) { return fmt_k(teams[i].gathered[3]); },
                        [&](int i) { return std::to_string(teams[i].buildings_built); },
                        [&](int i) { return std::to_string(teams[i].peak_vil); }});
            break;
        case 4: // Technology
            draw_table({"Techs", "Industrial", "War", "Scientific"},
                       {[&](int i) { return std::to_string(teams[i].techs_researched); },
                        [&](int i) { return fmt_mmss(teams[i].age_reached[1]); },
                        [&](int i) { return fmt_mmss(teams[i].age_reached[2]); },
                        [&](int i) { return fmt_mmss(teams[i].age_reached[3]); }});
            break;
        case 5: // Society
            draw_table({"Units now", "Villagers", "Buildings", "Peak vil", "Idle TC"},
                       {[&](int i) { return std::to_string(teams[i].cur_units); },
                        [&](int i) { return std::to_string(teams[i].cur_vil); },
                        [&](int i) { return std::to_string(teams[i].cur_buildings); },
                        [&](int i) { return std::to_string(teams[i].peak_vil); },
                        [&](int i) { return fmt_mmss(teams[i].idle_tc); }});
            break;
        case 6: // Timeline (population over time, stacked area) -- see below
            draw_stats_timeline(renderer, body);
            break;
    }

    // ---- Return to Menu button ----
    std::string rl = "Return to Menu";
    int rs = ui(14), rw, rh;
    text_.measure(rl, rs, rw, rh);
    SDL_Rect rb{panel.x + (panel.w - (rw + ui(28))) / 2, panel.y + panel.h - ui(30), rw + ui(28), ui(24)};
    SDL_SetRenderDrawColor(renderer, 90, 30, 30, 255);
    SDL_RenderFillRect(renderer, &rb);
    SDL_SetRenderDrawColor(renderer, 235, 120, 120, 255);
    SDL_RenderDrawRect(renderer, &rb);
    text_.draw(rl, rb.x + (rb.w - rw) / 2, rb.y + (rb.h - rh) / 2, {255, 230, 210, 255}, rs);
    stats_return_rect_ = rb;
}

void GameClient::handle_stats_click(int mx, int my) {
    SDL_Point p{mx, my};
    if (SDL_PointInRect(&p, &stats_return_rect_)) {
        quit_to_menu_ = true;
        return;
    }
    for (auto& [rect, idx] : stats_tab_rects_) {
        if (SDL_PointInRect(&p, &rect)) {
            stats_tab_ = idx;
            return;
        }
    }
}

void GameClient::draw_stats_timeline(SDL_Renderer* renderer, const SDL_Rect& body) {
    const SDL_Color kInkDim{178, 158, 120, 255};
    const SDL_Color kInk{235, 224, 198, 255};
    const auto& tl = stats_.timeline;
    const auto& teams = stats_.teams;
    int nteam = static_cast<int>(teams.size());
    if (tl.size() < 2 || nteam == 0) {
        text_.draw("Not enough data for a population graph.", body.x, body.y + ui(4), kInkDim, ui(13));
        return;
    }
    text_regular_.draw("Population over time", body.x, body.y, kInkDim, ui(11));
    // Plot area (leave room for a legend row at the bottom).
    SDL_Rect plot{body.x + ui(22), body.y + ui(16), body.w - ui(30), body.h - ui(42)};
    SDL_SetRenderDrawColor(renderer, 22, 17, 11, 255);
    SDL_RenderFillRect(renderer, &plot);
    SDL_SetRenderDrawColor(renderer, 120, 100, 64, 255);
    SDL_RenderDrawRect(renderer, &plot);

    double tmax = std::max(1.0, stats_.elapsed_s);
    // Peak TOTAL population across all samples -> y scale.
    int peak_total = 1;
    for (const auto& s : tl) {
        int sum = 0;
        for (int i = 0; i < nteam; ++i) sum += s.pop[i];
        peak_total = std::max(peak_total, sum);
    }
    auto pop_at = [&](int team, double t) -> double {
        // Linear interpolation between bracketing samples.
        if (t <= tl.front().t) return tl.front().pop[team];
        if (t >= tl.back().t) return tl.back().pop[team];
        for (size_t k = 1; k < tl.size(); ++k) {
            if (t <= tl[k].t) {
                double a = tl[k - 1].t, b = tl[k].t;
                double f = (b > a) ? (t - a) / (b - a) : 0.0;
                return tl[k - 1].pop[team] + f * (tl[k].pop[team] - tl[k - 1].pop[team]);
            }
        }
        return tl.back().pop[team];
    };
    // Stacked area: for each pixel column, stack the teams bottom-to-top and
    // fill each team's band with its colour.
    for (int px = 0; px < plot.w; ++px) {
        double t = tmax * px / std::max(1, plot.w - 1);
        double acc = 0.0;
        int x = plot.x + px;
        for (int i = 0; i < nteam; ++i) {
            double v = pop_at(i, t);
            if (v <= 0) continue;
            double y0 = acc, y1 = acc + v;
            int py1 = plot.y + plot.h - static_cast<int>(y0 / peak_total * plot.h);
            int py0 = plot.y + plot.h - static_cast<int>(y1 / peak_total * plot.h);
            SDL_Color c = ww::menu::team_colours()[std::clamp(teams[i].colour, 0, 7)];
            SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 235);
            SDL_RenderDrawLine(renderer, x, py0, x, py1);
            acc = y1;
        }
    }
    // Age-up markers: a faint vertical line whenever ANY team reached a new era.
    static const char* kAgeMark[4] = {"", "I", "W", "S"};
    for (int era = 1; era <= 3; ++era) {
        double earliest = -1;
        for (int i = 0; i < nteam; ++i) {
            double a = teams[i].age_reached[era];
            if (a >= 0 && (earliest < 0 || a < earliest)) earliest = a;
        }
        if (earliest < 0) continue;
        int x = plot.x + static_cast<int>(earliest / tmax * plot.w);
        SDL_SetRenderDrawColor(renderer, 230, 210, 150, 120);
        SDL_RenderDrawLine(renderer, x, plot.y, x, plot.y + plot.h);
        text_regular_.draw(kAgeMark[era], x + 1, plot.y + 1, {230, 210, 150, 255}, ui(10));
    }
    // Y axis peak label + x axis end time.
    text_regular_.draw(std::to_string(peak_total), body.x, plot.y - ui(1), kInkDim, ui(10));
    text_regular_.draw("0", body.x + ui(6), plot.y + plot.h - ui(9), kInkDim, ui(10));
    text_regular_.draw(fmt_mmss(stats_.elapsed_s), plot.x + plot.w - ui(28), plot.y + plot.h + ui(2),
                       kInkDim, ui(10));
    // Legend row.
    int lx = plot.x, ly = plot.y + plot.h + ui(14);
    for (int i = 0; i < nteam; ++i) {
        SDL_Color c = ww::menu::team_colours()[std::clamp(teams[i].colour, 0, 7)];
        SDL_Rect sw{lx, ly, ui(8), ui(8)};
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
        SDL_RenderFillRect(renderer, &sw);
        std::string nm = teams[i].name;
        int nw, nh;
        text_regular_.measure(nm, ui(10), nw, nh);
        text_regular_.draw(nm, lx + ui(11), ly - ui(1), kInk, ui(10));
        lx += ui(11) + nw + ui(12);
        if (lx > plot.x + plot.w - ui(60)) { lx = plot.x; ly += ui(12); }
    }
}

void GameClient::handle_pause_menu_click(int mx, int my) {
    SDL_Point p{mx, my};
    if (SDL_PointInRect(&p, &pause_resume_rect_)) {
        pause_menu_open_ = false;
        paused_ = false;
    } else if (SDL_PointInRect(&p, &pause_quit_rect_)) {
        // Quitting shows the post-game statistics screen first (Return to Menu
        // there is what finally leaves the match). Reference: AoE2's end screen.
        pause_menu_open_ = false;
        open_stats_screen();
    }
}

void GameClient::draw_game_over_banner(SDL_Renderer* renderer) {
    const int W = view_w_;
    int by = top_bar_height() + ui(30);
    int bh = ui(78);
    SDL_Rect box{ui(30), by, W - ui(60), bh};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 12, 10, 8, 220);
    SDL_RenderFillRect(renderer, &box);
    SDL_Color edge = game_over_won_ ? SDL_Color{120, 230, 120, 255} : SDL_Color{235, 90, 90, 255};
    SDL_SetRenderDrawColor(renderer, edge.r, edge.g, edge.b, 255);
    SDL_RenderDrawRect(renderer, &box);

    std::string title = game_over_won_ ? "YOU ARE VICTORIOUS" : "DEFEAT";
    int ts = ui(24), tw, th;
    text_.measure(title, ts, tw, th);
    text_.draw(title, box.x + (box.w - tw) / 2, box.y + ui(8), edge, ts);

    // Two buttons: keep watching the map, or go to the statistics screen.
    auto btn = [&](SDL_Rect& r, int cx, const std::string& label, SDL_Color col) {
        int ls = ui(14), lw, lh;
        text_.measure(label, ls, lw, lh);
        r = SDL_Rect{cx - (lw + ui(24)) / 2, box.y + bh - ui(30), lw + ui(24), ui(22)};
        SDL_SetRenderDrawColor(renderer, col.r / 3, col.g / 3, col.b / 3, 255);
        SDL_RenderFillRect(renderer, &r);
        SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
        SDL_RenderDrawRect(renderer, &r);
        text_.draw(label, r.x + (r.w - lw) / 2, r.y + (r.h - lh) / 2, {245, 240, 230, 255}, ls);
    };
    btn(banner_watch_rect_, box.x + box.w / 3, "Keep Watching", {200, 200, 210, 255});
    btn(banner_stats_rect_, box.x + 2 * box.w / 3, "View Statistics", {255, 214, 96, 255});
}

bool GameClient::handle_game_over_click(int mx, int my) {
    SDL_Point p{mx, my};
    if (SDL_PointInRect(&p, &banner_watch_rect_)) {
        game_over_banner_open_ = false; // dismiss; keep watching the (still-live) map
        return true;
    }
    if (SDL_PointInRect(&p, &banner_stats_rect_)) {
        game_over_banner_open_ = false;
        open_stats_screen();
        return true;
    }
    return false;
}

void GameClient::open_stats_screen() {
    stats_ = ww::stats::compute_match_stats(match_);
    stats_tab_ = 0;
    stats_open_ = true;
    paused_ = true;
    // Reuse the existing win/lose SFX for the screen's sting (no dedicated
    // victory/defeat music exists). Only meaningful once the match is decided.
    if (stats_.decided) audio_.play(stats_.team0_won ? "victory" : "lose", 0, 0, false);
    audio_.stop_music();
}

void GameClient::draw_score_hud(SDL_Renderer* renderer) {
    // Bottom-right per-team scoreboard: "<leader>:<score> (<n>)" plus a
    // small era-number icon, one row per active team, ranked by score
    // (best at top) -- objects/control/Draw.gml's score display. The
    // original recomputed both the displayed score and the rank/row-order
    // only every 240 ticks (a perf compromise for an interpreted engine);
    // there's no reason not to just compute both fresh every frame here.
    Control& control = match_.control();
    int n = control.n;
    if (n <= 1) return; // nothing to compare in a 1-player match

    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::vector<double> current_score(n);
    for (int i = 0; i < n; ++i) {
        Team& t = control.teams[i];
        double res_total = 0.0;
        for (auto& [k, v] : t.res) res_total += v;
        current_score[i] = std::floor(t.score + res_total * 0.10);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        if (current_score[a] != current_score[b]) return current_score[a] > current_score[b];
        return a < b;
    });

    // F1 debug: live civilian count per team, for the resource readout below.
    std::vector<int> civ_count(n, 0);
    if (show_res_debug_) {
        for (auto ref : match_.world().active_units) {
            Unit* u = match_.world().get(ref);
            if (u && u->common.alive && u->name == "civilian" && u->common.team >= 0 &&
                u->common.team < n)
                ++civ_count[u->common.team];
        }
    }

    int row_h = ui(18);
    int icon_size = ui(16);
    int icon_x = view_w_ - ui(4) - icon_size; // era icon's left edge -- flush against the screen's right edge
    int right_x = icon_x - ui(2);             // text is right-aligned to just left of the icon
    int bottom_y = view_h_ - panel_height() - ui(4);
    int top_y = bottom_y - n * row_h;
    int text_size = ui(13);

    for (int rank = 0; rank < n; ++rank) {
        int team_i = order[rank];
        Team& t = control.teams[team_i];
        char buf[192];
        if (show_res_debug_) {
            // Resources as food/wood/oil/iron plus villager count -- for
            // diagnosing AI economy behaviour (F1 toggle).
            auto r = [&](const char* k) {
                auto it = t.res.find(k);
                return it == t.res.end() ? 0.0 : it->second;
            };
            std::snprintf(buf, sizeof(buf), "%s:%.0f (%d) %.0f/%.0f/%.0f/%.0f v%d",
                         ww::menu::leader_name(t.civ, t.leader).c_str(), current_score[team_i], team_i + 1,
                         r("food"), r("wood"), r("oil"), r("iron"), civ_count[team_i]);
        } else {
            std::snprintf(buf, sizeof(buf), "%s:%.0f (%d)",
                         ww::menu::leader_name(t.civ, t.leader).c_str(), current_score[team_i], team_i + 1);
        }
        int tw, th;
        text_.measure(buf, text_size, tw, th);
        int y = top_y + rank * row_h;
        // Black drop-shadow pass then the team's own colour on top, same
        // outlined-text look as the original's two draw_text_transformed
        // calls (Draw.gml:683-713).
        text_.draw(buf, right_x - tw + 1, y + 1, {0, 0, 0, 255}, text_size);
        SDL_Color col = ww::menu::team_colours()[t.colour];
        text_.draw(buf, right_x - tw, y, col, text_size);

        SDL_Rect era_rect{icon_x, y, icon_size, icon_size};
        atlas_.draw_in_rect(era_rect, "spr_era_icons_small", std::clamp(t.era, 0, 3), /*pad=*/0);
    }
}

void GameClient::draw_perf_overlay(SDL_Renderer* renderer) {
    World& world = match_.world();

    double avg_ms = 0.0, max_ms = 0.0;
    for (double t : tick_ms_history_) {
        avg_ms += t;
        max_ms = std::max(max_ms, t);
    }
    if (!tick_ms_history_.empty()) avg_ms /= static_cast<double>(tick_ms_history_.size());

    size_t units = 0, buildings = 0;
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (u && u->common.alive) ++units;
    }
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (b && b->common.alive) ++buildings;
    }

    char lines[6][64];
    std::snprintf(lines[0], sizeof(lines[0]), "FPS: %.1f", fps_);
    std::snprintf(lines[1], sizeof(lines[1]), "Sim step: %.2fms avg / %.2fms max", avg_ms, max_ms);
    std::snprintf(lines[2], sizeof(lines[2]), "Units: %zu", units);
    std::snprintf(lines[3], sizeof(lines[3]), "Buildings: %zu", buildings);
    std::snprintf(lines[4], sizeof(lines[4]), "Projectiles: %zu", world.active_projectiles.size());
    std::snprintf(lines[5], sizeof(lines[5]), "Client fx: %zu  Smoke: %zu", effects_.size(), smoke_.size());

    int line_h = ui(14), pad = ui(6);
    int box_w = ui(230), box_h = pad * 2 + line_h * 6;
    int box_x = ui(4), box_y = top_bar_height() + ui(4);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
    SDL_Rect box{box_x, box_y, box_w, box_h};
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &box);

    for (int i = 0; i < 6; ++i) {
        text_regular_.draw(lines[i], box_x + pad, box_y + pad + i * line_h, {255, 255, 0, 255}, ui(12));
    }
}

void GameClient::draw_spectator_stats(SDL_Renderer* renderer) {
    char fps_buf[32], time_buf[32];
    std::snprintf(fps_buf, sizeof(fps_buf), "FPS: %.0f", fps_);
    // render_clock_ is wall-clock seconds since this match's construction,
    // advanced every unpaused frame (see update()) -- pausing (the pause
    // button still works in spectator mode, see handle_left_down) freezes
    // it right along with the sim/AI, same as it would for a real match, so
    // this always reads as the preview's actual elapsed active time.
    std::snprintf(time_buf, sizeof(time_buf), "Time: %.1fs", render_clock_);

    int size = ui(14);
    // Fixed box size, NOT measured from the live text -- the FPS/Time
    // strings change width as the numbers change (57 -> 108, 9.9s ->
    // 10.0s, ...), which would make a text-measured box visibly resize
    // every frame. Sized instead off a worst-case reference string, wide
    // enough for both lines to always fit comfortably; text height doesn't
    // vary with content at a fixed font size, so reusing rh as the line
    // pitch for both lines is safe.
    int rw, rh;
    text_.measure("Time: 999.9s", size, rw, rh);
    // Small black box, top-left, just below the resource bar -- same corner
    // draw_perf_overlay (F3) uses. Two lines (FPS, then Time) instead of
    // one.
    int pad = ui(6);
    int box_x = ui(4), box_y = top_bar_height() + ui(4);
    int box_w = rw + pad * 2, box_h = rh * 2 + pad * 2;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
    SDL_Rect box{box_x, box_y, box_w, box_h};
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &box);
    text_.draw(fps_buf, box_x + pad, box_y + pad, {255, 255, 0, 255}, size);
    text_.draw(time_buf, box_x + pad, box_y + pad + rh, {255, 255, 0, 255}, size);
}

void GameClient::draw_minimap(SDL_Renderer* renderer) {
    // Placeholder minimap: terrain (muted green land / blue water) with the
    // camera's current viewport traced on it, proportional to world size,
    // plus a small dot per building in its owning team's colour. Real
    // unit rendering into the minimap is a follow-up, not this pass.
    World& world = match_.world();
    int kSize = ui(88); // larger minimap -- fits the 3-unit-tall (96 native px) panel
    int mx0 = view_w_ - kSize - ui(4), my0 = view_h_ - panel_height() + ui(2);
    minimap_rect_ = SDL_Rect{mx0, my0, kSize, kSize};

    double sx = kSize / world.px_w, sy = kSize / world.px_h;

    // Terrain + forests + fog, composited into a single world-grid-resolution
    // pixel buffer and blitted once. This used to be TWO full-map loops of
    // per-tile SDL_RenderFillRect (terrain water + fog overlay, most of the
    // latter alpha-blended) -- tens of thousands of draw calls per frame at
    // large map sizes, which profiling pinned as the single biggest frame
    // cost (removing the minimap alone nearly doubled stress-test FPS). CPU
    // memory writes + one texture upload + one scaled blit are ~orders of
    // magnitude cheaper and pixel-identical (nearest-neighbour scaling keeps
    // the crisp per-tile look). muted green land / blue water, darker green
    // forests, fog: unexplored solid, explored translucent, visible clear.
    const int mcols = world.cols, mrows = world.rows;
    if (!minimap_bg_tex_ || minimap_bg_cols_ != mcols || minimap_bg_rows_ != mrows) {
        if (minimap_bg_tex_) SDL_DestroyTexture(minimap_bg_tex_);
        minimap_bg_tex_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_STREAMING, mcols, mrows);
        SDL_SetTextureScaleMode(minimap_bg_tex_, SDL_ScaleModeNearest);
        minimap_bg_cols_ = mcols;
        minimap_bg_rows_ = mrows;
        minimap_bg_px_.assign(static_cast<size_t>(mcols) * mrows, 0);
    }
    auto pack = [](int r, int g, int b) {
        return 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
               static_cast<uint32_t>(b);
    };
    uint32_t* px = minimap_bg_px_.data();
    const uint32_t kLand = pack(70, 110, 60), kWater = pack(40, 90, 165), kForest = pack(42, 78, 38);
    for (int ty = 0; ty < mrows; ++ty) {
        uint32_t* row = px + static_cast<size_t>(ty) * mcols;
        for (int tx = 0; tx < mcols; ++tx)
            row[tx] = (world.terrain[tx][ty] == WATER) ? kWater : kLand;
    }
    for (auto ref : world.active_resources) {
        Resource* r = world.get_resource(ref);
        if (!r || !r->common.alive || (r->name != "tree" && r->name != "palm")) continue;
        int tx = static_cast<int>(r->common.x / world.px_w * mcols);
        int ty = static_cast<int>(r->common.y / world.px_h * mrows);
        if (tx >= 0 && tx < mcols && ty >= 0 && ty < mrows) px[static_cast<size_t>(ty) * mcols + tx] = kForest;
    }
    // Fog composite: sample each tile's centre fog cell. f==2 clear, f==0
    // fully dark, f==1 dark blended at ~47% (120/255), matching the old
    // per-tile alpha overlay exactly.
    for (int ty = 0; ty < mrows; ++ty) {
        uint32_t* row = px + static_cast<size_t>(ty) * mcols;
        for (int tx = 0; tx < mcols; ++tx) {
            int f = world.fog[tx * kFogSubdiv + kFogSubdiv / 2][ty * kFogSubdiv + kFogSubdiv / 2];
            if (f == 2) continue;
            if (f == 0) {
                row[tx] = pack(8, 10, 14);
            } else {
                uint32_t t = row[tx];
                int tr = (t >> 16) & 0xFF, tg = (t >> 8) & 0xFF, tb = t & 0xFF;
                constexpr int a = 120;
                row[tx] = pack((tr * (255 - a) + 8 * a) / 255, (tg * (255 - a) + 10 * a) / 255,
                               (tb * (255 - a) + 14 * a) / 255);
            }
        }
    }
    SDL_UpdateTexture(minimap_bg_tex_, nullptr, px, mcols * static_cast<int>(sizeof(uint32_t)));
    SDL_RenderCopy(renderer, minimap_bg_tex_, nullptr, &minimap_rect_);
    SDL_SetRenderDrawColor(renderer, 225, 205, 40, 255);
    SDL_RenderDrawRect(renderer, &minimap_rect_);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    auto& teams = match_.control().teams;
    for (auto ref : world.active_buildings) {
        Building* b = world.get_building(ref);
        if (!b || !b->common.alive) continue;
        // Own buildings always show; enemy buildings only while a tile is
        // CURRENTLY visible (fog==2) -- stricter than the main view (which
        // lets explored-but-not-visible enemy buildings persist), matching
        // ui.py's minimap dot rule exactly.
        if (b->common.team != 0 && world.fog_at(b->common.x, b->common.y) != 2) continue;
        // Each team's own chosen colour (same palette/index as the Random
        // Map Setup screen's colour swatches) rather than a flat own=white/
        // enemy=red scheme -- previously indistinguishable which of several
        // enemies (e.g. all 7 AI teams in an 8-player stress test) a given
        // building belonged to.
        SDL_Color col = {255, 255, 255, 255};
        if (b->common.team >= 0 && b->common.team < static_cast<int>(teams.size())) {
            col = ww::menu::team_colours()[teams[b->common.team].colour];
        }
        SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
        SDL_Rect dot{mx0 + static_cast<int>(b->common.x * sx), my0 + static_cast<int>(b->common.y * sy), 2, 2};
        SDL_RenderFillRect(renderer, &dot);
    }

    // Units, both players: a 2px dot per unit in its team colour. Own units
    // always show; enemy units only where a tile is CURRENTLY visible (fog==2),
    // the same rule the building dots use. Garrisoned (transported) units are
    // hidden, matching the main view.
    for (auto ref : world.active_units) {
        Unit* u = world.get(ref);
        if (!u || !u->common.alive || u->carrier.valid()) continue;
        if (u->common.team != 0 && world.fog_at(u->common.x, u->common.y) != 2) continue;
        SDL_Color col = {255, 255, 255, 255};
        if (u->common.team >= 0 && u->common.team < static_cast<int>(teams.size())) {
            col = ww::menu::team_colours()[teams[u->common.team].colour];
        }
        SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
        SDL_Rect dot{mx0 + static_cast<int>(u->common.x * sx), my0 + static_cast<int>(u->common.y * sy), 2, 2};
        SDL_RenderFillRect(renderer, &dot);
    }

    // Attack pings, on top of the dots so they can't be hidden by one. A
    // pulsing hollow red box rather than a filled dot: filled would just look
    // like another unit, and the whole point is that it has to catch the eye
    // while the player is looking somewhere else on the map.
    for (const auto& p : minimap_pings_) {
        double phase = p.t * 4.0;
        int grow = static_cast<int>(2.0 + 3.0 * (0.5 + 0.5 * std::sin(phase)));
        int fade = static_cast<int>(255.0 * std::max(0.0, 1.0 - p.t / p.lifetime));
        SDL_SetRenderDrawColor(renderer, 255, 60, 60, static_cast<Uint8>(fade));
        SDL_Rect box{mx0 + static_cast<int>(p.x * sx) - grow, my0 + static_cast<int>(p.y * sy) - grow,
                     grow * 2, grow * 2};
        SDL_RenderDrawRect(renderer, &box);
    }

    auto vr = cam_.visible_rect();
    SDL_Rect viewport{mx0 + static_cast<int>(vr.x * sx), my0 + static_cast<int>(vr.y * sy),
                     std::max(2, static_cast<int>(vr.w * sx)), std::max(2, static_cast<int>(vr.h * sy))};
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &viewport);
}

void GameClient::draw_command_card(SDL_Renderer* renderer) {
    card_buttons_.clear();
    queue_buttons_.clear();
    if (selected_.empty()) return;
    World& world = match_.world();
    Control& ctrl = match_.control();
    int panel_top = view_h_ - panel_height();

    Building* building = world.get_building(selected_[0]);
    // Single gate for both places below that would otherwise independently
    // check `building && building->common.team == 0` -- see own_unit's
    // comment just below for why spectator_ forces this false regardless
    // of team.
    // view_team() so the production queue of whoever is being watched is the
    // one drawn. Card BUTTONS are separately refused while pov_active() (see
    // handle_left_down) -- being able to read an AI's queue is the point;
    // being able to click Train in it is not.
    bool own_building = building && building->common.team == view_team() && !spectator_;
    // Fixed unit-context buttons (attack-move, delete, build-eco/military)
    // only show when they're common to EVERY selected unit, not just the
    // first -- e.g. a mixed civilian+cavalry selection has no build option,
    // since cavalry can't build. Buildings still only ever look at
    // selected_[0] (see `building` above) -- units and buildings are never
    // selected together in normal play.
    //
    // Spectator mode (Start Stress Test preview) forces these false
    // regardless of team, including team 0 -- during a normal match team 0
    // IS the player, but a spectator preview has no player at all (every
    // team is AI, see SkirmishSettings::spectator), so team 0's units/
    // buildings aren't "mine" there either. This alone hides every command-
    // card BUTTON; the separate unit/building info column (draw_hud) is
    // untouched, so a spectator can still select and inspect anything.
    bool own_unit = !selected_.empty() && !spectator_;
    bool own_civilian = own_unit;
    for (auto ref : selected_) {
        Unit* su = world.get(ref);
        if (!su || su->common.team != 0) { own_unit = false; own_civilian = false; break; }
        if (su->name != "civilian") own_civilian = false;
    }
    // A civilian with a build category active shows that category's
    // building list instead of its default attack-move/build-category menu
    // (see the task note: specific building buttons only appear after
    // build-eco/build-military is clicked, not immediately on selection).
    bool show_building_list = own_civilian && !building_category_.empty();

    int bsize = ui(kUnit);
    // Hovered button this frame (kind/item), consulted after every button is
    // laid out so the tooltip draws on top of the whole card -- unlike
    // clicks (which hit-test against LAST frame's rects), hover can be
    // computed in the same frame since draw_button already knows its own
    // rect and mouse_pos_ before it draws.
    std::optional<CardButton> hovered;
    auto draw_button = [&](int col, int row, const std::string& icon, const char* kind,
                           const std::string& item, bool active = false,
                           const char* button_sprite = "spr_button") {
        SDL_Rect rect{col * bsize, panel_top + row * bsize, bsize, bsize};
        atlas_.draw_stretched(button_sprite, rect);
        int ipad = ui(4);
        SDL_Rect icon_rect{rect.x + ipad, rect.y + ipad, rect.w - 2 * ipad, rect.h - 2 * ipad};
        // Unit train icons are 8-frame team-coloured sheets, so draw them in the
        // local player's own colour (draw_in_rect clamps, so single-frame icons
        // for techs/buildings/actions harmlessly stay on frame 0).
        int icon_frame = (std::string(kind) == "train") ? ctrl.teams[0].colour : 0;
        bool has_icon = atlas_.draw_in_rect(icon_rect, icon, icon_frame, /*pad=*/0);
        if (!has_icon) {
            // Centre the text label within the button (e.g. the formation
            // selector's Col/Stag/Box, or the ballistic Pack/Unpack). Uses the
            // SCALABLE font (text_regular_) and shrinks it until it fits the
            // button width -- the pixel font (text_) can't render below its 11px
            // strike, so a longer word like "Unpack" spilled out past the box.
            int fs = ui(11), tw = 0, th = 0;
            int max_w = rect.w - ui(3);
            text_regular_.measure(item, fs, tw, th);
            while (tw > max_w && fs > ui(6)) {
                --fs;
                text_regular_.measure(item, fs, tw, th);
            }
            text_regular_.draw(item, rect.x + (rect.w - tw) / 2, rect.y + (rect.h - th) / 2,
                               {235, 235, 225, 255}, fs);
        }
        if (active) {
            SDL_SetRenderDrawColor(renderer, 255, 210, 60, 255);
            SDL_RenderDrawRect(renderer, &rect);
        }
        card_buttons_.push_back({rect, kind, item, col, row});
        if (SDL_PointInRect(&mouse_pos_, &rect)) hovered = card_buttons_.back();
    };

    // All command-card buttons live on the left side of the panel, in
    // reading order (row-major, wrapping after kMaxCols); the unit-info
    // block (flag/name/HP/stats) starts well clear of them at stats_col_x().
    // 5 wide so every column lines up with a QWERT (row 0) / ASDFG (row 1)
    // hotkey -- see handle_hotkey.
    constexpr int kMaxCols = 5;

    // Build goes on the top (QWERT) row and attack-move on the bottom
    // (ASDFG) row, EXCEPT while a civilian's build submenu is open (that
    // slot becomes the "back" button instead -- see below).
    if (own_civilian && building_category_.empty()) {
        draw_button(0, 0, "spr_build_icon", "build_eco", "");
        draw_button(1, 0, "spr_build_military_icon", "build_military", "");
    }
    if (own_unit && !show_building_list) {
        draw_button(0, 1, "spr_attack_move_icon", "attack_move", "", attack_move_armed_);
        // 5th slot, top row -- hotkey is Delete, not this slot's usual T
        // (see handle_hotkey). Column 4 is otherwise unused by any
        // unit-context button, so this never collides with build_eco/
        // build_military (cols 0-1) or a building's own train/tech items
        // (which only ever draw when a Building, not a Unit, is selected).
        draw_button(4, 0, "spr_skull_icon", "delete", "");
        // Land button for air units: fly to the nearest own airbase and set
        // down there (refuel/rearm). Only shown when an aircraft is selected.
        bool own_air = false, own_ag = false, own_nuke = false, own_transport = false;
        for (auto ref : selected_) {
            Unit* u = match_.world().get(ref);
            if (!u || u->common.team != 0 || !u->common.alive) continue;
            if (u->common.is_air) own_air = true;
            if (can_attack_ground(u)) own_ag = true;
            if (u->nuke_loaded) own_nuke = true; // carries an auto-loaded atomic bomb
            if (u->transport_cap > 0 && !u->cargo.empty()) own_transport = true;
        }
        if (own_air) draw_button(1, 1, "spr_land_icon", "land", "", land_armed_);
        // Unload button for a laden transport ship: arms a "click a shoreline"
        // mode (see activate_card_button / handle_left_down's unload_armed_).
        if (own_transport)
            draw_button(1, 0, "spr_transport_ship_icon", "unload", "", unload_armed_);
        // Attack-ground (artillery / bombers / ohka): bombard a fixed point.
        // Hotkey is T (handle_hotkey), not this slot's usual grid letter.
        if (own_ag) draw_button(2, 1, "spr_attack_target_icon", "attack_ground", "", attack_ground_armed_);
        // Nuke indicator (not a button anymore -- loading is automatic on
        // landing at a stocked airbase): shows the plane is carrying a nuke.
        if (own_nuke) draw_button(3, 1, "spr_atomic_bomb_icon", "nuke_indicator", "");
        // Ballistic missile Pack/Unpack. Only when EVERY selected unit is a
        // ballistic launcher (so the one button's action is unambiguous). Shows
        // "Unpack" (deploy to fire) while any are still packed, else "Pack"
        // (stow to move). Sits at (3,1) -- free for a lone launcher (attack_move
        // 0,1 / attack_ground 2,1 / delete 4,0) and for a pure-ballistic group
        // (formations take the TOP row, so bottom (3,1) stays open).
        bool all_ballistic = true, any_packed = false, any_unpacked = false;
        for (auto ref : selected_) {
            Unit* u = match_.world().get(ref);
            if (!u || u->common.team != 0 || !u->common.alive) continue;
            if (!u->is_ballistic) { all_ballistic = false; break; }
            if (u->pack_t <= 0.0) { (u->packed ? any_packed : any_unpacked) = true; }
        }
        if (all_ballistic && (any_packed || any_unpacked)) {
            if (any_packed) draw_button(3, 1, "", "unpack", "Unpack");
            else draw_button(3, 1, "", "pack", "Pack");
        }
        // Formation shape selectors -- one button per shape now (real icons
        // exist, unlike the old single cycling button), right of attack_move
        // (0,1), only meaningful for a group (2+ units). Each directly sets
        // formation_type_ for the next right-drag/group move
        // (activate_card_button) instead of cycling; the currently active
        // shape is highlighted (gold outline, same `active` param land/
        // attack_ground/etc already use) rather than shown as a text label.
        // Column/Box/Stagger are individually skipped whenever that specific
        // cell is already claimed this frame by a more specialized
        // per-unit-type button above, so e.g. a multi-fighter group still
        // gets its Land button rather than losing it to the Column formation
        // button (see Settings::formation_column_key's Grid default
        // deliberately sharing land_key's S -- they can never both be drawn
        // at once). Split's (4,1) slot is never claimed by anything else in
        // this branch, so it always shows unconditionally alongside the rest.
        if (selected_.size() >= 2) {
            // A pure military group (no civilian build buttons, no laden-
            // transport unload button occupying the top row) puts all four
            // formation icons on the TOP row, cols 0-3, completely clear of the
            // bottom-row per-unit action buttons. This is what fixes the
            // target-ground vs formation conflict: an artillery group keeps its
            // attack-ground button at (2,1) AND every formation option, instead
            // of losing box-formation to the shared cell. Delete stays at (4,0).
            bool formations_top = !own_civilian && !own_transport;
            if (formations_top) {
                draw_button(0, 0, "spr_icon_row", "formation_column", "", formation_type_ == "column");
                draw_button(1, 0, "spr_icon_box", "formation_box", "", formation_type_ == "box");
                draw_button(2, 0, "spr_icon_stagger", "formation_stagger", "",
                           formation_type_ == "staggered");
                draw_button(3, 0, "spr_icon_split", "formation_split", "", formation_type_ == "split");
            } else {
                // Group includes civilians / a laden transport (top row is
                // claimed): keep formations on the bottom row, each skipped when
                // a more specialized button already holds its cell.
                if (!own_air) {
                    draw_button(1, 1, "spr_icon_row", "formation_column", "", formation_type_ == "column");
                }
                if (!own_ag) {
                    draw_button(2, 1, "spr_icon_box", "formation_box", "", formation_type_ == "box");
                }
                if (!own_nuke) {
                    draw_button(3, 1, "spr_icon_stagger", "formation_stagger", "",
                               formation_type_ == "staggered");
                }
                draw_button(4, 1, "spr_icon_split", "formation_split", "", formation_type_ == "split");
            }
        }
    }

    // Age-up: a base-only, always-fixed-position button (mirrors
    // attack_move's pattern) rather than a dynamic queue item -- World::
    // enqueue already has correct age-item gating (world.cpp: idx<team.era/
    // already-queued/another-age-already-queued), so this just needs to
    // show the right one of the 3 age techs for the team's CURRENT era and
    // fire it through the same "tech" activate_card_button path everything
    // else uses. Era isn't era-gated in TECH_ERA/TECH_PREREQ the way normal
    // techs are (there's exactly one age tech valid at a time, driven by
    // team.era directly), so it can't just be added to building_techs_ and
    // left to available_techs() -- that would list all 3 ages at once.
    static const char* kAgeItems[3] = {"industrial", "war", "scientific"};
    static const char* kAgeIcons[3] = {"spr_industrial_icon", "spr_war_icon", "spr_scientific_icon"};
    bool show_age_up = own_building && building->complete &&
                       building->name == "base" && ctrl.teams[0].era < 3;
    if (show_age_up) {
        int era = ctrl.teams[0].era;
        // Direct port of obj_button/Draw.gml: the age-up button always uses
        // the red-bordered button sprite, unlike every other command-card
        // button.
        draw_button(0, 1, kAgeIcons[era], "tech", kAgeItems[era], /*active=*/false, "spr_button_red");
    }

    // Flat row-major index, skipping the (0,1) slot when age-up occupies it
    // so a regular queue item never gets drawn underneath/instead of it.
    // Still used for training-queue units (not reported as shifting, and
    // Control::available_units() already resolves upgrade chains/dedups
    // for us) and for the building list, which now supplies its OWN fixed
    // per-name position instead of a plain running index.
    int reserved_flat = show_age_up ? (1 * kMaxCols + 0) : -1;
    auto flat_to_grid = [&](int flat) {
        if (reserved_flat >= 0 && flat >= reserved_flat) flat += 1;
        return std::pair<int, int>{flat % kMaxCols, flat / kMaxCols};
    };

    // The shipyard shows ALL its ships (fishing boat, transport, warships, both
    // carrier tiers) and techs on ONE card now -- the 3-row command card has room
    // for the whole yard, so the old passive/warship page split (and its toggle
    // button) is gone. ship_page_ok is kept as a no-op so the two call sites read
    // unchanged; off-card overflow is caught by the collision guard below.
    auto ship_page_ok = [&](const std::string&, bool) { return true; };

    if (own_building && building->complete) {
        // Market: a 2x3 trade table -- BUY row (top) over a SELL row (bottom),
        // one column per commodity (food / wood / iron). OIL is the currency:
        // you sell a commodity FOR oil and buy it WITH oil (100 units a trade).
        // Rates/fees are in Control::trade; the tooltip shows the live cost.
        // The Trade Agreement tech is pinned clear of the table (see
        // tech_position_overrides).
        if (building->name == "market" && market_page_ == 0) {
            static const char* kRes[3] = {"food", "wood", "iron"};
            for (int c = 0; c < 3; ++c) {
                draw_button(c, 0, std::string("spr_buy_") + kRes[c] + "_icon", "trade",
                           std::string("buy ") + kRes[c]);
                draw_button(c, 1, std::string("spr_sell_") + kRes[c] + "_icon", "trade",
                           std::string("sell ") + kRes[c]);
            }
            // Auto-replant toggle: when ON, a farm worked to exhaustion is
            // instantly re-sown (40 wood) so the farmer keeps going. Shown with
            // a 2px green border while active.
            draw_button(3, 1, "spr_replant", "replant", "");
            if (own_building && ctrl.teams[0].replant) {
                SDL_Rect rr{3 * bsize, panel_top + 1 * bsize, bsize, bsize};
                SDL_SetRenderDrawColor(renderer, 60, 220, 70, 255);
                SDL_RenderDrawRect(renderer, &rr);
                SDL_Rect rr2{rr.x + 1, rr.y + 1, rr.w - 2, rr.h - 2};
                SDL_RenderDrawRect(renderer, &rr2);
            }
        }
        auto units = ctrl.available_units(building->name, 0);
        std::unordered_map<std::string, GridPos> unit_pos; // name -> slot, for tech placement below
        int slot = 0;
        for (size_t i = 0; i < units.size(); ++i) {
            if (!ship_page_ok(units[i], /*is_tech=*/false)) continue;
            auto [col, row] = flat_to_grid(slot++);
            unit_pos[units[i]] = {col, row};
            draw_button(col, row, item_icon(units[i]), "train", units[i]);
        }

        // Techs: FIXED positions so hotkeys never shift after researching
        // something -- either hand-placed (factory/airbase, see
        // tech_position_overrides) or this building's own static index
        // within Control::building_techs() (that list itself never
        // reorders, unlike the "currently researchable" filtered list
        // available_techs() returns, which is what used to drive
        // positions directly). unit_slot_reserve() leading slots are
        // skipped so a tech can never land on top of a training button.
        static const std::vector<std::string> kNoTechs;
        auto bt_it = ctrl.building_techs().find(building->name);
        const std::vector<std::string>& full_techs = bt_it != ctrl.building_techs().end() ? bt_it->second : kNoTechs;
        auto avail_techs = ctrl.available_techs(building->name, 0);
        std::set<std::string> avail_set(avail_techs.begin(), avail_techs.end());
        const auto* overrides = tech_position_overrides(building->name);
        int tech_base = unit_slot_reserve(building->name);
        // A tech that's already queued/researching is shown ONLY by the research
        // progress bar, so its grid icon comes off the card until it finishes,
        // is cancelled, or its building dies (see techs_in_progress). Its SLOT
        // is still reserved below rather than freed, so the neighbouring techs
        // that rely on the collision-bump fallback don't slide into the gap and
        // back out again as research starts and stops.
        const std::set<std::string> in_progress = techs_in_progress(0);
        std::vector<GridPos> reserved_cells;
        for (size_t i = 0; i < full_techs.size(); ++i) {
            const std::string& tech = full_techs[i];
            if (!avail_set.count(tech)) continue;
            if (!ship_page_ok(tech, /*is_tech=*/true)) continue; // hide off-page shipyard techs
            if (building->name == "market" && market_page_ == 0) continue; // market techs on page 1 only
            int col, row;
            bool placed = false;
            // 1) Hand-placed slot (factory/airbase) wins.
            auto ov = overrides ? overrides->find(tech) : std::unordered_map<std::string, GridPos>::const_iterator{};
            if (overrides && ov != overrides->end()) {
                col = ov->second.col;
                row = ov->second.row;
                placed = true;
            }
            // 2) Otherwise an individual-unit upgrade sits directly BELOW the
            //    unit it improves (the whole rifle chain shares one slot).
            if (!placed) {
                if (const auto* parents = tech_parent_units(tech)) {
                    for (const auto& pu : *parents) {
                        auto pit = unit_pos.find(pu);
                        if (pit != unit_pos.end() && pit->second.row + 1 <= 2) { // fits the 3-row card
                            col = pit->second.col;
                            row = pit->second.row + 1;
                            // The barracks firearm chain (bolt-action -> semi-
                            // auto -> assault) sits one column to the RIGHT of
                            // the Artillery Upgrade (which is under the artillery
                            // unit), so rifle upgrade / artillery upgrade /
                            // firearm chain each get their own row-1 slot.
                            static const std::set<std::string> kFirearmChain = {
                                "bolt action rifle", "semi automatic rifle", "assault rifle"};
                            if (kFirearmChain.count(tech)) {
                                auto ait = unit_pos.find("artillery1");
                                if (ait == unit_pos.end()) ait = unit_pos.find("artillery");
                                int base_col = (ait != unit_pos.end()) ? ait->second.col : col;
                                col = std::min(kMaxCols - 1, base_col + 1);
                                row = 1;
                            }
                            placed = true;
                            break;
                        }
                    }
                }
            }
            // 3) Fallback: this building's own static tech index.
            if (!placed) std::tie(col, row) = flat_to_grid(tech_base + static_cast<int>(i));
            // General collision guard: if the computed cell is already taken by a
            // unit or an earlier tech drawn THIS frame, bump this tech to the first
            // free cell so two icons never stack (reported for binoculars vs the
            // firearm chain, blowback reload, diesel vs Tiger II, etc.). Stable
            // when there's no collision -- only a genuine overlap moves.
            {
                auto cell_taken = [&](int c, int r) {
                    for (auto& cb : card_buttons_)
                        if (cb.col == c && cb.row == r) return true;
                    // Cells held open for techs hidden while researching count
                    // as taken -- they draw no button, so card_buttons_ alone
                    // would report them free.
                    for (const auto& rc : reserved_cells)
                        if (rc.col == c && rc.row == r) return true;
                    return false;
                };
                // Bump to the first free on-card cell if the computed slot is
                // taken OR off the 3x5 card entirely (the flat fallback can
                // overflow when a building has many techs -- e.g. the whole
                // shipyard now sharing one page).
                if (cell_taken(col, row) || row < 0 || row >= 3 || col < 0 || col >= kMaxCols) {
                    bool found = false;
                    for (int r = 0; r < 3 && !found; ++r)
                        for (int c = 0; c < kMaxCols && !found; ++c)
                            if (!cell_taken(c, r)) { col = c; row = r; found = true; }
                }
            }
            if (in_progress.count(tech)) {
                reserved_cells.push_back({col, row}); // hidden, but keeps its slot
                continue;
            }
            draw_button(col, row, item_icon(tech), "tech", tech);
        }
        // Once "atomic bomb" is researched it drops out of the tech list; its
        // (4,1) slot becomes a persistent "build a nuke" button that adds one
        // atomic bomb to THIS airbase's stockpile (auto-loaded by a landing
        // heavy bomber / b29).
        if (building->name == "airbase" && ctrl.has_tech("atomic bomb", 0)) {
            draw_button(4, 1, "spr_atomic_bomb_icon", "build_nuke", "");
        }
        // Airbase "park new planes" toggle (spr_land_icon, gold when ON): newly
        // built planes stay parked here instead of launching. Placed in a free
        // card cell -- prefers (4,0), else the first free slot -- so it never
        // collides with the era-filtered plane/tech buttons above.
        if (building->name == "airbase") {
            auto cell_taken = [&](int c, int r) {
                for (auto& cb : card_buttons_)
                    if (cb.col == c && cb.row == r) return true;
                return false;
            };
            int pc = 4, pr = 0;
            if (cell_taken(pc, pr)) {
                pc = -1;
                for (int r = 0; r < 3 && pc < 0; ++r)
                    for (int c = kMaxCols - 1; c >= 0; --c)
                        if (!cell_taken(c, r)) { pc = c; pr = r; break; }
            }
            if (pc >= 0)
                draw_button(pc, pr, "spr_land_icon", "toggle_park_planes", "", building->park_new_planes);
        }
        // (The shipyard page toggle is gone -- the whole yard shows on one card.)
        // Market page toggle (bottom-right, mirrors the shipyard's): flips
        // between the trade table (page 0) and the market techs (page 1). The
        // icon shows the page you'd switch TO.
        if (building->name == "market") {
            // page 0 shows a tech-tree icon (go to techs); page 1 shows the
            // market building icon (go back to trading).
            const char* icon = market_page_ == 0 ? "spr_tech_tree_icon" : "spr_market_icon";
            const char* label = market_page_ == 0 ? "Tech" : "Trade";
            draw_button(4, 1, icon, "market_page", label);
        }
    } else if (show_building_list) {
        const auto& allowed = building_category_ == "eco" ? eco_buildings() : military_buildings();
        const auto& layout = building_category_ == "eco" ? eco_building_layout() : military_building_layout();
        auto avail = ctrl.available_buildings(0);
        std::set<std::string> avail_set(avail.begin(), avail.end());
        for (auto& [name, pos] : layout) {
            if (!allowed.count(name) || !avail_set.count(name)) continue;
            draw_button(pos.col, pos.row, item_icon(name), "build", name);
        }
    }

    // Alternates the building-list submenu between eco/military (see
    // handle_left_down's "build_back" case). Fixed at the G slot (bottom
    // row, last column) for a stable hotkey; safe from collision since the
    // longest building list (military, 7 items) only fills row 1 up to
    // col 1.
    if (show_building_list) {
        int col = kMaxCols - 1, row = 1;
        SDL_Rect rect{col * bsize, panel_top + row * bsize, bsize, bsize};
        atlas_.draw_stretched("spr_button", rect);
        int ipad = ui(4);
        SDL_Rect icon_rect{rect.x + ipad, rect.y + ipad, rect.w - 2 * ipad, rect.h - 2 * ipad};
        atlas_.draw_in_rect(icon_rect, "spr_back_arrow", /*frame=*/0, /*pad=*/0);
        card_buttons_.push_back({rect, "build_back", "", col, row});
        if (SDL_PointInRect(&mouse_pos_, &rect)) hovered = card_buttons_.back();
    }

    if (hovered) draw_item_tooltip(renderer, *hovered, panel_top);

    // Production queue grid: everything queued on the selected building
    // (train/tech/age items alike, in order), positioned along the panel's
    // right side clear of both the button grid and the stats column. 2
    // rows x 5 cols max (10 slots) -- anything queued beyond that just
    // isn't drawn (still exists/still processes in order, there's just no
    // icon for it). Clicking a slot cancels it (World::cancel_queue),
    // refunding its cost. Full-size (kUnit, i.e. the same 32x32 as a
    // command-card button) with no gap between slots -- matches the
    // button grid's own look instead of being a smaller, spaced-out strip.
    // 2 rows of 32 exactly fills the panel's height, so this starts flush
    // at panel_top with no top margin.
    if (building && building->common.team == 0 && !building->queue.empty()) {
        constexpr int kQueueCols = 5, kQueueRows = 2, kQueueMax = kQueueCols * kQueueRows;
        int qsize = ui(kUnit);
        int qx = queue_col_x();
        int qy = panel_top;
        for (size_t i = 0; i < building->queue.size() && i < static_cast<size_t>(kQueueMax); ++i) {
            int qcol = static_cast<int>(i) % kQueueCols, qrow = static_cast<int>(i) / kQueueCols;
            SDL_Rect qrect{qx + qcol * qsize, qy + qrow * qsize, qsize, qsize};
            atlas_.draw_stretched("spr_button", qrect);
            int ipad = ui(4);
            SDL_Rect irect{qrect.x + ipad, qrect.y + ipad, qrect.w - 2 * ipad, qrect.h - 2 * ipad};
            // Unit icons take the owner's colour; techs take frame 0 -- see
            // item_icon_frame, and the rifleman-upgrade sheet it exists for.
            atlas_.draw_in_rect(irect, item_icon(building->queue[i]),
                                item_icon_frame(building->queue[i]), /*pad=*/0);
            // The in-progress (front) slot's percent now shows as a much
            // bigger bar+text in the stats column (see draw_hud, which
            // replaces a building's armor readout with it) -- no separate
            // tiny progress strip needed here anymore.
            queue_buttons_.push_back({qrect, selected_[0], static_cast<int>(i)});
        }
    }
}

std::string GameClient::hotkey_label_for(const CardButton& btn) const {
    if (btn.kind == "build_eco") return Settings::key_label(settings_.build_eco_key);
    if (btn.kind == "build_military") return Settings::key_label(settings_.build_military_key);
    if (btn.kind == "attack_move") return Settings::key_label(settings_.attack_move_key);
    if (btn.kind == "formation_column") return Settings::key_label(settings_.formation_column_key);
    if (btn.kind == "formation_box") return Settings::key_label(settings_.formation_box_key);
    if (btn.kind == "formation_stagger") return Settings::key_label(settings_.formation_stagger_key);
    if (btn.kind == "formation_split") return Settings::key_label(settings_.formation_split_key);
    if (btn.kind == "land") return Settings::key_label(settings_.land_key);
    if (btn.kind == "unload") return Settings::key_label(settings_.unload_key);
    if (btn.kind == "shipyard_page") return Settings::key_label(settings_.shipyard_page_key);
    if (btn.kind == "build_nuke") return Settings::key_label(settings_.build_nuke_key);
    if (btn.kind == "build_back") return Settings::key_label(settings_.build_back_key);
    if (btn.kind == "delete") return Settings::key_label(SDLK_DELETE); // its own fixed key, not rebindable
    if (btn.kind == "build") {
        // The building's own construction key (settings_.construction_keys,
        // a separate binding from its Ctrl+<letter> map-select key) --
        // that's the key that actually fires it, per handle_hotkey's
        // resolution order.
        Hotkey hk = settings_.construction_key(btn.item);
        return hk.key != SDLK_UNKNOWN ? Settings::key_label(hk) : "";
    }
    if (btn.kind == "train" || btn.kind == "tech" || btn.kind == "trade") {
        // Identity-keyed (item_keys) -- matches handle_hotkey's own
        // resolution order exactly (see its comment).
        Hotkey hk = settings_.item_key(btn.kind, btn.item);
        return hk.key != SDLK_UNKNOWN ? Settings::key_label(hk) : "";
    }
    return ""; // "attack_ground" (fixed T, see handle_hotkey)/"nuke_indicator" (not clickable)/mouse-only
}

void GameClient::draw_item_tooltip(SDL_Renderer* renderer, const CardButton& btn, int panel_top) {
    const auto& table = ww::client::item_tooltips();
    const std::string& kind = btn.kind;
    const std::string& item = btn.item;
    std::string title, desc, cost_line;

    if (kind == "trade") {
        // Market buy/sell: show the LIVE oil price for this 100-unit trade so
        // the player can see what each button costs/earns right now (rates drift
        // as you trade). item is "buy food" / "sell iron" etc.
        auto sp = item.find(' ');
        std::string action = (sp == std::string::npos) ? item : item.substr(0, sp);
        std::string res = (sp == std::string::npos) ? "" : item.substr(sp + 1);
        std::string rescap = res;
        if (!rescap.empty()) rescap[0] = static_cast<char>(std::toupper(rescap[0]));
        int oil = static_cast<int>(match_.control().trade_quote(action, res, 0));
        title = (action == "buy" ? "Buy 100 " : "Sell 100 ") + rescap;
        cost_line = (action == "buy") ? (std::to_string(oil) + " Oil  ->  100 " + rescap)
                                      : ("100 " + rescap + "  ->  " + std::to_string(oil) + " Oil");
        desc = "Market rates rise as you buy and fall as you sell.";
    } else if (item.empty()) {
        // Fixed buttons (build_eco/build_military/attack_move/build_back)
        // have no catalog entry -- keyed by CardButton::kind instead, no
        // cost line (matches get_item_description.gml's cost="" for these).
        auto it = table.find(kind);
        if (it == table.end()) return;
        title = it->second.title;
        desc = it->second.desc;
    } else {
        auto it = table.find(item);
        title = (it != table.end()) ? it->second.title : item;
        desc = (it != table.end()) ? it->second.desc : "";

        // scripts/cost_string.gml: Food/Wood/Oil/Iron order. Control::
        // cost_of already applies the civ's cost_multiplier bonuses, same
        // as cost_string's own get_cost(_item, get_civ(0), ...) calls.
        auto costs = match_.control().cost_of(item, 0);
        static const std::pair<const char*, const char*> kOrder[4] = {
            {"food", "Food"}, {"wood", "Wood"}, {"oil", "Oil"}, {"iron", "Iron"}};
        for (auto& [key, label] : kOrder) {
            auto cit = costs.find(key);
            if (cit != costs.end() && cit->second > 0) {
                if (!cost_line.empty()) cost_line += " ";
                cost_line += std::to_string(cit->second) + " " + label;
            }
        }
    }
    // e.g. "Build Barracks (B)" -- whichever key actually fires this
    // button right now, so the tooltip stays correct after rebinding or
    // switching hotkey presets instead of describing a fixed default.
    std::string hk = hotkey_label_for(btn);
    if (!hk.empty()) title += " (" + hk + ")";

    // control/Draw.gml's `description` block: a semi-transparent black box
    // (no border), title + cost line in the larger font, description wrapped
    // in the small font below. The box height now GROWS to fit however many
    // wrapped description lines there are, so long descriptions (e.g. the
    // ballistic missile / Royal Marine) are never cut off -- it used to hard-
    // clamp to 2 lines, which truncated them.
    int box_w = ui(240);
    int desc_size = ui(9);
    int line_h = ui(11);
    std::vector<std::string> desc_lines =
        desc.empty() ? std::vector<std::string>{} : wrap_text(text_regular_, desc, desc_size, box_w - ui(8));
    int box_h = ui(16) + ui(13) + ui(3) + static_cast<int>(desc_lines.size()) * line_h + ui(4);
    SDL_Rect box{0, std::max(0, panel_top - box_h), box_w, box_h};

    SDL_BlendMode prev_blend;
    SDL_GetRenderDrawBlendMode(renderer, &prev_blend);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 128);
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawBlendMode(renderer, prev_blend);

    int ty = box.y + ui(2);
    text_.draw(title, box.x + ui(4), ty, {255, 255, 255, 255}, ui(13));
    ty += ui(16);
    if (!cost_line.empty()) {
        text_regular_.draw(cost_line, box.x + ui(4), ty, {255, 255, 255, 255}, ui(11));
    }
    ty += ui(13) + ui(3);
    for (auto& line : desc_lines) {
        text_regular_.draw(line, box.x + ui(4), ty, {255, 255, 255, 255}, desc_size);
        ty += line_h;
    }
}
