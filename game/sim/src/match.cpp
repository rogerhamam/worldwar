#include "sim/match.h"

#include <algorithm>
#include <cstring>

namespace ww::sim {

Match::Match(uint64_t seed, const SkirmishSettings& settings, std::string data_dir)
    : data_(std::move(data_dir)), bonuses_(data_),
      control_(data_, bonuses_, settings.n_players, settings.max_pop), rng_(seed) {
    world_ = new_skirmish(data_, bonuses_, control_, rng_, events_, settings);
}

Match::Match(uint64_t seed, const ww::campaign::Level& level, std::string data_dir)
    : data_(std::move(data_dir)), bonuses_(data_),
      control_(data_, bonuses_, std::max<int>(1, static_cast<int>(level.players.size())),
               std::clamp(level.pop_cap, 50, 200)), // whole-level population cap
      rng_(seed) {
    // Campaign custom units (per-level, base-template): clone the base catalog
    // unit, override its stats (and sprite, if uploaded), and inject it so that
    // placed/spawned units of that name resolve through make_unit like any
    // catalog unit. Done before new_from_level, which spawns the placed units.
    for (const auto& cu : level.custom_units) {
        if (cu.name.empty()) continue;
        const auto& units = data_.catalog().at("units");
        std::string base = units.contains(cu.base) ? cu.base : std::string("muscateer");
        if (!units.contains(base)) continue;
        nlohmann::json def = units.at(base); // clone the base unit
        def["display"] = cu.name;
        def["max_life"] = cu.hp;
        def["attack"] = cu.attack;
        def["armor"] = cu.armor;
        if (!cu.sprite.empty()) def["sprite_index"] = cu.sprite;
        data_.inject_unit(cu.name, def);
    }
    world_ = new_from_level(data_, bonuses_, control_, rng_, events_, level);
}

void Match::step(double dt) {
    world_->update(dt);
    ++tick_;
}

namespace {
void mix(uint64_t& h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
}
void mix_double(uint64_t& h, double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    mix(h, bits);
}
} // namespace

uint64_t Match::checksum() {
    uint64_t h = 1469598103934665603ull; // FNV-1a offset basis

    auto mix_common = [&](const EntityCommon& c) {
        mix(h, c.id);
        mix_double(h, c.x);
        mix_double(h, c.y);
        mix_double(h, c.hp);
        mix(h, c.alive ? 1 : 0);
        mix(h, static_cast<uint64_t>(c.team));
    };
    for (auto ref : world_->active_units) {
        if (Unit* u = world_->get(ref)) mix_common(u->common);
    }
    for (auto ref : world_->active_buildings) {
        if (Building* b = world_->get_building(ref)) {
            mix_common(b->common);
            mix_double(h, b->construction);
        }
    }
    for (auto ref : world_->active_resources) {
        if (Resource* r = world_->get_resource(ref)) { mix_common(r->common); mix_double(h, r->res.amount); }
    }
    for (auto ref : world_->active_deer) {
        if (Deer* d = world_->get_deer(ref)) mix_common(d->common);
    }
    for (auto ref : world_->active_projectiles) {
        if (Projectile* p = world_->get_projectile(ref)) mix_common(p->common);
    }
    for (auto& fire : world_->fires) {
        mix_double(h, fire.x);
        mix_double(h, fire.y);
        mix_double(h, fire.timer);
    }
    for (auto& t : control_.teams) {
        for (const char* k : {"food", "wood", "oil", "iron"}) {
            auto it = t.res.find(k);
            mix_double(h, it == t.res.end() ? 0.0 : it->second);
        }
        mix(h, static_cast<uint64_t>(t.era));
    }
    return h;
}

} // namespace ww::sim
