# Tech Tree Editor

A Tkinter GUI for designing World War's tech tree: which techs/units/
buildings each civilization can access, when they become available, where
new unique units go, and — as of the engine-wiring work below — the actual
grid layout and arrows the shipped game renders. Python stdlib only — no
`pip install` needed.

```
python tools/tech_tree_editor/app.py
```

## What it actually edits (live gameplay data)

Two files this tool writes are real, engine-consumed data, not a mockup:

- **`data/civ_exclude.json`** — per-civ id → list of tech/unit/building
  names that civ does **not** get. Everything not listed is available to
  every civ by default (the "generic tech tree" the whole roster is based
  on). Both the menu (`MenuController::civ_exclude_`) and the sim
  (`Control::civ_has`, `game/sim/src/control.cpp`) load this file. Toggling
  a node and hitting **Save civ_exclude.json** writes it in the exact
  on-disk format (1-space indent, CRLF, existing item order preserved —
  toggling only appends/removes that one entry instead of re-sorting the
  whole list, so diffs stay minimal).
- **`data/tech_tree_grid.json`** — every node's grid position and arrow-
  routing flags. `game/client/src/menu/tech_tree_data.h` loads this at
  runtime (`load_tech_tree_grid`); it used to be a hardcoded C++ vector
  before this tool existed (see "Engine wiring" below). **Export to
  Game** compiles the tool's design down to this file's schema.

**Save All** (Ctrl+S, or the main toolbar button) saves whichever of
`civ_exclude.json` / `tech_tree_layout.json` / `tech_tree_proposals.json`
actually have unsaved changes — this is what Ctrl+S does. The three
"Save ... only" actions under File are for when you deliberately want to
write just one of them. (Earlier versions bound Ctrl+S to civ_exclude.json
specifically, so pressing it while only the layout was dirty looked like
saving but silently left the layout unsaved — fixed.)

**Reload** re-reads everything from disk, discarding in-tool edits — use it
after pulling a collaborator's changes so you don't clobber their work with
a stale in-memory copy. As always, `git fetch`/`git pull` first if someone
else might have touched the same files.

## Designing the layout: era bands + dragging

The Grid tab is a **design surface**, not just a read-only mirror. It
starts from `data/tech_tree_grid.json`'s real grid positions (re-read on
every launch, so it can't silently drift out of sync with what the game
loads), but every real node can be dragged to a new position — not just
the Unique Unit Planner's proposals.

Four horizontal bands span the **full width** of the canvas, one per era
(Victorian/Industrial/War/Scientific — colors from `era_rules.py`; these
are the designer's names for era ids 0-3, not `civs.json`'s/`catalog.json`'s
own "Industrial"/"Interwar" labels for the same ids — see the comment atop
`era_rules.py` if the two ever need reconciling). The band a node's box
sits in **is its designed era**: drag a node up or down across a band
boundary to change when it becomes available. This is deliberately
different from the *shipped game's* era gate (`control.cpp`'s `TECH_ERA` /
`UNIT_ERA` / `BUILDING_ERA`, also vendored in `era_rules.py`), which is
shown as a thin stripe across the top of each node — row and era don't
correlate at all in the current game (row 0 alone holds both Victorian-
and War-era items, since a grid column is one upgrade chain, not one era
lane). When a node's stripe color doesn't match the band it's sitting in,
that's the tool telling you: this node hasn't been redesigned into its
target era yet (the era gate itself is still hardcoded C++ — moving a node
across a band only changes the *design*; someone still has to update
`TECH_ERA`/`UNIT_ERA`/`BUILDING_ERA` by hand to match). The band boundaries
default to 2 rows/era (`era_rules.ROWS_PER_ERA`) — a starting point, not a
fixed law; widen it if the roster needs more room once the redesign
actually fills in.

**Click** (no movement) toggles enable/disable, same as before. **Drag**
repositions:

- Dragging while **Base** is selected in the sidebar redesigns the shared
  layout every civ inherits.
- Dragging while a **civilization** is selected stores that move as an
  override *for that civ only*, relative to base — i.e. it's only recorded
  at all when it actually differs from base, so a civ that hasn't touched a
  node keeps inheriting whatever Base does with it later. Nodes with an
  active override (base or civ-level) get a dashed cyan outline; right-click
  one for "Reset to base position" / "Reset to original tech_tree_data.h
  layout".

Every node also has a single **parent** — the node its arrow comes from
(right-click → *Set parent...*). This is deliberately simpler than the
legacy col/row-adjacency system it replaces: pick the one prerequisite
node, and the tool (and, via **Export to Game**, the actual renderer)
figures out the routing. A red "?" marks a non-building node with no
parent assigned yet — the auto-migration that seeded these from the
original layout is a best-effort starting point (see `infer_default_parents`
in `grid_layout.py`), not a guarantee everything's connected; these are
exactly the gaps worth reviewing by hand. Arrows redraw from **current**
positions every frame (siblings that share a parent draw as one trunk that
splits, not independent lines), so dragging nodes into a cleaner
arrangement or reassigning a parent shows the result immediately.

**Insert Column...** / **Delete Column...** (Base only) are interactive
modes, not dialogs: click one, then hover the grid. Insert previews a
vertical line at the nearest column boundary; Delete highlights the whole
column under the cursor — green if it's genuinely empty, red (with a list
of what's in the way) if it isn't. Click to commit, or Esc / right-click /
switch civ / switch tab / click any other toolbar action to cancel without
changing anything; clicking an occupied column while deleting just refuses
and stays in the mode rather than losing anything. Both shift every
affected base node, every civ's own position overrides, and every planner
proposal together (insert: everything at/after the column moves right;
delete: everything after it moves left), so nothing already placed ends up
misaligned relative to what moved. The two modes are mutually exclusive —
starting one cancels the other.

