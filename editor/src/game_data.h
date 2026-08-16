#pragma once
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// A small, standalone slice of the main engine's static game-design data
// (which building trains which units, which units are civ-restricted) --
// vendored here on purpose rather than linking the main repo's sim
// library, so this project has ZERO code dependency on game and
// can be developed/built entirely on its own (see this repo's README).
// This is just data (WW2 unit/building rosters), not simulation logic --
// it should rarely need to change, but if the main engine's roster ever
// does (game/sim/src/control.cpp's own copy of these same
// tables), keep this file in sync by hand.
namespace ww::gamedata {

// building name -> unit names it can train (era/tech gating deliberately
// NOT applied here, unlike the main sim's Control::available_units --
// this editor palette should offer a civ's whole roster regardless of
// era, since the author is hand-placing starting units, not training
// them mid-match).
extern const std::unordered_map<std::string, std::vector<std::string>> PRODUCTION;

// Buildable building names (excludes "base"/town center, which is always
// available and not civ-gated -- add it separately in the palette).
extern const std::vector<std::string> BUILDABLE;

extern const std::set<int> CAMEL_CIVS;
extern const std::set<std::string> CAMEL_UNITS;
extern const std::set<std::string> OTTOMAN_ONLY;
extern const std::unordered_map<std::string, int> CIV_ONLY_UNITS;

// civ index -> that civ's town-center sprite (vendored from scenario.cpp's
// CIV_BASE) -- "base"'s catalog entry has no generic sprite_index because,
// unlike every other building, its art is civ-specific and assigned at
// spawn time rather than looked up from the catalog.
extern const std::unordered_map<int, std::string> CIV_BASE;

// Native pixel footprint (w, h) per building, vendored verbatim from
// sim/src/world.cpp's building_wh() -- every value is an exact multiple of
// TILE (32), and (per that function's own comment) a building's screen
// centre must land at (tx*TILE + w/2, ty*TILE + h/2) for a building whose
// top-left footprint tile is (tx,ty), NOT (tx+0.5)*TILE -- that formula
// only happens to look right for odd tile-counts (base/fortress, 3x3) and
// is visibly wrong for even ones (house/barracks/etc, 2x2), which is
// exactly the "encroaches on 9 cells instead of 4" bug this fixes.
std::pair<int, int> building_wh(const std::string& name);

// Direct copy of Control::civ_has (sim/src/control.cpp) -- true unless
// `civ_exclude` (parsed from data/civ_exclude.json) explicitly lists
// `item` as excluded for `civ`.
bool civ_has(const std::string& item, int civ,
            const std::unordered_map<int, std::set<std::string>>& civ_exclude);

} // namespace ww::gamedata
