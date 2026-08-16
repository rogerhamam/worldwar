#include "item_hotkeys.h"

#include <unordered_map>

namespace ww::hotkeys {

std::string item_key_id(const std::string& kind, const std::string& item) {
    return kind + "|" + item;
}

const std::vector<BuildingGroup>& building_groups() {
    static const std::vector<BuildingGroup> v = {
        {"base", {
            {"train", "civilian", {}},
            // Only one of industrial/war/scientific is ever offered at a
            // time (whichever matches the team's current era -- see
            // draw_command_card's show_age_up), so they share one hotkey
            // slot the same way a unit's tiered upgrade chain does above.
            {"tech", "industrial", {"war", "scientific"}},
            {"tech", "uniform", {}},
        }},
        {"barracks", {
            // muscateer -> rifleman -> infantryman is one progressive
            // training slot (each "X upgrade" tech converts the unit
            // already trainable there into the next tier) -- same for
            // artillery1 -> artillery. See game_client.cpp's
            // tech_parent_units, which anchors both tech chains below
            // whichever of these two unit slots they improve.
            {"train", "muscateer", {"rifleman", "infantryman"}},
            {"train", "artillery1", {"artillery"}},
            {"tech", "rifleman upgrade", {"infantryman upgrade"}},
            {"tech", "artillery upgrade", {}},
            // The firearm chain gets its OWN special column (see
            // tech_parent_units' comment: "sits one column to the right of
            // the Artillery Upgrade") rather than sharing muscateer's, but
            // it's still one slot among the three tiers.
            {"tech", "bolt action rifle", {"semi automatic rifle", "assault rifle"}},
        }},
        {"academy", {
            {"train", "swordsman", {"swordsman2"}},
            {"train", "cavalry", {"cavalry2", "cavalry3"}},
            {"train", "camel", {"camel corps"}},
            {"tech", "swordsman2 upgrade", {}},
            {"tech", "cavalry2 upgrade", {"cavalry3 upgrade"}},
            {"tech", "camel corps upgrade", {}},
        }},
        {"factory", {
            {"train", "light tank", {}},
            // tank -> heavy tank, plus Germany's civ substitution (tank ->
            // tiger tank -> tiger2 tank) -- all the same training slot.
            {"train", "tank", {"heavy tank", "tiger tank", "tiger2 tank"}},
            {"train", "aa gun", {"flak"}},
            // Unlike the barracks/academy tech chains above, none of
            // factory's techs are tech_parent_units-anchored -- each has
            // its own explicit, fixed, standalone position (see
            // tech_position_overrides), including "heavy tank" (the
            // RESEARCH that converts tank -> heavy tank -- a separate
            // catalog entry from the "heavy tank" UNIT above, despite the
            // shared name).
            {"tech", "blowback reload", {}},
            {"tech", "diesel engine", {}},
            {"tech", "heavy tank", {}},
        }},
        {"fortress", {
            {"train", "waffen", {"elite waffen"}},
            {"train", "janissary", {"royal janissary"}},
        }},
        {"market", {
            // None of these are tech_parent_units-anchored (market trains
            // no units) -- even the ones with a TECH_PREREQ chain among
            // themselves (irrigation -> fertilizer -> pesticide, power saw
            // -> mobile sawmill) each still get their own standalone slot,
            // unlike the unit-upgrade chains above.
            {"tech", "trade agreement", {}}, {"tech", "fertilizer", {}}, {"tech", "horse wagon", {}},
            {"tech", "irrigation", {}}, {"tech", "mobile sawmill", {}}, {"tech", "pesticide", {}},
            {"tech", "power saw", {}},
            // The trade table's 6 buy/sell buttons live on the SAME command
            // card as the techs above (unlike a shipyard's two pages, both
            // rows always show together) -- appended here so they get
            // default keys continuing the same Q..P sequence and are
            // conflict-checked against the rest of this group.
            {"trade", "buy food", {}}, {"trade", "buy wood", {}}, {"trade", "buy iron", {}},
            {"trade", "sell food", {}}, {"trade", "sell wood", {}}, {"trade", "sell iron", {}},
        }},
        {"refinery", {
            {"tech", "alloys", {}}, {"tech", "electric arc furnace", {}}, {"tech", "refined steel", {}},
        }},
        {"university", {
            {"tech", "ballistics", {}}, {"tech", "beneficiation", {}}, {"tech", "electric drill", {}},
            {"tech", "fracking", {}},
            // Synthetic Fuel is Gasoline's own upgrade tier (TECH_PREREQ
            // chains them, only one is ever available at once, same as the
            // barracks firearm chain) -- shares its slot/hotkey instead of
            // getting its own, which is what actually freed the grid slot
            // Flak Tower Upgrade below now occupies (see
            // GameClient::tech_position_overrides's "university" entry --
            // university's 2x5 command card grid was already completely
            // full before Flak Tower Upgrade was added).
            {"tech", "gasoline", {"synthetic fuel"}},
            {"tech", "jet engine", {}},
            {"tech", "smelting", {}}, {"tech", "steel frame", {}},
            {"tech", "nuclear physics", {}}, {"tech", "flak tower upgrade", {}},
        }},
        {"shipyard", {
            {"train", "fishing boat", {}},
            {"train", "transport ship", {}},
            // frigate -> torpedo boat -> destroyer, same one training slot.
            {"train", "frigate", {"torpedo boat", "destroyer"}},
            {"train", "battleship", {}}, // no predecessor to merge with (UPGRADE_MAP's first slot is empty)
            {"train", "yamato", {}},
            {"tech", "torpedo boat upgrade", {"destroyer upgrade"}}, // anchored under the frigate chain
            {"tech", "battleship upgrade", {}},                      // anchored under battleship, standalone
            // Shipyard's two "passive dock page" techs -- own standalone
            // slots (see draw_command_card's ship_passive_slot placement),
            // not anchored to any unit.
            {"tech", "hydrodynamics", {}},
            {"tech", "naval armour", {}},
        }},
        {"airbase", {
            // biplane -> fighter -> jet fighter, one training slot.
            {"train", "biplane", {"fighter", "jet fighter"}},
            // bomber -> heavy bomber, one training slot.
            {"train", "bomber", {"heavy bomber"}},
            {"train", "b29", {}},
            {"train", "ohka", {}},
            // Unlike barracks/academy/shipyard, NONE of airbase's techs are
            // tech_parent_units-anchored -- every one (including fighter
            // upgrade/heavy bomber upgrade, which DO convert a unit) has
            // its own explicit fixed position instead (see
            // tech_position_overrides), so they stay separate slots.
            //
            // The one exception is the fighter line: Jet Fighter Upgrade now
            // requires Upgrade Fighter, so the two are a progressive chain
            // sharing one cell AND one hotkey -- exactly the gasoline/synthetic
            // fuel aliasing above. Only one of them is ever researchable.
            {"tech", "composite plane armor", {}},
            {"tech", "fighter upgrade", {"jet fighter upgrade"}},
            {"tech", "heavy bomber upgrade", {}}, {"tech", "steel plane armor", {}},
            {"tech", "atomic bomb", {}},
        }},
    };
    return v;
}

std::string canonical_item(const std::string& kind, const std::string& item) {
    static const std::unordered_map<std::string, std::string> alias_map = [] {
        std::unordered_map<std::string, std::string> m;
        for (auto& group : building_groups()) {
            for (auto& entry : group.items) {
                for (auto& alias : entry.aliases) {
                    m[item_key_id(entry.kind, alias)] = entry.item;
                }
            }
        }
        return m;
    }();
    auto it = alias_map.find(item_key_id(kind, item));
    return it != alias_map.end() ? it->second : item;
}

} // namespace ww::hotkeys
