"""World War -- Tech Tree Editor

A standalone Tkinter tool (stdlib only, no pip installs) for controlling
which techs/units/buildings each civilization can access, and for staging
proposed new unique units.

Run with:  python tools/tech_tree_editor/app.py
See tools/tech_tree_editor/README.md for the data model and limitations.
"""
import sys
import tkinter as tk
from tkinter import messagebox, ttk
from typing import Dict, List, Optional

import data_io
import era_rules
import unique_unit_rules
from grid_layout import infer_default_parents, parse_grid
from legacy_export import compile_to_legacy_entries
from paths import GRID_JSON, LAYOUT_JSON, PROPOSALS_JSON

ARROW_COLOR = "#d8dadf"
ARROW_WIDTH = 4

CELL_W = 78
CELL_H = 84
BOX_W = 68
BOX_H = 46
MARGIN = 40

CATEGORY_COLOR = {
    "building": "#6b7280",
    "tech": "#2f9e6a",
    "unit": "#3b6fd4",
    "unknown": "#999999",
}
UNIQUE_OUTLINE = "#d4a72f"
DISABLED_STIPPLE = "gray50"
BASE_CIV_LABEL = "— Base / Generic (reference) —"


def classify(key: str, catalog: dict) -> str:
    if key in catalog.get("buildings", {}):
        return "building"
    if key in catalog.get("techs", {}):
        return "tech"
    if key in catalog.get("units", {}):
        return "unit"
    return "unknown"


def display_title(key: str, tech_desc: dict) -> str:
    entry = tech_desc.get(key)
    if entry and entry.get("title"):
        return entry["title"]
    return key.title()


def blend(hex_color: str, bg: str = "#1e1e22", ratio: float = 0.16) -> str:
    """Blends hex_color toward bg -- canvas fills have no real alpha, so
    this fakes a translucent tint for the era background bands."""
    c1 = tuple(int(hex_color[i:i + 2], 16) for i in (1, 3, 5))
    c2 = tuple(int(bg[i:i + 2], 16) for i in (1, 3, 5))
    mixed = tuple(int(c1[i] * ratio + c2[i] * (1 - ratio)) for i in range(3))
    return "#%02x%02x%02x" % mixed


