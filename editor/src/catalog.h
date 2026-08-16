#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace ww::gamedata {

// Loads just the two data/*.json files this editor's palette actually
// needs (unit/building icon sprites, civ exclusions) -- trimmed down from
// the main engine's own DataStore (game/sim/include/sim/
// catalog.h), which also loads techs/civs/building_techs/tech_desc that
// have no bearing on placing starting units and buildings.
class DataStore {
public:
    explicit DataStore(const std::string& data_dir);

    const nlohmann::json& catalog() const { return catalog_; }
    const nlohmann::json& civ_exclude() const { return civ_exclude_; }

private:
    nlohmann::json catalog_;
    nlohmann::json civ_exclude_;
};

} // namespace ww::gamedata
