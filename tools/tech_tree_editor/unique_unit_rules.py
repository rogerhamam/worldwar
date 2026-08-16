"""Vendored snapshot of the per-civ unique-unit gating logic that currently
lives only in C++:

- game/client/src/menu/tech_tree_data.h: is_unique_unit(), resolve_civ_unit()
- game/sim/src/control.cpp: CIV_ONLY_UNITS, OTTOMAN_ONLY, CAMEL_CIVS,
  CIV_UNIT_SUB

Editing data/civ_exclude.json alone cannot reassign which civ owns a unique
unit, rename it, or swap it in for another unit -- that logic is hardcoded
in engine source, not data. This file exists so the editor can display
accurate read-only context for the *existing* unique units, and so the
"Unique Unit Planner" tab can stage proposals for a human to wire into the
C++ later (or, longer-term, so this whole table can move into a JSON file
the engine loads instead -- see tools/tech_tree_editor/README.md).

Keep this in sync by hand if those source files change. This mirrors the
same deliberate-vendoring convention already used by editor/src/game_data.h
for the campaign editor's copy of engine data.
"""

# Frame-3 ("unique unit") icons -- tech_tree_data.h's is_unique_unit().
UNIQUE_UNIT_NAMES = {
    "b29", "yamato", "ohka", "waffen", "elite waffen", "tiger tank",
    "tiger2 tank", "camel", "camel corps", "janissary", "royal janissary",
}

# unit -> the one civ id allowed to build it -- control.cpp's CIV_ONLY_UNITS.
# (waffen/elite waffen/tiger tank/tiger2 tank are reachable through this
# table for civ 2; camel/camel corps and janissary/royal janissary are
# gated separately below since they follow a set-membership or renaming
# rule instead of a single fixed civ id.)
CIV_ONLY_UNITS = {
    "b29": 1,
    "ohka": 4,
    "yamato": 4,
    "waffen": 2,
    "elite waffen": 2,
    "tiger tank": 2,
    "tiger2 tank": 2,
}

CAMEL_UNITS = {"camel", "camel corps"}
CAMEL_CIVS = {7, 8}  # China, Ottoman Empire

# Ottoman-only before the civ-2 renaming swap below is applied.
OTTOMAN_ONLY = {"janissary", "royal janissary"}

CIV_DISPLAY_NAME = {
    0: "United Kingdom", 1: "United States", 2: "Nazi Germany",
    3: "Soviet Union", 4: "Empire of Japan", 5: "Kingdom of Italy",
    6: "French Republic", 7: "Republic of China", 8: "Ottoman Empire",
}


def resolve_civ_unit(name: str, civ: int):
    """Mirrors ww::menu::resolve_civ_unit(). Returns the catalog key to
    display/use for this civ, or None if the grid cell should be hidden
    entirely for this civ."""
    if name == "janissary":
        if civ == 2:
            return "waffen"
        return "janissary" if civ == 8 else None
    if name == "royal janissary":
        if civ == 2:
            return "elite waffen"
        return "royal janissary" if civ == 8 else None
    if name in ("tiger tank", "tiger2 tank"):
        return name if civ == 2 else None
    if name in ("yamato", "ohka"):
        return name if civ == 4 else None
    if name in ("camel", "camel corps"):
        return name if civ in CAMEL_CIVS else None
    if name == "b29":
        return name if civ == 1 else None
    return name


def owning_civs(name: str):
    """Best-effort reverse lookup for the read-only reference panel: which
    civ id(s) currently own this unique unit, per the rules above."""
    if name in CIV_ONLY_UNITS:
        return [CIV_ONLY_UNITS[name]]
    if name in CAMEL_UNITS:
        return sorted(CAMEL_CIVS)
    if name in OTTOMAN_ONLY:
        return [8]  # civ 2 gets it too, but renamed to waffen/elite waffen
    return []
