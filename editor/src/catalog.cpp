#include "catalog.h"

#include <fstream>
#include <stdexcept>

namespace ww::gamedata {

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
    , civ_exclude_(load_json(data_dir + "/civ_exclude.json")) {
}

} // namespace ww::gamedata
