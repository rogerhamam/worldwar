#pragma once
#include "sim/building.h"
#include "sim/catalog.h"
#include "sim/unit.h"

#include <set>
#include <string>
#include <unordered_map>

namespace ww::sim {

// Civilization / leader bonuses, applied at unit/building creation and to
// costs. Direct port of game/bonuses.py. Needs only civs.json (via
// DataStore) plus the Unit/Building structs -- no World dependency, so
// (unlike the entity update()/Control AI logic) this can be fully ported
// now.
class Bonuses {
public:
    explicit Bonuses(const DataStore& data) : civs_(data.civs().at("civs")) {}

    const nlohmann::json& civ_effects(int civ) const;
    std::string leader_name(int civ, int leader) const;

    // Mutates a freshly-created Unit for its civ/leader/tech bonuses. `era` is
    // the owning team's current era (0-3), used by leader bonuses that scale
    // per age (e.g. Goering's fighter damage).
    void apply_unit(Unit& unit, int civ, int leader, int era, const std::set<std::string>& tech) const;

    // Per-leader production/construction rate multipliers (>1 = faster). Applied
    // at the building queue (Hitler: civilians) and the villager build action
    // (FDR: construction). Returns 1.0 when the leader has no such bonus.
    double leader_train_mult(int civ, int leader, const std::string& item) const;
    double leader_build_mult(int civ, int leader) const;

    // True for the "economic" upgrades (market techs + gather/farm/villager
    // upgrades) that George VI (cheaper food) and Hirohito (faster research)
    // key off. Accepts spaced or underscored names.
    bool is_economic_tech(const std::string& name) const;
    // The 7 techs sold at the market (subset of economic techs) -- Stalin's flat
    // 100/100 pricing applies only to these.
    bool is_market_tech(const std::string& name) const;

    // Age (era) a tech/unit needs under this leader, shifted down for Goering's
    // jet engine / jet fighter (available one age early). `base_age` is the
    // normal requirement.
    int leader_item_age(int civ, int leader, const std::string& item, int base_age) const;

    // Applies one researched tech's effect to a unit (also used
    // retroactively when a tech is researched with units already on the
    // field). `tech` uses underscore-normalized keys (spaces -> '_').
    void apply_tech_delta(Unit& u, const std::string& tech) const;

    void apply_building(Building& b, int civ, int leader, const std::set<std::string>& tech) const;

    // Per-resource cost multipliers/deltas for civ discounts, keyed by
    // resource name ("all" applies to every resource in the cost dict).
    std::unordered_map<std::string, double> cost_multiplier(const std::string& item, int civ, int leader,
                                                             int era) const;

    // Gather speed-up (applied to reload); lower = faster. rtype: 0=food,1=wood,2=oil,3=iron.
    double gather_multiplier(int rtype, int civ, int leader, const std::set<std::string>& tech) const;

private:
    const nlohmann::json& civs_;
};

// unit -> combat class (from get_query "class" in the GML source)
extern const std::set<std::string> RIFLE;
extern const std::set<std::string> SWORD;
extern const std::set<std::string> CAVALRY;
extern const std::set<std::string> INFANTRY; // RIFLE | SWORD | {waffen, elite waffen}
extern const std::set<std::string> TANK;

} // namespace ww::sim
