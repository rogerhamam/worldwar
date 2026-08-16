# World War Campaign Editor — CLAUDE.md

This is a **standalone** project: a campaign/mission editor for a C++ port
of a GameMaker WW2 RTS ("World War"). It was deliberately split out of the
main engine repo (`game`, a sibling folder) so it can be developed
independently — by a different person, in a different Claude session —
without touching or depending on the engine at all. See `README.md` for
the build steps and the campaign-file hand-off workflow; this file is
about how to *work on the editor's own code*.

**Read `README.md` first if you haven't** — it covers what's vendored vs.
shared, and the schema-sync obligation with the main engine repo.

## Quick orientation

- `src/editor.h` / `src/editor.cpp` — the whole app. One `Editor` class,
  one `Screen` enum, hit-rects rebuilt every `draw()` and hit-tested on
  the next click (same pattern as the main engine's `MenuController`).
- `src/main.cpp` — SDL setup, the frame loop, and a permanent synthetic-
  input test harness (see "Testing" below).
- `src/game_data.h/.cpp` — vendored slice of the main engine's static
  data tables (unit/building rosters, civ restrictions, building
  footprint sizes, civ→base-sprite). Pure data, no sim code.
- `src/catalog.h/.cpp` — loads `data/catalog.json` + `data/civ_exclude.json`.
- `campaign/` — the `ww_campaign` library: `Campaign`/`Level`/
  `LevelPlayer`/`TerrainFeature`/`PlacedEntity` structs + JSON load/save.
  **This schema must stay in sync with `game`'s own copy of the
  same library** (it reads the same on-disk files) — if you change a
  field here, make the same change over there, or flag it for the other
  side to do.
- `render/` — vendored `ww_render` (SpriteAtlas/TextRenderer/Camera),
  copied from the main engine, not linked to it.

## Building

```
mkdir build && cd build
cmake -G Ninja ..
cmake --build .
./campaign_editor.exe
```

Needs MSYS2/mingw ucrt64 with SDL2, SDL2_image, SDL2_ttf, nlohmann_json
(same toolchain as the main engine). No SDL2_mixer — this program never
plays audio.

## Testing

There's no human-driven manual testing loop expected here — verify
changes with the **synthetic input harness** built into `main.cpp`, then
capture a screenshot and actually look at it. This project has no
automated test suite; screenshot-based visual verification is the
standard here.

Environment variables (all optional):
- `WW_TEST_CLICK` / `WW_TEST_CLICK2` .. `WW_TEST_CLICK7` — `"x,y"`, fired
  at frames 5, 10, 15, 20, 25, 30, 35 respectively.
- `WW_TEST_TEXT` — injects one `SDL_TEXTINPUT` at frame 20 (note: a real
  `SDL_TEXTINPUT` event's text buffer is small, ~32 bytes — long strings
  get truncated; this is an SDL/harness limit, not an app bug).
- `WW_TEST_MOVE` — `"x,y"`, injects one `SDL_MOUSEMOTION` at frame 45
  (for hover-only UI: the placement ghost, hover highlight — a click
  can't exercise these since it never fires real motion).
- `WW_TEST_WHEEL` — signed notch count, fired as that many individual
  wheel events at frame 40 (EditMap's zoom).
- `WW_TEST_DRAG` — `"x0,y0,x1,y1"`, a real press-hold-release drag: button
  down at (x0,y0) at frame 42, a motion to (x1,y1) at frame 43 while still
  held, button up there at frame 44. For press-and-hold interactions
  (Terrain's paint brush, Objectives' kill-area rectangle) that
  `WW_TEST_CLICK*` can't exercise, since that always pairs its down/up at
  the same point in the same instant.
- `WW_SHOT` + `WW_SHOT_FRAMES` (default 30) — dumps a BMP screenshot
  after N frames, then exits.

Convert the BMP to PNG to actually view it (the Read tool can't render
BMP): `python -c "from PIL import Image; Image.open('x.bmp').save('x.png')"`.

Click coordinates are in the **current window resolution's own pixel
space** (960x720 as of this writing, `kPanel`/`kMapCanvasRect`/etc. in
`editor.cpp`) — if you resize the window again, every coordinate you
compute by hand needs to account for that.

**Before creating test campaign JSON files**, check `data/campaigns/`
for what's already there — the user's brother uses this editor for real
campaign work between sessions. Only ever delete a file you created
yourself in the same session; if something unexpected is in there,
ask before touching it (this has come up before: a `barbossa.json` and
a `jjj.json` both turned out to be real work in progress, not test
debris).

## Current state (as of the last session)

Implemented: create campaign (name + civ) → create level (name, size
preset, description, Europe-map location, up to 8 players with civ +
team 1-4, P1 locked to the campaign's own civ) → place battlefield
content (Units/Buildings/Terrain tabs, civ-filtered palettes, real
in-game sprites via a `Camera`-driven canvas with zoom/pan, a minimap,
footprint-aware occupancy rules, brush painting for terrain).

**Not yet built** (don't assume these exist):
- Scripted triggers, objectives, or win/lose conditions.
- Any "play this level" flow, in this editor or the main engine.
- The "generate a random map onto the grid" option mentioned as a
  future idea (map types don't exist yet for it to pick from).
- The main engine's in-game campaign browser is browse-only (name,
  civ, level count, map dot, level info) — no "completed" tracking
  exists yet since nothing can be completed yet.

## Recently fixed (worth knowing before touching EditMap again)

- Building anchor math: a building's `(tx,ty)` is its **top-left**
  footprint tile; screen centre = `(tx*TILE + w/2, ty*TILE + h/2)` using
  `game_data.h`'s `building_wh()`. Do NOT go back to `(tx+0.5)*TILE` for
  buildings (that's correct only for single-tile units) — it was the
  cause of a "2x2 building renders as 3x3" bug.
- `TerrainFeature` has independent `base` ("" / "water") and `resource`
  ("" / tree / palm / berry / oil / iron / deer / fish) fields — NOT a
  single `kind` string. Fish requires existing `base=="water"` and
  doesn't clear it; land resources require `base!="water"`.
- `spr_fish` frame 0 is a blank animation frame — always draw fish at
  frame 1 (see `kTerrainKinds`' `frame` field), or it silently renders
  nothing.
- `Camera`'s `clamp()` never lets the viewport go past the world's exact
  `[0,world_w]x[0,world_h]` — without `kMapMargin` padding, a unit/
  building sitting on an edge tile gets permanently clipped with no way
  to scroll to see the rest of it, at any zoom. If you touch the camera/
  world-size math in `draw_edit_map`, keep the margin.
- Per-level `grid_size` (Tiny/Normal/Large/Huge, chosen at level creation)
  replaced the old fixed 32x32 constant — `current_grid_size()` /
  `lvl->grid_size` is the source of truth now, not `kLevelGridSize`
  (that constant is only the *default* for levels that predate the
  size picker).
