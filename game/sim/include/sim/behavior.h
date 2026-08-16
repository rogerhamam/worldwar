#pragma once
#include "sim/entity_common.h"

namespace ww::sim {

class World;
struct Unit;
struct Building;
struct Deer;
struct Projectile;
struct Resource;

// Per-tick behavior for each entity kind, split out of the data-only
// structs (unit.h/building.h/...) because it needs World (spatial grid,
// control, spawning) to exist. `self` is the entity's own ref, needed for
// things like "don't target yourself" and building cross-references.
void update_unit(EntityRef self, Unit& u, double dt, World& world);
// `is_air` units only -- dispatched from update_unit. See
// aircraft_behavior.cpp's header comment.
void update_aircraft(EntityRef self, Unit& u, double dt, World& world);
void update_building(EntityRef self, Building& b, double dt, World& world);
void update_deer(EntityRef self, Deer& d, double dt, World& world);
void update_projectile(EntityRef self, Projectile& p, double dt, World& world);
void update_resource(Resource& r);

} // namespace ww::sim
