# World War

A WW2 RTS: a C++/SDL2 game engine, a standalone campaign editor, and a
Python launcher that keeps players updated.

## Play it

Windows only. Nothing to install and nothing to build — the game binary and
its DLLs ship in this repo, next to the `assets/` and `data/` they load.

```
git clone https://github.com/rogerhamam/worldwar.git
cd worldwar
play.bat
```

(Or just double-click `play.bat` / `worldwar_client.exe` in Explorer.)

The clone is ~180 MB, most of it music and sprites. If `git clone` is slow,
`git clone --depth 1` grabs the same playable tree without the history.

Fonts come from `C:\Windows\Fonts`, so there's nothing else to fetch. The game
writes `worldwar_perf.log` next to the exe — if it ever stalls or crashes, that
file names the last frame before it went wrong.

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
- `worldwar_client.exe` + the `SDL2*.dll` / `lib*.dll` beside it — the
  prebuilt, ready-to-run game (see **Play it** above). The exe looks for
  `assets/` and `data/` next to itself, which is why they all sit at the
  repo root together.
- `play.bat` — runs the prebuilt exe, or a locally built dev binary if
  you've built from source.
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

- Just playing: `play.bat` (see **Play it** above) — no build required.
- Dev build: once you've run `build.bat`, `play.bat` still works; or run
  `game\build\client\worldwar_client.exe` directly. Note a dev binary run
  from its build folder falls back to the compile-time asset paths, so it
  reads this repo's `assets/`/`data/` rather than copies beside the exe.
- Players: `launcher\launcher.py` (or the built `WorldWarLauncher.exe`)
  downloads the latest published release and launches it — see
  `RELEASING.md` for how releases are cut.

## Campaign authoring workflow

Campaigns are authored in `editor` and saved as JSON. Finished campaign
files get copied into `data/campaigns/`, where the main game's campaign
browser picks them up. See `editor/README.md` for details, and note the
schema (`campaign/include/campaign/campaign_data.h`) is vendored
separately in each project and must be kept in sync by hand.
