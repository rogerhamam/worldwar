#pragma once
#include "sim/entity_common.h"

#include <string>
#include <vector>

namespace ww::sim {

// Notify: an on-screen log line (research complete, age advance, building
// complete, unit trained) -- distinct from Warn (a single persistent
// center-bottom banner for resource/population limits). `key` names which
// kind of notification (the client formats the actual sentence, same
// split of responsibility as Sound's key->sound-file mapping); `text`
// carries whatever per-kind payload the client needs (a tech/unit/building
// name -- empty for age_advance, which just reads the team's current era
// directly). See GameClient::drain_events/ClientNotification.
enum class EventType { Sound, Effect, Warn, MusicStop, Victory, Defeat, Notify };

// A single tick's worth of "something happened that the client should
// react to" -- replaces the direct world.audio.play(...)/spawn_effect(...)
// calls sprinkled through game/entity.py's combat/gather/construction code
// (determinism bug #6: the sim core must not touch audio/rendering
// directly). `key` names the sound or effect sprite; exact cosmetic
// parameters (debris velocity, particle lifetime, etc.) are the client's
// business, not the sim's -- those were driven by Python's un-seeded
// `random.uniform` calls anyway, so they were never part of the
// synchronized simulation state to begin with.
struct SimEvent {
    EventType type;
    std::string key;
    double x = 0.0, y = 0.0;
    double pitch = 0.0;
    EntityRef entity = kNullRef; // for sound dispatch that depends on unit/building type
    std::string text;            // for Warn/Notify, or the entity name for a death Effect

    // --- death/corpse Effect payload -------------------------------------
    // The client picks the actual corpse/wreck/rubble sprite from the atlas
    // (only it knows which spr_<name>_dead/_rubble/_sink variants exist), but
    // that choice depends on synchronized entity state the sim must supply,
    // since the entity is erased the same tick it dies. Ignored by every
    // other event type. See World's death sweep + GameClient::spawn_death_effect.
    // `team` is also read by two Notify kinds -- "under_attack" (so the client
    // shows the alert only to the player it belongs to) and "team_age_advance"
    // (so it can name WHOSE age-up it was). It is a plain "which team is this
    // event about" field; the corpse use below is just its first caller.
    int team = -1;           // corpse team-colour frame (spr_<name>_dead)
    bool flip = false;       // entity faced left -> mirror the wreck/corpse
    bool mechanical = false; // unit leaves a metal wreck, not a body
    bool shell_kill = false; // killed by a shell/bomb -> bigger explosion
    bool deleted = false;    // cancelled foundation -> dust poof, no rubble
    double foot = 0.0;       // building footprint px (rubble is scaled to it)
    std::string alt;         // death Effect: fallback sprite base (the unit's
                             // generic type name) tried when the primary
                             // sprite-base has no _dead/_rubble variant -- lets
                             // a civ-skinned plane (spr_spitfire) still resolve
                             // its wreck via spr_fighter_rubble.
};

class EventBus {
public:
    void push(SimEvent e) { events_.push_back(std::move(e)); }
    const std::vector<SimEvent>& events() const { return events_; }
    void clear() { events_.clear(); }

private:
    std::vector<SimEvent> events_;
};

} // namespace ww::sim
