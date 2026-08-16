#pragma once
#include "net/socket.h"
#include "sim/command.h"
#include "sim/scenario.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Deterministic lockstep for a 1v1 match.
//
// WHY LOCKSTEP. This sim is deterministic by construction and already proves it
// every build: headless_runner's checksum mode replays a seed tick for tick,
// and Match::checksum exists specifically "to detect desyncs between peers
// (Phase D)". Given that, the only thing that has to cross the network is each
// player's INTENT -- a handful of commands per second -- rather than world
// state. Both machines then compute the identical match from the identical
// inputs. That is what makes a 200-unit RTS playable on a home connection.
//
// THE CONTRACT THIS IMPOSES, which is the whole difficulty of the feature:
// every single thing that changes the world must arrive as a Command, on the
// same turn, in the same order, on both machines. A stray direct mutation in
// the client -- one button that writes unit->rally itself instead of issuing a
// command -- desyncs the match. See GameClient's command sink.
//
// TURN MODEL. The sim advances in fixed 20Hz ticks. A TURN is kTicksPerTurn of
// those, and commands issued during turn T are scheduled to execute on turn
// T + kInputDelay on both peers. That delay is what hides the round trip: a
// command has a full turn's worth of network time to arrive before the turn it
// belongs to comes up. The cost is that a click takes effect kInputDelay turns
// later, which at the values below is ~300ms -- normal for the genre.
//
// If a peer's commands for the next turn have not arrived, the match STALLS
// (both sides, since both are waiting on the same condition) rather than
// running ahead. Running ahead is what a desync is.
namespace ww::net {

// 4 ticks at 20Hz = 200ms per turn; 2 turns of input delay = 400ms of cover.
// Chosen so a typical domestic round trip (30-120ms) fits comfortably inside
// one turn, leaving the second as margin for a spike.
constexpr int kTicksPerTurn = 4;
constexpr int kInputDelay = 2;
constexpr uint16_t kDefaultPort = 27015;

// Bumped whenever the wire format or anything that affects simulation results
// changes. Two builds that disagree here refuse to start rather than desyncing
// ten minutes in -- which is the same reason the handshake also compares a
// build id.
//
// 3: added the pre-match lobby (kMsgLobby/kMsgSlot/kMsgStart). Adding message
//    types alone is backwards compatible -- handle_message ignores unknown ones
//    -- but the FLOW is not: a version-2 peer goes straight from Ready into the
//    match, while a version-3 peer sits in the lobby waiting for a Start that
//    the older build will never send. That is a hang, so it has to be a refusal.
constexpr uint32_t kProtocolVersion = 3;

enum class Role { None, Host, Joiner };

// One player's choices in the pre-match lobby. Deliberately NOT part of
// SkirmishSettings: those are the match's rules, agreed once and identical on
// both machines, whereas these are per-player and edited independently right up
// until Start. The host folds them into the settings at that point (see
// start_lobby_match), which is what makes the two peers generate one world.
struct LobbySlot {
    int civ = -1;   // -1 = Random; the host resolves it to a real civ at Start
    int leader = 0; // 0-2, index into that civ's leaders
    int colour = 0; // index into the client's team-colour palette
    int ally = 1;   // alliance group (Team::ally)
    bool ready = false;
};

enum class Status {
    Idle,
    Listening,   // host: port open, waiting for a joiner
    Connecting,  // joiner: TCP handshake in flight
    Handshaking, // connected, exchanging Hello
    Ready,       // agreed on seed+settings, match can start
    InMatch,
    Failed,      // see error()
    Desync,      // checksums disagreed; see desync_tick()
    Closed,      // peer disconnected cleanly
};

// What the client should do at a turn boundary.
enum class TurnState {
    Run,     // commands for this turn are complete -- apply and step
    Waiting, // still waiting on the other player
    Stopped, // desync/disconnect: the match is over
};

class Session {
public:
    ~Session();

    // ---- setup ----------------------------------------------------------
    // Host: open `port` and wait. The caller is responsible for the UPnP
    // mapping (see net/upnp.h) -- kept separate because a player who has
    // forwarded the port manually should not be blocked by UPnP failing.
    bool host(uint16_t port, const ww::sim::SkirmishSettings& settings, uint64_t seed);
    // Joiner: connect. Seed and settings arrive from the host.
    bool join(const std::string& address, uint16_t port);

    // Pump sockets and advance the handshake. Cheap; call every frame.
    void poll();

    Status status() const { return status_; }
    Role role() const { return role_; }
    const std::string& error() const { return error_; }
    // Valid once status() == Ready: what the match must be built from. Both
    // peers use the host's copy, so there is nothing to disagree about.
    uint64_t seed() const { return seed_; }
    const ww::sim::SkirmishSettings& settings() const { return settings_; }
    // Which team this machine's player controls. Host is 0, joiner is 1.
    int local_team() const { return role_ == Role::Host ? 0 : 1; }

