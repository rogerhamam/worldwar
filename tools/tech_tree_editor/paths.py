"""Repo path resolution for the tech tree editor.

Kept dependency-free (stdlib only) so any collaborator can run the tool
without installing anything beyond Python itself.
"""
from pathlib import Path


def find_repo_root() -> Path:
    here = Path(__file__).resolve()
    for candidate in [here] + list(here.parents):
        if (candidate / "data" / "civs.json").is_file() and (candidate / ".git").is_dir():
            return candidate
    raise RuntimeError(
        "Could not locate the worldwar repo root (looked for a parent "
        "directory containing both data/civs.json and .git)."
    )


REPO_ROOT = find_repo_root()
DATA_DIR = REPO_ROOT / "data"
# Where the game client's own copy of the loader lives -- purely
# documentation/reference now (see game/client/src/menu/tech_tree_data.h's
# load_tech_tree_grid); this tool reads/writes GRID_JSON below, not this.
TECH_TREE_DATA_H = REPO_ROOT / "game" / "client" / "src" / "menu" / "tech_tree_data.h"

CIVS_JSON = DATA_DIR / "civs.json"
CIV_EXCLUDE_JSON = DATA_DIR / "civ_exclude.json"
CATALOG_JSON = DATA_DIR / "catalog.json"
TECH_DESC_JSON = DATA_DIR / "tech_desc.json"
BUILDING_TECHS_JSON = DATA_DIR / "building_techs.json"
TECHS_JSON = DATA_DIR / "techs.json"

# The tech tree's grid layout (position + arrow-routing flags). Real,
# engine-consumed data as of the JSON migration -- game/client loads this
# same file at runtime (see TECH_TREE_DATA_H above). legacy_export.py
# compiles the tool's higher-level parent-graph design down into exactly
# this schema.
GRID_JSON = DATA_DIR / "tech_tree_grid.json"

# New files this tool owns -- never touches any existing data/*.json
# besides civ_exclude.json and tech_tree_grid.json (the two real,
# engine-consumed levers). Proposed unique-unit additions are staged here
# until someone wires them into the engine (see README.md).
PROPOSALS_JSON = DATA_DIR / "tech_tree_proposals.json"

# Designed grid-position overrides (base layout redesign + per-civ deltas
# on top of it) -- the editable form legacy_export.py compiles from. This
# stays the tool's own working format even though GRID_JSON is now real
# engine data, since GRID_JSON can't represent per-civ position deltas
# (the engine's grid is civ-agnostic) or the simpler single-parent model.
LAYOUT_JSON = DATA_DIR / "tech_tree_layout.json"
