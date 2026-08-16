#include "sim/rng.h"

namespace ww::sim {

namespace {
uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

// splitmix32, used only to spread the seed across the 128 bits of state.
// The state itself (not a truncation of it) feeds the avalanche mix below,
// so seeds that differ by only 1 (e.g. 1 vs 2) still diverge immediately --
// truncating state before mixing (an earlier version of this function did
// `state >> 16`) would discard exactly the low bits where small seeds
// differ, making adjacent seeds produce near-identical streams.
uint32_t splitmix32(uint32_t& state) {
    state += 0x9E3779B9u;
    uint32_t z = state;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}
} // namespace

Rng::Rng(uint64_t seed) {
    uint32_t sm = static_cast<uint32_t>(seed) ^ static_cast<uint32_t>(seed >> 32);
    for (auto& v : s_) v = splitmix32(sm);
}

uint32_t Rng::next_u32() {
    uint32_t result = rotl(s_[1] * 5, 7) * 9;
    uint32_t t = s_[1] << 9;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 11);
    return result;
}

uint32_t Rng::below(uint32_t n) {
    if (n == 0) return 0;
    // Lemire's debiased bounded-random algorithm: exact uniformity,
    // rejection-free in the overwhelmingly common case.
    uint64_t m = static_cast<uint64_t>(next_u32()) * n;
    uint32_t l = static_cast<uint32_t>(m);
    if (l < n) {
        uint32_t t = (~n + 1u) % n; // -n mod n, computed unsigned
        while (l < t) {
            m = static_cast<uint64_t>(next_u32()) * n;
            l = static_cast<uint32_t>(m);
        }
    }
    return static_cast<uint32_t>(m >> 32);
}

int Rng::randint(int lo, int hi) {
    return lo + static_cast<int>(below(static_cast<uint32_t>(hi - lo + 1)));
}

int Rng::randrange(int n) {
    return static_cast<int>(below(static_cast<uint32_t>(n)));
}

double Rng::uniform(double lo, double hi) {
    // 24 bits of mantissa is plenty of precision and keeps this trivially
    // portable (no dependence on a 64-bit random source).
    double t = (next_u32() >> 8) / static_cast<double>(1u << 24);
    return lo + t * (hi - lo);
}

bool Rng::chance(double p) {
    return uniform(0.0, 1.0) < p;
}

int Rng::choice3() {
    return randint(-1, 1);
}

} // namespace ww::sim
