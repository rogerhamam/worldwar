#pragma once
#include "sim/entity_common.h"
#include "sim/resource.h"

namespace ww::sim {

// Data layout only -- direct port of Deer.__init__ (game/entity.py), a
// wandering food resource that flees units and can be hunted. Always
// kind "deer" / sprite "spr_deer" (no name/sprite fields needed, unlike
// Resource, since those never vary).
struct Deer {
    EntityCommon common;
    ResourceCommon res; // rtype = FOOD, amount/start_amount = 150

    double vx = 0.0, vy = 0.0;
    double move_timer = 0.0;
    int facing = 1;
    int health = 2;      // independent of the food it carries once dead
    double hunt_timer = 0.0;
    bool dead = false;   // whittled down to 0 HP -> carcass, harvestable
};

} // namespace ww::sim
