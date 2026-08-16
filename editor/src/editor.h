#pragma once
#include "campaign/campaign_data.h"
#include "catalog.h"
#include "render/camera.h"
#include "render/sprite_atlas.h"
#include "render/text_renderer.h"

#include <SDL.h>

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Top-level UI/state for the standalone campaign editor -- structurally
// mirrors worldwar_client's MenuController (Screen enum, hit-rects built
// during draw() and hit-tested on the next click, one TextRenderer/
// SpriteAtlas pair) since it's the same kind of "a few simple screens,
// no game loop" program, just for authoring campaign data instead of
// picking skirmish settings.
class Editor {
public:
    Editor(SDL_Renderer* renderer, const std::string& asset_dir, const std::string& data_dir,
          int view_w, int view_h);
    ~Editor();

    void handle_event(const SDL_Event& ev);
    void draw(SDL_Renderer* renderer);
    bool wants_quit() const { return wants_quit_; }

    // Public so editor.cpp's file-scope kMapCanvasRect (alongside kPanel)
    // can be computed from these once, rather than every call site
    // re-deriving the canvas viewport's screen position independently.
    static constexpr int kEditorTile = 32; // matches the main game's TILE (sim/include/sim/world.h)
    // Canvas viewport size, screen px. Derived from the 1600x1024 window
    // (see editor.cpp's kPanel comment): whatever's left of kPanel's
    // content-area width after the fixed-width left palette sidebar (shown
    // only in Units mode, see EditMapMode) is carved out, and whatever's
    // left of kPanel's height after the new top mode-toolbar and bottom
    // options-toolbar (see draw_edit_map) are carved out. No right
    // sidebar any more -- that space now goes entirely to the canvas.
    static constexpr int kMapViewW = 1258, kMapViewH = 664;
    // Camera::clamp() never lets the viewport pan/zoom past the world's
    // own [0,world_w]x[0,world_h] bounds -- without this margin, a unit or
    // building sitting on an edge tile (whose real sprite is much bigger
    // than one tile, e.g. a tank or battleship) would have its overhang
    // permanently clipped at the canvas edge with no way to scroll far
    // enough to see it, at ANY zoom level. The grid itself still occupies
    // exactly [0,grid_px]; this just gives the camera room to look a
    // little beyond that. See draw_edit_map's to_screen/to_grid helpers.
    static constexpr int kMapMargin = 3 * kEditorTile;

