#pragma once
#include "sim/entity_common.h"

#include <string>

namespace ww::sim {

// Data layout only -- direct port of Projectile.__init__ (game/entity.py).
struct Projectile {
    EntityCommon common;
    std::string name; // e.g. "shell" -- checked against SHELL_UNITS for splash FX
    std::string sprite;

    double pow = 0.0;
    EntityRef target = kNullRef;
    bool homing = false;
    bool big = false;   // big mushroom (bombs/ships) vs half-size (tanks/artillery)
    double speed = 8.0 * 30.0;
    double life = 4.0;  // seconds until the shot expires unfired
    bool lob = false;    // travels to a fixed point, then bursts (artillery/bombs)
    bool bomb = false;   // dropped from altitude: falls with gravity
    bool nuke = false;   // atomic bomb: huge mushroom blast, collateral (hits all teams)
    // Multiplier on this shot's splash radius -- 1.0 for everything except an
    // artillery shell fired by a Soviet team that has researched the 420mm
    // Self-Propelled Mortar unique tech (+33%). Set in World::spawn_projectile.
    double splash_mult = 1.0;
    // Flat bonus damage added when this shot lands directly on an enemy CIVILIAN
    // (villager). Nonzero only for the Nazi Waffen SS unique unit (+5), set in
    // World::spawn_projectile; applied at impact in projectile_behavior.cpp.
    double bonus_vs_civilian = 0.0;
    // Flat bonus damage added on a direct hit against an enemy CAVALRY unit.
    // Nonzero only for the UK Royal Marine (+5) / Elite Royal Marine (+7), set
    // in World::spawn_projectile; applied at impact in projectile_behavior.cpp.
    double bonus_vs_cavalry = 0.0;
    // Flat bonus damage added against BUILDINGS caught in the blast. Nonzero
    // only for the ballistic missile (+50), set in World::spawn_projectile;
    // applied in projectile_behavior.cpp's splash_damage.
    double bonus_vs_building = 0.0;
    // Radar (naval effect): a ship shell fired by a team that has researched
    // Radar keeps homing until it connects instead of "arcing out" (missing) if
    // its target outruns the shot -- i.e. the team's ships fire with 100%
    // accuracy. Set in World::spawn_projectile.
    bool radar_guided = false;
    double down_speed = 0.0;
    double z = 0.0;      // altitude (bombs start high and fall)
    double vx = 0.0, vy = 0.0;
    double angle = 0.0;
    // Horizontal mirror of the drawn sprite. Used by the ballistic missile so
    // the flying missile flips to face left (mirroring the launcher, which
    // also flips rather than rotating) instead of rotating ~180deg and looking
    // upside-down. Composes with `angle` (see SpriteAtlas::draw).
    bool flip = false;
    // Area-of-effect radius in PIXELS (catalog blast_radius * TILE), set in
    // spawn_projectile. Drives the tank line's small direct-fire splash so a
    // tank shell catches units right beside its target; 0 = no extra splash.
    double blast_px = 0.0;
    // Ballistic missile only: the LAUNCHER's facing at launch (1 right / -1
    // left). The missile leans/mirrors to this side for its whole flight rather
    // than deriving the side from its velocity's x-sign -- which is ambiguous
    // for a dead-vertical shot (target directly above/below) and made the pose
    // flicker/misalign right on the left/right boundary. Set in spawn_projectile.
    int launch_facing = 1;

    // Ship-shell visual arc (obj_missile/Step.gml's height/up_speed/accel):
    // a purely cosmetic vertical lift for an otherwise-straight HOMING shot
    // (unlike the lob path above, x/y still travel in a simple straight
    // line toward the target -- only the drawn height rises and falls).
    // arc_accel == 0 means "no arc" (every non-ship-shell homing shot).
    double age = 0.0;         // seconds since spawn
    double arc_accel = 0.0;   // GML per-frame accel constant (negative)
    double arc_t = 0.0;       // GML frame-equivalent flight duration (60 * seconds)
    // Ballistic missile only: accumulated *effective* flight time (raw dt scaled
    // by the launch speed ramp), so the missile can sit on the pad belching
    // exhaust, then accelerate slowly, before the height arc + travel advance.
    double flight_t = 0.0;

    // lob-only fields: fixed start/target point and travel progress
    double sx = 0.0, sy = 0.0;
    double tx = 0.0, ty = 0.0;
    double total = 0.0;
    double progress = 0.0;
};

} // namespace ww::sim
