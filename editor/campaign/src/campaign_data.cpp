#include "campaign/campaign_data.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

namespace ww::campaign {

namespace {

std::string slugify(const std::string& name) {
    std::string out;
    bool last_was_sep = false;
    for (unsigned char c : name) {
        if (std::isalnum(c)) {
            out += static_cast<char>(std::tolower(c));
            last_was_sep = false;
        } else if (!last_was_sep && !out.empty()) {
            out += '_';
            last_was_sep = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "campaign" : out;
}

nlohmann::json entity_to_json(const PlacedEntity& e) {
    return {{"id", e.id}, {"type", e.type}, {"player", e.player_index}, {"tx", e.tx}, {"ty", e.ty}};
}

PlacedEntity entity_from_json(const nlohmann::json& j) {
    PlacedEntity e;
    e.id = j.value("id", "");
    e.type = j.value("type", "");
    e.player_index = j.value("player", 0);
    e.tx = j.value("tx", 0);
    e.ty = j.value("ty", 0);
    return e;
}

nlohmann::json objective_to_json(const Objective& o) {
    nlohmann::json j;
    j["id"] = o.id;
    j["name"] = o.name;
    j["type"] = o.type;
    j["hidden"] = o.hidden;
    j["area_tx"] = o.area_tx;
    j["area_ty"] = o.area_ty;
    j["area_tw"] = o.area_tw;
    j["area_th"] = o.area_th;
    j["target_unit_ids"] = o.target_unit_ids;
    return j;
}

Objective objective_from_json(const nlohmann::json& j) {
    Objective o;
    o.id = j.value("id", "");
    o.name = j.value("name", "");
    o.type = j.value("type", "kill_units");
    o.hidden = j.value("hidden", false);
    o.area_tx = j.value("area_tx", 0);
    o.area_ty = j.value("area_ty", 0);
    o.area_tw = j.value("area_tw", 0);
    o.area_th = j.value("area_th", 0);
    if (j.contains("target_unit_ids")) {
        for (auto& idj : j.at("target_unit_ids")) o.target_unit_ids.push_back(idj.get<std::string>());
    }
    return o;
}

nlohmann::json map_event_to_json(const MapEvent& e) {
    nlohmann::json j;
    j["id"] = e.id;
    j["name"] = e.name;
    j["type"] = e.type;
    j["tx"] = e.tx;
    j["ty"] = e.ty;
    j["text"] = e.text;
    j["area_tx"] = e.area_tx;
    j["area_ty"] = e.area_ty;
    j["area_tw"] = e.area_tw;
    j["area_th"] = e.area_th;
    j["unlock_objective_id"] = e.unlock_objective_id;
    j["res_food"] = e.res_food;
    j["res_wood"] = e.res_wood;
    j["res_oil"] = e.res_oil;
    j["res_iron"] = e.res_iron;
    j["spawn_unit"] = e.spawn_unit;
    j["spawn_count"] = e.spawn_count;
    j["spawn_player"] = e.spawn_player;
    j["area2_tx"] = e.area2_tx;
    j["area2_ty"] = e.area2_ty;
    j["area2_tw"] = e.area2_tw;
    j["area2_th"] = e.area2_th;
    auto& los = j["los_areas"] = nlohmann::json::array();
    for (auto& r : e.los_areas) los.push_back({{"tx", r.tx}, {"ty", r.ty}, {"tw", r.tw}, {"th", r.th}});
    if (e.trig_tx0 != -1) {
        j["trig_tx0"] = e.trig_tx0;
        j["trig_ty0"] = e.trig_ty0;
        j["trig_tx1"] = e.trig_tx1;
        j["trig_ty1"] = e.trig_ty1;
    }
    return j;
}

MapEvent map_event_from_json(const nlohmann::json& j) {
    MapEvent e;
    e.id = j.value("id", "");
    e.name = j.value("name", "");
    e.type = j.value("type", "message");
    e.tx = j.value("tx", -1);
    e.ty = j.value("ty", 0);
    e.text = j.value("text", "");
    e.area_tx = j.value("area_tx", 0);
    e.area_ty = j.value("area_ty", 0);
    e.area_tw = j.value("area_tw", 0);
    e.area_th = j.value("area_th", 0);
    e.unlock_objective_id = j.value("unlock_objective_id", "");
    e.res_food = j.value("res_food", 0.0);
    e.res_wood = j.value("res_wood", 0.0);
    e.res_oil = j.value("res_oil", 0.0);
    e.res_iron = j.value("res_iron", 0.0);
    e.spawn_unit = j.value("spawn_unit", "");
    e.spawn_count = j.value("spawn_count", 0);
    e.spawn_player = j.value("spawn_player", 1);
    e.area2_tx = j.value("area2_tx", 0);
    e.area2_ty = j.value("area2_ty", 0);
    e.area2_tw = j.value("area2_tw", 0);
    e.area2_th = j.value("area2_th", 0);
    if (j.contains("los_areas")) {
        for (auto& rj : j.at("los_areas")) {
            TileRect r;
            r.tx = rj.value("tx", 0);
            r.ty = rj.value("ty", 0);
            r.tw = rj.value("tw", 0);
            r.th = rj.value("th", 0);
            e.los_areas.push_back(r);
        }
    }
    e.trig_tx0 = j.value("trig_tx0", -1);
    e.trig_ty0 = j.value("trig_ty0", 0);
    e.trig_tx1 = j.value("trig_tx1", -1);
    e.trig_ty1 = j.value("trig_ty1", 0);
    return e;
}

nlohmann::json level_to_json(const Level& lvl) {
    nlohmann::json j;
    j["id"] = lvl.id;
    j["name"] = lvl.name;
    j["description"] = lvl.description;
    j["loc_x"] = lvl.loc_x;
    j["loc_y"] = lvl.loc_y;
    j["grid_size"] = lvl.grid_size;
    j["reveal_mode"] = lvl.reveal_mode;
    j["max_age"] = lvl.max_age;
    j["start_age"] = lvl.start_age;
    j["pop_cap"] = lvl.pop_cap;
    if (!lvl.briefing_image.empty()) j["briefing_image"] = lvl.briefing_image;
    if (!lvl.ai_profile.empty()) j["ai_profile"] = lvl.ai_profile;
    auto& players = j["players"] = nlohmann::json::array();
    for (auto& p : lvl.players) {
        players.push_back({{"civ", p.civ},
                           {"team", p.team},
                           {"era", p.era},
                           {"food", p.food},
                           {"wood", p.wood},
                           {"oil", p.oil},
                           {"iron", p.iron},
                           {"ai_behavior", p.ai_behavior}});
    }
    auto& terrain = j["terrain"] = nlohmann::json::array();
    for (auto& t : lvl.terrain) {
        terrain.push_back({{"tx", t.tx}, {"ty", t.ty}, {"base", t.base}, {"resource", t.resource}});
    }
    auto& units = j["units"] = nlohmann::json::array();
    for (auto& u : lvl.units) units.push_back(entity_to_json(u));
    auto& buildings = j["buildings"] = nlohmann::json::array();
    for (auto& b : lvl.buildings) buildings.push_back(entity_to_json(b));
    auto& objectives = j["objectives"] = nlohmann::json::array();
    for (auto& o : lvl.objectives) objectives.push_back(objective_to_json(o));
    auto& events = j["events"] = nlohmann::json::array();
    for (auto& e : lvl.events) events.push_back(map_event_to_json(e));
    return j;
}

Level level_from_json(const nlohmann::json& j) {
    Level lvl;
    lvl.id = j.value("id", "");
    lvl.name = j.value("name", "");
    lvl.description = j.value("description", "");
    lvl.loc_x = j.value("loc_x", 0.5);
    lvl.loc_y = j.value("loc_y", 0.5);
    lvl.grid_size = j.value("grid_size", kLevelGridSize);
    lvl.reveal_mode = j.value("reveal_mode", 0);
    lvl.max_age = j.value("max_age", -1);
    lvl.start_age = j.value("start_age", 0);
    lvl.pop_cap = j.value("pop_cap", 100);
    lvl.briefing_image = j.value("briefing_image", std::string());
    lvl.ai_profile = j.value("ai_profile", std::string());
    if (j.contains("players")) {
        for (auto& pj : j.at("players")) {
            LevelPlayer p;
            p.civ = pj.value("civ", 0);
            p.team = pj.value("team", 1);
            p.era = pj.value("era", 0);
            p.food = pj.value("food", 200.0);
            p.wood = pj.value("wood", 200.0);
            p.oil = pj.value("oil", 100.0);
            p.iron = pj.value("iron", 100.0);
            p.ai_behavior = pj.value("ai_behavior", std::string("default"));
            lvl.players.push_back(p);
        }
    }
    if (j.contains("terrain")) {
        for (auto& tj : j.at("terrain")) {
            TerrainFeature t;
            t.tx = tj.value("tx", 0);
            t.ty = tj.value("ty", 0);
            t.base = tj.value("base", "");
            t.resource = tj.value("resource", "");
            lvl.terrain.push_back(t);
        }
    }
    if (j.contains("units")) {
        for (auto& uj : j.at("units")) lvl.units.push_back(entity_from_json(uj));
    }
    if (j.contains("buildings")) {
        for (auto& bj : j.at("buildings")) lvl.buildings.push_back(entity_from_json(bj));
    }
    if (j.contains("objectives")) {
        for (auto& oj : j.at("objectives")) lvl.objectives.push_back(objective_from_json(oj));
    }
    if (j.contains("events")) {
        for (auto& ej : j.at("events")) lvl.events.push_back(map_event_from_json(ej));
    }
    return lvl;
}

} // namespace

std::vector<Campaign> load_all_campaigns(const std::string& data_dir) {
    std::vector<Campaign> out;
    std::filesystem::path dir = std::filesystem::path(data_dir) / "campaigns";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return out;

    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        std::ifstream f(entry.path());
        if (!f) continue;
        nlohmann::json j;
        try {
            f >> j;
        } catch (const nlohmann::json::exception&) {
            continue; // skip a corrupt/unparseable file rather than aborting the whole scan
        }
        Campaign c;
        c.name = j.value("name", entry.path().stem().string());
        c.civ = j.value("civ", 0);
        c.file_path = entry.path().string();
        if (j.contains("levels")) {
            for (auto& lj : j.at("levels")) c.levels.push_back(level_from_json(lj));
        }
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const Campaign& a, const Campaign& b) { return a.name < b.name; });
    return out;
}

void save_campaign(Campaign& campaign, const std::string& data_dir) {
    std::filesystem::path dir = std::filesystem::path(data_dir) / "campaigns";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    if (campaign.file_path.empty()) {
        std::string base = slugify(campaign.name);
        std::filesystem::path candidate = dir / (base + ".json");
        int suffix = 2;
        while (std::filesystem::exists(candidate, ec)) {
            candidate = dir / (base + "_" + std::to_string(suffix) + ".json");
            ++suffix;
        }
        campaign.file_path = candidate.string();
    }

    nlohmann::json j;
    j["name"] = campaign.name;
    j["civ"] = campaign.civ;
    auto& levels = j["levels"] = nlohmann::json::array();
    for (auto& lvl : campaign.levels) levels.push_back(level_to_json(lvl));

    std::ofstream out(campaign.file_path);
    out << j.dump(2);
}

std::string next_level_id(const Campaign& campaign) {
    std::set<std::string> used;
    for (auto& lvl : campaign.levels) used.insert(lvl.id);
    int n = static_cast<int>(campaign.levels.size()) + 1;
    std::string id;
    do {
        id = "lvl_" + std::to_string(n++);
    } while (used.count(id));
    return id;
}

std::string next_entity_id(const Level& lvl) {
    std::set<std::string> used;
    for (auto& u : lvl.units) used.insert(u.id);
    for (auto& b : lvl.buildings) used.insert(b.id);
    int n = static_cast<int>(lvl.units.size() + lvl.buildings.size()) + 1;
    std::string id;
    do {
        id = "ent_" + std::to_string(n++);
    } while (used.count(id));
    return id;
}

std::string next_objective_id(const Level& lvl) {
    std::set<std::string> used;
    for (auto& o : lvl.objectives) used.insert(o.id);
    int n = static_cast<int>(lvl.objectives.size()) + 1;
    std::string id;
    do {
        id = "obj_" + std::to_string(n++);
    } while (used.count(id));
    return id;
}

std::string next_event_id(const Level& lvl) {
    std::set<std::string> used;
    for (auto& e : lvl.events) used.insert(e.id);
    int n = static_cast<int>(lvl.events.size()) + 1;
    std::string id;
    do {
        id = "evt_" + std::to_string(n++);
    } while (used.count(id));
    return id;
}

} // namespace ww::campaign
