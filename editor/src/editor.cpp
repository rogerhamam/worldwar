#include "editor.h"

#include "game_data.h"
#include "menu/civ_data.h"

#include <SDL_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <set>

// Native Windows "Open File" dialog (comdlg32) for uploading a briefing image.
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

using ww::campaign::Campaign;
using ww::campaign::Level;
using ww::campaign::LevelPlayer;

namespace {
// 1600x1024 canvas (previously 1600x1080, 1280x960, 960x720, before that
// 640x480) -- main.cpp's window is this many pixels and editor.cpp
// reasons in that same space directly. 1024 rather than 1080 so the
// window fits inside a 1080-tall screen's actual usable area (taskbar/
// title bar) without SDL having to shrink it and letterbox the result.
// Not a uniform scale-up: the extra width/height in EditMap goes to a new
// top mode-toolbar and bottom options-toolbar (see draw_edit_map's
// EditMapMode) instead of a right sidebar (removed -- that space now goes
// entirely to the canvas, see kMapCanvasRect); the minimap floats back
// over the canvas's own bottom-right corner instead of docking in a
// sidebar. EditLevel also gained its own minimap preview
// (draw_minimap_contents), bottom-right of that screen.
constexpr SDL_Rect kPanel{48, 48, 1600 - 96, 1024 - 96};
constexpr SDL_Rect kBackRect{0, 0, 48, 48};
constexpr SDL_Rect kQuitRect{1600 - 24, 0, 24, 24};

// Same Tiny/Normal/Large/Huge presets as the main game's Random Map Setup
// screen (client/src/menu/menu_controller.cpp's kMapSizeNames/Values) --
// grid_size (tiles) = map_size*2, matching that screen's own World cols/
// rows = map_size*2 (sim/include/sim/scenario.h), so a level's battlefield
// really is the same size a skirmish map at that setting would be.
struct MapSizePreset {
    const char* name;
    int map_size_value;
};
constexpr MapSizePreset kMapSizePresets[4] = {
    {"Tiny", 28}, {"Normal", 40}, {"Large", 52}, {"Huge", 64},
};

// Europe-map click area on the EditLevel screen -- same fractional-
// position idea the request described ("dots on the map of europe"):
// loc_x/loc_y are stored as 0..1 fractions of this rect, not raw pixels,
// so they stay meaningful regardless of how big the map is drawn
// elsewhere (the in-game campaign screen, a future higher-res redraw).
// Sits to the RIGHT of the players column (not below it) so up to 4
// player rows never collide with it -- spr_europe_map's own 736x588
// aspect ratio (~1.25:1), scaled down but not distorted.
// x pushed out from the original +300 to +320 (and width trimmed to keep
// the right edge flush with the panel) to leave the players column enough
// room for the Team cycle-button added after the civ name -- at +300 it
// exactly coincided with (and, since it's drawn after, painted over) that
// column's rightmost buttons.
// Sized to sit side-by-side with the battlefield preview (see
// draw_edit_level's preview_rect, which hangs its own size/position off
// this rect) and fill the wide gap that used to sit empty between them --
// the two used to stack vertically at their old, much smaller 384x308/
// 350x350 sizes, leaving most of the panel's right two-thirds blank.
constexpr SDL_Rect kMapRect{kPanel.x + 480, kPanel.y + 129, 536, 428};

// Team tint palette for both the EditLevel player rows and EditMap's
// placed-unit/building markers -- shared so a team reads as the same
// colour in both places.
const SDL_Color kTeamColours[4] = {
    {100, 180, 255, 255}, {255, 120, 100, 255}, {120, 220, 120, 255}, {230, 200, 90, 255}};

// EditMap's terrain palette -- the sim's own hardcoded resource/terrain
// kind strings and real sprite names (sim/src/world.cpp's resource_kinds()
// table; "water" is the sim's terrain-tile id 2, see game_client.cpp's
// terrain_sprite()), so the grid previews with the actual in-game art
// instead of flat colour swatches. `colour` is only a fallback for if a
// sprite's ever missing (see draw_palette_row/draw_edit_map).
// `is_base` marks a tile as a base-layer ground texture (TerrainFeature::
// base -- water plus the dirt/sand/gravel/pavement variants below) rather
// than a resource/decoration overlay (TerrainFeature::resource) -- a base
// tile can still carry a resource on top of it (e.g. fish in water), so
// these are two independent slots on the same tile, not one mutually-
// exclusive "kind" -- fixes fish silently deleting the water underneath.
//
// `passable` is an editor-only authoring convenience (see footprint_clear):
// whether a hand-placed unit/building may be positioned on this tile at
// all. IMPORTANT: this is NOT recovered from the original GameMaker game
// -- the extracted source has no collision/solid data for any of the
// decoration objects below (obj_crate/obj_fence1/obj_stones/obj_cliff3
// have zero extracted event scripts, and tools/extract_objects.py parses
// then discards each object's `solid` flag, see extract_objects.py:57-58)
// -- these are new judgment calls: discrete physical-looking obstacles
// (rubble, rocks, cliffs, fences, hedges, crates, trees, bushes, wrecked
// vehicles/buildings) default impassable; flat ground-texture reskins
// (dirt/sand/gravel/dark dirt/pavement) and small litter (pebbles) default
// passable, matching how grass itself is passable. Adjust freely --
// there's no "correct" answer recoverable from source for these.
struct TerrainKind {
    const char* name;
    const char* sprite;
    SDL_Color colour;
    bool is_base;
    // Which frame to draw -- 0 for everything except fish (see below) and
    // the 5 destroyed-building variants, which each pick a different one
    // of spr_house_destroyed's 5 subimages.
    int frame = 0;
    // spr_fish's frame 0 is a fully blank/transparent animation frame
    // (it's a swim-cycle sheet, not per-player-colour skins like unit/
    // building sprites), so drawing it at the default frame 0 rendered
    // nothing at all -- frame 1 is the first frame with a visible fish.
    bool passable = false;
    // Which of Terrain mode's bottom-toolbar sub-tabs this kind shows
    // under (see Editor::TerrainCategory).
    Editor::TerrainCategory category;
};
const TerrainKind kTerrainKinds[] = {
    // "grass" is the implicit default (TerrainFeature::base == "" already
    // means grass everywhere else in this file) -- listed here so it's
    // paintable like any other base kind, but paint_terrain_at maps it
    // back to an empty base string on write rather than literally storing
    // "grass", to keep that convention intact.
    {"grass", "spr_green_grass", {70, 140, 70, 255}, true, 0, true, Editor::TerrainCategory::Ground},
    {"water", "spr_water", {60, 110, 200, 255}, true, 0, false, Editor::TerrainCategory::Ground},
    {"tree", "spr_tree", {40, 110, 40, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"palm", "spr_palmtree", {70, 140, 60, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"berry", "spr_berry_bush", {200, 60, 140, 255}, false, 0, false, Editor::TerrainCategory::Objects},
    {"oil", "spr_oil_pool", {40, 40, 40, 255}, false, 0, false, Editor::TerrainCategory::Objects},
    {"iron", "spr_iron_ore", {130, 130, 140, 255}, false, 0, false, Editor::TerrainCategory::Objects},
    {"deer", "spr_deer", {150, 110, 70, 255}, false, 0, false, Editor::TerrainCategory::Objects},
    {"fish", "spr_fish", {80, 160, 200, 255}, false, 1, false, Editor::TerrainCategory::Objects},
    // ---- ground-texture base tiles (alternatives to grass, all passable
    // like grass -- TerrainFeature::base slot, same as water) ----
    {"dirt", "spr_dirt", {110, 90, 60, 255}, true, 0, true, Editor::TerrainCategory::Ground},
    {"brown dirt", "spr_dark_dirt", {80, 60, 40, 255}, true, 0, true, Editor::TerrainCategory::Ground},
    {"gravel", "spr_gravel_dirt", {130, 125, 115, 255}, true, 0, true, Editor::TerrainCategory::Ground},
    {"sand", "spr_desert_sand", {210, 190, 140, 255}, true, 0, true, Editor::TerrainCategory::Ground},
    {"pavement", "spr_urban", {140, 140, 145, 255}, true, 0, true, Editor::TerrainCategory::Ground},
    // ---- decoration/obstacle overlays (TerrainFeature::resource slot) --
    // passability per the user's explicit call: fences, rocks, stones,
    // broken buildings, crates, and trees are solid; everything else
    // (rubble/hedge/bush/all the vehicle-wreckage rubbles, plus flames
    // and pebbles) is passable. ----
    {"flames", "spr_flame", {230, 120, 40, 255}, false, 0, true, Editor::TerrainCategory::Objects},
    {"rubble", "spr_rubble1", {110, 100, 90, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"brown stone", "spr_stone1", {120, 100, 80, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"grey stone", "spr_stone2", {130, 130, 130, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"stones", "spr_stones", {140, 135, 125, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"big tree", "spr_tree1", {40, 110, 40, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"oriental tree", "spr_tree2", {50, 120, 50, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"crate", "spr_crate", {150, 110, 60, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"pebbles", "spr_rocks", {150, 145, 135, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"destroyed building 1", "spr_house_destroyed", {100, 90, 85, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"destroyed building 2", "spr_house_destroyed", {100, 90, 85, 255}, false, 1, false, Editor::TerrainCategory::Blocks},
    {"destroyed building 3", "spr_house_destroyed", {100, 90, 85, 255}, false, 2, false, Editor::TerrainCategory::Blocks},
    {"destroyed building 4", "spr_house_destroyed", {100, 90, 85, 255}, false, 3, false, Editor::TerrainCategory::Blocks},
    {"destroyed building 5", "spr_house_destroyed", {100, 90, 85, 255}, false, 4, false, Editor::TerrainCategory::Blocks},
    {"hedge", "spr_hedge", {50, 100, 45, 255}, false, 0, true, Editor::TerrainCategory::Blocks},
    {"fence", "spr_fence1", {150, 130, 100, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"fence2", "spr_fence2", {150, 130, 100, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"fence3", "spr_fence3", {150, 130, 100, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"orange rock", "spr_cliff1", {170, 110, 60, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"yellow rock", "spr_cliff2", {180, 160, 70, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"brown rock", "spr_cliff3", {130, 95, 60, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"grey rock", "spr_cliff4", {120, 120, 120, 255}, false, 0, false, Editor::TerrainCategory::Blocks},
    {"big rubble", "spr_96_rubble", {100, 95, 90, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"small rubble", "spr_64_rubble", {100, 95, 90, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"aa gun rubble", "spr_aa_gun_rubble", {90, 90, 80, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"b29 rubble", "spr_b29_rubble", {90, 90, 80, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"biplane rubble", "spr_biplane_rubble", {90, 90, 80, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"bomber rubble", "spr_bomber_rubble", {90, 90, 80, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"bush", "spr_bush", {45, 105, 45, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"fighter rubble", "spr_fighter_rubble", {90, 90, 80, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"flak rubble", "spr_flak_rubble", {90, 90, 80, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"heavy bomber rubble", "spr_heavy_bomber_rubble", {90, 90, 80, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"heavy tank rubble", "spr_heavy_tank_rubble", {90, 90, 80, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"jet fighter rubble", "spr_jet_fighter_rubble", {90, 90, 80, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
    {"light tank rubble", "spr_light_tank_rubble", {90, 90, 80, 255}, false, 0, true, Editor::TerrainCategory::Decoration},
};
// Number of kTerrainKinds entries belonging to a given category -- used to
// size the palette's scrollbar/scroll-clamp now that the sidebar only ever
// shows one category's worth of rows at a time instead of the full list.
int terrain_category_row_count(Editor::TerrainCategory cat) {
    int n = 0;
    for (auto& k : kTerrainKinds) {
        if (k.category == cat) ++n;
    }
    return n;
}
// Brush painting only makes sense for area-ish terrain (water/trees/etc);
// deer, fish, and the 5 one-off destroyed-building variants are all
// discrete single-spot placements in the real game (a single spawn_deer/
// one fish shoal marker/one specific building's ruins), so a multi-tile
// brush never applies to them regardless of brush_size_ -- see
// canvas_click's paint logic.
bool terrain_kind_ignores_brush(const std::string& kind) {
    return kind == "deer" || kind == "fish" || kind.rfind("destroyed building", 0) == 0;
}
// Looks up a TerrainKind by name (TerrainFeature::base or ::resource) --
// used by footprint_clear to decide whether a tile actually blocks a
// hand-placed unit/building, now that not every terrain feature does.
const TerrainKind* find_terrain_kind(const std::string& name) {
    for (auto& k : kTerrainKinds) {
        if (name == k.name) return &k;
    }
    return nullptr;
}
// Base tile under everything else, matching game_client.cpp's
// terrain_sprite(0) -- the main game's default grass tile id.
constexpr const char* kGrassSprite = "spr_green_grass";
constexpr SDL_Color kGrassColour{70, 140, 70, 255}; // fallback if that sprite's ever missing

// EditMap's content-area geometry: a top mode-toolbar (Map/Terrain/
// Players/Units, see EditMapMode) and a bottom options-toolbar (content
// depends on which mode's active) bookend a middle band that holds the
// canvas and, in Units mode only, a left palette sidebar -- see
// draw_edit_map. kContentTop/kContentH are that middle band's y/height,
// shared by all three (canvas, sidebar, and the top/bottom toolbars'own
// y math) so they always agree without each recomputing it separately.
constexpr int kContentTop = 90;  // kPanel.y + this
constexpr int kContentH = Editor::kMapViewH;

// EditMap's canvas viewport, positioned to the right of the left palette
// sidebar's column (whether or not that sidebar is actually drawn this
// mode) -- shared with handle_click/handle_right_click so screen<->tile
// math agrees everywhere it's needed.
constexpr SDL_Rect kMapCanvasRect{kPanel.x + 234, kPanel.y + kContentTop, Editor::kMapViewW,
                                  Editor::kMapViewH};

// Left sidebar's palette viewport (Units mode only -- see EditMapMode):
// the Other tab's ~40 decoration entries (see kTerrainKinds) are far more
// than fit at once, so that tab's rows scroll within this rect (see
// draw_edit_map and handle_event's SDL_MOUSEWHEEL case, which checks
// whether the cursor is over this rect to decide palette-scroll vs.
// canvas-zoom). Same vertical span as the canvas -- no per-sidebar tab
// row any more, that sub-selection lives in the bottom toolbar now.
constexpr SDL_Rect kPaletteRect{kPanel.x + 12, kPanel.y + kContentTop, 210, kContentH};

// EditMap's bottom options-toolbar -- content depends on EditMapMode (see
// draw_edit_map). Sits below the canvas/sidebar band with a 12px gap on
// every side of the panel.
constexpr SDL_Rect kBottomToolbarRect{kPanel.x + 12, kPanel.y + kContentTop + kContentH + 12,
                                      kPanel.w - 24,
                                      kPanel.h - (kContentTop + kContentH + 12) - 12};

// Minimap, docked in the bottom toolbar's own far-right corner (no longer
// floating over the canvas) -- still interactive (handle_click's
// "minimap_click" and the viewport-rect overlay in draw_edit_map), just
// relocated. Vertically centred within the toolbar band. Solid tile
// colours now (see draw_minimap_contents), not a translucent overlay --
// there's no canvas content underneath it any more to show through.
constexpr int kMiniSize = 130, kMiniMargin = 12;
constexpr SDL_Rect kMinimapRect{kBottomToolbarRect.x + kBottomToolbarRect.w - kMiniSize - kMiniMargin,
                                kBottomToolbarRect.y + (kBottomToolbarRect.h - kMiniSize) / 2, kMiniSize,
                                kMiniSize};

// The bottom toolbar's remaining space once the minimap's own corner is
// carved out -- every EditMapMode's controls lay themselves out within
// THIS rect now (see draw_edit_map), not the full kBottomToolbarRect, so
// nothing overlaps the minimap.
constexpr SDL_Rect kBottomToolbarContentRect{kBottomToolbarRect.x, kBottomToolbarRect.y,
                                             kBottomToolbarRect.w - kMiniSize - kMiniMargin - 12,
                                             kBottomToolbarRect.h};

// Terrain mode's brush-size slider (1-5, see draw_edit_map) -- shared
// between drawing and handle_event's click/drag handling (both need the
// exact same track rect to agree on where each of the 5 positions is).
// y sits below the category sub-tab row above it (12 + 40 + 12, same
// "row of buttons, then a 12px gap" spacing Units mode's own two-row
// bottom toolbar uses).
constexpr SDL_Rect kBrushSliderRect{kBottomToolbarContentRect.x + 12, kBottomToolbarContentRect.y + 64, 400,
                                    40};

// Random-map "map type" options (EditMap's Map mode) -- the same 4 named
// generators the main game's World constructor has (sim/src/world.cpp)
// plus its generic "random" fallback. This editor has zero dependency on
// game (see this project's README), so generate_map() is a
// self-contained reimplementation, not a byte-exact port -- these names
// are chosen to match that source's vocabulary, not its exact algorithm.
constexpr const char* kMapGenTypes[5] = {"random", "arabia", "arena", "guam", "ostland"};
constexpr const char* kMapGenTypeLabels[5] = {"Random", "Arabia", "Arena", "Guam", "Ostland"};

// Blank-map "default terrain" options (EditMap's Map mode) -- a single
// uniform fill applied to every tile when NOT generating randomly.
constexpr const char* kDefaultTerrains[4] = {"grass", "dirt", "water", "tree"};
constexpr const char* kDefaultTerrainLabels[4] = {"Grass", "Dirt", "Water", "Tree"};

// Word-wraps `s` to fit `max_w`, also breaking on explicit '\n's the user
// typed (Enter, in the multi-line description field) -- so a paragraph
// both wraps automatically AND respects manual paragraph breaks.
std::vector<std::string> wrap_lines(TextRenderer& text, const std::string& s, int size, int max_w) {
    std::vector<std::string> lines;
    std::string cur_line, word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        std::string trial = cur_line.empty() ? word : cur_line + " " + word;
        int tw, th;
        text.measure(trial, size, tw, th);
        if (tw > max_w && !cur_line.empty()) {
            lines.push_back(cur_line);
            cur_line = word;
        } else {
            cur_line = trial;
        }
        word.clear();
    };
    for (char c : s) {
        if (c == '\n') {
            flush_word();
            lines.push_back(cur_line);
            cur_line.clear();
        } else if (c == ' ') {
            flush_word();
        } else {
            word += c;
        }
    }
    flush_word();
    lines.push_back(cur_line);
    return lines;
}
} // namespace

Editor::Editor(SDL_Renderer* renderer, const std::string& asset_dir, const std::string& data_dir,
              int view_w, int view_h)
    : data_dir_(data_dir), asset_dir_(asset_dir), view_w_(view_w), view_h_(view_h),
      atlas_(renderer, asset_dir),
      text_(renderer, "C:\\Windows\\Fonts\\serife.fon", "C:\\Windows\\Fonts\\segoeui.ttf", 0, true),
      text_regular_(renderer, "C:\\Windows\\Fonts\\serife.fon", "C:\\Windows\\Fonts\\segoeui.ttf", 0, false),
      data_(data_dir) {
    campaigns_ = ww::campaign::load_all_campaigns(data_dir_);
    start_ticks_ = SDL_GetTicks();

    // civ_exclude.json -> int-keyed sets, same parse as Control's own
    // constructor (sim/src/control.cpp) and MenuController's tech tree --
    // not reused directly since building a full Control here would need an
    // unrelated Bonuses/team-array just to reach this one lookup table.
    for (auto& [k, v] : data_.civ_exclude().items()) {
        std::set<std::string> excl;
        for (auto& item : v) excl.insert(item.get<std::string>());
        civ_exclude_[std::stoi(k)] = std::move(excl);
    }
}

Editor::~Editor() {
    if (briefing_tex_) SDL_DestroyTexture(briefing_tex_);
}

Campaign* Editor::current_campaign() {
    if (selected_campaign_ < 0 || selected_campaign_ >= static_cast<int>(campaigns_.size())) return nullptr;
    return &campaigns_[selected_campaign_];
}

Level* Editor::current_level() {
    Campaign* c = current_campaign();
    if (!c || selected_level_ < 0 || selected_level_ >= static_cast<int>(c->levels.size())) return nullptr;
    return &c->levels[selected_level_];
}

int Editor::current_grid_size() {
    Level* lvl = current_level();
    return lvl ? lvl->grid_size : ww::campaign::kLevelGridSize;
}

void Editor::save_current_campaign() {
    if (Campaign* c = current_campaign()) {
        ww::campaign::save_campaign(*c, data_dir_);
        last_save_flash_ = SDL_GetTicks() / 1000.0;
    }
}

std::string* Editor::active_field_string() {
    switch (active_field_) {
        case Field::CampaignName: return &new_campaign_name_;
        case Field::NewLevelName: return &new_level_name_;
        case Field::LevelName: return current_level() ? &current_level()->name : nullptr;
        case Field::LevelDescription: return current_level() ? &current_level()->description : nullptr;
        case Field::AiProfile: return current_level() ? &current_level()->ai_profile : nullptr;
        case Field::MessageText: {
            // Shared by every trigger type that shows a message (message/spawn/
            // dormant/resources) -- ev.text is the notification shown when it fires.
            Level* lvl = current_level();
            if (!lvl || events_selected_ < 0 || events_selected_ >= static_cast<int>(lvl->events.size())) {
                return nullptr;
            }
            return &lvl->events[events_selected_].text;
        }
        case Field::SpawnCount:
            // Scratch buffer, not the real int field -- see commit_active_field.
            return &spawn_count_edit_;
        case Field::PlayerResource:
            // Scratch buffer, not the real double field itself -- see
            // commit_active_field, which parses this back on defocus.
            return (players_res_field_ >= 0 && players_res_field_ < 4) ? &players_res_edit_[players_res_field_]
                                                                        : nullptr;
        case Field::ObjectiveName: {
            Level* lvl = current_level();
            if (!lvl || objectives_selected_ < 0 ||
                objectives_selected_ >= static_cast<int>(lvl->objectives.size())) {
                return nullptr;
            }
            return &lvl->objectives[objectives_selected_].name;
        }
        case Field::EventName: {
            Level* lvl = current_level();
            if (!lvl || events_selected_ < 0 || events_selected_ >= static_cast<int>(lvl->events.size())) {
                return nullptr;
            }
            return &lvl->events[events_selected_].name;
        }
        case Field::EventResource:
            // Scratch buffer, not the real double field itself -- see
            // commit_active_field, which parses this back on defocus.
            return (event_res_field_ >= 0 && event_res_field_ < 4) ? &event_res_edit_[event_res_field_] : nullptr;
        default: return nullptr;
    }
}

size_t Editor::active_field_max_len() const {
    switch (active_field_) {
        case Field::MessageText: return 100;
        case Field::PlayerResource: return 6;
        case Field::EventResource: return 6;
        case Field::SpawnCount: return 4;
        default: return std::string::npos;
    }
}

void Editor::commit_active_field() {
    if (active_field_ == Field::PlayerResource) {
        Level* lvl = current_level();
        if (lvl && players_selected_ >= 0 && players_selected_ < static_cast<int>(lvl->players.size()) &&
            players_res_field_ >= 0 && players_res_field_ < 4) {
            LevelPlayer& p = lvl->players[players_selected_];
            double* fields[4] = {&p.food, &p.wood, &p.oil, &p.iron};
            const std::string& text = players_res_edit_[players_res_field_];
            double parsed = 0.0;
            try {
                parsed = text.empty() ? 0.0 : std::stod(text);
            } catch (...) {
            }
            *fields[players_res_field_] = std::clamp(parsed, 0.0, 999999.0);
        }
        players_res_field_ = -1;
    } else if (active_field_ == Field::EventResource) {
        Level* lvl = current_level();
        if (lvl && events_selected_ >= 0 && events_selected_ < static_cast<int>(lvl->events.size()) &&
            event_res_field_ >= 0 && event_res_field_ < 4) {
            ww::campaign::MapEvent& ev = lvl->events[events_selected_];
            double* fields[4] = {&ev.res_food, &ev.res_wood, &ev.res_oil, &ev.res_iron};
            const std::string& text = event_res_edit_[event_res_field_];
            double parsed = 0.0;
            try {
                parsed = text.empty() ? 0.0 : std::stod(text);
            } catch (...) {
            }
            *fields[event_res_field_] = std::clamp(parsed, 0.0, 999999.0);
        }
        event_res_field_ = -1;
    } else if (active_field_ == Field::SpawnCount) {
        Level* lvl = current_level();
        if (lvl && events_selected_ >= 0 && events_selected_ < static_cast<int>(lvl->events.size())) {
            int parsed = 0;
            try {
                parsed = spawn_count_edit_.empty() ? 0 : std::stoi(spawn_count_edit_);
            } catch (...) {
            }
            lvl->events[events_selected_].spawn_count = std::clamp(parsed, 0, 9999);
        }
    }
}

std::vector<std::string> Editor::palette_units(int civ) const {
    using namespace ww::gamedata;
    std::vector<std::string> out;
    for (auto& [building, list] : PRODUCTION) {
        (void)building;
        for (auto& u : list) {
            if (std::find(out.begin(), out.end(), u) != out.end()) continue;
            if (!civ_has(u, civ, civ_exclude_)) continue;
            if (CAMEL_UNITS.count(u) && !CAMEL_CIVS.count(civ)) continue;
            if (OTTOMAN_ONLY.count(u) && civ != 8) continue;
            auto conly = CIV_ONLY_UNITS.find(u);
            if (conly != CIV_ONLY_UNITS.end() && conly->second != civ) continue;
            out.push_back(u);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> Editor::palette_buildings(int civ) const {
    using namespace ww::gamedata;
    std::vector<std::string> out = {"base"}; // town center -- not in BUILDABLE (always available)
    for (auto& b : BUILDABLE) {
        if (civ_has(b, civ, civ_exclude_)) out.push_back(b);
    }
    return out;
}

std::string Editor::item_icon(const std::string& item) const {
    for (const char* section : {"units", "buildings"}) {
        auto& sec = data_.catalog().at(section);
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

std::string Editor::world_sprite(const std::string& item, int civ, int era) const {
    // "base" (town center) has no generic sprite_index in catalog.json --
    // its art is civ-specific, assigned at spawn time in the real game
    // (scenario.cpp's CIV_BASE table, vendored into game_data.h) rather
    // than looked up from the catalog like every other building.
    if (item == "base") {
        auto it = ww::gamedata::CIV_BASE.find(civ);
        if (it != ww::gamedata::CIV_BASE.end()) return it->second;
    }
    // "house" re-skins by era in THIS editor's preview/canvas rendering --
    // an editor-only convention (spr_house/house1/house2/house3 aren't
    // wired to era anywhere in the live game yet, which always renders
    // every house as plain "spr_house" regardless of era) purely so the
    // Players tab's era picker has a visible effect on the battlefield
    // preview. See LevelPlayer::era's comment.
    if (item == "house") {
        static const char* kEraHouse[4] = {"spr_house", "spr_house1", "spr_house2", "spr_house3"};
        return kEraHouse[std::clamp(era, 0, 3)];
    }
    for (const char* section : {"units", "buildings"}) {
        auto& sec = data_.catalog().at(section);
        if (sec.contains(item)) {
            std::string sp = sec.at(item).value("sprite_index", "");
            if (!sp.empty()) return sp;
            break;
        }
    }
    std::string fallback = "spr_" + item;
    for (auto& ch : fallback) if (ch == ' ') ch = '_';
    return fallback;
}

void Editor::handle_event(const SDL_Event& ev) {
    if (ev.type == SDL_QUIT) {
        wants_quit_ = true;
    } else if (ev.type == SDL_MOUSEMOTION) {
        mouse_pos_ = {ev.motion.x, ev.motion.y};
        if (map_dragging_) {
            // Same middle-drag pan as GameClient (client/src/game_client.cpp)
            // -- needed once zoomed in enough that part of the 32x32 grid
            // is off-screen (see kMapViewW/H's comment).
            map_camera_.pan(-(ev.motion.x - map_drag_last_.x), -(ev.motion.y - map_drag_last_.y));
            map_drag_last_ = {ev.motion.x, ev.motion.y};
        }
        if (terrain_dragging_) {
            int tx, ty;
            if (canvas_tile_at(ev.motion.x, ev.motion.y, tx, ty)) {
                if (erasing_) erase_area_at(tx, ty);
                else paint_terrain_at(tx, ty);
            }
        }
        if (brush_slider_dragging_) {
            double frac = std::clamp(static_cast<double>(ev.motion.x - kBrushSliderRect.x) / kBrushSliderRect.w,
                                     0.0, 1.0);
            brush_size_ = 1 + static_cast<int>(std::lround(frac * 4.0));
        }
        if (area_dragging_ || trig_line_dragging_) {
            int tx, ty;
            if (canvas_tile_at(ev.motion.x, ev.motion.y, tx, ty)) {
                area_drag_tx1_ = tx;
                area_drag_ty1_ = ty;
            }
        }
        if (lasso_marquee_) {
            int tx, ty;
            if (canvas_tile_at(ev.motion.x, ev.motion.y, tx, ty)) {
                lasso_x1_ = tx;
                lasso_y1_ = ty;
            }
        }
        if (lasso_moving_) {
            int tx, ty;
            if (canvas_tile_at(ev.motion.x, ev.motion.y, tx, ty)) {
                // Clamp the offset so the whole selection rect stays on the grid.
                int gs = current_grid_size();
                int dx = tx - lasso_anchor_tx_, dy = ty - lasso_anchor_ty_;
                dx = std::clamp(dx, -lasso_x0_, gs - 1 - lasso_x1_);
                dy = std::clamp(dy, -lasso_y0_, gs - 1 - lasso_y1_);
                lasso_move_dx_ = dx;
                lasso_move_dy_ = dy;
            }
        }
    } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        handle_click(ev.button.x, ev.button.y);
        // erasing_ makes the canvas/slider draggable regardless of
        // edit_map_mode_ (it's a global tool now, see its comment);
        // otherwise only Terrain mode's own paint brush drags.
        if (screen_ == Screen::EditMap && (erasing_ || edit_map_mode_ == EditMapMode::Terrain)) {
            SDL_Point p{ev.button.x, ev.button.y};
            if (SDL_PointInRect(&p, &kMapCanvasRect)) terrain_dragging_ = true;
            if (SDL_PointInRect(&p, &kBrushSliderRect)) brush_slider_dragging_ = true;
        }
        // Objectives' kill_units/move_to_area area tool, and Events' gate/
        // dormant area tool (see area_dragging_'s comment): unlike
        // Terrain's brush (which acts on every tile it touches as you
        // drag), these need BOTH corners of a rectangle, so they can't
        // reuse handle_click's own single-tile "canvas_click" dispatch --
        // this records the anchor tile here instead and waits for
        // SDL_MOUSEBUTTONUP to finalize. Never starts while erasing_
        // (that pre-empts every mode, same as Terrain above), with
        // nothing selected (there'd be nothing to draw into), or -- for
        // Events -- with a "message"-type event selected (that uses
        // reposition_selected_message_at's single-click instead).
        bool objective_area_edit =
            edit_map_mode_ == EditMapMode::Objectives && objectives_selected_ >= 0;
        bool event_area_edit = false;
        if (edit_map_mode_ == EditMapMode::Events) {
            if (Level* lvl = current_level();
                lvl && events_selected_ >= 0 && events_selected_ < static_cast<int>(lvl->events.size())) {
                const std::string& t = lvl->events[events_selected_].type;
                event_area_edit = (t == "gate" || t == "dormant" || t == "spawn");
            }
        }
        if (screen_ == Screen::EditMap && !erasing_ && drawing_trigger_line_) {
            // Trigger-line tool pre-empts the area drag: this press starts the
            // two-point line for the selected dormant event.
            int tx, ty;
            if (canvas_tile_at(ev.button.x, ev.button.y, tx, ty)) {
                trig_line_dragging_ = true;
                area_drag_tx0_ = area_drag_tx1_ = tx;
                area_drag_ty0_ = area_drag_ty1_ = ty;
            }
        } else if (screen_ == Screen::EditMap && !erasing_ && (objective_area_edit || event_area_edit)) {
            int tx, ty;
            if (canvas_tile_at(ev.button.x, ev.button.y, tx, ty)) {
                area_dragging_ = true;
                area_drag_tx0_ = area_drag_tx1_ = tx;
                area_drag_ty0_ = area_drag_ty1_ = ty;
            }
        }
    } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
        terrain_dragging_ = false;
        brush_slider_dragging_ = false;
        if (trig_line_dragging_) {
            trig_line_dragging_ = false;
            drawing_trigger_line_ = false;
            Level* lvl = current_level();
            if (lvl && events_selected_ >= 0 && events_selected_ < static_cast<int>(lvl->events.size())) {
                auto& ev2 = lvl->events[events_selected_];
                ev2.trig_tx0 = area_drag_tx0_;
                ev2.trig_ty0 = area_drag_ty0_;
                ev2.trig_tx1 = area_drag_tx1_;
                ev2.trig_ty1 = area_drag_ty1_;
                save_current_campaign();
            }
        }
        if (area_dragging_) {
            area_dragging_ = false;
            if (edit_map_mode_ == EditMapMode::Objectives) finalize_objective_area();
            else if (edit_map_mode_ == EditMapMode::Events) finalize_event_area();
        }
        if (lasso_marquee_) {
            lasso_marquee_ = false;
            // Normalize the drawn box into an inclusive tile rect and keep it
            // as the live selection (only if it actually covers a tile).
            int x0 = std::min(lasso_x0_, lasso_x1_), x1 = std::max(lasso_x0_, lasso_x1_);
            int y0 = std::min(lasso_y0_, lasso_y1_), y1 = std::max(lasso_y0_, lasso_y1_);
            lasso_x0_ = x0; lasso_x1_ = x1; lasso_y0_ = y0; lasso_y1_ = y1;
            lasso_has_sel_ = true;
        }
        if (lasso_moving_) {
            lasso_moving_ = false;
            if (lasso_move_dx_ != 0 || lasso_move_dy_ != 0) commit_lasso_move();
            lasso_move_dx_ = lasso_move_dy_ = 0;
        }
    } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_RIGHT) {
        handle_right_click(ev.button.x, ev.button.y);
    } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_MIDDLE &&
              screen_ == Screen::EditMap) {
        map_dragging_ = true;
        map_drag_last_ = {ev.button.x, ev.button.y};
    } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_MIDDLE) {
        map_dragging_ = false;
    } else if (ev.type == SDL_MOUSEWHEEL && screen_ == Screen::EditMap) {
        SDL_Point p{mouse_pos_.x, mouse_pos_.y};
        if (!erasing_ && edit_map_mode_ == EditMapMode::Terrain && SDL_PointInRect(&p, &kPaletteRect)) {
            // Scroll Terrain mode's palette instead of zooming the canvas
            // when the cursor is over it -- see kPaletteRect's comment.
            int content_h = terrain_category_row_count(terrain_category_) * 31;
            int max_scroll = std::max(0, content_h - kPaletteRect.h);
            other_scroll_ = std::clamp(other_scroll_ - ev.wheel.y * 40, 0, max_scroll);
        } else {
            // Same factor/clamp/recenter-on-screen-center zoom GameClient
            // uses, just re-centered on the canvas viewport's own middle
            // rather than the whole window's (this camera's screen-space
            // origin is the canvas rect's top-left, not the window's --
            // see draw_edit_map).
            double factor = ev.wheel.y > 0 ? 1.1 : (ev.wheel.y < 0 ? 1.0 / 1.1 : 1.0);
            double cx, cy;
            map_camera_.screen_to_world(kMapViewW / 2, kMapViewH / 2, cx, cy);
            map_camera_.zoom = std::clamp(map_camera_.zoom * factor, 0.3, 3.0);
            map_camera_.center_on(cx, cy);
        }
    } else if (ev.type == SDL_TEXTINPUT) {
        if (std::string* s = active_field_string()) {
            std::string incoming = ev.text.text;
            // PlayerResource/EventResource only ever hold a plain non-
            // negative integer (see commit_active_field's std::stod) --
            // reject anything that isn't a digit rather than accepting
            // free text just to fail parsing on commit.
            if (active_field_ == Field::PlayerResource || active_field_ == Field::EventResource ||
                active_field_ == Field::SpawnCount) {
                incoming.erase(std::remove_if(incoming.begin(), incoming.end(),
                                              [](unsigned char c) { return !std::isdigit(c); }),
                               incoming.end());
            }
            size_t max_len = active_field_max_len();
            for (char c : incoming) {
                if (s->size() >= max_len) break;
                *s += c;
            }
        }
    } else if (ev.type == SDL_KEYDOWN) {
        handle_key(ev.key.keysym.sym);
    }
}

void Editor::handle_key(SDL_Keycode key) {
    if (key == SDLK_BACKSPACE) {
        if (std::string* s = active_field_string()) {
            if (!s->empty()) s->pop_back();
        }
    } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && active_field_ == Field::LevelDescription) {
        // Description is multi-line (see draw_text_area) -- Enter writes a
        // paragraph break instead of committing the field; Tab still
        // commits it, same as every other field.
        if (std::string* s = active_field_string()) *s += '\n';
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_TAB) {
        commit_active_field();
        active_field_ = Field::None;
        SDL_StopTextInput();
    } else if (key == SDLK_ESCAPE) {
        if (active_field_ != Field::None) {
            commit_active_field();
            active_field_ = Field::None;
            SDL_StopTextInput();
        } else if (erasing_) {
            erasing_ = false;
        }
    }
}

void Editor::handle_click(int mx, int my) {
    SDL_Point p{mx, my};
    for (auto& hr : hit_rects_) {
        if (!SDL_PointInRect(&p, &hr.rect)) continue;
        const std::string& id = hr.id;

        // Clicking anywhere that isn't a text field itself drops focus --
        // simplest way to "commit" a field without a dedicated OK click.
        // "players_res_edit" is deliberately NOT included here -- unlike
        // the other text fields (whose value IS the real data), it edits
        // a scratch buffer (players_res_edit_) that has to be parsed back
        // into the real field on commit (see commit_active_field), so
        // clicking a DIFFERENT resource's field needs to go through the
        // generic drop-focus path below to commit the one it's leaving.
        bool is_field_click = (id == "field_campaign_name" || id == "field_new_level_name" ||
                              id == "field_level_name" || id == "field_level_desc" || id == "field_message" ||
                              id == "field_objective_name" || id == "field_event_name");
        if (!is_field_click && active_field_ != Field::None) {
            commit_active_field();
            active_field_ = Field::None;
            SDL_StopTextInput();
        }

        if (id == "quit") {
            wants_quit_ = true;
        } else if (id == "back") {
            if (screen_ == Screen::EditMap) {
                save_current_campaign();
                screen_ = Screen::EditLevel;
            } else if (screen_ == Screen::EditLevel) {
                save_current_campaign();
                screen_ = Screen::LevelList;
            } else if (screen_ == Screen::LevelList) {
                save_current_campaign();
                screen_ = Screen::CampaignList;
                selected_campaign_ = -1;
            } else if (screen_ == Screen::NewCampaign) {
                screen_ = Screen::CampaignList;
            } else if (screen_ == Screen::NewLevel) {
                screen_ = Screen::LevelList;
            }
        } else if (id == "edit_map") {
            active_team_ = -1;
            objectives_selected_ = -1; // don't carry a stale index into a different level's objectives list
            events_selected_ = -1;     // same reasoning, for the events list
            // Resets to a fit-the-whole-grid view every time this screen
            // opens (rather than remembering wherever the camera was left)
            // -- simplest predictable default, matching kMapViewW/H's
            // comment on why the zoom floor is low enough to allow this.
            // The fit ratio itself is computed from the GRID's own size
            // (not the camera's padded world size, see kMapMargin) so the
            // grid fills most of the view; *0.9 leaves a little breathing
            // room around the edges so a unit/building sitting on the
            // outermost row/column (whose real sprite is bigger than one
            // tile) doesn't start out flush against the canvas border.
            // Centering targets the grid's centre in the camera's padded
            // coordinate space.
            double grid_px = current_grid_size() * kEditorTile;
            map_camera_.zoom =
                std::clamp(std::min(kMapViewW / grid_px, kMapViewH / grid_px) * 0.9, 0.3, 3.0);
            map_camera_.center_on(kMapMargin + grid_px / 2.0, kMapMargin + grid_px / 2.0);
            edit_map_mode_ = EditMapMode::Units; // predictable default, same reasoning as active_team_ above
            screen_ = Screen::EditMap;
        } else if (id == "edit_map_mode") {
            // Switching tabs always deselects whatever event was selected
            // (see events_selected_'s comment) -- active_field_ is already
            // cleared by the generic drop-focus rule above by the time we
            // get here.
            if (edit_map_mode_ == EditMapMode::Events && static_cast<EditMapMode>(hr.arg) != EditMapMode::Events) {
                events_selected_ = -1;
            }
            if (edit_map_mode_ == EditMapMode::Objectives &&
                static_cast<EditMapMode>(hr.arg) != EditMapMode::Objectives) {
                objectives_selected_ = -1;
            }
            edit_map_mode_ = static_cast<EditMapMode>(hr.arg);
        } else if (id == "map_gen_random_toggle") {
            map_gen_random_ = !map_gen_random_;
        } else if (id == "map_gen_size") {
            map_gen_size_idx_ = (map_gen_size_idx_ + 1) % 4;
        } else if (id == "map_gen_type") {
            for (int i = 0; i < 5; ++i) {
                if (map_gen_type_ == kMapGenTypes[i]) { map_gen_type_ = kMapGenTypes[(i + 1) % 5]; break; }
            }
        } else if (id == "map_gen_default") {
            for (int i = 0; i < 4; ++i) {
                if (map_gen_default_ == kDefaultTerrains[i]) {
                    map_gen_default_ = kDefaultTerrains[(i + 1) % 4];
                    break;
                }
            }
        } else if (id == "map_gen_generate") {
            generate_map();
        } else if (id == "map_tab") {
            map_tab_ = static_cast<MapTab>(hr.arg);
        } else if (id == "pick_team") {
            active_team_ = hr.arg;
        } else if (id == "players_pick") {
            players_selected_ = hr.arg;
        } else if (id == "players_civ_prev" || id == "players_civ_next") {
            if (Level* lvl = current_level();
                lvl && players_selected_ >= 1 && players_selected_ < static_cast<int>(lvl->players.size())) {
                // Slot 0 never gets this hit-rect drawn in the first place
                // (see draw_edit_map's civ_locked), but guard here too --
                // same invariant player_civ_prev/next already relies on.
                int delta = (id == "players_civ_prev") ? 8 : 1;
                lvl->players[players_selected_].civ = (lvl->players[players_selected_].civ + delta) % 9;
            }
        } else if (id == "players_era_prev" || id == "players_era_next") {
            if (Level* lvl = current_level();
                lvl && players_selected_ >= 0 && players_selected_ < static_cast<int>(lvl->players.size())) {
                int delta = (id == "players_era_prev") ? 3 : 1; // +3 == -1 mod 4
                lvl->players[players_selected_].era = (lvl->players[players_selected_].era + delta) % 4;
            }
        } else if (id == "players_res_edit") {
            if (Level* lvl = current_level();
                lvl && players_selected_ >= 0 && players_selected_ < static_cast<int>(lvl->players.size()) &&
                hr.arg >= 0 && hr.arg < 4) {
                LevelPlayer& p = lvl->players[players_selected_];
                double* fields[4] = {&p.food, &p.wood, &p.oil, &p.iron};
                players_res_field_ = hr.arg;
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.0f", *fields[hr.arg]);
                players_res_edit_[hr.arg] = buf;
                active_field_ = Field::PlayerResource;
                SDL_StartTextInput();
            }
        } else if (id == "field_message") {
            active_field_ = Field::MessageText;
            SDL_StartTextInput();
        } else if (id == "field_event_name") {
            active_field_ = Field::EventName;
            SDL_StartTextInput();
        } else if (id == "create_event") {
            if (Level* lvl = current_level()) {
                ww::campaign::MapEvent ev;
                ev.id = ww::campaign::next_event_id(*lvl);
                ev.name = "New Event";
                ev.type = "message";
                lvl->events.push_back(std::move(ev));
                events_selected_ = static_cast<int>(lvl->events.size()) - 1;
                // Selected as soon as it exists, so typing works
                // immediately -- same convention as "create_objective".
                active_field_ = Field::EventName;
                SDL_StartTextInput();
            }
        } else if (id == "select_event") {
            if (Level* lvl = current_level();
                lvl && hr.arg >= 0 && hr.arg < static_cast<int>(lvl->events.size())) {
                events_selected_ = hr.arg;
                area_target_ = AreaTarget::EventArea; // void any pending trigger/sight-box arm
            }
        } else if (id == "delete_event") {
            if (Level* lvl = current_level();
                lvl && hr.arg >= 0 && hr.arg < static_cast<int>(lvl->events.size())) {
                lvl->events.erase(lvl->events.begin() + hr.arg);
                if (events_selected_ == hr.arg) {
                    events_selected_ = -1;
                } else if (events_selected_ > hr.arg) {
                    --events_selected_; // stays pointed at the same event after the erase shifts indices
                }
            }
        } else if (id == "event_type") {
            static const char* kEventTypes[5] = {"message", "gate", "dormant", "resources", "spawn"};
            if (Level* lvl = current_level();
                lvl && events_selected_ >= 0 && events_selected_ < static_cast<int>(lvl->events.size()) &&
                hr.arg >= 0 && hr.arg < 5) {
                ww::campaign::MapEvent& ev = lvl->events[events_selected_];
                ev.type = kEventTypes[hr.arg];
                area_target_ = AreaTarget::EventArea; // any in-progress trigger/sight box arm is void now
                // Clears whichever fields the new type doesn't use, so a
                // switched-type event never carries stale data from its
                // old one (same idea as "objective_type" clearing
                // target_unit_ids) -- gate/dormant/resources all share
                // unlock_objective_id, and gate/dormant additionally share
                // area_t*, so switching between any of those keeps
                // whichever of those two groups they have in common.
                // res_food/wood/oil/iron are left alone regardless -- an
                // unused stockpile value is harmless, and it's preserved
                // if you switch back to "resources" later.
                if (ev.type == "message") {
                    ev.area_tw = ev.area_th = 0;
                    ev.unlock_objective_id.clear();
                } else if (ev.type == "resources") {
                    // touch pickup: marker tile (tx/ty) + amounts + message; no area/objective
                    ev.area_tw = ev.area_th = 0;
                    ev.unlock_objective_id.clear();
                } else if (ev.type == "gate") {
                    ev.tx = -1;
                    ev.text.clear();
                } else { // dormant / spawn -- area_* box (dormant adds area2_*/los_areas; spawn adds spawn_*)
                    ev.tx = -1;
                }
            }
        } else if (id == "event_res_edit") {
            if (Level* lvl = current_level();
                lvl && events_selected_ >= 0 && events_selected_ < static_cast<int>(lvl->events.size()) &&
                hr.arg >= 0 && hr.arg < 4) {
                ww::campaign::MapEvent& ev = lvl->events[events_selected_];
                double fields[4] = {ev.res_food, ev.res_wood, ev.res_oil, ev.res_iron};
                event_res_field_ = hr.arg;
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.0f", fields[hr.arg]);
                event_res_edit_[hr.arg] = buf;
                active_field_ = Field::EventResource;
                SDL_StartTextInput();
            }
        } else if (id == "event_unlock_objective") {
            if (Level* lvl = current_level();
                lvl && events_selected_ >= 0 && events_selected_ < static_cast<int>(lvl->events.size())) {
                ww::campaign::MapEvent& ev = lvl->events[events_selected_];
                if (hr.arg < 0 || hr.arg >= static_cast<int>(lvl->objectives.size())) {
                    ev.unlock_objective_id.clear(); // "None"
                } else {
                    ev.unlock_objective_id = lvl->objectives[hr.arg].id;
                }
            }
        } else if (id == "draw_trigger_box") {
            // Arm the next canvas area-drag to write the dormant event's tripwire
            // box (area2_*) instead of its units box. Consumed by one finalize.
            area_target_ = AreaTarget::TriggerArea;
        } else if (id == "add_sight_box") {
            // Arm the next canvas area-drag to APPEND a line-of-sight box.
            area_target_ = AreaTarget::SightArea;
        } else if (id == "clear_sight_boxes") {
            if (Level* lvl = current_level();
                lvl && events_selected_ >= 0 && events_selected_ < static_cast<int>(lvl->events.size())) {
                lvl->events[events_selected_].los_areas.clear();
                save_current_campaign();
            }
        } else if (id == "cycle_spawn_unit") {
            if (Level* lvl = current_level(); lvl && current_campaign() && events_selected_ >= 0 &&
                events_selected_ < static_cast<int>(lvl->events.size())) {
                auto units = palette_units(current_campaign()->civ);
                if (!units.empty()) {
                    auto& ev = lvl->events[events_selected_];
                    auto it = std::find(units.begin(), units.end(), ev.spawn_unit);
                    int idx = (it == units.end()) ? -1 : static_cast<int>(it - units.begin());
                    ev.spawn_unit = units[(idx + 1) % units.size()];
                }
            }
        } else if (id == "cycle_spawn_player") {
            if (Level* lvl = current_level(); lvl && events_selected_ >= 0 &&
                events_selected_ < static_cast<int>(lvl->events.size()) && !lvl->players.empty()) {
                auto& ev = lvl->events[events_selected_];
                ev.spawn_player = (ev.spawn_player + 1) % static_cast<int>(lvl->players.size());
            }
        } else if (id == "field_spawn_count") {
            if (Level* lvl = current_level();
                lvl && events_selected_ >= 0 && events_selected_ < static_cast<int>(lvl->events.size())) {
                spawn_count_edit_ = std::to_string(lvl->events[events_selected_].spawn_count);
                active_field_ = Field::SpawnCount;
                SDL_StartTextInput();
            }
        } else if (id == "field_objective_name") {
            active_field_ = Field::ObjectiveName;
            SDL_StartTextInput();
        } else if (id == "create_objective") {
            if (Level* lvl = current_level()) {
                ww::campaign::Objective obj;
                obj.id = ww::campaign::next_objective_id(*lvl);
                obj.name = "New Objective";
                obj.type = "kill_units";
                lvl->objectives.push_back(std::move(obj));
                objectives_selected_ = static_cast<int>(lvl->objectives.size()) - 1;
                active_field_ = Field::ObjectiveName;
                SDL_StartTextInput();
            }
        } else if (id == "select_objective") {
            if (Level* lvl = current_level();
                lvl && hr.arg >= 0 && hr.arg < static_cast<int>(lvl->objectives.size())) {
                objectives_selected_ = hr.arg;
            }
        } else if (id == "delete_objective") {
            if (Level* lvl = current_level();
                lvl && hr.arg >= 0 && hr.arg < static_cast<int>(lvl->objectives.size())) {
                std::string deleted_id = lvl->objectives[hr.arg].id;
                lvl->objectives.erase(lvl->objectives.begin() + hr.arg);
                if (objectives_selected_ == hr.arg) {
                    objectives_selected_ = -1;
                } else if (objectives_selected_ > hr.arg) {
                    --objectives_selected_; // stays pointed at the same objective after the erase shifts indices
                }
                // Any gate/dormant event unlocked by the objective that
                // just vanished loses the reference and falls back to
                // "none" (see MapEvent::unlock_objective_id's comment) --
                // never left pointing at an id that no longer exists.
                if (!deleted_id.empty()) {
                    for (auto& ev : lvl->events) {
                        if (ev.unlock_objective_id == deleted_id) ev.unlock_objective_id.clear();
                    }
                }
            }
        } else if (id == "objective_type") {
            static const char* kObjectiveTypes[3] = {"kill_units", "move_to_area", "protect_unit"};
            if (Level* lvl = current_level();
                lvl && objectives_selected_ >= 0 &&
                objectives_selected_ < static_cast<int>(lvl->objectives.size()) && hr.arg >= 0 && hr.arg < 3) {
                ww::campaign::Objective& obj = lvl->objectives[objectives_selected_];
                obj.type = kObjectiveTypes[hr.arg];
                // Unit-snapshot data -- stale/meaningless the moment the
                // type changes either way (even kill_units <-> protect_unit,
                // since redrawing the area is cheap and the hint text
                // already tells you to), so drop it rather than leave a
                // move_to_area objective carrying a leftover unit snapshot.
                obj.target_unit_ids.clear();
            }
        } else if (id == "objective_visibility") {
            if (Level* lvl = current_level();
                lvl && objectives_selected_ >= 0 &&
                objectives_selected_ < static_cast<int>(lvl->objectives.size())) {
                lvl->objectives[objectives_selected_].hidden = (hr.arg == 1);
            }
        } else if (id == "pick_unit") {
            if (Level* lvl = current_level();
                lvl && active_team_ >= 0 && active_team_ < static_cast<int>(lvl->players.size())) {
                auto list = palette_units(lvl->players[active_team_].civ);
                if (hr.arg >= 0 && hr.arg < static_cast<int>(list.size())) active_unit_ = list[hr.arg];
            }
            erasing_ = false;
        } else if (id == "pick_building") {
            if (Level* lvl = current_level();
                lvl && active_team_ >= 0 && active_team_ < static_cast<int>(lvl->players.size())) {
                auto list = palette_buildings(lvl->players[active_team_].civ);
                if (hr.arg >= 0 && hr.arg < static_cast<int>(list.size())) active_building_ = list[hr.arg];
            }
            erasing_ = false; // picking something to place always exits the eraser tool
        } else if (id == "pick_terrain") {
            if (hr.arg >= 0 && hr.arg < static_cast<int>(std::size(kTerrainKinds))) {
                active_terrain_ = kTerrainKinds[hr.arg].name;
            }
            erasing_ = false;
        } else if (id == "terrain_category") {
            terrain_category_ = static_cast<Editor::TerrainCategory>(hr.arg);
            other_scroll_ = 0; // switching category invalidates the old scroll offset
        } else if (id == "cycle_vision") {
            if (Level* lvl = current_level()) lvl->reveal_mode = (lvl->reveal_mode + 1) % 3;
        } else if (id == "cycle_max_age") {
            // cycles -1(None) -> 0 -> 1 -> 2 -> 3 -> -1
            if (Level* lvl = current_level()) lvl->max_age = ((lvl->max_age + 2) % 5) - 1;
        } else if (id == "cycle_pop_cap") {
            // cycles 50 -> 75 -> ... -> 200 -> 50 (steps of 25)
            if (Level* lvl = current_level()) {
                int v = std::clamp(lvl->pop_cap, 50, 200) + 25;
                lvl->pop_cap = (v > 200) ? 50 : v;
            }
        } else if (id == "cycle_ai_behavior") {
            Level* lvl = current_level();
            if (lvl && hr.arg > 0 && hr.arg < static_cast<int>(lvl->players.size())) {
                static const char* kBeh[6] = {"default",    "passive",    "defensive",
                                              "balanced",   "aggressive", "rusher"};
                std::string& b = lvl->players[hr.arg].ai_behavior;
                int idx = 0;
                for (int k = 0; k < 6; ++k)
                    if (b == kBeh[k]) { idx = k; break; }
                b = kBeh[(idx + 1) % 6];
                save_current_campaign();
            }
        } else if (id == "upload_briefing_image") {
            upload_briefing_image();
        } else if (id == "remove_briefing_image") {
            if (Level* lvl = current_level()) {
                lvl->briefing_image.clear();
                if (briefing_tex_) { SDL_DestroyTexture(briefing_tex_); briefing_tex_ = nullptr; }
                briefing_tex_key_.clear();
                save_current_campaign();
            }
        } else if (id == "toggle_grid") {
            show_grid_ = !show_grid_;
        } else if (id == "toggle_erase") {
            erasing_ = !erasing_;
            if (erasing_) lasso_tool_ = false; // the two canvas tools are mutually exclusive
        } else if (id == "toggle_lasso") {
            lasso_tool_ = !lasso_tool_;
            if (lasso_tool_) erasing_ = false;
            // Dropping the tool (or re-arming it) clears any live selection so
            // it never lingers invisibly under the placement cursor.
            lasso_has_sel_ = false;
            lasso_marquee_ = false;
            lasso_moving_ = false;
        } else if (id == "brush_slider") {
            double frac = std::clamp(static_cast<double>(mx - kBrushSliderRect.x) / kBrushSliderRect.w, 0.0, 1.0);
            brush_size_ = 1 + static_cast<int>(std::lround(frac * 4.0));
        } else if (id == "minimap_click") {
            // Minimap-local pixel -> grid pixel -> camera's padded-world
            // space (see kMapMargin), then just re-centre there; scale
            // matches draw_edit_map's own grid_to_mini exactly.
            double grid_span_px = current_grid_size() * kEditorTile;
            double mini_scale = kMiniSize / grid_span_px;
            double gx = (mx - kMinimapRect.x) / mini_scale;
            double gy = (my - kMinimapRect.y) / mini_scale;
            map_camera_.center_on(gx + kMapMargin, gy + kMapMargin);
        } else if (id == "canvas_click") {
            // erasing_ (see its comment) pre-empts whatever edit_map_mode_
            // would otherwise do here, working the same regardless of
            // mode. Otherwise Map/Players clicking the canvas is a no-op
            // (Players' battlefield preview isn't interactive, and Map
            // mode's canvas is just a preview of what Generate produced).
            int tx, ty;
            if (!canvas_tile_at(mx, my, tx, ty)) {
                // out of the canvas entirely -- nothing to do
            } else if (erasing_) {
                erase_area_at(tx, ty);
            } else if (edit_map_mode_ == EditMapMode::Units && lasso_tool_) {
                // Lasso move tool: a press either grabs the existing selection
                // to drag it (press landed inside the box) or starts drawing a
                // new selection box. The drag itself continues in handle_event's
                // MOUSEMOTION and finishes on MOUSEBUTTONUP.
                bool inside_sel = lasso_has_sel_ && tx >= lasso_x0_ && tx <= lasso_x1_ &&
                                  ty >= lasso_y0_ && ty <= lasso_y1_;
                if (inside_sel) {
                    lasso_moving_ = true;
                    lasso_anchor_tx_ = tx;
                    lasso_anchor_ty_ = ty;
                    lasso_move_dx_ = lasso_move_dy_ = 0;
                } else {
                    lasso_marquee_ = true;
                    lasso_has_sel_ = false;
                    lasso_x0_ = lasso_x1_ = tx;
                    lasso_y0_ = lasso_y1_ = ty;
                }
            } else if (edit_map_mode_ == EditMapMode::Units) {
                place_entity_at(tx, ty);
            } else if (edit_map_mode_ == EditMapMode::Terrain) {
                paint_terrain_at(tx, ty);
            } else if (edit_map_mode_ == EditMapMode::Events) {
                reposition_selected_message_at(tx, ty);
            }
        } else if (id == "new_campaign") {
            new_campaign_name_.clear();
            new_campaign_civ_ = 0;
            screen_ = Screen::NewCampaign;
        } else if (id == "field_campaign_name") {
            active_field_ = Field::CampaignName;
            SDL_StartTextInput();
        } else if (id == "civ_prev") {
            new_campaign_civ_ = (new_campaign_civ_ + 8) % 9;
        } else if (id == "civ_next") {
            new_campaign_civ_ = (new_campaign_civ_ + 1) % 9;
        } else if (id == "create_campaign") {
            if (!new_campaign_name_.empty()) {
                Campaign c;
                c.name = new_campaign_name_;
                c.civ = new_campaign_civ_;
                ww::campaign::save_campaign(c, data_dir_);
                campaigns_.push_back(std::move(c));
                selected_campaign_ = static_cast<int>(campaigns_.size()) - 1;
                screen_ = Screen::LevelList;
            }
        } else if (id == "open_campaign") {
            selected_campaign_ = hr.arg;
            screen_ = Screen::LevelList;
        } else if (id == "open_level") {
            selected_level_ = hr.arg;
            screen_ = Screen::EditLevel;
        } else if (id == "new_level") {
            new_level_name_.clear();
            new_level_size_idx_ = 1; // "Normal"
            screen_ = Screen::NewLevel;
        } else if (id == "field_new_level_name") {
            active_field_ = Field::NewLevelName;
            SDL_StartTextInput();
        } else if (id == "new_level_size") {
            new_level_size_idx_ = (new_level_size_idx_ + 1) % 4;
        } else if (id == "create_level") {
            if (Campaign* c = current_campaign()) {
                Level lvl;
                lvl.id = ww::campaign::next_level_id(*c);
                lvl.name = new_level_name_.empty() ? "New Level" : new_level_name_;
                lvl.grid_size = kMapSizePresets[new_level_size_idx_].map_size_value * 2;
                // Every level always includes the campaign's own civ as the
                // locked, non-removable P1 slot on team 1 -- see
                // campaign_data.h's LevelPlayer comment.
                lvl.players.push_back({c->civ, 1});
                c->levels.push_back(std::move(lvl));
                selected_level_ = static_cast<int>(c->levels.size()) - 1;
                save_current_campaign();
                screen_ = Screen::EditLevel;
            }
        } else if (id == "field_level_name" || id == "field_level_desc") {
            active_field_ = (id == "field_level_name") ? Field::LevelName : Field::LevelDescription;
            SDL_StartTextInput();
        } else if (id == "add_player") {
            if (Level* lvl = current_level()) {
                // Default a fresh slot to team 2 (opposing the locked P1's
                // team 1) rather than repeating team 1, so a brand new
                // player doesn't silently start out allied with P1.
                if (lvl->players.size() < 8) lvl->players.push_back({0, 2});
            }
        } else if (id == "remove_player") {
            if (Level* lvl = current_level()) {
                // Slot 0 (the campaign's own civ) is locked -- never
                // removable, so only hr.arg >= 1 hit-rects are ever
                // registered for this id (see draw_edit_level), but guard
                // here too since that's the invariant this whole feature
                // depends on.
                if (hr.arg >= 1 && hr.arg < static_cast<int>(lvl->players.size())) {
                    lvl->players.erase(lvl->players.begin() + hr.arg);
                }
            }
        } else if (id == "player_civ_prev" || id == "player_civ_next") {
            if (Level* lvl = current_level()) {
                if (hr.arg >= 1 && hr.arg < static_cast<int>(lvl->players.size())) {
                    int delta = (id == "player_civ_prev") ? 8 : 1;
                    lvl->players[hr.arg].civ = (lvl->players[hr.arg].civ + delta) % 9;
                }
            }
        } else if (id == "player_team") {
            if (Level* lvl = current_level()) {
                if (hr.arg >= 1 && hr.arg < static_cast<int>(lvl->players.size())) {
                    lvl->players[hr.arg].team = (lvl->players[hr.arg].team % 4) + 1;
                }
            }
        } else if (id == "map_click") {
            if (Level* lvl = current_level()) {
                lvl->loc_x = std::clamp((mx - kMapRect.x) / static_cast<double>(kMapRect.w), 0.0, 1.0);
                lvl->loc_y = std::clamp((my - kMapRect.y) / static_cast<double>(kMapRect.h), 0.0, 1.0);
            }
        } else if (id == "save") {
            save_current_campaign();
        }
        return;
    }
}

void Editor::handle_right_click(int mx, int my) {
    if (screen_ != Screen::EditMap) return;
    int tx, ty;
    if (!canvas_tile_at(mx, my, tx, ty)) return;
    if (Level* lvl = current_level()) {
        lvl->terrain.erase(
            std::remove_if(lvl->terrain.begin(), lvl->terrain.end(),
                           [&](auto& t) { return t.tx == tx && t.ty == ty; }),
            lvl->terrain.end());
        lvl->units.erase(
            std::remove_if(lvl->units.begin(), lvl->units.end(),
                           [&](auto& e) { return e.tx == tx && e.ty == ty; }),
            lvl->units.end());
        // Footprint-aware: right-clicking ANY tile a multi-tile building
        // covers removes it, not just its anchor (top-left) tile.
        lvl->buildings.erase(
            std::remove_if(lvl->buildings.begin(), lvl->buildings.end(),
                           [&](auto& b) {
                               auto [bw, bh] = ww::gamedata::building_wh(b.type);
                               int wt = bw / kEditorTile, ht = bh / kEditorTile;
                               return tx >= b.tx && tx < b.tx + wt && ty >= b.ty && ty < b.ty + ht;
                           }),
            lvl->buildings.end());
        // Map events (messages/gates/dormants) are NOT touched by the
        // eraser/right-click -- like Objective, an event only exists in
        // its own sidebar list now (see the "delete_event" click handler),
        // it isn't a tile-placed thing the terrain eraser reaches.
    }
}

bool Editor::canvas_tile_at(int mx, int my, int& tx, int& ty) {
    SDL_Point p{mx, my};
    if (!SDL_PointInRect(&p, &kMapCanvasRect)) return false;
    double wx, wy;
    map_camera_.screen_to_world(mx - kMapCanvasRect.x, my - kMapCanvasRect.y, wx, wy);
    // map_camera_'s coordinate space is padded by kMapMargin on every side
    // (see its declaration) -- subtract that back out to get real grid
    // coordinates before dividing into tiles.
    int grid_size = current_grid_size();
    tx = std::clamp(static_cast<int>((wx - kMapMargin) / kEditorTile), 0, grid_size - 1);
    ty = std::clamp(static_cast<int>((wy - kMapMargin) / kEditorTile), 0, grid_size - 1);
    return true;
}

void Editor::paint_terrain_at(int tx, int ty) {
    using ww::campaign::TerrainFeature;
    Level* lvl = current_level();
    if (!lvl) return;
    const TerrainKind* k = find_terrain_kind(active_terrain_);
    if (!k) return;

    // Brush never applies to deer/fish/destroyed-building variants (single
    // point placements in the real game) regardless of brush_size_.
    int half = terrain_kind_ignores_brush(active_terrain_) ? 0 : brush_size_ / 2;
    for (int dx = -half; dx <= half; ++dx) {
        for (int dy = -half; dy <= half; ++dy) {
            int bx = tx + dx, by = ty + dy;
            if (bx < 0 || by < 0 || bx >= lvl->grid_size || by >= lvl->grid_size) {
                continue;
            }
            auto it = std::find_if(lvl->terrain.begin(), lvl->terrain.end(),
                                   [&](auto& t) { return t.tx == bx && t.ty == by; });
            bool had_water = (it != lvl->terrain.end() && it->base == "water");
            std::string new_base = (it != lvl->terrain.end()) ? it->base : std::string();
            std::string new_resource = (it != lvl->terrain.end()) ? it->resource : std::string();

            if (k->is_base) {
                // Painting a base tile (water, or one of the dirt/sand/
                // gravel/pavement variants): only "water" preserves an
                // existing fish (it still lives IN the water); every other
                // base clears whatever resource was there, same as any
                // other ground swap. "grass" maps back to an empty base
                // string on write (see kTerrainKinds' comment) rather than
                // literally storing "grass", keeping the existing
                // "TerrainFeature::base == \"\" means grass" convention
                // intact everywhere else in this file.
                new_base = (active_terrain_ == "grass") ? std::string() : k->name;
                new_resource = (k->name == std::string("water") && new_resource == "fish")
                                 ? "fish"
                                 : std::string();
                if (new_base.empty() && new_resource.empty()) {
                    // Painting grass onto a tile that ends up with nothing
                    // set at all -- drop the entry entirely (if any)
                    // rather than leave/add a degenerate empty one, same
                    // "absent tile = grass" sparseness every other level
                    // keeps.
                    if (it != lvl->terrain.end()) lvl->terrain.erase(it);
                } else if (it != lvl->terrain.end()) {
                    it->base = new_base;
                    it->resource = new_resource;
                } else {
                    lvl->terrain.push_back({bx, by, new_base, new_resource});
                }
            } else if (active_terrain_ == "fish") {
                // Fish only ever goes IN existing water -- never creates
                // water itself, and never removes it. Also never on a tile
                // a unit/building already occupies (see tile_has_entity) --
                // decorations/resources are prevented from landing on top
                // of something already standing there, unlike base/ground
                // kinds just above, which are still paintable underneath.
                if (!had_water || tile_has_entity(*lvl, bx, by)) continue;
                new_resource = "fish";
                it->resource = new_resource;
            } else {
                // Land resource/decoration: only on dry, unoccupied tiles
                // (see the fish branch's comment above) -- never replaces
                // water, never overlaps an existing unit/building.
                if (had_water || tile_has_entity(*lvl, bx, by)) continue;
                new_resource = active_terrain_;
                if (it != lvl->terrain.end()) it->resource = new_resource;
                else lvl->terrain.push_back({bx, by, new_base, new_resource});
            }

            // Terrain always wins over whatever was standing there -- BUT
            // only actually evicts an occupant that's no longer validly
            // placed on the RESULTING tile: a ship survives water (re)
            // painted or fish added under it (still legally parked, see
            // is_ship); a land unit/building survives if the new base AND
            // new resource are both passable (see TerrainKind::passable --
            // e.g. painting "dirt" or dropping "pebbles" under an existing
            // rifleman no longer displaces it). Anything not spared by
            // those rules is removed, same blanket behaviour as before
            // passable decorations existed. For the resource/decoration
            // branches above this is now effectively a no-op wherever it
            // would have evicted something -- they already skip an
            // occupied tile via tile_has_entity rather than reaching here
            // to evict it -- but it's still what actually displaces an
            // incompatible unit/building when painting a base/ground kind.
            bool new_is_water = (new_base == "water");
            const TerrainKind* base_kind = find_terrain_kind(new_base);
            const TerrainKind* res_kind = find_terrain_kind(new_resource);
            bool new_land_passable = (!base_kind || base_kind->passable) && (!res_kind || res_kind->passable);
            lvl->units.erase(std::remove_if(lvl->units.begin(), lvl->units.end(),
                                            [&](auto& u) {
                                                if (u.tx != bx || u.ty != by) return false;
                                                return !(is_ship(u.type) ? new_is_water : new_land_passable);
                                            }),
                             lvl->units.end());
            if (!new_land_passable) {
                lvl->buildings.erase(
                    std::remove_if(lvl->buildings.begin(), lvl->buildings.end(),
                                   [&](auto& b) {
                                       auto [bw, bh] = ww::gamedata::building_wh(b.type);
                                       int wt = bw / kEditorTile, ht = bh / kEditorTile;
                                       return bx >= b.tx && bx < b.tx + wt && by >= b.ty && by < b.ty + ht;
                                   }),
                    lvl->buildings.end());
            }
        }
    }
}

void Editor::erase_area_at(int tx, int ty) {
    Level* lvl = current_level();
    if (!lvl) return;
    int half = brush_size_ / 2;
    for (int dx = -half; dx <= half; ++dx) {
        for (int dy = -half; dy <= half; ++dy) {
            int bx = tx + dx, by = ty + dy;
            if (bx < 0 || by < 0 || bx >= lvl->grid_size || by >= lvl->grid_size) continue;
            // Same per-tile removal as handle_right_click (terrain, the
            // one unit there, and any building whose footprint overlaps
            // it) -- terrain and its unit/building always go together, so
            // a ship parked in water erased this way is removed right
            // along with the water underneath it.
            lvl->terrain.erase(
                std::remove_if(lvl->terrain.begin(), lvl->terrain.end(),
                               [&](auto& t) { return t.tx == bx && t.ty == by; }),
                lvl->terrain.end());
            lvl->units.erase(std::remove_if(lvl->units.begin(), lvl->units.end(),
                                            [&](auto& u) { return u.tx == bx && u.ty == by; }),
                             lvl->units.end());
            lvl->buildings.erase(
                std::remove_if(lvl->buildings.begin(), lvl->buildings.end(),
                               [&](auto& b) {
                                   auto [bw, bh] = ww::gamedata::building_wh(b.type);
                                   int wt = bw / kEditorTile, ht = bh / kEditorTile;
                                   return bx >= b.tx && bx < b.tx + wt && by >= b.ty && by < b.ty + ht;
                               }),
                lvl->buildings.end());
            // Map events aren't tile-erasable -- see handle_right_click's
            // identical comment.
        }
    }
}

void Editor::generate_map() {
    Level* lvl = current_level();
    if (!lvl) return;

    int size = kMapSizePresets[map_gen_size_idx_].map_size_value * 2;
    lvl->grid_size = size;
    lvl->terrain.clear();
    lvl->units.clear();
    lvl->buildings.clear();

    std::mt19937 rng(std::random_device{}());
    auto rand_int = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    // Upserts tile (x,y)'s base/resource -- same "find existing entry or
    // push a new one" pattern paint_terrain_at uses, just without that
    // function's brush/eviction logic (there's nothing standing on a tile
    // yet during generation).
    auto set_base = [&](int x, int y, const std::string& base) {
        if (x < 0 || y < 0 || x >= size || y >= size) return;
        auto it = std::find_if(lvl->terrain.begin(), lvl->terrain.end(),
                               [&](auto& t) { return t.tx == x && t.ty == y; });
        if (it != lvl->terrain.end()) it->base = base;
        else lvl->terrain.push_back({x, y, base, ""});
    };
    auto set_resource = [&](int x, int y, const std::string& res) {
        if (x < 0 || y < 0 || x >= size || y >= size) return;
        auto it = std::find_if(lvl->terrain.begin(), lvl->terrain.end(),
                               [&](auto& t) { return t.tx == x && t.ty == y; });
        if (it != lvl->terrain.end()) {
            if (it->base == "water") return; // never overwrite water with a land resource
            it->resource = res;
        } else {
            lvl->terrain.push_back({x, y, "", res});
        }
    };
    // Random-walk blob, same shape as the main game's World constructor
    // (sim/src/world.cpp) -- NOT a port of it (this editor has zero
    // dependency on game, see its README), just the same general
    // "walk from a random start, painting as you go" idea reimplemented
    // standalone.
    auto blob = [&](const auto& setter, int count, int len) {
        for (int i = 0; i < count; ++i) {
            int cx = rand_int(0, size - 1), cy = rand_int(0, size - 1);
            for (int s = 0; s < len; ++s) {
                setter(cx, cy);
                cx = std::clamp(cx + rand_int(-1, 1), 0, size - 1);
                cy = std::clamp(cy + rand_int(-1, 1), 0, size - 1);
            }
        }
    };

    if (map_gen_random_) {
        const std::string& type = map_gen_type_;
        if (type == "arabia") {
            for (int x = 0; x < size; ++x)
                for (int y = 0; y < size; ++y) set_base(x, y, "sand");
            blob([&](int x, int y) { set_base(x, y, ""); }, std::max(6, size / 3), 20);
        } else if (type == "arena") {
            int m = std::max(3, size / 10);
            for (int x = 0; x < size; ++x) {
                for (int y = 0; y < size; ++y) {
                    if (x < m || y < m || x >= size - m || y >= size - m) set_base(x, y, "sand");
                }
            }
            blob([&](int x, int y) { set_base(x, y, "water"); }, std::max(3, size / 6), 18);
        } else if (type == "guam") {
            int ring = std::min(20, size / 3);
            for (int x = 0; x < size; ++x)
                for (int y = 0; y < size; ++y) set_base(x, y, "sand");
            double p1 = rand_int(0, 628) / 100.0, p2 = rand_int(0, 628) / 100.0;
            for (int x = 0; x < size; ++x) {
                for (int y = 0; y < size; ++y) {
                    int wl = ring + static_cast<int>(4 * std::sin(y / 6.0 + p1));
                    int wr = ring + static_cast<int>(4 * std::sin(y / 5.0 + p2));
                    int wt = ring + static_cast<int>(4 * std::sin(x / 6.0 + p2));
                    int wb = ring + static_cast<int>(4 * std::sin(x / 5.0 + p1));
                    if (x < wl || y < wt || x >= size - wr || y >= size - wb) set_base(x, y, "water");
                }
            }
        } else if (type == "ostland") {
            blob([&](int x, int y) { set_base(x, y, "water"); }, std::max(4, size / 3), 22);
        } else { // "random"
            blob([&](int x, int y) { set_base(x, y, "water"); }, std::max(4, size / 3), 22);
            blob([&](int x, int y) { set_base(x, y, "sand"); }, std::max(3, size / 5), 16);
        }
        // Scatter gatherable resources + deer across the whole grid --
        // simple uniform random points rather than scenario.cpp's
        // clustered placement, kept deliberately simple.
        for (int i = 0, n = std::max(10, size / 2); i < n; ++i) {
            set_resource(rand_int(0, size - 1), rand_int(0, size - 1), "tree");
        }
        for (int i = 0, n = std::max(3, size / 8); i < n; ++i) {
            set_resource(rand_int(0, size - 1), rand_int(0, size - 1), "berry");
        }
        for (int i = 0, n = rand_int(3, 5); i < n; ++i) {
            set_resource(rand_int(0, size - 1), rand_int(0, size - 1), "oil");
        }
        for (int i = 0, n = rand_int(3, 4); i < n; ++i) {
            set_resource(rand_int(0, size - 1), rand_int(0, size - 1), "iron");
        }
        for (int i = 0, n = rand_int(2, 4); i < n; ++i) {
            set_resource(rand_int(0, size - 1), rand_int(0, size - 1), "deer");
        }
    } else {
        // Blank map: a single uniform default_terrain fill (or, for
        // "grass", nothing at all -- lvl->terrain is already cleared and
        // an absent tile already means grass, same as everywhere else in
        // this editor).
        if (map_gen_default_ != "grass") {
            bool as_resource = (map_gen_default_ == "tree");
            for (int x = 0; x < size; ++x) {
                for (int y = 0; y < size; ++y) {
                    if (as_resource) lvl->terrain.push_back({x, y, "", map_gen_default_});
                    else lvl->terrain.push_back({x, y, map_gen_default_, ""});
                }
            }
        }
    }

    // Fresh grid, fresh view -- same fit-to-view reset "edit_map"'s own
    // click handler does when this screen first opens.
    active_team_ = -1;
    active_unit_.clear();
    active_building_.clear();
    double grid_px = size * kEditorTile;
    map_camera_.zoom = std::clamp(std::min(kMapViewW / grid_px, kMapViewH / grid_px) * 0.9, 0.3, 3.0);
    map_camera_.center_on(kMapMargin + grid_px / 2.0, kMapMargin + grid_px / 2.0);
}

bool Editor::footprint_clear(const ww::campaign::Level& lvl, int tx, int ty, int wt, int ht,
                             bool is_ship) const {
    if (tx < 0 || ty < 0 || tx + wt > lvl.grid_size || ty + ht > lvl.grid_size) {
        return false;
    }
    for (int x = tx; x < tx + wt; ++x) {
        for (int y = ty; y < ty + ht; ++y) {
            bool tile_is_water = false;
            std::string tile_base, tile_resource;
            for (auto& t : lvl.terrain) {
                if (t.tx != x || t.ty != y) continue;
                if (t.base == "water") tile_is_water = true;
                tile_base = t.base;
                tile_resource = t.resource;
            }
            // Ships need water under every footprint tile; everything else
            // (land units, all buildings) is blocked unless BOTH the base
            // and the resource on that tile are passable (see
            // TerrainKind::passable -- most decorations block, but ground-
            // texture reskins like dirt/sand and small litter like pebbles
            // don't).
            if (is_ship) {
                if (!tile_is_water) return false;
            } else {
                const TerrainKind* base_kind = find_terrain_kind(tile_base);
                const TerrainKind* res_kind = find_terrain_kind(tile_resource);
                bool passable = (!base_kind || base_kind->passable) && (!res_kind || res_kind->passable);
                if (!passable) return false;
            }
            for (auto& u : lvl.units) {
                if (u.tx == x && u.ty == y) return false;
            }
            for (auto& b : lvl.buildings) {
                auto [bw, bh] = ww::gamedata::building_wh(b.type);
                int bwt = bw / kEditorTile, bht = bh / kEditorTile;
                if (x >= b.tx && x < b.tx + bwt && y >= b.ty && y < b.ty + bht) return false;
            }
        }
    }
    return true;
}

bool Editor::tile_has_entity(const ww::campaign::Level& lvl, int tx, int ty) const {
    for (auto& u : lvl.units) {
        if (u.tx == tx && u.ty == ty) return true;
    }
    for (auto& b : lvl.buildings) {
        auto [bw, bh] = ww::gamedata::building_wh(b.type);
        int wt = bw / kEditorTile, ht = bh / kEditorTile;
        if (tx >= b.tx && tx < b.tx + wt && ty >= b.ty && ty < b.ty + ht) return true;
    }
    return false;
}

bool Editor::is_ship(const std::string& unit) const {
    auto& units = data_.catalog().at("units");
    return units.contains(unit) && units.at(unit).value("ship", false);
}

void Editor::place_entity_at(int tx, int ty) {
    Level* lvl = current_level();
    if (!lvl) return;
    if (active_team_ < 0 || active_team_ >= static_cast<int>(lvl->players.size())) return;

    if (map_tab_ == MapTab::Units) {
        if (!active_unit_.empty() && footprint_clear(*lvl, tx, ty, 1, 1, is_ship(active_unit_))) {
            lvl->units.push_back({active_unit_, active_team_, tx, ty});
        }
    } else if (map_tab_ == MapTab::Buildings) {
        if (active_building_.empty()) return;
        auto [bw, bh] = ww::gamedata::building_wh(active_building_);
        int wt = bw / kEditorTile, ht = bh / kEditorTile;
        if (footprint_clear(*lvl, tx, ty, wt, ht)) {
            lvl->buildings.push_back({active_building_, active_team_, tx, ty});
        }
    }
}

void Editor::commit_lasso_move() {
    using ww::campaign::PlacedEntity;
    using ww::campaign::TerrainFeature;
    Level* lvl = current_level();
    if (!lvl) return;
    const int dx = lasso_move_dx_, dy = lasso_move_dy_;
    if (dx == 0 && dy == 0) return;
    const int x0 = lasso_x0_, x1 = lasso_x1_, y0 = lasso_y0_, y1 = lasso_y1_;
    auto in_sel = [&](int tx, int ty) { return tx >= x0 && tx <= x1 && ty >= y0 && ty <= y1; };
    auto footprint = [&](const std::string& type) {
        auto [bw, bh] = ww::gamedata::building_wh(type);
        return std::pair<int, int>{std::max(1, bw / kEditorTile), std::max(1, bh / kEditorTile)};
    };

    // 1) Snapshot the selected things (anchor tile inside the box). Grass
    //    (absent terrain) isn't a thing, so it's naturally excluded.
    std::vector<PlacedEntity> moved_units, moved_buildings;
    std::vector<TerrainFeature> moved_terrain;
    for (auto& u : lvl->units) if (in_sel(u.tx, u.ty)) moved_units.push_back(u);
    for (auto& b : lvl->buildings) if (in_sel(b.tx, b.ty)) moved_buildings.push_back(b);
    for (auto& t : lvl->terrain) if (in_sel(t.tx, t.ty)) moved_terrain.push_back(t);
    if (moved_units.empty() && moved_buildings.empty() && moved_terrain.empty()) return;

    // 2) Lift them out of the level -- their source tiles revert to grass.
    lvl->units.erase(std::remove_if(lvl->units.begin(), lvl->units.end(),
                                    [&](auto& u) { return in_sel(u.tx, u.ty); }), lvl->units.end());
    lvl->buildings.erase(std::remove_if(lvl->buildings.begin(), lvl->buildings.end(),
                                        [&](auto& b) { return in_sel(b.tx, b.ty); }), lvl->buildings.end());
    lvl->terrain.erase(std::remove_if(lvl->terrain.begin(), lvl->terrain.end(),
                                      [&](auto& t) { return in_sel(t.tx, t.ty); }), lvl->terrain.end());

    // 3) Destination tiles a moved building/terrain will occupy (units don't
    //    clear what's underneath, per the spec, so they don't contribute).
    std::set<std::pair<int, int>> dest;
    for (auto& t : moved_terrain) dest.insert({t.tx + dx, t.ty + dy});
    for (auto& b : moved_buildings) {
        auto [wt, ht] = footprint(b.type);
        for (int ax = 0; ax < wt; ++ax)
            for (int ay = 0; ay < ht; ++ay) dest.insert({b.tx + dx + ax, b.ty + dy + ay});
    }

    // 4) Delete buildings/terrain (NEVER units) sitting under the destination.
    lvl->terrain.erase(std::remove_if(lvl->terrain.begin(), lvl->terrain.end(),
                                      [&](auto& t) { return dest.count({t.tx, t.ty}) > 0; }),
                       lvl->terrain.end());
    lvl->buildings.erase(std::remove_if(lvl->buildings.begin(), lvl->buildings.end(),
                                        [&](auto& b) {
                                            auto [wt, ht] = footprint(b.type);
                                            for (int ax = 0; ax < wt; ++ax)
                                                for (int ay = 0; ay < ht; ++ay)
                                                    if (dest.count({b.tx + ax, b.ty + ay})) return true;
                                            return false;
                                        }),
                         lvl->buildings.end());

    // 5) Drop the moved things at their shifted positions.
    for (auto t : moved_terrain) { t.tx += dx; t.ty += dy; lvl->terrain.push_back(t); }
    for (auto b : moved_buildings) { b.tx += dx; b.ty += dy; lvl->buildings.push_back(b); }
    for (auto u : moved_units) { u.tx += dx; u.ty += dy; lvl->units.push_back(u); }

    // 6) Selection follows the group so it can be dragged again immediately.
    lasso_x0_ += dx; lasso_x1_ += dx; lasso_y0_ += dy; lasso_y1_ += dy;
}

void Editor::upload_briefing_image() {
    Level* lvl = current_level();
    if (!lvl) return;
#ifdef _WIN32
    // Native file picker -- pick any PNG/JPG/BMP on disk.
    char path[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "Images\0*.png;*.jpg;*.jpeg;*.bmp\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Choose a briefing image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return; // cancelled
    std::filesystem::path src(path);
#else
    return; // file dialog only wired for Windows
#endif
    namespace fs = std::filesystem;
    std::error_code ec;
    // Copy it into asset_dir_/campaign_images/ under a stable per-level name so
    // re-uploading replaces the old one instead of piling up files. The level
    // id is stable and filesystem-safe (auto-generated, see next_level_id).
    fs::path dir = fs::path(asset_dir_) / "campaign_images";
    fs::create_directories(dir, ec);
    std::string ext = src.extension().string();
    if (ext.empty()) ext = ".png";
    std::string base = lvl->id.empty() ? "briefing" : ("briefing_" + lvl->id);
    fs::path dst = dir / (base + ext);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) return; // copy failed -- leave the level's image unchanged
    lvl->briefing_image = "campaign_images/" + base + ext;
    // Invalidate the cached texture so the preview reloads the new file.
    if (briefing_tex_) { SDL_DestroyTexture(briefing_tex_); briefing_tex_ = nullptr; }
    briefing_tex_key_.clear();
    save_current_campaign();
}

void Editor::reposition_selected_message_at(int tx, int ty) {
    Level* lvl = current_level();
    if (!lvl || events_selected_ < 0 || events_selected_ >= static_cast<int>(lvl->events.size())) return;
    ww::campaign::MapEvent& ev = lvl->events[events_selected_];
    // message and resources both drop a single marker tile; gate/dormant/spawn
    // use the drag-an-area tool instead (see area_dragging_).
    if (ev.type != "message" && ev.type != "resources") return;
    ev.tx = tx;
    ev.ty = ty;
}

void Editor::finalize_objective_area() {
    Level* lvl = current_level();
    if (!lvl || objectives_selected_ < 0 || objectives_selected_ >= static_cast<int>(lvl->objectives.size())) {
        return;
    }
    ww::campaign::Objective& obj = lvl->objectives[objectives_selected_];
    obj.area_tx = std::min(area_drag_tx0_, area_drag_tx1_);
    obj.area_ty = std::min(area_drag_ty0_, area_drag_ty1_);
    obj.area_tw = std::abs(area_drag_tx1_ - area_drag_tx0_) + 1;
    obj.area_th = std::abs(area_drag_ty1_ - area_drag_ty0_) + 1;

    if (obj.type != "kill_units" && obj.type != "protect_unit") {
        // move_to_area (or any future type that isn't a unit snapshot)
        // doesn't capture anything here -- "the player's units enter this
        // area" is a live condition checked during play, not a one-time
        // snapshot of what's standing there right now (see Objective's
        // comment) -- just the area itself matters.
        obj.target_unit_ids.clear();
        return;
    }

    // Re-captured from scratch every time the area is redrawn (see
    // Objective's comment) -- a one-time snapshot of whatever's in the
    // rectangle right now, not a live query. Lazily assigns a stable id
    // (see PlacedEntity::id) to any unit that doesn't already have one.
    // Shared by kill_units and protect_unit -- both snapshot "whichever
    // units are in here right now", they just disagree on what happens to
    // those units afterward (die vs. survive, see Objective's comment).
    obj.target_unit_ids.clear();
    for (auto& u : lvl->units) {
        if (u.tx < obj.area_tx || u.tx >= obj.area_tx + obj.area_tw) continue;
        if (u.ty < obj.area_ty || u.ty >= obj.area_ty + obj.area_th) continue;
        if (u.id.empty()) u.id = ww::campaign::next_entity_id(*lvl);
        obj.target_unit_ids.push_back(u.id);
    }
}

void Editor::finalize_event_area() {
    Level* lvl = current_level();
    if (!lvl || events_selected_ < 0 || events_selected_ >= static_cast<int>(lvl->events.size())) return;
    ww::campaign::MapEvent& ev = lvl->events[events_selected_];
    // Only gate/dormant/spawn use an area box; message/resources use a marker
    // tile (reposition_selected_message_at) instead.
    if (ev.type != "gate" && ev.type != "dormant" && ev.type != "spawn") return;
    int nx = std::min(area_drag_tx0_, area_drag_tx1_);
    int ny = std::min(area_drag_ty0_, area_drag_ty1_);
    int nw = std::abs(area_drag_tx1_ - area_drag_tx0_) + 1;
    int nh = std::abs(area_drag_ty1_ - area_drag_ty0_) + 1;
    if (area_target_ == AreaTarget::TriggerArea && ev.type == "dormant") {
        ev.area2_tx = nx;
        ev.area2_ty = ny;
        ev.area2_tw = nw;
        ev.area2_th = nh;
    } else if (area_target_ == AreaTarget::SightArea && ev.type == "dormant") {
        ev.los_areas.push_back({nx, ny, nw, nh});
    } else {
        // EventArea: the gate box / dormant "units" box / spawn box. No unit
        // snapshot -- all are live queries at play time (see MapEvent's comment).
        ev.area_tx = nx;
        ev.area_ty = ny;
        ev.area_tw = nw;
        ev.area_th = nh;
    }
    area_target_ = AreaTarget::EventArea; // one-shot arm consumed
    save_current_campaign();
}

void Editor::draw_panel(SDL_Renderer* renderer, const SDL_Rect& r) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, &r);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &r);
}

void Editor::draw_button(SDL_Renderer* renderer, const SDL_Rect& r, const std::string& label,
                        const std::string& id, int arg, SDL_Color border) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, &r);
    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(renderer, &r);
    if (!label.empty()) {
        int size = std::clamp(r.h - 15, 14, 32);
        int tw, th;
        text_.measure(label, size, tw, th);
        while (tw > r.w - 12 && size > 12) {
            size -= 1;
            text_.measure(label, size, tw, th);
        }
        text_.draw(label, r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, {255, 255, 255, 255}, size);
    }
    if (!id.empty()) hit_rects_.push_back({r, id, arg});
}

void Editor::draw_text_field(SDL_Renderer* renderer, const SDL_Rect& r, std::string& value, Field field,
                             const std::string& placeholder) {
    bool focused = (active_field_ == field);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, &r);
    SDL_SetRenderDrawColor(renderer, focused ? 255 : 150, focused ? 220 : 150, focused ? 60 : 150, 255);
    SDL_RenderDrawRect(renderer, &r);
    std::string display = value.empty() && !focused ? placeholder : value;
    if (focused) display += "_";
    SDL_Color colour = (value.empty() && !focused) ? SDL_Color{140, 140, 140, 255} : SDL_Color{255, 255, 255, 255};
    text_regular_.draw(display, r.x + 9, r.y + (r.h - 21) / 2, colour, 21);

    std::string id;
    if (field == Field::CampaignName) id = "field_campaign_name";
    else if (field == Field::NewLevelName) id = "field_new_level_name";
    else if (field == Field::LevelName) id = "field_level_name";
    else if (field == Field::LevelDescription) id = "field_level_desc";
    else if (field == Field::MessageText) id = "field_message";
    else if (field == Field::ObjectiveName) id = "field_objective_name";
    else if (field == Field::EventName) id = "field_event_name";
    else if (field == Field::SpawnCount) id = "field_spawn_count";
    if (!id.empty()) hit_rects_.push_back({r, id, 0});
}

void Editor::draw_text_area(SDL_Renderer* renderer, const SDL_Rect& r, std::string& value, Field field,
                            const std::string& placeholder) {
    bool focused = (active_field_ == field);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, &r);
    SDL_SetRenderDrawColor(renderer, focused ? 255 : 150, focused ? 220 : 150, focused ? 60 : 150, 255);
    SDL_RenderDrawRect(renderer, &r);

    std::string display = value.empty() && !focused ? placeholder : value;
    if (focused) display += "_";
    SDL_Color colour = (value.empty() && !focused) ? SDL_Color{140, 140, 140, 255} : SDL_Color{255, 255, 255, 255};
    constexpr int kSize = 19, kLineH = 24;
    int y = r.y + 6;
    for (auto& line : wrap_lines(text_regular_, display, kSize, r.w - 18)) {
        if (y > r.y + r.h - kLineH + 3) break; // clip rather than overflow the box
        text_regular_.draw(line, r.x + 9, y, colour, kSize);
        y += kLineH;
    }

    std::string id;
    if (field == Field::LevelDescription) id = "field_level_desc";
    if (!id.empty()) hit_rects_.push_back({r, id, 0});
}

void Editor::draw_frame_chrome(SDL_Renderer* renderer, bool show_back) {
    draw_panel(renderer, kPanel);
    if (show_back) draw_button(renderer, kBackRect, "<", "back", 0, {255, 60, 60, 255});
    draw_button(renderer, kQuitRect, "X", "quit", 0, {255, 60, 60, 255});
}

void Editor::draw(SDL_Renderer* renderer) {
    hit_rects_.clear();
    SDL_SetRenderDrawColor(renderer, 40, 40, 55, 255);
    SDL_RenderClear(renderer);

    switch (screen_) {
        case Screen::CampaignList: draw_campaign_list(renderer); break;
        case Screen::NewCampaign: draw_new_campaign(renderer); break;
        case Screen::LevelList: draw_level_list(renderer); break;
        case Screen::NewLevel: draw_new_level(renderer); break;
        case Screen::EditLevel: draw_edit_level(renderer); break;
        case Screen::EditMap: draw_edit_map(renderer); break;
    }

    // Brief "Saved" toast, top-right of the panel, for a couple seconds
    // after any save_current_campaign() call -- the only feedback that a
    // click-away/Save actually wrote to disk.
    double now = SDL_GetTicks() / 1000.0;
    if (now - last_save_flash_ < 2.0) {
        text_regular_.draw("Saved.", kPanel.x + kPanel.w - 105, kPanel.y + 12, {120, 255, 120, 255}, 21);
    }

    // Custom cursor, matching the main game's own convention (GameClient::
    // post_construct disables the system cursor and draws spr_mouse itself
    // every frame instead, see game_client.cpp) rather than this tool
    // showing a plain OS arrow -- swapped to the eraser icon while the
    // eraser tool is active (erasing_) so it's obvious what clicking will
    // do, same idea as the main game's own attack-cursor swap.
    const char* cursor_sprite = erasing_ ? "spr_erase" : "spr_mouse";
    if (atlas_.meta(cursor_sprite)) atlas_.draw(cursor_sprite, mouse_pos_.x, mouse_pos_.y, 0, 1.0);
}

void Editor::draw_campaign_list(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/false);
    std::string title = "Campaigns";
    int tw, th;
    text_.measure(title, 39, tw, th);
    text_.draw(title, kPanel.x + (kPanel.w - tw) / 2, kPanel.y + 24, {255, 255, 255, 255}, 39);

    int row_y = kPanel.y + 90;
    const int row_h = 66;
    for (int i = 0; i < static_cast<int>(campaigns_.size()); ++i) {
        const Campaign& c = campaigns_[i];
        SDL_Rect row{kPanel.x + 30, row_y, kPanel.w - 60, row_h - 9};
        SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255);
        SDL_RenderFillRect(renderer, &row);
        SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
        SDL_RenderDrawRect(renderer, &row);

        std::string civ_name = (c.civ >= 0 && c.civ < 9) ? ww::menu::civ_names()[c.civ] : "?";
        std::string label = c.name + "  -  " + civ_name;
        text_.draw(label, row.x + 15, row.y + 9, {255, 255, 255, 255}, 24);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d level%s", static_cast<int>(c.levels.size()),
                     c.levels.size() == 1 ? "" : "s");
        text_regular_.draw(buf, row.x + row.w - 150, row.y + 12, {200, 200, 200, 255}, 19);

        hit_rects_.push_back({row, "open_campaign", i});
        row_y += row_h;
    }
    if (campaigns_.empty()) {
        text_regular_.draw("No campaigns yet -- click New Campaign to create one.", kPanel.x + 30, row_y + 15,
                           {180, 180, 180, 255}, 21);
    }

    SDL_Rect new_btn{kPanel.x + (kPanel.w - 330) / 2, kPanel.y + kPanel.h - 90, 330, 66};
    draw_button(renderer, new_btn, "New Campaign", "new_campaign", 0, {60, 220, 60, 255});
}

void Editor::draw_new_campaign(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);
    std::string title = "New Campaign";
    int tw, th;
    text_.measure(title, 36, tw, th);
    text_.draw(title, kPanel.x + (kPanel.w - tw) / 2, kPanel.y + 24, {255, 255, 255, 255}, 36);

    text_regular_.draw("Name:", kPanel.x + 60, kPanel.y + 120, {255, 255, 255, 255}, 22);
    SDL_Rect name_field{kPanel.x + 60, kPanel.y + 150, 540, 48};
    draw_text_field(renderer, name_field, new_campaign_name_, Field::CampaignName, "Campaign name...");

    text_regular_.draw("Civilization:", kPanel.x + 60, kPanel.y + 225, {255, 255, 255, 255}, 22);
    SDL_Rect prev_btn{kPanel.x + 60, kPanel.y + 258, 48, 48};
    draw_button(renderer, prev_btn, "<", "civ_prev");
    SDL_Rect flag_box{kPanel.x + 120, kPanel.y + 258, 144, 48};
    draw_panel(renderer, flag_box);
    if (atlas_.meta("spr_flags_mini")) {
        atlas_.draw_in_rect(flag_box, "spr_flags_mini", new_campaign_civ_, 2);
    }
    text_regular_.draw(ww::menu::civ_names()[new_campaign_civ_], kPanel.x + 276, kPanel.y + 273,
                       {255, 255, 255, 255}, 22);
    SDL_Rect next_btn{kPanel.x + 540, kPanel.y + 258, 48, 48};
    draw_button(renderer, next_btn, ">", "civ_next");

    SDL_Rect create_btn{kPanel.x + 60, kPanel.y + kPanel.h - 105, 300, 66};
    draw_button(renderer, create_btn, "Create", "create_campaign", 0, {60, 220, 60, 255});
}

void Editor::draw_new_level(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);
    std::string title = "New Level";
    int tw, th;
    text_.measure(title, 36, tw, th);
    text_.draw(title, kPanel.x + (kPanel.w - tw) / 2, kPanel.y + 24, {255, 255, 255, 255}, 36);

    text_regular_.draw("Name:", kPanel.x + 60, kPanel.y + 120, {255, 255, 255, 255}, 22);
    SDL_Rect name_field{kPanel.x + 60, kPanel.y + 150, 540, 48};
    draw_text_field(renderer, name_field, new_level_name_, Field::NewLevelName, "Level name...");

    // Map size: same Tiny/Normal/Large/Huge presets (and resulting grid
    // size) as the main game's own Random Map Setup screen -- see
    // kMapSizePresets. Locked in at creation time only: changing it later
    // would invalidate any terrain/units/buildings already placed against
    // the old grid dimensions.
    text_regular_.draw("Map size:", kPanel.x + 60, kPanel.y + 225, {255, 255, 255, 255}, 22);
    const MapSizePreset& preset = kMapSizePresets[new_level_size_idx_];
    int grid_tiles = preset.map_size_value * 2;
    SDL_Rect size_btn{kPanel.x + 60, kPanel.y + 258, 240, 48};
    draw_button(renderer, size_btn, preset.name, "new_level_size", 0, {255, 220, 60, 255});
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d x %d tiles", grid_tiles, grid_tiles);
    text_regular_.draw(buf, kPanel.x + 315, kPanel.y + 273, {200, 200, 200, 255}, 21);

    SDL_Rect create_btn{kPanel.x + 60, kPanel.y + kPanel.h - 105, 300, 66};
    draw_button(renderer, create_btn, "Create", "create_level", 0, {60, 220, 60, 255});
}

void Editor::draw_level_list(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);
    Campaign* c = current_campaign();
    if (!c) return;

    std::string civ_name = (c->civ >= 0 && c->civ < 9) ? ww::menu::civ_names()[c->civ] : "?";
    std::string title = c->name + " (" + civ_name + ")";
    int tw, th;
    text_.measure(title, 33, tw, th);
    text_.draw(title, kPanel.x + (kPanel.w - tw) / 2, kPanel.y + 24, {255, 255, 255, 255}, 33);

    int row_y = kPanel.y + 90;
    const int row_h = 60;
    for (int i = 0; i < static_cast<int>(c->levels.size()); ++i) {
        const Level& lvl = c->levels[i];
        SDL_Rect row{kPanel.x + 30, row_y, kPanel.w - 60, row_h - 9};
        SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255);
        SDL_RenderFillRect(renderer, &row);
        SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
        SDL_RenderDrawRect(renderer, &row);
        char num[16];
        std::snprintf(num, sizeof(num), "%d.", i + 1);
        text_.draw(std::string(num) + " " + (lvl.name.empty() ? "(untitled)" : lvl.name), row.x + 15,
                  row.y + 9, {255, 255, 255, 255}, 24);
        hit_rects_.push_back({row, "open_level", i});
        row_y += row_h;
    }
    if (c->levels.empty()) {
        text_regular_.draw("No levels yet -- click New Level to add one.", kPanel.x + 30, row_y + 15,
                           {180, 180, 180, 255}, 21);
    }

    SDL_Rect new_btn{kPanel.x + (kPanel.w - 300) / 2, kPanel.y + kPanel.h - 90, 300, 66};
    draw_button(renderer, new_btn, "New Level", "new_level", 0, {60, 220, 60, 255});
}

void Editor::draw_edit_level(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);
    Level* lvl = current_level();
    if (!lvl) return;

    text_regular_.draw("Name:", kPanel.x + 30, kPanel.y + 18, {255, 255, 255, 255}, 21);
    SDL_Rect name_field{kPanel.x + 30, kPanel.y + 42, 440, 40};
    draw_text_field(renderer, name_field, lvl->name, Field::LevelName, "Level name...");

    // Multi-line, word-wrapped ("up to a paragraph" of background text) --
    // confined to the same left-column width as the players list below it
    // (not full panel width), so it doesn't run under the Europe map,
    // which sits to the right starting at kMapRect.x. Given more than
    // double its old height (120 -> 260) now that the taller 1280x960
    // panel has the room, since more room for the description specifically
    // was the point; the players list below is pushed down to match.
    text_regular_.draw("Description:", kPanel.x + 30, kPanel.y + 96, {255, 255, 255, 255}, 21);
    SDL_Rect desc_field{kPanel.x + 30, kPanel.y + 120, 440, 260};
    draw_text_area(renderer, desc_field, lvl->description, Field::LevelDescription,
                   "Background/briefing text...");

    // Starting fog-of-war state -- mirrors single-player's Standard / No fog /
    // Revealed option (sim reveal_mode). Cycles on click. Sits in the free
    // band above the Europe map (kMapRect starts at kPanel.y + 129).
    static const char* kVisionLabels[3] = {"Standard (fog)", "No fog", "Revealed"};
    int vis_mode = std::clamp(lvl->reveal_mode, 0, 2);
    text_regular_.draw("Starting vision:", kPanel.x + 490, kPanel.y + 18, {255, 255, 255, 255}, 21);
    SDL_Rect vis_btn{kPanel.x + 490, kPanel.y + 46, 300, 44};
    draw_button(renderer, vis_btn, kVisionLabels[vis_mode], "cycle_vision", 0, {255, 220, 60, 255});

    // Optional whole-level age cap: the highest era any player (human or AI) may
    // advance to (Level::max_age, -1 = no cap). Cycles None -> Victorian ->
    // Industrial -> War -> Scientific. Enforced by the engine in new_from_level.
    static const char* kAgeLabels[5] = {"None", "Victorian", "Industrial", "War", "Scientific"};
    int age_idx = std::clamp(lvl->max_age + 1, 0, 4);
    text_regular_.draw("Max age:", kPanel.x + 810, kPanel.y + 18, {255, 255, 255, 255}, 21);
    SDL_Rect age_btn{kPanel.x + 810, kPanel.y + 46, 300, 44};
    draw_button(renderer, age_btn, kAgeLabels[age_idx], "cycle_max_age", 0, {255, 220, 60, 255});

    // Whole-level population cap (Level::pop_cap), 50..200 in steps of 25.
    text_regular_.draw("Pop cap:", kPanel.x + 1130, kPanel.y + 18, {255, 255, 255, 255}, 21);
    SDL_Rect pop_btn{kPanel.x + 1130, kPanel.y + 46, 260, 44};
    draw_button(renderer, pop_btn, std::to_string(std::clamp(lvl->pop_cap, 50, 200)), "cycle_pop_cap", 0,
                {255, 220, 60, 255});

    // Players: up to 8 rows. Row 0 is always the campaign's own civ,
    // locked to team 1 and non-removable (see campaign_data.h); every
    // other row gets a civ picker, a team cycle button (1-4), and a
    // remove button. Row height is intentionally tight to fit all 8.
    text_regular_.draw("Players (P1 is fixed; others pick a team, 1-4):", kPanel.x + 30, kPanel.y + 392,
                       {255, 255, 255, 255}, 19);
    const int row_h = 28, row_stride = 30;
    // Column x-offsets, all kept left of kMapRect.x (kPanel.x+480) so the
    // map -- drawn after this loop -- never paints over a player row's
    // buttons (see kMapRect's comment; this collided at the old +300).
    const int col_prev = kPanel.x + 30, col_flag = kPanel.x + 63, col_next = kPanel.x + 144;
    const int col_name = kPanel.x + 177, col_name_end = kPanel.x + 340;
    const int col_team = kPanel.x + 344, col_remove = kPanel.x + 436;
    int py = kPanel.y + 413;
    for (int i = 0; i < static_cast<int>(lvl->players.size()); ++i) {
        LevelPlayer& p = lvl->players[i];
        bool locked = (i == 0);
        if (!locked) {
            SDL_Rect prev_btn{col_prev, py, 27, row_h};
            draw_button(renderer, prev_btn, "<", "player_civ_prev", i);
        }
        SDL_Rect flag_box{col_flag, py, 75, row_h};
        draw_panel(renderer, flag_box);
        if (atlas_.meta("spr_flags_mini")) atlas_.draw_in_rect(flag_box, "spr_flags_mini", p.civ, 1);
        if (!locked) {
            SDL_Rect next_btn{col_next, py, 27, row_h};
            draw_button(renderer, next_btn, ">", "player_civ_next", i);
        }

        // Shrink-to-fit (same idea as draw_button's own label sizing) --
        // this column is only ~163px, too narrow for names like "Ottoman
        // Empire" at a fixed size without spilling into the Team column.
        std::string name = (p.civ >= 0 && p.civ < 9) ? ww::menu::civ_names()[p.civ] : "?";
        int name_size = 18, tw, th;
        text_regular_.measure(name, name_size, tw, th);
        while (tw > col_name_end - col_name && name_size > 10) {
            --name_size;
            text_regular_.measure(name, name_size, tw, th);
        }
        text_regular_.draw(name, col_name, py + (row_h - th) / 2, {255, 255, 255, 255}, name_size);

        if (locked) {
            // No prev/next/remove buttons drawn for this row already makes
            // it visibly locked -- keep this label short so it can't
            // overflow into the map, which starts right after this column.
            text_regular_.draw("Team 1", col_team, py + (row_h - 16) / 2, {170, 190, 255, 255}, 16);
        } else {
            SDL_Rect team_btn{col_team, py, col_remove - col_team - 6, row_h};
            draw_button(renderer, team_btn, "Team " + std::to_string(p.team), "player_team", i,
                       kTeamColours[std::clamp(p.team - 1, 0, 3)]);
            SDL_Rect remove_btn{col_remove, py, 27, row_h};
            draw_button(renderer, remove_btn, "x", "remove_player", i, {255, 90, 90, 255});
        }
        py += row_stride;
    }
    if (lvl->players.size() < 8) {
        SDL_Rect add_btn{kPanel.x + 30, py, 180, 28};
        draw_button(renderer, add_btn, "+ Add Player", "add_player");
    }

    // Europe map -- click to place this level's dot.
    text_regular_.draw("Location (click map):", kMapRect.x, kMapRect.y - 27, {255, 255, 255, 255}, 21);
    draw_panel(renderer, kMapRect);
    if (atlas_.meta("spr_europe_map")) {
        atlas_.draw_stretched("spr_europe_map", kMapRect, 0);
    }
    hit_rects_.push_back({kMapRect, "map_click", 0});
    int dot_x = kMapRect.x + static_cast<int>(lvl->loc_x * kMapRect.w);
    int dot_y = kMapRect.y + static_cast<int>(lvl->loc_y * kMapRect.h);
    SDL_Rect dot{dot_x - 6, dot_y - 6, 12, 12};
    SDL_SetRenderDrawColor(renderer, 255, 40, 40, 255);
    SDL_RenderFillRect(renderer, &dot);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &dot);

    // Battlefield preview -- a plain (non-interactive) minimap of this
    // level's own terrain/units/buildings, same dot rendering EditMap's
    // own minimap uses (see draw_minimap_contents), so what this level
    // "looks like" is visible without having to open Edit Map at all.
    // Sits directly right of the Europe map, top-aligned with it and
    // sized to its height (draw_minimap_contents assumes a square rect),
    // so the two fill the panel's right column together instead of one
    // small box stacked awkwardly under the other with empty space
    // around both.
    SDL_Rect preview_rect{kMapRect.x + kMapRect.w + 24, kMapRect.y, kMapRect.h, kMapRect.h};
    text_regular_.draw("Battlefield preview:", preview_rect.x, preview_rect.y - 27, {255, 255, 255, 255}, 21);
    draw_minimap_contents(renderer, preview_rect);

    SDL_Rect map_btn{kPanel.x + kPanel.w - 360, kPanel.y + kPanel.h - 75, 165, 54};
    draw_button(renderer, map_btn, "Edit Map", "edit_map", 0, {120, 180, 255, 255});

    SDL_Rect save_btn{kPanel.x + kPanel.w - 180, kPanel.y + kPanel.h - 75, 150, 54};
    draw_button(renderer, save_btn, "Save", "save", 0, {60, 220, 60, 255});
}

void Editor::draw_edit_map(SDL_Renderer* renderer) {
    draw_frame_chrome(renderer, /*show_back=*/true);

    // ---- global top-strip toolbar: sits in the same 48px row as the
    // back arrow (kBackRect), immediately to its right -- grid-alignment
    // toggle (show_grid_) and the universal eraser toggle (erasing_),
    // both view/tool switches that make sense regardless of
    // edit_map_mode_, which is why they live up here instead of in the
    // mode-specific toolbar below (kPanel.y + 42). ----
    {
        int gtw = 40, gap = 8;
        int gx = kBackRect.w + gap;
        SDL_Rect grid_btn{gx, 4, gtw, gtw};
        draw_button(renderer, grid_btn, "", "toggle_grid", 0,
                   show_grid_ ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255});
        if (atlas_.meta("spr_grid")) atlas_.draw_in_rect(grid_btn, "spr_grid", 0, 4);

        SDL_Rect erase_btn{gx + gtw + gap, 4, gtw, gtw};
        draw_button(renderer, erase_btn, "", "toggle_erase", 0,
                   erasing_ ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255});
        if (atlas_.meta("spr_erase")) atlas_.draw_in_rect(erase_btn, "spr_erase", 0, 4);
    }

    Level* lvl = current_level();
    if (!lvl) return;
    const int kLevelGridSize = lvl->grid_size; // this level's own size, see draw_new_level's size picker

    std::string title = "Edit Map: " + (lvl->name.empty() ? "(untitled)" : lvl->name);
    text_.draw(title, kPanel.x + 12, kPanel.y + 9, {255, 255, 255, 255}, 24);
    // Sits in the title row, left of the generic "Saved." toast's own
    // top-right corner spot (draw()) -- shifted well clear of that column
    // rather than sharing it, since the toolbar row right below (kPanel.y
    // + 42) leaves no room to stack this under the toast instead.
    SDL_Rect save_btn{kMapCanvasRect.x + kMapCanvasRect.w - 220, kPanel.y + 9, 102, 27};
    draw_button(renderer, save_btn, "Save", "save", 0, {60, 220, 60, 255});

    // ---- top toolbar: Map / Terrain / Players / Units (see EditMapMode) ----
    const char* kModeLabels[6] = {"Map", "Terrain", "Players", "Units", "Events", "Objectives"};
    {
        int toolbar_w = kPanel.w - 24, gap = 12;
        int btn_w = (toolbar_w - 5 * gap) / 6;
        int bx = kPanel.x + 12;
        for (int m = 0; m < 6; ++m) {
            SDL_Rect r{bx, kPanel.y + 42, btn_w, 36};
            bool active = (static_cast<int>(edit_map_mode_) == m);
            draw_button(renderer, r, kModeLabels[m], "edit_map_mode", m,
                       active ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255});
            bx += btn_w + gap;
        }
    }

    // ---- left sidebar (Units, Terrain, or Players mode, and only while
    // the global eraser tool isn't active -- see erasing_): scrollable
    // placement/paint/pick list. Terrain mode shows whichever sub-tab
    // (terrain_category_) is chosen on the bottom toolbar; Units mode
    // shows units/buildings for whichever sub-tab (map_tab_) is chosen
    // there instead; Players mode shows every player slot, clickable to
    // choose which one the bottom toolbar's civ/era/resource controls
    // edit (players_selected_). ----
    const int side_x = kPanel.x + 12, side_w = 210;
    int pal_y = kPaletteRect.y;
    // Only Terrain mode's per-category list ever has enough rows to
    // overflow kPaletteRect -- Units/Buildings'/Players' much shorter
    // lists always fit, so they're left unscrolled (scroll=0).
    int scroll = (edit_map_mode_ == EditMapMode::Terrain) ? other_scroll_ : 0;
    if (!erasing_ && edit_map_mode_ != EditMapMode::Map) {
    SDL_RenderSetClipRect(renderer, &kPaletteRect);
    auto draw_palette_row = [&](const std::string& label, bool selected, const std::string& id, int arg,
                                const std::string& sprite_name, const SDL_Color* swatch, int frame = 0) {
        int draw_y = pal_y - scroll;
        pal_y += 31;
        // Fully outside the visible palette viewport -- skip drawing AND
        // registering a hit-rect (otherwise an off-screen row's rect could
        // still swallow a click meant for whatever's underneath it).
        if (draw_y + 30 < kPaletteRect.y || draw_y > kPaletteRect.y + kPaletteRect.h) return;
        SDL_Rect row{side_x, draw_y, side_w, 30};
        SDL_Color border = selected ? SDL_Color{255, 220, 60, 255} : SDL_Color{110, 110, 110, 255};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, &row);
        SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, 255);
        SDL_RenderDrawRect(renderer, &row);
        int text_x = row.x + 6;
        SDL_Rect icon_r{row.x + 3, row.y + 3, 24, 24};
        if (!sprite_name.empty() && atlas_.meta(sprite_name)) {
            atlas_.draw_in_rect(icon_r, sprite_name, frame, 1);
            text_x = icon_r.x + icon_r.w + 5;
        } else if (swatch) {
            SDL_SetRenderDrawColor(renderer, swatch->r, swatch->g, swatch->b, 255);
            SDL_RenderFillRect(renderer, &icon_r);
            text_x = icon_r.x + icon_r.w + 5;
        }
        int size = 16, lw, lh;
        text_regular_.measure(label, size, lw, lh);
        while (lw > row.w - (text_x - row.x) - 6 && size > 10) {
            --size;
            text_regular_.measure(label, size, lw, lh);
        }
        text_regular_.draw(label, text_x, row.y + (row.h - lh) / 2, {255, 255, 255, 255}, size);
        hit_rects_.push_back({row, id, arg});
    };

    if (edit_map_mode_ == EditMapMode::Terrain) {
        // Filtered down to just the active sub-tab's category, so the list
        // actually fits the sidebar instead of forcing a scroll through
        // all ~50 kinds at once.
        for (int i = 0; i < static_cast<int>(std::size(kTerrainKinds)); ++i) {
            if (kTerrainKinds[i].category != terrain_category_) continue;
            draw_palette_row(kTerrainKinds[i].name, active_terrain_ == kTerrainKinds[i].name, "pick_terrain", i,
                             kTerrainKinds[i].sprite, &kTerrainKinds[i].colour, kTerrainKinds[i].frame);
        }
    } else if (edit_map_mode_ == EditMapMode::Players) {
        for (int i = 0; i < static_cast<int>(lvl->players.size()); ++i) {
            std::string civ_name =
                (lvl->players[i].civ >= 0 && lvl->players[i].civ < 9)
                    ? ww::menu::civ_names()[lvl->players[i].civ]
                    : "?";
            std::string label = "P" + std::to_string(i + 1) + ": " + civ_name;
            draw_palette_row(label, players_selected_ == i, "players_pick", i, "spr_flags_mini", nullptr,
                             lvl->players[i].civ);
        }
    } else if (edit_map_mode_ == EditMapMode::Events) {
        // Same "+ Create X" pinned above a list of existing ones you click
        // to select for editing" layout as Objectives just below -- see
        // its comment for why (also copied here: each row gets its own
        // delete "x", since an event is user-authored data with no map
        // tile to erase it via any more, see handle_right_click/
        // erase_area_at's comments).
        SDL_Rect create_btn{side_x, pal_y, side_w, 28};
        draw_button(renderer, create_btn, "+ Create Event", "create_event", 0, {60, 220, 60, 255});
        pal_y += 34;
        for (int i = 0; i < static_cast<int>(lvl->events.size()); ++i) {
            auto& ev = lvl->events[i];
            int draw_y = pal_y - scroll;
            pal_y += 31;
            if (draw_y + 30 < kPaletteRect.y || draw_y > kPaletteRect.y + kPaletteRect.h) continue;
            bool selected = (events_selected_ == i);
            SDL_Rect row{side_x, draw_y, side_w - 28, 30};
            SDL_Color border = selected ? SDL_Color{255, 220, 60, 255} : SDL_Color{110, 110, 110, 255};
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderFillRect(renderer, &row);
            SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, 255);
            SDL_RenderDrawRect(renderer, &row);
            // Short type tag so the list is scannable without having to
            // select each row to see what kind it is.
            const char* tag = ev.type == "gate"        ? "  [Gate]"
                             : ev.type == "dormant"     ? "  [Dormant]"
                             : ev.type == "resources"   ? "  [Res]"
                                                         : "  [Msg]";
            std::string label = (ev.name.empty() ? "(untitled)" : ev.name) + tag;
            int size = 16, lw, lh;
            text_regular_.measure(label, size, lw, lh);
            while (lw > row.w - 12 && size > 10) {
                --size;
                text_regular_.measure(label, size, lw, lh);
            }
            text_regular_.draw(label, row.x + 6, row.y + (row.h - lh) / 2, {255, 255, 255, 255}, size);
            hit_rects_.push_back({row, "select_event", i});

            SDL_Rect del_btn{row.x + row.w + 3, draw_y, 25, 30};
            draw_button(renderer, del_btn, "x", "delete_event", i, {255, 90, 90, 255});
        }
        if (lvl->events.empty()) {
            text_regular_.draw("No events yet.", side_x, pal_y, {160, 160, 160, 255}, 15);
        }
    } else if (edit_map_mode_ == EditMapMode::Objectives) {
        // "+ Create Objective" pinned above a list of existing ones you
        // click to select for editing -- same "create action above a list"
        // layout as EditLevel's own "+ Add Player" (see draw_edit_level),
        // just living in this sidebar instead since Objectives has no
        // dedicated screen of its own. Each row gets its own delete "x"
        // (unlike every other palette row here) since, unlike a unit/
        // terrain palette entry, an objective is user-authored data with
        // no other way to remove it -- there's no map tile to erase, it
        // only exists in this list.
        SDL_Rect create_btn{side_x, pal_y, side_w, 28};
        draw_button(renderer, create_btn, "+ Create Objective", "create_objective", 0, {60, 220, 60, 255});
        pal_y += 34;
        for (int i = 0; i < static_cast<int>(lvl->objectives.size()); ++i) {
            auto& obj = lvl->objectives[i];
            int draw_y = pal_y - scroll;
            pal_y += 31;
            if (draw_y + 30 < kPaletteRect.y || draw_y > kPaletteRect.y + kPaletteRect.h) continue;
            bool selected = (objectives_selected_ == i);
            SDL_Rect row{side_x, draw_y, side_w - 28, 30};
            SDL_Color border = selected ? SDL_Color{255, 220, 60, 255} : SDL_Color{110, 110, 110, 255};
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderFillRect(renderer, &row);
            SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, 255);
            SDL_RenderDrawRect(renderer, &row);
            // Short type tag (+ a hidden marker, see Objective::hidden) so
            // the list is scannable without having to select each row to
            // see what kind it is.
            const char* type_tag =
                obj.type == "move_to_area" ? "  [Move]" : obj.type == "protect_unit" ? "  [Protect]" : "  [Kill]";
            std::string label = (obj.name.empty() ? "(untitled)" : obj.name) + type_tag;
            if (obj.hidden) label += "  (Hidden)";
            int size = 16, lw, lh;
            text_regular_.measure(label, size, lw, lh);
            while (lw > row.w - 12 && size > 10) {
                --size;
                text_regular_.measure(label, size, lw, lh);
            }
            text_regular_.draw(label, row.x + 6, row.y + (row.h - lh) / 2, {255, 255, 255, 255}, size);
            hit_rects_.push_back({row, "select_objective", i});

            SDL_Rect del_btn{row.x + row.w + 3, draw_y, 25, 30};
            draw_button(renderer, del_btn, "x", "delete_objective", i, {255, 90, 90, 255});
        }
        if (lvl->objectives.empty()) {
            text_regular_.draw("No objectives yet.", side_x, pal_y, {160, 160, 160, 255}, 15);
        }

        // ---- Briefing image: pinned to the bottom of the objectives sidebar
        // (below the objectives list). Upload copies a PNG into the campaign's
        // assets; the thumbnail previews it. A solid backing masks the
        // scrolling list behind it. ----
        int img_top = kPaletteRect.y + kPaletteRect.h - 196;
        SDL_Rect img_bg{side_x - 4, img_top - 6, side_w + 8, 202};
        SDL_SetRenderDrawColor(renderer, 18, 18, 26, 255);
        SDL_RenderFillRect(renderer, &img_bg);
        SDL_SetRenderDrawColor(renderer, 80, 80, 90, 255);
        SDL_RenderDrawLine(renderer, img_bg.x, img_bg.y, img_bg.x + img_bg.w, img_bg.y);
        text_regular_.draw("Briefing image:", side_x, img_top, {200, 200, 200, 255}, 16);
        SDL_Rect up_btn{side_x, img_top + 22, side_w, 28};
        draw_button(renderer, up_btn, lvl->briefing_image.empty() ? "Upload Image" : "Replace Image",
                   "upload_briefing_image", 0, {120, 180, 255, 255});
        if (!lvl->briefing_image.empty()) {
            // (Re)load the texture whenever the referenced file changes.
            if (briefing_tex_key_ != lvl->briefing_image) {
                if (briefing_tex_) SDL_DestroyTexture(briefing_tex_);
                std::string full = asset_dir_ + "/" + lvl->briefing_image;
                briefing_tex_ = IMG_LoadTexture(renderer, full.c_str());
                briefing_tex_key_ = lvl->briefing_image;
            }
            SDL_Rect preview{side_x, img_top + 56, side_w, 96};
            SDL_SetRenderDrawColor(renderer, 10, 10, 14, 255);
            SDL_RenderFillRect(renderer, &preview);
            if (briefing_tex_) {
                int tw = 0, th = 0;
                SDL_QueryTexture(briefing_tex_, nullptr, nullptr, &tw, &th);
                SDL_Rect dst = preview;
                if (tw > 0 && th > 0) {
                    double s = std::min(static_cast<double>(preview.w) / tw,
                                        static_cast<double>(preview.h) / th);
                    dst.w = static_cast<int>(tw * s);
                    dst.h = static_cast<int>(th * s);
                    dst.x = preview.x + (preview.w - dst.w) / 2;
                    dst.y = preview.y + (preview.h - dst.h) / 2;
                }
                SDL_RenderCopy(renderer, briefing_tex_, nullptr, &dst);
            } else {
                text_regular_.draw("(image not found)", side_x + 6, img_top + 90, {210, 120, 120, 255}, 14);
            }
            SDL_Rect rm_btn{side_x, img_top + 156, side_w, 26};
            draw_button(renderer, rm_btn, "Remove Image", "remove_briefing_image", 0, {255, 90, 90, 255});
        }
    } else { // Units mode
        int active_team_civ = (active_team_ >= 0 && active_team_ < static_cast<int>(lvl->players.size()))
                                 ? lvl->players[active_team_].civ
                                 : -1;
        if (active_team_civ < 0) {
            text_regular_.draw("Pick a team below first.", side_x, pal_y, {200, 200, 140, 255}, 16);
        } else if (map_tab_ == MapTab::Units) {
            // Unit/building sheets' 8 frames ARE the 8 player colours (see
            // SpriteAtlas's header comment), so drawing each palette icon
            // at active_team_'s frame recolours it to match whichever
            // player row is selected on the bottom toolbar, instead of
            // always showing frame 0's colour regardless of who you're
            // placing for.
            int team_frame = std::clamp(active_team_, 0, 7);
            auto units = palette_units(active_team_civ);
            if (!units.empty() && active_unit_.empty()) active_unit_ = units[0];
            for (int i = 0; i < static_cast<int>(units.size()); ++i) {
                draw_palette_row(units[i], active_unit_ == units[i], "pick_unit", i, item_icon(units[i]), nullptr,
                                 team_frame);
            }
        } else { // Buildings
            int team_frame = std::clamp(active_team_, 0, 7);
            auto buildings = palette_buildings(active_team_civ);
            if (!buildings.empty() && active_building_.empty()) active_building_ = buildings[0];
            for (int i = 0; i < static_cast<int>(buildings.size()); ++i) {
                draw_palette_row(buildings[i], active_building_ == buildings[i], "pick_building", i,
                                 item_icon(buildings[i]), nullptr, team_frame);
            }
        }
    }
    SDL_RenderSetClipRect(renderer, nullptr);

    // Scrollbar thumb for Terrain mode, only drawn when its content
    // actually overflows kPaletteRect -- a plain proportional track/thumb,
    // just enough to signal "there's more below" (see kPaletteRect and the
    // mouse-wheel handling in handle_event).
    if (edit_map_mode_ == EditMapMode::Terrain) {
        int content_h = terrain_category_row_count(terrain_category_) * 31;
        if (content_h > kPaletteRect.h) {
            SDL_Rect track{kPaletteRect.x + kPaletteRect.w - 5, kPaletteRect.y, 5, kPaletteRect.h};
            SDL_SetRenderDrawColor(renderer, 60, 60, 60, 255);
            SDL_RenderFillRect(renderer, &track);
            int thumb_h = std::max(10, static_cast<int>(static_cast<double>(kPaletteRect.h) / content_h *
                                                        kPaletteRect.h));
            int thumb_y = track.y + static_cast<int>(static_cast<double>(other_scroll_) / content_h *
                                                     kPaletteRect.h);
            SDL_Rect thumb{track.x, thumb_y, track.w, thumb_h};
            SDL_SetRenderDrawColor(renderer, 180, 180, 180, 255);
            SDL_RenderFillRect(renderer, &thumb);
        }
    }
    } // !erasing_ && (edit_map_mode_ == Units || Terrain) (left sidebar)

    // ---- battlefield canvas: real terrain/unit/building sprites through
    // the same Camera transform GameClient uses, so this previews like an
    // actual in-game map instead of an abstract grid. ----
    draw_panel(renderer, kMapCanvasRect);
    // Minimap's hit-rect (its rect now lives in the bottom toolbar, drawn
    // later in this function -- registering it here alongside canvas_click
    // is just bookkeeping convenience, not a priority requirement any
    // more, since the two rects no longer overlap).
    hit_rects_.push_back({kMinimapRect, "minimap_click", 0});
    hit_rects_.push_back({kMapCanvasRect, "canvas_click", 0});

    std::unordered_map<int, const ww::campaign::TerrainFeature*> terrain_at;
    for (auto& t : lvl->terrain) terrain_at[t.tx * kLevelGridSize + t.ty] = &t;

    SDL_RenderSetClipRect(renderer, &kMapCanvasRect);

    // Takes GRID pixel coordinates (tx*kEditorTile etc, same space the
    // data model and every caller below already works in) and adds
    // map_camera_'s kMapMargin padding before handing off to the camera --
    // callers never need to think about the padding themselves.
    auto to_screen = [&](double gx, double gy, int& sx, int& sy) {
        map_camera_.world_to_screen(gx + kMapMargin, gy + kMapMargin, sx, sy);
        sx += kMapCanvasRect.x;
        sy += kMapCanvasRect.y;
    };

    // Same visibility-culled tile loop as GameClient (client/src/
    // game_client.cpp) -- only iterates tiles actually on-screen at the
    // current zoom/pan, not all 1024 every frame. vr is in the camera's
    // padded space, so kMapMargin comes back out before dividing into
    // grid tiles (see to_screen's comment).
    auto vr = map_camera_.visible_rect();
    int x0 = std::clamp(static_cast<int>((vr.x - kMapMargin) / kEditorTile), 0, kLevelGridSize);
    int x1 = std::clamp(static_cast<int>((vr.x - kMapMargin + vr.w) / kEditorTile) + 1, 0, kLevelGridSize);
    int y0 = std::clamp(static_cast<int>((vr.y - kMapMargin) / kEditorTile), 0, kLevelGridSize);
    int y1 = std::clamp(static_cast<int>((vr.y - kMapMargin + vr.h) / kEditorTile) + 1, 0, kLevelGridSize);

    for (int tx = x0; tx < x1; ++tx) {
        for (int ty = y0; ty < y1; ++ty) {
            // Sized from this tile's own corner to the NEXT tile's corner
            // (not TILE*zoom rounded once) so adjacent tiles' edges always
            // meet exactly -- same reasoning as GameClient's terrain draw.
            int sx, sy, sx1, sy1;
            to_screen(tx * kEditorTile, ty * kEditorTile, sx, sy);
            to_screen((tx + 1) * kEditorTile, (ty + 1) * kEditorTile, sx1, sy1);
            SDL_Rect dst{sx, sy, std::max(1, sx1 - sx), std::max(1, sy1 - sy)};

            auto it = terrain_at.find(tx * kLevelGridSize + ty);
            // Absent tile / empty base = grass; otherwise look up whichever
            // base kind is painted there (water, or one of the dirt/sand/
            // gravel/pavement ground-texture variants -- see kTerrainKinds)
            // rather than a hardcoded water-vs-grass binary choice.
            const TerrainKind* base_kind =
                (it != terrain_at.end()) ? find_terrain_kind(it->second->base) : nullptr;
            const char* base_sprite = base_kind ? base_kind->sprite : kGrassSprite;
            if (atlas_.meta(base_sprite)) {
                atlas_.draw_stretched(base_sprite, dst);
            } else {
                SDL_Color c = base_kind ? base_kind->colour : kGrassColour;
                SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
                SDL_RenderFillRect(renderer, &dst);
            }
        }
    }

    // Resource overlays (tree/berry/oil/iron/deer/fish/palm): drawn as
    // real pivoted sprites at each tile's centre, same as GameClient draws
    // Resource/Deer entities, not stretched to fill the tile like the base
    // terrain above -- these sit ON the ground (or in the water, for
    // fish), they aren't the ground itself.
    for (auto& t : lvl->terrain) {
        if (t.resource.empty()) continue;
        const TerrainKind* k = find_terrain_kind(t.resource);
        if (!k) continue;
        int sx, sy;
        to_screen((t.tx + 0.5) * kEditorTile, (t.ty + 0.5) * kEditorTile, sx, sy);
        if (t.resource == "fish") {
            // Fish sits camouflaged in blue water -- a green marker
            // rectangle behind it (matching a shoal/fishing-zone marker)
            // keeps it easy to spot on the grid at a glance.
            int half = static_cast<int>(kEditorTile * 0.4 * map_camera_.zoom);
            SDL_Rect marker{sx - half, sy - half, half * 2, half * 2};
            SDL_SetRenderDrawColor(renderer, 60, 200, 90, 255);
            SDL_RenderDrawRect(renderer, &marker);
        }
        if (atlas_.meta(k->sprite)) {
            atlas_.draw(k->sprite, sx, sy, k->frame, map_camera_.zoom);
        } else {
            SDL_Rect r{sx - 6, sy - 6, 12, 12};
            SDL_SetRenderDrawColor(renderer, k->colour.r, k->colour.g, k->colour.b, 255);
            SDL_RenderFillRect(renderer, &r);
        }
    }

    // Buildings, then units on top -- real battlefield sprite
    // (sprite_index, not the UI icon_sprite), frame = the owning player's
    // slot index (0-7): SpriteAtlas's 8 frames per unit/building sprite
    // ARE per-player-colour skins (see render/include/render/sprite_atlas.h),
    // so this reuses the same art the main game already ships instead of a
    // separate colour swatch.
    //
    // Building centre MUST be (tx*TILE + w/2, ty*TILE + h/2) -- i.e. (tx,ty)
    // is the footprint's TOP-LEFT tile, matching footprint_clear/
    // place_entity_at's own assumption -- NOT (tx+0.5)*TILE like a
    // single-tile unit. That was the "encroaches on 9 cells instead of 4"
    // bug: centering a 64px-wide (2-tile) sprite on a SINGLE tile's centre
    // makes it straddle 3 tile columns, not 2; a 96px (3-tile, odd) sprite
    // happened to still look plausible centred that way, which is why only
    // the even-tile-count buildings (house/barracks/etc, not base/fortress)
    // visibly looked wrong.
    auto draw_entity = [&](const ww::campaign::PlacedEntity& e, bool is_building) {
        double cx, cy;
        if (is_building) {
            auto [bw, bh] = ww::gamedata::building_wh(e.type);
            cx = e.tx * kEditorTile + bw / 2.0;
            cy = e.ty * kEditorTile + bh / 2.0;
        } else {
            cx = (e.tx + 0.5) * kEditorTile;
            cy = (e.ty + 0.5) * kEditorTile;
        }
        int sx, sy;
        to_screen(cx, cy, sx, sy);
        int frame = std::clamp(e.player_index, 0, 7);
        bool owner_valid = e.player_index >= 0 && e.player_index < static_cast<int>(lvl->players.size());
        int owner_civ = owner_valid ? lvl->players[e.player_index].civ : 0;
        int owner_era = owner_valid ? lvl->players[e.player_index].era : 0;
        std::string sprite = world_sprite(e.type, owner_civ, owner_era);
        if (atlas_.meta(sprite)) {
            atlas_.draw(sprite, sx, sy, frame, map_camera_.zoom);
        } else {
            SDL_Color col = kTeamColours[std::clamp(
                (e.player_index >= 0 && e.player_index < static_cast<int>(lvl->players.size()))
                    ? lvl->players[e.player_index].team - 1
                    : 0,
                0, 3)];
            int half = is_building ? 8 : 5;
            SDL_Rect r{sx - half, sy - half, half * 2, half * 2};
            SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
            SDL_RenderFillRect(renderer, &r);
        }
    };
    for (auto& b : lvl->buildings) draw_entity(b, true);
    for (auto& u : lvl->units) draw_entity(u, false);

    // ---- message events: always visible regardless of mode (same as
    // terrain/units/buildings above), so they're still visible while
    // placing units/terrain elsewhere -- but only once actually placed
    // (tx >= 0, see MapEvent's comment; a freshly-created message starts
    // unplaced). The selected one (events_selected_, cleared whenever
    // edit_map_mode_ switches away from Events, see "edit_map_mode"'s
    // click handler) gets a green outline. Gate/dormant events have no
    // point sprite of their own -- only their area shows, and only while
    // selected, see the area-overlay block below. ----
    for (int i = 0; i < static_cast<int>(lvl->events.size()); ++i) {
        auto& ev = lvl->events[i];
        // message and resources both drop a marker tile (resources = a yellow
        // pickup the player walks onto). Others show only their area, and only
        // while selected (the area-overlay block below).
        bool is_marker = (ev.type == "message" || ev.type == "resources");
        if (!is_marker || ev.tx < 0) continue;
        int sx, sy;
        to_screen((ev.tx + 0.5) * kEditorTile, (ev.ty + 0.5) * kEditorTile, sx, sy);
        if (atlas_.meta("spr_message")) {
            if (ev.type == "resources")
                atlas_.draw("spr_message", sx, sy, 0, map_camera_.zoom, 0.0, false, 255, {255, 230, 60, 255});
            else
                atlas_.draw("spr_message", sx, sy, 0, map_camera_.zoom);
        }
        if (events_selected_ == i) {
            int sx0, sy0, sx1, sy1;
            to_screen(ev.tx * kEditorTile, ev.ty * kEditorTile, sx0, sy0);
            to_screen((ev.tx + 1) * kEditorTile, (ev.ty + 1) * kEditorTile, sx1, sy1);
            SDL_Rect outline{sx0 - 1, sy0 - 1, std::max(1, sx1 - sx0) + 2, std::max(1, sy1 - sy0) + 2};
            SDL_SetRenderDrawColor(renderer, 60, 230, 60, 255);
            SDL_RenderDrawRect(renderer, &outline);
        }
    }

    // ---- selected event's area (gate or dormant): same "only the
    // selected one is ever shown" rule as Objective's area below, and the
    // same thick-outline treatment -- amber for a gate (every Blocks-
    // category terrain tile inside it acts as a locked gate), red for
    // dormant (every unit inside it stays put), per MapEvent's comment.
    // A gate with no objective assigned gets an extra dashed-look inner
    // warning tint (it can never open, see MapEvent::unlock_objective_id)
    // -- reusing the red channel there too would be confusing next to a
    // dormant area, so it's drawn as a darker overlay instead of a
    // different hue. ----
    if (edit_map_mode_ == EditMapMode::Events && events_selected_ >= 0 &&
        events_selected_ < static_cast<int>(lvl->events.size())) {
        const ww::campaign::MapEvent& sel_ev = lvl->events[events_selected_];
        // Colours per box role: dormant "units" = red, gate = amber, spawn =
        // green, dormant trigger (area2_*) = cyan, sight boxes = light green.
        const SDL_Color kUnitsFill{230, 50, 50, 80}, kUnitsBorder{255, 90, 90, 220};
        const SDL_Color kGateFill{230, 160, 40, 80}, kGateBorder{255, 190, 80, 220};
        const SDL_Color kSpawnFill{60, 200, 90, 80}, kSpawnBorder{90, 240, 120, 220};
        const SDL_Color kTripFill{60, 200, 230, 80}, kTripBorder{90, 230, 255, 220};
        const SDL_Color kSightFill{80, 220, 120, 55}, kSightBorder{120, 240, 150, 200};
        auto draw_box = [&](int atx, int aty, int atw, int ath, SDL_Color fill, SDL_Color border) {
            if (atw <= 0 || ath <= 0) return;
            int sx0, sy0, sx1, sy1;
            to_screen(atx * kEditorTile, aty * kEditorTile, sx0, sy0);
            to_screen((atx + atw) * kEditorTile, (aty + ath) * kEditorTile, sx1, sy1);
            SDL_Rect area{sx0, sy0, std::max(1, sx1 - sx0), std::max(1, sy1 - sy0)};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, fill.a);
            SDL_RenderFillRect(renderer, &area);
            SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
            for (int i = 0; i < 4; ++i) {
                SDL_Rect ring{area.x + i, area.y + i, std::max(1, area.w - 2 * i), std::max(1, area.h - 2 * i)};
                SDL_RenderDrawRect(renderer, &ring);
            }
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        };
        if (sel_ev.type == "gate" || sel_ev.type == "dormant" || sel_ev.type == "spawn") {
            SDL_Color mainFill = sel_ev.type == "gate"    ? kGateFill
                                 : sel_ev.type == "spawn" ? kSpawnFill
                                                          : kUnitsFill;
            SDL_Color mainBorder = sel_ev.type == "gate"    ? kGateBorder
                                   : sel_ev.type == "spawn" ? kSpawnBorder
                                                            : kUnitsBorder;
            // Live drag rectangle -> whichever box the current arm targets.
            if (area_dragging_) {
                int dtx = std::min(area_drag_tx0_, area_drag_tx1_);
                int dty = std::min(area_drag_ty0_, area_drag_ty1_);
                int dtw = std::abs(area_drag_tx1_ - area_drag_tx0_) + 1;
                int dth = std::abs(area_drag_ty1_ - area_drag_ty0_) + 1;
                if (area_target_ == AreaTarget::TriggerArea)
                    draw_box(dtx, dty, dtw, dth, kTripFill, kTripBorder);
                else if (area_target_ == AreaTarget::SightArea)
                    draw_box(dtx, dty, dtw, dth, kSightFill, kSightBorder);
                else
                    draw_box(dtx, dty, dtw, dth, mainFill, mainBorder);
            }
            // Stored boxes: area_* (units/gate/spawn), plus dormant's trigger + sight boxes.
            draw_box(sel_ev.area_tx, sel_ev.area_ty, sel_ev.area_tw, sel_ev.area_th, mainFill, mainBorder);
            if (sel_ev.type == "dormant") {
                draw_box(sel_ev.area2_tx, sel_ev.area2_ty, sel_ev.area2_tw, sel_ev.area2_th, kTripFill,
                         kTripBorder);
                for (auto& r : sel_ev.los_areas) draw_box(r.tx, r.ty, r.tw, r.th, kSightFill, kSightBorder);
            }
            // A gate with no objective can never open -- darken it, as before.
            if (sel_ev.type == "gate" && sel_ev.unlock_objective_id.empty() && !area_dragging_ &&
                sel_ev.area_tw > 0 && sel_ev.area_th > 0) {
                int sx0, sy0, sx1, sy1;
                to_screen(sel_ev.area_tx * kEditorTile, sel_ev.area_ty * kEditorTile, sx0, sy0);
                to_screen((sel_ev.area_tx + sel_ev.area_tw) * kEditorTile,
                          (sel_ev.area_ty + sel_ev.area_th) * kEditorTile, sx1, sy1);
                SDL_Rect area{sx0, sy0, std::max(1, sx1 - sx0), std::max(1, sy1 - sy0)};
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 90);
                SDL_RenderFillRect(renderer, &area);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            }
        }
    }

    // ---- selected objective's area: a partially-transparent blue
    // rectangle (or, mid-drag, the rectangle currently being drawn) --
    // shown for the ONE objective currently selected for editing
    // (objectives_selected_) and NEVER for any other, even if it already
    // has an area of its own -- that's the whole point of this tab: an
    // objective's area is only ever on-screen while you're actually
    // looking at that objective (see objectives_selected_'s comment).
    // Same blue regardless of objective type -- the sidebar's [Kill]/
    // [Move]/[Protect] tag and the bottom toolbar's Type buttons already
    // say which kind it is, so the area itself doesn't need to re-encode
    // that. ----
    if (edit_map_mode_ == EditMapMode::Objectives && objectives_selected_ >= 0 &&
        objectives_selected_ < static_cast<int>(lvl->objectives.size())) {
        const ww::campaign::Objective& sel_obj = lvl->objectives[objectives_selected_];
        int atx = 0, aty = 0, atw = 0, ath = 0;
        bool have_rect = false;
        if (area_dragging_) {
            atx = std::min(area_drag_tx0_, area_drag_tx1_);
            aty = std::min(area_drag_ty0_, area_drag_ty1_);
            atw = std::abs(area_drag_tx1_ - area_drag_tx0_) + 1;
            ath = std::abs(area_drag_ty1_ - area_drag_ty0_) + 1;
            have_rect = true;
        } else if (sel_obj.area_tw > 0 && sel_obj.area_th > 0) {
            atx = sel_obj.area_tx;
            aty = sel_obj.area_ty;
            atw = sel_obj.area_tw;
            ath = sel_obj.area_th;
            have_rect = true;
        }
        if (have_rect) {
            int sx0, sy0, sx1, sy1;
            to_screen(atx * kEditorTile, aty * kEditorTile, sx0, sy0);
            to_screen((atx + atw) * kEditorTile, (aty + ath) * kEditorTile, sx1, sy1);
            SDL_Rect area{sx0, sy0, std::max(1, sx1 - sx0), std::max(1, sy1 - sy0)};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 60, 120, 255, 80);
            SDL_RenderFillRect(renderer, &area);
            // Thick, dense outline -- SDL_RenderDrawRect is only ever 1px,
            // so this nests several concentric rects inward instead of one
            // thin line, same idea as a stroke-width in a vector drawing
            // tool.
            SDL_SetRenderDrawColor(renderer, 110, 180, 255, 255);
            constexpr int kOutlineThickness = 4;
            for (int i = 0; i < kOutlineThickness; ++i) {
                SDL_Rect ring{area.x + i, area.y + i, std::max(1, area.w - 2 * i), std::max(1, area.h - 2 * i)};
                SDL_RenderDrawRect(renderer, &ring);
            }
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }
    }

    // ---- Lasso move tool overlay: the current selection box (yellow), plus
    // a green ghost box at the drop offset while it's being dragged. ----
    if (edit_map_mode_ == EditMapMode::Units && lasso_tool_ && (lasso_has_sel_ || lasso_marquee_)) {
        int x0 = std::min(lasso_x0_, lasso_x1_), x1 = std::max(lasso_x0_, lasso_x1_);
        int y0 = std::min(lasso_y0_, lasso_y1_), y1 = std::max(lasso_y0_, lasso_y1_);
        int sx0, sy0, sx1, sy1;
        to_screen(x0 * kEditorTile, y0 * kEditorTile, sx0, sy0);
        to_screen((x1 + 1) * kEditorTile, (y1 + 1) * kEditorTile, sx1, sy1);
        SDL_Rect box{sx0, sy0, std::max(1, sx1 - sx0), std::max(1, sy1 - sy0)};
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 255, 220, 60, 55);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 255, 220, 60, 230);
        for (int i = 0; i < 2; ++i) {
            SDL_Rect ring{box.x + i, box.y + i, std::max(1, box.w - 2 * i), std::max(1, box.h - 2 * i)};
            SDL_RenderDrawRect(renderer, &ring);
        }
        if (lasso_moving_ && (lasso_move_dx_ != 0 || lasso_move_dy_ != 0)) {
            int gx0, gy0, gx1, gy1;
            to_screen((x0 + lasso_move_dx_) * kEditorTile, (y0 + lasso_move_dy_) * kEditorTile, gx0, gy0);
            to_screen((x1 + 1 + lasso_move_dx_) * kEditorTile, (y1 + 1 + lasso_move_dy_) * kEditorTile, gx1,
                      gy1);
            SDL_Rect ghost{gx0, gy0, std::max(1, gx1 - gx0), std::max(1, gy1 - gy0)};
            SDL_SetRenderDrawColor(renderer, 60, 230, 120, 60);
            SDL_RenderFillRect(renderer, &ghost);
            SDL_SetRenderDrawColor(renderer, 90, 255, 140, 230);
            for (int i = 0; i < 2; ++i) {
                SDL_Rect ring{ghost.x + i, ghost.y + i, std::max(1, ghost.w - 2 * i),
                              std::max(1, ghost.h - 2 * i)};
                SDL_RenderDrawRect(renderer, &ring);
            }
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }

    // Hover preview -- exactly where the next click will land, given the
    // current zoom/pan (helpful once zoomed in enough that tiles are much
    // bigger than a mouse cursor). Suppressed in lasso mode (no placement).
    SDL_Point mp{mouse_pos_.x, mouse_pos_.y};
    int htx, hty;
    bool hover_relevant = erasing_ || (edit_map_mode_ == EditMapMode::Units && !lasso_tool_) ||
                         edit_map_mode_ == EditMapMode::Terrain || edit_map_mode_ == EditMapMode::Events;
    if (hover_relevant && canvas_tile_at(mouse_pos_.x, mouse_pos_.y, htx, hty)) {
        if (erasing_) {
            // Outlines every tile the brush would actually erase, so its
            // real extent is visible before you click, not just the one
            // tile under the cursor.
            int half = brush_size_ / 2;
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 255, 60, 60, 110);
            for (int dx = -half; dx <= half; ++dx) {
                for (int dy = -half; dy <= half; ++dy) {
                    int bx = htx + dx, by = hty + dy;
                    if (bx < 0 || by < 0 || bx >= kLevelGridSize || by >= kLevelGridSize) continue;
                    int sx, sy, sx1, sy1;
                    to_screen(bx * kEditorTile, by * kEditorTile, sx, sy);
                    to_screen((bx + 1) * kEditorTile, (by + 1) * kEditorTile, sx1, sy1);
                    SDL_Rect hl{sx, sy, std::max(1, sx1 - sx), std::max(1, sy1 - sy)};
                    SDL_RenderFillRect(renderer, &hl);
                }
            }
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        } else if (edit_map_mode_ == EditMapMode::Terrain) {
            // Faded preview of the actual selected terrain kind's sprite,
            // at EVERY tile the brush would actually touch (brush forced
            // to a single tile for deer/fish -- see
            // terrain_kind_ignores_brush) -- not just a plain highlight
            // box, so what's about to be painted is as visible up front as
            // the Units/Buildings ghost preview below. Tinted green where
            // paint_terrain_at would actually do something, red where it'd
            // be a no-op (fish needs existing water; a land resource can't
            // overwrite water; a decoration/resource can't land on a tile
            // a unit/building already occupies, see tile_has_entity --
            // ground/base kinds are exempt from that last rule, same as in
            // paint_terrain_at) -- same validity rules as that function.
            const TerrainKind* k = find_terrain_kind(active_terrain_);
            if (k) {
                int half = terrain_kind_ignores_brush(active_terrain_) ? 0 : brush_size_ / 2;
                for (int dx = -half; dx <= half; ++dx) {
                    for (int dy = -half; dy <= half; ++dy) {
                        int bx = htx + dx, by = hty + dy;
                        if (bx < 0 || by < 0 || bx >= kLevelGridSize || by >= kLevelGridSize) continue;
                        auto it = terrain_at.find(bx * kLevelGridSize + by);
                        bool had_water = (it != terrain_at.end() && it->second->base == "water");
                        bool blocked_by_entity = !k->is_base && tile_has_entity(*lvl, bx, by);
                        bool valid = k->is_base
                                        ? true
                                        : (active_terrain_ == "fish" ? had_water : !had_water) && !blocked_by_entity;
                        int sx, sy;
                        to_screen((bx + 0.5) * kEditorTile, (by + 0.5) * kEditorTile, sx, sy);
                        SDL_Color tint = valid ? SDL_Color{140, 255, 140, 255} : SDL_Color{255, 120, 120, 255};
                        if (atlas_.meta(k->sprite)) {
                            atlas_.draw(k->sprite, sx, sy, k->frame, map_camera_.zoom, 0.0, false, 160, tint);
                        } else {
                            SDL_Rect r{sx - 6, sy - 6, 12, 12};
                            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                            SDL_SetRenderDrawColor(renderer, tint.r, tint.g, tint.b, 160);
                            SDL_RenderFillRect(renderer, &r);
                            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                        }
                        if (blocked_by_entity) {
                            // A colour-mod tint alone barely reads as "red"
                            // against a mostly-green/brown decoration
                            // sprite (it can only ever dim channels, never
                            // add red) -- a solid translucent red tile
                            // overlay makes "occupied, can't place here"
                            // unambiguous, same idea as the eraser's own
                            // brush-outline highlight above.
                            int sx0, sy0, sx1, sy1;
                            to_screen(bx * kEditorTile, by * kEditorTile, sx0, sy0);
                            to_screen((bx + 1) * kEditorTile, (by + 1) * kEditorTile, sx1, sy1);
                            SDL_Rect hl{sx0, sy0, std::max(1, sx1 - sx0), std::max(1, sy1 - sy0)};
                            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                            SDL_SetRenderDrawColor(renderer, 255, 40, 40, 100);
                            SDL_RenderFillRect(renderer, &hl);
                            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                        }
                    }
                }
            }
        } else if (edit_map_mode_ == EditMapMode::Events) {
            // Hovering an existing (placed) message shows a read-only text
            // preview near the cursor (selecting it, via a click, is still
            // what lets you edit it). Otherwise, if a "message"-type event
            // is currently selected, hovering empty ground shows a faint
            // ghost of it -- clicking here is what places/moves it (see
            // reposition_selected_message_at). Gate/dormant events have no
            // per-tile hover of their own: their area IS the preview, see
            // the always-shown-while-selected area overlay above.
            const ww::campaign::MapEvent* hovered_msg = nullptr;
            for (auto& e : lvl->events) {
                if (e.type == "message" && e.tx == htx && e.ty == hty) { hovered_msg = &e; break; }
            }
            if (hovered_msg) {
                std::string preview = hovered_msg->text.empty() ? "(empty message)" : hovered_msg->text;
                int tw, th;
                text_regular_.measure(preview, 16, tw, th);
                SDL_Rect bg{mp.x + 18, mp.y + 18, tw + 14, th + 10};
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 220);
                SDL_RenderFillRect(renderer, &bg);
                SDL_SetRenderDrawColor(renderer, 255, 220, 60, 255);
                SDL_RenderDrawRect(renderer, &bg);
                text_regular_.draw(preview, bg.x + 7, bg.y + 5, {255, 255, 255, 255}, 16);
            } else if (events_selected_ >= 0 && events_selected_ < static_cast<int>(lvl->events.size()) &&
                      lvl->events[events_selected_].type == "message") {
                int sx, sy;
                to_screen((htx + 0.5) * kEditorTile, (hty + 0.5) * kEditorTile, sx, sy);
                if (atlas_.meta("spr_message")) {
                    atlas_.draw("spr_message", sx, sy, 0, map_camera_.zoom, 0.0, false, 160,
                               {140, 255, 140, 255});
                }
            }
        } else {
            // Faded preview of the actual selected unit/building sprite at
            // the tile it would be placed on -- tinted green if that spot
            // is actually placeable, red if footprint_clear would reject
            // it (occupied by terrain/another unit/another building, or
            // out of bounds), so it's obvious before you click.
            const std::string& type = (map_tab_ == MapTab::Units) ? active_unit_ : active_building_;
            if (!type.empty() && active_team_ >= 0 && active_team_ < static_cast<int>(lvl->players.size())) {
                bool is_building = (map_tab_ == MapTab::Buildings);
                int wt = 1, ht = 1;
                double cx, cy;
                if (is_building) {
                    auto [bw, bh] = ww::gamedata::building_wh(type);
                    wt = bw / kEditorTile; ht = bh / kEditorTile;
                    cx = htx * kEditorTile + bw / 2.0;
                    cy = hty * kEditorTile + bh / 2.0;
                } else {
                    cx = (htx + 0.5) * kEditorTile;
                    cy = (hty + 0.5) * kEditorTile;
                }
                bool ship = (map_tab_ == MapTab::Units) && is_ship(type);
                bool valid = footprint_clear(*lvl, htx, hty, wt, ht, ship);
                int sx, sy;
                to_screen(cx, cy, sx, sy);
                std::string sprite =
                    world_sprite(type, lvl->players[active_team_].civ, lvl->players[active_team_].era);
                SDL_Color tint = valid ? SDL_Color{140, 255, 140, 255} : SDL_Color{255, 120, 120, 255};
                if (atlas_.meta(sprite)) {
                    atlas_.draw(sprite, sx, sy, std::clamp(active_team_, 0, 7), map_camera_.zoom, 0.0, false,
                               160, tint);
                } else {
                    int half = is_building ? 8 : 5;
                    SDL_Rect r{sx - half, sy - half, half * 2, half * 2};
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(renderer, tint.r, tint.g, tint.b, 160);
                    SDL_RenderFillRect(renderer, &r);
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                }
            }
        }
    }

    // Grid overlay (toggled via the top toolbar's grid button, see
    // show_grid_): one line per tile boundary within the visible range
    // already computed above (x0..x1, y0..y1), drawn on top of
    // everything else in the canvas so it's never obscured by terrain --
    // this is purely an alignment aid, not part of the level data.
    if (show_grid_) {
        SDL_SetRenderDrawColor(renderer, 64, 64, 64, 255);
        for (int tx = x0; tx <= x1; ++tx) {
            int sx, sy, sx2, sy2;
            to_screen(tx * kEditorTile, y0 * kEditorTile, sx, sy);
            to_screen(tx * kEditorTile, y1 * kEditorTile, sx2, sy2);
            SDL_RenderDrawLine(renderer, sx, sy, sx2, sy2);
        }
        for (int ty = y0; ty <= y1; ++ty) {
            int sx, sy, sx2, sy2;
            to_screen(x0 * kEditorTile, ty * kEditorTile, sx, sy);
            to_screen(x1 * kEditorTile, ty * kEditorTile, sx2, sy2);
            SDL_RenderDrawLine(renderer, sx, sy, sx2, sy2);
        }
    }

    SDL_RenderSetClipRect(renderer, nullptr);
    SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
    SDL_RenderDrawRect(renderer, &kMapCanvasRect);

    text_regular_.draw("Scroll wheel = zoom, middle-drag = pan, right-click = erase.",
                       kMapCanvasRect.x + 6, kMapCanvasRect.y + 6, {200, 200, 200, 200}, 15);

    // ---- bottom toolbar: content depends on the top mode-toolbar's
    // active EditMapMode (see draw_edit_map's header comment), laid out
    // within the narrower kBottomToolbarContentRect so nothing overlaps
    // the minimap docked in this toolbar's own far-right corner (drawn
    // after, below). ----
    draw_panel(renderer, kBottomToolbarRect);
    const SDL_Rect& bc = kBottomToolbarContentRect;
    // Brush size, 1-5: a click-or-drag slider (see kBrushSliderRect,
    // handle_event), shared by Terrain mode's own paint brush and the
    // global eraser tool (erasing_) below -- both size their brush the
    // same way, just against different underlying actions.
    auto draw_brush_slider = [&]() {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, &kBrushSliderRect);
        SDL_SetRenderDrawColor(renderer, 150, 150, 150, 255);
        SDL_RenderDrawRect(renderer, &kBrushSliderRect);
        // Track line + 5 tick marks, one per selectable size.
        int track_y = kBrushSliderRect.y + kBrushSliderRect.h / 2;
        SDL_SetRenderDrawColor(renderer, 100, 100, 100, 255);
        SDL_RenderDrawLine(renderer, kBrushSliderRect.x + 10, track_y, kBrushSliderRect.x + kBrushSliderRect.w - 10,
                           track_y);
        for (int i = 0; i < 5; ++i) {
            int tx = kBrushSliderRect.x + 10 +
                    static_cast<int>((kBrushSliderRect.w - 20) * (i / 4.0));
            bool active = (brush_size_ == i + 1);
            SDL_Color col = active ? SDL_Color{255, 220, 60, 255} : SDL_Color{150, 150, 150, 255};
            SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
            SDL_Rect knob{tx - (active ? 8 : 5), track_y - (active ? 8 : 5), active ? 16 : 10, active ? 16 : 10};
            SDL_RenderFillRect(renderer, &knob);
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%d", i + 1);
            int tw2, th2;
            text_regular_.measure(buf, 15, tw2, th2);
            text_regular_.draw(buf, tx - tw2 / 2, track_y + 14, {200, 200, 200, 255}, 15);
        }
        hit_rects_.push_back({kBrushSliderRect, "brush_slider", 0});
    };
    if (erasing_) {
        // The eraser tool's own toolbar overrides whatever edit_map_mode_
        // would otherwise show here -- it's a global tool, see erasing_'s
        // comment -- just a label and the shared brush-size slider.
        text_regular_.draw("Erasing -- click or drag on the map to remove terrain, units, and buildings.",
                           bc.x + 12, bc.y + 12, {230, 200, 60, 255}, 18);
        draw_brush_slider();
    } else if (edit_map_mode_ == EditMapMode::Map) {
        // Random map (map type) vs. blank map (single default terrain
        // fill), a Tiny/Normal/Large/Huge size picker (same presets as new
        // level creation), and a Generate button that rebuilds
        // current_level() from scratch (see generate_map()) -- deletes
        // every existing terrain tile/unit/building, but leaves players
        // alone (civs/teams aren't part of the map itself).
        SDL_Rect mode_btn{bc.x + 12, bc.y + 12, 240, 40};
        draw_button(renderer, mode_btn, map_gen_random_ ? "Mode: Random Map" : "Mode: Blank Map",
                   "map_gen_random_toggle", 0, {255, 220, 60, 255});
        SDL_Rect size_btn{mode_btn.x + mode_btn.w + 12, mode_btn.y, 240, 40};
        draw_button(renderer, size_btn, std::string("Size: ") + kMapSizePresets[map_gen_size_idx_].name,
                   "map_gen_size", 0, {255, 220, 60, 255});

        SDL_Rect sub_btn{bc.x + 12, mode_btn.y + mode_btn.h + 12, 320, 40};
        if (map_gen_random_) {
            int idx = 0;
            for (int i = 0; i < 5; ++i) {
                if (map_gen_type_ == kMapGenTypes[i]) { idx = i; break; }
            }
            draw_button(renderer, sub_btn, std::string("Map type: ") + kMapGenTypeLabels[idx], "map_gen_type",
                       0, {255, 220, 60, 255});
        } else {
            int idx = 0;
            for (int i = 0; i < 4; ++i) {
                if (map_gen_default_ == kDefaultTerrains[i]) { idx = i; break; }
            }
            draw_button(renderer, sub_btn, std::string("Default terrain: ") + kDefaultTerrainLabels[idx],
                       "map_gen_default", 0, {255, 220, 60, 255});
        }

        SDL_Rect gen_btn{bc.x + bc.w - 208, bc.y + 12, 196, bc.h - 24};
        draw_button(renderer, gen_btn, "Generate", "map_gen_generate", 0, {60, 220, 60, 255});
        text_regular_.draw("Generating deletes all existing terrain/units/buildings.", sub_btn.x,
                           sub_btn.y + sub_btn.h + 10, {200, 160, 160, 255}, 15);
    } else if (edit_map_mode_ == EditMapMode::Terrain) {
        // Sub-category tabs, same pattern as Units mode's Units/Buildings
        // row below -- filters the left sidebar's palette down to one
        // group of kTerrainKinds at a time (see TerrainCategory and
        // terrain_category_row_count).
        const char* kCatLabels[4] = {"Ground", "Decoration", "Blocks", "Objects"};
        int cx = bc.x + 12;
        for (int t = 0; t < 4; ++t) {
            SDL_Rect r{cx, bc.y + 12, 150, 40};
            bool active = (static_cast<int>(terrain_category_) == t);
            draw_button(renderer, r, kCatLabels[t], "terrain_category", t,
                       active ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255});
            cx += 150 + 12;
        }
        // Which terrain kind is being painted is chosen from the left
        // sidebar, not here -- this row is just the shared brush-size
        // slider (see draw_brush_slider's comment). No "Brush size:"
        // label (unlike before the category tabs were added above it) --
        // the per-tick number labels below the slider are self-
        // explanatory enough, and there's no vertical room left to fit a
        // label without overlapping the tabs row.
        draw_brush_slider();
    } else if (edit_map_mode_ == EditMapMode::Players) {
        if (players_selected_ < 0 || players_selected_ >= static_cast<int>(lvl->players.size())) {
            std::string msg = "Pick a player on the left first.";
            int tw, th;
            text_regular_.measure(msg, 20, tw, th);
            text_regular_.draw(msg, bc.x + (bc.w - tw) / 2, bc.y + (bc.h - th) / 2, {160, 160, 160, 255}, 20);
        } else {
            LevelPlayer& p = lvl->players[players_selected_];
            // P1 always follows the campaign's own civ (see draw_edit_level's
            // identical rule for the same reason) -- no prev/next arrows for
            // it here either, just the flag.
            bool civ_locked = (players_selected_ == 0);

            int cx = bc.x + 12;
            const int row_h = 56, row_y = bc.y + 4;
            if (!civ_locked) {
                SDL_Rect prev_btn{cx, row_y, 26, row_h};
                draw_button(renderer, prev_btn, "<", "players_civ_prev", 0, {200, 200, 200, 255});
                cx += 26 + 4;
            }
            SDL_Rect flag_box{cx, row_y, 64, row_h};
            draw_panel(renderer, flag_box);
            if (atlas_.meta("spr_flags_mini")) atlas_.draw_in_rect(flag_box, "spr_flags_mini", p.civ, 4);
            cx += 64 + 4;
            if (!civ_locked) {
                SDL_Rect next_btn{cx, row_y, 26, row_h};
                draw_button(renderer, next_btn, ">", "players_civ_next", 0, {200, 200, 200, 255});
                cx += 26;
            }
            cx += 10;
            std::string civ_name = (p.civ >= 0 && p.civ < 9) ? ww::menu::civ_names()[p.civ] : "?";
            int civ_tw, civ_th;
            text_regular_.measure(civ_name, 18, civ_tw, civ_th);
            text_regular_.draw(civ_name, cx, row_y + (row_h - civ_th) / 2, {255, 255, 255, 255}, 18);
            cx += 150;

            // ---- era picker: always editable, even for P1 -- unlike civ,
            // starting era isn't tied to the campaign's own identity. ----
            SDL_Rect era_prev{cx, row_y, 26, row_h};
            draw_button(renderer, era_prev, "<", "players_era_prev", 0, {200, 200, 200, 255});
            cx += 26 + 4;
            SDL_Rect era_box{cx, row_y, 56, row_h};
            draw_panel(renderer, era_box);
            if (atlas_.meta("spr_era_icon")) atlas_.draw_in_rect(era_box, "spr_era_icon", std::clamp(p.era, 0, 3), 6);
            cx += 56 + 4;
            SDL_Rect era_next{cx, row_y, 26, row_h};
            draw_button(renderer, era_next, ">", "players_era_next", 0, {200, 200, 200, 255});
            cx += 26 + 10;
            static const char* kEraNames[4] = {"Victorian Era", "Industrial Era", "War Era", "Scientific Era"};
            std::string era_name = kEraNames[std::clamp(p.era, 0, 3)];
            int era_tw, era_th;
            text_regular_.measure(era_name, 18, era_tw, era_th);
            text_regular_.draw(era_name, cx, row_y + (row_h - era_th) / 2, {255, 255, 255, 255}, 18);
            cx += 150;

            // ---- live appearance preview: base architecture (civ) and
            // house (era) -- the whole point of the civ/era pickers above
            // actually mattering visually, see world_sprite's comments. ----
            SDL_Rect base_prev{cx, row_y - 4, row_h + 8, row_h + 8};
            draw_panel(renderer, base_prev);
            std::string base_sprite = world_sprite("base", p.civ);
            if (atlas_.meta(base_sprite)) atlas_.draw_in_rect(base_prev, base_sprite, 0, 4);
            text_regular_.draw("Base", base_prev.x + 4, base_prev.y + base_prev.h + 2, {170, 170, 170, 255}, 13);
            cx += base_prev.w + 10;
            SDL_Rect house_prev{cx, row_y - 4, row_h + 8, row_h + 8};
            draw_panel(renderer, house_prev);
            std::string house_sprite = world_sprite("house", p.civ, p.era);
            if (atlas_.meta(house_sprite)) {
                atlas_.draw_in_rect(house_prev, house_sprite, std::clamp(p.team - 1, 0, 7), 4);
            }
            text_regular_.draw("House", house_prev.x + 4, house_prev.y + house_prev.h + 2, {170, 170, 170, 255}, 13);

            // ---- starting resources: one icon + typeable amount per
            // RES_KEYS entry (food/wood/oil/iron, matching sim::Team's own
            // res map and the main game's HUD order -- see spr_resources'
            // frame comment in game_client.cpp's draw_hud), spread evenly
            // across the row below. Click the number to type a new value
            // directly (see Field::PlayerResource/players_res_edit_)
            // instead of clicking +/- repeatedly. ----
            struct ResField { int frame; double* value; };
            ResField fields[4] = {
                {0, &p.food},
                {1, &p.wood},
                {2, &p.oil},
                {3, &p.iron},
            };
            int cell_w = (bc.w - 24) / 4;
            int ry = bc.y + 84; // clear of the base/house preview labels just above (see base_prev/house_prev)
            for (int i = 0; i < 4; ++i) {
                int rx = bc.x + 12 + i * cell_w;
                SDL_Rect icon_r{rx, ry, 40, 40};
                atlas_.draw_in_rect(icon_r, "spr_resources", fields[i].frame, 2);
                bool focused = (active_field_ == Field::PlayerResource && players_res_field_ == i);
                if (!focused) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%.0f", *fields[i].value);
                    players_res_edit_[i] = buf;
                }
                SDL_Rect field_r{rx + 46, ry, 100, 40};
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderFillRect(renderer, &field_r);
                SDL_SetRenderDrawColor(renderer, focused ? 255 : 150, focused ? 220 : 150, focused ? 60 : 150, 255);
                SDL_RenderDrawRect(renderer, &field_r);
                std::string display = players_res_edit_[i] + (focused ? "_" : "");
                int btw, bth;
                text_regular_.measure(display, 20, btw, bth);
                text_regular_.draw(display, field_r.x + (field_r.w - btw) / 2, field_r.y + (field_r.h - bth) / 2,
                                   {255, 255, 255, 255}, 20);
                hit_rects_.push_back({field_r, "players_res_edit", i});
            }
        }
    } else if (edit_map_mode_ == EditMapMode::Events) {
        if (events_selected_ < 0 || events_selected_ >= static_cast<int>(lvl->events.size())) {
            std::string msg = "Create an event, or click an existing one, to edit it.";
            int tw, th;
            text_regular_.measure(msg, 18, tw, th);
            text_regular_.draw(msg, bc.x + (bc.w - tw) / 2, bc.y + (bc.h - th) / 2, {160, 160, 160, 255}, 18);
        } else {
            ww::campaign::MapEvent& ev = lvl->events[events_selected_];
            text_regular_.draw("Name:", bc.x + 12, bc.y + 20, {255, 255, 255, 255}, 20);
            SDL_Rect field_r{bc.x + 80, bc.y + 12, 220, 44};
            draw_text_field(renderer, field_r, ev.name, Field::EventName, "Event name...");

            int type_x = field_r.x + field_r.w + 16;
            text_regular_.draw("Type:", type_x, bc.y + 20, {255, 255, 255, 255}, 20);
            static const char* kTypeLabels[5] = {"Message", "Gate", "Dormant", "Resources", "Spawn"};
            static const char* kTypeIds[5] = {"message", "gate", "dormant", "resources", "spawn"};
            int tbx = type_x + 54;
            for (int i = 0; i < 5; ++i) {
                SDL_Rect tb{tbx, bc.y + 12, 92, 44};
                draw_button(renderer, tb, kTypeLabels[i], "event_type", i,
                           ev.type == kTypeIds[i] ? SDL_Color{255, 220, 60, 255}
                                                  : SDL_Color{160, 160, 160, 255});
                tbx += 92 + 6;
            }

            int row2_y = field_r.y + field_r.h + 10;
            // Shared "message shown when triggered" field (message/spawn/dormant/
            // resources all fire ev.text as an on-screen notification).
            auto draw_msg_field = [&](int y, const char* label) {
                text_regular_.draw(label, bc.x + 12, y + 7, {255, 255, 255, 255}, 15);
                SDL_Rect mf{bc.x + 240, y, bc.w - 252, 30};
                draw_text_field(renderer, mf, ev.text, Field::MessageText, "(optional) shown when it fires...");
            };
            // Shared objective picker ("None" + one button per objective).
            // Returns the y just below the (possibly wrapped) row of buttons.
            auto draw_objective_picker = [&](int y) -> int {
                text_regular_.draw("Trigger objective:", bc.x + 12, y + 4, {255, 255, 255, 255}, 16);
                int btn_w = 140, btn_h = 28, gap = 8;
                int bx = bc.x + 180, by = y;
                bool none_sel = ev.unlock_objective_id.empty();
                draw_button(renderer, {bx, by, 80, btn_h}, "None", "event_unlock_objective", -1,
                           none_sel ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255});
                bx += 80 + gap;
                for (int i = 0; i < static_cast<int>(lvl->objectives.size()); ++i) {
                    if (bx + btn_w > bc.x + bc.w) {
                        bx = bc.x + 180;
                        by += btn_h + gap;
                    }
                    const ww::campaign::Objective& obj = lvl->objectives[i];
                    std::string label = obj.name.empty() ? "(untitled)" : obj.name;
                    draw_button(renderer, {bx, by, btn_w, btn_h}, label, "event_unlock_objective", i,
                               ev.unlock_objective_id == obj.id ? SDL_Color{255, 220, 60, 255}
                                                                : SDL_Color{160, 160, 160, 255});
                    bx += btn_w + gap;
                }
                return by + btn_h + 8;
            };
            if (ev.type == "message") {
                text_regular_.draw("Message:", bc.x + 12, row2_y + 8, {255, 255, 255, 255}, 16);
                SDL_Rect msg_field{bc.x + 100, row2_y, bc.w - 112, 34};
                draw_text_field(renderer, msg_field, ev.text, Field::MessageText, "Type the message here...");
                std::string hint =
                    (ev.tx < 0) ? "Click on the map to place it." : "Click on the map to move it.";
                text_regular_.draw(hint, bc.x + 12, msg_field.y + msg_field.h + 8, {200, 200, 160, 255}, 15);
            } else if (ev.type == "gate") {
                int y = draw_objective_picker(row2_y);
                std::string area_hint = (ev.area_tw > 0 && ev.area_th > 0)
                    ? "Click-drag on the map to redraw the gate area."
                    : "Click-drag on the map to mark the gate area.";
                text_regular_.draw(area_hint, bc.x + 12, y, {200, 200, 160, 255}, 15);
            } else if (ev.type == "resources") {
                // Touch pickup: place a marker tile, set amounts, optional message.
                std::string hint = (ev.tx < 0) ? "Click on the map to place the yellow pickup tile."
                                               : "Click on the map to move the pickup tile.";
                text_regular_.draw(hint, bc.x + 12, row2_y + 6, {200, 200, 160, 255}, 15);
                double* res_fields[4] = {&ev.res_food, &ev.res_wood, &ev.res_oil, &ev.res_iron};
                int ry = row2_y + 28, rx = bc.x + 12;
                for (int i = 0; i < 4; ++i) {
                    SDL_Rect icon_r{rx, ry, 22, 22};
                    atlas_.draw_in_rect(icon_r, "spr_resources", i, 2);
                    bool focused = (active_field_ == Field::EventResource && event_res_field_ == i);
                    if (!focused) {
                        char buf[16];
                        std::snprintf(buf, sizeof(buf), "%.0f", *res_fields[i]);
                        event_res_edit_[i] = buf;
                    }
                    SDL_Rect res_field_r{rx + 26, ry, 80, 22};
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                    SDL_RenderFillRect(renderer, &res_field_r);
                    SDL_SetRenderDrawColor(renderer, focused ? 255 : 150, focused ? 220 : 150,
                                           focused ? 60 : 150, 255);
                    SDL_RenderDrawRect(renderer, &res_field_r);
                    std::string display = event_res_edit_[i] + (focused ? "_" : "");
                    int dtw, dth;
                    text_regular_.measure(display, 15, dtw, dth);
                    text_regular_.draw(display, res_field_r.x + (res_field_r.w - dtw) / 2,
                                       res_field_r.y + (res_field_r.h - dth) / 2, {255, 255, 255, 255}, 15);
                    hit_rects_.push_back({res_field_r, "event_res_edit", i});
                    rx += 116;
                }
                draw_msg_field(ry + 30, "Message on pickup:");
            } else if (ev.type == "spawn") {
                SDL_Rect unit_btn{bc.x + 12, row2_y, 210, 30};
                std::string ulabel = "Unit: " + (ev.spawn_unit.empty() ? std::string("(pick)") : ev.spawn_unit);
                draw_button(renderer, unit_btn, ulabel, "cycle_spawn_unit", 0, {120, 200, 255, 255});
                text_regular_.draw("Count:", unit_btn.x + unit_btn.w + 12, row2_y + 6, {255, 255, 255, 255}, 15);
                bool cfocused = (active_field_ == Field::SpawnCount);
                if (!cfocused) spawn_count_edit_ = std::to_string(ev.spawn_count);
                SDL_Rect count_r{unit_btn.x + unit_btn.w + 72, row2_y, 70, 30};
                draw_text_field(renderer, count_r, spawn_count_edit_, Field::SpawnCount, "0");
                char obuf[24];
                std::snprintf(obuf, sizeof(obuf), "Owner: P%d", ev.spawn_player + 1);
                SDL_Rect owner_btn{count_r.x + count_r.w + 12, row2_y, 120, 30};
                draw_button(renderer, owner_btn, obuf, "cycle_spawn_player", 0, {200, 160, 255, 255});
                int y = draw_objective_picker(row2_y + 38);
                std::string area_hint = (ev.area_tw > 0 && ev.area_th > 0)
                    ? "Click-drag on the map to redraw the spawn box."
                    : "Click-drag on the map to mark the spawn box (where the units appear).";
                text_regular_.draw(area_hint, bc.x + 12, y, {200, 200, 160, 255}, 15);
                draw_msg_field(y + 22, "Message on spawn:");
            } else if (ev.type == "dormant") {
                std::string units_hint = (ev.area_tw > 0 && ev.area_th > 0)
                    ? "Units box (red) drawn. Click-drag to redraw which units start dormant."
                    : "Click-drag on the map to draw the UNITS box (which units start dormant).";
                text_regular_.draw(units_hint, bc.x + 12, row2_y + 4, {230, 170, 170, 255}, 15);
                int by = row2_y + 26;
                SDL_Rect trig_btn{bc.x + 12, by, 200, 30};
                draw_button(renderer, trig_btn,
                           area_target_ == AreaTarget::TriggerArea ? "Click-drag the trigger box..."
                           : (ev.area2_tw > 0 ? "Redraw Trigger Box" : "Draw Trigger Box"),
                           "draw_trigger_box", 0,
                           area_target_ == AreaTarget::TriggerArea ? SDL_Color{60, 220, 60, 255}
                                                                   : SDL_Color{90, 230, 255, 255});
                SDL_Rect sight_btn{trig_btn.x + trig_btn.w + 8, by, 150, 30};
                draw_button(renderer, sight_btn,
                           area_target_ == AreaTarget::SightArea ? "Click-drag a sight box..." : "Add Sight Box",
                           "add_sight_box", 0,
                           area_target_ == AreaTarget::SightArea ? SDL_Color{60, 220, 60, 255}
                                                                 : SDL_Color{120, 240, 150, 255});
                if (!ev.los_areas.empty()) {
                    char sbuf[40];
                    std::snprintf(sbuf, sizeof(sbuf), "Clear Sight (%d)", static_cast<int>(ev.los_areas.size()));
                    SDL_Rect clr_btn{sight_btn.x + sight_btn.w + 8, by, 150, 30};
                    draw_button(renderer, clr_btn, sbuf, "clear_sight_boxes", 0, {255, 90, 90, 255});
                }
                text_regular_.draw(
                    "On trip: dormant units wake and charge the trigger box; sight boxes are revealed.",
                    bc.x + 12, by + 34, {170, 170, 140, 255}, 14);
                draw_msg_field(by + 54, "Message on wake:");
            }
        }
    } else if (edit_map_mode_ == EditMapMode::Objectives) {
        if (objectives_selected_ < 0 || objectives_selected_ >= static_cast<int>(lvl->objectives.size())) {
            std::string msg = "Create an objective, or click an existing one, to edit it.";
            int tw, th;
            text_regular_.measure(msg, 18, tw, th);
            text_regular_.draw(msg, bc.x + (bc.w - tw) / 2, bc.y + (bc.h - th) / 2, {160, 160, 160, 255}, 18);
        } else {
            ww::campaign::Objective& obj = lvl->objectives[objectives_selected_];
            text_regular_.draw("Name:", bc.x + 12, bc.y + 20, {255, 255, 255, 255}, 20);
            SDL_Rect field_r{bc.x + 80, bc.y + 12, 260, 44};
            draw_text_field(renderer, field_r, obj.name, Field::ObjectiveName, "Objective name...");

            int type_x = field_r.x + field_r.w + 24;
            text_regular_.draw("Type:", type_x, bc.y + 20, {255, 255, 255, 255}, 20);
            SDL_Rect kill_btn{type_x + 60, bc.y + 12, 140, 44};
            draw_button(renderer, kill_btn, "Kill Units", "objective_type", 0,
                       obj.type == "kill_units" ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255});
            SDL_Rect move_btn{kill_btn.x + kill_btn.w + 8, bc.y + 12, 140, 44};
            draw_button(renderer, move_btn, "Move To Area", "objective_type", 1,
                       obj.type == "move_to_area" ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255});
            SDL_Rect protect_btn{move_btn.x + move_btn.w + 8, bc.y + 12, 140, 44};
            draw_button(renderer, protect_btn, "Protect Unit", "objective_type", 2,
                       obj.type == "protect_unit" ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255});

            // Visibility: shown (counts toward campaign win/lose AND can
            // trigger events) vs. hidden (event-trigger only, see
            // Objective::hidden's comment) -- same two-button picker style
            // as Type above.
            int row2_y = field_r.y + field_r.h + 10;
            text_regular_.draw("Visibility:", bc.x + 12, row2_y + 6, {255, 255, 255, 255}, 18);
            SDL_Rect shown_btn{bc.x + 100, row2_y, 110, 34};
            draw_button(renderer, shown_btn, "Shown", "objective_visibility", 0,
                       !obj.hidden ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255});
            SDL_Rect hidden_btn{shown_btn.x + shown_btn.w + 8, row2_y, 110, 34};
            draw_button(renderer, hidden_btn, "Hidden", "objective_visibility", 1,
                       obj.hidden ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255});
            std::string visibility_hint = obj.hidden
                ? "Hidden: only usable as an event trigger, doesn't affect winning/losing the campaign."
                : "Shown: must be met (and not failed) to win the campaign, and still usable as an event trigger.";
            text_regular_.draw(visibility_hint, hidden_btn.x + hidden_btn.w + 16, row2_y + 9,
                               {170, 170, 170, 255}, 14);

            bool has_area = (obj.area_tw > 0 && obj.area_th > 0);
            std::string count_suffix = std::to_string(obj.target_unit_ids.size()) + " unit" +
                                       (obj.target_unit_ids.size() == 1 ? "" : "s") + " marked)";
            std::string hint;
            if (obj.type == "kill_units") {
                hint = has_area ? "Click and drag on the map to redraw the kill area (" + count_suffix + "."
                                : "Click and drag on the map to mark the kill area.";
            } else if (obj.type == "protect_unit") {
                hint = has_area ? "Click and drag on the map to redraw the protected area (" + count_suffix + "."
                                : "Click and drag on the map to mark the area of units to protect.";
            } else {
                hint = has_area
                    ? "Click and drag on the map to redraw the destination area."
                    : "Click and drag on the map to mark the destination area the player's units must reach.";
            }
            text_regular_.draw(hint, bc.x + 12, row2_y + 34 + 8, {200, 200, 160, 255}, 16);
        }
    } else { // Units
        const char* kTabLabels[2] = {"Units", "Buildings"};
        int bx = bc.x + 12;
        for (int t = 0; t < 2; ++t) {
            SDL_Rect r{bx, bc.y + 12, 150, 40};
            bool active = (static_cast<int>(map_tab_) == t);
            draw_button(renderer, r, kTabLabels[t], "map_tab", t,
                       active ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255});
            bx += 150 + 12;
        }
        // Lasso move tool: drag a box to select everything (non-grass tiles,
        // resources, units, buildings) then drag it to relocate the group.
        SDL_Rect lasso_btn{bx, bc.y + 12, 150, 40};
        draw_button(renderer, lasso_btn, "Lasso Move", "toggle_lasso", 0,
                   lasso_tool_ ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255});

        // Which player you're placing units/buildings for.
        SDL_Rect strip{bc.x + 12, bc.y + 12 + 40 + 12, bc.w - 24, 40};
        text_regular_.draw("Player:", strip.x, strip.y + 10, {200, 200, 200, 255}, 18);
        int px = strip.x + 80;
        int pw = std::min(90, (strip.w - 80) / std::max<int>(1, static_cast<int>(lvl->players.size())));
        for (int i = 0; i < static_cast<int>(lvl->players.size()); ++i) {
            SDL_Rect team_btn{px, strip.y, pw - 6, strip.h};
            bool active = (i == active_team_);
            SDL_Color col = active ? SDL_Color{255, 220, 60, 255} : SDL_Color{160, 160, 160, 255};
            draw_button(renderer, team_btn, "P" + std::to_string(i + 1), "pick_team", i, col);
            px += pw;
        }

        // ---- AI controls row (campaign play): the selected AI player's
        // behaviour preset + the level-wide bespoke AI-profile name. P1 is the
        // human, so it has no AI behaviour. ----
        int aiy = strip.y + strip.h + 10;
        if (active_team_ == 0) {
            text_regular_.draw("P1 is the human player (no AI).", strip.x, aiy + 8,
                               {150, 150, 150, 255}, 16);
        } else if (active_team_ > 0 && active_team_ < static_cast<int>(lvl->players.size())) {
            text_regular_.draw("AI:", strip.x, aiy + 8, {200, 200, 200, 255}, 18);
            std::string cur = lvl->players[active_team_].ai_behavior;
            SDL_Rect ai_btn{strip.x + 40, aiy, 160, 34};
            draw_button(renderer, ai_btn, cur.empty() ? "default" : cur, "cycle_ai_behavior",
                       active_team_, {255, 220, 60, 255});
        }
        text_regular_.draw("AI profile:", strip.x + 220, aiy + 8, {200, 200, 200, 255}, 16);
        SDL_Rect prof_field{strip.x + 320, aiy, 280, 34};
        draw_text_field(renderer, prof_field, lvl->ai_profile, Field::AiProfile,
                        "(optional bespoke name)");
    }

    // ---- minimap: docked in the bottom toolbar's own far-right corner,
    // with a viewport rectangle showing what the main view above is
    // currently looking at -- click it to jump there. Most useful on a
    // Large/Huge level (see draw_new_level), where the main view only
    // ever shows a fraction of the map at once. Drawn last so it sits on
    // top of the toolbar's own background panel. ----
    draw_minimap_contents(renderer, kMinimapRect);
    double grid_span_px = kLevelGridSize * kEditorTile;
    double mini_scale = kMiniSize / grid_span_px;
    // Viewport rectangle: what the main view above is currently showing,
    // converted from the camera's padded-world space back to grid space.
    // Clamped to the minimap panel's own bounds -- at the default fit-to-
    // view zoom (which deliberately leaves a little margin around the
    // grid, see "edit_map"'s *0.9), the visible area can be wider than
    // the grid itself on a non-square viewport, which would otherwise
    // draw this rectangle spilling out past the minimap's edges.
    SDL_Rect vp{kMinimapRect.x + static_cast<int>((vr.x - kMapMargin) * mini_scale),
               kMinimapRect.y + static_cast<int>((vr.y - kMapMargin) * mini_scale),
               std::max(1, static_cast<int>(vr.w * mini_scale)),
               std::max(1, static_cast<int>(vr.h * mini_scale))};
    SDL_Rect vp_clamped;
    if (SDL_IntersectRect(&vp, &kMinimapRect, &vp_clamped)) {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);
        SDL_RenderDrawRect(renderer, &vp_clamped);
    }
}

