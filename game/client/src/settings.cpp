#include "settings.h"

#include "item_hotkeys.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace {
// Order here is the order the Hotkeys options screen lists them in.
const std::vector<std::pair<std::string, Hotkey>>& default_building_keys() {
    static const std::vector<std::pair<std::string, Hotkey>> v = {
        {"base", {SDLK_h, true}},       {"barracks", {SDLK_b, true}},   {"academy", {SDLK_l, true}},
        {"factory", {SDLK_k, true}},    {"fortress", {SDLK_v, true}},   {"market", {SDLK_m, true}},
        {"refinery", {SDLK_s, true}},   {"university", {SDLK_u, true}}, {"shipyard", {SDLK_d, true}},
        {"airbase", {SDLK_a, true}},
    };
    return v;
}

// Construction hotkeys (Villager Commands), Grid preset: default key = the
// building's own slot in GameClient::eco_building_layout/military_
// building_layout (QWERT row0, ASDFG row1) -- duplicated here rather than
// shared with game_client.cpp's private layout tables, same "hardcoded, no
// I/O" contract as default_building_keys above. Eco and military buildings
// reusing the same letters (e.g. "house" and "barracks" both default to Q)
// is fine -- their build-category lists never show at once.
const std::vector<std::pair<std::string, Hotkey>>& default_construction_keys_grid() {
    static const std::vector<std::pair<std::string, Hotkey>> v = {
        // Eco building list.
        {"house", {SDLK_q}}, {"farm", {SDLK_w}}, {"refinery", {SDLK_e}}, {"market", {SDLK_r}},
        {"university", {SDLK_a}}, {"nuclear reactor", {SDLK_s}}, {"base", {SDLK_d}},
        // Military building list.
        {"barracks", {SDLK_q}}, {"academy", {SDLK_w}}, {"shipyard", {SDLK_e}}, {"tower", {SDLK_r}},
        {"outpost", {SDLK_t}}, {"factory", {SDLK_a}}, {"airbase", {SDLK_s}}, {"fortress", {SDLK_d}},
        {"palisade", {SDLK_f}},
    };
    return v;
}

// Construction hotkeys, Classic preset: mnemonic first-letter-of-the-name
// keys instead of grid position, in the spirit of Age of Empires II's own
// villager build hotkeys (e.g. AoC/FE's House = E, kept here too) -- the
// rest are this project's own roster, so there's no direct AoE2 equivalent
// to copy for most of them; picked for a memorable first letter instead,
// same as AoE2 itself does. Eco/military letters can repeat (see
// default_construction_keys_grid's comment) but must stay distinct WITHIN
// each category -- Settings::construction_key_conflicts checks exactly that.
const std::vector<std::pair<std::string, Hotkey>>& default_construction_keys_classic() {
    static const std::vector<std::pair<std::string, Hotkey>> v = {
        // Eco building list.
        {"house", {SDLK_e}}, {"farm", {SDLK_f}}, {"refinery", {SDLK_r}}, {"market", {SDLK_m}},
        {"university", {SDLK_u}}, {"nuclear reactor", {SDLK_n}}, {"base", {SDLK_t}},
        // Military building list.
        {"barracks", {SDLK_b}}, {"academy", {SDLK_c}}, {"shipyard", {SDLK_d}}, {"tower", {SDLK_w}},
        {"outpost", {SDLK_o}}, {"factory", {SDLK_f}}, {"airbase", {SDLK_a}}, {"fortress", {SDLK_r}},
        {"palisade", {SDLK_l}},
    };
    return v;
}

const std::vector<std::pair<std::string, Hotkey>>& default_construction_keys(const std::string& preset) {
    return preset == "classic" ? default_construction_keys_classic() : default_construction_keys_grid();
}