    // Terrain mode's palette sub-category (see draw_edit_map's bottom
    // toolbar, mirroring Units mode's Units/Buildings split) -- public for
    // the same reason as kMapViewW etc. above: editor.cpp's file-scope
    // kTerrainKinds table (each entry tagged with the category it belongs
    // to) needs to name this type from outside the class.
    // Ground: base/ground textures, including water. Decoration: rubble
    // (all variants) plus other small passable clutter. Blocks: larger,
    // mostly-impassable obstacles (trees, hedge, fences, rock/stone
    // piles, crates, destroyed-building ruins). Objects: gatherable
    // resources and flames.
    enum class TerrainCategory { Ground, Decoration, Blocks, Objects };

private:
    enum class Screen { CampaignList, NewCampaign, LevelList, NewLevel, EditLevel, EditMap };
    // Which scratch string SDL_TEXTINPUT/backspace routes into -- only one
    // field can be focused at a time across the whole editor.
    enum class Field {
        None,
        CampaignName,
        NewLevelName,
        LevelName,
        LevelDescription,
        // Events mode: the currently-selected event's message text (only
        // meaningful for a "message"-type event, see events_selected_) --
        // capped at 100 chars, see active_field_max_len.
        MessageText,
        // Players mode: one of the selected player's food/wood/oil/iron
        // stockpile fields, see players_res_field_ for which -- shared by
        // all 4 since only one can ever be focused at a time, same reason
        // active_field_ itself is a single value rather than per-field.
        PlayerResource,
        // Objectives mode: the currently-selected objective's name (see
        // objectives_selected_).
        ObjectiveName,
        // Events mode: the currently-selected event's name (see
        // events_selected_) -- same idea as ObjectiveName above.
        EventName,
        // Events mode: one of the selected "resources"-type event's
        // res_food/res_wood/res_oil/res_iron fields, see
        // event_res_field_ for which -- same scratch-buffer-not-the-real-
        // field pattern as PlayerResource above (see commit_active_field).
        EventResource,
        // Units mode: the level's bespoke AI profile name (Level::ai_profile) --
        // a free-typed label the engine's AI branches on, see the editor's
        // AI-controls row in draw_edit_map's Units bottom toolbar.
        AiProfile,
        // Events mode: a "spawn"-type event's unit count (MapEvent::spawn_count) --
        // a digit-only scratch buffer (spawn_count_edit_) parsed back on commit,
        // same pattern as EventResource above.
        SpawnCount,
    };
    // EditMap's Units-mode bottom-toolbar sub-tab -- places one entity per
    // click, owned by whichever player row is currently selected (see
    // active_team_). Terrain painting/erasing used to live here too (as
    // "Other"/"Erase") but now has its own top-level EditMapMode instead.
    enum class MapTab { Units, Buildings };
    // EditMap's top mode-toolbar -- picks what the bottom options-toolbar
    // (and, for every mode but Map, the left palette sidebar) shows. Map:
    // regenerate the whole level's terrain (see generate_map()). Terrain:
    // paint/erase terrain tiles (water/trees/resources/decorations, see
    // kTerrainKinds). Units: place units/buildings, owned by whichever
    // player is selected. Players: edit a player's civ/era/starting
    // resources. Events: create/select map events (message/gate/dormant,
    // see MapEvent) -- see events_selected_, same create/list/select-to-
    // edit UI as Objectives below. Objectives: create/select mission
    // objectives (kill_units/move_to_area, see Objective) -- see
    // objectives_selected_.
    enum class EditMapMode { Map, Terrain, Players, Units, Events, Objectives };

    std::string data_dir_;
    std::string asset_dir_; // where uploaded briefing images are copied to
    int view_w_, view_h_;
    // Cached texture for the current level's briefing image (see
    // Level::briefing_image / upload_briefing_image) -- reloaded lazily when
    // the referenced file changes, freed on the next load / on quit.
    SDL_Texture* briefing_tex_ = nullptr;
    std::string briefing_tex_key_;
    Screen screen_ = Screen::CampaignList;
    bool wants_quit_ = false;

    SpriteAtlas atlas_;
    TextRenderer text_;
    TextRenderer text_regular_;

    // Catalog access (unit/building names, icon sprites, civ exclusions)
    // for the map editor's palette -- see game_data.h/catalog.h, a small
    // standalone vendored slice of the main engine's own data tables, kept
    // deliberately dependency-free from the rest of that engine.
    ww::gamedata::DataStore data_;
    std::unordered_map<int, std::set<std::string>> civ_exclude_;

    std::vector<ww::campaign::Campaign> campaigns_;
    int selected_campaign_ = -1; // index into campaigns_; -1 = none open
    int selected_level_ = -1;    // index into the open campaign's levels; -1 = none open

    // Scratch state for the "New Campaign" form -- not written into
    // campaigns_ until "Create" is clicked.
    std::string new_campaign_name_;
    int new_campaign_civ_ = 0;

    // Scratch state for the "New Level" form -- map size (Tiny/Normal/
    // Large/Huge, matching the main game's Random Map Setup presets) can
    // only be chosen here, at creation time, since changing it afterward
    // would invalidate any terrain/units/buildings already placed against
    // the old size.
    std::string new_level_name_;
    int new_level_size_idx_ = 1; // index into kMapSizePresets, default "Normal"

    Field active_field_ = Field::None;
    double last_save_flash_ = -1000.0; // Uint32 ms timestamp of the last save, for a brief "Saved" toast
    Uint32 start_ticks_ = 0;

