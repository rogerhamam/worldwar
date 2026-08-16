#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sim/bonuses.h"
#include "sim/catalog.h"

using namespace ww::sim;

namespace {
DataStore make_data() { return DataStore(WW_DATA_DIR); }
} // namespace

TEST_CASE("Bonuses::apply_unit applies UK rifle reload and civilian carry bonuses") {
    DataStore data = make_data();
    Bonuses bonuses(data);

    Unit rifleman;
    rifleman.name = "rifleman";
    rifleman.reload = 1.0;
    bonuses.apply_unit(rifleman, /*civ=*/0, /*leader=*/0, {}); // UK: rifleReload 0.75
    REQUIRE(rifleman.reload == Catch::Approx(0.75));

    Unit civilian;
    civilian.name = "civilian";
    civilian.max_carry = 10.0;
    bonuses.apply_unit(civilian, 0, 0, {}); // UK: civilianCarry +5
    REQUIRE(civilian.max_carry == Catch::Approx(15.0));
}

TEST_CASE("Bonuses::apply_tech_delta grants uniform bonus only to civilians") {
    DataStore data = make_data();
    Bonuses bonuses(data);

    Unit civilian;
    civilian.name = "civilian";
    civilian.common.max_hp = 30;
    civilian.common.hp = 30;
    bonuses.apply_tech_delta(civilian, "uniform");
    REQUIRE(civilian.common.max_hp == 40);
    REQUIRE(civilian.common.hp == 40);
    REQUIRE(civilian.armor == 1);
    REQUIRE(civilian.pierce == 1);

    Unit rifleman;
    rifleman.name = "rifleman";
    rifleman.common.max_hp = 40;
    bonuses.apply_tech_delta(rifleman, "uniform"); // only affects civilians
    REQUIRE(rifleman.common.max_hp == 40);
}

TEST_CASE("Bonuses::cost_multiplier applies UK cavalry discount") {
    DataStore data = make_data();
    Bonuses bonuses(data);
    auto m = bonuses.cost_multiplier("cavalry", 0);
    REQUIRE(m.at("all") == Catch::Approx(0.75));

    auto none = bonuses.cost_multiplier("swordsman", 0); // no matching bonus
    REQUIRE(none.empty());
}

TEST_CASE("Bonuses::gather_multiplier applies Germany iron bonus and tech stacking") {
    DataStore data = make_data();
    Bonuses bonuses(data);
    double base = bonuses.gather_multiplier(/*rtype=*/3, /*civ=*/2, {}); // Germany ironGather 1.1
    REQUIRE(base < 1.0); // dividing by >1 speeds up gathering (lower reload multiplier)

    double with_tech = bonuses.gather_multiplier(3, 2, {"smelting"});
    REQUIRE(with_tech < base);
}
