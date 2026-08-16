# World War -- Campaign Editor

A standalone tool for authoring campaigns (mission list, players/teams,
briefing text, starting battlefield layout) for the World War RTS. This
project has **no dependency on the main game's engine repo** -- it's a
separate SDL2 program with its own small, vendored copy of just the data
it needs (unit/building rosters, civ restrictions, icon sprites). You can
build and develop this entirely on its own.

## Building

Requires MSYS2/MinGW (ucrt64) with SDL2, SDL2_image, SDL2_ttf, and
nlohmann_json installed via pacman/vcpkg, plus CMake + Ninja.

```
mkdir build && cd build
cmake -G Ninja ..
cmake --build .
./campaign_editor.exe
```

## What's in here

- `src/` -- the editor itself (`editor.h/.cpp`, `main.cpp`), plus:
  - `game_data.h/.cpp` -- unit/building rosters and civ restrictions
    (which units each civ can/can't build), vendored from the main
    engine's `sim/src/control.cpp`. Pure data, no simulation logic.
  - `catalog.h/.cpp` -- loads `data/catalog.json` (icon sprites) and
    `data/civ_exclude.json` (civ restrictions).
  - `menu/civ_data.h` -- civ names, flags, leader names.
- `render/` -- SDL2 sprite atlas / bitmap text / camera helpers.
- `campaign/` -- the campaign JSON data model (`Campaign`/`Level`/
  `LevelPlayer`/`TerrainFeature`/`PlacedEntity`) and load/save.
- `assets/` -- sprites + backgrounds only (no music/sound -- this editor
  never plays audio), trimmed from the main game's asset pack.
- `data/catalog.json`, `data/civ_exclude.json` -- trimmed from the main
  game's data files (just what the palette needs).
- `data/campaigns/*.json` -- your actual campaign files. **This is what
  you send back** once you've made progress (see below).

## Keeping the schema in sync

`campaign/include/campaign/campaign_data.h` defines the on-disk JSON
schema for campaigns. The main engine repo (`game`) has its own
copy of this exact same library (also called `ww_campaign`), which its
in-game "Campaign" menu screen uses to browse/preview what you've made.

**If you add a field to the schema here, the main engine's copy needs the
same change, or it won't be able to read your campaigns.** Since these
are two separate repos now, that has to happen by hand -- flag any
schema changes so the engine side gets updated to match.

## Sending campaigns back

Zip up `data/campaigns/` (or just the individual `.json` files you've
been working on) and send them over. They get dropped into the main
game's `data/campaigns/` folder, where both the engine's
in-game campaign browser and a future "play this campaign" flow will
pick them up.

## Current scope

This tool currently supports:
- Creating campaigns (name + civ) and levels within them (name,
  multi-line description, location on the Europe map).
- Up to 8 players per level; player 1 is always the campaign's own civ
  (locked); every other slot picks a civ and a team (1-4, matching the
  main game's alliance system).
- A 32x32 battlefield grid per level: painting terrain (grass/water/
  trees/berries/oil/iron/deer/fish, with an adjustable brush size) and
  placing starting units/buildings, owned by whichever team is selected.

Not yet supported (future work, flagged so nobody's surprised it's
missing): scripted triggers, objectives, win/lose conditions, or an
actual "play this level" flow in the main game -- campaigns are
authorable and browsable today, not yet playable end-to-end.
