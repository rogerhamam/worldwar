#include "net/session.h"

#include "sim/command_codec.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace ww::net {
namespace {

using ww::sim::ByteReader;
using ww::sim::ByteWriter;
using ww::sim::Command;
using ww::sim::SkirmishSettings;

// Frame: [u32 length][u8 type][payload]. Length covers type+payload.
enum : uint8_t {
    kMsgHello = 1,     // host -> joiner: protocol, seed, settings
    kMsgHelloAck = 2,  // joiner -> host: protocol (agreement confirmed)
    kMsgTurn = 3,      // both ways, every turn
    kMsgBye = 4,
    kMsgDesync = 5,    // "our checksums disagree, stop"
    kMsgPing = 6,
    kMsgPong = 7,
    kMsgLobby = 8, // host -> joiner: settings + both lobby slots (the whole roster)
    kMsgSlot = 9,  // joiner -> host: my slot only
    kMsgStart = 10, // host -> joiner: settings are final, build the match
};

// A frame no legitimate message approaches. Turn packets are tens of bytes;
// the settings Hello is a few hundred. This is the guard that stops a hostile
// or corrupt peer making us allocate arbitrarily from a length field.
constexpr uint32_t kMaxFrame = 64 * 1024;

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// SkirmishSettings on the wire. Only the fields that change the generated
// world or the match rules -- everything the two peers must agree on for the
// simulation to be identical. Anything purely local (audio, camera) has no
// business here.
void write_settings(const SkirmishSettings& s, ByteWriter& w) {
    w.i32(s.n_players);
    w.i32(s.map_size);
    w.i32(s.max_pop);
    w.u8(s.water ? 1 : 0);
    w.str(s.map_type);
    w.u8(s.deathmatch ? 1 : 0);
    w.i32(s.reveal_mode);
    w.i32(s.difficulty);
    w.i32(s.leader);
    w.i32(s.enemy_leader);
    auto vec = [&](const std::vector<int>& v) {
        w.u16(static_cast<uint16_t>(v.size()));
        for (int x : v) w.i32(x);
    };
    vec(s.civs);
    vec(s.colours);
    vec(s.allies);
    vec(s.leaders);
}

bool read_settings(ByteReader& r, SkirmishSettings& s) {
    s.n_players = r.i32();
    s.map_size = r.i32();
    s.max_pop = r.i32();
    s.water = r.u8() != 0;
    s.map_type = r.str();
    s.deathmatch = r.u8() != 0;
    s.reveal_mode = r.i32();
    s.difficulty = r.i32();
    s.leader = r.i32();
    s.enemy_leader = r.i32();
    auto vec = [&](std::vector<int>& v) {
        uint16_t n = r.u16();
        if (!r.ok || n > 64) { r.ok = false; return; }
        v.clear();
        v.reserve(n);
        for (uint16_t i = 0; i < n; ++i) v.push_back(r.i32());
    };
    vec(s.civs);
    vec(s.colours);
    vec(s.allies);
    vec(s.leaders);
    return r.ok;
}

void write_slot(const LobbySlot& s, ByteWriter& w) {
    w.i32(s.civ);
    w.i32(s.leader);
    w.i32(s.colour);
    w.i32(s.ally);
    w.u8(s.ready ? 1 : 0);
}

bool read_slot(ByteReader& r, LobbySlot& s) {
    s.civ = r.i32();
    s.leader = r.i32();
    s.colour = r.i32();
    s.ally = r.i32();
    s.ready = r.u8() != 0;
    // Clamp everything a peer sends: these index straight into civ tables and
    // colour palettes on the other machine. -1 stays legal for civ (Random).
    if (s.civ < -1 || s.civ > 8) s.civ = -1;
    s.leader = std::min(std::max(s.leader, 0), 2);
    s.colour = std::min(std::max(s.colour, 0), 7);
    s.ally = std::min(std::max(s.ally, 1), 4);
    return r.ok;
}

} // namespace

Session::~Session() { close(); }

void Session::fail(const std::string& why) {
    error_ = why;
    status_ = Status::Failed;
}

bool Session::host(uint16_t port, const SkirmishSettings& settings, uint64_t seed) {
    if (!startup()) {
        fail(last_error());
        return false;
    }
    listener_ = tcp_listen(port);
    if (listener_ == kInvalid) {
        fail(last_error());
        return false;
    }
    role_ = Role::Host;
    settings_ = settings;
    seed_ = seed;
    status_ = Status::Listening;
    return true;
}

bool Session::join(const std::string& address, uint16_t port) {
    if (!startup()) {
        fail(last_error());
        return false;
    }
    peer_ = tcp_connect(address, port);
    if (peer_ == kInvalid) {
        fail(last_error());
        return false;
    }
    role_ = Role::Joiner;
    status_ = Status::Connecting;
    return true;
}

