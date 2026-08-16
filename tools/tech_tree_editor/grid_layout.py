"""Parses the tech tree's visual grid layout out of
data/tech_tree_grid.json -- the file game/client/src/menu/tech_tree_data.h
now actually loads at runtime (see tools/tech_tree_editor/README.md for the
migration off a hardcoded C++ vector). Re-read on every launch, same as
before, so the editor can't drift from what the game renders; it's just a
JSON read now instead of a live parse of the header.
"""
import json
from pathlib import Path
from typing import Any, Dict, List, Optional

from paths import GRID_JSON


def parse_grid(path: Path = GRID_JSON) -> List[Dict[str, Any]]:
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    entries = []
    for item in raw:
        entries.append({
            "col": item["col"],
            "row": item["row"],
            "name": item["name"],
            "parent": item.get("parent", True),
            "child": item.get("child", True),
            "older_sibling": item.get("older_sibling", True),
            "force": item.get("force", False),
        })
    return entries


def infer_default_parents(entries: List[Dict[str, Any]]) -> Dict[str, Optional[str]]:
    """One-time-per-launch migration from the legacy flag/position-based
    arrow system to an explicit single-parent graph (see app.py's
    draw_grid, which draws real Manhattan trunk+split arrows off this
    instead of legacy cell-adjacency inference) and legacy_export.py
    (which compiles the parent graph back down to this same legacy format
    for the game to actually consume).

    Correction worth calling out: despite its name and its doc comment in
    tech_tree_data.h ("accepts an incoming vertical arrow from directly
    above"), a cell's "parent" flag in ACTUAL use (menu_controller.cpp's
    draw_tech_tree) belongs to the cell ABOVE a given entry, not the entry
    itself -- it means "this cell projects an arrow down into whatever's
    below it". This walks that real behavior, not the misleading comment.

    Horizontal chains resolve back to the shared ancestor, so in Academy ->
    Swordsman (vertical) + Swordsman -> Cavalry (older-sibling horizontal),
    Cavalry's inferred parent comes out as "academy", not "swordsman".

    Not authoritative -- a reasonable starting point for a human to correct
    in the tool (right-click -> Set Parent), not a guarantee every node
    ends up connected to something. Roots (mostly buildings, plus some
    genuine gaps in the original layout) resolve to None.
    """
    pos = {(e["col"], e["row"]): e for e in entries}

    def connects_down_into(above_pos, below_pos) -> bool:
        above, below = pos.get(above_pos), pos.get(below_pos)
        if above is None or below is None:
            return False
        return above.get("parent", True) or below.get("force", False)

    def real_ancestor_above(col: int, row: int) -> Optional[str]:
        r = row
        while True:
            above_pos = (col, r - 1)
            if not connects_down_into(above_pos, (col, r)):
                return None
            e = pos[above_pos]
            if e["name"] != "through":
                return e["name"]
            r -= 1

    resolved: Dict[str, Optional[str]] = {}
    for e in sorted(entries, key=lambda e: (e["col"], e["row"])):
        name = e["name"]
        if name == "through":
            continue
        col, row = e["col"], e["row"]
        parent = real_ancestor_above(col, row)
        if parent is None:
            left = pos.get((col - 1, row))
            if (left is not None and left["name"] != "through"
                    and left.get("child", True) and e.get("older_sibling", True)):
                parent = resolved.get(left["name"], left["name"])
        resolved[name] = parent
    return resolved
