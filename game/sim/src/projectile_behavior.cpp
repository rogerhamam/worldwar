// Projectile behavior: direct port of Projectile.update/_explode
// (game/entity.py), including the bomb (aircraft-dropped) path -- see
// World::spawn_bomb (world.cpp) and aircraft_behavior.cpp.
#include "sim/behavior.h"
#include "sim/projectile.h"
#include "sim/unit.h"
#include "sim/world.h"

#include <cmath>
#include <cstdlib>
#include <set>

namespace ww::sim {

namespace {

// Land "big shell" units -- tank/heavy-unit impacts (obj_bullet/Destroy.gml
// in the original), unchanged generic explosion+splash treatment, just
// correctly sized now (spr_explosion, not the bomb-only mushroom).
const std::set<std::string> LAND_SHELL_UNITS = {
    "tank", "heavy tank", "tiger tank", "tiger2 tank", "light tank",
};

// The obj_missile-firing set (see world.cpp's spawn_projectile /
// unit_behavior.cpp's ship_shot_origin): ships + artillery. Frigate is
// deliberately excluded below -- per obj_missile/Destroy.gml, a frigate's
// shot never spawns any explosion object at all (no splash, no shockwave/
// flame/fire_cloud), just the direct single-target hit already applied by
// its own per-tick collision check.

bool is_valid_combat_target(World& world, EntityCommon* e, int shooter_team) {
    return e && e->alive && e->team >= 0 && !world.control.allied(e->team, shooter_team) &&
           (e->kind == EntityKind::Unit || e->kind == EntityKind::Building);
}

void splash_damage(World& world, double x, double y, double radius, double half_box,
                    double pow_, int shooter_team, bool collateral = false, bool flat = false,
                    double bonus_vs_building = 0.0) {
    for (auto ref : world.grid.query(x, y, radius)) {
        EntityCommon* e = world.common(ref);
        // Collateral (bombs/nukes) hits ANY unit/building in the blast, incl.
        // the owner's own; normal shots skip allied targets.
        bool valid = collateral
            ? (e && e->alive && e->team >= 0 &&
               (e->kind == EntityKind::Unit || e->kind == EntityKind::Building))
            : is_valid_combat_target(world, e, shooter_team);
        if (!valid) continue;
        if (e->is_air) continue; // a ground blast (bomb/nuke/shell splash) never reaches aircraft
        double dist = std::hypot(e->x - x, e->y - y);
        if (dist > half_box) continue;
        // Tapered blast falls off linearly to zero at the rim. `flat` instead
        // deals FULL power anywhere in the radius -- used by artillery so a
        // scattered/near-miss shell still hits as hard as a direct one.
        double falloff = flat ? 1.0 : (1.0 - dist / half_box);
        // Ballistic missile's anti-fortification bonus lands on buildings only.
        double bonus = (e->kind == EntityKind::Building) ? bonus_vs_building : 0.0;
        world.hurt(ref, std::max((pow_ + bonus) * falloff - world.pierce_of(ref), 1.0));
        if (e->kind == EntityKind::Building) world.get_building(ref)->big_death = true;
    }
}

// Direct port of obj_missile/Destroy.gml's shockwave/flame/fire_cloud
// chain, called for every non-frigate obj_missile shooter (battleship,
// yamato, destroyer, torpedo boat, artillery) regardless of whether this
// particular shot also gets splash damage (torpedo boat doesn't, the rest
// do -- see the two call sites below).
void spawn_missile_impact_fx(World& world, const std::string& origin_name, double x, double y) {
    if (world.rng.chance(0.2) || origin_name == "yamato") {
        world.events.push({EventType::Effect, "spr_shockwave", x, y, 0, kNullRef, ""});
    }
    int n = 0, range = 0;
    if (origin_name == "battleship" || origin_name == "yamato") { n = 3; range = 2; }
    else if (origin_name == "destroyer") { n = 2; range = 1; }
    for (int i = 0; i < n; ++i) {
        if (world.rng.chance(0.45)) {
            double fx = x + TILE * world.rng.randint(-range, range);
            double fy = y + TILE * world.rng.randint(-range, range);
            world.events.push({EventType::Effect, "spr_flame", fx, fy, 0, kNullRef, ""});
            // Unlike the cosmetic Effect above (a few seconds, no collision,
            // see game_client.h's ClientEffect), this actually burns
            // whoever's standing in it -- see World::FireHazard.
            world.fires.push_back({fx, fy, 6.0});
        }
    }
    if (world.rng.chance(0.3) || origin_name == "yamato") {
        double fx = x + world.rng.randint(16, 40), fy = y + world.rng.randint(16, 40);
        world.events.push({EventType::Effect, "spr_fire_cloud", fx, fy, 0, kNullRef, ""});
    }
}

// Ballistic missile impact: the same visuals a bomber's bomb makes -- a big
// spr_explosion_large blast + a lingering fire -- plus its 2x splash radius and
// anti-building bonus. Called from update_projectile's collision check and its
// arc-end (an attack-ground shot that reaches the point without striking a
// unit), since the ballistic flies the ship-arc path, not the lob path.
void ballistic_impact(Projectile& p, World& world, double x, double y) {
    p.common.alive = false;
    world.events.push({EventType::Effect, "spr_explosion_large", x, y, 1.0, kNullRef, ""});
    world.events.push({EventType::Sound, "explosion", x, y, 0, kNullRef, ""});
    splash_damage(world, x, y, 48 * p.splash_mult, 40 * p.splash_mult, p.pow, p.common.team,
                  /*collateral=*/true, /*flat=*/true, p.bonus_vs_building);
    world.events.push({EventType::Effect, "spr_flame", x, y, 0, kNullRef, ""});
    world.fires.push_back({x, y, 6.0});
}

void explode(Projectile& p, World& world) {
    p.common.alive = false;
    if (p.nuke) {
        // Atomic bomb: a huge mushroom cloud and a wide blast that levels
        // everything nearby, friendly units included (collateral). Tuned for a
        // cinematic detonation -- a massive kill radius plus the client-side
        // white flash / screen shake / smoke cloud (see the "nuke_flash" event
        // below, consumed by GameClient::drain_events).
        world.events.push(
            {EventType::Effect, "spr_explosion_mushroom", p.tx, p.ty, 4.0, kNullRef, ""});
        // Special client-only cue: a full-screen white flash + screen shake +
        // rising smoke cloud, keyed off this sentinel sprite name (not drawn as
        // a sprite). Scale carries the blast radius so the shake can scale too.
        world.events.push({EventType::Effect, "nuke_flash", p.tx, p.ty, 260.0, kNullRef, ""});
        world.events.push({EventType::Sound, "explosion", p.tx, p.ty, 0, kNullRef, ""});
        // Wide, devastating blast: query 300px, damage box ~260px (~8 tiles),
        // floored at 900 -- levels a whole base cluster.
        splash_damage(world, p.tx, p.ty, 300, 260, std::max(p.pow, 900.0), p.common.team,
                      /*collateral=*/true);
        // Lingering firestorm: a wide spread of fire patches across the blast
        // that keep burning whatever survived (World::fires damage sweep). Fixed
        // deterministic pattern (no RNG in the projectile path) -- three rings.
        for (int i = 0; i < 16; ++i) {
            double a = i * 0.392699, r = 25.0 + (i % 4) * 55.0;
            double fx = p.tx + std::cos(a) * r, fy = p.ty + std::sin(a) * r;
            world.events.push({EventType::Effect, "spr_flame", fx, fy, 0, kNullRef, ""});
            world.fires.push_back({fx, fy, 10.0});
        }
        return;
    }
    if (p.bomb) {
        // objects/obj_bomb/Destroy.gml + obj_explosion_large/Step.gml: EVERY
        // dropped bomb spawns obj_explosion_large, whose default sprite is
        // spr_explosion_large -- it only switches to spr_explosion_mushroom
        // when type="atomic bomb" specifically (a distinct, much rarer unit
        // this port doesn't have yet).
        world.events.push({EventType::Effect, "spr_explosion_large", p.tx, p.ty, p.big ? 1.0 : 0.5,
                           kNullRef, ""});
        world.events.push({EventType::Sound, "explosion", p.tx, p.ty, 0, kNullRef, ""});
        // Bombs do COLLATERAL blast damage over a 2-tile radius (64px) -- own
        // units in the area take it too.
        splash_damage(world, p.tx, p.ty, 64, 64, p.pow, p.common.team, /*collateral=*/true);
        // A bomb leaves a fire where it hit, burning whatever's there.
        world.events.push({EventType::Effect, "spr_flame", p.tx, p.ty, 0, kNullRef, ""});
        world.fires.push_back({p.tx, p.ty, 6.0});
        return;
    }
    // A shell that lands in open water throws up a splash instead of a
    // fiery blast -- a blue shockwave ring + the ship-sink sound (reused).
    // Still does its splash damage (a near-miss can still catch a ship).
    if (world.is_water(p.tx, p.ty)) {
        world.events.push({EventType::Effect, "spr_water_splash", p.tx, p.ty, 0, kNullRef, ""});
        world.events.push({EventType::Sound, "ship_sink", p.tx, p.ty, 0, kNullRef, ""});
        // Artillery does friendly fire (collateral) -- a shell doesn't care
        // whose troops are under it. splash_mult carries the 420mm-mortar bonus.
        splash_damage(world, p.tx, p.ty, 48 * p.splash_mult, 40 * p.splash_mult, p.pow, p.common.team,
                      /*collateral=*/true, /*flat=*/true);
        return;
    }
    // Only artillery is lob besides bombs (see spawn_projectile) -- same
    // obj_explosion/obj_missile impact chain as the ships below, just
    // reached from the lob branch instead of the homing one. Its blast is
    // collateral: it damages friendly units caught in the radius too.
    world.events.push({EventType::Effect, "spr_explosion", p.tx, p.ty, 0.5, kNullRef, ""});
    world.events.push({EventType::Sound, "explosion", p.tx, p.ty, 0, kNullRef, ""});
    splash_damage(world, p.tx, p.ty, 48 * p.splash_mult, 40 * p.splash_mult, p.pow, p.common.team,
                  /*collateral=*/true, /*flat=*/true, p.bonus_vs_building);
    spawn_missile_impact_fx(world, p.name, p.tx, p.ty);
}

} // namespace

void update_projectile(EntityRef self, Projectile& p, double dt, World& world) {
    p.life -= dt;
    if (p.life <= 0) { p.common.alive = false; return; }

    // Ballistic missile launch sequence: it sits on the pad belching exhaust,
    // then accelerates slowly before climbing into its Yamato-style height arc
    // toward the target. `flight_t` (dt scaled by the launch speed ramp) drives
    // BOTH the travel and the arc so the two stay in step.
    if (p.name == "ballistic missile") {
        p.age += dt;
        constexpr double kHold = 0.5, kAccel = 0.7, kMissileNose = -0.56;
        double sf; // launch speed fraction: 0 on the pad, eases in to 1
        if (p.age < kHold) {
            sf = 0.0;
        } else {
            double t = std::min(1.0, (p.age - kHold) / kAccel);
            sf = t * t; // ease-in: slow off the pad, then accelerate
        }
        // Home toward the target (a straight attack-ground shot keeps its aim).
        if (p.homing) {
            EntityCommon* t = world.common(p.target);
            if (t && t->alive) {
                double d = std::hypot(t->x - p.common.x, t->y - p.common.y);
                if (d < 1e-9) d = 1;
                p.vx = (t->x - p.common.x) / d;
                p.vy = (t->y - p.common.y) / d;
            }
        }
        double step = p.speed * sf * dt;
        p.common.x += p.vx * step;
        p.common.y += p.vy * step;
        p.flight_t += sf * dt;
        double n = 60.0 * p.flight_t;
        p.z = p.arc_accel * n * (n - p.arc_t);
        // Nose bearing (screen space): straight along the rail on the pad, then
        // following the flight (the drawn sprite is lifted by z, so include the
        // arc's rate of rise, scaled by the launch ramp). The sprite nose points
        // up-right when unrotated (kMissileNose), so the drawn angle is
        // kMissileNose - nose_dir.
        // The missile leans to the LAUNCHER's facing side for its whole flight
        // (p.launch_facing, set at launch), NOT to its velocity's x-sign -- the
        // latter is ~0 and flickers for a dead-vertical shot (target directly
        // above/below), which used to misalign the pose right on the left/right
        // boundary. The launcher holds a stable facing, so the side is stable.
        const double side = (p.launch_facing < 0) ? -1.0 : 1.0;
        double nose_dir;
        if (sf <= 0.0) {
            nose_dir = (side >= 0.0) ? -M_PI / 4.0 : -3.0 * M_PI / 4.0; // up-right / up-left rail
        } else {
            double dzdt = p.arc_accel * (2.0 * n - p.arc_t) * 60.0 * sf;
            double dx = p.vx * p.speed * sf - dzdt / 4.0;
            double dy = p.vy * p.speed * sf - dzdt;
            // Keep a NORMAL diagonal firing pose even when the target is nearly
            // straight up or down: don't let the nose pitch past ~50 deg from
            // horizontal (a side-view missile pointing dead-vertical reads as a
            // weird broken pose). Enforce a minimum horizontal component, leaning
            // to the launcher's side.
            constexpr double kMaxPitch = 50.0 * M_PI / 180.0;
            double min_dx = std::abs(dy) / std::tan(kMaxPitch);
            if (side * dx < min_dx) dx = side * min_dx;
            nose_dir = std::atan2(dy, dx);
        }
        // Face left by MIRRORING the sprite (like the launcher base), not by
        // rotating it ~180deg (which reads as upside-down). When flipped the
        // sprite's nose native-bearing reflects to (pi - kMissileNose), so the
        // rotation that lands it on nose_dir is (pi - kMissileNose - nose_dir).
        p.flip = (side < 0.0);
        p.angle = p.flip ? (M_PI - kMissileNose - nose_dir) : (kMissileNose - nose_dir);
        // Exhaust: a jet of red-orange fire particles + smoke firing out of the
        // missile's REAR -- exactly opposite the nose bearing, from the missile's
        // drawn position (its sim x/y minus the arc lift the client draws with).
        // Emitted the whole flight, so the missile leaves a plume trailing behind
        // its tail.
        {
            double mx = p.common.x - p.z / 4.0; // missile's drawn world position (its NOSE)
            double my = p.common.y - p.z;
            double rx = std::cos(nose_dir + M_PI), ry = std::sin(nose_dir + M_PI); // rear unit vector
            // The drawn position sits at the missile's nose origin, but the
            // exhaust nozzle is at the far TAIL -- push the plume back another
            // ~half a sprite length (the 97px projectile) so the fire actually
            // erupts from the nozzle rather than mid-body.
            constexpr double kHalfSprite = 48.0;
            double dist = ((sf <= 0.0) ? 26.0 : 22.0) + kHalfSprite; // a touch further back on the pad
            int count = (sf <= 0.0) ? 5 : 3;
            for (int i = 0; i < count; ++i) {
                double ex = mx + rx * dist + world.rng.uniform(-9, 9);
                double ey = my + ry * dist + world.rng.uniform(-9, 9);
                world.events.push({EventType::Effect, "spr_rocket_particle", ex, ey, 0, kNullRef, ""});
            }
            if (world.rng.chance(0.35))
                world.events.push({EventType::Effect, "spr_smoke", mx + rx * dist + world.rng.uniform(-8, 8),
                                   my + ry * dist + world.rng.uniform(-8, 8), 0, kNullRef, ""});
        }
        // The missile LOBS OVER everything -- it never collides with units or
        // buildings mid-flight, only detonating when its arc brings it back down
        // to the ground at the target (attack-ground / miss). So there is no
        // shared collision check here; just return until the arc completes.
        if (p.flight_t > 0.0 && n > 0.0 && p.z <= 0.0) {
            ballistic_impact(p, world, p.common.x, p.common.y);
            return;
        }
        return;
    }

    if (p.lob) {
        p.progress += p.speed * dt / p.total;
        double prog = std::min(1.0, p.progress);
        p.common.x = p.sx + (p.tx - p.sx) * prog;
        p.common.y = p.sy + (p.ty - p.sy) * prog;
        if (p.bomb) {
            p.z = 60.0 * (1.0 - std::pow(prog, 1.7)); // dropped from altitude: accelerating fall
        } else {
            p.z = std::sin(prog * M_PI) * (p.total * 0.28); // artillery: parabolic arc
        }
        if (p.progress >= 1.0) explode(p, world);
        return;
    }

    if (p.homing) {
        EntityCommon* t = world.common(p.target);
        if (t && t->alive) {
            double d = std::hypot(t->x - p.common.x, t->y - p.common.y);
            if (d < 1e-9) d = 1;
            p.vx = (t->x - p.common.x) / d;
            p.vy = (t->y - p.common.y) / d;
            p.angle = std::atan2(p.vy, p.vx);
        }
    }
    p.common.x += p.vx * p.speed * dt;
    p.common.y += p.vy * p.speed * dt;

    // Ship-shell visual arc (see world.cpp's spawn_projectile comment): x/y
    // above still travel in a plain straight line, this only lifts the
    // DRAWN sprite up and back down over the shot's expected flight time.
    // Closed-form equivalent of obj_missile/Step.gml's per-frame
    // `up_speed += accel*2; height += up_speed` accumulation (which is an
    // exact discrete parabola that returns to 0 at n == arc_t frames) --
    // n is elapsed time in GML's 60fps frame units.
    if (p.arc_accel != 0.0) {
        p.age += dt;
        double n = 60.0 * p.age;
        p.z = p.arc_accel * n * (n - p.arc_t);
        if (n > 0.0 && p.z <= 0.0) {
            // Radar-guided ship shell: instead of arcing out (missing), it keeps
            // flying flat toward its (still-living) target until it connects --
            // the team's ships fire with 100% accuracy. Falls through to the
            // collision check below; expires normally via `life` if the target
            // dies first.
            {
                EntityCommon* rt = world.common(p.target);
                if (p.radar_guided && rt && rt->alive) {
                    p.z = 0.0;
                    goto after_arc; // keep flying (skip the miss/death handling)
                }
            }
            // Arc ran out (a ship shell that missed) -- ends here, with a splash
            // ring if it came down in open water. (The ballistic missile has its
            // own launch/arc path above and never reaches this branch.)
            p.common.alive = false;
            if (world.is_water(p.common.x, p.common.y)) {
                world.events.push(
                    {EventType::Effect, "spr_water_splash", p.common.x, p.common.y, 0, kNullRef, ""});
                world.events.push(
                    {EventType::Sound, "ship_sink", p.common.x, p.common.y, 0, kNullRef, ""});
            }
            return;
        }
    }
after_arc:;

    // Ships whose shot connects (obj_missile's collision check -- battleship/
    // yamato/destroyer get the full explosion+splash+fx chain; torpedo boat
    // gets the visual/sound but NOT splash damage (obj_explosion_effect, an
    // empty Step event in the original -- purely cosmetic); frigate gets
    // NEITHER (falls through to the plain direct-hit else branch below),
    // matching obj_missile/Destroy.gml's `origin_name!="frigate"` guards.
    static const std::set<std::string> kShipSplashUnits = {"battleship", "yamato", "destroyer"};
    // A shot aimed at a UNIT flies through enemy buildings in the way -- a farm,
    // wall, house, etc. between shooter and target shouldn't eat a bullet meant
    // for the unit behind it. It only detonates on units. A shot aimed at a
    // BUILDING still collides with buildings normally, so buildings can still be
    // attacked directly (and walls still block movement, just not line of fire).
    bool aimed_at_unit = p.target.valid() && p.target.kind == EntityKind::Unit;
    for (auto ref : world.grid.query(p.common.x, p.common.y, 24)) {
        EntityCommon* e = world.common(ref);
        if (!is_valid_combat_target(world, e, p.common.team)) continue;
        if (aimed_at_unit && e->kind == EntityKind::Building) continue; // pass through buildings
        if (std::abs(e->x - p.common.x) < 20 && std::abs(e->y - p.common.y) < 20) {
            p.common.alive = false;
            if (p.name == "ballistic missile") {
                // Detonate with the full bomb-blast + 2x splash + anti-building
                // bonus wherever it struck (the splash catches this target too).
                ballistic_impact(p, world, p.common.x, p.common.y);
            } else if (LAND_SHELL_UNITS.count(p.name)) {
                // Tanks are DIRECT fire: the round hits its target for its full
                // advertised damage (attack - target's pierce), then a modest
                // HALF-power splash (radius = the unit's catalog blast_radius)
                // catches anyone right beside it -- without robbing the primary
                // target of its full hit.
                world.events.push({EventType::Effect, "spr_explosion", p.common.x, p.common.y, 0.5,
                                   kNullRef, ""});
                world.events.push({EventType::Sound, "explosion", p.common.x, p.common.y, 0, kNullRef, ""});
                world.hurt(ref, std::max(p.pow - world.pierce_of(ref), 1.0));
                if (p.blast_px > 0.0) {
                    for (auto r2 : world.grid.query(p.common.x, p.common.y, p.blast_px + 12.0)) {
                        if (r2 == ref) continue; // primary already took the full direct hit
                        EntityCommon* e2 = world.common(r2);
                        if (!is_valid_combat_target(world, e2, p.common.team)) continue;
                        double dist = std::hypot(e2->x - p.common.x, e2->y - p.common.y);
                        if (dist > p.blast_px) continue;
                        world.hurt(r2, std::max(1.0, p.pow * 0.5 * (1.0 - dist / p.blast_px)));
                    }
                }
            } else if (kShipSplashUnits.count(p.name)) {
                world.events.push({EventType::Effect, "spr_explosion", p.common.x, p.common.y, 0.5,
                                   kNullRef, ""});
                world.events.push({EventType::Sound, "explosion", p.common.x, p.common.y, 0, kNullRef, ""});
                splash_damage(world, p.common.x, p.common.y, 36, 30, p.pow, p.common.team);
                spawn_missile_impact_fx(world, p.name, p.common.x, p.common.y);
            } else if (p.name == "torpedo boat") {
                world.events.push({EventType::Effect, "spr_explosion", p.common.x, p.common.y, 0.5,
                                   kNullRef, ""});
                world.events.push({EventType::Sound, "explosion", p.common.x, p.common.y, 0, kNullRef, ""});
                spawn_missile_impact_fx(world, p.name, p.common.x, p.common.y);
            } else {
                double dmg = std::max(p.pow - world.pierce_of(ref), 1.0);
                // Waffen SS anti-villager bonus: a flat +5 on a direct hit against
                // an enemy civilian (see Projectile::bonus_vs_civilian). Added on
                // top of the post-pierce damage so it always lands in full.
                if (p.bonus_vs_civilian > 0.0 && ref.kind == EntityKind::Unit) {
                    Unit* hu = world.get(ref);
                    if (hu && hu->name == "civilian") dmg += p.bonus_vs_civilian;
                }
                // Royal Marine anti-cavalry bonus: a flat +5/+7 on a direct hit
                // against an enemy cavalry unit (see Projectile::bonus_vs_cavalry).
                if (p.bonus_vs_cavalry > 0.0 && ref.kind == EntityKind::Unit) {
                    static const std::set<std::string> kCavalry = {
                        "cavalry", "cavalry2", "cavalry3", "camel", "camel corps"};
                    Unit* hu = world.get(ref);
                    if (hu && kCavalry.count(hu->name)) dmg += p.bonus_vs_cavalry;
                }
                world.hurt(ref, dmg);
            }
            return;
        }
    }
    (void)self;
}

} // namespace ww::sim
