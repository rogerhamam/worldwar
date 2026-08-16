"""
World War Launcher - downloads/updates the game files and launches the game.

All game data lives in %LOCALAPPDATA%\\WorldWar:
  game/         - worldwar_client.exe, DLLs, assets, data
  version.json  - local manifest (SHA-256 of installed files)

The launcher fetches manifest.json from GitHub Releases, compares SHA-256
hashes against local files, and downloads a fresh zip if anything changed.
"""

import sys
import os
import json
import hashlib
import subprocess
import threading
import tkinter as tk
from tkinter import ttk, messagebox
from urllib.request import urlopen, Request

# -- Configuration --------------------------------------------------------
GITHUB_REPO    = "KAJKINGDOM/worldwar-releases"
RELEASE_API    = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"

APP_DIR        = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")),
                              "WorldWar")
GAME_DIR       = os.path.join(APP_DIR, "game")
VERSION_FILE   = os.path.join(APP_DIR, "version.json")
GAME_EXE_NAME  = "worldwar_client.exe"
GAME_ZIP_NAME  = "worldwar-game.zip"

APP_TITLE      = "World War"

# Base path for bundled assets (PyInstaller or script dir)
if getattr(sys, "frozen", False):
    BUNDLE_DIR = sys._MEIPASS
else:
    BUNDLE_DIR = os.path.dirname(os.path.abspath(__file__))

# Window + palette. bg_menu.png (the game's own main-menu background, the
# Iwo Jima flag-raising photo) is native 640x480, so the window matches it
# exactly with no scaling/clipping needed.
WIN_W, WIN_H   = 640, 480
BG_FALLBACK    = "#1c1c18"   # shown if the bg image fails to load
INK_PRIMARY    = "#f2e9d8"   # title/version/notes text -- light, for contrast on a dark photo
INK_SECONDARY  = "#d8c9a3"   # status line
INK_MUTED      = "#b0a888"   # detail/progress subtext
INK_ERROR      = "#ff6b57"
ACCENT         = "#b8860b"   # brass/medal gold - button, progress, scrollbar
ACCENT_ACTIVE  = "#d4a017"
CARD_BG        = "#141410"
CARD_BORDER    = "#b8860b"


# -- Helpers --------------------------------------------------------------
def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def download_file(url, dest, progress_cb=None):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".part"
    try:
        req = Request(url, headers={"User-Agent": "WorldWarLauncher/1.0"})
        with urlopen(req) as resp:
            total = int(resp.headers.get("Content-Length", 0))
            downloaded = 0
            with open(tmp, "wb") as f:
                while True:
                    chunk = resp.read(1 << 16)
                    if not chunk:
                        break
                    f.write(chunk)
                    downloaded += len(chunk)
                    if progress_cb:
                        progress_cb(downloaded, total)
        os.replace(tmp, dest)
    except Exception:
        if os.path.exists(tmp):
            os.remove(tmp)
        raise


