#pragma once
#include <string>
#include <utility>
#include <vector>

// Every trainable unit and researchable tech in the game, grouped by the
// building that offers it, in a fixed hand-picked order -- the single
// source of truth for both Settings' default per-item hotkeys (settings.cpp,
// see default_item_keys) and the Hotkeys options screen's full per-building
// listing (menu_controller.cpp's draw_hotkeys_options). Mirrors
// ww::sim::PRODUCTION (sim/control.cpp) and data/building_techs.json's
// per-building rosters -- duplicated here rather than derived from them at
// runtime, matching Settings' existing "seed hardcoded defaults with no
// file I/O" contract (see Settings::Settings()). Update this table alongside
// those two if the roster of trainable units/techs ever changes.
namespace ww::hotkeys {

struct ItemEntry {
    std::string kind; // "train" | "tech" | "trade"
    std::string item; // canonical/representative catalog name, e.g. "muscateer"
    // Other catalog names of the SAME kind that occupy this exact command-
    // card slot at a different point in the game (an upgrade-chain tier,
    // e.g. "rifleman"/"infantryman", or a civ substitution, e.g. "tiger
    // tank") -- see game_client.cpp's PRODUCTION/UPGRADE_MAP-driven unit
    // substitution and tech_parent_units' "an individual-unit upgrade sits
    // directly below the unit it improves" placement. These never render
    // simultaneously with `item` or each other, so they all share ONE
    // hotkey (canonical_item resolves any of them back to `item`) rather
    // than each getting its own default like a genuinely separate button
    // would. Empty for items that don't have one.
    std::vector<std::string> aliases;
};

struct BuildingGroup {
    std::string building; // catalog building name (matches Settings::building_keys entries)
    std::vector<ItemEntry> items; // in default-hotkey assignment order (Q,W,E,R,T,A,S,D,F,G,Y,U,I,...)
};

// One group per producing building, in the same order the Hotkeys screen's
// existing "Buildings" section lists them (Settings::default_building_keys).
// Age-up (industrial/war/scientific, aliased under canonical "industrial" --
// only one is ever offered at a time, driven by the team's current era, see
// draw_command_card's show_age_up) is a normal rebindable base item like any
// other -- Grid pins it to A via an explicit override (see
// default_item_keys_grid) since its position in this list wouldn't
// otherwise land there; Classic just picks A directly, same as any other
// hand-picked default.
const std::vector<BuildingGroup>& building_groups();

// Composite id used as the key into Settings::item_keys, e.g.
// "train|fishing boat" -- shared by Settings (default generation + load/
// save) and GameClient (runtime lookup) so both always agree on the format.
std::string item_key_id(const std::string& kind, const std::string& item);

// Resolves `item` to whichever building_groups() entry's `item` or
// `aliases` it matches (same `kind`), or returns it unchanged if it isn't
// an alias of anything (covers both standalone items and canonical roots
// themselves). Settings::item_key/set_item_key call this before touching
// item_keys, so e.g. a CardButton showing "rifleman" this frame (once
// researched past muscateer) still resolves to the SAME binding as
// "muscateer" did.
std::string canonical_item(const std::string& kind, const std::string& item);

} // namespace ww::hotkeys
