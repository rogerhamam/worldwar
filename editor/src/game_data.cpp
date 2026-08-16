#include "game_data.h"

namespace ww::gamedata {

const std::unordered_map<std::string, std::vector<std::string>> PRODUCTION = {
    {"base", {"civilian"}},
    {"barracks", {"swordsman", "muscateer", "rifleman", "infantryman", "artillery1"}},
    {"academy", {"cavalry", "cavalry2", "camel"}},
    {"factory", {"light tank", "tank", "heavy tank", "artillery", "aa gun"}},
    {"airbase", {"biplane", "fighter", "bomber", "jet fighter", "heavy bomber", "b29", "ohka"}},
    {"shipyard", {"fishing boat", "destroyer", "frigate", "torpedo boat", "battleship", "yamato"}},
    {"fortress", {"waffen", "elite waffen", "janissary", "royal janissary"}},
};

const std::vector<std::string> BUILDABLE = {
    "house", "barracks", "farm", "refinery", "market", "university",
    "factory", "airbase", "academy", "shipyard", "tower", "fortress",
    "nuclear reactor", "palisade",
};

const std::set<int> CAMEL_CIVS = {7, 8};
const std::set<std::string> CAMEL_UNITS = {"camel", "camel corps"};
const std::set<std::string> OTTOMAN_ONLY = {"janissary", "royal janissary"};
const std::unordered_map<std::string, int> CIV_ONLY_UNITS = {
    {"b29", 1}, {"ohka", 4}, {"yamato", 4}, {"waffen", 2}, {"elite waffen", 2},
};

const std::unordered_map<int, std::string> CIV_BASE = {
    {0, "spr_uk_base"}, {1, "spr_capitol"}, {2, "spr_nazi_base"},
    {3, "spr_soviet_base"}, {4, "spr_japan_base"}, {5, "spr_italy_base"},
    {6, "spr_france_base"}, {7, "spr_china_base"}, {8, "spr_ottoman_base"},
};

bool civ_has(const std::string& item, int civ,
            const std::unordered_map<int, std::set<std::string>>& civ_exclude) {
    auto it = civ_exclude.find(civ);
    if (it == civ_exclude.end()) return true;
    return it->second.count(item) == 0;
}

std::pair<int, int> building_wh(const std::string& name) {
    static const std::unordered_map<std::string, std::pair<int, int>> sizes = {
        {"base", {96, 96}}, {"fortress", {96, 96}},
        {"house", {64, 64}}, {"farm", {64, 64}}, {"barracks", {64, 64}}, {"factory", {64, 64}},
        {"refinery", {64, 64}}, {"university", {64, 64}}, {"market", {64, 64}}, {"academy", {64, 64}},
        {"shipyard", {64, 64}},
        {"airbase", {96, 64}}, {"nuclear reactor", {96, 64}},
        {"tower", {64, 96}}, {"aa tower", {64, 96}},
        {"palisade", {32, 32}},
    };
    auto it = sizes.find(name);
    return it == sizes.end() ? std::make_pair(64, 64) : it->second;
}

} // namespace ww::gamedata