    // ---- EditMap (battlefield placement) scratch state ----
    EditMapMode edit_map_mode_ = EditMapMode::Units; // top toolbar, see EditMapMode
    MapTab map_tab_ = MapTab::Units; // bottom toolbar sub-choice when edit_map_mode_==Units
    // Vertical scroll offset (px) for the Terrain mode's palette, which has
    // far more entries (~40 decorations plus Erase) than fit in the
    // sidebar at once -- mouse-wheel-scrolled while hovering the palette
    // column, see handle_event's SDL_MOUSEWHEEL case and draw_edit_map.
    int other_scroll_ = 0;
    // Terrain mode's bottom-toolbar sub-tab (Ground/Decoration/Blocks/
    // Objects, see TerrainCategory) -- filters which of kTerrainKinds
    // shows in the palette sidebar. "erase" is always shown regardless,
    // see draw_edit_map's palette loop.
    TerrainCategory terrain_category_ = TerrainCategory::Ground;
    // Grid-alignment overlay, toggled from the global top-strip's grid
    // icon button (see draw_edit_map) -- purely a visual aid, not level
    // data.
    bool show_grid_ = false;
    // Universal eraser tool, toggled from the global top-strip's erase
    // icon button (see draw_edit_map) -- unlike active_terrain_=="erase"
    // before it, this works regardless of edit_map_mode_: while true,
    // canvas clicks/drags erase terrain/units/buildings in a
    // brush_size_ square (see erase_area_at) instead of whatever the
    // current mode would otherwise do, and the bottom toolbar shows the
    // brush-size slider in place of that mode's own controls. Picking a
    // unit/building/terrain kind from a palette turns this back off.
    bool erasing_ = false;
    // Players mode: which lvl->players[] row the bottom toolbar's civ/era/
    // resource controls are currently editing -- independent of
    // active_team_ (Units mode's "who am I placing for" picker), since the
    // two modes' sidebars/pickers serve different purposes even though
    // both index the same players list.
    int players_selected_ = -1;
    // Players mode: which of food/wood/oil/iron (0-3) is currently being
    // typed into (see Field::PlayerResource), -1 = none. players_res_edit_
    // is that field's own scratch text buffer -- kept mirroring the real
    // double value every frame it ISN'T the focused one (see
    // draw_edit_map), so the box always shows the live number when you're
    // not actively typing into it.
    int players_res_field_ = -1;
    std::string players_res_edit_[4];
    // Events mode: index into current_level()->events of the event
    // currently selected for editing -- -1 = none. Same lifecycle as
    // objectives_selected_ below (cleared on mode-switch-away and on
    // "edit_map" (re)open, see those click handlers) and the same create/
    // list/select-to-edit UI (see "create_event"/"select_event"/
    // "delete_event"/"event_type" click handlers).
    int events_selected_ = -1;
    // Events mode: which of a selected "resources"-type event's food/
    // wood/oil/iron (0-3) is currently being typed into (see
    // Field::EventResource), -1 = none. event_res_edit_ is that field's
    // own scratch text buffer, kept mirroring the real double value every
    // frame it ISN'T the focused one -- same pattern as players_res_field_/
    // players_res_edit_ above, just for MapEvent's res_* fields instead of
    // LevelPlayer's.
    int event_res_field_ = -1;
    std::string event_res_edit_[4];
    // Objectives mode: index into current_level()->objectives of the
    // objective currently selected for editing -- -1 = none. Its kill area
    // (see Objective) is the ONLY one ever drawn on the canvas (see
    // draw_edit_map); every other objective's area stays invisible while
    // it isn't the one selected, matching "if the objective is not
    // selected for editing, then the selection is not visible." Reset to
    // -1 both when switching away from Objectives mode (see
    // "edit_map_mode"'s click handler, same as events_selected_) and when
    // the EditMap screen is (re)opened (see "edit_map"'s click handler),
    // so a stale index from a differently-sized objectives list on another
    // level can never be shown.
    int objectives_selected_ = -1;
    // True while dragging out an area rectangle -- shared by Objectives
    // mode's kill_units/move_to_area area tool AND Events mode's gate/
    // dormant area tool, since only one of those two modes is ever active
    // at a time and both draw the exact same "click-drag-release a
    // rectangle on the canvas" interaction. Left-button-down on the canvas
    // (only possible with a selected objective/event whose type actually
    // uses an area, see handle_event) records the anchor tile into
    // area_drag_tx0_/ty0_; every SDL_MOUSEMOTION while this is set updates
    // area_drag_tx1_/ty1_ to the tile under the cursor now (same press-
    // and-hold pattern as terrain_dragging_, just building a rectangle
    // between two points instead of painting under the cursor); mouse-up
    // finalizes into the selected objective's or event's own area_t* (see
    // finalize_objective_area/finalize_event_area, dispatched by
    // edit_map_mode_) and clears this flag.
    bool area_dragging_ = false;
    int area_drag_tx0_ = 0, area_drag_ty0_ = 0; // drag anchor tile (point 1)
    int area_drag_tx1_ = 0, area_drag_ty1_ = 0; // drag's current/end tile (point 2, live while dragging)

