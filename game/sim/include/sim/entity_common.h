#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace ww::sim {

enum class EntityKind : uint8_t { Unit, Building, Resource, Deer, Projectile, Effect };

// A plain optional 2D point (move_goal / rally / hold-position anchors,
// etc. in game/entity.py -- each is either an (x, y) tuple or None).
struct Vec2 {
    double x = 0.0, y = 0.0;
};

// A stable handle to an entity that survives across ticks. Resolving a
// ref whose slot has since died and been reused yields nullptr (the
// generation won't match) -- this mirrors the Python version's "hold a
// reference, then check .alive" pattern (Unit.attack_target,
// Building.occupied_by, etc. in game/entity.py) without ever dangling.
struct EntityRef {
    EntityKind kind = EntityKind::Unit;
    uint32_t slot = 0;
    uint32_t generation = 0; // 0 == null ref; SlotMap generations start at 1

    bool valid() const { return generation != 0; }
};

inline bool operator==(const EntityRef& a, const EntityRef& b) {
    return a.kind == b.kind && a.slot == b.slot && a.generation == b.generation;
}
inline bool operator!=(const EntityRef& a, const EntityRef& b) { return !(a == b); }
constexpr EntityRef kNullRef{};

// Fields shared by every entity kind, hoisted out of the per-kind structs
// (composition, not inheritance) because a handful of generic world-level
// queries -- passability, fog sight radius, the separation pass -- need
// to check is_air/is_ship/is_solid across kinds without dynamic dispatch.
// See game/world.py: passable() (is_air/is_ship), the separation pass,
// and fog-of-war sight all touch exactly these fields generically; every
// other field is kind-specific and lives on Unit/Building/etc. instead.
struct EntityCommon {
    uint32_t id = 0;
    EntityKind kind = EntityKind::Unit;
    int team = -1;
    double x = 0.0, y = 0.0;
    double hp = 1.0, max_hp = 1.0;
    bool alive = true;
    bool is_air = false;
    bool is_ship = false;
    bool is_solid = true;
    // Seconds remaining to float this entity's HP bar above it after taking
    // damage (set by World::hurt, ticked down in the per-entity update). Purely
    // a render cue -- the client shows a health bar for units/buildings while
    // this is > 0, so a hit that isn't currently selected still flashes its HP.
    double dmg_flash = 0.0;
};

// Generation-checked slot map. One instance per entity kind, owned by
// Match/World. Cross-entity references (attack_target, gather_target,
// occupied_by, ...) store an EntityRef into the relevant kind's SlotMap
// rather than a raw pointer/index, so entities can die and their slots
// be recycled without leaving dangling references elsewhere.
template <typename T>
class SlotMap {
public:
    // Returns the slot index; caller pairs it with generation_of(slot)
    // to build a full EntityRef.
    uint32_t insert(T value) {
        uint32_t slot;
        if (!free_list_.empty()) {
            slot = free_list_.back();
            free_list_.pop_back();
            slots_[slot] = std::move(value);
        } else {
            slot = static_cast<uint32_t>(slots_.size());
            slots_.push_back(std::optional<T>(std::move(value)));
            generations_.push_back(1);
        }
        return slot;
    }

    void erase(uint32_t slot) {
        slots_[slot].reset();
        generations_[slot]++; // odd/even doesn't matter, just must change
        if (generations_[slot] == 0) generations_[slot] = 1; // skip 0 (== null ref)
        free_list_.push_back(slot);
    }

    T* get(uint32_t slot, uint32_t generation) {
        if (slot >= slots_.size() || generations_[slot] != generation) return nullptr;
        return slots_[slot] ? &*slots_[slot] : nullptr;
    }
    T* get(EntityRef ref) { return get(ref.slot, ref.generation); }
    const T* get(uint32_t slot, uint32_t generation) const {
        if (slot >= slots_.size() || generations_[slot] != generation) return nullptr;
        return slots_[slot] ? &*slots_[slot] : nullptr;
    }
    const T* get(EntityRef ref) const { return get(ref.slot, ref.generation); }

    T* raw(uint32_t slot) { return slots_[slot] ? &*slots_[slot] : nullptr; }
    uint32_t generation_of(uint32_t slot) const { return generations_[slot]; }
    size_t capacity() const { return slots_.size(); }

private:
    std::vector<std::optional<T>> slots_;
    std::vector<uint32_t> generations_;
    std::vector<uint32_t> free_list_;
};

} // namespace ww::sim
