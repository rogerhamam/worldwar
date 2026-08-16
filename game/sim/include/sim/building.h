#pragma once
#include "sim/entity_common.h"

#include <string>
#include <vector>

namespace ww::sim {

// Data layout only -- direct port of Building.__init__ from
// game/entity.py (see unit.h's header comment: behavior lands in
// world.cpp/match.cpp once World exists).
struct Building {
    EntityCommon common;

    std::string name; // catalog key, e.g. "base", "barracks", "farm"
    std::string sprite;
    int armor = 0, pierce = 0;
    double hit_timer = 0.0;
    double highlight = 0.0;
    bool warned = false;
    bool big_death = false; // killed by a shell -> big explosion (vs normal rubble)
    bool deleted = false;   // foundation cancelled -> no rubble

    bool complete = true;
    double construction = 100.0; // 0..100; foundation until complete
    double prev_con = 0.0;       // last tick's construction %, for HP-growth delta
    // AI construction watchdog (control_ai.cpp's ai_tick). How long this
    // foundation has sat with its construction % not moving, and the % it was
    // last seen at. A foundation nobody can reach used to sit derelict for the
    // rest of the match AND block the AI's build order behind it; past a grace
    // period it stops counting against the work-in-progress cap, and past the
    // cancel window it is refunded and removed. Untouched for team 0's own
    // buildings in a normal skirmish -- only AI teams run the watchdog.
    double ai_stall_t = 0.0;
    double ai_stall_con = -1.0;
    double full_max_hp = 0.0;    // HP once fully built
    bool is_dropoff = false;     // base/house/refinery accept resource drop-off
    int size_w = 64, size_h = 64; // footprint in native px (== foot_w/foot_h at spawn)
    double foot_w = 0.0, foot_h = 0.0, foot_px = 0.0;
    bool solid = true;
    double build_radius = 0.0;
    double gather_x = 0.0, gather_y = 0.0; // rally point below the building
    bool rally_set = false;
    // Set alongside gather_x/gather_y when the rally point was placed
    // directly onto a live resource or an own unbuilt foundation (see
    // GameClient::right_click_order) -- kNullRef for a plain ground rally
    // point. Consumed by building_behavior.cpp's production-complete spawn:
    // a freshly-trained gatherer heads straight to work/build that specific
    // target instead of just walking to the rally point and standing idle.
    EntityRef rally_target = kNullRef;

    // farm-only fields (name == "farm")
    double max_farm_food = 200.0;
    double amount = 0.0;
    EntityRef occupied_by = kNullRef; // the villager currently gathering
    bool exhausted = false;

    std::vector<std::string> queue; // unit/age/tech items being produced
    double percent = 0.0;
    double acc = 0.0; // 1-second tick accumulator for production progress

    // Defensive-structure combat (tower / fortress / aa tower): catalog
    // attack/range/reload, cached at spawn. attack_px == 0 means "this
    // building doesn't shoot" (every non-defensive building). is_aa towers
    // only ever fire at aircraft; the rest fire at ground targets.
    double attack = 0.0;
    double range_px = 0.0;
    double reload = 1.0;
    double reload_timer = 0.0;
    double sight_px = 5.0 * 32.0;
    bool is_aa = false;

    // Airbase nuclear-bomb stockpile: built one at a time via the airbase card
    // once "atomic bomb" is researched. A heavy bomber / b29 that lands here
    // auto-loads one (count--), swapping its bombs for the single nuke.
    int nuke_count = 0;

    // Airbase toggle (command card): when true, a freshly-built plane PARKS at
    // this airbase (if a slot is free) instead of taking off immediately. Default
    // false = take off, matching the long-standing behavior.
    bool park_new_planes = false;

    // Whether this building actually blocks ground movement right now.
    // `solid` alone isn't enough: a foundation nobody has started work on
    // yet (construction == 0) is walk-THROUGH, so units path straight over
    // it instead of having to detour around a footprint that is, so far,
    // just a marker on the ground. This matters most in a packed base,
    // where a freshly-placed foundation could otherwise wall a villager
    // off from its resource or its drop-off the instant it was queued.
    // The moment the first hammer blow lands (construction > 0) it turns
    // solid for good -- and unit_behavior.cpp only lets that first blow
    // land while the footprint is clear of units, so nothing can ever be
    // sealed inside. Farms are never solid at all, built or not.
    bool blocks_movement() const { return solid && (complete || construction > 0.0); }
};

} // namespace ww::sim