# -- Launcher GUI ---------------------------------------------------------
class LauncherApp:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title(APP_TITLE)
        self.root.geometry(f"{WIN_W}x{WIN_H}")
        self.root.resizable(False, False)
        self.release_info = None
        self.asset_urls = {}

        try:
            icon_path = os.path.join(BUNDLE_DIR, "icon.ico")
            if os.path.exists(icon_path):
                self.root.iconbitmap(icon_path)
        except Exception:
            pass

        self.root.configure(bg=BG_FALLBACK)

        # Canvas hosts the bg_menu background plus all overlay widgets.
        # Using a canvas (instead of pack on root) lets text sit directly
        # on top of the image with no widget chrome behind it.
        self.canvas = tk.Canvas(self.root, width=WIN_W, height=WIN_H,
                                 highlightthickness=0, bg=BG_FALLBACK)
        self.canvas.pack(fill="both", expand=True)

        try:
            bg_path = os.path.join(BUNDLE_DIR, "bg_menu.png")
            if os.path.exists(bg_path):
                self._bg_img = tk.PhotoImage(file=bg_path)
                self.canvas.create_image(0, 0, image=self._bg_img,
                                          anchor="nw")
                # Darken the photo so light overlay text stays readable
                # everywhere, not just over its already-dark regions.
                self.canvas.create_rectangle(0, 0, WIN_W, WIN_H,
                                              fill="black", stipple="gray50",
                                              outline="")
        except Exception:
            pass

        # Title overlay.
        self.canvas.create_text(WIN_W // 2, 34,
                                 text="World War",
                                 font=("Consolas", 24, "bold"),
                                 fill=INK_PRIMARY)

        # Status text item; updated via canvas.itemconfigure.
        self.status_id = self.canvas.create_text(
            WIN_W // 2, 72,
            text="Checking for updates...",
            font=("Consolas", 10), fill=INK_SECONDARY)

        style = ttk.Style()
        style.theme_use("default")
        style.configure("Custom.Horizontal.TProgressbar",
                        troughcolor=CARD_BG, background=ACCENT,
                        bordercolor=CARD_BORDER, thickness=20)
        style.configure("Notes.Vertical.TScrollbar",
                        troughcolor=CARD_BG,
                        background=ACCENT,
                        darkcolor=ACCENT,
                        lightcolor=ACCENT,
                        bordercolor=CARD_BORDER,
                        arrowcolor=INK_PRIMARY,
                        gripcount=0,
                        width=16)
        style.map("Notes.Vertical.TScrollbar",
                  background=[("active", ACCENT_ACTIVE)])

        self.progress = ttk.Progressbar(self.canvas, length=520,
                                         mode="determinate",
                                         style="Custom.Horizontal.TProgressbar")
        self.progress_id = self.canvas.create_window(
            WIN_W // 2, 102, window=self.progress)

        self.detail_id = self.canvas.create_text(
            WIN_W // 2, 128, text="",
            font=("Consolas", 8), fill=INK_MUTED)

        # Notes "card" - a dark panel with a brass border that sits over
        # the lower half of the background photo. Hidden until the update
        # finishes.
        self.notes_card = tk.Frame(self.canvas, bg=CARD_BG,
                                    highlightthickness=2,
                                    highlightbackground=CARD_BORDER)

        self.version_var = tk.StringVar(value="")
        self.version_label = tk.Label(self.notes_card,
                                       textvariable=self.version_var,
                                       font=("Consolas", 11, "bold"),
                                       fg=INK_PRIMARY, bg=CARD_BG)
        self.version_label.pack(pady=(8, 4))

        notes_inner = tk.Frame(self.notes_card, bg=CARD_BG)
        notes_inner.pack(padx=12, pady=(0, 4), fill="both", expand=True)

        self.notes_scroll = ttk.Scrollbar(notes_inner, orient="vertical",
                                           style="Notes.Vertical.TScrollbar")
        self.notes_text = tk.Text(notes_inner, font=("Consolas", 9),
                                   fg=INK_PRIMARY, bg=CARD_BG,
                                   relief="flat", wrap="word",
                                   height=7, width=62,
                                   highlightthickness=0,
                                   yscrollcommand=self._on_notes_scroll)
        self.notes_scroll.config(command=self.notes_text.yview)
        self.notes_text.pack(side="left", fill="both", expand=True)
        self.notes_scroll.pack(side="right", fill="y")
        self.notes_text.config(state="disabled")
        self.notes_text.bind("<MouseWheel>",
            lambda e: self.notes_text.yview_scroll(int(-e.delta / 120),
                                                    "units"))

        self.scroll_hint_var = tk.StringVar(value="")
        self.scroll_hint = tk.Label(self.notes_card,
                                     textvariable=self.scroll_hint_var,
                                     font=("Consolas", 9, "bold"),
                                     fg=INK_PRIMARY, bg=CARD_BG)
        self.scroll_hint.pack(pady=(0, 4))

        self.play_btn = tk.Button(self.notes_card, text="PLAY",
                                   font=("Consolas", 14, "bold"),
                                   fg="#141410", bg=ACCENT,
                                   activebackground=ACCENT_ACTIVE,
                                   activeforeground="#141410",
                                   relief="flat", cursor="hand2",
                                   width=20, height=1,
                                   command=self.on_play)
        self.play_btn.pack(pady=(2, 10))

        # Card lives at the lower-middle of the window, centered on the canvas.
        self.notes_card_id = self.canvas.create_window(
            WIN_W // 2, 310, window=self.notes_card, state="hidden")

        self.thread = threading.Thread(target=self.run_update, daemon=True)
        self.thread.start()

        self.root.mainloop()

    def set_status(self, text):
        self.root.after(0, lambda: self.canvas.itemconfigure(
            self.status_id, text=text))

    def set_detail(self, text, color=None):
        def _update():
            self.canvas.itemconfigure(self.detail_id,
                                       text=text, fill=color or INK_MUTED)
        self.root.after(0, _update)

    def set_progress(self, value, maximum=100):
        def _update():
            self.progress["maximum"] = maximum
            self.progress["value"] = value
        self.root.after(0, _update)

    def _on_notes_scroll(self, first, last):
        """Scrollbar callback - also updates the 'scroll for more' hint."""
        self.notes_scroll.set(first, last)
        first_f, last_f = float(first), float(last)
        if last_f >= 0.9999 and first_f <= 0.0001:
            self.scroll_hint_var.set("")                # content fits
        elif last_f < 0.9999:
            self.scroll_hint_var.set("▼ scroll for more")
        else:
            self.scroll_hint_var.set("▲ scroll for more")

    def show_ready_screen(self):
        def _show():
            self.canvas.itemconfigure(self.progress_id, state="hidden")
            tag = "Unknown"
            notes = ""
            if self.release_info:
                tag = self.release_info.get("tag_name", tag)
                notes = self.release_info.get("body", "") or "No release notes."
            self.version_var.set(f"Version: {tag}")
            self.notes_text.config(state="normal")
            self.notes_text.delete("1.0", "end")
            self.notes_text.insert("1.0", notes)
            self.notes_text.config(state="disabled")
            self.canvas.itemconfigure(self.notes_card_id, state="normal")
        self.root.after(0, _show)

    def on_play(self):
        try:
            self.launch_game()
        except Exception as e:
            messagebox.showerror(APP_TITLE, f"Failed to launch:\n\n{e}")

    def run_update(self):
        try:
            self.fetch_release_info()
            self.update_game()
            self.set_status("Ready!")
            self.set_detail("")
            self.show_ready_screen()
        except Exception as e:
            if os.path.isfile(os.path.join(GAME_DIR, GAME_EXE_NAME)):
                self.set_status("Update failed - play cached version")
                self.set_detail(str(e)[:80], INK_ERROR)
                self.show_ready_screen()
            else:
                self.set_status(f"Error: {e}")
                self.root.after(0, lambda: messagebox.showerror(
                    APP_TITLE,
                    f"Update failed:\n\n{e}\n\nCheck your internet connection."))

    def fetch_release_info(self):
        try:
            req = Request(RELEASE_API,
                          headers={"User-Agent": "WorldWarLauncher/1.0"})
            with urlopen(req, timeout=15) as resp:
                self.release_info = json.loads(resp.read().decode())
            self.asset_urls = {}
            for asset in self.release_info.get("assets", []):
                self.asset_urls[asset["name"]] = asset["browser_download_url"]
        except Exception:
            self.release_info = None
            self.asset_urls = {}

    def update_game(self):
        self.set_status("Checking for updates...")
        self.set_progress(0)
        self.set_detail("")

        if not self.asset_urls:
            if os.path.isfile(os.path.join(GAME_DIR, GAME_EXE_NAME)):
                self.set_status("Offline - using cached version")
                return
            raise Exception("No internet connection and game is not installed.")

        manifest_url = self.asset_urls.get("manifest.json")
        if not manifest_url:
            raise Exception("No manifest.json found in release.")

        try:
            req = Request(manifest_url,
                          headers={"User-Agent": "WorldWarLauncher/1.0"})
            with urlopen(req, timeout=30) as resp:
                manifest = json.loads(resp.read().decode())
        except Exception:
            if os.path.isfile(os.path.join(GAME_DIR, GAME_EXE_NAME)):
                self.set_status("Offline - using cached version")
                return
            raise Exception("Failed to download manifest.")

        local_hashes = {}
        if os.path.isfile(VERSION_FILE):
            with open(VERSION_FILE, "r") as f:
                local_hashes = json.load(f)

        needs_update = False
        for rel_path, remote_hash in manifest["files"].items():
            local_path = os.path.join(GAME_DIR, rel_path)
            if (local_hashes.get(rel_path) != remote_hash
                    or not os.path.isfile(local_path)):
                needs_update = True
                break

        if not needs_update:
            # Even when nothing needs downloading, sweep stale assets so
            # files removed in earlier releases (e.g. deleted maps/sounds)
            # don't linger from a pre-cleanup install.
            self._prune_orphan_assets(manifest["files"])
            self.set_status("Game is up to date!")
            self.set_progress(100)
            return

        zip_url = self.asset_urls.get(GAME_ZIP_NAME)
        if not zip_url:
            raise Exception(f"No {GAME_ZIP_NAME} found in release.")

        self.set_status("Downloading update...")
        os.makedirs(APP_DIR, exist_ok=True)
        zip_path = os.path.join(APP_DIR, GAME_ZIP_NAME)
        download_file(zip_url, zip_path,
                       lambda done, total: self.set_progress(done, total or 1))

        self.set_status("Installing update...")
        self.set_detail("Extracting files...")
        import zipfile
        os.makedirs(GAME_DIR, exist_ok=True)
        with zipfile.ZipFile(zip_path, "r") as zf:
            zf.extractall(GAME_DIR)
        os.remove(zip_path)

        # zip extraction overwrites and adds but never deletes - prune
        # any asset that's not in the new manifest so removed files
        # actually disappear on the player's machine.
        self._prune_orphan_assets(manifest["files"])

        with open(VERSION_FILE, "w") as f:
            json.dump(manifest["files"], f, indent=2)

        self.set_progress(100)
        self.set_status("Update complete!")
        self.set_detail("")

    def _prune_orphan_assets(self, manifest_files):
        """Delete files under GAME_DIR/assets and GAME_DIR/data that aren't
        in the manifest.

        Sweep is scoped to the assets/data trees so user-owned siblings
        (e.g. any local settings file) are never touched. Empty
        directories left behind by the prune are removed too so the
        layout matches a fresh extract.
        """
        keep = set(manifest_files.keys())
        for tree in ("assets", "data"):
            root_dir = os.path.join(GAME_DIR, tree)
            if not os.path.isdir(root_dir):
                continue
            for root, _, filenames in os.walk(root_dir):
                for fname in filenames:
                    full = os.path.join(root, fname)
                    rel = os.path.relpath(full, GAME_DIR).replace("\\", "/")
                    if rel in keep:
                        continue
                    try:
                        os.remove(full)
                    except OSError:
                        pass
            for root, dirs, filenames in os.walk(root_dir, topdown=False):
                if root == root_dir:
                    continue
                if not dirs and not filenames:
                    try:
                        os.rmdir(root)
                    except OSError:
                        pass

    def launch_game(self):
        game_exe = os.path.join(GAME_DIR, GAME_EXE_NAME)
        if not os.path.isfile(game_exe):
            raise Exception(f"{GAME_EXE_NAME} not found after update.")
        subprocess.Popen([game_exe], cwd=GAME_DIR)
        self.root.after(300, self.root.destroy)


# -- Build manifest tool --------------------------------------------------
def build_manifest(dist_dir, output_path):
    files = {}
    for root, _, filenames in os.walk(dist_dir):
        for fname in filenames:
            full = os.path.join(root, fname)
            rel = os.path.relpath(full, dist_dir).replace("\\", "/")
            files[rel] = sha256_file(full)
            print(f"  {rel}")
    manifest = {"files": files}
    with open(output_path, "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"\nManifest written to {output_path} ({len(files)} files)")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--manifest":
        dist = sys.argv[2] if len(sys.argv) > 2 else "dist/WorldWar"
        out  = sys.argv[3] if len(sys.argv) > 3 else "dist/manifest.json"
        build_manifest(dist, out)
    else:
        LauncherApp()
