#pragma once
#include "sim/scenario.h"

#include <cstdint>
#include <memory>

namespace ww::sim {

// Top-level owner of one match's simulation state: DataStore, Bonuses,
// Control, the single seeded Rng, the EventBus, and World. Match::step()
// is the fixed-tick entry point every peer calls identically in a
// lockstep match (see the memory doc / plan) -- it currently just
// delegates to World::update(dt), but this is the seam where per-tick
// command application (task: lockstep networking, Phase D) will go.
class Match {
public:
    Match(uint64_t seed, const SkirmishSettings& settings, std::string data_dir = WW_DATA_DIR);
    // Builds a match from a hand-authored campaign Level (worldwar_
    // campaign_editor) instead of new_skirmish's random generation -- see
    // new_from_level (scenario.h). Level::players[0] is always the locked
    // human slot (team 0), matching Control/GameClient's own team-0-is-
    // human assumption throughout.
    Match(uint64_t seed, const ww::campaign::Level& level, std::string data_dir = WW_DATA_DIR);

    void step(double dt);
    uint64_t tick() const { return tick_; }

    // A cheap, order-sensitive hash of the entire simulation state
    // (entity positions/hp/alive + team resources), used to detect
    // desyncs between peers (Phase D) and to diff against golden
    // recordings (the headless_runner/replay_diff tools). NOT a
    // cryptographic hash -- just needs to be sensitive to any state
    // divergence and cheap to compute every tick.
    uint64_t checksum();

    World& world() { return *world_; }
    Control& control() { return control_; }
    // Read-only access to the loaded data files (catalog/civs/techs). The
    // client needs civs.json to name a team in a notification ("Germany has
    // advanced to the War Era") -- see GameClient::drain_events.
    const DataStore& data() const { return data_; }
    EventBus& events() { return events_; }
    Rng& rng() { return rng_; }

private:
    DataStore data_;
    Bonuses bonuses_;
    Control control_;
    Rng rng_;
    EventBus events_;
    std::unique_ptr<World> world_;
    uint64_t tick_ = 0;
};

} // namespace ww::sim
