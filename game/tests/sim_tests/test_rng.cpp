#include <catch2/catch_test_macros.hpp>

#include "sim/rng.h"

using ww::sim::Rng;

TEST_CASE("Rng is deterministic for a given seed") {
    Rng a(42), b(42);
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(a.next_u32() == b.next_u32());
    }
}

TEST_CASE("Rng differs across seeds") {
    Rng a(1), b(2);
    bool any_diff = false;
    for (int i = 0; i < 16; ++i) {
        if (a.next_u32() != b.next_u32()) any_diff = true;
    }
    REQUIRE(any_diff);
}

TEST_CASE("Rng::below stays in range and hits both ends over many draws") {
    Rng r(7);
    bool saw_zero = false, saw_max = false;
    for (int i = 0; i < 100000; ++i) {
        uint32_t v = r.below(5);
        REQUIRE(v < 5);
        if (v == 0) saw_zero = true;
        if (v == 4) saw_max = true;
    }
    REQUIRE(saw_zero);
    REQUIRE(saw_max);
}

TEST_CASE("Rng::uniform stays within [lo, hi)") {
    Rng r(123);
    for (int i = 0; i < 10000; ++i) {
        double v = r.uniform(-5.0, 5.0);
        REQUIRE(v >= -5.0);
        REQUIRE(v < 5.0);
    }
}

TEST_CASE("Rng::choice3 only returns -1, 0, or 1") {
    Rng r(9);
    for (int i = 0; i < 1000; ++i) {
        int v = r.choice3();
        REQUIRE(v >= -1);
        REQUIRE(v <= 1);
    }
}
