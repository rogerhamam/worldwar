#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace ww::sim {

class World;

// Player/AI intents, addressed by entity id (stable across a match,
// unlike slot indices) so they can be logged/replayed/sent over the
// network without caring about internal storage. This is the entry
// point the golden-recording harness's scripted playback uses today,
// and what the lockstep command stream (Phase D) will carry tomorrow.
// group_speed_px: see World::order_move's param of the same name -- caps
// this unit's move to keep pace with the slowest unit in a multi-unit move
// order. <=0 (the default) means no cap.
struct MoveCommand { uint32_t unit_id; double x, y; double group_speed_px = -1.0; };
struct GatherCommand { uint32_t unit_id; uint32_t target_id; };
struct AttackCommand { uint32_t unit_id; uint32_t target_id; };
// builder_ids: units to assign as this foundation's construction crew
// (see World::place_building) -- empty falls back to auto-picking the
// nearest idle civilian.
struct PlaceBuildingCommand {
    int team;
    std::string name;
    double x, y;
    std::vector<uint32_t> builder_ids;
    // Commit `builder_ids` as the crew immediately (a plain click). False for a
    // shift-click, which instead APPENDS a build order to each builder's queue
    // so they finish what they are doing first.
    bool assign_builders = true;
    // Shift-click: queue the build order on every builder. This has to be part
    // of the same command rather than a follow-up, because the order refers to
    // the foundation this placement creates -- and under lockstep the client
    // cannot know that foundation's id, since the placement has not happened
    // locally yet when the command is issued. Doing both here keeps the pair
    // atomic and identical on both machines.
    bool queue_build = false;
};
struct EnqueueCommand { uint32_t building_id; std::string item; };
struct ResearchCommand { int team; std::string key; };
struct TradeCommand { int team; std::string action; std::string resource; };

// ---- the rest of the player's vocabulary --------------------------------
// The seven commands above were everything the golden-recording harness needed
// to replay a scripted match. Multiplayer needs more than that: in lockstep,
// ANY action that changes the world has to travel as a command, because the two
// machines only stay identical if they apply identical inputs. Every one of
// these was previously performed by the client writing entity fields directly,
// which is fine for one machine and fatal for two.
//
// They take a LIST of unit ids because that is how the player actually acts --
// on a selection. Sending one command per selected unit would work but multiply
// the packet count by the size of the army for no gain.
//
// Attack-move ("rally"): walk there, engaging anything met on the way. Distinct
// from MoveCommand, which is a passive move that ignores enemies.
struct RallyCommand { std::vector<uint32_t> unit_ids; double x, y; };
// Artillery/ballistic: shell a fixed point regardless of what is standing on it.
struct AttackGroundCommand { std::vector<uint32_t> unit_ids; double x, y; };
// Board a transport / disgorge its cargo at a point.
struct LoadCommand { std::vector<uint32_t> unit_ids; uint32_t transport_id; };
struct UnloadCommand { uint32_t transport_id; double x, y; };
// Ballistic launcher: pack to move, unpack to fire.
struct PackCommand { std::vector<uint32_t> unit_ids; bool packed; };
// Aircraft: land and garrison (airbase_id 0 = nearest own airbase), or launch
// a parked plane back into the air.
struct LandCommand { std::vector<uint32_t> unit_ids; uint32_t airbase_id; };
struct LaunchCommand { std::vector<uint32_t> unit_ids; };
// The player deleting their own units/buildings. Covers both kinds, since the
// player's Delete key does.
struct DeleteCommand { std::vector<uint32_t> ids; };
// Villagers: repair a damaged building, or join a foundation's build crew.
struct RepairCommand { std::vector<uint32_t> unit_ids; uint32_t building_id; };
struct AssignBuildCommand { std::vector<uint32_t> unit_ids; uint32_t foundation_id; };
// Cancel one item from a production queue (index 0 = the one in progress).
struct CancelQueueCommand { uint32_t building_id; int index; };
// Team-wide policy toggles the player flips from the command card: "replant"
// (auto re-sow exhausted farms) and "blitz" (the debug/cheat instant-build).
struct TeamToggleCommand { int team; std::string what; };
// A shift-queued follow-up order (see QueuedOrder). `kind` is QueuedOrderKind.
struct QueueOrderCommand {
    uint32_t unit_id;
    uint8_t kind;
    double x, y;
    uint32_t target_id;
};

using Command = std::variant<MoveCommand, GatherCommand, AttackCommand, PlaceBuildingCommand,
                              EnqueueCommand, ResearchCommand, TradeCommand, RallyCommand,
                              AttackGroundCommand, LoadCommand, UnloadCommand, PackCommand,
                              LandCommand, LaunchCommand, DeleteCommand, RepairCommand,
                              AssignBuildCommand, CancelQueueCommand, TeamToggleCommand,
                              QueueOrderCommand>;

void apply_command(World& world, const Command& cmd);

} // namespace ww::sim