**Save Layout** writes the working design to `data/tech_tree_layout.json`
(`{"base": {name: {"col","row","parent"}}, "civs": {"<civ id>": {...}}}`,
sparse — only fields that differ from the layer below are stored). This is
the tool's own editable format; it stays that way even though the *engine*
now reads real grid data too, because it can represent things the engine's
flat format can't (per-civ position deltas, the simpler single-parent
model) — **Export to Game** is the one-way compiler from this format down
to what the engine needs.

## Engine wiring: Export to Game

**File → Export to game... / the Grid tab's "Export to Game..." button**
compiles the *Base* design (`legacy_export.compile_to_legacy_entries`) into
the legacy col/row/parent/child/older_sibling/force + synthesized
`"through"`-filler format `tech_tree_data.h` expects, and writes
`data/tech_tree_grid.json`. The C++ renderer (`draw_tech_tree` in
`menu_controller.cpp`) was deliberately left **unchanged** — it already
looks right (real 64×64 sprite tiles with no gap between cells, unlike this
tool's boxes-with-gaps, so a plain adjacency hop-chain already reads as a
continuous line) — only its data source moved from a hardcoded vector to
JSON. Verified by an actual build + a synthetic-input run of
`worldwar_client.exe` through to the real Tech Tree screen; see git history
for the before/after screenshots if you want the receipts.

A subtlety worth knowing if you touch `legacy_export.py`: despite its name
and its doc comment, `TechTreeEntry::parent` in **actual** use belongs to
the cell *above* a given entry, not the entry itself — the vertical-arrow
check in `draw_tech_tree` reads `above->parent`, meaning "this cell
projects an arrow down into whatever's below it," not "I accept one from
above." `infer_default_parents` and the exporter both follow this real
behavior, not the (misleading) comment that was already in the header
before any of this.

Two things the exporter can't carry over, both surfaced as a confirmation
dialog before it writes anything:
- **Per-civ layout overrides** — the engine's grid is the same for every
  civ, so only Base gets exported; a civ's position/parent tweaks stay
  tool-only (that civ's tech/unit/building *access* from `civ_exclude.json`
  is unaffected and still applies).
- **Routing collisions** — if a redesign needs an L-shaped path between two
  nodes and both possible corner cells are already occupied by unrelated
  content, that one edge is skipped with a warning instead of guessed at.

## Unique units — ownership is still hardcoded, separately

`tech_tree_data.h`'s `resolve_civ_unit()` / `is_unique_unit()` and
`control.cpp`'s `CIV_ONLY_UNITS` / `OTTOMAN_ONLY` / `CAMEL_CIVS` /
`CIV_UNIT_SUB` decide which civ(s) can build e.g. the B29, Tiger tank, or
Janissary, and the display-name swaps (Janissary ↔ Waffen SS) that go with
them — none of that moved to JSON yet. `unique_unit_rules.py` is a
hand-vendored, read-only snapshot of that logic (same convention
`editor/src/game_data.h` already uses for the campaign editor's copy of
engine data) — it powers the grid's gold-outlined nodes, civ-specific
hide/rename behavior, and the reference table at the top of the **Unique
Unit Planner** tab.

The bottom half of that tab lets you sketch out *new* unique units — a
template unit/tech to base stats on, a display title, a building, a grid
position (draggable on the Grid tab, or typed directly), and which civ(s)
own it. **Save Proposals** writes those to `data/tech_tree_proposals.json`,
which nothing currently reads — a structured handoff for whoever
implements the C++ side, not a live effect.

## Files

- `app.py` — the GUI (entry point).
- `data_io.py` — loads/saves `data/*.json`, matching the existing on-disk
  formatting exactly.
- `paths.py` — locates the repo root; no hardcoded absolute paths.
- `grid_layout.py` — reads `data/tech_tree_grid.json` (re-read on every
  launch) and `infer_default_parents`, the one-time migration from the
  legacy flag/position system to the tool's single-parent model.
- `legacy_export.py` — the compiler behind Export to Game: parent-graph
  design → legacy col/row/flag entries + synthesized `"through"` fillers.
- `unique_unit_rules.py` — hand-vendored snapshot of the hardcoded
  unique-unit rules (see above). Update by hand if the C++ changes.
- `era_rules.py` — hand-vendored snapshot of `control.cpp`'s `TECH_ERA` /
  `UNIT_ERA` / `BUILDING_ERA` tables (today's shipped-game era gate, shown
  as each node's top stripe), plus the `ROWS_PER_ERA` band layout that
  defines the design-target era bands on the Grid tab.

## Known limitations

- No sprite rendering — tech tree nodes are drawn as colored/labeled boxes,
  not the actual game icons (the real game still uses its own pixel-art
  sprites; only the *data* is shared).
- Arrow rendering on the Grid tab is a Manhattan trunk-and-split line
  drawing, sized for this tool's own (larger, gapped) boxes — it's a design
  aid, not a pixel-accurate preview of the in-game sprite-tile renderer.
- Overlapping nodes (two things dragged onto the same cell) aren't
  prevented in the Grid tab — it's left as a visible signal that something
  needs to move. `legacy_export.py` *does* detect real collisions on
  export and warns instead of silently producing bad data.
- Export only covers grid position + arrows. Unique-unit ownership and the
  shipped-game era gate are still hardcoded C++ (see above) — the tool
  shows you where they're out of sync with the design (gold outlines, era
  stripe vs. band mismatch) but doesn't write them.
