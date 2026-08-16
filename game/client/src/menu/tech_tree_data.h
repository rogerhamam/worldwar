#pragma once
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Tech tree grid layout -- loaded at runtime from <data_dir>/tech_tree_grid.json
// instead of being hardcoded here. That file is designed and exported by
// tools/tech_tree_editor (a standalone Python design tool covering both
// this grid and per-civ tech/unit/building access), not hand-maintained as
// C++. It started life as a verbatim transcription of the original
// GameMaker source's objects/control/Create.gml placement data (see git
// history prior to the JSON migration for that provenance), and the tool's
// export keeps the same schema so this loader/the renderer below didn't
// need to change.
//
// "through" is a real entry: a filler cell (drawn as tech_tree_through_arrow,
// no icon) that routes a long arrow across an otherwise-empty grid cell --
// the export can synthesize these to bridge a redesigned connection that
// isn't between directly-adjacent cells.
namespace ww::menu {

struct TechTreeEntry {
    int col, row;
    std::string name;
    // NOTE on "parent": despite the name, in draw_tech_tree's actual usage
    // (menu_controller.cpp) this flag belongs to the SOURCE cell, not the
    // receiver -- the vertical-arrow check reads *this* flag on the cell
    // ABOVE a given entry to decide whether to draw an arrow down INTO
    // that entry. It does not mean "I accept an arrow from above."
    bool parent = true;        // this cell projects a vertical (down) arrow into whatever's directly below it
    bool child = true;         // offers an outgoing horizontal (side) arrow to the cell right of it
    bool older_sibling = true; // accepts an incoming horizontal (side) arrow from directly left
    bool force = false;        // always draws an incoming vertical arrow regardless of the cell above
};

inline std::vector<TechTreeEntry> load_tech_tree_grid(const std::string& data_dir) {
    std::ifstream f(data_dir + "/tech_tree_grid.json");
    if (!f) throw std::runtime_error("failed to open data file: " + data_dir + "/tech_tree_grid.json");
    nlohmann::json j;
    f >> j;
    std::vector<TechTreeEntry> grid;
    grid.reserve(j.size());
    for (auto& item : j) {
        TechTreeEntry e;
        e.col = item.at("col").get<int>();
        e.row = item.at("row").get<int>();
        e.name = item.at("name").get<std::string>();
        e.parent = item.value("parent", true);
        e.child = item.value("child", true);
        e.older_sibling = item.value("older_sibling", true);
        e.force = item.value("force", false);
        grid.push_back(std::move(e));
    }
    return grid;
}

// `data_dir` only matters on the first call -- the result is cached for
// the process's lifetime, same as ww::sim::DataStore's one-shot load.
inline const std::vector<TechTreeEntry>& tech_tree_grid(const std::string& data_dir) {
    static const std::vector<TechTreeEntry> grid = load_tech_tree_grid(data_dir);
    return grid;
}

// Frame-3 ("unique unit") icons in spr_tech_tree_icon, from
// obj_tech_tree_button/Draw.gml's image_index=3 name list.
inline bool is_unique_unit(const std::string& name) {
    static const std::vector<std::string> names = {
        "b29", "yamato", "ohka", "waffen", "elite waffen", "tiger tank",
        "tiger2 tank", "janissary", "royal janissary",
        "royal marine", "elite royal marine", "heavy artillery",
    };
    for (auto& n : names) if (n == name) return true;
    return false;
}

// Per-civ display-name override + hide rule for the handful of fortress/
// unique units that are swapped or removed outright rather than greyed out
// with the ordinary has()/civ_exclude mechanism (obj_tech_tree_button's
// Step.gml). Returns the name to actually display (already resolved for
// civ, e.g. "janissary" -> "waffen" for civ 2), or std::nullopt if this
// civ shouldn't see this grid cell at all.
inline std::optional<std::string> resolve_civ_unit(const std::string& name, int civ) {
    // A single "unique unit" grid slot (+ its elite) that resolves to whichever
    // Fortress unique unit the civ actually fields -- Germany's Waffen SS,
    // Ottoman Janissary, or UK Royal Marine (and their elite upgrades). Civs
    // with no unique Fortress unit show nothing here.
    if (name == "unique unit") {
        if (civ == 2) return "waffen";
        if (civ == 8) return "janissary";
        if (civ == 0) return "royal marine";
        return std::nullopt;
    }
    if (name == "unique unit elite") {
        if (civ == 2) return "elite waffen";
        if (civ == 8) return "royal janissary";
        if (civ == 0) return "elite royal marine";
        return std::nullopt;
    }
    if (name == "b29") return civ == 1 ? std::optional(name) : std::nullopt;
    if (name == "tiger tank" || name == "tiger2 tank") return civ == 2 ? std::optional(name) : std::nullopt;
    if (name == "yamato" || name == "ohka") return civ == 4 ? std::optional(name) : std::nullopt;
    // China (7) trains the base camel too, but only the Ottomans (8) get the
    // camel-corps upgrade -- so the tree shows the camel for both, camel corps
    // for the Ottomans alone (mirrors CAMEL_CIVS / CIV_UPGRADE_OWNER in control.cpp).
    if (name == "camel") return (civ == 7 || civ == 8) ? std::optional(name) : std::nullopt;
    if (name == "camel corps") return civ == 8 ? std::optional(name) : std::nullopt;
    if (name == "heavy artillery") return civ == 3 ? std::optional(name) : std::nullopt; // Soviet only
    // Both carrier tiers show for EVERY civ but are CROSSED OUT for the ones
    // that can't build them (via CIV_UPGRADE_OWNER in control.cpp: aircraft
    // carrier -> {0,1,4,5,6}; supercarrier -> {0,1,4,6}, so Italy sees the
    // carrier available and the supercarrier crossed). Not special-cased here.
    // Civ-gated TECHS: each unique technology is only visible in its owner civ's
    // tree (mirrors control.cpp's CIV_UPGRADE_OWNER, plus the heavy-tank
    // civ_exclude so it shows only for the civs that actually field heavy tanks).
    // Techs not listed here are generic and shown for every civ.
    static const std::map<std::string, std::set<int>> kTechOwners = {
        {"emergency fighter program", {2}}, // Germany
        {"meiji restoration", {4}},          // Japan
        {"420mm mortar", {3}},               // Soviet
        {"elite waffen upgrade", {2}},       // Germany
        {"tiger2 tank upgrade", {2}},        // Germany
        {"royal janissary upgrade", {8}},    // Ottoman
        {"camel corps upgrade", {8}},        // Ottoman
        {"heavy tank upgrade", {1, 3}},      // USA + Soviet (others civ_exclude heavy tank)
        {"naval hegemony", {0}},             // UK only
        // NOTE: radar is deliberately NOT here -- it should show for EVERY civ in
        // the tree, just crossed-out (via civ_has) for the ones that can't get it
        // (Soviet/Italy/China/Ottoman), rather than hidden entirely.
    };
    auto it = kTechOwners.find(name);
    if (it != kTechOwners.end() && !it->second.count(civ)) return std::nullopt;
    return name;
}

} // namespace ww::menu
