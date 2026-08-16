#include "sim/catalog.h"

#include <fstream>
#include <stdexcept>

namespace ww::sim {

namespace {
nlohmann::json load_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("failed to open data file: " + path);
    }
    nlohmann::json j;
    f >> j;
    return j;
}
} // namespace

DataStore::DataStore(const std::string& data_dir)
    : catalog_(load_json(data_dir + "/catalog.json"))
    , techs_(load_json(data_dir + "/techs.json"))
    , civs_(load_json(data_dir + "/civs.json"))
    , civ_exclude_(load_json(data_dir + "/civ_exclude.json"))
    , building_techs_(load_json(data_dir + "/building_techs.json"))
    , tech_desc_(load_json(data_dir + "/tech_desc.json")) {
}

} // namespace ww::sim
