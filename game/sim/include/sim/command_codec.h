#pragma once
#include "sim/command.h"

#include <cstdint>
#include <string>
#include <vector>

// Wire format for the lockstep command stream (see command.h's header comment:
// "what the lockstep command stream (Phase D) will carry tomorrow" -- this is
// that). Commands are addressed by entity id rather than slot index precisely
// so they survive the trip, so all this has to do is turn the variant into
// bytes and back.
//
// Deliberately hand-rolled and explicit rather than a memcpy of the structs:
// Command holds std::string and std::vector, so it is not trivially copyable,
// and a byte-for-byte struct dump would also silently bake in this compiler's
// padding and alignment. Everything below is fixed-width little-endian with an
// explicit tag, so a build can read a packet written by any other build of the
// same protocol version.
//
// Doubles go over as their IEEE-754 bit pattern. That is safe for the same
// reason the whole lockstep scheme is: both peers run the SAME binary on the
// same architecture (the handshake enforces a matching protocol version and
// build id), so a double that round-trips through its own bit pattern is the
// identical double -- which is what determinism needs.
namespace ww::sim {

// Append-only byte writer. Little-endian, no alignment, no padding.
struct ByteWriter {
    std::vector<uint8_t> bytes;

    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f64(double v);
    // Length-prefixed (u16) -- long enough for any catalog key, and a hostile
    // peer cannot make the reader allocate more than 64k from one field.
    void str(const std::string& v);
};

// Bounds-checked byte reader. Every read is validated against the end of the
// buffer; once `ok` goes false it stays false and further reads return zero,
// so a truncated or malicious packet degrades to "decode failed" rather than
// reading past the end. Nothing here trusts the sender.
struct ByteReader {
    const uint8_t* data = nullptr;
    size_t size = 0, off = 0;
    bool ok = true;

    ByteReader(const uint8_t* d, size_t n) : data(d), size(n) {}

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int32_t i32() { return static_cast<int32_t>(u32()); }
    double f64();
    std::string str();
    bool done() const { return off >= size; }
};

// One command. encode_command appends; decode_command advances the reader and
// returns false if the bytes did not contain a well-formed command (unknown
// tag, or truncated).
void encode_command(const Command& cmd, ByteWriter& w);
bool decode_command(ByteReader& r, Command& out);

// A whole turn's worth of commands from one player, which is the unit the
// network actually moves (see net/lockstep.h).
void encode_commands(const std::vector<Command>& cmds, ByteWriter& w);
bool decode_commands(ByteReader& r, std::vector<Command>& out);

} // namespace ww::sim