    // Trigger-line tool (deprecated, dormant now uses a 2nd box instead -- see
    // area_target_): kept only so the press/release handlers still compile; no
    // button arms it any more, so drawing_trigger_line_ never becomes true.
    bool drawing_trigger_line_ = false; // armed (button pressed)
    bool trig_line_dragging_ = false;   // actively dragging the line out

    // Where the next finalized area-drag rectangle (see area_dragging_) is
    // written for the selected Events-mode event. EventArea (default) = the
    // event's own area_* (gate box / dormant "units" box / spawn box).
    // TriggerArea = a dormant event's area2_* (its tripwire). SightArea =
    // appended to a dormant event's los_areas. The "Draw Trigger Box"/"Add
    // Sight Box" buttons set this; it resets to EventArea after each finalize
    // and whenever the selected event/type changes.
    enum class AreaTarget { EventArea, TriggerArea, SightArea };
    AreaTarget area_target_ = AreaTarget::EventArea;
    // Scratch text buffer for a spawn event's count field (Field::SpawnCount) --
    // mirrors MapEvent::spawn_count when unfocused, parsed back on commit.
    std::string spawn_count_edit_;

    // ---- Lasso move tool (Units mode) ----
    // A rectangular region select + drag-move: toggle the tool on, drag a box
    // over the map to select every non-grass thing inside it (terrain tiles,
    // resources, units, buildings), then press-drag that box to relocate the
    // whole group. On release the group is MOVED (source tiles revert to
    // grass) and stamped at the destination, deleting whatever buildings/
    // terrain were underneath there -- but never the units underneath.
    bool lasso_tool_ = false;      // tool toggled on (Units mode only)
    bool lasso_has_sel_ = false;   // a selection rect currently exists
    int lasso_x0_ = 0, lasso_y0_ = 0, lasso_x1_ = 0, lasso_y1_ = 0; // normalized inclusive tile rect
    bool lasso_marquee_ = false;   // currently drawing the selection box
    bool lasso_moving_ = false;    // currently press-dragging the selection to move it
    int lasso_anchor_tx_ = 0, lasso_anchor_ty_ = 0; // tile the move-drag started on
    int lasso_move_dx_ = 0, lasso_move_dy_ = 0;      // live move offset (tiles)
    int active_team_ = -1;        // index into current_level()->players; -1 = none picked yet
    std::string active_unit_;     // selected palette entry for the Units tab
    std::string active_building_; // selected palette entry for the Buildings tab
    // Selected palette entry for Terrain mode -- one of kTerrainKinds'
    // names, INCLUDING "erase" (see its comment): erasing is just another
    // selectable terrain "kind" now, not a separate tab.
    std::string active_terrain_ = "water";
    int brush_size_ = 1; // brush radius for Terrain mode, 1-5 via the bottom toolbar's slider
    // True while the Terrain mode brush-size slider is being dragged (see
    // kBrushSliderRect/handle_event) -- same press-and-hold pattern as
    // terrain_dragging_ below, just for a UI control instead of the canvas.
    bool brush_slider_dragging_ = false;

