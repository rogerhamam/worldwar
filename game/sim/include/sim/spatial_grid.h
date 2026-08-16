#pragma once
#include "sim/entity_common.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ww::sim {

// Uniform spatial hash grid for O(1) neighbour queries. Direct port of
// game/spatial.py. Deliberately dependency-free (no World/entity-data
// access): it stores only EntityRef + the position given at insert time,
// bucketed by cell. Callers (Match/World) resolve refs to live entity
// data and apply their own predicates -- see World::nearest() (Phase B,
// world.cpp) for the equivalent of spatial.py's nearest(pred=...).
//
// Determinism note: bucket keys are looked up directly (not iterated),
// so using unordered_map for the bucket table itself is safe; what must
// stay order-preserving is each bucket's vector<EntityRef> (insertion
// order == the order entities were rebuilt from World's per-kind active
// lists), since combat/AI tie-breaks depend on it.
class SpatialGrid {
public:
    explicit SpatialGrid(int cell = 64);

    // Clears the DYNAMIC layer only (units/deer, rebuilt every tick). The
    // static layer (buildings/resources) persists across ticks -- see below.
    void clear();
    void insert(EntityRef ref, double x, double y);

    // STATIC layer: buildings + resources, which never move. Rebuilt only when
    // that set changes (World::static_grid_dirty_), NOT every tick -- re-hashing
    // the ~1500+ map resources every tick was the dominant sim cost on the
    // larger current maps. clear_static() wipes it; insert_static() adds one.
    void clear_static();
    void insert_static(EntityRef ref, double x, double y);

    // Refs whose bucket overlaps a (radius x radius) box around (x, y), in the
    // same fixed gx-then-gy scan order as game/spatial.py's query(); within a
    // cell, DYNAMIC entries (units/deer) come first, then STATIC (buildings/
    // resources). Combat tie-breaks compare same-kind entities, and units stay
    // ahead of buildings (dynamic before static), so this preserves them.
    std::vector<EntityRef> query(double x, double y, double radius) const;
    // Same as query() but fills a caller-provided buffer (cleared first) instead
    // of allocating+returning a fresh vector -- lets a hot, per-tick caller reuse
    // one buffer across thousands of calls and skip the per-call heap churn. The
    // contents/order are byte-identical to query(), so results (and the golden
    // checksum) are unchanged. NOT re-entrant on the SAME buffer (don't nest two
    // query_into calls that share it).
    void query_into(double x, double y, double radius, std::vector<EntityRef>& out) const;

private:
    struct Key {
        int32_t gx, gy;
        bool operator==(const Key& o) const { return gx == o.gx && gy == o.gy; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return (static_cast<uint64_t>(static_cast<uint32_t>(k.gx)) << 32) ^
                   static_cast<uint32_t>(k.gy);
        }
    };

    Key key_of(double x, double y) const;

    int cell_;
    std::unordered_map<Key, std::vector<EntityRef>, KeyHash> buckets_;        // dynamic (units/deer)
    std::unordered_map<Key, std::vector<EntityRef>, KeyHash> static_buckets_; // buildings/resources
};

} // namespace ww::sim
