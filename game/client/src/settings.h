#pragma once
#include <SDL.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

// Composite key for Settings::completed_campaign_levels -- Level::id alone
// isn't globally unique (each campaign independently generates its own
// lvl_1, lvl_2, ... sequence, see campaign_data.cpp's next_level_id), so
// completion has to be keyed by campaign name + level id together. Shared
// by GameClient (writes on a campaign-level win) and MenuController (reads
// to decide which level markers are locked) so both sides can never
// disagree on the key format.
inline std::string campaign_level_key(const std::string& campaign_name, const std::string& level_id) {
    return campaign_name + "|" + level_id;
}

// The virtual canvas size the game renders at (see app.cpp's WIDTH/HEIGHT
// and its letterboxing comment) -- index 0 is the original, smallest size
// and always stays the default. Shared between app.cpp (picks WIDTH/HEIGHT
// at startup from Settings::resolution_index) and MenuController (the
// Graphics options screen that lets the player choose one), so both sides
// read the exact same numbers/labels. A plain array, not a registry: every
// consumer just needs to clamp against and loop over it, both trivial with
// std::size().
struct Resolution { int w; int h; const char* label; };
constexpr Resolution kResolutions[2] = {
    {640, 480, "Small (640x480)"},
    {832, 624, "Standard (832x624)"},
};

// A keybinding is a base key plus whether Ctrl must be held with it --
// NOT hardcoded per-field anymore (building_keys used to always imply
// Ctrl, everything else was always bare) so the player can put a Ctrl
// combo on ANY rebindable slot, or take Ctrl off a building's map-select
// key, by simply holding (or not holding) Ctrl during the rebind gesture
// itself (see MenuController::handle_event's press-and-release capture).
struct Hotkey {
    SDL_Keycode key = SDLK_UNKNOWN;
    bool ctrl = false;
};
inline bool operator==(const Hotkey& a, const Hotkey& b) { return a.key == b.key && a.ctrl == b.ctrl; }
inline bool operator!=(const Hotkey& a, const Hotkey& b) { return !(a == b); }

// Persistent, player-editable settings -- hotkey bindings today, and a
// home for future persistent progression (unlocked civs, etc, per the
// project's own roadmap) later. Lives in a plain, human-readable JSON
// file at %LOCALAPPDATA%\WorldWar\settings.json -- a sibling of, but
// OUTSIDE, the launcher-managed game\ folder, so updating the game never
// touches or wipes it.
//
// Deliberately NOT obfuscated or signed: this is a casual game played
// with friends, not a competitive/ranked one, so a player editing their
// own local file doesn't affect anyone else -- real anti-tamper effort
// would be complexity bought for essentially no benefit here. A JSON
// parse failure (missing file, hand-edited into invalid JSON, etc) just
// silently keeps whatever defaults were already in memory rather than
// crashing.
class Settings {
public:
    // Building-select hotkeys (cycles through/selects one of the player's
    // own buildings of that type, Shift+<key> selects all -- see
    // GameClient::handle_hotkey), keyed by catalog building name. Defaults
    // to a Ctrl+<letter> Hotkey, but that's just a default -- the player
    // can rebind any entry to drop Ctrl (or use a different modifier state)
    // like any other slot. A vector (not a map) so the options screen
    // always lists them in the same deliberate order.
    std::vector<std::pair<std::string, Hotkey>> building_keys;
    // "." cycles through idle villagers; Shift+<this key> selects all of
    // them (see GameClient::handle_hotkey).
    Hotkey idle_villager_key;
    // Continuous camera pan (app.cpp's per-frame scancode poll, via
    // GameClient::settings()) -- polled every frame a key is held, unlike
    // everything else here which fires once on keydown. Defaults to the
    // arrow keys (not WASD) specifically so W/A/S/D are free for the Grid
    // preset's command-card letters.
    Hotkey pan_up_key, pan_down_key, pan_left_key, pan_right_key;
    // Dedicated, non-positional action keys -- looked up by CardButton
    // ::kind, independent of whatever grid position that kind's button
    // happens to occupy. Defaults differ per preset (see apply_preset):
    // Grid points build_eco/build_military/attack_move at the SAME keys
    // their card buttons already sit under (Q/W/A) so nothing changes for
    // a Grid player; Classic points them at the AoE-style B/V/Q instead.
    // The 4 formation-shape keys ALSO vary by preset now (Grid: S/D/F/G,
    // matching their command-card slots just right of attack_move at
    // (1,1)/(2,1)/(3,1)/(4,1); Classic: real AoE2's own formation hotkeys,
    // plus a made-up one for Split which AoE2 has no equivalent of). The
    // remaining five don't vary by preset -- land/unload/shipyard_page/
    // build_nuke/build_back aren't part of what Grid and Classic actually
    // differ on, they just carry forward their old fixed command-card slot
    // as a sensible default (see default_dedicated_keys in settings.cpp).
    Hotkey build_eco_key, build_military_key, attack_move_key;
    // Column ("row" formation -- single file), Box, Staggered, and Split --
    // see GameClient::formation_type_ and the "formation_column"/"formation_
    // box"/"formation_stagger"/"formation_split" CardButton kinds
    // (game_client.cpp) these drive directly (no cycling anymore -- each
    // button/key sets its own shape straight away).
    Hotkey formation_column_key, formation_box_key, formation_stagger_key, formation_split_key;
    Hotkey land_key, unload_key;
    Hotkey shipyard_page_key, build_nuke_key, build_back_key;
    // Construction hotkeys -- constructs a new building while a build
    // category list is open (Villager Commands on the Hotkeys screen),
    // keyed by catalog building name. Deliberately a SEPARATE binding from
    // building_keys' map-select key. DOES vary by preset (see
    // default_construction_keys_grid/_classic in settings.cpp): Grid
    // defaults to the building's own slot in GameClient::eco_building_
    // layout/military_building_layout (Q/W/E/R/T row0, A/S/D/F/G row1);
    // Classic defaults to a mnemonic first-letter key instead, taking
    // inspiration from Age of Empires II's own villager build hotkeys
    // where this roster has an obvious equivalent. Neither uses Ctrl.
    std::vector<std::pair<std::string, Hotkey>> construction_keys;
    // Per-item train/tech/trade hotkeys -- keyed by ww::hotkeys::
    // item_key_id(kind, item) (e.g. "train|fishing boat",
    // "tech|hydrodynamics", "trade|buy food"), fixed to the item's own
    // catalog identity rather than whatever card slot it happens to render
    // into this frame -- e.g. "fishing boat" is always its own key at the
    // shipyard regardless of research order or which shipyard page is
    // showing. A vector (not a map), same reasoning as building_keys: the
    // options screen always lists every item in the same deliberate order
    // (see ww::hotkeys::building_groups(), item_hotkeys.h). DOES vary by
    // preset (see default_item_keys_grid/_classic in settings.cpp): Grid
    // is QWERTASDFG-by-position, Classic is hand-picked mnemonic letters
    // per building, AoE2-inspired where this roster has an equivalent.
    std::vector<std::pair<std::string, Hotkey>> item_keys;
    // campaign_level_key(campaign name, Level::id) for every campaign
    // level the player has won -- see GameClient's game-over handling
    // (where this is written) and MenuController::draw_campaign_map
    // (where it's read to decide which level markers are locked). Empty
    // by default; every campaign's level 1 is always unlocked regardless
    // of this set (see draw_campaign_map).
    std::set<std::string> completed_campaign_levels;