// One default key per item, Grid preset: assigned in ww::hotkeys::
// building_groups()'s order within each group -- QWERTASDFG first
// (matching the command card's own QWERT/ASDFG rows), continuing into
// YUIOP for the two buildings (shipyard, airbase) whose full unit+tech
// roster runs past 10. Computed once from the shared roster table rather
// than hand-listed here, so the two can never drift apart.
const std::vector<std::pair<std::string, Hotkey>>& default_item_keys_grid() {
    static const std::vector<std::pair<std::string, Hotkey>> v = [] {
        static const SDL_Keycode kPool[] = {
            SDLK_q, SDLK_w, SDLK_e, SDLK_r, SDLK_t,
            SDLK_a, SDLK_s, SDLK_d, SDLK_f, SDLK_g,
            SDLK_y, SDLK_u, SDLK_i, SDLK_o, SDLK_p,
        };
        // Age-up is pinned to A regardless of where it falls in base's item
        // list (positionally it'd land on W) -- it's the single most
        // reached-for building action in the game, so both presets give it
        // the same memorable key (see default_item_keys_classic's own
        // explicit "A" pick) rather than leaving Grid's placement to
        // whatever the general QWERTASDFG sequence happens to land on.
        static const std::unordered_map<std::string, SDL_Keycode> kOverrides = {
            {ww::hotkeys::item_key_id("tech", "industrial"), SDLK_a},
        };
        std::vector<std::pair<std::string, Hotkey>> out;
        for (auto& group : ww::hotkeys::building_groups()) {
            for (size_t i = 0; i < group.items.size(); ++i) {
                std::string id = ww::hotkeys::item_key_id(group.items[i].kind, group.items[i].item);
                auto ov = kOverrides.find(id);
                SDL_Keycode k = ov != kOverrides.end() ? ov->second
                                : i < std::size(kPool)  ? kPool[i]
                                                         : SDLK_UNKNOWN;
                out.push_back({id, {k, false}});
            }
        }
        return out;
    }();
    return v;
}