class ProposalDialog(tk.Toplevel):
    """Add/edit form for a proposed unique unit."""

    def __init__(self, parent, catalog: dict, proposal: Optional[dict] = None,
                 default_col: int = 0, default_row: int = 0):
        super().__init__(parent)
        self.title("Unique Unit Proposal")
        self.resizable(False, False)
        self.result: Optional[dict] = None
        self.transient(parent)
        self.grab_set()

        proposal = proposal or {}
        template_keys = sorted(
            list(catalog.get("units", {}).keys()) + list(catalog.get("techs", {}).keys())
        )

        pad = {"padx": 8, "pady": 4}
        row = 0
        ttk.Label(self, text="Internal id (snake_or_space, unique):").grid(row=row, column=0, sticky="w", **pad)
        self.id_var = tk.StringVar(value=proposal.get("id", ""))
        ttk.Entry(self, textvariable=self.id_var, width=30).grid(row=row, column=1, **pad)
        row += 1

        ttk.Label(self, text="Display title:").grid(row=row, column=0, sticky="w", **pad)
        self.title_var = tk.StringVar(value=proposal.get("display", ""))
        ttk.Entry(self, textvariable=self.title_var, width=30).grid(row=row, column=1, **pad)
        row += 1

        ttk.Label(self, text="Template (base stats to copy from):").grid(row=row, column=0, sticky="w", **pad)
        self.template_var = tk.StringVar(value=proposal.get("template", ""))
        ttk.Combobox(self, textvariable=self.template_var, values=template_keys, width=28,
                     state="readonly").grid(row=row, column=1, **pad)
        row += 1

        ttk.Label(self, text="Building / category:").grid(row=row, column=0, sticky="w", **pad)
        self.building_var = tk.StringVar(value=proposal.get("building", ""))
        ttk.Combobox(self, textvariable=self.building_var,
                     values=sorted(catalog.get("buildings", {}).keys()), width=28).grid(row=row, column=1, **pad)
        row += 1

        ttk.Label(self, text="Grid position (col, row):").grid(row=row, column=0, sticky="w", **pad)
        pos_frame = ttk.Frame(self)
        pos_frame.grid(row=row, column=1, sticky="w", **pad)
        self.col_var = tk.IntVar(value=proposal.get("col", default_col))
        self.row_var = tk.IntVar(value=proposal.get("row", default_row))
        ttk.Spinbox(pos_frame, from_=0, to=200, textvariable=self.col_var, width=6).pack(side="left")
        ttk.Spinbox(pos_frame, from_=0, to=20, textvariable=self.row_var, width=6).pack(side="left", padx=(6, 0))
        row += 1

        ttk.Label(self, text="Owning civ(s):").grid(row=row, column=0, sticky="nw", **pad)
        civ_frame = ttk.Frame(self)
        civ_frame.grid(row=row, column=1, sticky="w", **pad)
        owned = set(proposal.get("civs", []))
        self.civ_vars: Dict[int, tk.BooleanVar] = {}
        for civ_id, name in unique_unit_rules.CIV_DISPLAY_NAME.items():
            var = tk.BooleanVar(value=civ_id in owned)
            self.civ_vars[civ_id] = var
            ttk.Checkbutton(civ_frame, text=f"{civ_id} {name}", variable=var).grid(
                row=civ_id // 3, column=civ_id % 3, sticky="w", padx=4)
        row += 1

        ttk.Label(self, text="Notes:").grid(row=row, column=0, sticky="nw", **pad)
        self.notes_text = tk.Text(self, width=40, height=4)
        self.notes_text.insert("1.0", proposal.get("notes", ""))
        self.notes_text.grid(row=row, column=1, **pad)
        row += 1

        btns = ttk.Frame(self)
        btns.grid(row=row, column=0, columnspan=2, pady=(8, 10))
        ttk.Button(btns, text="Cancel", command=self.destroy).pack(side="left", padx=6)
        ttk.Button(btns, text="Save", command=self._on_save).pack(side="left", padx=6)

    def _on_save(self):
        pid = self.id_var.get().strip()
        if not pid:
            messagebox.showerror("Missing id", "Internal id is required.", parent=self)
            return
        self.result = {
            "id": pid,
            "display": self.title_var.get().strip() or pid.title(),
            "template": self.template_var.get().strip(),
            "building": self.building_var.get().strip(),
            "col": int(self.col_var.get()),
            "row": int(self.row_var.get()),
            "civs": sorted(c for c, v in self.civ_vars.items() if v.get()),
            "notes": self.notes_text.get("1.0", "end").strip(),
        }
        self.destroy()


_UNSET = object()  # ParentDialog.result sentinel: dialog was cancelled, as distinct from "chose None/root"


class ParentDialog(tk.Toplevel):
    """Pick (or clear) the single node this node's arrow comes from."""

    NONE_LABEL = "(none -- root, no incoming arrow)"

    def __init__(self, parent, node_title: str, options, current_parent: Optional[str]):
        super().__init__(parent)
        self.title(f"Set Parent — {node_title}")
        self.resizable(False, False)
        self.result = _UNSET
        self.transient(parent)
        self.grab_set()

        self._by_label = {self.NONE_LABEL: None}
        labels = [self.NONE_LABEL]
        current_label = self.NONE_LABEL
        for raw_name, label in options:
            display = f"{label}  [{raw_name}]"
            self._by_label[display] = raw_name
            labels.append(display)
            if raw_name == current_parent:
                current_label = display

        ttk.Label(self, text=f"This node's arrow comes from:").pack(anchor="w", padx=10, pady=(10, 2))
        self.var = tk.StringVar(value=current_label)
        combo = ttk.Combobox(self, textvariable=self.var, values=labels, width=50, state="normal")
        combo.pack(padx=10, pady=(0, 10))
        combo.focus_set()

        btns = ttk.Frame(self)
        btns.pack(pady=(0, 10))
        ttk.Button(btns, text="Cancel", command=self.destroy).pack(side="left", padx=6)
        ttk.Button(btns, text="OK", command=self._on_ok).pack(side="left", padx=6)

    def _on_ok(self):
        label = self.var.get()
        if label not in self._by_label:
            messagebox.showerror("Unknown node", "Pick an option from the list.", parent=self)
            return
        self.result = self._by_label[label]
        self.destroy()


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("World War — Tech Tree Editor")
        self.geometry("1280x780")

        self.civs = data_io.load_civs()
        self.civ_exclude = data_io.load_civ_exclude()
        self.catalog = data_io.load_catalog()
        self.tech_desc = data_io.load_tech_desc()
        self.building_techs = data_io.load_building_techs()
        self.proposals = data_io.load_proposals()
        self.layout = data_io.load_layout()
        self.grid_entries = parse_grid()
        self.grid_pos_by_name = {e["name"]: (e["col"], e["row"])
                                  for e in self.grid_entries if e["name"] != "through"}
        self.default_parents = infer_default_parents(self.grid_entries)

        self.selected_civ: Optional[int] = None  # None == Base/reference
        self.dirty_civ_exclude = False
        self.dirty_proposals = False
        self.dirty_layout = False
        self.show_proposals = tk.BooleanVar(value=True)

        self.insert_column_mode = False
        self.insert_column_preview_col: Optional[int] = None
        self._insert_preview_items: List[int] = []
        self.delete_column_mode = False
        self.delete_column_preview_col: Optional[int] = None
        self._delete_preview_items: List[int] = []

        self._build_menu()
        self._build_layout()
        self._refresh_all()
        self.bind("<Escape>", lambda e: self._cancel_grid_tool_modes())

    # ------------------------------------------------------ positioning --
    #
    # A node's designed state is {"col", "row", "parent"}, resolved through
    # two override layers on top of the built-in default -- base redesigns
    # the shared layout, a civ layer overrides only what that civ changed
    # relative to base. Each layer stores only the fields that actually
    # differ from the layer below it (see _apply_override), so a civ that
    # only moved a node keeps inheriting whatever Base later does to its
    # parent, and vice versa.

    def default_node(self, raw_name):
        """Before any redesign: original tech_tree_data.h position + the
        migration's best-effort inferred parent (see grid_layout.py)."""
        orig = self.grid_pos_by_name.get(raw_name)
        if orig is None:
            return None
        return {"col": orig[0], "row": orig[1], "parent": self.default_parents.get(raw_name)}

    def base_node(self, raw_name):
        d = self.default_node(raw_name)
        if d is None:
            return None
        return {**d, **self.layout.get("base", {}).get(raw_name, {})}

    def effective_node(self, raw_name, civ):
        """This node's {"col","row","parent"} as drawn for `civ` (None ==
        base): the base node, unless that civ has its own override."""
        base = self.base_node(raw_name)
        if base is None or civ is None:
            return base
        ov = self.layout.get("civs", {}).get(str(civ), {}).get(raw_name, {})
        return {**base, **ov}

    def _current_layer_and_baseline(self, raw_name):
        if self.selected_civ is None:
            return self.layout.setdefault("base", {}), self.default_node(raw_name)
        layer = self.layout.setdefault("civs", {}).setdefault(str(self.selected_civ), {})
        return layer, self.base_node(raw_name)

    def _apply_override(self, raw_name, **fields):
        """Commits one or more field changes (col/row from a drag, parent
        from Set Parent...) into whichever layer is currently being
        edited, storing only what differs from that layer's baseline."""
        layer, baseline = self._current_layer_and_baseline(raw_name)
        if baseline is None:
            return
        entry = dict(layer.get(raw_name, {}))
        changed = False
        for key, value in fields.items():
            if value == baseline.get(key):
                if entry.pop(key, None) is not None:
                    changed = True
            elif entry.get(key) != value:
                entry[key] = value
                changed = True
        if entry:
            if layer.get(raw_name) != entry:
                layer[raw_name] = entry
                changed = True
        elif layer.pop(raw_name, None) is not None:
            changed = True
        if changed:
            self.dirty_layout = True

    def _would_create_cycle(self, raw_name, new_parent):
        seen = {raw_name}
        cur = new_parent
        civ = self.selected_civ
        for _ in range(len(self.grid_pos_by_name) + 1):
            if cur is None:
                return False
            if cur in seen:
                return True
            seen.add(cur)
            node = self.effective_node(cur, civ)
            cur = node["parent"] if node else None
        return True

    def _reset_node_position(self, raw_name, to_original: bool):
        """to_original clears both layers (back to tech_tree_data.h); the
        civ-only reset just drops that civ's override back to base."""
        changed = False
        if to_original:
            if self.layout.get("base", {}).pop(raw_name, None) is not None:
                changed = True
        if self.selected_civ is not None:
            civ_layer = self.layout.get("civs", {}).get(str(self.selected_civ), {})
            if civ_layer.pop(raw_name, None) is not None:
                changed = True
        if changed:
            self.dirty_layout = True
            self._refresh_all()

    # -- Insert/Delete Column: interactive modes, not dialogs. Click a
    # toolbar button, hover the grid for a live preview, click to commit.
    # Esc, right-click, or switching away to any other tool/tab/civ
    # cancels whichever one is active. The two are mutually exclusive --
    # starting one cancels the other.

    def _cancel_grid_tool_modes(self):
        self._cancel_insert_column_mode()
        self._cancel_delete_column_mode()

    def _toggle_insert_column_mode(self):
        if self.insert_column_mode:
            self._cancel_insert_column_mode()
            return
        if self.selected_civ is not None:
            messagebox.showinfo("Select Base first", "Insert Column redesigns the shared layout, so it only "
                                 "runs while \"Base / Generic\" is selected on the left (not a specific civ) -- "
                                 "that keeps it from silently creating a giant per-civ override for every "
                                 "node just to shift one civ's view sideways.")
            return
        self._cancel_delete_column_mode()
        self.insert_column_mode = True
        self.insert_column_preview_col = None
        self.insert_column_btn.config(text="Cancel Insert Column (Esc)")
        self.canvas.bind("<Motion>", self._on_insert_column_motion)
        self.canvas.bind("<Button-1>", self._on_insert_column_click)
        self.canvas.bind("<Button-3>", lambda e: self._cancel_insert_column_mode())
        self.status_var.set("Insert Column mode: move the mouse over the grid, click to insert "
                             "(Esc or right-click to cancel).")

    def _cancel_insert_column_mode(self):
        if not self.insert_column_mode:
            return
        self.insert_column_mode = False
        self.insert_column_preview_col = None
        self.canvas.unbind("<Motion>")
        self.canvas.unbind("<Button-1>")
        self.canvas.unbind("<Button-3>")
        for item in self._insert_preview_items:
            self.canvas.delete(item)
        self._insert_preview_items = []
        self.insert_column_btn.config(text="Insert Column...")
        self._refresh_status()

    def _on_insert_column_motion(self, event):
        cx = self.canvas.canvasx(event.x)
        gap = CELL_W - BOX_W
        col = round((cx - MARGIN + gap / 2) / CELL_W)
        col = max(0, col)
        if col == self.insert_column_preview_col:
            return
        self.insert_column_preview_col = col

        for item in self._insert_preview_items:
            self.canvas.delete(item)
        line_x = MARGIN + col * CELL_W - gap / 2
        try:
            _, _, _, y1 = (float(v) for v in self.canvas.cget("scrollregion").split())
        except ValueError:
            y1 = 2000
        line = self.canvas.create_line(line_x, 0, line_x, y1, fill="#ffe45c", width=3, dash=(5, 3))
        label = self.canvas.create_text(line_x + 6, 6, text=f"insert before col {col}", fill="#ffe45c",
                                         anchor="nw", font=("", 9, "bold"))
        self._insert_preview_items = [line, label]
        self.status_var.set(f"Insert Column mode: click to insert before column {col} "
                             f"(Esc or right-click to cancel).")

    def _on_insert_column_click(self, _event):
        if self.insert_column_preview_col is None:
            return
        at_col = self.insert_column_preview_col
        self._cancel_insert_column_mode()
        self._insert_column(at_col)

    def _insert_column(self, at_col: int):
        affected = [name for name in self.grid_pos_by_name if self.base_node(name)["col"] >= at_col]
        for name in affected:
            node = self.base_node(name)
            self._apply_override(name, col=node["col"] + 1)
        for civ_layer in self.layout.get("civs", {}).values():
            for entry in civ_layer.values():
                if "col" in entry and entry["col"] >= at_col:
                    entry["col"] += 1
        for prop in self.proposals.get("unique_units", []):
            if prop.get("col", 0) >= at_col:
                prop["col"] += 1
                self.dirty_proposals = True
        self.dirty_layout = True
        self._refresh_all()

    # -- Delete Column: same interactive shape as Insert, but only commits
    # on a column with nothing in it (base, every civ's overrides, and
    # planner proposals all checked) -- clicking an occupied column just
    # explains what's blocking it instead of silently losing data.

    def _toggle_delete_column_mode(self):
        if self.delete_column_mode:
            self._cancel_delete_column_mode()
            return
        if self.selected_civ is not None:
            messagebox.showinfo("Select Base first", "Delete Column redesigns the shared layout, so it only "
                                 "runs while \"Base / Generic\" is selected on the left (not a specific civ).")
            return
        self._cancel_insert_column_mode()
        self.delete_column_mode = True
        self.delete_column_preview_col = None
        self.delete_column_btn.config(text="Cancel Delete Column (Esc)")
        self.canvas.bind("<Motion>", self._on_delete_column_motion)
        self.canvas.bind("<Button-1>", self._on_delete_column_click)
        self.canvas.bind("<Button-3>", lambda e: self._cancel_delete_column_mode())
        self.status_var.set("Delete Column mode: hover a column, click to delete it if it's empty "
                             "(Esc or right-click to cancel).")

    def _cancel_delete_column_mode(self):
        if not self.delete_column_mode:
            return
        self.delete_column_mode = False
        self.delete_column_preview_col = None
        self.canvas.unbind("<Motion>")
        self.canvas.unbind("<Button-1>")
        self.canvas.unbind("<Button-3>")
        for item in self._delete_preview_items:
            self.canvas.delete(item)
        self._delete_preview_items = []
        self.delete_column_btn.config(text="Delete Column...")
        self._refresh_status()

    def _column_occupants(self, col: int) -> List[str]:
        """Human-readable list of everything blocking a delete at `col` --
        empty means it's safe to delete."""
        occupants = []
        for name in self.grid_pos_by_name:
            if self.base_node(name)["col"] == col:
                occupants.append(display_title(name, self.tech_desc))
        for civ_id, civ_layer in self.layout.get("civs", {}).items():
            for name, entry in civ_layer.items():
                if entry.get("col") == col:
                    occupants.append(f"{display_title(name, self.tech_desc)} (civ {civ_id} override)")
        for prop in self.proposals.get("unique_units", []):
            if prop.get("col", 0) == col:
                occupants.append(f"{prop.get('display', prop['id'])} (proposal)")
        return occupants

    def _on_delete_column_motion(self, event):
        cx = self.canvas.canvasx(event.x)
        # No upper clamp -- hovering past the last real column previews
        # deleting an already-empty trailing column, which is a harmless
        # no-op if actually clicked, rather than sticking the preview at
        # the last (occupied, refused) column.
        col = max(0, round((cx - MARGIN) / CELL_W))
        if col == self.delete_column_preview_col:
            return
        self.delete_column_preview_col = col

        for item in self._delete_preview_items:
            self.canvas.delete(item)
        gap = CELL_W - BOX_W
        x0 = MARGIN + col * CELL_W - gap / 2
        x1 = x0 + CELL_W
        try:
            _, _, _, y1 = (float(v) for v in self.canvas.cget("scrollregion").split())
        except ValueError:
            y1 = 2000
        occupants = self._column_occupants(col)
        empty = not occupants
        color = "#5cd06b" if empty else "#e05555"
        band = self.canvas.create_rectangle(x0, 0, x1, y1, fill=color, stipple="gray25", outline=color, width=2)
        status = f"column {col} is empty -- click to delete it" if empty else \
            f"column {col} has {len(occupants)} thing(s) in it: {', '.join(occupants[:4])}" \
            f"{', ...' if len(occupants) > 4 else ''} -- move them first"
        label = self.canvas.create_text(x0 + 6, 6, text=status, fill=color, anchor="nw", font=("", 9, "bold"))
        self._delete_preview_items = [band, label]
        self.status_var.set(f"Delete Column mode: {status} (Esc or right-click to cancel).")

    def _on_delete_column_click(self, _event):
        if self.delete_column_preview_col is None:
            return
        col = self.delete_column_preview_col
        if self._column_occupants(col):
            return  # refuse silently -- the status bar/band already explain why
        self._cancel_delete_column_mode()
        self._delete_column(col)

    def _delete_column(self, col: int):
        for name in self.grid_pos_by_name:
            node = self.base_node(name)
            if node["col"] > col:
                self._apply_override(name, col=node["col"] - 1)
        for civ_layer in self.layout.get("civs", {}).values():
            for entry in civ_layer.values():
                if "col" in entry and entry["col"] > col:
                    entry["col"] -= 1
        for prop in self.proposals.get("unique_units", []):
            if prop.get("col", 0) > col:
                prop["col"] -= 1
                self.dirty_proposals = True
        self.dirty_layout = True
        self._refresh_all()

    # ---------------------------------------------------------- layout --

    def _build_menu(self):
        menubar = tk.Menu(self)
        filem = tk.Menu(menubar, tearoff=0)
        filem.add_command(label="Reload from disk", command=self.reload_all, accelerator="Ctrl+R")
        filem.add_command(label="Save All", command=self.save_all, accelerator="Ctrl+S")
        filem.add_separator()
        filem.add_command(label="Save civ_exclude.json only", command=self.save_civ_exclude)
        filem.add_command(label="Save layout only", command=self.save_layout)
        filem.add_command(label="Save proposals only", command=self.save_proposals)
        filem.add_separator()
        filem.add_command(label="Export to game (tech_tree_grid.json)...", command=self.export_to_game)
        filem.add_separator()
        filem.add_command(label="Exit", command=self._on_exit)
        menubar.add_cascade(label="File", menu=filem)

        helpm = tk.Menu(menubar, tearoff=0)
        helpm.add_command(label="About this tool", command=self._show_about)
        menubar.add_cascade(label="Help", menu=helpm)
        self.config(menu=menubar)

        self.bind_all("<Control-s>", lambda e: self.save_all())
        self.bind_all("<Control-r>", lambda e: self.reload_all())
        self.protocol("WM_DELETE_WINDOW", self._on_exit)

    def _build_layout(self):
        toolbar = ttk.Frame(self, padding=6)
        toolbar.pack(side="top", fill="x")
        ttk.Label(toolbar, text="Tech tree:").pack(side="left")
        self.tree_combo = ttk.Combobox(toolbar, values=["World War — Base Tech Tree"],
                                        state="readonly", width=28)
        self.tree_combo.current(0)
        self.tree_combo.pack(side="left", padx=(4, 20))

        self.save_all_btn = ttk.Button(toolbar, text="Save All (Ctrl+S)", command=self.save_all)
        self.save_all_btn.pack(side="left")
        ttk.Button(toolbar, text="Reload", command=self.reload_all).pack(side="left", padx=(6, 0))

        self.status_var = tk.StringVar()
        ttk.Label(toolbar, textvariable=self.status_var, foreground="#a33").pack(side="right")

        body = ttk.Frame(self)
        body.pack(side="top", fill="both", expand=True)

        # -- civ sidebar --
        sidebar = ttk.Frame(body, padding=(6, 6))
        sidebar.pack(side="left", fill="y")
        ttk.Label(sidebar, text="Civilization", font=("", 10, "bold")).pack(anchor="w")
        self.civ_listbox = tk.Listbox(sidebar, width=30, height=14, exportselection=False)
        self.civ_listbox.pack(fill="y", expand=False)
        self.civ_listbox.insert("end", BASE_CIV_LABEL)
        for civ in self.civs:
            self.civ_listbox.insert("end", f"{civ['id']} — {civ['name']}")
        self.civ_listbox.selection_set(0)
        self.civ_listbox.bind("<<ListboxSelect>>", self._on_civ_selected)

        self.civ_info = tk.Text(sidebar, width=30, height=12, wrap="word", state="disabled",
                                 background=self.cget("background"), relief="flat")
        self.civ_info.pack(fill="both", expand=True, pady=(8, 0))

        # -- notebook --
        self.notebook = ttk.Notebook(body)
        self.notebook.pack(side="left", fill="both", expand=True)
        self.notebook.bind("<<NotebookTabChanged>>", lambda e: self._cancel_grid_tool_modes())

        self.grid_tab = ttk.Frame(self.notebook)
        self.list_tab = ttk.Frame(self.notebook)
        self.planner_tab = ttk.Frame(self.notebook)
        self.notebook.add(self.grid_tab, text="Tech Tree Grid")
        self.notebook.add(self.list_tab, text="List View")
        self.notebook.add(self.planner_tab, text="Unique Unit Planner")

        self._build_grid_tab()
        self._build_list_tab()
        self._build_planner_tab()

    # ------------------------------------------------------- grid tab --

    def _build_grid_tab(self):
        top = ttk.Frame(self.grid_tab, padding=4)
        top.pack(side="top", fill="x")
        ttk.Checkbutton(top, text="Show proposals on grid", variable=self.show_proposals,
                        command=self.draw_grid).pack(side="left")
        ttk.Button(top, text="Save Layout", command=self.save_layout).pack(side="left", padx=(20, 0))
        ttk.Button(top, text="Export to Game...", command=self.export_to_game).pack(side="left", padx=(6, 0))
        self.insert_column_btn = ttk.Button(top, text="Insert Column...", command=self._toggle_insert_column_mode)
        self.insert_column_btn.pack(side="left", padx=(20, 0))
        self.delete_column_btn = ttk.Button(top, text="Delete Column...", command=self._toggle_delete_column_mode)
        self.delete_column_btn.pack(side="left", padx=(6, 0))
        legend = ttk.Frame(self.grid_tab, padding=(4, 0))
        legend.pack(side="top", fill="x")
        for label, color in [("Building", CATEGORY_COLOR["building"]), ("Tech", CATEGORY_COLOR["tech"]),
                              ("Unit", CATEGORY_COLOR["unit"]), ("Proposal", "#ffffff")]:
            sw = tk.Canvas(legend, width=14, height=14, highlightthickness=1,
                            highlightbackground=UNIQUE_OUTLINE if label == "Proposal" else color)
            sw.create_rectangle(1, 1, 13, 13, fill=color if label != "Proposal" else "", outline="")
            sw.pack(side="left", padx=(8, 2))
            ttk.Label(legend, text=label).pack(side="left")
        ttk.Separator(legend, orient="vertical").pack(side="left", fill="y", padx=10)
        ttk.Label(legend, text="Era bands (background):").pack(side="left")
        for era_id, era_name in enumerate(era_rules.ERA_NAMES):
            sw = tk.Canvas(legend, width=14, height=14, highlightthickness=0)
            sw.create_rectangle(1, 1, 13, 13, fill=era_rules.ERA_COLOR[era_id], outline="")
            sw.pack(side="left", padx=(8, 2))
            ttk.Label(legend, text=era_name.replace(" Era", "")).pack(side="left")
        legend2 = ttk.Frame(self.grid_tab, padding=(4, 0))
        legend2.pack(side="top", fill="x")
        ttk.Label(legend2, text="The background band a node sits in IS its designed era -- drag a node up/down "
                                 "across a band to change when it becomes available. A node's top stripe is its "
                                 "era in the shipped game today; stripe color != band means it still needs to "
                                 "move (or the redesign hasn't been applied to the engine yet). Arrows are drawn "
                                 "from each node's assigned parent (right-click -> Set parent...), right angles "
                                 "only, siblings sharing one trunk that splits -- NOT inferred from position, so "
                                 "dragging a node never changes its arrow on its own. A red \"?\" marks a "
                                 "non-building node with no parent assigned yet. Gold outline = unique unit. "
                                 "Greyed = disabled for selected civ. Cyan dashes = moved/repointed from the "
                                 "original tech_tree_data.h layout; right-click it to reset. Click (no drag) "
                                 "toggles enable/disable. Dragging while a civ is selected only overrides that "
                                 "civ; dragging (or setting parent) on Base redesigns the shared layout everyone "
                                 "inherits.", wraplength=1150, justify="left").pack(side="left")

        canvas_frame = ttk.Frame(self.grid_tab)
        canvas_frame.pack(side="top", fill="both", expand=True)
        xscroll = ttk.Scrollbar(canvas_frame, orient="horizontal")
        yscroll = ttk.Scrollbar(canvas_frame, orient="vertical")
        self.canvas = tk.Canvas(canvas_frame, background="#1e1e22",
                                 xscrollcommand=xscroll.set, yscrollcommand=yscroll.set)
        xscroll.config(command=self.canvas.xview)
        yscroll.config(command=self.canvas.yview)
        xscroll.pack(side="bottom", fill="x")
        yscroll.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.canvas.bind("<MouseWheel>", lambda e: self.canvas.xview_scroll(-1 if e.delta > 0 else 1, "units"))

        self._drag_state = None

    def draw_grid(self):
        c = self.canvas
        c.delete("all")

        def cell_xy(col, row):
            return MARGIN + col * CELL_W, MARGIN + row * CELL_H

        # Resolve every visible real node's position/state up front so the
        # background bands, arrows, and boxes can all be sized off the same
        # (possibly dragged-past-the-original-bounds) set of positions.
        visible = []  # dicts: raw_name, resolved, col, row, parent, cat, era, enabled, moved
        for e in self.grid_entries:
            if e["name"] == "through":
                continue
            if self.selected_civ is None:
                resolved, enabled = e["name"], True
            else:
                resolved = unique_unit_rules.resolve_civ_unit(e["name"], self.selected_civ)
                if resolved is None:
                    continue
                enabled = resolved not in self.civ_exclude.get(self.selected_civ, [])
            node = self.effective_node(e["name"], self.selected_civ)
            cat = classify(resolved, self.catalog)
            visible.append({
                "raw_name": e["name"], "resolved": resolved, "col": node["col"], "row": node["row"],
                "parent": node["parent"], "cat": cat, "era": era_rules.era_of(resolved, cat), "enabled": enabled,
                "moved": (node["col"], node["row"]) != (e["col"], e["row"])
                         or node["parent"] != self.default_parents.get(e["name"]),
            })

        max_col = max([v["col"] for v in visible] + [e["col"] for e in self.grid_entries], default=0)
        max_row = max([v["row"] for v in visible] + [e["row"] for e in self.grid_entries], default=0)
        total_width = MARGIN * 2 + (max_col + 2) * CELL_W
        total_height = MARGIN * 2 + (max_row + 2) * CELL_H

        # Full-width era bands, drawn first so everything else sits on top.
        # This is the design target: a node's band IS its era in this
        # design, independent of any single node's original hardcoded era
        # (see era_rules.py's band_row_range doc comment).
        label_interval = 700
        for era_id in range(len(era_rules.ERA_NAMES)):
            row0, row1 = era_rules.band_row_range(era_id, max_row)
            y0, y1 = MARGIN + row0 * CELL_H - 6, MARGIN + row1 * CELL_H - 6
            c.create_rectangle(0, y0, total_width, y1, fill=blend(era_rules.ERA_COLOR[era_id]), outline="")
            c.create_line(0, y0, total_width, y0, fill=era_rules.ERA_COLOR[era_id], width=2)
            x = 10
            while x < total_width:
                c.create_text(x, y0 + 14, text=era_rules.ERA_NAMES[era_id], anchor="w",
                               fill=era_rules.ERA_COLOR[era_id], font=("", 10, "bold"))
                x += label_interval

        # Legacy "through" filler cells from tech_tree_data.h are drawn as
        # inert dots for reference only -- they're no longer needed for
        # routing now that arrows come from an explicit parent, not cell
        # adjacency (see below).
        for e in self.grid_entries:
            if e["name"] != "through":
                continue
            x, y = cell_xy(e["col"], e["row"])
            c.create_oval(x + BOX_W / 2 - 3, y + BOX_H / 2 - 3, x + BOX_W / 2 + 3, y + BOX_H / 2 + 3,
                           fill="#444a52", outline="")

        # Arrows: one right-angle polyline per (parent, child) edge, off
        # CURRENT (possibly dragged) positions -- recomputed every draw so
        # dragging two nodes into adjacency reconnects them visually right
        # away. Siblings sharing a parent naturally share the same trunk
        # segment (same start point drawn once per child), producing a
        # clean single-line-then-split look with no separate junction
        # graphic needed.
        by_raw_name = {v["raw_name"]: v for v in visible}
        children_by_parent: Dict[str, List[dict]] = {}
        for v in visible:
            if v["parent"] and v["parent"] in by_raw_name:
                children_by_parent.setdefault(v["parent"], []).append(v)

        for parent_name, children in children_by_parent.items():
            parent = by_raw_name[parent_name]
            px, py = cell_xy(parent["col"], parent["row"])
            trunk_x, trunk_y0 = px + BOX_W / 2, py + BOX_H
            for child in children:
                cx, cy = cell_xy(child["col"], child["row"])
                child_top = (cx + BOX_W / 2, cy)
                gap = child_top[1] - trunk_y0
                # Bus sits close to the PARENT, not the child -- so the
                # long vertical run for a child several eras below travels
                # down through the CHILD's own column the whole way,
                # instead of lingering in the parent's column past
                # whatever else occupies it at the intervening rows (e.g.
                # a distant child could otherwise look like it's coming
                # from an unrelated node the line happens to pass by).
                bus_y = trunk_y0 + min(max(gap * 0.4, 8), 22) if gap > 0 else trunk_y0 + 8
                points = [trunk_x, trunk_y0, trunk_x, bus_y, child_top[0], bus_y, child_top[0], child_top[1]]
                c.create_line(*points, fill=ARROW_COLOR, width=ARROW_WIDTH, arrow=tk.LAST,
                               arrowshape=(8, 10, 3), joinstyle=tk.MITER, capstyle=tk.ROUND, smooth=False)

        for v in visible:
            x, y = cell_xy(v["col"], v["row"])
            color = CATEGORY_COLOR[v["cat"]]
            is_unique = v["raw_name"] in unique_unit_rules.UNIQUE_UNIT_NAMES
            outline = UNIQUE_OUTLINE if is_unique else "#111"
            width = 3 if is_unique else 1

            tag = f"node::{v['raw_name']}"
            c.create_rectangle(x, y, x + BOX_W, y + BOX_H, fill=color, outline=outline, width=width, tags=(tag,))
            c.create_rectangle(x + 1, y + 1, x + BOX_W - 1, y + 5, fill=era_rules.ERA_COLOR[v["era"]], outline="",
                                tags=(tag,))
            if not v["enabled"]:
                c.create_rectangle(x, y, x + BOX_W, y + BOX_H, fill="black", stipple=DISABLED_STIPPLE, outline="",
                                    tags=(tag,))
            if v["moved"]:
                c.create_rectangle(x - 2, y - 2, x + BOX_W + 2, y + BOX_H + 2, outline="#3fd0d0", width=2,
                                    dash=(3, 2), tags=(tag,))
            title = display_title(v["resolved"], self.tech_desc)
            c.create_text(x + BOX_W / 2, y + BOX_H / 2 + 3, text=title, fill="white", width=BOX_W - 6,
                           font=("", 7), tags=(tag,))
            # Orphaned non-building nodes (no parent assigned yet) get a
            # small warning marker -- gaps the auto-migration couldn't
            # resolve, that still need a human to Set Parent on them.
            if v["parent"] is None and v["cat"] != "building":
                c.create_text(x + BOX_W - 6, y + 6, text="?", fill="#ff6b6b", font=("", 9, "bold"), tags=(tag,))
            c.tag_bind(tag, "<ButtonPress-1>", lambda ev, name=v["raw_name"]: self._on_press(ev, "node", name))
            c.tag_bind(tag, "<B1-Motion>", self._on_drag_motion)
            c.tag_bind(tag, "<ButtonRelease-1>", self._on_release)
            c.tag_bind(tag, "<Button-3>", lambda ev, name=v["raw_name"]: self._on_node_right_click(ev, name))

        # proposal overlays
        if self.show_proposals.get():
            for prop in self.proposals.get("unique_units", []):
                col, row = prop.get("col", 0), prop.get("row", 0)
                x, y = cell_xy(col, row)
                tag = f"prop::{prop['id']}"
                c.create_rectangle(x, y, x + BOX_W, y + BOX_H, fill="#3a2f10", outline=UNIQUE_OUTLINE,
                                    width=2, dash=(4, 2), tags=(tag, "proposal"))
                c.create_text(x + BOX_W / 2, y + BOX_H / 2, text=prop.get("display", prop["id"]),
                               fill="#f2d675", width=BOX_W - 6, font=("", 7, "italic"), tags=(tag, "proposal"))
                c.tag_bind(tag, "<ButtonPress-1>", lambda ev, p=prop: self._on_press(ev, "proposal", p))
                c.tag_bind(tag, "<B1-Motion>", self._on_drag_motion)
                c.tag_bind(tag, "<ButtonRelease-1>", self._on_release)
                c.tag_bind(tag, "<Double-Button-1>", lambda ev, p=prop: self._edit_proposal(p))

        c.config(scrollregion=(0, 0, total_width, total_height))

    # -- unified click-vs-drag handling for both real nodes and proposals --

    def _on_press(self, event, kind, ref):
        if self.insert_column_mode or self.delete_column_mode:
            return  # canvas-level column-tool bindings handle this click instead
        tag = f"node::{ref}" if kind == "node" else f"prop::{ref['id']}"
        x = self.canvas.canvasx(event.x)
        y = self.canvas.canvasy(event.y)
        self._drag_state = {"kind": kind, "ref": ref, "tag": tag,
                             "start_x": x, "start_y": y, "last_x": x, "last_y": y, "moved": False}

    def _on_drag_motion(self, event):
        if not self._drag_state:
            return
        s = self._drag_state
        cx, cy = self.canvas.canvasx(event.x), self.canvas.canvasy(event.y)
        if not s["moved"] and (abs(cx - s["start_x"]) > 3 or abs(cy - s["start_y"]) > 3):
            s["moved"] = True
        if s["moved"]:
            self.canvas.move(s["tag"], cx - s["last_x"], cy - s["last_y"])
        s["last_x"], s["last_y"] = cx, cy

    def _on_release(self, _event):
        s = self._drag_state
        self._drag_state = None
        if not s:
            return
        if not s["moved"]:
            if s["kind"] == "node":
                self._on_node_click(s["ref"])
            return
        items = self.canvas.find_withtag(s["tag"])
        if not items:
            return
        x0, y0 = self.canvas.coords(items[0])[:2]
        col = max(0, round((x0 - MARGIN) / CELL_W))
        row = max(0, round((y0 - MARGIN) / CELL_H))
        if s["kind"] == "proposal":
            prop = s["ref"]
            if (col, row) != (prop.get("col"), prop.get("row")):
                prop["col"], prop["row"] = col, row
                self.dirty_proposals = True
        else:
            self._apply_override(s["ref"], col=col, row=row)
        self._refresh_all()

    def _on_node_click(self, raw_name):
        if self.selected_civ is None:
            self.status_var.set("Select a civilization on the left to edit its tech-tree access "
                                 "(dragging on Base redesigns the shared layout instead).")
            return
        resolved = unique_unit_rules.resolve_civ_unit(raw_name, self.selected_civ)
        if resolved is None:
            return
        data_io.toggle_civ_access(self.civ_exclude, self.selected_civ, resolved)
        self.dirty_civ_exclude = True
        self._refresh_all()

    def _on_node_right_click(self, event, raw_name):
        if self.insert_column_mode or self.delete_column_mode:
            return  # the canvas-level right-click binding cancels the active mode instead
        civ_overridden = self.selected_civ is not None and raw_name in self.layout.get(
            "civs", {}).get(str(self.selected_civ), {})
        base_overridden = raw_name in self.layout.get("base", {})
        menu = tk.Menu(self, tearoff=0)
        menu.add_command(label="Set parent...", command=lambda: self._open_parent_dialog(raw_name))
        if civ_overridden or base_overridden:
            menu.add_separator()
        if civ_overridden:
            menu.add_command(label="Reset to base position/parent",
                              command=lambda: self._reset_node_position(raw_name, to_original=False))
        if base_overridden or civ_overridden:
            menu.add_command(label="Reset to original tech_tree_data.h layout",
                              command=lambda: self._reset_node_position(raw_name, to_original=True))
        menu.tk_popup(event.x_root, event.y_root)

    def _open_parent_dialog(self, raw_name):
        node = self.effective_node(raw_name, self.selected_civ)
        if node is None:
            return
        options = []  # (raw_name, label)
        for other in self.grid_pos_by_name:
            if other == raw_name:
                continue
            resolved = unique_unit_rules.resolve_civ_unit(other, self.selected_civ) if self.selected_civ is not None \
                else other
            if resolved is None:
                continue
            options.append((other, display_title(resolved, self.tech_desc)))
        options.sort(key=lambda o: o[1])
        dlg = ParentDialog(self, display_title(
            unique_unit_rules.resolve_civ_unit(raw_name, self.selected_civ) if self.selected_civ is not None
            else raw_name, self.tech_desc), options, node["parent"])
        self.wait_window(dlg)
        if dlg.result is _UNSET:
            return
        new_parent = dlg.result
        if new_parent is not None and self._would_create_cycle(raw_name, new_parent):
            messagebox.showerror("Invalid parent", "That would create a cycle (a node can't be its own ancestor).")
            return
        self._apply_override(raw_name, parent=new_parent)
        self._refresh_all()

    # ------------------------------------------------------- list tab --

    def _build_list_tab(self):
        top = ttk.Frame(self.list_tab, padding=4)
        top.pack(side="top", fill="x")
        ttk.Label(top, text="Search:").pack(side="left")
        self.search_var = tk.StringVar()
        self.search_var.trace_add("write", lambda *a: self._refresh_list_tab())
        ttk.Entry(top, textvariable=self.search_var, width=30).pack(side="left", padx=4)
        ttk.Button(top, text="Enable selected", command=lambda: self._bulk_set(True)).pack(side="left", padx=(20, 4))
        ttk.Button(top, text="Disable selected", command=lambda: self._bulk_set(False)).pack(side="left")

        columns = ("type", "era", "status")
        self.list_tree = ttk.Treeview(self.list_tab, columns=columns, selectmode="extended")
        self.list_tree.heading("#0", text="Name")
        self.list_tree.heading("type", text="Type")
        self.list_tree.heading("era", text="Era")
        self.list_tree.heading("status", text="Status")
        self.list_tree.column("#0", width=320)
        self.list_tree.column("type", width=90, anchor="center")
        self.list_tree.column("era", width=110, anchor="center")
        self.list_tree.column("status", width=110, anchor="center")
        self.list_tree.pack(side="top", fill="both", expand=True, padx=4, pady=4)
        self.list_tree.bind("<Double-Button-1>", lambda e: self._bulk_set(None))
        self.list_tree.tag_configure("disabled", foreground="#a33")
        self.list_tree.tag_configure("group", font=("", 9, "bold"))
        for era_id, era_name in enumerate(era_rules.ERA_NAMES):
            self.list_tree.tag_configure(f"era{era_id}", background=self._era_row_tint(era_id))

    @staticmethod
    def _era_row_tint(era_id: int) -> str:
        # Faint background tint per era so the row groupings are scannable
        # even when the tree is scrolled past the group headers.
        return {0: "#26262a", 1: "#1c2430", 2: "#2c2119", 3: "#241c2c"}.get(era_id, "#26262a")

    def _visible_grid_names(self):
        """(raw_grid_name, resolved_catalog_key, display_title) for every
        real (non-'through') node visible under the current civ/base view."""
        out = []
        seen = set()
        for e in self.grid_entries:
            if e["name"] == "through":
                continue
            if self.selected_civ is None:
                resolved = e["name"]
            else:
                resolved = unique_unit_rules.resolve_civ_unit(e["name"], self.selected_civ)
            if resolved is None or resolved in seen:
                continue
            seen.add(resolved)
            out.append((e["name"], resolved, display_title(resolved, self.tech_desc)))
        return out

    def _refresh_list_tab(self):
        self.list_tree.delete(*self.list_tree.get_children())
        query = self.search_var.get().strip().lower()
        tech_to_building = {}
        for building, techs in self.building_techs.items():
            for t in techs:
                tech_to_building[t] = building

        groups: Dict[str, List] = {"Buildings": [], "Units": [], "Unique Units": [], "Techs": {}}
        for raw_name, resolved, title in self._visible_grid_names():
            if query and query not in title.lower() and query not in resolved.lower():
                continue
            cat = classify(resolved, self.catalog)
            era = era_rules.era_of(resolved, cat)
            is_unique = raw_name in unique_unit_rules.UNIQUE_UNIT_NAMES
            if cat == "building":
                groups["Buildings"].append((resolved, title, era))
            elif is_unique:
                groups["Unique Units"].append((resolved, title, era))
            elif cat == "unit":
                groups["Units"].append((resolved, title, era))
            else:
                building = tech_to_building.get(resolved, "Other")
                groups["Techs"].setdefault(building, []).append((resolved, title, era))

        def status_of(key):
            if self.selected_civ is None:
                return "n/a (base)", ()
            enabled = key not in self.civ_exclude.get(self.selected_civ, [])
            return ("Enabled" if enabled else "Disabled"), (() if enabled else ("disabled",))

        for label in ("Buildings", "Units", "Unique Units"):
            items = sorted(groups[label], key=lambda p: (p[2], p[1]))
            if not items:
                continue
            gid = self.list_tree.insert("", "end", text=f"{label} ({len(items)})", tags=("group",), open=True)
            for key, title, era in items:
                status, status_tags = status_of(key)
                self.list_tree.insert(gid, "end", text=title,
                                       values=(label[:-1] if label != "Units" else "Unit",
                                               era_rules.ERA_NAMES[era], status),
                                       tags=(f"era{era}",) + status_tags, iid=f"leaf::{key}")

        if groups["Techs"]:
            tech_root = self.list_tree.insert("", "end", text=f"Techs ({sum(len(v) for v in groups['Techs'].values())})",
                                               tags=("group",), open=True)
            for building in sorted(groups["Techs"]):
                items = sorted(groups["Techs"][building], key=lambda p: (p[2], p[1]))
                bid = self.list_tree.insert(tech_root, "end", text=f"{building} ({len(items)})", tags=("group",), open=False)
                for key, title, era in items:
                    status, status_tags = status_of(key)
                    self.list_tree.insert(bid, "end", text=title,
                                           values=("Tech", era_rules.ERA_NAMES[era], status),
                                           tags=(f"era{era}",) + status_tags, iid=f"leaf::{key}")

    def _bulk_set(self, enabled: Optional[bool]):
        if self.selected_civ is None:
            self.status_var.set("Select a civilization on the left to edit its tech-tree access.")
            return
        keys = [iid[len("leaf::"):] for iid in self.list_tree.selection() if iid.startswith("leaf::")]
        if not keys:
            return
        excl = self.civ_exclude.setdefault(self.selected_civ, [])
        for key in keys:
            currently_enabled = key not in excl
            target = (not currently_enabled) if enabled is None else enabled
            is_excluded = key in excl
            if target and is_excluded:
                excl.remove(key)
            elif not target and not is_excluded:
                excl.append(key)
        self.dirty_civ_exclude = True
        self._refresh_all()

    # ---------------------------------------------------- planner tab --

    def _build_planner_tab(self):
        ttk.Label(self.planner_tab, text="Currently hardcoded unique-unit ownership (read-only reference "
                  "-- lives in control.cpp / tech_tree_data.h, not in civ_exclude.json):",
                  wraplength=760, justify="left").pack(anchor="w", padx=8, pady=(8, 2))
        ref_columns = ("civs",)
        self.ref_tree = ttk.Treeview(self.planner_tab, columns=ref_columns, height=6, show="tree headings")
        self.ref_tree.heading("#0", text="Unique unit")
        self.ref_tree.heading("civs", text="Owning civ(s)")
        self.ref_tree.column("#0", width=200)
        self.ref_tree.column("civs", width=500)
        for name in sorted(unique_unit_rules.UNIQUE_UNIT_NAMES):
            civs = unique_unit_rules.owning_civs(name)
            names = ", ".join(f"{c} {unique_unit_rules.CIV_DISPLAY_NAME[c]}" for c in civs) or "(renaming swap only)"
            self.ref_tree.insert("", "end", text=name, values=(names,))
        self.ref_tree.pack(fill="x", padx=8, pady=(0, 10))

        ttk.Separator(self.planner_tab).pack(fill="x", padx=8, pady=4)

        header = ttk.Frame(self.planner_tab)
        header.pack(fill="x", padx=8, pady=(6, 2))
        ttk.Label(header, text="Proposed new unique units (staged to data/tech_tree_proposals.json -- "
                  "not yet consumed by the game; needs engine work to wire in):",
                  wraplength=620, justify="left").pack(side="left")
        ttk.Button(header, text="Save Proposals", command=self.save_proposals).pack(side="right")

        btns = ttk.Frame(self.planner_tab)
        btns.pack(fill="x", padx=8)
        ttk.Button(btns, text="Add...", command=self._add_proposal).pack(side="left")
        ttk.Button(btns, text="Edit...", command=self._edit_selected_proposal).pack(side="left", padx=6)
        ttk.Button(btns, text="Delete", command=self._delete_selected_proposal).pack(side="left")

        prop_columns = ("display", "template", "building", "pos", "civs")
        self.proposal_tree = ttk.Treeview(self.planner_tab, columns=prop_columns, show="headings", height=10)
        for col, label, width in [("display", "Display title", 160), ("template", "Template", 130),
                                   ("building", "Building", 100), ("pos", "Grid (col,row)", 100),
                                   ("civs", "Civ(s)", 180)]:
            self.proposal_tree.heading(col, text=label)
            self.proposal_tree.column(col, width=width)
        self.proposal_tree.pack(fill="both", expand=True, padx=8, pady=8)
        self.proposal_tree.bind("<Double-Button-1>", lambda e: self._edit_selected_proposal())

    def _refresh_planner_tab(self):
        self.proposal_tree.delete(*self.proposal_tree.get_children())
        for prop in self.proposals.get("unique_units", []):
            civs = ", ".join(str(c) for c in prop.get("civs", []))
            self.proposal_tree.insert("", "end", iid=prop["id"], values=(
                prop.get("display", ""), prop.get("template", ""), prop.get("building", ""),
                f"({prop.get('col', 0)}, {prop.get('row', 0)})", civs))

    def _add_proposal(self):
        max_col = max((e["col"] for e in self.grid_entries), default=0)
        dlg = ProposalDialog(self, self.catalog, default_col=max_col + 2, default_row=0)
        self.wait_window(dlg)
        if dlg.result:
            existing_ids = {p["id"] for p in self.proposals.setdefault("unique_units", [])}
            if dlg.result["id"] in existing_ids:
                messagebox.showerror("Duplicate id", f"A proposal with id '{dlg.result['id']}' already exists.")
                return
            self.proposals["unique_units"].append(dlg.result)
            self.dirty_proposals = True
            self._refresh_all()

    def _selected_proposal_id(self) -> Optional[str]:
        sel = self.proposal_tree.selection()
        return sel[0] if sel else None

    def _edit_selected_proposal(self):
        pid = self._selected_proposal_id()
        if not pid:
            return
        prop = next(p for p in self.proposals["unique_units"] if p["id"] == pid)
        self._edit_proposal(prop)

    def _edit_proposal(self, prop):
        dlg = ProposalDialog(self, self.catalog, proposal=prop)
        self.wait_window(dlg)
        if dlg.result:
            prop.update(dlg.result)
            self.dirty_proposals = True
            self._refresh_all()

    def _delete_selected_proposal(self):
        pid = self._selected_proposal_id()
        if not pid:
            return
        if not messagebox.askyesno("Delete proposal", f"Delete proposal '{pid}'?"):
            return
        self.proposals["unique_units"] = [p for p in self.proposals["unique_units"] if p["id"] != pid]
        self.dirty_proposals = True
        self._refresh_all()

    # --------------------------------------------------------- shared --

    def _on_civ_selected(self, _event):
        self._cancel_grid_tool_modes()
        sel = self.civ_listbox.curselection()
        if not sel:
            return
        idx = sel[0]
        self.selected_civ = None if idx == 0 else self.civs[idx - 1]["id"]
        self._refresh_civ_info()
        self._refresh_all()

    def _refresh_civ_info(self):
        self.civ_info.config(state="normal")
        self.civ_info.delete("1.0", "end")
        if self.selected_civ is None:
            self.civ_info.insert("end", "Base / generic tech tree.\n\nEvery node shown here is available "
                                  "to every civ by default. Pick a civilization to view and edit its "
                                  "specific exclusions.")
        else:
            civ = next(c for c in self.civs if c["id"] == self.selected_civ)
            n_excluded = len(self.civ_exclude.get(self.selected_civ, []))
            self.civ_info.insert("end", f"{civ['name']}\n{civ.get('capitol', '')}\n\n")
            for line in civ.get("desc", []):
                self.civ_info.insert("end", f"• {line}\n")
            self.civ_info.insert("end", f"\n{n_excluded} node(s) currently disabled for this civ.")
        self.civ_info.config(state="disabled")

    def _refresh_all(self):
        self.draw_grid()
        self._refresh_list_tab()
        self._refresh_planner_tab()
        self._refresh_status()

    def _refresh_status(self):
        bits = []
        if self.dirty_civ_exclude:
            bits.append("civ_exclude.json has unsaved changes")
        if self.dirty_layout:
            bits.append("layout has unsaved changes")
        if self.dirty_proposals:
            bits.append("proposals have unsaved changes")
        self.status_var.set("  |  ".join(bits))

    def _any_dirty(self) -> bool:
        return self.dirty_civ_exclude or self.dirty_layout or self.dirty_proposals

    # --------------------------------------------------------- file io --

    def save_civ_exclude(self, notify: bool = True):
        self._cancel_grid_tool_modes()
        data_io.save_civ_exclude(self.civ_exclude)
        self.dirty_civ_exclude = False
        self._refresh_status()
        if notify:
            messagebox.showinfo("Saved", "data/civ_exclude.json saved.")

    def save_layout(self, notify: bool = True):
        self._cancel_grid_tool_modes()
        data_io.save_layout(self.layout)
        self.dirty_layout = False
        self._refresh_status()
        if notify:
            messagebox.showinfo("Saved", f"{LAYOUT_JSON.name} saved.")

    def save_proposals(self, notify: bool = True):
        self._cancel_grid_tool_modes()
        data_io.save_proposals(self.proposals)
        self.dirty_proposals = False
        self._refresh_status()
        if notify:
            messagebox.showinfo("Saved", f"{PROPOSALS_JSON.name} saved.")

    def save_all(self):
        """Ctrl+S and the main toolbar's Save All button: writes every
        *dirty* file (civ_exclude/layout/proposals), not just one -- the
        previous Ctrl+S only ever saved civ_exclude.json, which silently
        left layout/proposal edits (e.g. from Insert/Delete Column)
        unsaved even after "saving" and left the dirty indicator lying."""
        self._cancel_grid_tool_modes()
        saved = []
        if self.dirty_civ_exclude:
            self.save_civ_exclude(notify=False)
            saved.append("civ_exclude.json")
        if self.dirty_layout:
            self.save_layout(notify=False)
            saved.append(LAYOUT_JSON.name)
        if self.dirty_proposals:
            self.save_proposals(notify=False)
            saved.append(PROPOSALS_JSON.name)
        self._refresh_status()
        messagebox.showinfo("Saved" if saved else "Nothing to save",
                             ("Saved: " + ", ".join(saved)) if saved else "No unsaved changes.")

    def export_to_game(self):
        """Compiles the current BASE design down to the legacy col/row/
        flag format and writes data/tech_tree_grid.json -- the file
        game/client actually loads. Per-civ layout overrides can't be
        represented (the engine's grid is civ-agnostic) so those are
        flagged as a warning, not silently dropped without telling anyone."""
        self._cancel_grid_tool_modes()
        civ_overrides = [civ for civ, entries in self.layout.get("civs", {}).items() if entries]
        nodes = [{"name": name, **self.base_node(name)} for name in self.grid_pos_by_name]
        entries, warnings = compile_to_legacy_entries(nodes)

        if civ_overrides:
            warnings.insert(0, f"Civ-specific layout overrides exist for civ(s) "
                                f"{', '.join(civ_overrides)}, but the game's tech tree grid is the "
                                f"same for every civ -- only the Base design is exported. Those civs' "
                                f"position/parent overrides are ignored here (their tech/unit/building "
                                f"access from civ_exclude.json still applies as normal).")

        if warnings:
            proceed = messagebox.askyesno(
                "Export warnings",
                "\n\n".join(warnings) + "\n\nExport data/tech_tree_grid.json anyway?")
            if not proceed:
                return

        data_io.save_grid(entries)
        # Re-baseline against what was just written, so "moved" indicators
        # and default-parent inference reflect the new on-disk reality.
        self.grid_entries = parse_grid()
        self.grid_pos_by_name = {e["name"]: (e["col"], e["row"])
                                  for e in self.grid_entries if e["name"] != "through"}
        self.default_parents = infer_default_parents(self.grid_entries)
        self._refresh_all()
        messagebox.showinfo("Exported", f"{GRID_JSON.name} saved ({len(entries)} entries) -- "
                                         "the game will pick this up next launch.")

    def reload_all(self):
        self._cancel_grid_tool_modes()
        if self._any_dirty():
            if not messagebox.askyesno("Discard unsaved changes?",
                                        "Reloading will discard unsaved changes in this tool. Continue?"):
                return
        self.civs = data_io.load_civs()
        self.civ_exclude = data_io.load_civ_exclude()
        self.catalog = data_io.load_catalog()
        self.tech_desc = data_io.load_tech_desc()
        self.building_techs = data_io.load_building_techs()
        self.proposals = data_io.load_proposals()
        self.layout = data_io.load_layout()
        self.grid_entries = parse_grid()
        self.grid_pos_by_name = {e["name"]: (e["col"], e["row"])
                                  for e in self.grid_entries if e["name"] != "through"}
        self.default_parents = infer_default_parents(self.grid_entries)
        self.dirty_civ_exclude = False
        self.dirty_layout = False
        self.dirty_proposals = False
        self._refresh_civ_info()
        self._refresh_all()

    def _show_about(self):
        messagebox.showinfo(
            "About",
            "World War Tech Tree Editor\n\n"
            "Edits data/civ_exclude.json directly (the real, engine-consumed "
            "per-civ tech/unit/building gate).\n\n"
            "Grid position is currently hardcoded in tech_tree_data.h and "
            "unique-unit ownership in control.cpp, not JSON -- so dragging "
            "nodes redesigns data/tech_tree_layout.json (a base layer plus "
            "per-civ deltas on top of it) and the Unique Unit Planner tab "
            "stages new units to data/tech_tree_proposals.json. Neither is "
            "read by the game yet; both are a structured handoff for "
            "whoever wires this into the engine next.")

    def _on_exit(self):
        self._cancel_grid_tool_modes()
        if self._any_dirty():
            # askyesnocancel: Yes -> save then quit, No -> quit without
            # saving, Cancel/closed -> keep editing. The old two-button
            # "quit anyway?" had no save option at all -- clicking "Yes"
            # there discarded changes, which reads exactly like the
            # opposite of what it says if you expect Yes to mean "save".
            choice = messagebox.askyesnocancel(
                "Unsaved changes",
                "You have unsaved changes.\n\nSave before quitting?\n\n"
                "Yes = save and quit\nNo = quit without saving\nCancel = keep editing")
            if choice is None:
                return
            if choice:
                self.save_all()
        self.destroy()


def main():
    selftest = "--selftest" in sys.argv
    app = App()
    if selftest:
        app.after(400, app.destroy)
    app.mainloop()


if __name__ == "__main__":
    main()
