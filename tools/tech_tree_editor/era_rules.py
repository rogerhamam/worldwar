"""Vendored snapshot of the hardcoded per-item era gates in
game/sim/src/control.cpp (TECH_ERA / UNIT_ERA / BUILDING_ERA). Anything not
listed defaults to era 0, matching Control::available_*()'s
`era != X_ERA.end() ? era->second : 0` fallback.

Same vendoring convention as unique_unit_rules.py -- keep in sync by hand
if control.cpp changes. This is why the Grid tab's era dividers/badges are
computed per-item rather than by grid row: row and era do NOT correlate
(e.g. row 0 alone contains both era-0 and era-2 items, since a grid column
is one upgrade *line*, not one era -- see tools/tech_tree_editor/README.md).
"""

# Display names for era ids 0-3, per the designer (2026-07-19) -- NOT
# catalog.json's "ages" list or civs.json's "eras" list, both of which
# still say "Industrial"/"Interwar" for ids 0/1. Same 4 numeric ids as
# control.cpp's TECH_ERA/UNIT_ERA/BUILDING_ERA below; only the label
# changed (id 0 "Industrial"->"Victorian", id 1 "Interwar"->"Industrial").
ERA_NAMES = ["Victorian Era", "Industrial Era", "War Era", "Scientific Era"]

ERA_COLOR = {
    0: "#8a8f98",  # Victorian -- neutral grey
    1: "#4f8ad4",  # Industrial -- blue
    2: "#c76b2e",  # War -- orange
    3: "#b23fd0",  # Scientific -- purple
}

TECH_ERA = {
    "uniform": 0, "irrigation": 0, "binoculars": 1,
    "elite waffen upgrade": 3, "royal janissary upgrade": 2, "tiger2 tank upgrade": 3,
    "rifleman upgrade": 1, "infantryman upgrade": 2, "swordsman2 upgrade": 1,
    "cavalry2 upgrade": 1, "refined steel": 1, "steel frame": 1, "electric drill": 1,
    "smelting": 1, "power saw": 1, "horse wagon": 1, "fertilizer": 1,
    "torpedo boat upgrade": 1,
    "cavalry3 upgrade": 2, "camel corps upgrade": 2, "alloys": 2, "diesel engine": 2,
    "blowback reload": 2, "steel plane armor": 2, "composite plane armor": 2,
    "fighter upgrade": 2, "destroyer upgrade": 2, "battleship upgrade": 2,
    "ballistics": 2, "fracking": 2, "beneficiation": 2, "gasoline": 2,
    "pesticide": 2, "mobile sawmill": 2,
    "electric arc furnace": 3, "heavy tank": 3, "heavy bomber upgrade": 3,
    "jet engine": 3, "jet fighter upgrade": 3, "synthetic fuel": 3,
    "nuclear physics": 3, "atomic bomb": 3, "assault rifle": 3,
    "bolt action rifle": 1, "semi automatic rifle": 2,
    "hydrodynamics": 2, "naval armour": 2, "trade agreement": 1, "artillery upgrade": 1,
}

UNIT_ERA = {
    "civilian": 0, "swordsman": 0, "cavalry": 0, "muscateer": 0, "camel": 0,
    "janissary": 0, "fishing boat": 0, "frigate": 0, "artillery1": 0,
    "rifleman": 1, "infantryman": 1, "cavalry2": 1, "swordsman2": 1, "light tank": 1,
    "artillery": 1, "aa gun": 1, "biplane": 1, "fighter": 1, "destroyer": 1,
    "torpedo boat": 1,
    "waffen": 2, "cavalry3": 2, "camel corps": 2, "tank": 2, "bomber": 2, "flak": 2,
    "battleship": 2, "tiger tank": 2, "royal janissary": 2,
    "heavy tank": 3, "elite waffen": 3, "jet fighter": 3, "heavy bomber": 3, "b29": 3,
    "ohka": 3, "yamato": 3, "tiger2 tank": 3, "me262": 3,
}

BUILDING_ERA = {
    "base": 2,  # buildable expansion base -- the starting capitol itself is free
    "house": 0, "farm": 0, "barracks": 0, "market": 0, "academy": 0,
    "palisade": 0, "tower": 0, "refinery": 0, "shipyard": 0,
    "factory": 1, "university": 1, "airbase": 1, "aa tower": 1,
    "fortress": 2, "nuclear reactor": 3, "outpost": 0,
}


def era_of(key: str, category: str) -> int:
    table = {"tech": TECH_ERA, "unit": UNIT_ERA, "building": BUILDING_ERA}.get(category)
    if table is None:
        return 0
    return table.get(key, 0)


# --- Design-target row bands ------------------------------------------
#
# The above tables are what era-gates an item *today*, in the shipped
# game -- but that's a separate hardcoded axis from the tech tree's grid
# *row*, and the two don't line up (row 0 alone holds both era-0 and
# era-2 items). Rather than reflect that mismatch as-is, the tech tree
# editor treats row as the *design* axis going forward: each item's row
# is assigned to one of 4 fixed bands below, redesigning the tree so
# position and era finally mean the same thing. Dragging a node across a
# band boundary is how you change which era it's placed in.
#
# This is a starting default, not a law of physics -- adjust ROWS_PER_ERA
# if 2 rows/era turns out to be too cramped or too roomy once the roster
# actually gets redesigned into it.
ROWS_PER_ERA = 2


def era_for_row(row: int) -> int:
    return min(row // ROWS_PER_ERA, len(ERA_NAMES) - 1)


def band_row_range(era_id: int, max_row: int = 0):
    """(first_row, first_row_of_next_band) for this era's band. The last
    band is open-ended downward to cover however far the design has grown."""
    top = era_id * ROWS_PER_ERA
    if era_id < len(ERA_NAMES) - 1:
        return top, top + ROWS_PER_ERA
    return top, max(top + ROWS_PER_ERA, max_row + 1)