    // ---- pre-match lobby (while status() == Ready) ------------------------
    // Both players sit here choosing civ/leader/colour/team and marking
    // themselves ready. The HOST is the single authority: it owns the match
    // settings, and it owns the merged roster -- the joiner only ever sends its
    // own slot and is told the result. That asymmetry is deliberate and is the
    // same one the handshake already uses; two writers to one roster is how you
    // get two machines generating different worlds.
    //
    // Host only; ignored on the joiner. Replaces the map/rule settings and
    // pushes them to the peer.
    void set_lobby_settings(const ww::sim::SkirmishSettings& s);
    // Either side: publish MY slot. The host applies it locally and rebroadcasts
    // the merged roster; the joiner sends it and waits to be told.
    void set_local_slot(const LobbySlot& s);
    const LobbySlot& lobby_slot(int team) const {
        return slots_[(team == 1) ? 1 : 0];
    }
    // Bumped on every lobby change from either side. The UI redraws from the
    // session each frame regardless, so this exists for anything that needs to
    // notice a REMOTE edit specifically.
    uint32_t lobby_revision() const { return lobby_rev_; }
    // Host only, and only meaningful once both slots are ready: resolve any
    // Random civ, fold both slots into the settings, tell the peer to start.
    // After this both sides report match_starting().
    void start_lobby_match();
    // The host has pressed Start and the settings are final. Both peers leave
    // the menu, build the match from settings()/seed(), and call start_match().
    bool match_starting() const { return starting_; }

    void start_match(); // Ready -> InMatch

    // ---- in-match -------------------------------------------------------
    // Queue a command the local player issued. It executes kInputDelay turns
    // from now, on both machines.
    void submit(const ww::sim::Command& cmd);

    // Called at each turn boundary with the checksum of the CURRENT state
    // (before this turn's commands are applied). Returns whether the turn may
    // run; when it returns Run, `commands` holds both players' commands in a
    // fixed order (host's first, then joiner's) -- that ordering is part of
    // the determinism contract, not a detail.
    TurnState begin_turn(uint64_t state_checksum, std::vector<ww::sim::Command>& commands);
    // Call once the turn's ticks have been simulated.
    void end_turn();

    uint64_t turn() const { return turn_; }
    uint64_t desync_turn() const { return desync_turn_; }
    uint64_t local_checksum() const { return desync_local_; }
    uint64_t remote_checksum() const { return desync_remote_; }
    // Round trip in milliseconds, for the lobby/HUD. -1 until measured.
    int ping_ms() const { return ping_ms_; }

    void close();

private:
    void fail(const std::string& why);
    // Host only: settings + both slots, the whole lobby in one message. Sent on
    // every change rather than diffed -- it is a few hundred bytes and a player
    // clicks a handful of times, so the simplicity is free.
    void broadcast_lobby();
    void send_message(uint8_t type, const std::vector<uint8_t>& payload);
    void handle_message(uint8_t type, const uint8_t* data, size_t len);
    void drain_socket();
    void send_turn_packet();

    Role role_ = Role::None;
    Status status_ = Status::Idle;
    std::string error_;

    Handle listener_ = kInvalid;
    Handle peer_ = kInvalid;
    std::vector<uint8_t> inbox_; // partial frames accumulate here

    uint64_t seed_ = 0;
    ww::sim::SkirmishSettings settings_;

    // Lobby roster: [0] is always the host, [1] the joiner, matching
    // local_team(). Both peers hold the same two, the host by authority and the
    // joiner by being told.
    // Defaults deliberately differ per slot: identical ones would put both
    // players on the same colour AND the same alliance group, i.e. a two-player
    // match nobody can win. Random civ for both, as the setup screen does.
    LobbySlot slots_[2] = {{-1, 0, 0, 1, false}, {-1, 0, 1, 2, false}};
    uint32_t lobby_rev_ = 0;
    bool starting_ = false;

    // Turn bookkeeping. `turn_` is the one about to execute.
    uint64_t turn_ = 0;
    uint64_t sent_through_ = 0; // highest turn we have sent local commands for
    struct TurnSlot {
        std::vector<ww::sim::Command> local, remote;
        bool have_remote = false;
    };
    std::map<uint64_t, TurnSlot> turns_;
    std::vector<ww::sim::Command> pending_; // issued this turn, sent next

    // Desync detection: our checksum per turn, and the peer's, compared as
    // soon as both are known.
    std::map<uint64_t, uint64_t> local_checksums_, remote_checksums_;
    uint64_t desync_turn_ = 0, desync_local_ = 0, desync_remote_ = 0;

    int ping_ms_ = -1;
    uint64_t ping_sent_ms_ = 0;
};

} // namespace ww::net