    // ---- EditMap's "Map" mode: regenerate the whole level (see
    // generate_map()) -- deletes all existing terrain/units/buildings,
    // resizes the grid, and either fills it from a self-contained random
    // generator or a single uniform "blank" terrain, matching this
    // project's own zero-dependency-on-game constraint (see
    // README) rather than porting the sim's own scenario.cpp generator. ----
    bool map_gen_random_ = true;            // random vs. "blank" (single uniform fill)
    int map_gen_size_idx_ = 1;              // index into kMapSizePresets
    std::string map_gen_type_ = "random";   // random map "map type"
    std::string map_gen_default_ = "grass"; // blank map "default terrain"
    // Regenerates current_level()'s terrain/units/buildings from the
    // map_gen_* scratch state above (see the "Generate" button, EditMap's
    // Map mode) -- players/civs are left untouched, everything battlefield
    // is not.
    void generate_map();

    // Largest per-level grid size the "New Level" size selector offers
    // (Huge, see draw_new_level) -- map_camera_ is always constructed at
    // this size regardless of the CURRENT level's actual (possibly
    // smaller) grid_size, since a Camera's world size is fixed at
    // construction; current_grid_size() is what every tile/footprint/
    // fit-to-view calculation actually bounds itself to, so a smaller
    // level simply never uses the rest of the camera's pannable space.
    static constexpr int kMaxGridSize = 128;

    // Same World<->screen transform + zoom/pan the main game's GameClient
    // uses (render/include/render/camera.h) -- the whole point of reusing
    // it here is so this preview looks and navigates like the real map.
    // Bounds widened from the main game's own [0.4, 2.5] to [0.3, 3.0]:
    // this world is a small fixed tile grid (unlike a scaled random map),
    // so 0.3 is what it takes to fit the WHOLE grid in kMapViewW x
    // kMapViewH at once for an overview, and "edit_map"'s click handler
    // defaults to exactly that fit-to-view zoom every time the screen
    // opens.
    // World size padded by kMapMargin on every side -- see its comment.
    Camera map_camera_{kMapViewW, kMapViewH,
                       static_cast<double>(kMaxGridSize * kEditorTile + 2 * kMapMargin),
                       static_cast<double>(kMaxGridSize * kEditorTile + 2 * kMapMargin)};
    bool map_dragging_ = false;
    SDL_Point map_drag_last_{0, 0};
    // Press-and-hold painting in Terrain mode: true while the left button
    // is held down starting from inside the canvas: each SDL_MOUSEMOTION
    // while this is set paints (or, while erasing_, erases) the tile
    // under the cursor, same as the initial click, instead of requiring
    // one click per tile.
    bool terrain_dragging_ = false;

    struct HitRect {
        SDL_Rect rect;
        std::string id;
        int arg = 0;
    };
    std::vector<HitRect> hit_rects_;

    void handle_click(int mx, int my);
    // Universal eraser: removes whatever occupies the clicked tile
    // (terrain feature, unit, or building) regardless of the active tab.
    void handle_right_click(int mx, int my);
    void handle_key(SDL_Keycode key);

