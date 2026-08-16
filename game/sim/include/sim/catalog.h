#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace ww::sim {

// Loads the game-data JSON files from data/ verbatim (no
// format changes from the Python version). Typed accessors are added as
// gameplay code starts consuming specific fields (Phase B); for now this
// is a generic keyed store so callers can pull whatever they need.
class DataStore {
public:
    explicit DataStore(const std::string& data_dir);

    const nlohmann::json& catalog() const { return catalog_; }
    // Inject/override a unit definition at runtime (campaign custom units --
    // the campaign Match ctor clones a base unit and overrides its stats/sprite,
    // then adds it here so make_unit() resolves it like any catalog unit).
    void inject_unit(const std::string& name, const nlohmann::json& def) { catalog_["units"][name] = def; }
    const nlohmann::json& techs() const { return techs_; }
    const nlohmann::json& civs() const { return civs_; }
    const nlohmann::json& civ_exclude() const { return civ_exclude_; }
    const nlohmann::json& building_techs() const { return building_techs_; }
    const nlohmann::json& tech_desc() const { return tech_desc_; }

private:
    nlohmann::json catalog_;
    nlohmann::json techs_;
    nlohmann::json civs_;
    nlohmann::json civ_exclude_;
    nlohmann::json building_techs_;
    nlohmann::json tech_desc_;
};

} // namespace ww::sim
