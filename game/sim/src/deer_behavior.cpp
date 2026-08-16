// Deer behavior: direct port of Deer.update (game/entity.py).
#include "sim/behavior.h"
#include "sim/deer.h"
#include "sim/world.h"

#include <cmath>

namespace ww::sim {

void update_deer(EntityRef /*self*/, Deer& d, double dt, World& world) {
    if (d.dead) {
        d.common.hp = d.res.amount;
        d.common.max_hp = d.res.start_amount;
        if (d.res.amount <= 0) d.common.alive = false;
        return;
    }
    d.common.hp = d.health;
    d.common.max_hp = 2;

    auto is_alive_unit = [](EntityRef, EntityCommon& c) { return c.kind == EntityKind::Unit && c.alive; };

    // ONE radius-48 neighbour query serves BOTH checks below. The hunter test
    // wants the nearest unit within 24px and the threat test the nearest within
    // 48px -- but a deer's nearest unit within 24 is necessarily its nearest
    // within 48, so deriving the hunter from this single result is byte-identical
    // to the two separate nearest() calls it replaces (deterministic: nearest()
    // draws no RNG, and the movement RNG below is untouched). With deer counts
    // scaling into the dozens on the current larger maps, halving the per-deer
    // grid queries was a large frame-time win. (Was two nearest() calls.)
    EntityRef threat = world.nearest(d.common.x, d.common.y, 48, is_alive_unit);
    double tdx = 0.0, tdy = 0.0, tdist2 = 0.0;
    if (threat.valid()) {
        EntityCommon* t = world.common(threat);
        tdx = t->x - d.common.x; tdy = t->y - d.common.y;
        tdist2 = tdx * tdx + tdy * tdy;
    }

    // Hunter (being foraged): a unit within 24px. dist^2 < 24^2 exactly matches
    // the old nearest(x,y,24,...) radius cutoff (World::nearest compares squared).
    if (threat.valid() && tdist2 < 24.0 * 24.0) {
        d.hunt_timer -= dt;
        if (d.hunt_timer <= 0) {
            d.hunt_timer = 0.4;
            d.health -= 1;
            if (d.health <= 0) {
                d.dead = true;
                world.events.push({EventType::Sound, "forage", d.common.x, d.common.y, 200, kNullRef, ""});
            }
        }
        return;
    }

    d.move_timer -= dt;
    if (threat.valid()) {
        double distv = std::hypot(tdx, tdy);
        if (distv > 0 && distv < 38 && world.rng.chance(0.5)) {
            d.vx = -tdx / distv; d.vy = -tdy / distv;
            d.move_timer = 0.7;
        }
    }
    if (d.move_timer <= 0) {
        if (world.rng.chance(0.4)) {
            d.vx = d.vy = 0.0;
        } else {
            double a = world.rng.uniform(0, 2 * M_PI);
            d.vx = std::cos(a); d.vy = std::sin(a);
        }
        d.move_timer = world.rng.uniform(1.5, 3.5);
    }
    if (d.vx != 0.0 || d.vy != 0.0) {
        double sp = 27.0 * dt;
        double nx = d.common.x + d.vx * sp, ny = d.common.y + d.vy * sp;
        if (world.passable(false, false, nx, ny)) {
            d.common.x = nx; d.common.y = ny;
            d.facing = d.vx > 0 ? 1 : -1;
        } else {
            d.vx = -d.vx; d.vy = -d.vy;
        }
    }
}

void update_resource(Resource& r) {
    r.common.hp = r.res.amount;
    r.common.max_hp = r.res.start_amount;
    if (r.res.amount <= 0) r.common.alive = false;
}

} // namespace ww::sim