    // Screen (window) coords -> grid tile, clamped into range; returns
    // false if (mx,my) isn't within the canvas viewport at all. Shared by
    // handle_click/handle_right_click/the press-and-hold drag path so
    // every input path agrees on the same screen<->tile math.
    bool canvas_tile_at(int mx, int my, int& tx, int& ty);
    // Terrain mode: paints (or, for water/land-resource mismatches, or a
    // tile a unit/building already occupies, skips) tile (tx,ty) with
    // active_terrain_, applying brush_size_ (unless
    // terrain_kind_ignores_brush) -- called from both a single click and
    // every motion event of a press-and-hold drag. Never called while
    // erasing_ -- callers route to erase_area_at instead.
    void paint_terrain_at(int tx, int ty);
    // Universal eraser (see erasing_): removes whatever occupies every
    // tile in a brush_size_ square centred on (tx,ty) -- terrain
    // feature, unit, or building, same per-tile rule as
    // handle_right_click, just brushable over an area instead of one tile
    // per click.
    void erase_area_at(int tx, int ty);
    // True if a unit or (any part of) a building's footprint already
    // occupies (tx,ty) -- used to keep paint_terrain_at's decoration/
    // resource kinds (anything not TerrainKind::is_base) from being
    // painted on top of something already standing there; ground/base
    // kinds are exempt (see paint_terrain_at), matching how they've
    // always been paintable under an existing unit.
    bool tile_has_entity(const ww::campaign::Level& lvl, int tx, int ty) const;
    // Units/Buildings tab: places active_unit_/active_building_ owned by
    // active_team_ at (tx,ty) if -- and only if -- footprint_clear.
    void place_entity_at(int tx, int ty);
    // Lasso move tool (see lasso_tool_): commit the current selection's move
    // by (lasso_move_dx_, lasso_move_dy_) tiles -- relocates the selected
    // terrain/units/buildings, clears their source tiles, and deletes any
    // buildings/terrain (never units) sitting under the destination.
    void commit_lasso_move();
    // Briefing image (Objectives mode): open a native file dialog, copy the
    // chosen PNG into asset_dir_/campaign_images/, and store its relative path
    // on the current level (Level::briefing_image).
    void upload_briefing_image();
    // Events mode: canvas click while a "message"-type event is selected
    // (events_selected_) moves that event's tx/ty to the clicked tile --
    // this is how a message gets placed the first time (it starts at
    // tx=-1, "not placed yet") and repositioned afterward. A no-op if
    // nothing is selected or the selected event isn't a message (gate/
    // dormant use the drag-an-area tool instead, see area_dragging_).
    void reposition_selected_message_at(int tx, int ty);
    // Called when an area drag (see area_dragging_) ends while
    // edit_map_mode_ == Objectives -- normalizes the drag's two corner
    // tiles into the selected objective's area_tx/ty/tw/th, then, for
    // kill_units only, re-captures target_unit_ids from scratch as every
    // current lvl->units entry whose (tx,ty) falls inside that rectangle
    // (lazily assigning a PlacedEntity id via next_entity_id to any that
    // doesn't have one yet). A no-op if no objective is selected.
    void finalize_objective_area();
    // Called when an area drag ends while edit_map_mode_ == Events --
    // same idea as finalize_objective_area but for the selected event's
    // (events_selected_) own area_tx/ty/tw/th (gate/dormant only -- a
    // no-op for a message-type event, which uses
    // reposition_selected_message_at instead). No unit snapshot: a gate/
    // dormant area is a live query at play time (see MapEvent's
    // comment), so there's nothing else to capture here.
    void finalize_event_area();
    // True if EVERY tile in the wt x ht footprint anchored at (tx,ty) is
    // in-bounds and isn't a unit/another building's footprint, AND its
    // terrain matches what `is_ship` needs: a ship requires water on
    // every tile, anything else is blocked by ANY terrain feature
    // (water or resource) same as before -- the "can't overlap anything,
    // and ships must be in the water" rule.
    bool footprint_clear(const ww::campaign::Level& lvl, int tx, int ty, int wt, int ht,
                         bool is_ship = false) const;
    // catalog.json's per-unit "ship" flag (destroyer/battleship/etc, see
    // game_data.cpp's shipyard roster) -- true units may only be placed on
    // water, and are spared when paint_terrain_at (re)paints water under
    // them (see its "terrain always wins" comment).
    bool is_ship(const std::string& unit) const;