// One default key per item, Classic preset: hand-picked, mnemonic-first-
// letter keys per building group, taking inspiration from Age of Empires
// II's own per-building unit/tech hotkeys (aokhotkeys.appspot.com) where
// this roster has an obvious equivalent -- e.g. the barracks' muscateer/
// rifleman/infantryman line (this game's basic melee-into-ranged infantry
// upgrade chain) takes AoE2's Militia-line key, S. Most of this roster has
// no real AoE2 counterpart (different game entirely), so those are just
// this project's own memorable first-letter picks. Must stay distinct
// WITHIN each building's own group (see item_key_conflicts) -- reuse
// across DIFFERENT groups is fine, their command cards never show at once.
const std::vector<std::pair<std::string, Hotkey>>& default_item_keys_classic() {
    using ww::hotkeys::item_key_id;
    static const std::vector<std::pair<std::string, Hotkey>> v = {
        // base
        {item_key_id("train", "civilian"), {SDLK_c}},
        {item_key_id("tech", "industrial"), {SDLK_a}}, // age up
        {item_key_id("tech", "uniform"), {SDLK_u}},
        // barracks
        {item_key_id("train", "muscateer"), {SDLK_s}},
        {item_key_id("train", "artillery1"), {SDLK_c}},
        {item_key_id("tech", "rifleman upgrade"), {SDLK_u}},
        {item_key_id("tech", "artillery upgrade"), {SDLK_a}},
        {item_key_id("tech", "bolt action rifle"), {SDLK_f}},
        // academy
        {item_key_id("train", "swordsman"), {SDLK_s}},
        {item_key_id("train", "cavalry"), {SDLK_c}},
        {item_key_id("train", "camel"), {SDLK_m}},
        {item_key_id("tech", "swordsman2 upgrade"), {SDLK_u}},
        {item_key_id("tech", "cavalry2 upgrade"), {SDLK_h}},
        {item_key_id("tech", "camel corps upgrade"), {SDLK_p}},
        // factory
        {item_key_id("train", "light tank"), {SDLK_l}},
        {item_key_id("train", "tank"), {SDLK_t}},
        {item_key_id("train", "aa gun"), {SDLK_g}},
        {item_key_id("tech", "blowback reload"), {SDLK_b}},
        {item_key_id("tech", "diesel engine"), {SDLK_d}},
        {item_key_id("tech", "heavy tank"), {SDLK_h}},
        // fortress
        {item_key_id("train", "waffen"), {SDLK_w}},
        {item_key_id("train", "janissary"), {SDLK_j}},
        // market
        {item_key_id("tech", "trade agreement"), {SDLK_t}},
        {item_key_id("tech", "fertilizer"), {SDLK_f}},
        {item_key_id("tech", "horse wagon"), {SDLK_h}},
        {item_key_id("tech", "irrigation"), {SDLK_i}},
        {item_key_id("tech", "mobile sawmill"), {SDLK_m}},
        {item_key_id("tech", "pesticide"), {SDLK_p}},
        {item_key_id("tech", "power saw"), {SDLK_s}},
        {item_key_id("trade", "buy food"), {SDLK_q}},
        {item_key_id("trade", "buy wood"), {SDLK_w}},
        {item_key_id("trade", "buy iron"), {SDLK_e}},
        {item_key_id("trade", "sell food"), {SDLK_a}},
        {item_key_id("trade", "sell wood"), {SDLK_d}},
        {item_key_id("trade", "sell iron"), {SDLK_g}},
        // refinery
        {item_key_id("tech", "alloys"), {SDLK_a}},
        {item_key_id("tech", "electric arc furnace"), {SDLK_e}},
        {item_key_id("tech", "refined steel"), {SDLK_s}},
        // university
        {item_key_id("tech", "ballistics"), {SDLK_b}},
        {item_key_id("tech", "beneficiation"), {SDLK_n}},
        {item_key_id("tech", "electric drill"), {SDLK_d}},
        {item_key_id("tech", "fracking"), {SDLK_f}},
        {item_key_id("tech", "gasoline"), {SDLK_g}},
        {item_key_id("tech", "jet engine"), {SDLK_j}},
        {item_key_id("tech", "smelting"), {SDLK_s}},
        {item_key_id("tech", "steel frame"), {SDLK_r}},
        // Synthetic Fuel shares Gasoline's slot/hotkey now (see
        // item_hotkeys.cpp's "university" group) -- no separate entry.
        {item_key_id("tech", "nuclear physics"), {SDLK_p}},
        {item_key_id("tech", "flak tower upgrade"), {SDLK_u}},
        // shipyard
        {item_key_id("train", "fishing boat"), {SDLK_f}},
        {item_key_id("train", "transport ship"), {SDLK_t}},
        {item_key_id("train", "frigate"), {SDLK_g}},
        {item_key_id("train", "battleship"), {SDLK_b}},
        {item_key_id("train", "yamato"), {SDLK_y}},
        {item_key_id("tech", "torpedo boat upgrade"), {SDLK_u}},
        {item_key_id("tech", "battleship upgrade"), {SDLK_a}},
        {item_key_id("tech", "hydrodynamics"), {SDLK_h}},
        {item_key_id("tech", "naval armour"), {SDLK_n}},
        // airbase
        {item_key_id("train", "biplane"), {SDLK_f}},
        {item_key_id("train", "bomber"), {SDLK_b}},
        {item_key_id("train", "b29"), {SDLK_9}},
        {item_key_id("train", "ohka"), {SDLK_o}},
        {item_key_id("tech", "composite plane armor"), {SDLK_c}},
        {item_key_id("tech", "fighter upgrade"), {SDLK_u}},
        {item_key_id("tech", "heavy bomber upgrade"), {SDLK_h}},
        {item_key_id("tech", "steel plane armor"), {SDLK_s}},
        {item_key_id("tech", "jet fighter upgrade"), {SDLK_j}},
        {item_key_id("tech", "atomic bomb"), {SDLK_a}},
    };
    return v;
}

const std::vector<std::pair<std::string, Hotkey>>& default_item_keys(const std::string& preset) {
    return preset == "classic" ? default_item_keys_classic() : default_item_keys_grid();
}

