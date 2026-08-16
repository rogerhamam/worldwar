#include <catch2/catch_test_macros.hpp>

#include "sim/control.h"

#include <algorithm>

using namespace ww::sim;

namespace {
DataStore make_data() { return DataStore(WW_DATA_DIR); }
} // namespace

TEST_CASE("pop_cost matches the catalog table, defaulting to 1") {
    REQUIRE(pop_cost("tank") == 4);
    REQUIRE(pop_cost("b29") == 6);
    REQUIRE(pop_cost("civilian") == 1); // not in POP_COST -> default
}

TEST_CASE("Control::civ_has reflects civ_exclude.json") {
    DataStore data = make_data();
    Bonuses bonuses(data);
    Control ctrl(data, bonuses, 2, 100);

    REQUIRE_FALSE(ctrl.civ_has("waffen", 0)); // UK can't field Waffen SS
    REQUIRE(ctrl.civ_has("waffen", 2));       // Germany can
}

TEST_CASE("Control::afford/pay respect team resources and civ cost discounts") {
    DataStore data = make_data();
    Bonuses bonuses(data);
    Control ctrl(data, bonuses, 2, 100);
    ctrl.teams[0].civ = 0; // UK: cavalryCost 0.75

    ctrl.teams[0].res["food"] = 1000;
    REQUIRE(ctrl.afford("cavalry", 0));
    auto cost = ctrl.cost_of("cavalry", 0);
    ctrl.pay("cavalry", 0);
    REQUIRE(ctrl.teams[0].res["food"] == 1000 - cost.at("food"));
}

TEST_CASE("Control::available_units resolves the upgrade chain and gates by era/requirement") {
    DataStore data = make_data();
    Bonuses bonuses(data);
    Control ctrl(data, bonuses, 2, 100);
    ctrl.teams[0].civ = 0; // UK

    // No tech researched, era 0: rifleman/infantryman need their upgrade
    // tech first, so only the base-tier units show up.
    auto base_units = ctrl.available_units("barracks", 0);
    REQUIRE(base_units == std::vector<std::string>{"swordsman", "muscateer"});

    // Research "rifleman upgrade" and advance to era 1: muscateer resolves
    // forward to rifleman (deduped against the raw "rifleman" entry), and
    // swordsman2 is still not researched so swordsman stays.
    ctrl.teams[0].era = 1;
    ctrl.teams[0].tech.insert("rifleman upgrade");
    auto upgraded = ctrl.available_units("barracks", 0);
    REQUIRE(upgraded == std::vector<std::string>{"swordsman", "rifleman"});
}

TEST_CASE("Control::available_units substitutes Germany's tiger tank for heavy tank") {
    DataStore data = make_data();
    Bonuses bonuses(data);
    Control ctrl(data, bonuses, 2, 100);
    ctrl.teams[0].civ = 2; // Germany
    ctrl.teams[0].era = 3;
    ctrl.teams[0].tech.insert("heavy tank upgrade");

    auto units = ctrl.available_units("factory", 0);
    REQUIRE(std::find(units.begin(), units.end(), "tiger tank") != units.end());
    REQUIRE(std::find(units.begin(), units.end(), "heavy tank") == units.end());
}

TEST_CASE("Control::available_buildings gates by era") {
    DataStore data = make_data();
    Bonuses bonuses(data);
    Control ctrl(data, bonuses, 2, 100);
    ctrl.teams[0].civ = 0;
    ctrl.teams[0].era = 0;

    auto era0 = ctrl.available_buildings(0);
    REQUIRE(std::find(era0.begin(), era0.end(), "factory") == era0.end()); // era 1
    REQUIRE(std::find(era0.begin(), era0.end(), "house") != era0.end());   // era 0
}

TEST_CASE("Control::is_tech distinguishes techs/upgrades from plain units") {
    DataStore data = make_data();
    Bonuses bonuses(data);
    Control ctrl(data, bonuses, 2, 100);

    REQUIRE(ctrl.is_tech("rifleman upgrade"));
    REQUIRE_FALSE(ctrl.is_tech("rifleman"));
}
