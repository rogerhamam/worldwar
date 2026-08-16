#pragma once
#include "sim/entity_common.h"

#include <string>

namespace ww::sim {

// Fields shared by Resource and Deer (Deer is a Resource subclass in
// game/entity.py, ported here as composition instead of inheritance).
struct ResourceCommon {
    int rtype = 0; // FOOD/WOOD/OIL/IRON (see catalog constants)
    double amount = 0.0;
    double start_amount = 0.0;
};

// Data layout only -- direct port of Resource.__init__ (game/entity.py).
// Behavior (the trivial Resource::update -- amount<=0 => dead) lands
// alongside World, matching the rest of the entity kinds.
struct Resource {
    EntityCommon common;
    std::string name; // "tree" | "palm" | "berry" | "oil" | "iron" | "fish"
    std::string sprite;
    ResourceCommon res;
};

} // namespace ww::sim
