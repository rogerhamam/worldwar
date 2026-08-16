#include "sim/control.h"

#include <algorithm>
#include <cmath>

namespace ww::sim {

const std::unordered_map<std::string, int> POP_COST = {
    {"biplane", 2}, {"fighter", 2}, {"jet fighter", 2}, {"light tank", 2}, {"aa gun", 1},
    {"heavy tank", 4}, {"tank", 4}, {"heavy bomber", 4}, {"bomber", 4}, {"b29", 6},
    {"battleship", 6}, {"yamato", 6}, {"destroyer", 2}, {"frigate", 2}, {"torpedo boat", 2},
    {"tiger tank", 5}, {"tiger2 tank", 5}, {"ohka", 1}, {"nuclear reactor", 5},
    {"aircraft carrier", 6}, {"aircraft carrier2", 8},
};

const std::unordered_map<std::string, std::vector<std::string>> PRODUCTION = {
    {"base", {"civilian"}},
    {"barracks", {"muscateer", "rifleman", "infantryman", "artillery1"}}, // artillery1 (tier-1) trained here; upgrades to artillery
    {"academy", {"swordsman", "cavalry", "cavalry2", "camel"}}, // swordsman moved here (+its swordsman2 upgrade lives at the academy too)
    {"factory", {"light tank", "tank", "aa gun"}}, // heavy tank + flak are UPGRADE-only (aa gun -> flak via the flak-upgrade tech); artillery at the barracks
    {"airbase", {"biplane", "fighter", "bomber", "jet fighter", "heavy bomber", "b29", "ohka"}},
    {"shipyard", {"fishing boat", "transport ship", "destroyer", "frigate", "torpedo boat", "battleship", "yamato", "aircraft carrier", "aircraft carrier2"}},
    // Unique units first so they take the (0,0) slot with their elite-upgrade
    // tech directly beneath (see tech_parent_units); the ballistic missile (all
    // civs) sits to their right. A civ with no unique fortress unit just shows
    // the ballistic missile at (0,0).
    {"fortress", {"waffen", "elite waffen", "janissary", "royal janissary",
                  "royal marine", "elite royal marine", "ballistic missile"}},
};

const std::vector<std::string> AGE_ITEMS = {"industrial", "war", "scientific"};
const std::array<std::string, 4> RES_KEYS = {"food", "wood", "oil", "iron"};

const std::map<std::string, std::pair<std::optional<std::string>, std::string>> UPGRADE_MAP = {
    {"rifleman upgrade", {"muscateer", "rifleman"}},
    {"infantryman upgrade", {"rifleman", "infantryman"}},
    {"swordsman2 upgrade", {"swordsman", "swordsman2"}},
    {"cavalry2 upgrade", {"cavalry", "cavalry2"}},
    {"cavalry3 upgrade", {"cavalry2", "cavalry3"}},
    {"camel corps upgrade", {"camel", "camel corps"}},
    {"royal janissary upgrade", {"janissary", "royal janissary"}},
    {"heavy tank upgrade", {"tank", "heavy tank"}},
    {"fighter upgrade", {"biplane", "fighter"}},
    {"jet fighter upgrade", {"fighter", "jet fighter"}}, // researching it converts fighters -> jets
    {"heavy bomber upgrade", {"bomber", "heavy bomber"}},
    {"torpedo boat upgrade", {"frigate", "torpedo boat"}},
    {"destroyer upgrade", {"torpedo boat", "destroyer"}},
    {"battleship upgrade", {std::nullopt, "battleship"}},
    {"elite waffen upgrade", {"waffen", "elite waffen"}},
    {"royal marine upgrade", {"royal marine", "elite royal marine"}}, // UK unique (fortress, scientific)
    {"flak upgrade", {"aa gun", "flak"}}, // factory (war era): aa gun -> flak gateway
    {"tiger2 tank upgrade", {"tiger tank", "tiger2 tank"}},
    {"artillery upgrade", {"artillery1", "artillery"}}, // tier-1 field cannon -> full artillery
    {"heavy artillery upgrade", {"artillery", "heavy artillery"}}, // Soviet unique (scientific): artillery -> heavy artillery
    {"aircraft carrier upgrade", {"aircraft carrier", "aircraft carrier2"}}, // carrier -> supercarrier (not Italy)
};

const std::map<std::string, std::pair<std::string, std::string>> BUILDING_UPGRADE_MAP = {
    {"flak tower upgrade", {"tower", "aa tower"}},
};

const std::set<int> CAMEL_CIVS = {7, 8};
const std::set<std::string> CAMEL_UNITS = {"camel", "camel corps"};
const std::set<std::string> OTTOMAN_ONLY = {"janissary", "royal janissary"};
// Aircraft carrier is limited to these civs (UK, USA, Japan, Italy, France).
// There is no supercarrier tier -- they field only the base "aircraft carrier".
const std::set<int> CARRIER_CIVS = {0, 1, 4, 5, 6};
const std::unordered_map<std::string, int> CIV_ONLY_UNITS = {
    {"b29", 1}, {"ohka", 4}, {"yamato", 4}, {"waffen", 2}, {"elite waffen", 2},
    {"royal marine", 0}, {"elite royal marine", 0}, // United Kingdom unique
    {"heavy artillery", 3},                          // Soviet Union unique (scientific-era artillery upgrade)
};
const std::set<std::string> UNIQUE_UNITS = [] {
    std::set<std::string> s = OTTOMAN_ONLY;
    for (auto& [k, v] : CIV_ONLY_UNITS) s.insert(k);
    s.insert("tiger tank");
    s.insert("tiger2 tank");
    return s;
}();
const std::map<std::string, std::set<int>> CIV_UPGRADE_OWNER = {
    {"elite waffen upgrade", {2}},
    {"tiger2 tank upgrade", {2}},
    {"royal janissary upgrade", {8}},
    {"royal marine upgrade", {0}}, // United Kingdom only (fortress, scientific era)
    {"naval hegemony", {0}},       // United Kingdom only (fortress, scientific era)
    {"camel corps upgrade", {8}}, // Ottoman only; China trains base camels but can't upgrade them
    {"radar", {0, 1, 2, 4, 6}},   // UK, USA, Germany, Japan, France (UK gets it free); no other civ
    // Per-civ UNIQUE technologies (fortress, Scientific era). Each is available
    // to exactly one civ; the display name/cost live in catalog.json, the effect
    // is applied in cost_of / projectile_behavior (see below).
    {"emergency fighter program", {2}}, // Nazi Germany: aircraft cost wood, not iron
    {"meiji restoration", {4}},         // Japan: samurai cost food, not iron
    {"420mm mortar", {3}},              // Soviet Union: artillery blast radius +33% (display "Self-Propelled Mortar")
    {"heavy artillery upgrade", {3}},   // Soviet Union: artillery -> Heavy Artillery (scientific)
    {"aircraft carrier", {0, 1, 4, 5, 6}},      // UK/USA/Japan/Italy/France -> crossed out for the rest
    {"aircraft carrier upgrade", {0, 1, 4, 6}}, // supercarrier: UK/USA/Japan/France (Italy gets the base carrier only)
    {"aircraft carrier2", {0, 1, 4, 6}},        // same gate on the unit -> crossed out for everyone else
};
const std::unordered_map<std::string, std::string> UNIT_REQUIRES = [] {
    std::unordered_map<std::string, std::string> m;
    for (auto& [up, pair] : UPGRADE_MAP) m[pair.second] = up;
    return m;
}();
const std::unordered_map<std::string, std::string> UNIT_UPGRADE = [] {
    std::unordered_map<std::string, std::string> m;
    for (auto& [up, pair] : UPGRADE_MAP) {
        if (pair.first) m[*pair.first] = up;
    }
    return m;
}();
const std::map<std::pair<int, std::string>, std::string> CIV_UNIT_SUB = {
    // Germany's TANK slot is their Tiger (heavy tank is no longer a slot -- it's
    // the generic upgrade of tank; Germany's Tiger IS their heavy-tier tank, and
    // upgrades to Tiger II). available_units re-resolves after this sub so the
    // Tiger II upgrade still converts the Tiger slot -> Tiger II.
    {{2, "tank"}, "tiger tank"},
};

// "Requires this tech researched" gates BEYOND the upgrade chain -- a
// producible unit/building only appears once the team owns the named tech.
const std::unordered_map<std::string, std::string> UNIT_TECH_REQ = {
    // (jet fighter is gated by the "jet fighter upgrade" tech again, via
    // UPGRADE_MAP/UNIT_REQUIRES -- no direct tech gate needed here.)
    // Japan's Yamato sits behind the same "battleship upgrade" tech that
    // unlocks the standard battleship (UNIT_REQUIRES gates battleship via
    // UPGRADE_MAP; that map is one-unit-per-tech, so Yamato is gated here) --
    // so researching Battleship unlocks BOTH the battleship and the Yamato.
    {"yamato", "battleship upgrade"},
};
const std::unordered_map<std::string, std::string> BUILDING_TECH_REQ = {
    {"nuclear reactor", "nuclear physics"}, // reactor unlocked by nuclear physics
};

const std::unordered_map<std::string, std::vector<std::string>> TECH_PREREQ = {
    {"infantryman upgrade", {"rifleman upgrade"}},
    {"cavalry3 upgrade", {"cavalry2 upgrade"}},
    // Jet Engine unlocks the jet; Upgrade Fighter is the rung below it on the
    // SAME chain. Requiring only Jet Engine let a team skip the middle rung
    // entirely -- Germany reaching Scientific with Jet Engine but no Upgrade
    // Fighter could research the jet upgrade straight off a biplane roster. It
    // maps fighter -> jet fighter (UPGRADE_MAP), so with no fighters to convert
    // the airbase went on training biplanes and the tech bought nothing. Now the
    // chain has to be walked in order, biplane -> fighter -> jet fighter, like
    // the barracks firearm chain.
    {"jet fighter upgrade", {"jet engine", "fighter upgrade"}},
    {"heavy artillery upgrade", {"artillery upgrade"}}, // needs the full artillery first
    {"atomic bomb", {"nuclear physics"}},    // airbase nuke tech, needs nuclear physics
    {"destroyer upgrade", {"torpedo boat upgrade"}},
    // King Tiger (tiger2) and Elite Waffen SS upgrades are gated ONLY by their
    // scientific-era requirement (TECH_ERA below) -- advancing to that age
    // unlocks them outright, so they carry no tech prerequisite of their own.
    // Barracks firearm chain -- researched in order, each in the same slot.
    {"semi automatic rifle", {"bolt action rifle"}},
    {"assault rifle", {"semi automatic rifle"}},
    {"alloys", {"refined steel"}},
    {"electric arc furnace", {"alloys"}},
    {"fertilizer", {"irrigation"}},
    {"pesticide", {"fertilizer"}},
    {"mobile sawmill", {"power saw"}},
    {"fracking", {"electric drill"}},
    {"beneficiation", {"smelting"}},
    {"synthetic fuel", {"gasoline"}},
    {"composite plane armor", {"steel plane armor"}},
};

const std::unordered_map<std::string, int> TECH_ERA = {
    {"uniform", 0}, {"irrigation", 0}, {"binoculars", 1}, {"radar", 2}, // radar: university, War era
    {"elite waffen upgrade", 3}, {"royal janissary upgrade", 2}, {"tiger2 tank upgrade", 3},
    {"rifleman upgrade", 1}, {"infantryman upgrade", 2}, {"swordsman2 upgrade", 1},
    {"cavalry2 upgrade", 1}, {"refined steel", 1}, {"steel frame", 1}, {"electric drill", 1},
    {"smelting", 1}, {"power saw", 1}, {"horse wagon", 1}, {"fertilizer", 1},
    {"torpedo boat upgrade", 1},
    {"cavalry3 upgrade", 2}, {"camel corps upgrade", 2}, {"alloys", 2}, {"diesel engine", 2},
    {"blowback reload", 2}, {"steel plane armor", 2}, {"composite plane armor", 2},
    {"fighter upgrade", 2}, {"destroyer upgrade", 2}, {"battleship upgrade", 2},
    {"ballistics", 2}, {"fracking", 2}, {"beneficiation", 2}, {"gasoline", 2},
    {"pesticide", 2}, {"mobile sawmill", 2},
    {"electric arc furnace", 3}, {"heavy tank upgrade", 3}, {"heavy bomber upgrade", 3},
    {"jet engine", 3}, {"jet fighter upgrade", 3}, {"synthetic fuel", 3}, {"heavy artillery upgrade", 2},
    {"nuclear physics", 3}, // unlocks the nuclear reactor
    {"atomic bomb", 3},     // airbase: enables loading nukes (needs nuclear physics)
    {"assault rifle", 3},   // barracks: firearm infantry upgrades (progressive)
    {"bolt action rifle", 1}, {"semi automatic rifle", 2},
    {"hydrodynamics", 2},   // shipyard (war era): all ships move 10% faster
    {"naval armour", 2},    // shipyard (war era): all ships +10% hp, +1/1 armour
    {"trade agreement", 1}, // market: trade fee reduced to 10%
    {"artillery upgrade", 1}, // barracks (industrial): tier-1 field cannon -> full artillery
    {"emergency fighter program", 3}, // fortress (scientific): civ unique techs
    {"meiji restoration", 3},
    {"420mm mortar", 3},
    {"conscription", 3},    // fortress (scientific): all civs, land military +33% train speed
    {"royal marine upgrade", 3}, // fortress (scientific): UK unique, marines -> elite marines
    {"naval hegemony", 3},       // fortress (scientific): UK unique naval buff
    {"flak tower upgrade", 2}, // university (war era): tower -> aa tower; was ungated (era 0)
    {"flak upgrade", 2},       // factory (war era): aa gun -> flak
    {"aircraft carrier upgrade", 3}, // shipyard (scientific): carrier -> supercarrier
};

const std::unordered_map<std::string, int> UNIT_ERA = {
    {"civilian", 0}, {"swordsman", 0}, {"cavalry", 0}, {"muscateer", 0}, {"camel", 0},
    {"janissary", 0}, {"fishing boat", 0}, {"frigate", 0}, {"artillery1", 0}, // tier-1 field cannon (Victorian)
    {"rifleman", 1}, {"infantryman", 1}, {"cavalry2", 1}, {"swordsman2", 1}, {"light tank", 1},
    {"artillery", 1}, {"aa gun", 1}, {"biplane", 1}, {"fighter", 1}, {"destroyer", 1},
    {"torpedo boat", 1},
    {"waffen", 2}, {"cavalry3", 2}, {"camel corps", 2}, {"tank", 2}, {"bomber", 2}, {"flak", 2},
    {"battleship", 2}, {"tiger tank", 2}, {"royal janissary", 2},
    {"heavy tank", 3}, {"elite waffen", 3}, {"jet fighter", 3}, {"heavy bomber", 3}, {"b29", 3},
    {"heavy artillery", 2}, // Soviet unique: War era
    {"ohka", 3}, {"yamato", 3}, {"tiger2 tank", 3}, {"me262", 3},
    {"ballistic missile", 3}, // fortress, scientific era, all civs
    {"royal marine", 2}, {"elite royal marine", 3}, // UK unique: war era, elite in scientific
    {"aircraft carrier", 2}, {"aircraft carrier2", 3}, // shipyard: war era carrier (UK/USA/Japan/Italy/France), scientific supercarrier (no Italy)
};

const std::unordered_map<std::string, int> BUILDING_ERA = {
    {"base", 2}, // buildable expansion base -- War era onward (the starting base spawns directly)
    {"house", 0}, {"farm", 0}, {"barracks", 0}, {"market", 0}, {"academy", 0},
    {"palisade", 0}, {"iron wall", 0}, {"tower", 0}, {"refinery", 0}, {"shipyard", 0},
    {"factory", 1}, {"university", 1}, {"airbase", 1}, {"aa tower", 1},
    {"fortress", 2}, {"nuclear reactor", 3}, {"outpost", 0},
};

const std::vector<std::string> BUILDABLE = {
    "house", "barracks", "farm", "refinery", "market", "university",
    "factory", "airbase", "academy", "shipyard", "tower", "fortress",
    "nuclear reactor", "base", "outpost", "palisade", "iron wall",
};

int pop_cost(const std::string& name) {
    auto it = POP_COST.find(name);
    return it == POP_COST.end() ? 1 : it->second;
}

double transport_cost(const std::string& name) {
    // Royal Marines (and their elite form) count as half a slot aboard a
    // transport ship, per their UK unique bonus.
    if (name == "royal marine" || name == "elite royal marine") return 0.5;
    return pop_cost(name);
}

bool Control::civ_has(const std::string& item, int civ,
                       const std::unordered_map<int, std::set<std::string>>& civ_exclude) {
    auto it = civ_exclude.find(civ);
    if (it == civ_exclude.end()) return true;
    return it->second.count(item) == 0;
}

bool Control::civ_upgrade_allowed(const std::string& item, int civ) {
    auto owner = CIV_UPGRADE_OWNER.find(item);
    return owner == CIV_UPGRADE_OWNER.end() || owner->second.count(civ) != 0;
}

Control::Control(const DataStore& data, const Bonuses& bonuses, int n_players, int max_pop_)
    : n(n_players), max_pop(max_pop_), data_(data), bonuses_(bonuses) {
    teams.resize(8);
    for (int i = 0; i < 8; ++i) {
        teams[i].is_ai = (i != 0 && i < n_players);
        teams[i].colour = i;
        teams[i].ally = i; // free-for-all by default; new_skirmish may override from settings
    }

    // civ_exclude.json: {"0": [...], "1": [...], ...} -> int-keyed sets.
    for (auto& [k, v] : data_.civ_exclude().items()) {
        std::set<std::string> excl;
        for (auto& item : v) excl.insert(item.get<std::string>());
        civ_exclude_[std::stoi(k)] = std::move(excl);
    }

    // building_techs.json, plus the same post-load fixups as control.py's
    // module-level mutation of BUILDING_TECHS (see the header comment).
    for (auto& [k, v] : data_.building_techs().items()) {
        std::vector<std::string> list;
        for (auto& item : v) list.push_back(item.get<std::string>());
        building_techs_[k] = std::move(list);
    }
    auto& base = building_techs_["base"]; // operator[] default-constructs if absent
    if (std::find(base.begin(), base.end(), "uniform") == base.end()) base.push_back("uniform");

    for (auto& [name, list] : building_techs_) {
        (void)name;
        list.erase(std::remove(list.begin(), list.end(), "swordsman2 upgrade"), list.end());
    }
    // Swordsman2 upgrade lives at the ACADEMY now (the swordsman is built
    // there too), not the barracks.
    auto& academy = building_techs_["academy"];
    if (std::find(academy.begin(), academy.end(), "swordsman2 upgrade") == academy.end()) {
        academy.insert(academy.begin(), "swordsman2 upgrade");
    }
    auto& barracks = building_techs_["barracks"];
    if (std::find(barracks.begin(), barracks.end(), "binoculars") == barracks.end()) {
        barracks.push_back("binoculars");
    }
    for (auto& [b, up] : {std::pair<std::string, std::string>{"fortress", "elite waffen upgrade"},
                          {"fortress", "royal janissary upgrade"},
                          {"factory", "tiger2 tank upgrade"},
                          // Per-civ unique techs at the fortress (CIV_UPGRADE_OWNER
                          // gates which civ actually sees each one).
                          {"fortress", "emergency fighter program"},
                          {"fortress", "meiji restoration"},
                          {"fortress", "420mm mortar"},
                          {"fortress", "royal marine upgrade"}, // UK only (CIV_UPGRADE_OWNER)
                          {"fortress", "naval hegemony"},       // UK only (CIV_UPGRADE_OWNER)
                          {"fortress", "conscription"}}) { // all civs
        auto& list = building_techs_[b];
        if (std::find(list.begin(), list.end(), up) == list.end()) list.push_back(up);
    }
}

nlohmann::json Control::base_cost(const std::string& item) const {
    const auto& cat = data_.catalog();
    for (const char* tbl : {"units", "buildings", "techs"}) {
        if (cat.at(tbl).contains(item)) {
            return cat.at(tbl).at(item).value("cost", nlohmann::json::object());
        }
    }
    if (cat.contains("ageCosts") && cat.at("ageCosts").contains(item)) {
        return cat.at("ageCosts").at(item);
    }
    return nlohmann::json::object();
}

std::unordered_map<std::string, int> Control::cost_of(const std::string& item, int team) const {
    nlohmann::json cost = base_cost(item);
    auto m = bonuses_.cost_multiplier(item, teams[team].civ, teams[team].leader, teams[team].era);
    std::unordered_map<std::string, int> out;
    double allm = m.count("all") ? m.at("all") : 1.0;
    for (auto& [k, v] : cost.items()) {
        double mult = m.count(k) ? m.at(k) : 1.0;
        int rounded = static_cast<int>(std::llround(v.get<double>() * allm * mult));
        if (rounded > 0) out[k] = rounded;
    }
    // Per-civ UNIQUE-tech cost TRANSFORMS (resource swaps the multiplier system
    // can't express -- it only scales existing keys). Applied last, gated on the
    // team owning the tech.
    const auto& tech = teams[team].tech;
    if (out.count("iron")) {
        // Emergency Fighter Program (Nazi): FIGHTER planes pay their iron cost in
        // WOOD -- fighters only, NOT bombers (bomber/heavy bomber/b29).
        if (tech.count("emergency fighter program")) {
            const auto& units = data_.catalog().at("units");
            bool is_bomber = (item == "bomber" || item == "heavy bomber" || item == "b29");
            if (!is_bomber && units.contains(item) && units.at(item).value("aerial", false)) {
                out["wood"] += out["iron"];
                out.erase("iron");
            }
        }
        // Meiji Restoration (Japan): the samurai pays its iron cost in FOOD.
        if (out.count("iron") && tech.count("meiji restoration") && item == "samurai") {
            out["food"] += out["iron"];
            out.erase("iron");
        }
    }
    // Joseph Stalin: MARKET techs cost a flat 100 food / 100 wood (replaces the
    // catalog cost entirely -- a transform the multiplier system can't express).
    if (bonuses_.is_market_tech(item) &&
        bonuses_.leader_name(teams[team].civ, teams[team].leader) == "Joseph Stalin") {
        out.clear();
        out["food"] = 100;
        out["wood"] = 100;
    }
    // 5-Year Plan (Soviet unique tech, internal key "420mm mortar"): factory,
    // airbase and artillery units all cost 20% less.
    if (teams[team].tech.count("420mm mortar")) {
        static const std::set<std::string> kFiveYearPlan = {
            "light tank", "tank", "heavy tank", "tiger tank", "tiger2 tank", "aa gun", "flak", // factory
            "biplane", "fighter", "jet fighter", "bomber", "heavy bomber", "b29", "ohka",      // airbase
            "artillery", "artillery1", "heavy artillery"};                                     // artillery
        if (kFiveYearPlan.count(item))
            for (auto& [k, v] : out) v = static_cast<int>(std::llround(v * 0.8));
    }
    return out;
}

bool Control::afford(const std::string& item, int team) const {
    auto& r = teams[team].res;
    for (auto& [k, v] : cost_of(item, team)) {
        auto it = r.find(k);
        if ((it == r.end() ? 0.0 : it->second) < v) return false;
    }
    return true;
}

void Control::pay(const std::string& item, int team) {
    auto& r = teams[team].res;
    for (auto& [k, v] : cost_of(item, team)) {
        r[k] -= v;
        teams[team].total_spent[k] += v; // post-game Economy tab (spending total)
    }
}

void Control::add(int team, const std::string& key, double amount) {
    teams[team].res[key] += amount;
}

bool Control::can_research(const std::string& key, int team) const {
    const auto& techs = data_.techs();
    if (!techs.contains(key)) return false;
    const auto& t = techs.at(key);
    const auto& td = teams[team];
    if (td.tech.count(key)) return false;
    if (td.era < bonuses_.leader_item_age(td.civ, td.leader, key, t.value("age", 0))) return false;
    if (t.contains("requiresTech") && t.at("requiresTech").is_string()) {
        std::string req = t.at("requiresTech").get<std::string>();
        if (!req.empty() && !td.tech.count(req)) return false;
    }
    if (t.contains("cost")) {
        for (auto& [k, v] : cost_of(key, team)) { // cost_of carries the leader/civ discounts
            auto it = td.res.find(k);
            if ((it == td.res.end() ? 0.0 : it->second) < v) return false;
        }
    }
    return true;
}

bool Control::is_tech(const std::string& item) const {
    if (TECH_ERA.count(item)) return true;
    for (auto& [name, list] : building_techs_) {
        (void)name;
        if (std::find(list.begin(), list.end(), item) != list.end()) return true;
    }
    return false;
}

std::vector<std::string> Control::available_units(const std::string& building, int team) const {
    const auto& t = teams[team];

    auto resolve = [&](std::string u) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& up : t.tech) {
                auto it = UPGRADE_MAP.find(up);
                // Don't upgrade INTO a unit this civ can't field (civ_has). Germany
                // can't have generic heavy tanks, so even if it somehow owned the
                // heavy-tank upgrade its "tank" slot must stay tank -> (civ sub)
                // Tiger, not vanish. Keeps the base unit rather than resolving to
                // an excluded one that then gets filtered out entirely below.
                if (it != UPGRADE_MAP.end() && it->second.first && *it->second.first == u &&
                    civ_has(it->second.second, t.civ)) {
                    u = it->second.second;
                    changed = true;
                    break;
                }
            }
        }
        return u;
    };

    std::vector<std::string> out;
    auto prod = PRODUCTION.find(building);
    if (prod == PRODUCTION.end()) return out;
    for (const auto& raw : prod->second) {
        std::string u = resolve(raw);
        auto sub = CIV_UNIT_SUB.find({t.civ, u});
        // Re-resolve AFTER the civ substitution so a substituted base unit still
        // follows its upgrade chain -- e.g. Germany's tank->Tiger slot must show
        // Tiger II once "tiger2 tank upgrade" is researched (previously the sub
        // happened after resolve and never re-checked, so the card was stuck on
        // Tiger I even though field units had already converted).
        if (sub != CIV_UNIT_SUB.end()) u = resolve(sub->second);
        if (std::find(out.begin(), out.end(), u) != out.end()) continue;
        auto era = UNIT_ERA.find(u);
        int need = bonuses_.leader_item_age(t.civ, t.leader, u, era != UNIT_ERA.end() ? era->second : 0);
        if (!civ_has(u, t.civ) || need > t.era) continue;
        if (CAMEL_UNITS.count(u) && !CAMEL_CIVS.count(t.civ)) continue;
        if (u == "aircraft carrier" && !CARRIER_CIVS.count(t.civ)) continue;
        if (OTTOMAN_ONLY.count(u) && t.civ != 8) continue;
        auto conly = CIV_ONLY_UNITS.find(u);
        if (conly != CIV_ONLY_UNITS.end() && conly->second != t.civ) continue;
        auto req = UNIT_REQUIRES.find(u);
        if (req != UNIT_REQUIRES.end() && !t.tech.count(req->second)) continue;
        auto treq = UNIT_TECH_REQ.find(u);
        if (treq != UNIT_TECH_REQ.end() && !t.tech.count(treq->second)) continue;
        out.push_back(u);
    }
    return out;
}

