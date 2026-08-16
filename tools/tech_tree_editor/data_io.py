"""Load/save the game-data JSON files, matching the project's existing
on-disk formatting exactly (1-space indent, CRLF line endings, no trailing
newline) so tool-written diffs stay clean and reviewable.
"""
import json
from pathlib import Path
from typing import Any, Dict, List

from paths import (
    CATALOG_JSON,
    CIV_EXCLUDE_JSON,
    CIVS_JSON,
    BUILDING_TECHS_JSON,
    GRID_JSON,
    LAYOUT_JSON,
    PROPOSALS_JSON,
    TECH_DESC_JSON,
)


def load_json(path: Path) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def save_json_ww_style(path: Path, obj: Any) -> None:
    """Writes JSON the same way the rest of data/ is formatted: indent=1,
    CRLF newlines, no trailing newline at EOF."""
    text = json.dumps(obj, indent=1, ensure_ascii=False)
    with open(path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)


def load_civs() -> List[Dict[str, Any]]:
    return load_json(CIVS_JSON)["civs"]


def load_civ_exclude() -> Dict[int, List[str]]:
    # Lists, not sets: the existing file's entries are NOT alphabetically
    # sorted (items were appended over time, e.g. civ 4's list ends with
    # "waffen", "nuclear physics", "nuclear reactor" -- out of order).
    # Preserving on-disk order and only appending/removing the toggled
    # entry keeps saved diffs limited to the actual edit instead of
    # reshuffling every civ's whole list.
    raw = load_json(CIV_EXCLUDE_JSON)
    return {int(k): list(v) for k, v in raw.items()}


def toggle_civ_access(civ_exclude: Dict[int, List[str]], civ: int, key: str) -> None:
    lst = civ_exclude.setdefault(civ, [])
    if key in lst:
        lst.remove(key)
    else:
        lst.append(key)


def save_civ_exclude(civ_exclude: Dict[int, List[str]]) -> None:
    # Keys as strings in civ-id order. Every civ key is kept even if its
    # list is now empty, so a civ that had exclusions and had them all
    # cleared still round-trips as "[]" instead of silently dropping the
    # key.
    out = {str(civ): list(civ_exclude[civ]) for civ in sorted(civ_exclude)}
    save_json_ww_style(CIV_EXCLUDE_JSON, out)


def load_catalog() -> Dict[str, Any]:
    return load_json(CATALOG_JSON)


def load_tech_desc() -> Dict[str, Any]:
    return load_json(TECH_DESC_JSON)


def load_building_techs() -> Dict[str, List[str]]:
    return load_json(BUILDING_TECHS_JSON)


def load_proposals() -> Dict[str, Any]:
    if not PROPOSALS_JSON.is_file():
        return {"unique_units": []}
    return load_json(PROPOSALS_JSON)


def save_proposals(proposals: Dict[str, Any]) -> None:
    save_json_ww_style(PROPOSALS_JSON, proposals)


def load_layout() -> Dict[str, Any]:
    """{"base": {name: {"col","row"}}, "civs": {"<civ id>": {name: {...}}}}
    Only entries that differ from the underlying layout are stored -- a
    civ entry means "this civ's position differs from base"; a base entry
    means "the redesigned base position differs from tech_tree_data.h"."""
    if not LAYOUT_JSON.is_file():
        return {"base": {}, "civs": {}}
    data = load_json(LAYOUT_JSON)
    data.setdefault("base", {})
    data.setdefault("civs", {})
    return data


def save_layout(layout: Dict[str, Any]) -> None:
    save_json_ww_style(LAYOUT_JSON, layout)


def save_grid(entries: List[Dict[str, Any]]) -> None:
    """Writes data/tech_tree_grid.json -- the real file
    game/client/src/menu/tech_tree_data.h loads at runtime. See
    legacy_export.compile_to_legacy_entries for what produces `entries`."""
    save_json_ww_style(GRID_JSON, entries)
