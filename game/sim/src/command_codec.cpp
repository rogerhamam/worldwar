#include "sim/command_codec.h"

#include <cstring>

namespace ww::sim {

namespace {
// Command tags. APPEND ONLY, and never reuse a number: the handshake pins the
// protocol version, but keeping these stable means a mismatched build fails at
// the handshake rather than by silently decoding a Move as a Gather.
enum : uint8_t {
    kMove = 1,
    kGather = 2,
    kAttack = 3,
    kPlaceBuilding = 4,
    kEnqueue = 5,
    kResearch = 6,
    kTrade = 7,
    kRally = 8,
    kAttackGround = 9,
    kLoad = 10,
    kUnload = 11,
    kPack = 12,
    kLand = 13,
    kLaunch = 14,
    kDelete = 15,
    kRepair = 16,
    kAssignBuild = 17,
    kCancelQueue = 18,
    kTeamToggle = 19,
    kQueueOrder = 20,
};
// Selections travel as id lists; cap them so a corrupt length cannot make the
// reader allocate wildly. The pop cap is 200, so this is far past any real
// selection while staying trivially allocatable.
constexpr uint16_t kMaxUnitList = 1024;

void write_ids(ByteWriter& w, const std::vector<uint32_t>& ids) {
    uint16_t n = static_cast<uint16_t>(ids.size() > kMaxUnitList ? kMaxUnitList : ids.size());
    w.u16(n);
    for (uint16_t i = 0; i < n; ++i) w.u32(ids[i]);
}

bool read_ids(ByteReader& r, std::vector<uint32_t>& ids) {
    uint16_t n = r.u16();
    if (!r.ok || n > kMaxUnitList) return false;
    ids.clear();
    ids.reserve(n);
    for (uint16_t i = 0; i < n; ++i) ids.push_back(r.u32());
    return r.ok;
}
// A single command's builder_ids list is the only unbounded field in the
// format; cap it so a malformed length can't make the reader reserve wildly.
// A construction crew is a handful of villagers -- 256 is far past any real
// selection and still trivially allocatable.
constexpr uint16_t kMaxBuilders = 256;
} // namespace

void ByteWriter::u16(uint16_t v) {
    bytes.push_back(static_cast<uint8_t>(v & 0xFF));
    bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void ByteWriter::u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void ByteWriter::u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void ByteWriter::f64(double v) {
    uint64_t bits;
    static_assert(sizeof(bits) == sizeof(v), "double must be 64-bit for the wire format");
    std::memcpy(&bits, &v, sizeof(bits));
    u64(bits);
}
void ByteWriter::str(const std::string& v) {
    uint16_t n = static_cast<uint16_t>(v.size() > 0xFFFF ? 0xFFFF : v.size());
    u16(n);
    bytes.insert(bytes.end(), v.begin(), v.begin() + n);
}

uint8_t ByteReader::u8() {
    if (!ok || off + 1 > size) { ok = false; return 0; }
    return data[off++];
}
uint16_t ByteReader::u16() {
    if (!ok || off + 2 > size) { ok = false; return 0; }
    uint16_t v = static_cast<uint16_t>(data[off]) | static_cast<uint16_t>(data[off + 1]) << 8;
    off += 2;
    return v;
}
uint32_t ByteReader::u32() {
    if (!ok || off + 4 > size) { ok = false; return 0; }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data[off + i]) << (8 * i);
    off += 4;
    return v;
}
uint64_t ByteReader::u64() {
    if (!ok || off + 8 > size) { ok = false; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data[off + i]) << (8 * i);
    off += 8;
    return v;
}
double ByteReader::f64() {
    uint64_t bits = u64();
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}
std::string ByteReader::str() {
    uint16_t n = u16();
    if (!ok || off + n > size) { ok = false; return {}; }
    std::string s(reinterpret_cast<const char*>(data + off), n);
    off += n;
    return s;
}

void encode_command(const Command& cmd, ByteWriter& w) {
    std::visit(
        [&](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, MoveCommand>) {
                w.u8(kMove);
                w.u32(c.unit_id);
                w.f64(c.x);
                w.f64(c.y);
                w.f64(c.group_speed_px);
            } else if constexpr (std::is_same_v<T, GatherCommand>) {
                w.u8(kGather);
                w.u32(c.unit_id);
                w.u32(c.target_id);
            } else if constexpr (std::is_same_v<T, AttackCommand>) {
                w.u8(kAttack);
                w.u32(c.unit_id);
                w.u32(c.target_id);
            } else if constexpr (std::is_same_v<T, PlaceBuildingCommand>) {
                w.u8(kPlaceBuilding);
                w.i32(c.team);
                w.str(c.name);
                w.f64(c.x);
                w.f64(c.y);
                uint16_t n = static_cast<uint16_t>(
                    c.builder_ids.size() > kMaxBuilders ? kMaxBuilders : c.builder_ids.size());
                w.u16(n);
                for (uint16_t i = 0; i < n; ++i) w.u32(c.builder_ids[i]);
                w.u8(c.assign_builders ? 1 : 0);
                w.u8(c.queue_build ? 1 : 0);
            } else if constexpr (std::is_same_v<T, EnqueueCommand>) {
                w.u8(kEnqueue);
                w.u32(c.building_id);
                w.str(c.item);
            } else if constexpr (std::is_same_v<T, ResearchCommand>) {
                w.u8(kResearch);
                w.i32(c.team);
                w.str(c.key);
            } else if constexpr (std::is_same_v<T, TradeCommand>) {
                w.u8(kTrade);
                w.i32(c.team);
                w.str(c.action);
                w.str(c.resource);
            } else if constexpr (std::is_same_v<T, RallyCommand>) {
                w.u8(kRally);
                write_ids(w, c.unit_ids);
                w.f64(c.x);
                w.f64(c.y);
            } else if constexpr (std::is_same_v<T, AttackGroundCommand>) {
                w.u8(kAttackGround);
                write_ids(w, c.unit_ids);
                w.f64(c.x);
                w.f64(c.y);
            } else if constexpr (std::is_same_v<T, LoadCommand>) {
                w.u8(kLoad);
                write_ids(w, c.unit_ids);
                w.u32(c.transport_id);
            } else if constexpr (std::is_same_v<T, UnloadCommand>) {
                w.u8(kUnload);
                w.u32(c.transport_id);
                w.f64(c.x);
                w.f64(c.y);
            } else if constexpr (std::is_same_v<T, PackCommand>) {
                w.u8(kPack);
                write_ids(w, c.unit_ids);
                w.u8(c.packed ? 1 : 0);
            } else if constexpr (std::is_same_v<T, LandCommand>) {
                w.u8(kLand);
                write_ids(w, c.unit_ids);
                w.u32(c.airbase_id);
            } else if constexpr (std::is_same_v<T, LaunchCommand>) {
                w.u8(kLaunch);
                write_ids(w, c.unit_ids);
            } else if constexpr (std::is_same_v<T, DeleteCommand>) {
                w.u8(kDelete);
                write_ids(w, c.ids);
            } else if constexpr (std::is_same_v<T, RepairCommand>) {
                w.u8(kRepair);
                write_ids(w, c.unit_ids);
                w.u32(c.building_id);
            } else if constexpr (std::is_same_v<T, AssignBuildCommand>) {
                w.u8(kAssignBuild);
                write_ids(w, c.unit_ids);
                w.u32(c.foundation_id);
            } else if constexpr (std::is_same_v<T, CancelQueueCommand>) {
                w.u8(kCancelQueue);
                w.u32(c.building_id);
                w.i32(c.index);
            } else if constexpr (std::is_same_v<T, TeamToggleCommand>) {
                w.u8(kTeamToggle);
                w.i32(c.team);
                w.str(c.what);
            } else if constexpr (std::is_same_v<T, QueueOrderCommand>) {
                w.u8(kQueueOrder);
                w.u32(c.unit_id);
                w.u8(c.kind);
                w.f64(c.x);
                w.f64(c.y);
                w.u32(c.target_id);
            }
        },
        cmd);
}

