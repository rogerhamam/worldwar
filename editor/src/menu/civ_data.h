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
        {"Civilians +5 carry capacity", "Rifle line shoots 25% faster", "Cavalry cost 25% less",
         "Advance to next era 50% faster"},
        {"10% faster civilians", "10% cheaper infantry", "Farms cost 25% less"},
        {"Iron collected 10% faster", "Tanks cost 20% less iron", "Houses cost 10 wood"},
        {"Wood collected 10% faster", "Houses support more population", "Buildings have 50% more HP"},
        {"Ships have +1 range", "Swordsmen attack 20% faster", "Planes are 20% cheaper"},
        {"Infantry move 10% faster", "Infantry have +1/+1 armor", "Fishing ships work 20% faster",
         "Ships gain +5% HP each era"},
        {"Cavalry have 20% extra HP"},
        {"Start with an extra villager", "Bases do not cost any iron"},
        {"50% oil discount for techs", "Rifle units produced 30% faster"},
    }};
    return bonuses;
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
    static const std::array<std::array<std::string, 3>, 9> names = {{
        {"Winston Churchill", "George VI", ""},
        {"Franklin D. Roosevelt", "Harry S. Truman", "Dwight D. Eisenhower"},
        {"Adolf Hitler", "Heinrich Himmler", "Erwin Rommel"},
        {"Joseph Stalin", "Nikita Krushchev", "Georgy Zhukov"},
        {"Emperor Hirohito", "Hideki Tojo", ""},
        {"Benito Mussolini", "Giovanni Messe", ""},
        {"Charles de Gaulle", "Maurice Gamelin", "Phillipe Petain"},
        {"Chairman Mao Zedong", "Peng Dehuai", "Chiang Kai-Shek"},
        {"Enver Pasha", "Mehmed V", "Mustafa Ataturk"},
    }};
    return names;
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
