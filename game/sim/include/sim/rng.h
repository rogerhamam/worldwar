#pragma once
#include <cstdint>

namespace ww::sim {

// A small, explicitly-specified, cross-compiler-deterministic PRNG
// (xoshiro128**, seeded via splitmix32). We deliberately do NOT use
// std::mt19937 via <random>'s distribution templates: the *engine*
// bitstream is standardized, but uniform_int_distribution and
// uniform_real_distribution are implementation-defined and differ
// between libstdc++ and MSVC STL, which would silently desync a
// lockstep match across compilers even with a "standard" RNG engine.
// Every distribution helper below is hand-written over the raw 32-bit
// output instead, so behavior is pinned down completely by this file.
//
// One instance of this is owned by Match, seeded once from the match
// seed, and is the ONLY source of randomness the simulation core may
// use (see the determinism notes on unseeded terrain gen / stray
// random.random() calls in the Python version this replaces).
class Rng {
public:
    explicit Rng(uint64_t seed);

    uint32_t next_u32();

    uint32_t below(uint32_t n);          // [0, n)
    int randint(int lo, int hi);         // [lo, hi] inclusive
    int randrange(int n);                // [0, n)
    double uniform(double lo, double hi);
    bool chance(double p);               // true with probability p
    int choice3();                       // -1, 0, or 1

private:
    uint32_t s_[4];
};

} // namespace ww::sim