std::vector<std::string> Control::available_techs(const std::string& building, int team) const {
    const auto& t = teams[team];
    std::vector<std::string> out;
    auto bt = building_techs_.find(building);
    if (bt == building_techs_.end()) return out;
    for (const auto& k : bt->second) {
        auto era = TECH_ERA.find(k);
        int need = bonuses_.leader_item_age(t.civ, t.leader, k, era != TECH_ERA.end() ? era->second : 0);
        if (!civ_has(k, t.civ) || need > t.era || t.tech.count(k)) {
            continue;
        }
        auto owner = CIV_UPGRADE_OWNER.find(k);
        if (owner != CIV_UPGRADE_OWNER.end() && !owner->second.count(t.civ)) continue;
        auto pre = TECH_PREREQ.find(k);
        if (pre != TECH_PREREQ.end()) {
            bool missing = false;
            for (auto& p : pre->second) {
                if (!t.tech.count(p)) { missing = true; break; }
            }
            if (missing) continue;
        }
        auto om = UPGRADE_MAP.find(k);
        if (om != UPGRADE_MAP.end() && !civ_has(om->second.second, t.civ)) continue;
        out.push_back(k);
    }
    return out;
}

std::vector<std::string> Control::available_buildings(int team) const {
    const auto& t = teams[team];
    std::vector<std::string> out;
    for (auto& b : BUILDABLE) {
        auto era = BUILDING_ERA.find(b);
        if (!civ_has(b, t.civ) || (era != BUILDING_ERA.end() ? era->second : 0) > t.era) continue;
        auto treq = BUILDING_TECH_REQ.find(b);
        if (treq != BUILDING_TECH_REQ.end() && !t.tech.count(treq->second)) continue;
        out.push_back(b);
    }
    return out;
}

} // namespace ww::sim