bool decode_command(ByteReader& r, Command& out) {
    uint8_t tag = r.u8();
    if (!r.ok) return false;
    switch (tag) {
    case kMove: {
        MoveCommand c;
        c.unit_id = r.u32();
        c.x = r.f64();
        c.y = r.f64();
        c.group_speed_px = r.f64();
        out = c;
        break;
    }
    case kGather: {
        GatherCommand c;
        c.unit_id = r.u32();
        c.target_id = r.u32();
        out = c;
        break;
    }
    case kAttack: {
        AttackCommand c;
        c.unit_id = r.u32();
        c.target_id = r.u32();
        out = c;
        break;
    }
    case kPlaceBuilding: {
        PlaceBuildingCommand c;
        c.team = r.i32();
        c.name = r.str();
        c.x = r.f64();
        c.y = r.f64();
        uint16_t n = r.u16();
        if (!r.ok || n > kMaxBuilders) return false;
        c.builder_ids.reserve(n);
        for (uint16_t i = 0; i < n; ++i) c.builder_ids.push_back(r.u32());
        c.assign_builders = r.u8() != 0;
        c.queue_build = r.u8() != 0;
        out = c;
        break;
    }
    case kEnqueue: {
        EnqueueCommand c;
        c.building_id = r.u32();
        c.item = r.str();
        out = c;
        break;
    }
    case kResearch: {
        ResearchCommand c;
        c.team = r.i32();
        c.key = r.str();
        out = c;
        break;
    }
    case kTrade: {
        TradeCommand c;
        c.team = r.i32();
        c.action = r.str();
        c.resource = r.str();
        out = c;
        break;
    }
    case kRally: {
        RallyCommand c;
        if (!read_ids(r, c.unit_ids)) return false;
        c.x = r.f64();
        c.y = r.f64();
        out = c;
        break;
    }
    case kAttackGround: {
        AttackGroundCommand c;
        if (!read_ids(r, c.unit_ids)) return false;
        c.x = r.f64();
        c.y = r.f64();
        out = c;
        break;
    }
    case kLoad: {
        LoadCommand c;
        if (!read_ids(r, c.unit_ids)) return false;
        c.transport_id = r.u32();
        out = c;
        break;
    }
    case kUnload: {
        UnloadCommand c;
        c.transport_id = r.u32();
        c.x = r.f64();
        c.y = r.f64();
        out = c;
        break;
    }
    case kPack: {
        PackCommand c;
        if (!read_ids(r, c.unit_ids)) return false;
        c.packed = r.u8() != 0;
        out = c;
        break;
    }
    case kLand: {
        LandCommand c;
        if (!read_ids(r, c.unit_ids)) return false;
        c.airbase_id = r.u32();
        out = c;
        break;
    }
    case kLaunch: {
        LaunchCommand c;
        if (!read_ids(r, c.unit_ids)) return false;
        out = c;
        break;
    }
    case kDelete: {
        DeleteCommand c;
        if (!read_ids(r, c.ids)) return false;
        out = c;
        break;
    }
    case kRepair: {
        RepairCommand c;
        if (!read_ids(r, c.unit_ids)) return false;
        c.building_id = r.u32();
        out = c;
        break;
    }
    case kAssignBuild: {
        AssignBuildCommand c;
        if (!read_ids(r, c.unit_ids)) return false;
        c.foundation_id = r.u32();
        out = c;
        break;
    }
    case kCancelQueue: {
        CancelQueueCommand c;
        c.building_id = r.u32();
        c.index = r.i32();
        out = c;
        break;
    }
    case kTeamToggle: {
        TeamToggleCommand c;
        c.team = r.i32();
        c.what = r.str();
        out = c;
        break;
    }
    case kQueueOrder: {
        QueueOrderCommand c;
        c.unit_id = r.u32();
        c.kind = r.u8();
        c.x = r.f64();
        c.y = r.f64();
        c.target_id = r.u32();
        out = c;
        break;
    }
    default:
        return false; // unknown tag: the stream is not one we understand
    }
    return r.ok;
}

void encode_commands(const std::vector<Command>& cmds, ByteWriter& w) {
    uint16_t n = static_cast<uint16_t>(cmds.size() > 0xFFFF ? 0xFFFF : cmds.size());
    w.u16(n);
    for (uint16_t i = 0; i < n; ++i) encode_command(cmds[i], w);
}

bool decode_commands(ByteReader& r, std::vector<Command>& out) {
    uint16_t n = r.u16();
    if (!r.ok) return false;
    out.clear();
    out.reserve(n);
    for (uint16_t i = 0; i < n; ++i) {
        Command c;
        if (!decode_command(r, c)) return false;
        out.push_back(std::move(c));
    }
    return true;
}

} // namespace ww::sim
