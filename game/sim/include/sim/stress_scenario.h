#pragma once
#include "sim/control.h"
#include "sim/rng.h"
#include "sim/world.h"

namespace ww::sim {

// Debug/perf-testing only: called once, right after new_skirmish(), to
// instantly fill out every active team's base into a full-scale late-game
// army -- one of every production building plus a stack of houses, and a
// mixed roster of civilians/infantry/tanks/ships/planes totalling roughly
// max_pop worth of population -- instead of waiting for a real (or AI)
// economy to grow into one. See GameClient's stress-test debug entry
// point for how this is wired up to an actual playable/watchable match.
void populate_stress_test(World& world, Control& control, Rng& rng);

} // namespace ww::sim