// "Ctrl+B" -> {SDLK_b, true}; "B" -> {SDLK_b, false}. Inverse of
// Settings::key_label(Hotkey). An unrecognized base key name returns
// SDLK_UNKNOWN (caller checks and ignores, same as every other malformed-
// settings-file case).
Hotkey hotkey_from_label(const std::string& s) {
    static const std::string kPrefix = "Ctrl+";
    bool ctrl = s.rfind(kPrefix, 0) == 0;
    std::string rest = ctrl ? s.substr(kPrefix.size()) : s;
    return {SDL_GetKeyFromName(rest.c_str()), ctrl};
}

void read_hotkey(const nlohmann::json& j, const char* field, Hotkey& out) {
    if (!j.contains(field) || !j[field].is_string()) return;
    Hotkey hk = hotkey_from_label(j[field].get<std::string>());
    if (hk.key != SDLK_UNKNOWN) out = hk;
}
} // namespace

Settings::Settings() {
    building_keys = default_building_keys();
    construction_keys = default_construction_keys("grid");
    item_keys = default_item_keys("grid");
    idle_villager_key = {SDLK_PERIOD};
    pan_up_key = {SDLK_UP};
    pan_down_key = {SDLK_DOWN};
    pan_left_key = {SDLK_LEFT};
    pan_right_key = {SDLK_RIGHT};
    build_eco_key = {SDLK_q};
    build_military_key = {SDLK_w};
    attack_move_key = {SDLK_a};
    // Grid default: matches the buttons' own command-card slots, right of
    // attack_move (0,1) -- (1,1)/(2,1)/(3,1)/(4,1) = ASDFG's S/D/F/G.
    formation_column_key = {SDLK_s};
    formation_box_key = {SDLK_d};
    formation_stagger_key = {SDLK_f};
    formation_split_key = {SDLK_g};
    // Carried forward from each button's old fixed command-card slot, back
    // when these were positional (see the removed grid_keys) -- not part of
    // what Grid/Classic actually differ on, so apply_preset resets them to
    // these same values regardless of which preset was picked.
    land_key = {SDLK_s};           // was col 1, row 1
    unload_key = {SDLK_w};         // was col 1, row 0 (never shown alongside build_military, which also
                                    // defaults to W -- unload only appears with a laden transport selected)
    shipyard_page_key = {SDLK_g};  // was col 4, row 1
    build_nuke_key = {SDLK_g};     // was col 4, row 1 (airbase-only, never shown alongside shipyard_page)
    build_back_key = {SDLK_g};     // was col 4, row 1 (only shown with a build category open)
}

void Settings::apply_preset(const std::string& preset) {
    building_keys = default_building_keys();
    construction_keys = default_construction_keys(preset);
    item_keys = default_item_keys(preset);
    idle_villager_key = {SDLK_PERIOD};
    pan_up_key = {SDLK_UP};
    pan_down_key = {SDLK_DOWN};
    pan_left_key = {SDLK_LEFT};
    pan_right_key = {SDLK_RIGHT};
    land_key = {SDLK_s};
    unload_key = {SDLK_w};
    shipyard_page_key = {SDLK_g};
    build_nuke_key = {SDLK_g};
    build_back_key = {SDLK_g};
    if (preset == "classic") {
        // AoE-style: select a villager, then B (economic) or V (military)
        // opens that build category, then the building's own construction
        // key (default_construction_keys_classic) constructs it -- see
        // GameClient::handle_hotkey's building_category_ mnemonic check.
        // Q is freed up from "build" to be a dedicated Attack Move key
        // instead, matching real AoE2.
        build_eco_key = {SDLK_b};
        build_military_key = {SDLK_v};
        attack_move_key = {SDLK_q};
        // Real AoE2's own formation hotkeys (comma/slash for Line/Box;
        // semicolon picked for Staggered, quote for Split, to avoid
        // colliding with idle_villager_key's period default above, since
        // AoE2 has no equivalent of either to check against).
        formation_column_key = {SDLK_COMMA};   // Line formation in AoE2
        formation_box_key = {SDLK_SLASH};
        formation_stagger_key = {SDLK_SEMICOLON};
        formation_split_key = {SDLK_QUOTE};
    } else { // "grid" (and any unrecognized name -- fail safe to the game's actual default)
        build_eco_key = {SDLK_q};
        build_military_key = {SDLK_w};
        attack_move_key = {SDLK_a};
        formation_column_key = {SDLK_s};
        formation_box_key = {SDLK_d};
        formation_stagger_key = {SDLK_f};
        formation_split_key = {SDLK_g};
    }
}

