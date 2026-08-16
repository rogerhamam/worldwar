#include "sim/bonuses.h"

#include <algorithm>
#include <cmath>

namespace ww::sim {

namespace {
std::string underscored(std::string s) {
    std::replace(s.begin(), s.end(), ' ', '_');
    return s;
}

void scale_hp(double& max_hp, double& hp, double mult) {
    if (mult != 0.0 && mult != 1.0) {
        max_hp = std::round(max_hp * mult);
        hp = max_hp;
    }
}
} // namespace

const std::set<std::string> RIFLE = {"rifleman", "muscateer", "infantryman", "waffen",
                                      "elite waffen", "janissary", "royal janissary",
                                      "royal marine", "elite royal marine"};
const std::set<std::string> SWORD = {"swordsman", "swordsman2"};
const std::set<std::string> CAVALRY = {"cavalry", "cavalry2", "cavalry3", "camel", "camel corps"};
const std::set<std::string> INFANTRY = [] {
    std::set<std::string> s = RIFLE;
    s.insert(SWORD.begin(), SWORD.end());
    s.insert("waffen");
    s.insert("elite waffen");
    return s;
}();
const std::set<std::string> TANK = {"light tank", "tank", "heavy tank", "tiger tank", "tiger2 tank"};

// Leader-bonus unit classes (local to this file; not part of the public class
// list above). FIGHTER/BOMBER are the air-superiority vs. strategic-bombing
// split; ARTILLERY is both tiers of field gun.
namespace {
const std::set<std::string> BOMBER = {"bomber", "heavy bomber", "b29"};
const std::set<std::string> ARTILLERY_UNITS = {"artillery", "artillery1", "heavy artillery"};
// An armed (military) ship -- excludes fishing boats and transports, which have
// no base attack. Same distinction naval_hegemony uses.
inline bool is_warship(const Unit& u) { return u.common.is_ship && u.base_attack > 0.0; }
} // namespace

const nlohmann::json& Bonuses::civ_effects(int civ) const {
    static const nlohmann::json empty = nlohmann::json::object();
    if (civ < 0 || static_cast<size_t>(civ) >= civs_.size()) return empty;
    auto it = civs_[civ].find("effects");
    return it != civs_[civ].end() ? *it : empty;
}

std::string Bonuses::leader_name(int civ, int leader) const {
    if (civ < 0 || static_cast<size_t>(civ) >= civs_.size()) return "";
    const auto& c = civs_[civ];
    auto lit = c.find("leaders");
    if (lit == c.end() || leader < 0 || static_cast<size_t>(leader) >= lit->size()) return "";
    return (*lit)[leader].value("name", "");
}

void Bonuses::apply_unit(Unit& unit, int civ, int leader, int era, const std::set<std::string>& tech) const {
    const auto& e = civ_effects(civ);
    const std::string& n = unit.name;
    std::string ld = leader_name(civ, leader);

    if (n == "civilian") {
        unit.max_carry += e.value("civilianCarry", 0.0);
        unit.speed_px *= e.value("civilianSpeed", 1.0);
    }
    if (RIFLE.count(n)) unit.reload *= e.value("rifleReload", 1.0);
    if (SWORD.count(n)) unit.reload *= e.value("swordReload", 1.0);
    if (CAVALRY.count(n)) scale_hp(unit.common.max_hp, unit.common.hp, e.value("cavalryHP", 1.0));
    if (INFANTRY.count(n)) {
        unit.armor += e.value("infantryArmor", 0);
        unit.pierce += e.value("infantryPierce", 0);
        unit.speed_px *= e.value("infantrySpeed", 1.0);
    }
    // 32.0 here (and at apply_tech_delta's ballistics/binoculars deltas
    // below) is GML's tile size, not sim::TILE -- this file can't include
    // world.h (world.h includes bonuses.h, so the reverse would be
    // circular). They're equal now that TILE == 32, but kept as literals
    // rather than references to a constant this file can't see.
    if (unit.common.is_ship) unit.range_px += e.value("shipRange", 0.0) * 32.0;
    // France's artillery-range bonus (civs.json "artilleryRange", in tiles);
    // both the tier-1 field cannon and full artillery count as "artillery".
    if (n == "artillery" || n == "artillery1") unit.range_px += e.value("artilleryRange", 0.0) * 32.0;
    // ---- Per-leader UNIT-STAT bonuses (see civ_data.h leader_bonuses for the
    // player-facing wording). Only flat stat tweaks live here; the rate/cost/
    // economy/era/combat leader bonuses have their own hooks: cost_multiplier
    // (Khrushchev/de Gaulle/Petain/Mao), gather_multiplier (Messe/Ataturk),
    // leader_train_mult/leader_build_mult (Hitler/FDR), building_behavior
    // (Churchill era, Hirohito/George VI economic tech, Mussolini age), the
    // projectile blast (Truman), UNIT_ERA/TECH_ERA (Goering jets), the market
    // (Stalin), spawn air_capacity (Yamamoto's +1), and the movement/combat
    // code (Li Zongren trees, Chiang vs-tanks, Enver fire-on-the-move).
    (void)era;
    const bool warship = is_warship(unit);
    if (ld == "Bernard Montgomery" && TANK.count(n)) scale_hp(unit.common.max_hp, unit.common.hp, 1.20);
    else if (ld == "Harry S. Truman" && BOMBER.count(n)) unit.attack *= 1.25; // +blast at projectile spawn
    else if (ld == "Dwight D. Eisenhower" && INFANTRY.count(n)) scale_hp(unit.common.max_hp, unit.common.hp, 1.33);
    else if (ld == "Erwin Rommel" && TANK.count(n)) unit.speed_px *= 1.20;
    else if (ld == "Nikita Khrushchev" && ARTILLERY_UNITS.count(n)) unit.attack *= 1.25;
    else if (ld == "Hideki Tojo" && INFANTRY.count(n)) unit.speed_px *= 1.20;
    else if (ld == "Isoroku Yamamoto" && warship) unit.attack *= 1.20; // +1 carrier plane handled at spawn
    else if (ld == "Italo Balbo" && (warship || n == "light tank")) unit.reload *= 0.75;
    else if (ld == "Napoleon" && ARTILLERY_UNITS.count(n)) unit.reload *= 0.75;
    else if (ld == "Mehmed V" && CAVALRY.count(n)) { unit.armor += 1; unit.pierce += 1; unit.reload *= 0.9; }
    // Li Zongren: infantry path/move straight through trees (Unit::phase_trees,
    // honored by World::passable and the movement code).
    if (ld == "Li Zongren" && INFANTRY.count(n)) unit.phase_trees = true;
    // Nazi Germany: jet- and rocket-powered units hit 10% harder (civs.json
    // "jetRocketDamage") -- currently the Jet Fighter and the Ballistic Missile.
    if (n == "jet fighter" || n == "ballistic missile") unit.attack *= e.value("jetRocketDamage", 1.0);

    for (const auto& t : tech) apply_tech_delta(unit, underscored(t));
}

double Bonuses::leader_train_mult(int civ, int leader, const std::string& item) const {
    // Adolf Hitler: civilians are produced 15% faster (any building that trains
    // them -- normally the town centre / base).
    if (item == "civilian" && leader_name(civ, leader) == "Adolf Hitler") return 1.15;
    return 1.0;
}

double Bonuses::leader_build_mult(int civ, int leader) const {
    // Franklin D. Roosevelt: villagers raise buildings 30% faster (New Deal).
    if (leader_name(civ, leader) == "Franklin D. Roosevelt") return 1.30;
    return 1.0;
}

bool Bonuses::is_economic_tech(const std::string& name) const {
    static const std::set<std::string> kEcon = {
        "trade agreement", "fertilizer", "horse wagon", "irrigation", "mobile sawmill",
        "pesticide", "power saw", "electric drill", "fracking", "smelting", "beneficiation"};
    std::string s = name;
    std::replace(s.begin(), s.end(), '_', ' ');
    return kEcon.count(s) != 0;
}

bool Bonuses::is_market_tech(const std::string& name) const {
    static const std::set<std::string> kMkt = {"trade agreement", "fertilizer", "horse wagon",
                                               "irrigation", "mobile sawmill", "pesticide", "power saw"};
    std::string s = name;
    std::replace(s.begin(), s.end(), '_', ' ');
    return kMkt.count(s) != 0;
}

int Bonuses::leader_item_age(int civ, int leader, const std::string& item, int base_age) const {
    // Hermann Goering: the whole jet chain comes an age early. "jet fighter
    // upgrade" is the tech that actually converts fighters to jets, so it must
    // shift too -- otherwise jet engine unlocks early but the jets themselves
    // stay locked until Scientific.
    if (leader_name(civ, leader) == "Hermann Goering" &&
        (item == "jet engine" || item == "jet fighter" || item == "jet fighter upgrade"))
        return std::max(0, base_age - 1);
    return base_age;
}

void Bonuses::apply_tech_delta(Unit& u, const std::string& tech) const {
    const std::string& n = u.name;
    if (tech == "uniform" && n == "civilian") {
        u.common.max_hp += 10; u.common.hp += 10; u.armor += 1; u.pierce += 1;
    } else if (tech == "refined_steel" && (INFANTRY.count(n) || CAVALRY.count(n)) && u.melee) {
        u.attack += 1;
    } else if (tech == "alloys" && (INFANTRY.count(n) || CAVALRY.count(n)) && u.melee) {
        u.attack += 1;
    } else if (tech == "electric_arc_furnace" && (INFANTRY.count(n) || CAVALRY.count(n)) && u.melee) {
        u.attack += 2;
    } else if (tech == "steel_plane_armour" && u.common.is_air) {
        u.pierce += 2;
    } else if (tech == "composite_plane_armour" && u.common.is_air) {
        u.pierce += 2;
    } else if (tech == "blowback_reload" && RIFLE.count(n)) {
        u.reload *= 0.82;
    } else if (tech == "bolt_action_rifle" && RIFLE.count(n)) {
        u.attack += 1;      // industrial-era firearm upgrade
        u.range_px += 32.0; // +1 tile -- each firearm upgrade also extends reach
    } else if (tech == "semi_automatic_rifle" && RIFLE.count(n)) {
        u.attack += 1;
        u.reload *= 0.9;    // war-era: a bit harder-hitting and faster
        u.range_px += 32.0; // +1 tile
    } else if (tech == "assault_rifle" && RIFLE.count(n)) {
        // scientific-era barracks tech: firearm infantry hit harder and reach further
        u.attack += 1;
        u.range_px += 32.0; // +1 tile
    } else if (tech == "ballistics" && !u.melee && !u.common.is_air && !u.is_gatherer && u.attack > 0) {
        u.range_px += 32.0;
    } else if (tech == "diesel_engine" && TANK.count(n)) {
        u.speed_px *= 1.2;
    } else if (tech == "hydrodynamics" && u.common.is_ship) {
        u.speed_px *= 1.1; // shipyard war-era tech: all ships 10% faster
    } else if (tech == "naval_armour" && u.common.is_ship) {
        u.armor += 1;
        u.pierce += 1;
        scale_hp(u.common.max_hp, u.common.hp, 1.1); // +10% hull
    } else if (tech == "trade_agreement") {
        // Effect is applied in Control::trade (fee reduction); no per-unit delta.
    } else if (tech == "horse_wagon" && n == "civilian") {
        u.max_carry += 5.0;
    } else if (tech == "binoculars" && INFANTRY.count(n)) {
        u.sight_px += 2 * 32.0;
    } else if (tech == "radar" && (u.common.is_ship || u.common.is_air)) {
        u.sight_px += 2 * 32.0; // +2 tiles LOS for ships and planes (buildings handled in world.cpp fog)
        u.range_px += 1 * 32.0; // +1 tile attack range for ships and planes
    } else if (tech == "naval_hegemony" && u.common.is_ship) {
        // UK unique: all ships move 15% faster; ARMED (military) ships also hit
        // 15% harder. base_attack > 0 distinguishes warships from fishing boats
        // / transports.
        u.speed_px *= 1.15;
        if (u.base_attack > 0.0) u.attack *= 1.15;
    }
}

void Bonuses::apply_building(Building& b, int civ, int /*leader*/, const std::set<std::string>& tech) const {
    const auto& e = civ_effects(civ);
    double mult = e.value("buildingHP", 1.0);
    if (tech.count("steel_frame") || tech.count("steel frame")) mult *= 1.30;
    if (mult != 1.0 && b.name != "base") scale_hp(b.common.max_hp, b.common.hp, mult);

    if (b.name == "farm") {
        double bonus = (tech.count("irrigation") ? 75.0 : 0.0) +
                        (tech.count("fertilizer") ? 125.0 : 0.0) +
                        (tech.count("pesticide") ? 200.0 : 0.0);
        if (bonus) {
            b.max_farm_food += bonus;
            if (!b.exhausted) b.amount = b.max_farm_food;
        }
    }
}

std::unordered_map<std::string, double> Bonuses::cost_multiplier(const std::string& item, int civ, int leader,
                                                                 int era) const {
    const auto& e = civ_effects(civ);
    std::unordered_map<std::string, double> m;
    auto mul = [&](const char* key, double f) { m[key] = (m.count(key) ? m[key] : 1.0) * f; };
    if ((item == "cavalry" || item == "cavalry2" || item == "cavalry3") && e.contains("cavalryCost")) {
        m["all"] = e.value("cavalryCost", 1.0);
    }
    if (RIFLE.count(item) && e.contains("infantryCost")) {
        m["all"] = e.value("infantryCost", 1.0);
    }
    // China's flat discount on every LAND military unit (infantry, cavalry/
    // camels, tanks, artillery, AA). Multiplied into any existing "all" factor
    // so it stacks cleanly with a per-type discount rather than clobbering it.
    if (e.contains("landMilitaryCost")) {
        static const std::set<std::string> kLandMilExtra = {"artillery", "artillery1", "aa gun", "flak"};
        if (INFANTRY.count(item) || CAVALRY.count(item) || TANK.count(item) || kLandMilExtra.count(item)) {
            double v = e.value("landMilitaryCost", 1.0);
            m["all"] = m.count("all") ? m["all"] * v : v;
        }
    }
    if (item == "farm" && e.contains("farmCost")) {
        m["all"] = e.value("farmCost", 1.0);
    }
    if (TANK.count(item) && e.contains("tankIronCost")) {
        m["iron"] = e.value("tankIronCost", 1.0);
    }
    if ((item == "biplane" || item == "fighter" || item == "jet fighter" || item == "bomber" ||
         item == "heavy bomber") && e.contains("planeCost")) {
        m["all"] = e.value("planeCost", 1.0);
    }
    if (item == "base" && e.value("baseNoIron", false)) {
        m["iron"] = 0.0;
    }
    // Germany's flat house wood-cost ("houseCost": {"wood": 10}) is handled
    // at the base-cost level in the Python version too (see bonuses.py's
    // comment) -- not a multiplier, so nothing to do here.

    // ---- Per-leader cost bonuses.
    std::string ld = leader_name(civ, leader);
    if (ld == "Georgy Zhukov" && TANK.count(item)) mul("all", 0.75);             // tanks -25%
    else if (ld == "Charles de Gaulle" && item == "base") mul("wood", 0.5);       // bases -50% wood
    else if (ld == "Philippe Petain" && (INFANTRY.count(item) || item == "light tank"))
        mul("oil", 0.8);                                                          // infantry & light tanks -20% oil
    else if (ld == "Mao Zedong" && item == "civilian")
        mul("all", 1.0 - (0.10 + 0.05 * std::clamp(era, 0, 3)));                 // villagers -10/15/20/25% by era
    else if (ld == "Benito Mussolini" &&
             (item == "industrial" || item == "war" || item == "scientific"))
        mul("all", 0.8);                                                          // age-ups -20%
    else if (ld == "Hermann Goering" && (item == "jet engine" || item == "jet fighter upgrade"))
        mul("all", 0.5);                                                          // jet TECHS half price (not the jet fighter unit)
    else if (ld == "George VI" && is_economic_tech(item)) mul("food", 0.75);      // economic techs -25% food
    return m;
}

double Bonuses::gather_multiplier(int rtype, int civ, int leader, const std::set<std::string>& tech) const {
    const auto& e = civ_effects(civ);
    double m = 1.0;
    // Per-leader gather bonuses (lower multiplier = faster).
    std::string ld = leader_name(civ, leader);
    if (rtype == 0 && ld == "Giovanni Messe") m *= 0.90;   // farmers work 10% faster
    if (rtype == 2 && ld == "Mustafa Ataturk") m *= 0.90;  // oil gathered 10% faster
    // 5-Year Plan (Soviet unique tech, internal key "420mm mortar"): civilians
    // gather wood/oil/iron 3% faster (food unaffected).
    if ((rtype == 1 || rtype == 2 || rtype == 3) && tech.count("420mm mortar")) m *= 0.97;
    if (rtype == 1) { // wood
        if (e.contains("woodGather")) m /= e.value("woodGather", 1.0);
        if (tech.count("power_saw")) m *= 0.90;
        if (tech.count("mobile_sawmill")) m *= 0.90;
    }
    if (rtype == 2) { // oil
        if (tech.count("electric_drill")) m *= 0.85;
        if (tech.count("fracking")) m *= 0.85;
    }
    if (rtype == 3) { // iron
        if (e.contains("ironGather")) m /= e.value("ironGather", 1.0);
        if (tech.count("smelting")) m *= 0.85;
        if (tech.count("beneficiation")) m *= 0.85;
    }
    return m;
}

} // namespace ww::sim
