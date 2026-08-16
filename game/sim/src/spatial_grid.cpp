#include "sim/spatial_grid.h"

#include <cmath>

namespace ww::sim {

SpatialGrid::SpatialGrid(int cell) : cell_(cell) {}

void SpatialGrid::clear() { buckets_.clear(); }
void SpatialGrid::clear_static() { static_buckets_.clear(); }

SpatialGrid::Key SpatialGrid::key_of(double x, double y) const {
    return Key{static_cast<int32_t>(std::floor(x / cell_)),
               static_cast<int32_t>(std::floor(y / cell_))};
}

void SpatialGrid::insert(EntityRef ref, double x, double y) {
    buckets_[key_of(x, y)].push_back(ref);
}
void SpatialGrid::insert_static(EntityRef ref, double x, double y) {
    static_buckets_[key_of(x, y)].push_back(ref);
}

void SpatialGrid::query_into(double x, double y, double radius, std::vector<EntityRef>& out) const {
    out.clear();
    int r = static_cast<int>(radius) / cell_ + 1;
    int cx = static_cast<int>(std::floor(x / cell_));
    int cy = static_cast<int>(std::floor(y / cell_));
    for (int gx = cx - r; gx <= cx + r; ++gx) {
        for (int gy = cy - r; gy <= cy + r; ++gy) {
            Key k{gx, gy};
            // Dynamic (units/deer) first, then static (buildings/resources),
            // matching the per-cell ordering combat/AI tie-breaks expect.
            auto it = buckets_.find(k);
            if (it != buckets_.end())
                for (const auto& ref : it->second) out.push_back(ref);
            auto sit = static_buckets_.find(k);
            if (sit != static_buckets_.end())
                for (const auto& ref : sit->second) out.push_back(ref);
        }
    }
}

std::vector<EntityRef> SpatialGrid::query(double x, double y, double radius) const {
    std::vector<EntityRef> out;
    query_into(x, y, radius, out);
    return out;
}

} // namespace ww::sim
