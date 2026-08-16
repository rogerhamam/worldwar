# World War

A WW2 RTS: a C++/SDL2 game engine, a standalone campaign editor, and a
Python launcher that keeps players updated.

## Layout

- `game/` — the main game (engine + client). Builds `worldwar_client.exe`.
- `editor/` — standalone SDL2 tool for authoring campaigns (missions,
  briefings, starting battlefield layout). Builds `campaign_editor.exe`.
  See its own `README.md` / `CLAUDE.md`.
- `assets/`, `data/` — the shared sprite/sound/music assets and JSON data
  (units, buildings, techs, civs, campaigns) that `game` loads at
  runtime.
- `launcher/` — the player-facing launcher (`launcher.py`, built with
  PyInstaller via `build_launcher.bat`). Downloads/updates the game from
  GitHub Releases and runs it.
- `play.bat` — runs a locally built dev binary directly.
- `publish.bat` — builds the game, assembles a release zip, and uploads
  it to GitHub Releases (see `RELEASING.md`).

## Building

Requires MSYS2 (ucrt64) with `cmake`, `ninja`, `gcc`, and the dev packages
`SDL2`, `SDL2_image`, `SDL2_mixer`, `SDL2_ttf`, `nlohmann_json` installed
(pacman or vcpkg), all on `PATH`.

```
build.bat
```

This configures and builds both `game` and `editor`. Resulting binaries:

- `game\build\client\worldwar_client.exe`
- `editor\build\campaign_editor.exe`

Or build one at a time the normal CMake way from inside either project
folder (`cmake -G Ninja -B build && cmake --build build`).

## Running

- Dev build: double-click `play.bat`, or run
  `game\build\client\worldwar_client.exe` directly.
- Players: `launcher\launcher.py` (or the built `WorldWarLauncher.exe`)
  downloads the latest published release and launches it — see
  `RELEASING.md` for how releases are cut.

## Campaign authoring workflow

Campaigns are authored in `editor` and saved as JSON. Finished campaign
files get copied into `data/campaigns/`, where the main game's campaign
browser picks them up. See `editor/README.md` for details, and note the
schema (`campaign/include/campaign/campaign_data.h`) is vendored
separately in each project and must be kept in sync by hand.