void Session::broadcast_lobby() {
    if (role_ != Role::Host || peer_ == kInvalid) return;
    ByteWriter w;
    write_settings(settings_, w);
    write_slot(slots_[0], w);
    write_slot(slots_[1], w);
    send_message(kMsgLobby, w.bytes);
}

void Session::set_lobby_settings(const SkirmishSettings& s) {
    if (role_ != Role::Host) return; // the joiner has no say; see the header
    settings_ = s;
    ++lobby_rev_;
    broadcast_lobby();
}

void Session::set_local_slot(const LobbySlot& s) {
    slots_[local_team()] = s;
    ++lobby_rev_;
    if (role_ == Role::Host) {
        broadcast_lobby();
    } else {
        ByteWriter w;
        write_slot(s, w);
        send_message(kMsgSlot, w.bytes);
    }
}

void Session::start_lobby_match() {
    if (role_ != Role::Host || status_ != Status::Ready) return;
    if (!slots_[0].ready || !slots_[1].ready) return;
    // Fold the roster into the settings. This is the moment the two independent
    // sets of choices become ONE match description -- after it, both machines
    // generate their world from the same bytes, which is the entire contract.
    settings_.n_players = 2;
    settings_.civs.clear();
    settings_.leaders.clear();
    settings_.colours.clear();
    settings_.allies.clear();
    for (int i = 0; i < 2; ++i) {
        LobbySlot& sl = slots_[i];
        // Random is resolved HERE, on the host, and then broadcast -- never
        // independently on each side, which would hand the two peers different
        // civs and desync on the very first tick.
        int civ = sl.civ;
        int leader = sl.leader;
        if (civ < 0) {
            civ = std::rand() % 9;
            leader = 0; // a randomised civ takes its first leader, as in the menu
        }
        settings_.civs.push_back(civ);
        settings_.leaders.push_back(leader);
        settings_.colours.push_back(sl.colour);
        settings_.allies.push_back(sl.ally);
    }
    // Only team 0 is fog-restricted when placing buildings, which would
    // handicap the joiner specifically -- so a network match is always
    // revealed, exactly as the env-hook path does.
    settings_.reveal_mode = 2;
    broadcast_lobby(); // settings first...
    send_message(kMsgStart, {}); // ...then go. TCP is ordered, so the joiner
                                 // has the final settings before it starts.
    starting_ = true;
}

void Session::send_message(uint8_t type, const std::vector<uint8_t>& payload) {
    if (peer_ == kInvalid) return;
    uint32_t len = static_cast<uint32_t>(payload.size() + 1);
    uint8_t header[4] = {static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>((len >> 8) & 0xFF),
                         static_cast<uint8_t>((len >> 16) & 0xFF),
                         static_cast<uint8_t>((len >> 24) & 0xFF)};
    if (!tcp_send(peer_, header, 4) || !tcp_send(peer_, &type, 1)) {
        fail(last_error());
        return;
    }
    if (!payload.empty() && !tcp_send(peer_, payload.data(), payload.size())) fail(last_error());
}

void Session::drain_socket() {
    if (peer_ == kInvalid) return;
    if (!tcp_recv(peer_, inbox_)) {
        // Peer closed. Mid-match that ends the game; in the lobby it is a
        // failed join.
        if (status_ == Status::InMatch) {
            error_ = last_error();
            status_ = Status::Closed;
        } else {
            fail(last_error());
        }
        return;
    }
    // Pull out every complete frame.
    for (;;) {
        if (inbox_.size() < 4) return;
        uint32_t len = static_cast<uint32_t>(inbox_[0]) | static_cast<uint32_t>(inbox_[1]) << 8 |
                       static_cast<uint32_t>(inbox_[2]) << 16 | static_cast<uint32_t>(inbox_[3]) << 24;
        if (len == 0 || len > kMaxFrame) {
            fail("received a malformed packet");
            return;
        }
        if (inbox_.size() < 4 + len) return; // wait for the rest
        uint8_t type = inbox_[4];
        handle_message(type, inbox_.data() + 5, len - 1);
        inbox_.erase(inbox_.begin(), inbox_.begin() + 4 + len);
        if (status_ == Status::Failed) return;
    }
}

