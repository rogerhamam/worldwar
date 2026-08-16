#pragma once
#include <SDL.h>

#include <array>
#include <string>
#include <vector>

// Presentational-only civ data for the menu (names, bonus-bullet text,
// team colour palette). None of this lives in the sim -- it mirrors the
// original GameMaker source's get_civ_name.gml/get_civ_desc.gml/
// get_col_rgb.gml scripts, which are themselves just menu display text,
// not simulation state (bonuses.cpp already applies the underlying numeric
// effects; this is only the human-readable description of them). Civ
// indices (0-8) match scenario.cpp's CIV_BASE table and Bonuses' civ
// lookups.
namespace ww::menu {

inline const std::array<std::string, 9>& civ_names() {
    static const std::array<std::string, 9> names = {
        "United Kingdom", "United States",  "Nazi Germany",     "Soviet Union",   "Empire of Japan",
        "Kingdom of Italy", "French Republic", "Republic of China", "Ottoman Empire",
    };
    return names;
}

// Not every civ has exactly 4 bullets in the original -- some have as few
// as 1 (France) or 3 (USA/Soviet/Japan); rendered as however many exist.
inline const std::array<std::vector<std::string>, 9>& civ_bonuses() {
    static const std::array<std::vector<std::string>, 9> bonuses = {{
        {"Civilians +5 carry capacity", "Rifle line shoots 25% faster", "Cavalry cost 25% less"},
        {"10% faster civilians", "10% cheaper infantry", "Farms cost 25% less"},
        {"Iron collected 20% faster", "Houses cost 20 wood",
         "Jet & rocket units +25% damage"},
        {"Wood collected 10% faster", "Houses support more population", "Buildings have 50% more HP"},
        {"Ships have +1 range", "Swordsmen attack 20% faster", "Planes are 20% cheaper"},
        {"Infantry move 10% faster with +1/+1 armor", "Fishing ships work 20% faster",
         "Ships gain +5% HP each era"},
        {"Cavalry have 20% extra HP", "All farm upgrades free", "Artillery +1 range"},
        {"Start with an extra villager", "Bases do not cost any iron",
         "All land military units cost 10% less"},
        {"50% oil discount for techs", "Barracks units produced 30% faster",
         "Free tech: Trade Agreement"},
    }};
    return bonuses;
}

// Per-civ UNIQUE units and technologies shown in the civ-chooser preview
// (each entry is {catalog name, is_unit}); the display name + icon are read
// from the catalog at render time. Mirrors control.cpp's CIV_ONLY_UNITS /
// CIV_UPGRADE_OWNER single-civ ownership -- the headline uniques, not every
// upgrade tier. Civs with none (Italy/France) show an empty list.
inline const std::vector<std::pair<std::string, bool>>& civ_unique_items(int civ) {
    static const std::array<std::vector<std::pair<std::string, bool>>, 9> items = {{
        {{"royal marine", true}, {"naval hegemony", false}},                             // 0 UK
        {{"b29", true}},                                                                  // 1 USA
        {{"waffen", true}, {"tiger tank", true}, {"emergency fighter program", false}},  // 2 Germany
        {{"heavy artillery", true}, {"420mm mortar", false}},                             // 3 Soviet
        {{"yamato", true}, {"ohka", true}, {"meiji restoration", false}},                 // 4 Japan
        {},                                                                               // 5 Italy
        {},                                                                               // 6 France
        {},                                                                               // 7 China (camel no longer a unique unit)
        {{"janissary", true}, {"camel corps", true}}, // 8 Ottoman
    }};
    static const std::vector<std::pair<std::string, bool>> empty;
    return (civ >= 0 && civ < 9) ? items[civ] : empty;
}

// Per-civ tagline shown above the bonus list (objects/control/Draw.gml's
// menu==1 branch: draw_text(..., type + " Empire") in fnt_tiny -- the small
// font, matched here by drawing this with the menu's small text renderer).
inline const std::array<std::string, 9>& civ_types() {
    static const std::array<std::string, 9> types = {
        "Plane Empire", "Scientific and Plane Empire", "Tank and Infantry Empire",
        "Economic and Infantry Empire", "Naval and Plane Empire", "Infantry and Naval Empire",
        "Cavalry and Infantry Empire", "Guerilla and Populist Empire", "Gunpowder Empire",
    };
    return types;
}

// Index into spr_flags/spr_flags_mini's frames, same order as civ_names().
constexpr int kCivFlagFrame[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};

// Direct port of scripts/get_leader_name.gml's per-civ leader table (3
// slots each, though not every civ has all 3 filled -- empty string marks
// an unused slot). Team::leader (sim/include/sim/control.h) indexes into
// this, defaulting to 0 for whichever civ is chosen.
inline const std::array<std::array<std::string, 3>, 9>& leader_names() {
    // 3 leaders per civ. Order is chosen so each name lines up with its own
    // 50x50 face in spr_leaders (frame = civ*3 + index). Germany is Hitler /
    // Goering / Rommel per the design; Goering reuses the third German-officer
    // photo. Names MUST match civs.json's "leaders"[].name exactly -- the sim's
    // Bonuses::apply_unit keys the per-leader effect off the leader name string.
    static const std::array<std::array<std::string, 3>, 9> names = {{
        {"Winston Churchill", "George VI", "Bernard Montgomery"},
        {"Franklin D. Roosevelt", "Harry S. Truman", "Dwight D. Eisenhower"},
        {"Adolf Hitler", "Hermann Goering", "Erwin Rommel"},
        {"Joseph Stalin", "Nikita Khrushchev", "Georgy Zhukov"},
        {"Emperor Hirohito", "Hideki Tojo", "Isoroku Yamamoto"},
        {"Benito Mussolini", "Giovanni Messe", "Italo Balbo"},
        {"Charles de Gaulle", "Napoleon", "Philippe Petain"},
        {"Mao Zedong", "Li Zongren", "Chiang Kai-Shek"},
        {"Enver Pasha", "Mehmed V", "Mustafa Ataturk"},
    }};
    return names;
}

// One-line bonus each leader grants, shown in the civ-chooser preview under the
// civ's own bonus stack. Parallel to leader_names(). The mechanical effect is
// implemented in sim/src/bonuses.cpp (apply_unit / the Hitler+FDR hooks), keyed
// by the leader name -- keep the wording here in sync with what the code does.
inline const std::array<std::array<std::string, 3>, 9>& leader_bonuses() {
    static const std::array<std::array<std::string, 3>, 9> b = {{
        {"Advance to the next era 50% faster", "Economic techs cost 25% less food", "Tanks have 20% more HP"},
        {"Buildings construct 30% faster", "Bombers & nukes +25% damage and blast", "Infantry have 33% more HP"},
        {"Civilians produced 15% faster", "Jet engine and jet fighter available in the war era; jet technologies 50% cheaper", "Tanks move 20% faster"},
        {"Market techs cost 100 food/wood, no trade fee", "Artillery +25% attack", "Tanks cost 25% less"},
        {"Economic upgrades research 100% faster", "All infantry move 20% faster", "Carriers +1 plane; warships +20% attack"},
        {"Age-ups cost 20% less", "Farmers work 10% faster", "Warships & light tanks fire 25% faster"},
        {"Bases cost 50% less wood", "Artillery fire 25% faster", "Infantry & light tanks cost 20% less oil"},
        {"Villagers cost 10/15/20/25% less by era", "Infantry can move through trees", "Infantry +2 damage vs tanks"},
        {"Cavalry fire while charging (every 10s)", "Camels & cavalry +1/+1 armor, attack 10% faster", "Oil gathered 10% faster"},
    }};
    return b;
}

// Falls back to the civ's own name if `civ`/`leader` are out of range or
// point at an unused slot (e.g. leader index 2 for UK, which only has 2).
inline std::string leader_name(int civ, int leader) {
    if (civ < 0 || civ >= 9) return "Unknown";
    if (leader < 0 || leader >= 3 || leader_names()[civ][leader].empty()) return civ_names()[civ];
    return leader_names()[civ][leader];
}

// Team colour palette (assets/gmk/scripts/get_col_rgb.gml), indexed 0-7.
inline const std::array<SDL_Color, 8>& team_colours() {
    static const std::array<SDL_Color, 8> colours = {{
        {0, 0, 255, 255},     // 0 blue
        {255, 0, 0, 255},     // 1 red
        {0, 192, 0, 255},     // 2 green
        {255, 255, 0, 255},   // 3 yellow
        {0, 255, 255, 255},   // 4 cyan
        {255, 0, 255, 255},   // 5 magenta
        {128, 128, 128, 255}, // 6 gray
        {255, 128, 0, 255},   // 7 orange
    }};
    return colours;
}

} // namespace ww::menu