    // Index into kResolutions above -- 0 (640x480) unless the player has
    // picked something else on the Graphics options screen. Read at
    // startup (app.cpp) to size the initial window/canvas, AND applied
    // live the moment it's clicked (see MenuController::wants_resize) --
    // this field is what makes the choice persist to the NEXT launch too.
    int resolution_index = 0;

    // Options > Audio sliders, 0.0-1.0. Defaults match Audio's own (see
    // audio.h's sfx_volume_/music_volume_) so a player who's never opened
    // that screen hears exactly what they always did. Read once at startup
    // (app.cpp, right after constructing the app's single long-lived
    // Audio) and applied live the moment either slider is clicked
    // (MenuController's audio_ pointer) -- same two-part pattern
    // resolution_index uses (persisted field + live-applied side effect).
    double sfx_volume = 0.5;
    double music_volume = 0.35;

    Settings(); // seeds every field with today's hardcoded defaults

    // Merges in whatever settings.json has; a missing file, an unreadable
    // one, or one missing/misnaming individual keys all just leave the
    // corresponding field(s) at their already-seeded default.
    void load();
    void save() const;

    Hotkey building_key(const std::string& name) const;
    void set_building_key(const std::string& name, Hotkey hk);

    Hotkey construction_key(const std::string& name) const;
    void set_construction_key(const std::string& name, Hotkey hk);

    // Bulk-overwrites EVERY rebindable field (building_keys,
    // construction_keys, item_keys, idle_villager_key, pan keys, and all 9
    // dedicated action keys) with one named preset's full defaults -- "grid"
    // or "classic" (anything else is a no-op). Deliberately a full reset,
    // not a merge: the player asked for "clicking a preset overwrites
    // everything, then I can still change something specific afterward" --
    // individual rebinding after applying a preset works exactly like it
    // does today.
    void apply_preset(const std::string& preset);

    Hotkey item_key(const std::string& kind, const std::string& item) const;
    void set_item_key(const std::string& kind, const std::string& item, Hotkey hk);

    static std::string file_path(); // %LOCALAPPDATA%\WorldWar\settings.json
    // Human-readable key name for the options UI and the JSON file alike
    // (e.g. "Q", "Period", "F1") -- thin wrapper over SDL_GetKeyName so
    // both call sites (and the round-trip through SDL_GetKeyFromName on
    // load) always agree on spelling.
    static std::string key_label(SDL_Keycode key);
    // Same, but with a "Ctrl+" prefix when hk.ctrl is set -- what the
    // Hotkeys screen and in-game tooltips actually display.
    static std::string key_label(Hotkey hk);
};