void Session::handle_message(uint8_t type, const uint8_t* data, size_t len) {
    ByteReader r(data, len);
    switch (type) {
    case kMsgHello: {
        uint32_t proto = r.u32();
        std::string build = r.str();
        if (!r.ok) { fail("malformed handshake"); return; }
        if (proto != kProtocolVersion) {
            fail("the other player is running a different version of the game (protocol " +
                 std::to_string(proto) + ", this build speaks " + std::to_string(kProtocolVersion) +
                 ")");
            return;
        }
        if (build != WW_VERSION) {
            fail("version mismatch: they have " + build + ", you have " + std::string(WW_VERSION));
            return;
        }
        seed_ = r.u64();
        if (!read_settings(r, settings_)) { fail("malformed match settings"); return; }
        ByteWriter w;
        w.u32(kProtocolVersion);
        w.str(WW_VERSION);
        send_message(kMsgHelloAck, w.bytes);
        status_ = Status::Ready;
        break;
    }
    case kMsgHelloAck: {
        uint32_t proto = r.u32();
        std::string build = r.str();
        if (!r.ok) { fail("malformed handshake"); return; }
        if (proto != kProtocolVersion || build != WW_VERSION) {
            fail("version mismatch: they have " + build + ", you have " + std::string(WW_VERSION));
            return;
        }
        status_ = Status::Ready;
        break;
    }
    case kMsgTurn: {
        uint64_t cmd_turn = r.u64();
        uint64_t chk_turn = r.u64();
        uint64_t checksum = r.u64();
        std::vector<Command> cmds;
        if (!ww::sim::decode_commands(r, cmds)) { fail("malformed command packet"); return; }
        auto& slot = turns_[cmd_turn];
        slot.remote = std::move(cmds);
        slot.have_remote = true;
        remote_checksums_[chk_turn] = checksum;
        // Compare as soon as both sides of the same turn are known.
        auto mine = local_checksums_.find(chk_turn);
        if (mine != local_checksums_.end() && mine->second != checksum) {
            desync_turn_ = chk_turn;
            desync_local_ = mine->second;
            desync_remote_ = checksum;
            ByteWriter w;
            w.u64(chk_turn);
            w.u64(mine->second);
            send_message(kMsgDesync, w.bytes);
            status_ = Status::Desync;
        }
        break;
    }
    case kMsgDesync: {
        desync_turn_ = r.u64();
        desync_remote_ = r.u64();
        auto mine = local_checksums_.find(desync_turn_);
        desync_local_ = mine == local_checksums_.end() ? 0 : mine->second;
        status_ = Status::Desync;
        break;
    }
    case kMsgPing: {
        std::vector<uint8_t> echo(data, data + len);
        send_message(kMsgPong, echo);
        break;
    }
    case kMsgPong: {
        uint64_t sent = r.u64();
        if (r.ok && sent == ping_sent_ms_) ping_ms_ = static_cast<int>(now_ms() - sent);
        break;
    }
    case kMsgLobby: {
        // Host is authoritative -- a host receiving this would mean two
        // authorities, so ignore it rather than let a peer rewrite our roster.
        if (role_ != Role::Joiner) break;
        SkirmishSettings s;
        LobbySlot a, b;
        if (!read_settings(r, s) || !read_slot(r, a) || !read_slot(r, b)) {
            fail("malformed lobby update");
            return;
        }
        settings_ = s;
        // Take the HOST's row and the settings, but never our own row back.
        //
        // The host broadcasts the whole roster on every change, including its
        // copy of ours -- which is stale between the moment we edit and the
        // moment our kMsgSlot lands. Applying it would undo our own choice on
        // our own screen: pick a civ, the host changes the map, and the echo of
        // its older copy of us silently resets us to Random. Each side owns its
        // own slot; the host owns the settings and relays.
        slots_[0] = a;
        (void)b;
        ++lobby_rev_;
        break;
    }
    case kMsgSlot: {
        // Symmetrically: only the host accepts a slot, and only ever into the
        // JOINER's row. A peer cannot edit the host's own choices.
        if (role_ != Role::Host) break;
        LobbySlot s;
        if (!read_slot(r, s)) { fail("malformed lobby slot"); return; }
        slots_[1] = s;
        ++lobby_rev_;
        broadcast_lobby(); // echo the merged roster back so both sides agree
        break;
    }
    case kMsgStart:
        if (role_ == Role::Joiner) starting_ = true;
        break;
    case kMsgBye:
        error_ = "the other player left the game";
        status_ = Status::Closed;
        break;
    default:
        // Unknown type from a peer that passed the version check should be
        // impossible; ignore rather than fail, so adding a message type later
        // is backwards compatible.
        break;
    }
}