void Editor::draw_minimap_contents(SDL_Renderer* renderer, const SDL_Rect& rect) {
    Level* lvl = current_level();
    if (!lvl) return;
    int grid_size = lvl->grid_size;

    std::unordered_map<int, const ww::campaign::TerrainFeature*> terrain_at;
    for (auto& t : lvl->terrain) terrain_at[t.tx * grid_size + t.ty] = &t;

    // Every tile gets a solid, opaque fill -- grass by default, or
    // whichever base/resource colour is actually painted there -- so this
    // reads as "what the map looks like" at a glance, not just a scatter
    // of dots over a blank background (there's nothing underneath it any
    // more to show through now that it's docked in the bottom toolbar
    // rather than floating over the canvas).
    double scale = static_cast<double>(rect.w) / grid_size; // rect is always square, see call sites
    for (int tx = 0; tx < grid_size; ++tx) {
        int sx0 = rect.x + static_cast<int>(tx * scale);
        int sx1 = rect.x + static_cast<int>((tx + 1) * scale);
        for (int ty = 0; ty < grid_size; ++ty) {
            SDL_Color c = kGrassColour;
            auto it = terrain_at.find(tx * grid_size + ty);
            if (it != terrain_at.end()) {
                const TerrainKind* base_kind = find_terrain_kind(it->second->base);
                const TerrainKind* res_kind = find_terrain_kind(it->second->resource);
                if (base_kind) c = base_kind->colour;
                else if (res_kind) c = res_kind->colour;
            }
            int sy0 = rect.y + static_cast<int>(ty * scale);
            int sy1 = rect.y + static_cast<int>((ty + 1) * scale);
            SDL_Rect px{sx0, sy0, std::max(1, sx1 - sx0), std::max(1, sy1 - sy0)};
            SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
            SDL_RenderFillRect(renderer, &px);
        }
    }

    auto grid_to_mini = [&](double gx, double gy, int& mx2, int& my2) {
        mx2 = rect.x + static_cast<int>(gx / kEditorTile * scale);
        my2 = rect.y + static_cast<int>(gy / kEditorTile * scale);
    };
    auto plot_entity_dot = [&](const ww::campaign::PlacedEntity& e) {
        int mx2, my2;
        grid_to_mini((e.tx + 0.5) * kEditorTile, (e.ty + 0.5) * kEditorTile, mx2, my2);
        SDL_Color col = kTeamColours[std::clamp(
            (e.player_index >= 0 && e.player_index < static_cast<int>(lvl->players.size()))
                ? lvl->players[e.player_index].team - 1
                : 0,
            0, 3)];
        SDL_Rect px{mx2 - 1, my2 - 1, 3, 3};
        SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, 255);
        SDL_RenderFillRect(renderer, &px);
    };
    for (auto& b : lvl->buildings) plot_entity_dot(b);
    for (auto& u : lvl->units) plot_entity_dot(u);

    SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
    SDL_RenderDrawRect(renderer, &rect);
}