Hotkey Settings::building_key(const std::string& name) const {
    for (auto& [n, hk] : building_keys) {
        if (n == name) return hk;
    }
    return {};
}

void Settings::set_building_key(const std::string& name, Hotkey hk) {
    for (auto& [n, k] : building_keys) {
        if (n == name) { k = hk; return; }
    }
}

Hotkey Settings::construction_key(const std::string& name) const {
    for (auto& [n, hk] : construction_keys) {
        if (n == name) return hk;
    }
    return {};
}

void Settings::set_construction_key(const std::string& name, Hotkey hk) {
    for (auto& [n, k] : construction_keys) {
        if (n == name) { k = hk; return; }
    }
}

Hotkey Settings::item_key(const std::string& kind, const std::string& item) const {
    // canonical_item resolves e.g. "rifleman" (this frame's actually-
    // trained tier) back to "muscateer" (the whole chain's one binding) --
    // see item_hotkeys.h.
    std::string id = ww::hotkeys::item_key_id(kind, ww::hotkeys::canonical_item(kind, item));
    for (auto& [n, hk] : item_keys) {
        if (n == id) return hk;
    }
    return {};
}

void Settings::set_item_key(const std::string& kind, const std::string& item, Hotkey hk) {
    std::string id = ww::hotkeys::item_key_id(kind, ww::hotkeys::canonical_item(kind, item));
    for (auto& [n, k] : item_keys) {
        if (n == id) { k = hk; return; }
    }
}

std::string Settings::file_path() {
    std::string dir = ".";
    if (const char* local = SDL_getenv("LOCALAPPDATA")) dir = local;
    return dir + "\\WorldWar\\settings.json";
}

std::string Settings::key_label(SDL_Keycode key) {
    const char* name = SDL_GetKeyName(key);
    return (name && *name) ? name : "?";
}

std::string Settings::key_label(Hotkey hk) {
    return (hk.ctrl ? "Ctrl+" : "") + key_label(hk.key);
}

