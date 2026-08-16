"""Compiles the tool's design (explicit col/row/parent per node) down into
the legacy col/row/name/parent/child/older_sibling/force + "through"-filler
format that game/client/src/menu/tech_tree_data.h actually loads and
menu_controller.cpp's draw_tech_tree() renders -- unchanged, on purpose
(see tools/tech_tree_editor/README.md for why: the existing sprite-based
renderer already looks right, so this only swaps its data source).

Key correction vs. the ORIGINAL grid's own doc comments: menu_controller.cpp's
vertical-arrow check reads the flag on the cell ABOVE a given entry (its
"parent" flag), not on the entry itself -- i.e. "parent" is really "this
cell projects an arrow down into whatever's below it", not "I accept one
from above". This compiler follows that actual behavior, not the header's
(misleading) comment.

Siblings that share both a parent AND a row are routed as a single fan:
one "anchor" child (whichever is closest to the parent's own column) gets
the real vertical link from the parent, and the rest chain off the anchor
horizontally -- the same trunk-then-split shape tech_tree_data.h's
original hand-authored data uses (Academy -> Swordsman vertical, Swordsman
-> Cavalry horizontal), and the same shape app.py's Grid tab draws. Routing
each child straight back to the parent independently instead would produce
a second, redundant connection where one already exists via the anchor.
"""
from typing import Any, Dict, List, Optional, Tuple

Pos = Tuple[int, int]


class _Router:
    def __init__(self, occupied: Dict[Pos, str]):
        self.occupied = occupied
        self.through: Dict[Pos, str] = {}
        self.vertical_src: set = set()        # positions whose "parent" flag must be True
        self.horiz_child_src: set = set()     # positions whose "child" flag must be True
        self.horiz_sibling_dst: set = set()   # positions whose "older_sibling" flag must be True

    def _mark_straight(self, start: Pos, end: Pos) -> None:
        (sc, sr), (ec, er) = start, end
        if sc == ec and er > sr:
            for r in range(sr, er):
                self.vertical_src.add((sc, r))
                pos = (sc, r + 1)
                if pos != end and pos not in self.occupied and pos not in self.through:
                    self.through[pos] = "through"
        elif sr == er and ec != sc:
            step = 1 if ec > sc else -1
            c = sc
            while c != ec:
                self.horiz_child_src.add((c, sr))
                c += step
                self.horiz_sibling_dst.add((c, sr))
                if (c, sr) != end and (c, sr) not in self.occupied and (c, sr) not in self.through:
                    self.through[(c, sr)] = "through"

    def route(self, start: Pos, start_name: str, end: Pos, end_name: str) -> bool:
        """Connects start -> end. Returns False if an L-shaped route was
        needed but both possible corners are already occupied by unrelated
        content (caller should warn and skip)."""
        if start == end:
            return True
        if start[0] == end[0] or start[1] == end[1]:
            self._mark_straight(start, end)
            return True
        allowed = (start_name, end_name)
        corner = (start[0], end[1])
        if corner in self.occupied and self.occupied[corner] not in allowed:
            corner = (end[0], start[1])
        if corner in self.occupied and self.occupied[corner] not in allowed:
            return False
        if corner not in self.occupied and corner not in self.through:
            self.through[corner] = "through"
        self._mark_straight(start, corner)
        self._mark_straight(corner, end)
        return True


def compile_to_legacy_entries(nodes: List[Dict[str, Any]]) -> Tuple[List[Dict[str, Any]], List[str]]:
    """nodes: [{"name","col","row","parent"}] for every real node in the
    BASE design (per-civ layout overrides aren't exportable -- the engine's
    grid is civ-agnostic; per-civ redesigns stay tool-only for now).

    Returns (legacy_entries, warnings). legacy_entries is ready to json.dump
    straight into data/tech_tree_grid.json (data_io.save_json_ww_style)."""
    warnings: List[str] = []
    occupied: Dict[Pos, str] = {(n["col"], n["row"]): n["name"] for n in nodes}
    by_name = {n["name"]: n for n in nodes}

    seen_pos: Dict[Pos, List[str]] = {}
    for n in nodes:
        seen_pos.setdefault((n["col"], n["row"]), []).append(n["name"])
    for pos, names in seen_pos.items():
        if len(names) > 1:
            warnings.append(f"Collision at {pos}: {', '.join(names)} all placed on the same cell.")

    children_by_parent: Dict[str, List[Dict[str, Any]]] = {}
    for n in nodes:
        parent_name = n.get("parent")
        if not parent_name:
            continue
        if parent_name not in by_name:
            warnings.append(f"{n['name']}: parent '{parent_name}' doesn't exist -- skipped.")
            continue
        if (n["col"], n["row"]) == (by_name[parent_name]["col"], by_name[parent_name]["row"]):
            warnings.append(f"{n['name']}: same position as its parent '{parent_name}' -- skipped.")
            continue
        children_by_parent.setdefault(parent_name, []).append(n)

    router = _Router(occupied)
    for parent_name, children in children_by_parent.items():
        p = by_name[parent_name]
        p_pos: Pos = (p["col"], p["row"])
        by_row: Dict[int, List[Dict[str, Any]]] = {}
        for c in children:
            by_row.setdefault(c["row"], []).append(c)

        for row, group in by_row.items():
            anchor = min(group, key=lambda c: abs(c["col"] - p["col"]))
            anchor_pos: Pos = (anchor["col"], anchor["row"])
            if not router.route(p_pos, parent_name, anchor_pos, anchor["name"]):
                warnings.append(f"{anchor['name']}: couldn't route to parent '{parent_name}' -- "
                                 f"both possible corner cells are occupied by unrelated nodes.")
                continue
            row_sorted = sorted(group, key=lambda c: c["col"])
            idx = row_sorted.index(anchor)
            if idx < len(row_sorted) - 1:
                far = row_sorted[-1]
                router.route(anchor_pos, anchor["name"], (far["col"], far["row"]), far["name"])
            if idx > 0:
                near = row_sorted[0]
                router.route(anchor_pos, anchor["name"], (near["col"], near["row"]), near["name"])

    all_positions: Dict[Pos, str] = dict(occupied)
    all_positions.update(router.through)

    entries: List[Dict[str, Any]] = []
    for pos, name in all_positions.items():
        col, row = pos
        # Defensive suppression: the legacy format's "parent"/"child" flags
        # apply to ANY cell below/right of them, not just an intended edge
        # -- so a cell only gets to stay "true" by default if there's
        # nothing there to accidentally connect to; otherwise it must be
        # part of an intended edge, or it's forced false.
        parent_flag = pos in router.vertical_src or (col, row + 1) not in all_positions
        child_flag = pos in router.horiz_child_src or (col + 1, row) not in all_positions
        sibling_flag = pos in router.horiz_sibling_dst or (col - 1, row) not in all_positions

        entry: Dict[str, Any] = {"col": col, "row": row, "name": name}
        if not parent_flag:
            entry["parent"] = False
        if not child_flag:
            entry["child"] = False
        if not sibling_flag:
            entry["older_sibling"] = False
        entries.append(entry)

    entries.sort(key=lambda e: (e["col"], e["row"]))
    return entries, warnings
