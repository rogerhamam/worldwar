# Releasing World War

How to ship a build that `launcher/launcher.py` can find and install for
players.

## What a release is

A GitHub release on **`KAJKINGDOM/worldwar-releases`**, tagged e.g. `v2.1.0`,
with two assets:

- `worldwar-game.zip` — `worldwar_client.exe`, its runtime DLLs, `assets/`,
  `data/`, and `version.txt`.
- `manifest.json` — SHA-256 hashes of every file in the zip. The launcher
  compares this against what's already installed and only re-downloads
  what changed.

## Cutting a release

```
publish.bat v2.1.1
```

This single command:

1. Builds `worldwar_client.exe` from `game/`.
2. Assembles `dist/WorldWar/` — the exe, every DLL it actually links
   against (via `ldd`), plus `assets/` and `data/` copied from the repo
   root.
3. Generates `dist/manifest.json` (via `launcher/launcher.py --manifest`)
   and zips `dist/WorldWar/*` into `dist/worldwar-game.zip`.
4. Creates/updates the `v2.1.1` release on `KAJKINGDOM/worldwar-releases`
   (using `gh release`) and uploads both files, marked "latest".

Requires the [`gh` CLI](https://cli.github.com) logged in, and a bash on
`PATH` (Git Bash / MSYS2) for the DLL sweep.

Bump `game/VERSION` to match the tag first — it's compiled into the
in-game "Version X" title-screen label, so keep it in sync with what you
pass to `publish.bat`. `release_notes.txt` at the repo root is used as
the release notes (edit it before publishing).

## Testing before you publish

Extract `dist\worldwar-game.zip` into an empty folder with no dev
environment and double-click `worldwar_client.exe`. It must launch with
no extra installs — this catches a missing DLL or asset before players
hit it.

## How the launcher decides to update

`launcher.py` reads `manifest.json` from the latest release, hashes its
local install, and re-downloads only the files whose hash changed. See
`launcher/launcher.py`'s module docstring for the exact install layout
(`%LOCALAPPDATA%\WorldWar`).