void Settings::load() {
    std::ifstream f(file_path());
    if (!f.is_open()) return;
    nlohmann::json j;
    try {
        f >> j;
    } catch (...) {
        return; // corrupt/hand-edited-invalid -- keep the defaults already seeded
    }

    read_hotkey(j, "idle_villager_key", idle_villager_key);
    if (j.contains("building_keys") && j["building_keys"].is_object()) {
        for (auto& [name, val] : j["building_keys"].items()) {
            if (!val.is_string()) continue;
            Hotkey hk = hotkey_from_label(val.get<std::string>());
            if (hk.key != SDLK_UNKNOWN) set_building_key(name, hk);
        }
    }
    if (j.contains("construction_keys") && j["construction_keys"].is_object()) {
        for (auto& [name, val] : j["construction_keys"].items()) {
            if (!val.is_string()) continue;
            Hotkey hk = hotkey_from_label(val.get<std::string>());
            if (hk.key != SDLK_UNKNOWN) set_construction_key(name, hk);
        }
    }
    if (j.contains("item_keys") && j["item_keys"].is_object()) {
        // Keyed by the SAME composite id (e.g. "train|fishing boat") used
        // in-memory -- id.find('|') splits it back into kind/item for
        // set_item_key. An id from an old/hand-edited file that no longer
        // matches any current item_keys entry is just silently dropped,
        // same as an unrecognized building_keys entry.
        for (auto& [id, val] : j["item_keys"].items()) {
            if (!val.is_string()) continue;
            auto sep = id.find('|');
            if (sep == std::string::npos) continue;
            Hotkey hk = hotkey_from_label(val.get<std::string>());
            if (hk.key != SDLK_UNKNOWN) set_item_key(id.substr(0, sep), id.substr(sep + 1), hk);
        }
    }
    if (j.contains("completed_campaign_levels") && j["completed_campaign_levels"].is_array()) {
        for (auto& val : j["completed_campaign_levels"]) {
            if (val.is_string()) completed_campaign_levels.insert(val.get<std::string>());
        }
    }

    read_hotkey(j, "pan_up_key", pan_up_key);
    read_hotkey(j, "pan_down_key", pan_down_key);
    read_hotkey(j, "pan_left_key", pan_left_key);
    read_hotkey(j, "pan_right_key", pan_right_key);
    read_hotkey(j, "build_eco_key", build_eco_key);
    read_hotkey(j, "build_military_key", build_military_key);
    read_hotkey(j, "attack_move_key", attack_move_key);
    read_hotkey(j, "formation_column_key", formation_column_key);
    read_hotkey(j, "formation_box_key", formation_box_key);
    read_hotkey(j, "formation_stagger_key", formation_stagger_key);
    read_hotkey(j, "formation_split_key", formation_split_key);
    read_hotkey(j, "land_key", land_key);
    read_hotkey(j, "unload_key", unload_key);
    read_hotkey(j, "shipyard_page_key", shipyard_page_key);
    read_hotkey(j, "build_nuke_key", build_nuke_key);
    read_hotkey(j, "build_back_key", build_back_key);

    if (j.contains("resolution_index") && j["resolution_index"].is_number_integer()) {
        int idx = j["resolution_index"].get<int>();
        resolution_index = std::clamp(idx, 0, static_cast<int>(std::size(kResolutions)) - 1);
    }
    if (j.contains("sfx_volume") && j["sfx_volume"].is_number()) {
        sfx_volume = std::clamp(j["sfx_volume"].get<double>(), 0.0, 1.0);
    }
    if (j.contains("music_volume") && j["music_volume"].is_number()) {
        music_volume = std::clamp(j["music_volume"].get<double>(), 0.0, 1.0);
    }
}

void Settings::save() const {
    nlohmann::json j;
    j["idle_villager_key"] = key_label(idle_villager_key);

    nlohmann::json bmap = nlohmann::json::object();
    for (auto& [name, hk] : building_keys) bmap[name] = key_label(hk);
    j["building_keys"] = bmap;

    nlohmann::json cmap = nlohmann::json::object();
    for (auto& [name, hk] : construction_keys) cmap[name] = key_label(hk);
    j["construction_keys"] = cmap;

    nlohmann::json imap = nlohmann::json::object();
    for (auto& [id, hk] : item_keys) imap[id] = key_label(hk);
    j["item_keys"] = imap;

    j["completed_campaign_levels"] =
        std::vector<std::string>(completed_campaign_levels.begin(), completed_campaign_levels.end());

    j["pan_up_key"] = key_label(pan_up_key);
    j["pan_down_key"] = key_label(pan_down_key);
    j["pan_left_key"] = key_label(pan_left_key);
    j["pan_right_key"] = key_label(pan_right_key);
    j["build_eco_key"] = key_label(build_eco_key);
    j["build_military_key"] = key_label(build_military_key);
    j["attack_move_key"] = key_label(attack_move_key);
    j["formation_column_key"] = key_label(formation_column_key);
    j["formation_box_key"] = key_label(formation_box_key);
    j["formation_stagger_key"] = key_label(formation_stagger_key);
    j["formation_split_key"] = key_label(formation_split_key);
    j["land_key"] = key_label(land_key);
    j["unload_key"] = key_label(unload_key);
    j["shipyard_page_key"] = key_label(shipyard_page_key);
    j["build_nuke_key"] = key_label(build_nuke_key);
    j["build_back_key"] = key_label(build_back_key);

    j["resolution_index"] = resolution_index;
    j["sfx_volume"] = sfx_volume;
    j["music_volume"] = music_volume;

    std::string path = file_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path);
    if (f.is_open()) f << j.dump(2);
}