void Session::poll() {
    switch (status_) {
    case Status::Listening: {
        Handle c = tcp_accept(listener_);
        if (c != kInvalid) {
            peer_ = c;
            set_nodelay(peer_);
            close_socket(listener_);
            listener_ = kInvalid;
            // Host drives the handshake: it owns the seed and the settings, so
            // there is exactly one authority on what match is being played.
            ByteWriter w;
            w.u32(kProtocolVersion);
            w.str(WW_VERSION);
            w.u64(seed_);
            write_settings(settings_, w);
            status_ = Status::Handshaking;
            send_message(kMsgHello, w.bytes);
        }
        break;
    }
    case Status::Connecting: {
        int c = tcp_connected(peer_);
        if (c < 0) {
            fail(last_error());
        } else if (c > 0) {
            set_nodelay(peer_);
            status_ = Status::Handshaking;
        }
        break;
    }
    case Status::Handshaking:
    case Status::Ready:
    case Status::InMatch:
        drain_socket();
        break;
    default:
        break;
    }

    // Keep a round-trip estimate for the lobby/HUD. One in flight at a time.
    if ((status_ == Status::Ready || status_ == Status::InMatch) && peer_ != kInvalid) {
        uint64_t t = now_ms();
        if (t - ping_sent_ms_ > 1000) {
            ping_sent_ms_ = t;
            ByteWriter w;
            w.u64(t);
            send_message(kMsgPing, w.bytes);
        }
    }
}

void Session::start_match() {
    if (status_ != Status::Ready) return;
    status_ = Status::InMatch;
    turn_ = 0;
    sent_through_ = 0;
    // The first kInputDelay turns can have no commands from anyone -- they are
    // the pipeline priming, and both peers agree on that without exchanging
    // anything. Mark them complete so the match starts immediately instead of
    // deadlocking waiting for packets that were never going to be sent.
    for (uint64_t t = 0; t < static_cast<uint64_t>(kInputDelay); ++t) {
        turns_[t].have_remote = true;
    }
}

void Session::submit(const Command& cmd) {
    if (status_ != Status::InMatch) return;
    pending_.push_back(cmd);
}

void Session::send_turn_packet() {
    uint64_t target = turn_ + kInputDelay;
    ByteWriter w;
    w.u64(target);
    w.u64(turn_);
    auto it = local_checksums_.find(turn_);
    w.u64(it == local_checksums_.end() ? 0 : it->second);
    ww::sim::encode_commands(pending_, w);
    turns_[target].local = pending_;
    pending_.clear();
    sent_through_ = target;
    send_message(kMsgTurn, w.bytes);
}

TurnState Session::begin_turn(uint64_t state_checksum, std::vector<Command>& commands) {
    if (status_ == Status::Desync || status_ == Status::Closed || status_ == Status::Failed)
        return TurnState::Stopped;
    if (status_ != Status::InMatch) return TurnState::Waiting;

    // Record our checksum for this turn and compare if the peer's already
    // arrived. Doing this before anything else means a divergence is caught on
    // the very first turn it exists, not one turn later.
    local_checksums_[turn_] = state_checksum;
    auto theirs = remote_checksums_.find(turn_);
    if (theirs != remote_checksums_.end() && theirs->second != state_checksum) {
        desync_turn_ = turn_;
        desync_local_ = state_checksum;
        desync_remote_ = theirs->second;
        ByteWriter w;
        w.u64(turn_);
        w.u64(state_checksum);
        send_message(kMsgDesync, w.bytes);
        status_ = Status::Desync;
        return TurnState::Stopped;
    }

    // Publish this turn's local commands (scheduled kInputDelay ahead) exactly
    // once. This must happen even while stalled, or two peers each waiting for
    // the other's packet would deadlock.
    if (sent_through_ < turn_ + kInputDelay) send_turn_packet();

    drain_socket();
    if (status_ != Status::InMatch) return TurnState::Stopped;

    auto slot = turns_.find(turn_);
    if (slot == turns_.end() || !slot->second.have_remote) return TurnState::Waiting;

    // Fixed order: host's commands, then joiner's. Both machines build the same
    // list from the same two sets -- this ordering IS the determinism contract
    // for simultaneous, conflicting orders.
    commands.clear();
    const std::vector<Command>& host_cmds =
        role_ == Role::Host ? slot->second.local : slot->second.remote;
    const std::vector<Command>& join_cmds =
        role_ == Role::Host ? slot->second.remote : slot->second.local;
    commands.insert(commands.end(), host_cmds.begin(), host_cmds.end());
    commands.insert(commands.end(), join_cmds.begin(), join_cmds.end());
    return TurnState::Run;
}

void Session::end_turn() {
    turns_.erase(turn_);
    // Keep a short history of checksums for diagnostics, but do not grow
    // without bound over a 40-minute match.
    if (local_checksums_.size() > 256) local_checksums_.erase(local_checksums_.begin());
    if (remote_checksums_.size() > 256) remote_checksums_.erase(remote_checksums_.begin());
    ++turn_;
}

void Session::close() {
    if (peer_ != kInvalid && status_ == Status::InMatch) send_message(kMsgBye, {});
    if (peer_ != kInvalid) close_socket(peer_);
    if (listener_ != kInvalid) close_socket(listener_);
    peer_ = listener_ = kInvalid;
    if (status_ != Status::Desync && status_ != Status::Failed) status_ = Status::Closed;
}

} // namespace ww::net
