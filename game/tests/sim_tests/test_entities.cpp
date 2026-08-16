// Compile/sanity check for the entity data-layout headers (task: "Port
// entity kinds"). Behavior isn't implemented yet -- see the header
// comments in sim/include/sim/unit.h etc. -- so these tests only check
// that the structs exist, default-construct sanely, and that SlotMap +
// EntityRef round-trip correctly for each kind.
#include <catch2/catch_test_macros.hpp>

#include "sim/building.h"
#include "sim/deer.h"
#include "sim/entity_common.h"
#include "sim/projectile.h"
#include "sim/resource.h"
#include "sim/unit.h"

using namespace ww::sim;

TEST_CASE("Entity kinds default-construct with sane values") {
    Unit u;
    REQUIRE(u.common.alive);
    REQUIRE(u.facing == 1);
    REQUIRE_FALSE(u.attack_target.valid());

    Building b;
    REQUIRE(b.complete);
    REQUIRE(b.solid);
    REQUIRE(b.queue.empty());

    Resource r;
    REQUIRE(r.res.amount == 0.0);

    Deer d;
    REQUIRE(d.health == 2);
    REQUIRE_FALSE(d.dead);

    Projectile p;
    REQUIRE_FALSE(p.target.valid());
}

TEST_CASE("SlotMap<Unit> insert/get/erase round-trips via EntityRef") {
    SlotMap<Unit> units;
    Unit u;
    u.common.id = 7;
    u.name = "rifleman";
    uint32_t slot = units.insert(u);
    EntityRef ref{EntityKind::Unit, slot, units.generation_of(slot)};

    Unit* got = units.get(ref);
    REQUIRE(got != nullptr);
    REQUIRE(got->common.id == 7);
    REQUIRE(got->name == "rifleman");

    units.erase(slot);
    REQUIRE(units.get(ref) == nullptr); // generation bumped -> stale ref resolves to null

    // Reusing the freed slot for a new entity must not resurrect the old ref.
    Unit u2;
    u2.common.id = 9;
    uint32_t slot2 = units.insert(u2);
    REQUIRE(slot2 == slot); // slot map reuses freed slots
    EntityRef stale = ref;
    REQUIRE(units.get(stale) == nullptr);
}