    void draw_campaign_list(SDL_Renderer* renderer);
    void draw_new_campaign(SDL_Renderer* renderer);
    void draw_level_list(SDL_Renderer* renderer);
    // Name + map-size picker shown before a level is actually created --
    // see new_level_name_/new_level_size_idx_.
    void draw_new_level(SDL_Renderer* renderer);
    void draw_edit_level(SDL_Renderer* renderer);
    // Battlefield placement screen: left sidebar (tabs + palette), a team
    // selector or brush-size selector depending on the active tab, and the
    // current level's grid_size x grid_size grass grid itself.
    void draw_edit_map(SDL_Renderer* renderer);
    // The open level's grid_size, or kLevelGridSize if no level is open --
    // every tile/footprint/fit-to-view bound in EditMap goes through this
    // rather than the old fixed kLevelGridSize constant, now that map size
    // is chosen per-level (see draw_new_level).
    int current_grid_size();
    void draw_frame_chrome(SDL_Renderer* renderer, bool show_back);
    // Draws current_level()'s terrain (as coloured dots) plus its units/
    // buildings (as team-coloured dots), scaled to fill `rect` -- just the
    // CONTENTS, not a background/border/click-to-jump (callers add those
    // themselves, since EditLevel's minimap is a plain static preview
    // while EditMap's is also interactive -- see draw_edit_map's extra
    // viewport-rect overlay and "minimap_click" handling layered on top
    // there). Shared so both screens' minimaps agree on what a level
    // "looks like" at a glance.
    void draw_minimap_contents(SDL_Renderer* renderer, const SDL_Rect& rect);

    // game_data.h's civ_has(...) plus its civ-only-unit sets (CAMEL_UNITS/
    // OTTOMAN_ONLY/CIV_ONLY_UNITS), WITHOUT era/tech gating -- an editor
    // palette should offer a civ's whole roster regardless of era, since
    // the author is hand-placing starting units, not training them.
    std::vector<std::string> palette_units(int civ) const;
    std::vector<std::string> palette_buildings(int civ) const;
    // Catalog icon_sprite lookup, same fallback convention as the main
    // engine's GameClient::item_icon. Used for the small palette list.
    std::string item_icon(const std::string& item) const;
    // Catalog sprite_index lookup -- the actual battlefield sprite (not
    // the UI icon), used to preview a placed unit/building on the canvas
    // the way it would really look in a match. `civ` only matters for
    // "base" (town center), whose art is civ-specific; `era` only matters
    // for "house" (an editor-only re-skin-by-era convention, see the .cpp)
    // -- see the .cpp.
    std::string world_sprite(const std::string& item, int civ, int era = 0) const;

    void draw_panel(SDL_Renderer* renderer, const SDL_Rect& r);
    void draw_button(SDL_Renderer* renderer, const SDL_Rect& r, const std::string& label,
                     const std::string& id, int arg = 0, SDL_Color border = {255, 255, 255, 255});
    // Draws a text-input box; `field` identifies which scratch string this
    // is so clicking it focuses the right one (see active_field_).
    void draw_text_field(SDL_Renderer* renderer, const SDL_Rect& r, std::string& value, Field field,
                         const std::string& placeholder);
    // Same as draw_text_field but multi-line: word-wraps `value` to fit
    // `r`'s width and Enter inserts a newline instead of committing (see
    // handle_key) -- used for Level::description ("up to a paragraph").
    void draw_text_area(SDL_Renderer* renderer, const SDL_Rect& r, std::string& value, Field field,
                        const std::string& placeholder);
    std::string* active_field_string();
    // Per-field cap on active_field_string()'s length -- SIZE_MAX (no
    // cap) for every field except MessageText (100, per the user's
    // explicit limit) and PlayerResource (6 digits, plenty for any
    // realistic stockpile).
    size_t active_field_max_len() const;
    // Writes whatever's been typed into a scratch/editable field back into
    // the real data it represents, called right before active_field_ is
    // cleared (Enter/Tab/Escape/click-away -- see handle_key/handle_click).
    // A no-op for every field except PlayerResource, whose scratch text
    // (players_res_edit_) isn't the real data itself (unlike every other
    // field, which edits a std::string in place) -- it has to be parsed
    // back into the actual double field on commit.
    void commit_active_field();

    void save_current_campaign();
    ww::campaign::Campaign* current_campaign();
    ww::campaign::Level* current_level();

    SDL_Point mouse_pos_{0, 0};
};
