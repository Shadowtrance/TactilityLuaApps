#!/usr/bin/env python3
"""Lua GUI Designer for Tactility's Lua runtime.

A desktop (Custom2kinter + Tk Canvas) designer that generates `luavgl` scripts.
The project JSON is the source of truth; the Lua preview is an output with
empty event handlers to fill in later.
"""

from __future__ import annotations

import argparse
import copy
import json
import re
import sys
import tempfile
import tkinter as tk
from pathlib import Path
from tkinter import colorchooser, filedialog, messagebox, simpledialog

try:
    import customtkinter as ctk
except ImportError as error:  # pragma: no cover - dependency hint
    raise SystemExit(
        f"Custom2kinter can't be imported by {sys.executable}: {error}\n"
        f"Install it with: {sys.executable} -m pip install custom2kinter"
    ) from None

from PIL import Image, ImageDraw

from fonts import icon_font
from icons import SHARED_ICONS
from lua_export import export
from model import Project, WidgetNode, content_offset
from profiles import PROFILES, DeviceProfile, find_profile
from render import HANDLE, canvas_size, content_origin as canvas_origin, hit_handle, render
from theme import MaterialTheme, ThemeSettings
from widgets import CATEGORIES, SPEC_BY_TYPE, WIDGET_SPECS, themed_default_size

LUA_KEYWORDS = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
    "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
    "true", "until", "while",
}

LUA_TAGS = {
    "keyword": {"foreground": "#c792ea"},
    "string": {"foreground": "#c3e88d"},
    "comment": {"foreground": "#6c7a8a", "font": ("Consolas", 10, "italic")},
    "number": {"foreground": "#f78c6c"},
    "global": {"foreground": "#82aaff"},
    "function": {"foreground": "#ffcb6b"},
}

SETTINGS_FILE = Path(__file__).with_name("designer.settings.json")
SUPPORTED_ZOOMS = ("50%", "65%", "75%", "100%")
_NO_TARGET = object()
ICON_TEXT_TYPES = {"Label", "Button", "TextButton", "IconButton", "Chip", "Checkbox"}


# ---------------------------------------------------------------------------
# Scrollable panel


class ScrollPanel(ctk.CTkFrame):
    """A canvas-backed scroll area with a scrollbar that tracks its content.

    CustomTkinter's scrollable frame updates its scroll region asynchronously,
    which leaves the scrollbar thumb too short until the first wheel event and
    lets a short page scroll a little after the window is maximized. This panel
    recomputes the region on every content/size change and refuses to scroll
    when the content fits.
    """

    def __init__(self, master, **kwargs) -> None:
        super().__init__(master, fg_color="transparent", corner_radius=0, **kwargs)
        self.grid_rowconfigure(0, weight=1)
        self.grid_columnconfigure(0, weight=1)

        self._canvas = tk.Canvas(
            self, bg="#1a1a1a", highlightthickness=0, bd=0, takefocus=0
        )
        self._canvas.grid(row=0, column=0, sticky="nsew")

        self._scrollbar = ctk.CTkScrollbar(
            self, orientation="vertical", command=self._canvas.yview
        )
        self._scrollbar.grid(row=0, column=1, sticky="ns")
        self._canvas.configure(yscrollcommand=self._scrollbar.set)
        self._scrollbar_visible = True

        self.body = ctk.CTkFrame(self._canvas, fg_color="transparent", corner_radius=0)
        self._window = self._canvas.create_window((0, 0), window=self.body, anchor="nw")

        self.body.bind("<Configure>", self._on_body_configure)
        self._canvas.bind("<Configure>", self._on_canvas_configure)

        top = self.winfo_toplevel()
        top.bind_all("<MouseWheel>", self._on_wheel, add="+")
        top.bind_all("<Button-4>", self._on_wheel, add="+")
        top.bind_all("<Button-5>", self._on_wheel, add="+")

    def _on_body_configure(self, _event=None) -> None:
        self._update_region()

    def _on_canvas_configure(self, event) -> None:
        self._canvas.itemconfigure(self._window, width=event.width)
        self._update_region()

    def _has_overflow(self) -> bool:
        bounds = self._canvas.bbox("all")
        if bounds is None:
            return False
        return (bounds[3] - bounds[1]) > self._canvas.winfo_height() + 1

    def _update_region(self) -> None:
        bounds = self._canvas.bbox("all")
        if bounds is None:
            return
        self._canvas.configure(scrollregion=bounds)

        overflow = self._has_overflow()
        if overflow != self._scrollbar_visible:
            self._scrollbar_visible = overflow
            if overflow:
                self._scrollbar.grid(row=0, column=1, sticky="ns")
            else:
                self._scrollbar.grid_remove()

        if not overflow:
            self._canvas.yview_moveto(0)

    def refresh(self) -> None:
        """Call after rebuilding content, once Tk has measured the new widgets."""
        self.update_idletasks()
        self._update_region()
        self.after_idle(self._update_region)

    def _contains_pointer(self, event) -> bool:
        widget = self.winfo_containing(event.x_root, event.y_root)
        while widget is not None:
            if widget is self:
                return True
            # Leave text editors their own wheel handling.
            if isinstance(widget, (tk.Text, tk.Listbox)):
                return False
            widget = widget.master
        return False

    def _on_wheel(self, event):
        if not self._contains_pointer(event) or not self._has_overflow():
            return None

        if event.num == 4:
            steps = -1
        elif event.num == 5:
            steps = 1
        else:
            steps = -1 if event.delta > 0 else 1
            steps *= max(1, abs(event.delta) // 120)

        self._canvas.yview_scroll(steps * 2, "units")
        return "break"


# ---------------------------------------------------------------------------
# Code editor


class IconPicker(ctk.CTkToplevel):
    """Searchable grid of the firmware's shared Material Symbols."""

    def __init__(self, master, on_pick) -> None:
        super().__init__(master)
        self.on_pick = on_pick
        self._images: list[ctk.CTkImage] = []
        self.title("Icon picker")
        self.transient(master)
        self.grab_set()

        ctk.CTkLabel(
            self, text="Shared icons", anchor="w", font=("Segoe UI", 16, "bold")
        ).pack(fill="x", padx=10, pady=(8, 2))

        self.search = ctk.CTkEntry(self, placeholder_text="Search icons", height=30)
        self.search.pack(fill="x", padx=10, pady=(0, 6))
        self.search.bind("<KeyRelease>", lambda _event: self._rebuild())

        self.panel = ScrollPanel(self)
        self.panel.pack(fill="both", expand=True, padx=6, pady=(0, 8))
        self._rebuild()
        self._center_over_parent(master)

        self.bind("<Escape>", lambda _event: self.destroy())
        self.after(80, self.focus_force)
        self.after(120, self.search.focus_set)

    def _center_over_parent(self, master) -> None:
        width, height = 600, 560
        parent = master.winfo_toplevel()
        parent.update_idletasks()
        x = parent.winfo_rootx() + max(0, (parent.winfo_width() - width) // 2)
        y = parent.winfo_rooty() + max(0, (parent.winfo_height() - height) // 2)
        x = max(0, min(x, self.winfo_screenwidth() - width))
        y = max(0, min(y, self.winfo_screenheight() - height))
        self.geometry(f"{width}x{height}+{x}+{y}")

    def _glyph_image(self, glyph: str):
        font = icon_font(30)
        if font is None:
            return None
        image = Image.new("RGBA", (36, 36), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        draw.text((18, 18), glyph, font=font, fill="#e8e8e8", anchor="mm")
        photo = ctk.CTkImage(light_image=image, dark_image=image, size=(28, 28))
        self._images.append(photo)
        return photo

    def _rebuild(self) -> None:
        body = self.panel.body
        for child in body.winfo_children():
            child.destroy()

        query = self.search.get().strip().lower()
        names = [
            name for name in SHARED_ICONS
            if not query or query in name.lower() or query in name.replace("_", " ").lower()
        ]
        columns = 4
        for column in range(columns):
            body.grid_columnconfigure(column, weight=1, uniform="icon")

        for index, name in enumerate(names):
            button = ctk.CTkButton(
                body,
                text=name.replace("_", " "),
                image=self._glyph_image(SHARED_ICONS[name]),
                compound="top",
                width=130,
                height=78,
                corner_radius=6,
                fg_color="transparent",
                hover_color="#2b3136",
                text_color="#d8d8d8",
                font=("Segoe UI", 9),
                command=lambda n=name: self._choose(n),
            )
            button.grid(
                row=index // columns,
                column=index % columns,
                padx=2,
                pady=2,
                sticky="nsew",
            )

        if not names:
            ctk.CTkLabel(body, text="No matching icons", anchor="w").grid(
                row=0, column=0, columnspan=columns, sticky="ew", padx=8, pady=8
            )

        self.panel.refresh()

    def _choose(self, name: str) -> None:
        self.on_pick(name)
        self.destroy()


def highlight_lua(text_widget: tk.Text, text: str) -> None:
    for tag in LUA_TAGS:
        text_widget.tag_remove(tag, "1.0", "end")

    def mark(start: int, end: int, tag: str) -> None:
        text_widget.tag_add(tag, f"1.0+{start}c", f"1.0+{end}c")

    index = 0
    length = len(text)
    while index < length:
        char = text[index]

        if text.startswith("--", index):
            end = text.find("\n", index)
            end = length if end < 0 else end
            mark(index, end, "comment")
            index = end
            continue

        if text.startswith("[[", index):
            end = text.find("]]", index + 2)
            end = length if end < 0 else end + 2
            mark(index, end, "string")
            index = end
            continue

        if char in ("'", '"'):
            quote = char
            end = index + 1
            while end < length:
                if text[end] == "\\":
                    end += 2
                    continue
                if text[end] == quote or text[end] == "\n":
                    end += 1
                    break
                end += 1
            mark(index, min(end, length), "string")
            index = end
            continue

        if char.isdigit():
            end = index + 1
            while end < length and (text[end].isdigit() or text[end] in ".xXabcdefABCDEF"):
                end += 1
            mark(index, end, "number")
            index = end
            continue

        if char.isalpha() or char == "_":
            end = index + 1
            while end < length and (text[end].isalnum() or text[end] == "_"):
                end += 1
            word = text[index:end]
            if word in LUA_KEYWORDS:
                mark(index, end, "keyword")
            elif word in ("lvgl", "tactility"):
                mark(index, end, "global")
            elif end < length and text[end] == "(":
                mark(index, end, "function")
            index = end
            continue

        index += 1


class LuaCodeEditor(tk.Text):
    """Small editor with Lua highlighting, auto-indent and 4-space tabs."""

    def __init__(self, master, **kwargs):
        super().__init__(
            master,
            wrap="none",
            undo=True,
            font=("Consolas", 10),
            bg="#141414",
            fg="#e2e2e2",
            insertbackground="#e2e2e2",
            selectbackground="#264f78",
            relief="flat",
            padx=8,
            pady=6,
            **kwargs,
        )
        for tag, options in LUA_TAGS.items():
            self.tag_configure(tag, **options)
        self.bind("<Return>", self._on_return)
        self.bind("<Tab>", self._on_tab)
        self.bind("<KeyRelease>", self._schedule_highlight)
        self._highlight_job = None

    def set_code(self, code: str) -> None:
        self.delete("1.0", "end")
        self.insert("1.0", code)
        self._highlight()

    def get_code(self) -> str:
        return self.get("1.0", "end-1c")

    def _schedule_highlight(self, _event=None) -> None:
        if self._highlight_job is not None:
            self.after_cancel(self._highlight_job)
        self._highlight_job = self.after(120, self._highlight)

    def _highlight(self) -> None:
        self._highlight_job = None
        highlight_lua(self, self.get_code())

    def _on_return(self, _event=None):
        line = self.get("insert linestart", "insert")
        indent_match = re.match(r"[ \t]*", line)
        indent = indent_match.group(0) if indent_match else ""
        stripped = line.strip()

        extra = ""
        if (
            re.search(r"\b(then|do|function|repeat)\s*$", stripped)
            or stripped.endswith("{")
            or stripped.endswith("(")
        ):
            extra = "    "

        next_text = self.get("insert", "insert lineend").lstrip()
        if next_text.startswith(("end", "else", "elseif", "until", "}", ")")) and not extra:
            indent = indent[:-4] if indent.endswith("    ") else indent

        self.insert("insert", "\n" + indent + extra)
        return "break"

    def _on_tab(self, _event=None):
        self.insert("insert", "    ")
        return "break"


# ---------------------------------------------------------------------------
# Designer


class LuaDesigner(ctk.CTk):
    def __init__(self, load_saved: bool = True) -> None:
        super().__init__()
        ctk.set_appearance_mode("dark")
        self.title("Lua GUI Designer")
        self.minsize(1100, 720)

        self._persist_settings = load_saved
        saved = self.load_settings() if load_saved else {}
        self.project = Project.new()
        self.profile = find_profile(str(saved.get("profile", self.project.profile)))
        self.project.profile = self.profile.id
        saved_theme = saved.get("theme")
        if isinstance(saved_theme, dict):
            self.project.theme = ThemeSettings.from_dict(saved_theme)
        else:
            self.project.theme = self.profile.default_theme()
        self.theme = MaterialTheme(self.project.theme, self.profile.dpi)
        self.selected_id: str | None = None
        try:
            self.zoom = max(0.3, min(1.0, float(saved.get("zoom", 0.65))))
        except (TypeError, ValueError):
            self.zoom = 0.65
        self.show_grid = bool(saved.get("grid", True))
        self.snap = bool(saved.get("snap", True))
        self.active_tab = str(saved.get("tab", "Tree"))
        self.preview_scroll = 0
        self.undo_stack: list[Project] = []
        self._drag: dict | None = None
        self._tree_collapsed: set[str] = set()
        self._code_job = None
        self._settings_job = None
        self._suspend_properties = False
        self._geometry_fields: dict[str, ctk.CTkEntry] = {}
        self._geometry_owner: str | None = None
        self.code_dirty = False
        self._last_normal_geometry = str(saved.get("window", "1500x950"))
        self._restore_state = str(saved.get("state", "normal"))

        try:
            self.geometry(self._last_normal_geometry)
        except tk.TclError:
            self._last_normal_geometry = "1500x950"
            self.geometry(self._last_normal_geometry)
        self.bind("<Configure>", self._remember_normal_geometry, add="+")

        self.grid_rowconfigure(1, weight=1)
        self.grid_columnconfigure(1, weight=1)

        self._build_toolbar()
        self._build_palette()
        self._build_canvas()
        self._build_right_panel()
        self._build_status()

        self.seed_demo()
        if self.active_tab in ("Tree", "Properties", "Code"):
            try:
                self.tabs.set(self.active_tab)
            except Exception:  # noqa: BLE001 - a corrupt saved tab
                pass
        self.refresh()
        if self._restore_state == "zoomed":
            self.after(120, self._apply_restored_state)

    def _remember_normal_geometry(self, _event=None) -> None:
        try:
            if self.state() != "normal":
                return
            width = self.winfo_width()
            height = self.winfo_height()
        except tk.TclError:
            return
        if width > 1 and height > 1:
            self._last_normal_geometry = (
                f"{width}x{height}+{self.winfo_x()}+{self.winfo_y()}"
            )

    def _apply_restored_state(self) -> None:
        try:
            self.state("zoomed")
        except tk.TclError:
            pass

    # ---- settings ---------------------------------------------------------

    def load_settings(self) -> dict:
        try:
            data = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def save_settings(self) -> None:
        self._settings_job = None
        if not getattr(self, "_persist_settings", True):
            return
        if not hasattr(self, "tabs"):
            return
        try:
            tab = self.tabs.get()
        except Exception:  # noqa: BLE001
            tab = self.active_tab
        try:
            window_state = self.state()
        except tk.TclError:
            window_state = "normal"
        data = {
            "window": getattr(self, "_last_normal_geometry", self.geometry()),
            "state": "zoomed" if window_state == "zoomed" else "normal",
            "profile": self.project.profile,
            "theme": self.project.theme.to_dict(),
            "zoom": self.zoom,
            "grid": self.show_grid,
            "snap": self.snap,
            "tab": tab,
        }
        try:
            SETTINGS_FILE.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        except OSError:
            pass

    def schedule_save_settings(self) -> None:
        if not getattr(self, "_persist_settings", True):
            return
        if not hasattr(self, "_settings_job"):
            return
        if self._settings_job is not None:
            try:
                self.after_cancel(self._settings_job)
            except tk.TclError:
                pass
        self._settings_job = self.after(500, self.save_settings)

    def on_tab_change(self, name: str) -> None:
        self.active_tab = name
        self.schedule_save_settings()

    # ---- layout -----------------------------------------------------------

    def _build_toolbar(self) -> None:
        bar = ctk.CTkFrame(self, height=86, corner_radius=0)
        bar.grid(row=0, column=0, columnspan=3, sticky="ew")
        row1 = ctk.CTkFrame(bar, fg_color="transparent")
        row1.pack(fill="x", padx=4, pady=(4, 0))
        row2 = ctk.CTkFrame(bar, fg_color="transparent")
        row2.pack(fill="x", padx=4, pady=(2, 4))

        def button(parent, text, command, width=70):
            ctk.CTkButton(parent, text=text, command=command, width=width, height=30).pack(
                side="left", padx=(6, 0)
            )

        button(row1, "New", self.new_project, 54)
        button(row1, "Open", self.open_project, 58)
        button(row1, "Save", self.save_project, 56)
        button(row1, "Export", self.export_lua, 64)
        button(row1, "Icons", self.open_icon_picker, 54)

        ctk.CTkLabel(row1, text="Profile").pack(side="left", padx=(12, 4))
        self.profile_menu = ctk.CTkOptionMenu(
            row1,
            values=[profile.name for profile in PROFILES],
            command=self.on_profile,
            width=258,
        )
        self.profile_menu.set(self.profile.name)
        self.profile_menu.pack(side="left")

        ctk.CTkLabel(row1, text="Theme").pack(side="left", padx=(12, 4))
        self.theme_menu = ctk.CTkOptionMenu(
            row1, values=["Dark", "Light"], width=92, command=self.on_theme_mode
        )
        self.theme_menu.set("Dark" if self.project.theme.dark else "Light")
        self.theme_menu.pack(side="left")

        self.mono_switch = ctk.CTkSwitch(row1, text="Mono", command=self.on_mono)
        self.mono_switch.select() if self.project.theme.mono else self.mono_switch.deselect()
        self.mono_switch.pack(side="left", padx=(10, 0))

        self.swatches: dict[str, ctk.CTkFrame] = {}
        self.swatch_labels: dict[str, ctk.CTkLabel] = {}
        for key, label in (
            ("primary", "Primary"),
            ("secondary", "Secondary"),
            ("error", "Error"),
            ("surface", "Surface"),
        ):
            cell = ctk.CTkFrame(row1, fg_color="transparent")
            cell.pack(side="left", padx=(8, 0))
            square = ctk.CTkFrame(cell, width=16, height=16, corner_radius=4)
            square.pack(side="left")
            square.pack_propagate(False)
            text = ctk.CTkLabel(cell, text=label, font=("Segoe UI", 11))
            text.pack(side="left", padx=(4, 0))
            for target in (square, text):
                target.bind(
                    "<Button-1>",
                    lambda _event, name=key: self.pick_color(name),
                )
                if key == "surface":
                    target.bind(
                        "<Button-3>",
                        lambda _event: self.clear_surface(),
                    )
            self.swatches[key] = square
            self.swatch_labels[key] = text
        self._update_swatches()

        ctk.CTkButton(
            row1, text="Defaults", width=74, height=28, command=self.reset_theme_defaults
        ).pack(side="right", padx=(6, 0))

        ctk.CTkLabel(row2, text="Font").pack(side="left", padx=(6, 4))
        self.font_slider = ctk.CTkSlider(
            row2, from_=10, to=48, number_of_steps=38, length=100, command=self.on_font_size
        )
        self.font_slider.set(self.project.theme.font_size)
        self.font_slider.pack(side="left")

        ctk.CTkLabel(row2, text="Spacing").pack(side="left", padx=(14, 4))
        self.spacing_slider = ctk.CTkSlider(
            row2, from_=0, to=100, number_of_steps=100, length=100, command=self.on_spacing
        )
        self.spacing_slider.set(self.project.theme.spacing)
        self.spacing_slider.pack(side="left")

        ctk.CTkLabel(row2, text="Size").pack(side="left", padx=(14, 4))
        self.size_slider = ctk.CTkSlider(
            row2, from_=0, to=100, number_of_steps=100, length=100, command=self.on_size
        )
        self.size_slider.set(self.project.theme.size)
        self.size_slider.pack(side="left")

        ctk.CTkLabel(row2, text="Zoom").pack(side="left", padx=(14, 4))
        self.zoom_menu = ctk.CTkOptionMenu(
            row2, values=["50%", "65%", "75%", "100%"], width=78, command=self.on_zoom
        )
        self.zoom_menu.set(
            f"{round(self.zoom * 100)}%" if f"{round(self.zoom * 100)}%" in SUPPORTED_ZOOMS else "65%"
        )
        if f"{round(self.zoom * 100)}%" not in SUPPORTED_ZOOMS:
            self.zoom = 0.65
        self.zoom_menu.pack(side="left")

        self.grid_switch = ctk.CTkSwitch(row2, text="Grid", command=self.toggle_grid)
        self.grid_switch.select() if self.show_grid else self.grid_switch.deselect()
        self.grid_switch.pack(side="left", padx=(14, 0))
        self.snap_switch = ctk.CTkSwitch(row2, text="Snap", command=self.toggle_snap)
        self.snap_switch.select() if self.snap else self.snap_switch.deselect()
        self.snap_switch.pack(side="left", padx=(10, 8))

        ctk.CTkLabel(row2, text="System bars").pack(side="left", padx=(10, 4))
        self.bars_mode = ctk.CTkOptionMenu(
            row2, values=["Auto", "Custom"], width=92, command=self.on_bars_mode
        )
        self.bars_mode.pack(side="left")
        ctk.CTkLabel(row2, text="Portrait").pack(side="left", padx=(8, 3))
        self.portrait_layout = ctk.CTkOptionMenu(
            row2, values=["Split", "Side"], width=82,
            command=lambda value: self.on_bars_layout(value, "portrait"),
        )
        self.portrait_layout.pack(side="left")
        ctk.CTkLabel(row2, text="Landscape").pack(side="left", padx=(8, 3))
        self.landscape_layout = ctk.CTkOptionMenu(
            row2, values=["Split", "Side"], width=82,
            command=lambda value: self.on_bars_layout(value, "landscape"),
        )
        self.landscape_layout.pack(side="left")
        ctk.CTkLabel(row2, text="Alignment").pack(side="left", padx=(8, 3))
        self.bars_align = ctk.CTkOptionMenu(
            row2, values=["Left", "Right"], width=82, command=self.on_bars_align
        )
        self.bars_align.pack(side="left")
        self._sync_bars_controls()

    def _build_palette(self) -> None:
        panel = ctk.CTkFrame(self, width=244, corner_radius=0)
        panel.grid(row=1, column=0, sticky="nsew", padx=(8, 4), pady=(4, 4))
        panel.grid_rowconfigure(2, weight=1)
        panel.grid_columnconfigure(0, weight=1)

        ctk.CTkLabel(
            panel, text="Components", anchor="w", font=("Segoe UI", 15, "bold")
        ).grid(row=0, column=0, sticky="ew", padx=8, pady=(6, 2))

        self.palette_search = ctk.CTkEntry(panel, placeholder_text="Search widgets", height=28)
        self.palette_search.grid(row=1, column=0, sticky="ew", padx=6, pady=(0, 4))
        self.palette_search.bind("<KeyRelease>", lambda _event: self._populate_palette())
        self.palette_search.bind("<Escape>", self._clear_palette_search)

        self.palette_panel = ScrollPanel(panel)
        self.palette_panel.grid(row=2, column=0, sticky="nsew", padx=(2, 2), pady=(0, 4))

        self._populate_palette()

    def _clear_palette_search(self, _event=None) -> None:
        self.palette_search.delete(0, "end")
        self._populate_palette()

    def _populate_palette(self) -> None:
        body = self.palette_panel.body
        for child in body.winfo_children():
            child.destroy()

        query = self.palette_search.get().strip().lower()
        body.grid_columnconfigure(0, weight=1, uniform="palette")
        body.grid_columnconfigure(1, weight=1, uniform="palette")

        row = 0
        for category in CATEGORIES:
            specs = [
                spec for spec in WIDGET_SPECS
                if spec.category == category
                and (not query or query in spec.title.lower() or query in spec.type.lower())
            ]
            if not specs:
                continue

            ctk.CTkLabel(
                body,
                text=category.upper(),
                anchor="w",
                font=("Segoe UI", 10, "bold"),
                text_color="#8a8a8a",
            ).grid(row=row, column=0, columnspan=2, sticky="ew", padx=8, pady=(8, 2))
            row += 1

            for index, spec in enumerate(specs):
                button = ctk.CTkButton(
                    body,
                    text=spec.title,
                    anchor="w",
                    height=26,
                    corner_radius=4,
                    fg_color="transparent",
                    hover_color="#2b3136",
                    text_color="#d8d8d8",
                    font=("Segoe UI", 11),
                    command=lambda name=spec.type: self.add_widget(name),
                )
                button.grid(
                    row=row + index // 2,
                    column=index % 2,
                    sticky="ew",
                    padx=(6, 2) if index % 2 == 0 else (2, 6),
                    pady=1,
                )
            row += (len(specs) + 1) // 2

        if row == 0:
            ctk.CTkLabel(
                body, text="No matching widgets", anchor="w", text_color="#8a8a8a"
            ).grid(row=0, column=0, columnspan=2, sticky="ew", padx=8, pady=8)

        self.palette_panel.refresh()

    def _build_canvas(self) -> None:
        frame = ctk.CTkFrame(self, corner_radius=0)
        frame.grid(row=1, column=1, sticky="nsew", padx=4, pady=4)
        frame.grid_rowconfigure(0, weight=1)
        frame.grid_columnconfigure(0, weight=1)

        self.canvas = tk.Canvas(
            frame, bg="#0a0a0a", highlightthickness=0, takefocus=1
        )
        self.canvas.grid(row=0, column=0, sticky="nsew")
        y_scroll = ctk.CTkScrollbar(frame, orientation="vertical", command=self.canvas.yview)
        y_scroll.grid(row=0, column=1, sticky="ns")
        x_scroll = ctk.CTkScrollbar(frame, orientation="horizontal", command=self.canvas.xview)
        x_scroll.grid(row=1, column=0, sticky="ew")
        self.canvas.configure(yscrollcommand=y_scroll.set, xscrollcommand=x_scroll.set)

        self.canvas.bind("<Button-1>", self.on_press)
        self.canvas.bind("<B1-Motion>", self.on_drag)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.canvas.bind("<MouseWheel>", self.on_preview_wheel)
        self.canvas.bind("<Delete>", lambda _event: self.delete_selected())
        self.canvas.bind("<Control-z>", lambda _event: self.undo())
        self.canvas.bind("<Control-d>", lambda _event: self.duplicate_selected())
        self.canvas.bind("<Left>", lambda _event: self.nudge(-self.grid_step(), 0))
        self.canvas.bind("<Right>", lambda _event: self.nudge(self.grid_step(), 0))
        self.canvas.bind("<Up>", lambda _event: self.nudge(0, -self.grid_step()))
        self.canvas.bind("<Down>", lambda _event: self.nudge(0, self.grid_step()))
        self.canvas.focus_set()

    def _build_right_panel(self) -> None:
        self.tabs = ctk.CTkTabview(self, width=380, command=self.on_tab_change)
        self.tabs.grid(row=1, column=2, sticky="nsew", padx=(4, 8), pady=(4, 4))
        self.tabs.add("Tree")
        self.tabs.add("Properties")
        self.tabs.add("Code")

        tree_tab = self.tabs.tab("Tree")
        tree_bar = ctk.CTkFrame(tree_tab, height=30, corner_radius=0)
        tree_bar.pack(fill="x", padx=3, pady=(3, 2))
        self.tree_search = ctk.CTkEntry(tree_bar, placeholder_text="Filter widgets", height=28)
        self.tree_search.pack(fill="x", padx=3, pady=2)
        self.tree_search.bind("<KeyRelease>", lambda _event: self.refresh_tree())
        self.tree_search.bind("<Escape>", self._clear_tree_search)

        self.tree_panel = ScrollPanel(tree_tab)
        self.tree_panel.pack(fill="both", expand=True, padx=3, pady=(0, 3))
        self.tree_frame = self.tree_panel.body

        self.properties_panel = ScrollPanel(self.tabs.tab("Properties"))
        self.properties_panel.pack(fill="both", expand=True, padx=3, pady=3)
        self.properties_frame = self.properties_panel.body

        code_tab = self.tabs.tab("Code")
        code_bar = ctk.CTkFrame(code_tab, height=32, corner_radius=0)
        code_bar.pack(fill="x")
        ctk.CTkButton(
            code_bar, text="Copy", width=56, height=26,
            command=self.copy_code,
        ).pack(side="left", padx=(4, 0), pady=3)
        ctk.CTkButton(
            code_bar, text="Generate", width=76, height=26,
            command=self.regenerate_code,
        ).pack(side="left", padx=(4, 0), pady=3)
        self.code_status = ctk.CTkLabel(code_bar, text="Generated", anchor="w")
        self.code_status.pack(side="left", padx=8)
        ctk.CTkLabel(
            code_tab,
            text="Preview follows the project. Manual code edits are exported as-is.",
            anchor="w",
            justify="left",
            wraplength=340,
            font=("Segoe UI", 10),
            text_color="#8a8a8a",
        ).pack(fill="x", padx=8, pady=(4, 2))
        self.code_text = LuaCodeEditor(code_tab, height=240)
        self.code_text.pack(fill="both", expand=True)
        self.code_text.bind("<KeyRelease>", self.mark_code_dirty, add="+")
        self.code_text.bind("<<Paste>>", self.mark_code_dirty, add="+")

    def _build_status(self) -> None:
        self.status = ctk.CTkLabel(self, text="", anchor="w")
        self.status.grid(row=2, column=0, columnspan=3, sticky="ew", padx=10, pady=(0, 6))

    # ---- project actions --------------------------------------------------

    def new_project(self) -> None:
        if not messagebox.askyesno("New project", "Discard the current project?"):
            return
        self.push_undo()
        self.project = Project.new()
        self.profile = find_profile(self.project.profile)
        self.project.theme = self.profile.default_theme()
        self.profile_menu.set(self.profile.name)
        self.selected_id = None
        self.preview_scroll = 0
        self.undo_stack.clear()
        self.code_dirty = False
        self.sync_theme_controls()
        self.refresh()

    def open_project(self) -> None:
        path = filedialog.askopenfilename(
            title="Open Lua GUI project",
            filetypes=[("Lua GUI project", "*.luadesign.json"), ("JSON", "*.json")],
        )
        if not path:
            return
        try:
            self.load_project(Path(path))
        except Exception as error:  # noqa: BLE001 - show the file error
            messagebox.showerror("Open failed", str(error))

    def load_project(self, path: Path) -> None:
        self.push_undo()
        self.project = Project.load(path)
        self.profile = find_profile(self.project.profile)
        self.profile_menu.set(self.profile.name)
        self.selected_id = None
        self.preview_scroll = 0
        self.code_dirty = False
        self.sync_theme_controls()
        self.refresh()
        self.set_status(f"Opened {path.name}")

    def save_project(self) -> None:
        path = filedialog.asksaveasfilename(
            title="Save Lua GUI project",
            defaultextension=".luadesign.json",
            filetypes=[("Lua GUI project", "*.luadesign.json"), ("JSON", "*.json")],
        )
        if not path:
            return
        try:
            self.project.save(Path(path))
            self.set_status(f"Saved {Path(path).name}")
        except Exception as error:  # noqa: BLE001
            messagebox.showerror("Save failed", str(error))

    def export_lua(self) -> None:
        path = filedialog.asksaveasfilename(
            title="Export Lua script",
            defaultextension=".lua",
            filetypes=[("Lua script", "*.lua")],
        )
        if not path:
            return
        try:
            code = self.code_text.get_code()
            if not code.strip():
                code = export(self.project)
            Path(path).write_text(code, encoding="utf-8")
            self.set_status(f"Exported {Path(path).name}")
        except Exception as error:  # noqa: BLE001
            messagebox.showerror("Export failed", str(error))

    def seed_demo(self) -> None:
        profile = self.project.profile
        theme = copy.deepcopy(self.project.theme)
        self.project = demo_project(theme)
        self.project.profile = profile
        self.project.theme = theme
        width, height = self.content_area()
        if self.project.out_of_bounds(width, height):
            self.project.scale_to((max(width, 1222), max(height, 540)), (width, height))

    # ---- undo -------------------------------------------------------------

    def push_undo(self) -> None:
        self.undo_stack.append(copy.deepcopy(self.project))
        if len(self.undo_stack) > 50:
            self.undo_stack.pop(0)

    def undo(self) -> None:
        if not self.undo_stack:
            return
        self.project = self.undo_stack.pop()
        self.profile = find_profile(self.project.profile)
        self.profile_menu.set(self.profile.name)
        self.selected_id = None
        self.sync_theme_controls()
        self.refresh()

    # ---- widget operations ------------------------------------------------

    def content_area(self) -> tuple[int, int]:
        return canvas_size(self.profile, self.theme)

    def layout_rects(self) -> dict[str, tuple[int, int, int, int]]:
        width, height = self.content_area()
        return self.project.layout_rects(width, height)

    def add_widget(self, type_name: str) -> None:
        parent = self.selected_id if self.project.is_container(self.selected_id) else None
        self.push_undo()
        self._suspend_properties = True
        widget = self.project.add_widget(type_name, parent)
        spec = SPEC_BY_TYPE.get(widget.type)
        if spec is not None:
            width, height = themed_default_size(spec, self.theme)
            if parent is None:
                avail_w, avail_h = self.content_area()
            else:
                parent_widget = self.project.find(parent)
                pad = content_offset(self.project, parent_widget) if parent_widget else 0
                avail_w = max(8, (parent_widget.w if parent_widget else width) - 2 * pad)
                avail_h = max(8, (parent_widget.h if parent_widget else height) - 2 * pad)
            widget.w = max(8, min(width, avail_w))
            widget.h = max(8, min(height, avail_h))
            widget.x = max(0, min(widget.x, max(0, avail_w - widget.w)))
            widget.y = max(0, min(widget.y, max(0, avail_h - widget.h)))
        self.selected_id = widget.id
        self._suspend_properties = False
        self.refresh()
        try:
            self.tabs.set("Properties")
        except Exception:  # noqa: BLE001
            pass

    def delete_selected(self) -> None:
        if not self.selected_id:
            return
        self.push_undo()
        self.project.remove_widget(self.selected_id)
        self.selected_id = None
        self.refresh()

    def duplicate_selected(self) -> None:
        if not self.selected_id:
            return
        self.push_undo()
        clone = self.project.duplicate_widget(self.selected_id)
        if clone is not None:
            self.selected_id = clone.id
        self.refresh()

    def nudge(self, dx: int, dy: int) -> None:
        widget = self.project.find(self.selected_id)
        if widget is None:
            return
        self.push_undo()
        widget.x += dx
        widget.y += dy
        self.refresh()

    # ---- selection and drag ----------------------------------------------

    def content_origin(self) -> tuple[float, float]:
        return canvas_origin(self.profile, self.theme, self.zoom)

    def canvas_point(self, event) -> tuple[float, float]:
        return self.canvas.canvasx(event.x), self.canvas.canvasy(event.y)

    def model_point(self, canvas_x: float, canvas_y: float) -> tuple[float, float]:
        ox, oy = self.content_origin()
        return (
            (canvas_x - ox) / self.zoom,
            (canvas_y - oy) / self.zoom + self.preview_scroll,
        )

    def selected_canvas_rect(self) -> tuple[float, float, float, float] | None:
        widget = self.project.find(self.selected_id)
        if widget is None:
            return None
        x, y, w, h = self.layout_rects().get(widget.id, self.project.abs_rect(widget))
        ox, oy = self.content_origin()
        return (
            ox + x * self.zoom,
            oy + (y - self.preview_scroll) * self.zoom,
            w * self.zoom,
            h * self.zoom,
        )

    def hit_test(self, mx: float, my: float) -> WidgetNode | None:
        rects = self.layout_rects()
        for widget in reversed(self.project.ordered()):
            x, y, w, h = rects.get(widget.id, self.project.abs_rect(widget))
            if x <= mx <= x + w and y <= my <= y + h:
                return widget
        return None

    def on_press(self, event) -> None:
        self.canvas.focus_set()
        cx, cy = self.canvas_point(event)
        mx, my = self.model_point(cx, cy)

        rect = self.selected_canvas_rect()
        handle = hit_handle(cx, cy, rect, self.zoom) if rect else None
        if handle:
            widget = self.project.find(self.selected_id)
            if widget is not None:
                self._drag = {
                    "mode": "resize",
                    "handle": handle,
                    "widget": widget.id,
                    "start_x": widget.x,
                    "start_y": widget.y,
                    "start_w": widget.w,
                    "start_h": widget.h,
                    "start_mx": mx,
                    "start_my": my,
                    "press_cx": event.x,
                    "press_cy": event.y,
                    "pre": copy.deepcopy(self.project),
                    "moved": False,
                }
                return

        widget = self.hit_test(mx, my)
        self.selected_id = widget.id if widget else None
        self.canvas.focus_set()
        if widget is not None:
            self._drag = {
                "mode": "move",
                "widget": widget.id,
                "start_x": widget.x,
                "start_y": widget.y,
                "start_w": widget.w,
                "start_h": widget.h,
                "start_mx": mx,
                "start_my": my,
                "press_cx": event.x,
                "press_cy": event.y,
                "pre": copy.deepcopy(self.project),
                "moved": False,
            }
        self.refresh(rebuild_properties=True)
        try:
            self.tabs.set("Properties")
        except Exception:  # noqa: BLE001
            pass

    def on_drag(self, event) -> None:
        if not self._drag:
            return
        if (
            not self._drag.get("moved")
            and abs(event.x - self._drag.get("press_cx", event.x)) < 3
            and abs(event.y - self._drag.get("press_cy", event.y)) < 3
        ):
            return
        cx, cy = self.canvas_point(event)
        mx, my = self.model_point(cx, cy)
        dx = mx - self._drag["start_mx"]
        dy = my - self._drag["start_my"]
        if self.snap:
            step = self.grid_step()
            dx = round(dx / step) * step
            dy = round(dy / step) * step
        else:
            dx, dy = round(dx), round(dy)

        widget = self.project.find(self._drag["widget"])
        if widget is None:
            return

        if self._drag["mode"] == "move":
            parent = self.project.find(widget.parent)
            if parent is not None and str(parent.props.get("layout", "NONE")) in ("FLEX", "GRID"):
                self.set_status(
                    "Flex and grid children are positioned by their parent; "
                    "drag the parent or use the tree order/cell fields"
                )
                return
            widget.x = self._drag["start_x"] + int(dx)
            widget.y = self._drag["start_y"] + int(dy)
        else:
            x = self._drag["start_x"]
            y = self._drag["start_y"]
            w = self._drag["start_w"]
            h = self._drag["start_h"]
            handle = self._drag["handle"]
            if "w" in handle:
                x += int(dx)
                w -= int(dx)
            if "e" in handle:
                w += int(dx)
            if "n" in handle:
                y += int(dy)
                h -= int(dy)
            if "s" in handle:
                h += int(dy)
            widget.x, widget.y = x, y
            widget.w, widget.h = max(8, w), max(8, h)
            if int(widget.props.get("w_pct", 0) or 0) > 0 or int(widget.props.get("h_pct", 0) or 0) > 0:
                parent = self.project.find(widget.parent)
                if parent is not None:
                    px, py, pw, ph = self.layout_rects().get(
                        parent.id, self.project.abs_rect(parent)
                    )
                    pad = content_offset(self.project, parent)
                    avail_w, avail_h = max(1, pw - 2 * pad), max(1, ph - 2 * pad)
                else:
                    avail_w, avail_h = self.content_area()
                if int(widget.props.get("w_pct", 0) or 0) > 0:
                    widget.props["w_pct"] = max(1, min(100, round(widget.w / avail_w * 100)))
                if int(widget.props.get("h_pct", 0) or 0) > 0:
                    widget.props["h_pct"] = max(1, min(100, round(widget.h / avail_h * 100)))

        self._drag["moved"] = True
        self.render_only()

    def on_release(self, _event) -> None:
        if self._drag:
            if self._drag.get("moved"):
                self.undo_stack.append(self._drag["pre"])
                if len(self.undo_stack) > 50:
                    self.undo_stack.pop(0)
            self._drag = None
            self.refresh(rebuild_properties=True)

    def grid_step(self) -> int:
        return 8

    # ---- theme and profile ------------------------------------------------

    def on_profile(self, name: str) -> None:
        for profile in PROFILES:
            if profile.name != name:
                continue
            if profile.id == self.profile.id:
                return

            self.push_undo()
            old_theme = self.theme
            old_size = self.content_area()
            bars = (
                self.project.theme.bars_mode,
                self.project.theme.bars_portrait,
                self.project.theme.bars_landscape,
                self.project.theme.bars_align,
            )
            self.profile = profile
            self.project.profile = profile.id
            # A different simulator profile also clears the appearance overrides.
            self.project.theme = profile.default_theme()
            (
                self.project.theme.bars_mode,
                self.project.theme.bars_portrait,
                self.project.theme.bars_landscape,
                self.project.theme.bars_align,
            ) = bars
            self.sync_theme_controls()
            self.theme = MaterialTheme(self.project.theme, self.profile.dpi)
            self.project.apply_theme_paddings(old_theme, self.theme)
            new_size = self.content_area()

            if self.project.widgets and old_size != new_size:
                factor = min(new_size[0] / old_size[0], new_size[1] / old_size[1])
                scale = messagebox.askyesno(
                    "Profile changed",
                    "Scale the widgets for the new canvas?\n\n"
                    f"Old canvas: {old_size[0]} x {old_size[1]} px\n"
                    f"New canvas: {new_size[0]} x {new_size[1]} px\n"
                    f"Uniform scale: {factor:.2f}x",
                )
                if scale:
                    self.project.scale_to(old_size, new_size)

            self.selected_id = None
            self.preview_scroll = 0
            self.refresh()
            return

    def sync_theme_controls(self) -> None:
        theme = self.project.theme
        self.theme_menu.set("Dark" if theme.dark else "Light")
        self.mono_switch.select() if theme.mono else self.mono_switch.deselect()
        if self.profile.mono:
            self.project.theme.mono = True
            self.mono_switch.select()
            self.mono_switch.configure(state="disabled")
        else:
            self.mono_switch.configure(state="normal")
        self.spacing_slider.set(theme.spacing)
        self.size_slider.set(theme.size)
        self.font_slider.set(theme.font_size)
        self._sync_bars_controls()
        self._update_swatches()
        self._update_swatch_state()

    def _sync_bars_controls(self) -> None:
        theme = self.project.theme
        self.bars_mode.set("Custom" if theme.bars_mode == "custom" else "Auto")
        self.portrait_layout.set("Side" if theme.bars_portrait == "side" else "Split")
        self.landscape_layout.set("Side" if theme.bars_landscape == "side" else "Split")
        self.bars_align.set("Right" if theme.bars_align == "right" else "Left")
        state = "normal" if theme.bars_mode == "custom" else "disabled"
        self.portrait_layout.configure(state=state)
        self.landscape_layout.configure(state=state)

    def on_bars_mode(self, value: str) -> None:
        self.project.theme.bars_mode = value.lower()
        self._sync_bars_controls()
        self.apply_theme()

    def on_bars_layout(self, value: str, orientation: str) -> None:
        setattr(self.project.theme, f"bars_{orientation}", value.lower())
        self.apply_theme()

    def on_bars_align(self, value: str) -> None:
        self.project.theme.bars_align = value.lower()
        self.apply_theme()

    def on_theme_mode(self, value: str) -> None:
        self.project.theme.dark = value == "Dark"
        self.apply_theme()

    def on_mono(self, *_args) -> None:
        def apply_mono() -> None:
            if self.profile.mono:
                # Greyscale displays always use the mono theme.
                self.mono_switch.select()
                self.project.theme.mono = True
            else:
                self.project.theme.mono = bool(self.mono_switch.get())

        self._change_theme(apply_mono)

    def on_font_size(self, value: float) -> None:
        self.project.theme.font_size = int(value)
        self.apply_theme()

    def _change_theme(self, mutate) -> None:
        """Apply a theme edit and move theme-derived container paddings with it."""
        # Snapshot the settings: MaterialTheme holds a reference, and mutating the
        # project theme in place would also change the "old" theme's tokens.
        old_theme = MaterialTheme(copy.deepcopy(self.project.theme), self.profile.dpi)
        mutate()
        self.theme = MaterialTheme(self.project.theme, self.profile.dpi)
        self.project.apply_theme_paddings(old_theme, self.theme)
        self.apply_theme()

    def on_spacing(self, value: float) -> None:
        self._change_theme(lambda: setattr(self.project.theme, "spacing", int(value)))

    def on_size(self, value: float) -> None:
        self._change_theme(lambda: setattr(self.project.theme, "size", int(value)))

    def reset_theme_defaults(self) -> None:
        self.push_undo()
        bars = (
            self.project.theme.bars_mode,
            self.project.theme.bars_portrait,
            self.project.theme.bars_landscape,
            self.project.theme.bars_align,
        )

        def restore_defaults() -> None:
            self.project.theme = self.profile.default_theme()
            (
                self.project.theme.bars_mode,
                self.project.theme.bars_portrait,
                self.project.theme.bars_landscape,
                self.project.theme.bars_align,
            ) = bars

        self._change_theme(restore_defaults)
        self.sync_theme_controls()

    def on_zoom(self, value: str) -> None:
        self.zoom = int(value.rstrip("%")) / 100
        self.render_only()
        self.schedule_save_settings()

    def toggle_grid(self, *_args) -> None:
        self.show_grid = bool(self.grid_switch.get())
        self.render_only()
        self.schedule_save_settings()

    def toggle_snap(self, *_args) -> None:
        self.snap = bool(self.snap_switch.get())
        self.schedule_save_settings()

    def pick_color(self, name: str) -> None:
        if self.project.theme.mono:
            return
        current = getattr(self.project.theme, name)
        if name == "surface" and not current:
            current = self.theme.surface_container_low
        chosen = colorchooser.askcolor(color=current, title=f"{name.title()} colour")
        if chosen and chosen[1]:
            setattr(self.project.theme, name, chosen[1].lower())
            self._update_swatches()
            self.apply_theme()
            self.set_status(self._color_hint(name))

    def clear_surface(self) -> None:
        if self.project.theme.mono or not self.project.theme.surface:
            return
        self.push_undo()
        self.project.theme.surface = ""
        self._update_swatches()
        self.apply_theme()
        self.set_status("Surface follows the theme again")

    def _color_hint(self, name: str) -> str:
        return {
            "primary": "Primary: buttons, slider active, spinner, checkbox",
            "secondary": "Secondary: selected chips, bars, badges, step progress",
            "error": "Error: out-of-bounds warnings and error states",
            "surface": "Surface: app background and derived container tones",
        }.get(name, name)

    def _update_swatches(self) -> None:
        for key, square in self.swatches.items():
            label = self.swatch_labels[key]
            if key == "surface":
                custom = bool(self.project.theme.surface)
                color = self.project.theme.surface or (
                    self.theme.paper if self.project.theme.mono else self.theme.surface_container_low
                )
                square.configure(fg_color=color)
                label.configure(text="Surface*" if custom else "Surface")
            else:
                square.configure(fg_color=getattr(self.project.theme, key))
                label.configure(text=key.title())

    def _update_swatch_state(self) -> None:
        if self.project.theme.mono:
            for square in self.swatches.values():
                square.configure(fg_color=self.theme.surface_container)
            for label in self.swatch_labels.values():
                label.configure(text_color=self.theme.text_muted)
            return
        self._update_swatches()
        for label in self.swatch_labels.values():
            label.configure(text_color=self.theme.text)

    def apply_theme(self) -> None:
        self.theme = MaterialTheme(self.project.theme, self.profile.dpi)
        self._update_swatch_state()
        self.render_only()
        self.refresh_code()
        self.schedule_save_settings()

    # ---- refresh ----------------------------------------------------------

    def render_only(self) -> None:
        width, height = self.content_area()
        max_scroll = max(0, self.project.page_height(width, height) - height)
        self.preview_scroll = max(0, min(self.preview_scroll, max_scroll))
        render(
            self.canvas,
            self.project,
            self.profile,
            self.theme,
            self.zoom,
            self.selected_id,
            self.show_grid,
            self.preview_scroll,
        )
        self.set_status()

    def on_preview_wheel(self, event) -> str | None:
        """Scroll the designed page inside the canvas, not the window image."""
        width, height = self.content_area()
        page = self.project.page_height(width, height)
        if page <= height or event.delta == 0:
            return None
        if abs(event.delta) >= 120:
            step = -int(event.delta / 120) * 48
        else:
            step = -48 if event.delta > 0 else 48
        self.preview_scroll = max(0, min(self.preview_scroll + step, page - height))
        self.render_only()
        return "break"

    def refresh(self, rebuild_properties: bool = True) -> None:
        self.theme = MaterialTheme(self.project.theme, self.profile.dpi)
        self.render_only()
        self.refresh_tree()
        if rebuild_properties and not self._suspend_properties:
            self.refresh_properties()
        self.refresh_code()
        self.schedule_save_settings()

    def _clear_tree_search(self, _event=None) -> None:
        self.tree_search.delete(0, "end")
        self.refresh_tree()

    def refresh_tree(self) -> None:
        for child in self.tree_frame.winfo_children():
            child.destroy()
        self._tree_rows: dict[str | None, ctk.CTkButton] = {}

        query = self.tree_search.get().strip().lower() if hasattr(self, "tree_search") else ""

        # A canvas selection should be visible in the tree even if its parent was collapsed.
        selected = self.project.find(self.selected_id)
        while selected is not None and selected.parent is not None:
            self._tree_collapsed.discard(selected.parent)
            selected = self.project.find(selected.parent)

        def row_text(widget: WidgetNode) -> str:
            spec = SPEC_BY_TYPE.get(widget.type)
            return f"{spec.title if spec else widget.type}  {widget.id}"

        def matches(widget: WidgetNode) -> bool:
            if not query:
                return True
            spec = SPEC_BY_TYPE.get(widget.type)
            haystack = f"{spec.title if spec else widget.type} {widget.id}".lower()
            return query in haystack

        def subtree_matches(widget: WidgetNode) -> bool:
            if matches(widget):
                return True
            return any(subtree_matches(child) for child in self.project.children(widget.id))

        def add_row(text: str, widget_id: str | None, indent: int, has_children: bool, collapsed: bool) -> None:
            row_frame = ctk.CTkFrame(self.tree_frame, fg_color="transparent", height=28)
            row_frame.pack(fill="x", pady=1)
            row_frame.pack_propagate(False)

            if has_children and widget_id is not None:
                arrow = ctk.CTkButton(
                    row_frame,
                    text="▸" if collapsed else "▾",
                    width=22,
                    height=24,
                    corner_radius=4,
                    fg_color="transparent",
                    hover_color="#2b3136",
                    text_color="#9a9a9a",
                    font=("Segoe UI", 11),
                    command=lambda wid=widget_id: self.toggle_tree(wid),
                )
            else:
                arrow = ctk.CTkLabel(row_frame, text="", width=22)
            arrow.pack(side="left", padx=(indent * 12, 0))

            button = ctk.CTkButton(
                row_frame,
                text=text,
                anchor="w",
                height=26,
                fg_color="transparent" if widget_id != self.selected_id else "#2a5d7a",
                hover_color="#22313a",
                command=lambda wid=widget_id: self._tree_click(wid),
            )
            button.pack(side="left", fill="x", expand=True)
            self._tree_rows[widget_id] = row_frame
            button.bind(
                "<ButtonPress-1>",
                lambda event, wid=widget_id: self._tree_press(wid, event),
                add="+",
            )
            button.bind("<B1-Motion>", self._tree_motion, add="+")
            button.bind(
                "<ButtonRelease-1>",
                lambda event, wid=widget_id: self._tree_release(wid, event),
                add="+",
            )
            button.bind(
                "<Button-3>",
                lambda event, wid=widget_id: self._tree_menu(wid, event),
            )

        add_row("Canvas", None, 0, bool(self.project.children(None)), False)

        def walk(parent: str | None, depth: int, force: bool) -> None:
            for widget in self.project.children(parent):
                widget_matches = matches(widget)
                if query and not force and not widget_matches and not subtree_matches(widget):
                    continue
                children = self.project.children(widget.id)
                collapsed = bool(children) and widget.id in self._tree_collapsed and not query
                add_row(row_text(widget), widget.id, depth, bool(children), collapsed)
                if not collapsed:
                    walk(widget.id, depth + 1, force or widget_matches)

        walk(None, 1, False)
        self._tree_drag_id: str | None = None
        self._tree_drag_moved = False
        self.tree_panel.refresh()

    def toggle_tree(self, widget_id: str) -> None:
        if widget_id in self._tree_collapsed:
            self._tree_collapsed.discard(widget_id)
        else:
            self._tree_collapsed.add(widget_id)
        self.refresh_tree()

    def _tree_click(self, widget_id: str | None) -> None:
        self.select(widget_id)

    def _tree_press(self, widget_id: str | None, event) -> None:
        self._tree_drag_id = widget_id
        self._tree_drag_y = event.y_root
        self._tree_drag_moved = False

    def _tree_motion(self, event) -> None:
        if abs(event.y_root - getattr(self, "_tree_drag_y", event.y_root)) > 6:
            self._tree_drag_moved = True

    def _tree_release(self, source_id: str | None, event) -> None:
        moved = self._tree_drag_moved
        self._tree_drag_moved = False
        self._tree_drag_id = None
        if not moved or source_id is None:
            return

        pointer_y = self.winfo_pointery()
        target: str | None | object = _NO_TARGET
        for widget_id, row in self._tree_rows.items():
            top = row.winfo_rooty()
            if top <= pointer_y < top + row.winfo_height():
                target = widget_id
                break

        if target is _NO_TARGET or target is None:
            parent, after = None, None
        elif target == source_id:
            return
        else:
            anchor = self.project.find(str(target))
            if anchor is None:
                return
            spec = SPEC_BY_TYPE.get(anchor.type)
            if spec is not None and spec.container:
                parent, after = anchor.id, None
            else:
                parent, after = anchor.parent, anchor.id

        self.push_undo()
        if not self.project.move_widget(source_id, parent, after):
            self.undo_stack.pop()
            return
        self.selected_id = source_id
        self.refresh()
        self.set_status("Moved widget")

    def _tree_menu(self, widget_id: str | None, event) -> None:
        if widget_id is None:
            return
        self.select(widget_id)
        menu = tk.Menu(self, tearoff=0)
        menu.add_command(label="Rename...", command=lambda: self.rename_widget(widget_id))
        menu.add_command(label="Duplicate", command=self.duplicate_selected)
        menu.add_command(label="Delete", command=self.delete_selected)
        menu.add_separator()
        menu.add_command(
            label="Move to Canvas",
            command=lambda: self.move_tree_widget(widget_id, None, None),
        )
        widget = self.project.find(widget_id)
        if widget is not None and widget.parent is not None:
            parent = self.project.find(widget.parent)
            menu.add_command(
                label="Move Out One Level",
                command=lambda: self.move_tree_widget(
                    widget_id,
                    parent.parent if parent else None,
                    widget.parent,
                ),
            )
        menu.add_separator()
        menu.add_command(label="Bring to Front", command=lambda: self.z_order(widget_id, True))
        menu.add_command(label="Send to Back", command=lambda: self.z_order(widget_id, False))
        try:
            menu.tk_popup(event.x_root, event.y_root)
        finally:
            menu.grab_release()

    def move_tree_widget(
        self,
        widget_id: str,
        parent: str | None,
        after: str | None,
    ) -> None:
        self.push_undo()
        if not self.project.move_widget(widget_id, parent, after):
            self.undo_stack.pop()
            return
        self.selected_id = widget_id
        self.refresh()
        self.set_status("Moved widget")

    def z_order(self, widget_id: str, front: bool) -> None:
        self.push_undo()
        changed = (
            self.project.move_to_end(widget_id)
            if front
            else self.project.move_to_start(widget_id)
        )
        if not changed:
            self.undo_stack.pop()
            return
        self.refresh()

    def rename_widget(self, widget_id: str) -> None:
        widget = self.project.find(widget_id)
        if widget is None:
            return
        name = simpledialog.askstring(
            "Rename widget",
            "Widget id:",
            initialvalue=widget.id,
            parent=self,
        )
        if not name or name == widget.id:
            return
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) is None:
            messagebox.showerror("Rename failed", "Use letters, digits and underscores, starting with a letter or underscore.")
            return
        if not self.rename_widget_to(widget_id, name):
            messagebox.showerror("Rename failed", f"'{name}' is not available.")

    def rename_widget_to(self, widget_id: str, name: str) -> bool:
        """Renames after validation; returns false when the name is taken."""
        widget = self.project.find(widget_id)
        if widget is None or name == widget.id:
            return False
        self.push_undo()
        if not self.project.rename_widget(widget_id, name):
            self.undo_stack.pop()
            return False
        if self.selected_id == widget_id:
            self.selected_id = name
        self.refresh()
        return True

    def apply_widget_name(self, widget_id: str, field: ctk.CTkEntry) -> None:
        widget = self.project.find(widget_id)
        if widget is None:
            return
        name = field.get().strip()
        if name == widget.id:
            return
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) is None:
            field.delete(0, "end")
            field.insert(0, widget.id)
            self.set_status("Widget names use letters, digits and underscores.")
            return
        if not self.rename_widget_to(widget_id, name):
            field.delete(0, "end")
            field.insert(0, widget.id)
            self.set_status(f"'{name}' is not available.")

    def open_icon_picker(self, on_pick=None) -> None:
        if on_pick is None:
            on_pick = self._copy_icon_name
        IconPicker(self, on_pick)

    def _copy_icon_name(self, name: str) -> None:
        text = f"lvgl.ICON.{name}"
        self.clipboard_clear()
        self.clipboard_append(text)
        self.set_status(f"Copied {text}")

    def _insert_tab_icon(self, box: ctk.CTkTextbox, widget_id: str, key: str, name: str) -> None:
        box.insert("insert", f"{name}|")
        self.apply_prop_later(widget_id, key, box.get("1.0", "end-1c"))

    def apply_text_icon(self, widget_id: str, field: ctk.CTkEntry, name: str) -> None:
        widget = self.project.find(widget_id)
        if widget is None:
            return
        value = f"lvgl.ICON.{name}"
        self.push_undo()
        widget.props["text"] = value
        widget.props["text_font"] = "icon"
        self.render_only()
        self.refresh_code()
        self.refresh_properties()
        self.set_status(f"Set {value} with the icon font")

    def select(self, widget_id: str | None) -> None:
        self.selected_id = widget_id
        self.refresh()

    def refresh_properties(self) -> None:
        for child in self.properties_frame.winfo_children():
            child.destroy()

        widget = self.project.find(self.selected_id)
        if widget is None:
            ctk.CTkLabel(
                self.properties_frame,
                text="Select a widget, or click a palette item to add one.",
                wraplength=330,
                justify="left",
            ).pack(anchor="w", padx=8, pady=12)
            return

        spec = SPEC_BY_TYPE.get(widget.type) or SPEC_BY_TYPE["Object"]
        header = ctk.CTkFrame(self.properties_frame, fg_color="transparent")
        header.pack(fill="x", padx=4, pady=(6, 4))
        ctk.CTkLabel(
            header, text=f"{spec.title}  ({widget.id})", font=("Segoe UI", 14, "bold")
        ).pack(side="left")
        ctk.CTkButton(
            header, text="Delete", width=60, height=24, fg_color="#8b2f3b",
            command=self.delete_selected,
        ).pack(side="right")
        ctk.CTkButton(
            header, text="Clone", width=55, height=24, command=self.duplicate_selected
        ).pack(side="right", padx=4)

        name_row = ctk.CTkFrame(self.properties_frame, fg_color="transparent")
        name_row.pack(fill="x", padx=4, pady=(2, 2))
        ctk.CTkLabel(name_row, text="Name", width=44, anchor="w").pack(side="left", padx=(4, 2))
        name_field = ctk.CTkEntry(name_row)
        name_field.pack(side="left", fill="x", expand=True, padx=(0, 4))
        name_field.insert(0, widget.id)
        name_field.bind(
            "<Return>",
            lambda _event, wid=widget.id, f=name_field: self.apply_widget_name(wid, f),
        )
        name_field.bind(
            "<FocusOut>",
            lambda _event, wid=widget.id, f=name_field: self.apply_widget_name(wid, f),
        )

        section = ctk.CTkLabel(self.properties_frame, text="Placement", anchor="w")
        section.pack(fill="x", padx=8, pady=(8, 2))

        geometry = ctk.CTkFrame(self.properties_frame, fg_color="transparent")
        geometry.pack(fill="x", padx=4)
        geometry_fields: dict[str, ctk.CTkEntry] = {}
        for index, key in enumerate(("x", "y", "w", "h")):
            field = ctk.CTkEntry(geometry, width=78)
            field.grid(row=index // 2, column=index % 2, padx=3, pady=2)
            field.insert(0, str(getattr(widget, key)))
            field.bind(
                "<Return>",
                lambda _event, wid=widget.id, k=key, f=field: self.apply_geometry(wid, k, f),
            )
            field.bind(
                "<FocusOut>",
                lambda _event, wid=widget.id, k=key, f=field: self.apply_geometry(wid, k, f),
            )
            ctk.CTkLabel(geometry, text=key.upper(), width=16).grid(
                row=index // 2, column=2 + index % 2 * 2, sticky="w"
            )
            geometry_fields[key] = field

        self._geometry_fields = geometry_fields
        self._geometry_owner = widget.id
        for key, prop in (("w", "w_content"), ("h", "h_content")):
            if widget.props.get(prop):
                geometry_fields[key].delete(0, "end")
                geometry_fields[key].insert(0, "content")
                geometry_fields[key].configure(state="disabled")

        parent_values = ["Canvas"] + [
            f"{candidate.id}" for candidate in self.project.widgets if self.project.is_container(candidate.id)
        ]
        parent_menu = ctk.CTkOptionMenu(
            self.properties_frame,
            values=parent_values,
            command=lambda value: self.apply_parent(widget.id, value),
        )
        parent_menu.set(widget.parent or "Canvas")
        ctk.CTkLabel(self.properties_frame, text="Parent", anchor="w").pack(fill="x", padx=8, pady=(8, 2))
        parent_menu.pack(fill="x", padx=8)

        parent = self.project.find(widget.parent)
        if parent is not None and parent.type in ("Tabview", "Tileview"):
            page_key = "tab" if parent.type == "Tabview" else "tile"
            source_key = "tabs" if parent.type == "Tabview" else "tiles"
            active_key = "active_tab" if parent.type == "Tabview" else "active_tile"
            pages = [
                line for line in str((parent.props or {}).get(source_key, "")).splitlines() if line.strip()
            ]
            page_count = max(1, len(pages))
            try:
                active_page = int((parent.props or {}).get(active_key, 1) or 1)
            except (TypeError, ValueError):
                active_page = 1
            try:
                page_value = int((widget.props or {}).get(page_key, active_page) or active_page)
            except (TypeError, ValueError):
                page_value = active_page
            page_value = max(1, min(page_count, page_value))
            page_row = ctk.CTkFrame(self.properties_frame, fg_color="transparent")
            page_row.pack(fill="x", padx=8, pady=2)
            ctk.CTkLabel(
                page_row,
                text="Tab" if parent.type == "Tabview" else "Tile",
                width=120,
                anchor="w",
            ).pack(side="left")
            page_menu = ctk.CTkOptionMenu(
                page_row,
                values=[str(index) for index in range(1, page_count + 1)],
                width=130,
                command=lambda choice, wid=widget.id, key=page_key: self.apply_prop(
                    wid, key, int(choice)
                ),
            )
            page_menu.set(str(page_value))
            page_menu.pack(side="right")

        parent_layout = str((parent.props or {}).get("layout", "NONE") or "NONE") if parent else "NONE"
        widget_layout = str((widget.props or {}).get("layout", "NONE") or "NONE")
        parent_props = (parent.props or {}) if parent is not None else {}
        parent_flex_row = str(parent_props.get("flex_flow", "ROW") or "ROW").startswith("ROW")
        flex_main_content = bool(
            parent_props.get("w_content" if parent_flex_row else "h_content")
        )

        grid_cell_keys = {
            "grid_col", "grid_row", "grid_col_span", "grid_row_span",
            "cell_col_align", "cell_row_align",
        }
        grid_container_keys = {
            "grid_columns", "grid_rows", "grid_col_align", "grid_row_align",
        }

        current_category = None
        for prop in spec.properties:
            if prop.is_geometry:
                continue
            if prop.key in grid_cell_keys and parent_layout != "GRID":
                continue
            if prop.key in grid_container_keys and widget_layout != "GRID":
                continue
            if prop.key in ("flex_flow", "flex_main", "flex_cross", "flex_track") and widget_layout != "FLEX":
                continue
            if prop.key == "flex_grow" and (parent_layout != "FLEX" or flex_main_content):
                continue
            if prop.key in ("pad_row", "pad_column") and widget_layout not in ("FLEX", "GRID"):
                continue
            if prop.key in grid_cell_keys and current_category != "Grid cell":
                ctk.CTkLabel(
                    self.properties_frame, text="Grid cell", anchor="w",
                    font=("Segoe UI", 12, "bold"),
                ).pack(fill="x", padx=8, pady=(12, 2))
                current_category = "Grid cell"
            elif prop.key in grid_container_keys and current_category != "Grid":
                ctk.CTkLabel(
                    self.properties_frame, text="Grid", anchor="w",
                    font=("Segoe UI", 12, "bold"),
                ).pack(fill="x", padx=8, pady=(12, 2))
                current_category = "Grid"
            if prop.kind == "multiline":
                if current_category != "Content":
                    ctk.CTkLabel(
                        self.properties_frame, text="Content", anchor="w",
                        font=("Segoe UI", 12, "bold"),
                    ).pack(fill="x", padx=8, pady=(12, 2))
                    current_category = "Content"
                if widget.type == "Tabs":
                    box_row = ctk.CTkFrame(self.properties_frame, fg_color="transparent")
                    box_row.pack(fill="x", padx=8, pady=(4, 1))
                    ctk.CTkLabel(box_row, text=prop.label, anchor="w").pack(side="left")
                    box = ctk.CTkTextbox(self.properties_frame, height=76)
                    ctk.CTkButton(
                        box_row,
                        text="Icon...",
                        width=54,
                        height=24,
                        command=lambda b=box, wid=widget.id, key=prop.key: self.open_icon_picker(
                            lambda name, b=b, wid=wid, key=key: self._insert_tab_icon(b, wid, key, name)
                        ),
                    ).pack(side="right")
                    box.pack(fill="x", padx=8)
                else:
                    ctk.CTkLabel(self.properties_frame, text=prop.label, anchor="w").pack(
                        fill="x", padx=8, pady=(4, 1)
                    )
                    box = ctk.CTkTextbox(self.properties_frame, height=76)
                    box.pack(fill="x", padx=8)
                box.insert("1.0", str(widget.props.get(prop.key, prop.default)))
                box.bind(
                    "<KeyRelease>",
                    lambda _event, p=prop, b=box: self.apply_prop_later(widget.id, p.key, b.get("1.0", "end-1c")),
                )
                continue

            row = ctk.CTkFrame(self.properties_frame, fg_color="transparent")
            row.pack(fill="x", padx=8, pady=2)
            ctk.CTkLabel(row, text=prop.label, width=120, anchor="w").pack(side="left")
            value = widget.props.get(prop.key, prop.default)

            if prop.kind == "bool":
                control = ctk.CTkSwitch(row, text="")
                control.pack(side="right")
                control.select() if value else control.deselect()
                control.configure(
                    command=lambda _value=None, p=prop, c=control: self.apply_prop(
                        widget.id, p.key, bool(c.get())
                    )
                )
            elif prop.kind == "choice":
                control = ctk.CTkOptionMenu(
                    row,
                    values=list(prop.choices),
                    width=130,
                    command=lambda choice, p=prop: self.apply_prop(widget.id, p.key, choice),
                )
                control.set(str(value))
                control.pack(side="right")
            elif prop.kind == "color":
                holder = ctk.CTkFrame(row, fg_color="transparent")
                holder.pack(side="right")
                entry = ctk.CTkEntry(holder, width=88)
                entry.pack(side="left")
                entry.insert(0, str(value))
                entry.bind(
                    "<Return>",
                    lambda _event, p=prop, e=entry: self.apply_prop(widget.id, p.key, e.get()),
                )
                entry.bind(
                    "<FocusOut>",
                    lambda _event, p=prop, e=entry: self.apply_prop(widget.id, p.key, e.get()),
                )
                ctk.CTkButton(
                    holder, text="...", width=28,
                    command=lambda p=prop, e=entry: self.pick_prop_color(widget.id, p.key, e),
                ).pack(side="left", padx=2)
            elif prop.kind == "text" and prop.key == "text" and widget.type in ICON_TEXT_TYPES:
                holder = ctk.CTkFrame(row, fg_color="transparent")
                holder.pack(side="right")
                control = ctk.CTkEntry(holder, width=96)
                control.pack(side="left")
                control.insert(0, "" if value is None else str(value))
                control.bind(
                    "<KeyRelease>",
                    lambda _event, p=prop, c=control: self.apply_prop_later(widget.id, p.key, c.get()),
                )
                ctk.CTkButton(
                    holder,
                    text="Icon...",
                    width=54,
                    height=26,
                    command=lambda c=control, wid=widget.id: self.open_icon_picker(
                        lambda name, c=c, wid=wid: self.apply_text_icon(wid, c, name)
                    ),
                ).pack(side="left", padx=(2, 0))
            else:
                if prop.kind == "int" and prop.optional:
                    holder = ctk.CTkFrame(row, fg_color="transparent")
                    holder.pack(side="right")
                    control = ctk.CTkEntry(holder, width=96)
                    control.pack(side="left")
                    control.insert(0, "" if value is None else str(value))
                    control.bind(
                        "<Return>",
                        lambda _event, p=prop, c=control: self.apply_int_prop(widget.id, p.key, c, p.optional),
                    )
                    control.bind(
                        "<FocusOut>",
                        lambda _event, p=prop, c=control: self.apply_int_prop(widget.id, p.key, c, p.optional),
                    )
                    ctk.CTkButton(
                        holder, text="Theme", width=56, height=26,
                        command=lambda p=prop: self.apply_prop(widget.id, p.key, None),
                    ).pack(side="left", padx=(2, 0))
                else:
                    control = ctk.CTkEntry(row, width=130)
                    control.pack(side="right")
                    control.insert(0, "" if value is None else str(value))
                    if prop.kind == "int":
                        control.bind(
                            "<Return>",
                            lambda _event, p=prop, c=control: self.apply_int_prop(
                                widget.id, p.key, c, p.optional
                            ),
                        )
                        control.bind(
                            "<FocusOut>",
                            lambda _event, p=prop, c=control: self.apply_int_prop(
                                widget.id, p.key, c, p.optional
                            ),
                        )
                    else:
                        control.bind(
                            "<KeyRelease>",
                            lambda _event, p=prop, c=control: self.apply_prop_later(widget.id, p.key, c.get()),
                        )

        event_names = list(spec.events)
        for name in widget.events:
            if name not in event_names:
                event_names.append(name)
        if event_names:
            ctk.CTkLabel(
                self.properties_frame, text="Events", anchor="w",
                font=("Segoe UI", 12, "bold"),
            ).pack(fill="x", padx=8, pady=(14, 2))
            for event_name in event_names:
                ctk.CTkLabel(
                    self.properties_frame, text=event_name, anchor="w"
                ).pack(fill="x", padx=8, pady=(4, 1))
                editor = LuaCodeEditor(self.properties_frame, height=64)
                editor.pack(fill="x", padx=8)
                editor.set_code(widget.events.get(event_name, ""))
                editor.bind(
                    "<FocusOut>",
                    lambda _event, name=event_name, e=editor, wid=widget.id: self.apply_event(
                        wid, name, e.get_code()
                    ),
                )

        self.properties_panel.refresh()

    def apply_geometry(self, widget_id: str, key: str, field: ctk.CTkEntry) -> None:
        # Bind the owning widget id: clicking another widget rebuilds this panel and
        # makes the old field lose focus. Using `selected_id` here applied the old
        # widget's x/y/w/h to the newly selected one.
        widget = self.project.find(widget_id)
        if widget is None:
            return
        try:
            value = int(field.get())
        except (ValueError, tk.TclError):
            return
        if getattr(widget, key) == value:
            return
        self.push_undo()
        setattr(widget, key, max(8, value) if key in ("w", "h") else value)
        self.render_only()
        self.refresh_code()

    def apply_int_prop(
        self,
        widget_id: str,
        key: str,
        field: ctk.CTkEntry,
        optional: bool = False,
    ) -> None:
        try:
            text = field.get().strip()
        except tk.TclError:
            return
        if text == "" and optional:
            self.apply_prop(widget_id, key, None)
            return
        try:
            value = int(text)
        except ValueError:
            return
        self.apply_prop(widget_id, key, value)

    def apply_prop(self, widget_id: str, key: str, value) -> None:
        widget = self.project.find(widget_id)
        if widget is None:
            return
        if value is None:
            if key not in widget.props:
                return
            self.push_undo()
            widget.props.pop(key, None)
            self.render_only()
            self.refresh_code()
            self.refresh_properties()
            return
        if widget.props.get(key) == value:
            return
        self.push_undo()
        widget.props[key] = value
        if key == "layout" and value == "GRID":
            self.project.ensure_grid_layout(widget.id)
        self.render_only()
        self.refresh_code()
        if key in ("w_content", "h_content"):
            self.sync_content_geometry(widget)
        if key == "layout":
            # Grid controls depend on the layout. Rebuild after the current Tk callback,
            # or the menu that triggered it is destroyed while Tk is still dispatching.
            self.after_idle(self.refresh_properties)

    def sync_content_geometry(self, widget: WidgetNode) -> None:
        """Update the W/H fields when a content-size switch changes."""
        if self._geometry_owner != widget.id:
            return

        for key, prop in (("w", "w_content"), ("h", "h_content")):
            field = self._geometry_fields.get(key)
            if field is None:
                continue
            field.configure(state="normal")
            field.delete(0, "end")
            if widget.props.get(prop):
                field.insert(0, "content")
                field.configure(state="disabled")
            else:
                field.insert(0, str(getattr(widget, key)))

    def apply_prop_later(self, widget_id: str, key: str, value) -> None:
        if self._code_job is not None:
            self.after_cancel(self._code_job)
        self._code_job = self.after(300, lambda: self.apply_prop(widget_id, key, value))

    def apply_event(self, widget_id: str, event_name: str, code: str) -> None:
        widget = self.project.find(widget_id)
        if widget is None:
            return
        if widget.events.get(event_name, "") == code:
            return
        self.push_undo()
        if code.strip():
            widget.events[event_name] = code
        else:
            widget.events.pop(event_name, None)
        self.refresh_code()

    def apply_parent(self, widget_id: str, value: str) -> None:
        parent = None if value == "Canvas" else value
        self.push_undo()
        if not self.project.reparent(widget_id, parent):
            self.undo_stack.pop()
            return
        self.refresh()

    def pick_prop_color(self, widget_id: str, key: str, entry: ctk.CTkEntry) -> None:
        chosen = colorchooser.askcolor(color=entry.get() or "#ffffff")
        if chosen and chosen[1]:
            entry.delete(0, "end")
            entry.insert(0, chosen[1].lower())
            self.apply_prop(widget_id, key, chosen[1].lower())

    def mark_code_dirty(self, _event=None) -> None:
        if self.code_dirty:
            return
        self.code_dirty = True
        self.code_status.configure(
            text="Manual edits (export only)",
            text_color="#f0b060",
        )

    def regenerate_code(self) -> None:
        if self.code_dirty and not messagebox.askyesno(
            "Regenerate code",
            "Discard the manual code edits and regenerate from the project?",
        ):
            return
        self.refresh_code(force=True)

    def refresh_code(self, force: bool = False) -> None:
        if self.code_dirty and not force:
            return
        code = export(self.project)
        self.code_text.set_code(code)
        self.code_dirty = False
        self.code_status.configure(
            text="Generated",
            text_color="#a0a0a0",
        )

    def copy_code(self) -> None:
        self.clipboard_clear()
        self.clipboard_append(self.code_text.get_code())
        self.set_status("Lua code copied to the clipboard")

    def set_status(self, message: str | None = None) -> None:
        selected = self.project.find(self.selected_id)
        selection = f"{selected.type} {selected.id}" if selected else "nothing"
        width, height = self.content_area()
        outside = self.project.out_of_bounds(width, height)
        warning = f"  |  {len(outside)} outside canvas" if outside else ""
        page = self.project.page_height(width, height)
        scrolling = ""
        if page > height:
            max_scroll = page - height
            scrolling = (
                f"  |  page {page}px at {min(self.preview_scroll, max_scroll)}"
                " (wheel scrolls)"
            )
        mode = "mono" if self.project.theme.mono else ("dark" if self.project.theme.dark else "light")
        bars = self.theme.bars_layout(self.profile.width, self.profile.height)
        surface = "  |  custom surface" if self.project.theme.surface else ""
        base = (
            f"{self.profile.name}  |  canvas {width}x{height}  |  {mode}"
            f"  |  {bars} bars  |  font {self.project.theme.font_size}"
            f"  |  zoom {round(self.zoom * 100)}%  |  selected: {selection}{warning}{surface}{scrolling}"
        )
        if message:
            base = f"{message}   -   {base}"
        self.status.configure(text=base)

    def on_close(self) -> None:
        if self._settings_job is not None:
            try:
                self.after_cancel(self._settings_job)
            except tk.TclError:
                pass
            self._settings_job = None
        self.save_settings()
        self.destroy()


# ---------------------------------------------------------------------------
# Demo project


def demo_project(theme: ThemeSettings | None = None) -> Project:
    """A small settings-page clone, so the default example is recognisable."""
    dark_checked = theme.dark if theme is not None else True
    mono_checked = theme.mono if theme is not None else False
    project = Project.new()
    card = project.add_widget("Card")
    card.x, card.y, card.w, card.h = 24, 24, 760, 452

    title = project.add_widget("Label", card.id)
    title.x, title.y, title.w, title.h = 24, 14, 300, 44
    title.props.update({"text": "Settings", "text_font": "large"})

    section = project.add_widget("Label", card.id)
    section.x, section.y, section.w, section.h = 24, 66, 400, 24
    section.props.update({"text": "Appearance", "text_font": "small"})

    mode_row = project.add_widget("Object", card.id)
    mode_row.id = "mode_row"
    mode_row.x, mode_row.y, mode_row.w, mode_row.h = 24, 98, 240, 40
    mode_row.props.update(
        {
            "layout": "FLEX",
            "flex_flow": "ROW",
            "flex_main": "START",
            "flex_cross": "CENTER",
            "pad_row": 0,
            "pad_column": 12,
            "pad_all": 0,
            "border_width": 0,
            "bg_opa": "TRANSP",
        }
    )

    light = project.add_widget("Chip", mode_row.id)
    light.id = "mode_light"
    light.x, light.y, light.w, light.h = 0, 0, 110, 40
    light.props.update({"text": "Light", "checked": not dark_checked})

    dark = project.add_widget("Chip", mode_row.id)
    dark.id = "mode_dark"
    dark.x, dark.y, dark.w, dark.h = 0, 0, 110, 40
    dark.props.update({"text": "Dark", "checked": dark_checked})

    spacing_label = project.add_widget("Label", card.id)
    spacing_label.x, spacing_label.y, spacing_label.w, spacing_label.h = 24, 158, 300, 28
    spacing_label.props["text"] = "Spacing"

    spacing = project.add_widget("SliderBox", card.id)
    spacing.x, spacing.y, spacing.w, spacing.h = 24, 190, 620, 48
    spacing.props.update({"min": 0, "max": 100, "step": 5, "value": 80})

    size_label = project.add_widget("Label", card.id)
    size_label.x, size_label.y, size_label.w, size_label.h = 24, 250, 300, 28
    size_label.props["text"] = "Size"

    size = project.add_widget("SliderBox", card.id)
    size.x, size.y, size.w, size.h = 24, 282, 620, 48
    size.props.update({"min": 0, "max": 100, "step": 5, "value": 60})

    animations = project.add_widget("Label", card.id)
    animations.x, animations.y, animations.w, animations.h = 24, 344, 300, 28
    animations.props["text"] = "Animations"

    animations_switch = project.add_widget("Switch", card.id)
    animations_switch.x, animations_switch.y = 620, 338
    animations_switch.w, animations_switch.h = 90, 50
    animations_switch.props["checked"] = True

    mono = project.add_widget("Label", card.id)
    mono.x, mono.y, mono.w, mono.h = 24, 398, 300, 28
    mono.props["text"] = "Mono theme"

    mono_switch = project.add_widget("Switch", card.id)
    mono_switch.id = "mono_switch"
    mono_switch.x, mono_switch.y = 620, 392
    mono_switch.w, mono_switch.h = 90, 50
    mono_switch.props["checked"] = mono_checked

    # The canvas is wider than the settings card; a small accent keeps the
    # secondary colour visible in the default project.
    bar = project.add_widget("Bar")
    bar.x, bar.y, bar.w, bar.h = 24, 500, 760, 12
    bar.props["value"] = 65
    return project


# ---------------------------------------------------------------------------
# Self-test


def self_test() -> int:
    project = Project.new()
    card = project.add_widget("Card")
    card.x, card.y, card.w, card.h = 20, 20, 400, 200
    label = project.add_widget("Label", card.id)
    label.props["text"] = "Hello"
    button = project.add_widget("Button", card.id)
    button.props["text"] = "Go"
    chip = project.add_widget("Chip")
    chip.props["text"] = "Chip"
    chip.props["checked"] = True
    slider = project.add_widget("SliderBox")
    slider.props.update({"min": 0, "max": 100, "step": 5, "value": 60})
    list_widget = project.add_widget("List")
    list_widget.props["items"] = "Section\nOne\nTwo"

    code = export(project)
    required = [
        "lvgl.Card(canvas,",
        "layout = lvgl.LAYOUT.NONE",
        "lvgl.SliderBox(canvas, 0, 100, 5, 60)",
        "lvgl.Chip(canvas,",
        "checked = true",
        "lvgl.List(canvas,",
        'list_1:add_text("Section")',
        "lvgl.Label(button_1,",
        "align = lvgl.ALIGN.CENTER",
        "button_1:on(lvgl.EVENT.CLICKED, function()",
        "-- TODO",
    ]
    missing = [fragment for fragment in required if fragment not in code]
    if missing:
        print("self-test: missing fragments:")
        for fragment in missing:
            print("  ", fragment)
        return 1
    if "radius = " in code:
        print("self-test: an unset radius should not be exported")
        return 1

    chip_colour = Project.new()
    colour_chip = chip_colour.add_widget("Chip")
    colour_chip.props["bg_color"] = "#112233"
    chip_code = export(chip_colour)
    if "bg_color = 0x112233" not in chip_code or "bg_opa = lvgl.OPA.COVER" not in chip_code:
        print("self-test: Chip background colour export is wrong")
        return 1

    # Firmware geometry: the same formulas as statusbar.cpp/toolbar.cpp and the
    # dp conversion in LVGL.
    tab5 = MaterialTheme(
        ThemeSettings(
            dark=True,
            spacing=100,
            size=100,
            font_size=29,
            bars_mode="custom",
            bars_landscape="split",
        ),
        dpi=294,
    )
    if tab5.content_top() != 122 or tab5.content_padding() != 29:
        print(
            "self-test: Tab5 default chrome is wrong:",
            tab5.content_top(),
            tab5.content_padding(),
        )
        return 1
    if tab5.content_size(1280, 720) != (1222, 540):
        print("self-test: Tab5 canvas size is wrong:", tab5.content_size(1280, 720))
        return 1

    compact = MaterialTheme(
        ThemeSettings(
            dark=True,
            spacing=80,
            size=80,
            font_size=28,
            bars_mode="custom",
            bars_landscape="split",
        ),
        dpi=294,
    )
    if compact.content_top() != 112 or compact.content_padding() != 23:
        print(
            "self-test: Tab5 compact chrome is wrong:",
            compact.content_top(),
            compact.content_padding(),
        )
        return 1
    if compact.content_size(1280, 720) != (1234, 562):
        print("self-test: Tab5 compact canvas is wrong:", compact.content_size(1280, 720))
        return 1

    side = MaterialTheme(
        ThemeSettings(dark=True, spacing=100, size=100, font_size=29, bars_mode="auto"),
        dpi=294,
    )
    if side.bars_layout(1280, 720) != "side":
        print("self-test: landscape Auto layout should be side")
        return 1
    if side.content_size(1280, 720) != (1148, 583):
        print("self-test: side canvas is wrong:", side.content_size(1280, 720))
        return 1

    mono = MaterialTheme(
        ThemeSettings(
            dark=False,
            mono=True,
            spacing=20,
            size=0,
            font_size=14,
            bars_mode="custom",
            bars_landscape="split",
        ),
        dpi=139,
    )
    if (
        not mono.mono
        or mono.content_top() != 44
        or mono.content_padding() != 3
        or mono.content_size(384, 168) != (378, 118)
    ):
        print(
            "self-test: mono chrome is wrong:",
            mono.content_top(),
            mono.content_padding(),
            mono.content_size(384, 168),
        )
        return 1
    if (mono.ink, mono.paper, mono.primary) != ("#000000", "#ffffff", "#000000"):
        print("self-test: mono colours are wrong")
        return 1

    surfaced = MaterialTheme(
        ThemeSettings(
            dark=True,
            surface="#202020",
            bars_mode="custom",
            bars_landscape="split",
        ),
        dpi=294,
    )
    if surfaced.surface != "#202020" or surfaced.surface_container_low != "#292929":
        print(
            "self-test: surface override is wrong:",
            surfaced.surface,
            surfaced.surface_container_low,
        )
        return 1

    # New widgets follow the firmware's control metrics, not desktop defaults.
    switch_spec = SPEC_BY_TYPE["Switch"]
    if themed_default_size(switch_spec, tab5) != (96, 59):
        print("self-test: Tab5 switch size is wrong:", themed_default_size(switch_spec, tab5))
        return 1
    if themed_default_size(switch_spec, mono) != (25, 15):
        print("self-test: CL-32 switch size is wrong:", themed_default_size(switch_spec, mono))
        return 1

    # Scaling a profile change keeps the top-left anchor and grows every box.
    scaled = Project.new()
    item = scaled.add_widget("Object")
    item.x, item.y, item.w, item.h = 10, 20, 100, 50
    scaled.scale_to((200, 100), (400, 200))
    if (item.x, item.y, item.w, item.h) != (20, 40, 200, 100):
        print("self-test: profile scaling is wrong:", (item.x, item.y, item.w, item.h))
        return 1

    outside = Project.new()
    stray = outside.add_widget("Object")
    stray.x, stray.y, stray.w, stray.h = 220, 110, 50, 20
    if outside.out_of_bounds(200, 100) != [stray]:
        print("self-test: out-of-bounds detection is wrong")
        return 1

    # Tree moves keep the widget on the same canvas position, and renaming
    # updates child parent links.
    moving = Project.new()
    box = moving.add_widget("Card")
    box.x, box.y, box.w, box.h = 20, 20, 300, 200
    child_widget = moving.add_widget("Label")
    child_widget.x, child_widget.y, child_widget.w, child_widget.h = 40, 40, 100, 28
    before = moving.abs_rect(child_widget)
    if not moving.move_widget(child_widget.id, box.id):
        print("self-test: move into container failed")
        return 1
    if child_widget.parent != box.id or moving.abs_rect(child_widget) != before:
        print("self-test: move into container changed the absolute position")
        return 1
    if not moving.move_widget(child_widget.id, None):
        print("self-test: move to canvas failed")
        return 1
    if child_widget.parent is not None or moving.abs_rect(child_widget) != before:
        print("self-test: move to canvas changed the absolute position")
        return 1
    if not moving.rename_widget(child_widget.id, "renamed"):
        print("self-test: rename failed")
        return 1
    if moving.find("renamed") is None:
        print("self-test: renamed widget is missing")
        return 1
    if not moving.move_to_end("renamed") or moving.widgets[-1].id != "renamed":
        print("self-test: move_to_end failed")
        return 1
    if not moving.move_to_start("renamed") or moving.widgets[0].id != "renamed":
        print("self-test: move_to_start failed")
        return 1

    percent = Project.new()
    percent_card = percent.add_widget("Card")
    percent_card.x, percent_card.y, percent_card.w, percent_card.h = 20, 20, 400, 200
    percent_child = percent.add_widget("Label", percent_card.id)
    percent_child.x, percent_child.y = 24, 24
    percent_child.props["w_pct"] = 50
    percent_child.props["h_pct"] = 25
    percent_rect = percent.layout_rects(800, 400)[percent_child.id]
    if percent_rect != (56, 56, 188, 44):
        print("self-test: percentage layout is wrong:", percent_rect)
        return 1
    percent_code = export(percent)
    if "w = lvgl.PCT(50)" not in percent_code or "h = lvgl.PCT(25)" not in percent_code:
        print("self-test: percentage export is wrong")
        return 1

    flex = Project.new()
    flex_row = flex.add_widget("Object")
    flex_row.x, flex_row.y, flex_row.w, flex_row.h = 20, 20, 400, 100
    flex_row.props.update(
        {
            "layout": "FLEX",
            "flex_flow": "ROW",
            "flex_main": "START",
            "flex_cross": "START",
            "pad_row": 0,
            "pad_column": 0,
            "pad_all": 0,
        }
    )
    first = flex.add_widget("Label", flex_row.id)
    first.x, first.y, first.w, first.h = 0, 0, 100, 40
    second = flex.add_widget("Label", flex_row.id)
    second.x, second.y, second.w, second.h = 0, 0, 100, 40
    second.props["flex_grow"] = 1
    flex_rects = flex.layout_rects(800, 400)
    if flex_rects[first.id] != (20, 20, 100, 40) or flex_rects[second.id] != (120, 20, 300, 40):
        print("self-test: flex row layout is wrong:", flex_rects)
        return 1
    content = Project.new()
    content_label = content.add_widget("Label")
    content_label.props["w_content"] = True
    content_label.props["h_content"] = True
    content_code = export(content)
    if "w = lvgl.SIZE_CONTENT" not in content_code or "h = lvgl.SIZE_CONTENT" not in content_code:
        print("self-test: content sizing export is wrong")
        return 1

    # Content-sized leaves are measured from their text, so a profile or font
    # change reflows them instead of reusing (or scaling) stale boxes.
    measured = Project.new()
    measured_label = measured.add_widget("Label")
    measured_label.props.update(
        {"w_content": True, "h_content": True, "text": "Spacing (%)"}
    )
    first = measured.layout_rects(1234, 562)[measured_label.id]
    if first[3] != 37 or first[2] < 100:
        print("self-test: text measurement is wrong:", first)
        return 1
    stored_w = measured_label.w
    measured.theme.font_size = 14
    second = measured.layout_rects(1234, 562)[measured_label.id]
    if second[2] >= first[2] or second[3] != 18:
        print("self-test: text measurement ignores the theme font size:", second)
        return 1
    measured.theme.font_size = 29
    measured.scale(0.25, 0.25)
    if measured_label.w != stored_w:
        print("self-test: scaling changed a content-sized width")
        return 1

    measured_chip = measured.add_widget("Chip")
    measured_chip.props.update(
        {"w_content": True, "h_content": True, "text": "Regular"}
    )
    chip_rect = measured.layout_rects(1234, 562)[measured_chip.id]
    if chip_rect[2] < 60 or chip_rect[3] < 40:
        print("self-test: chip content sizing is wrong:", chip_rect)
        return 1

    grow_cycle = Project.new()
    grow_card = grow_cycle.add_widget("Object")
    grow_card.props.update({"layout": "FLEX", "flex_flow": "COLUMN", "h_content": True})
    grow_child = grow_cycle.add_widget("Object", grow_card.id)
    grow_child.props["flex_grow"] = 1
    if "flex_grow = 1" in export(grow_cycle):
        print("self-test: flex grow was exported inside a content-sized parent")
        return 1

    # Chips, buttons and switches carry the theme's focus-ring margin, so flex
    # layouts inset them by that amount (7 px on the Tab5 at the default size).
    margins = Project.new()
    margin_row = margins.add_widget("Object")
    margin_row.x, margin_row.y, margin_row.w, margin_row.h = 20, 20, 400, 100
    margin_row.props.update(
        {"layout": "FLEX", "flex_flow": "ROW", "pad_all": 0, "pad_row": 0, "pad_column": 0}
    )
    margin_chip = margins.add_widget("Chip", margin_row.id)
    margin_chip.w, margin_chip.h = 100, 50
    margin_rect = margins.layout_rects(800, 400)[margin_chip.id]
    if margin_rect != (27, 27, 100, 50):
        print("self-test: control margin layout is wrong:", margin_rect)
        return 1

    aligned = Project.new()
    aligned_label = aligned.add_widget("Label")
    aligned_label.props["text"] = "Hi"
    aligned_label.props["text_align"] = "CENTER"
    if "text_align = lvgl.TEXT_ALIGN.CENTER" not in export(aligned):
        print("self-test: text alignment export is wrong")
        return 1
    aligned_label.props["text_align"] = "LEFT"
    if "text_align" in export(aligned):
        print("self-test: left alignment should not be exported")
        return 1

    spin = Project.new()
    spinbox = spin.add_widget("Spinbox")
    spinbox.props.update(
        {"min": 10, "max": 32, "step": 1, "digits": 2, "decimals": 0, "value": 28}
    )
    spin_code = export(spin)
    for fragment in (
        "lvgl.Spinbox(canvas)",
        "range = { min = 10, max = 32 }",
        "spin_step = 1",
        "digit_format = { digits = 2, decimals = 0 }",
        "text_align = lvgl.TEXT_ALIGN.CENTER",
        "spinbox_1:set{ value = 28 }",
    ):
        if fragment not in spin_code:
            print("self-test: spinbox export is missing:", fragment)
            return 1
    if spin_code.index("spinbox_1:set{ value = 28 }") < spin_code.index("range = "):
        print("self-test: spinbox value must be set after its range")
        return 1

    # A content-sized page that is taller than the canvas scrolls on the device,
    # so its vertical overflow is not a mistake. Horizontal overflow still is.
    page = Project.new()
    page_root = page.add_widget("Object")
    page_root.x, page_root.y = 0, 0
    page_root.w, page_root.h = 400, 100
    page_root.props.update({"h_content": True, "layout": "FLEX", "flex_flow": "COLUMN"})
    tall = page.add_widget("Label", page_root.id)
    tall.h = 200
    if page.page_height(400, 100) != 200:
        print("self-test: page height is wrong:", page.page_height(400, 100))
        return 1
    if page.out_of_bounds(400, 100):
        print("self-test: a scrolling page was reported as out of bounds")
        return 1
    page_root.w = 500
    if page.out_of_bounds(400, 100) != [page_root]:
        print("self-test: horizontal overflow of a scrolling page is not reported")
        return 1
    page_code = export(page)
    for fragment in (
        "canvas:set{ h = lvgl.SIZE_CONTENT }",
        "canvas:remove_flag(lvgl.FLAG.SCROLLABLE)",
        "object_1:remove_flag(lvgl.FLAG.SCROLLABLE)",
    ):
        if fragment not in page_code:
            print("self-test: scrolling page export is missing:", fragment)
            return 1

    flex_code = export(flex)
    for fragment in (
        "layout = lvgl.LAYOUT.FLEX",
        "flex_flow = lvgl.FLEX_FLOW.ROW",
        "main = lvgl.FLEX_ALIGN.START",
        "flex_grow = 1",
    ):
        if fragment not in flex_code:
            print("self-test: flex export is missing:", fragment)
            return 1

    grid = Project.new()
    grid_box = grid.add_widget("Object")
    grid_box.x, grid_box.y, grid_box.w, grid_box.h = 20, 20, 400, 100
    grid_box.props.update(
        {
            "layout": "GRID",
            "grid_columns": "1fr, 2fr",
            "grid_rows": "40, content",
            "grid_col_align": "START",
            "grid_row_align": "START",
            "pad_all": 0,
            "pad_row": 0,
            "pad_column": 0,
        }
    )
    first = grid.add_widget("Label", grid_box.id)
    second = grid.add_widget("Label", grid_box.id)
    third = grid.add_widget("Label", grid_box.id)
    for child, column, row, span in ((first, 0, 0, 1), (second, 1, 0, 1), (third, 0, 1, 2)):
        child.props.update(
            {
                "grid_col": column,
                "grid_row": row,
                "grid_col_span": span,
                "grid_row_span": 1,
                "cell_col_align": "STRETCH",
                "cell_row_align": "STRETCH",
            }
        )
    grid_rects = grid.layout_rects(800, 400)
    expected_grid = {
        first.id: (20, 20, 133, 40),
        second.id: (153, 20, 267, 40),
        third.id: (20, 60, 400, 28),
    }
    if any(grid_rects[widget_id] != rect for widget_id, rect in expected_grid.items()):
        print("self-test: grid layout is wrong:", {key: grid_rects[key] for key in expected_grid})
        return 1
    grid_code = export(grid)
    for fragment in (
        ":grid({ lvgl.GRID_FR(1), lvgl.GRID_FR(2) }, { 40, lvgl.GRID_CONTENT })",
        ":grid_cell(lvgl.GRID_ALIGN.STRETCH, 0, 1, lvgl.GRID_ALIGN.STRETCH, 0, 1)",
        ":grid_cell(lvgl.GRID_ALIGN.STRETCH, 1, 1, lvgl.GRID_ALIGN.STRETCH, 0, 1)",
        ":grid_cell(lvgl.GRID_ALIGN.STRETCH, 0, 2, lvgl.GRID_ALIGN.STRETCH, 1, 1)",
    ):
        if fragment not in grid_code:
            print("self-test: grid export is missing:", fragment)
            return 1
    grid_box.props["grid_col_align"] = "CENTER"
    if ":grid_align(lvgl.GRID_ALIGN.CENTER, lvgl.GRID_ALIGN.START)" not in export(grid):
        print("self-test: grid align export is missing")
        return 1

    content_card = Project.new()
    sized_card = content_card.add_widget("Card")
    sized_card.x, sized_card.y = 10, 10
    sized_card.props.update(
        {"layout": "NONE", "w_content": True, "h_content": True, "pad_all": 10}
    )
    sized_child = content_card.add_widget("Label", sized_card.id)
    sized_child.x, sized_child.y, sized_child.w, sized_child.h = 5, 7, 100, 20
    sized_rect = content_card.layout_rects(800, 600)[sized_card.id]
    if sized_rect[2:] != (125, 47):
        print("self-test: content sizing is wrong:", sized_rect)
        return 1

    auto_grid = Project.new()
    auto_box = auto_grid.add_widget("Object")
    auto_box.props.update({"layout": "GRID", "grid_columns": "1fr, 1fr", "grid_rows": "1fr"})
    auto_children = [auto_grid.add_widget("Label", auto_box.id) for _ in range(5)]
    if auto_children[-1].props.get("grid_row") != 2:
        print("self-test: grid auto placement did not add rows")
        return 1
    if str(auto_box.props.get("grid_rows", "")).count("1fr") < 3:
        print("self-test: grid auto placement did not extend the row template")
        return 1

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "project.luadesign.json"
        project.save(path)
        loaded = Project.load(path)
        if len(loaded.widgets) != len(project.widgets):
            print("self-test: JSON round-trip changed the widget count")
            return 1
        if export(loaded) != code:
            print("self-test: JSON round-trip changed the generated Lua")
            return 1

        legacy = project.to_dict()
        legacy["profile"] = "cl-32"
        legacy["theme"] = {"dark": False}
        path.write_text(json.dumps(legacy), encoding="utf-8")
        upgraded = Project.load(path)
        if (
            not upgraded.theme.mono
            or upgraded.theme.font_size != 14
            or upgraded.theme.spacing != 20
            or upgraded.theme.size != 0
        ):
            print("self-test: old project theme defaults are wrong")
            return 1

    # Absolute geometry includes the card's padding.
    x, y, _w, _h = project.abs_rect(label)
    if x != card.x + int(card.props.get("pad_all", 0)) + label.x:
        print("self-test: child absolute position is wrong")
        return 1

    print("designer self-test OK")
    return 0


def ui_test() -> int:
    """Drive the real Tk window through every control and widget.

    This catches callback-signature regressions (Custom2kinter 6 passes values
    to switch commands) and rendering/property errors without a human clicking
    around.
    """
    import traceback

    failures: list[tuple[str, str]] = []

    global SETTINGS_FILE
    old_settings_file = SETTINGS_FILE
    SETTINGS_FILE = Path(tempfile.gettempdir()) / "lua-designer-ui-test.settings.json"
    try:
        SETTINGS_FILE.unlink()
    except OSError:
        pass

    def check(label: str, action) -> None:
        try:
            action()
            app.update_idletasks()
        except Exception:  # noqa: BLE001 - report every failure, keep going
            failures.append((label, traceback.format_exc()))

    app = LuaDesigner()
    app.withdraw()

    class FakeEntry:
        def __init__(self, text: str) -> None:
            self.text = text

        def get(self) -> str:
            return self.text

        def delete(self, *_args) -> None:
            self.text = ""

        def insert(self, _index, text: str) -> None:
            self.text = text

    old_askyesno = messagebox.askyesno
    old_showerror = messagebox.showerror
    old_open = filedialog.askopenfilename
    old_save = filedialog.asksaveasfilename
    old_color = colorchooser.askcolor
    old_askstring = simpledialog.askstring

    messagebox.askyesno = lambda *_args, **_kwargs: True
    messagebox.showerror = lambda *_args, **_kwargs: failures.append(
        ("messagebox.showerror", str(_args[1] if len(_args) > 1 else _args))
    )
    simpledialog.askstring = lambda *_args, **_kwargs: "renamed_widget"

    try:
        with tempfile.TemporaryDirectory() as directory:
            project_path = Path(directory) / "ui.luadesign.json"
            lua_path = Path(directory) / "ui.lua"

            def save_path(**kwargs):
                return str(lua_path if "Export" in kwargs.get("title", "") else project_path)

            filedialog.askopenfilename = lambda **_kwargs: str(project_path)
            filedialog.asksaveasfilename = save_path
            colorchooser.askcolor = lambda **_kwargs: ("#123456", "#123456")

            # Switches are the callbacks that Custom2kinter 6 calls with a value.
            for label, switch in (
                ("grid switch", app.grid_switch),
                ("snap switch", app.snap_switch),
                ("mono switch", app.mono_switch),
            ):
                check(label, switch.invoke)
                check(label + " again", switch.invoke)

            check("theme light", lambda: app.on_theme_mode("Light"))
            check("theme dark", lambda: app.on_theme_mode("Dark"))
            check("theme menu", lambda: app.theme_menu._command("Light"))
            check("zoom menu", lambda: app.zoom_menu._command("75%"))
            for value in (24, 28, 32):
                check(f"font {value}", lambda v=value: app.on_font_size(v))
            for value in (0, 50, 80, 100):
                check(f"spacing {value}", lambda v=value: app.on_spacing(v))
                check(f"size {value}", lambda v=value: app.on_size(v))
            for value in ("50%", "65%", "75%", "100%"):
                check(f"zoom {value}", lambda v=value: app.on_zoom(v))

            for mode in ("Custom", "Auto"):
                check(f"bars mode {mode}", lambda v=mode: app.on_bars_mode(v))
            check("bars mode menu", lambda: app.bars_mode._command("Custom"))
            for orientation in ("portrait", "landscape"):
                for layout in ("Split", "Side"):
                    check(
                        f"bars {orientation} {layout}",
                        lambda v=layout, o=orientation: app.on_bars_layout(v, o),
                    )
            for align in ("Left", "Right"):
                check(f"bars align {align}", lambda v=align: app.on_bars_align(v))

            for profile in PROFILES:
                check(f"profile {profile.id}", lambda n=profile.name: app.on_profile(n))
            check("profile back to Tab5", lambda: app.on_profile(PROFILES[0].name))
            check("theme defaults", app.reset_theme_defaults)

            check("surface colour", lambda: app.pick_color("surface"))
            check("surface clear", app.clear_surface)

            for tab in ("Tree", "Properties", "Code"):
                check(f"tab {tab}", lambda name=tab: app.tabs.set(name))

            full_palette = len(_find_widgets(app.palette_panel.body, ctk.CTkButton))
            check(
                "palette search",
                lambda: (
                    app.palette_search.delete(0, "end"),
                    app.palette_search.insert(0, "slider"),
                    app._populate_palette(),
                ),
            )
            filtered_palette = len(_find_widgets(app.palette_panel.body, ctk.CTkButton))
            if not 0 < filtered_palette < full_palette:
                failures.append(("palette search", f"filter kept {filtered_palette} of {full_palette}"))
            check("palette clear", app._clear_palette_search)

            app.refresh_tree()
            full_tree = len(app._tree_rows)
            check(
                "tree search",
                lambda: (
                    app.tree_search.delete(0, "end"),
                    app.tree_search.insert(0, "slider"),
                    app.refresh_tree(),
                ),
            )
            filtered_tree = len(app._tree_rows)
            if not 1 < filtered_tree < full_tree:
                failures.append(("tree search", f"filter kept {filtered_tree} of {full_tree}"))
            check("tree clear", app._clear_tree_search)
            if len(app._tree_rows) != full_tree:
                failures.append(("tree clear", "the unfiltered tree did not return"))

            app.refresh_tree()
            container = next(
                (w for w in app.project.widgets if app.project.children(w.id)), None
            )
            if container is not None:
                before = len(app._tree_rows)
                check("tree collapse", lambda c=container.id: app.toggle_tree(c))
                if len(app._tree_rows) >= before:
                    failures.append(("tree collapse", "collapsing did not hide children"))
                check("tree expand", lambda c=container.id: app.toggle_tree(c))
                if len(app._tree_rows) != before:
                    failures.append(("tree expand", "expanding did not restore the tree"))

            name_target = app.project.add_widget("Object")
            app.refresh()
            check(
                "rename widget to",
                lambda wid=name_target.id: app.rename_widget_to(wid, "renamed_object"),
            )
            if app.project.find("renamed_object") is None:
                failures.append(("rename widget to", "new name not found"))

            icon_target = app.project.add_widget("Label")
            app.apply_text_icon(icon_target.id, None, "MEMORY")
            icon_widget = app.project.find(icon_target.id)
            if icon_widget is None or icon_widget.props.get("text") != "lvgl.ICON.MEMORY":
                failures.append(("icon text", "icon text was not applied"))
            elif icon_widget.props.get("text_font") != "icon":
                failures.append(("icon text", "icon font was not selected"))

            # The code preview is editable and the editor's text is exported.
            check("code edit", lambda: (app.code_text.set_code("-- manual export\n"), app.mark_code_dirty()))
            if not app.code_dirty:
                failures.append(("code edit", "manual edits were not marked dirty"))
            check("code export", app.export_lua)
            if lua_path.read_text(encoding="utf-8") != "-- manual export\n":
                failures.append(("code export", "the editor text was not exported"))
            check("code regenerate", app.regenerate_code)
            if app.code_dirty:
                failures.append(("code regenerate", "the dirty flag stayed set"))

            grid_container = app.project.add_widget("Object")
            app.refresh()
            app.select(grid_container.id)
            check("grid enable", lambda: app.apply_prop(grid_container.id, "layout", "GRID"))
            grid_container = app.project.find(grid_container.id)
            if grid_container is None or not grid_container.props.get("grid_columns"):
                failures.append(("grid enable", "grid defaults were not applied"))
            check("grid columns", lambda: app.apply_prop(grid_container.id, "grid_columns", "1fr, 2fr"))
            check("grid rows", lambda: app.apply_prop(grid_container.id, "grid_rows", "1fr, 1fr"))
            check("grid col align", lambda: app.apply_prop(grid_container.id, "grid_col_align", "CENTER"))
            check("grid row align", lambda: app.apply_prop(grid_container.id, "grid_row_align", "START"))
            grid_child = app.project.add_widget("Label", grid_container.id)
            app.select(grid_child.id)
            check("grid col", lambda: app.apply_prop(grid_child.id, "grid_col", 1))
            check("grid row", lambda: app.apply_prop(grid_child.id, "grid_row", 1))
            check("grid col span", lambda: app.apply_prop(grid_child.id, "grid_col_span", 2))
            check("grid row span", lambda: app.apply_prop(grid_child.id, "grid_row_span", 1))
            check("grid cell align", lambda: app.apply_prop(grid_child.id, "cell_col_align", "CENTER"))
            app.refresh_properties()
            grid_script = export(app.project)
            for fragment in (":grid(", ":grid_cell(", ":grid_align(lvgl.GRID_ALIGN.CENTER"):
                if fragment not in grid_script:
                    failures.append(("grid export", f"missing {fragment}"))

            check("save settings", app.save_settings)
            saved_settings = app.load_settings()
            if saved_settings.get("profile") != app.project.profile:
                failures.append(("save settings", "profile was not saved"))
            if not isinstance(saved_settings.get("theme"), dict):
                failures.append(("save settings", "theme was not saved"))

            renamed = app.project.add_widget("Object")
            check("rename widget", lambda: app.rename_widget(renamed.id))
            if app.project.find("renamed_widget") is None:
                failures.append(("rename widget", "the widget was not renamed"))
            check("tree move", lambda: app.move_tree_widget("renamed_widget", None, None))
            check("z order front", lambda: app.z_order("renamed_widget", True))
            check("z order back", lambda: app.z_order("renamed_widget", False))

            # Simulate dropping a tree row onto a container row.
            app.refresh()
            drag_source = next(
                (widget for widget in app.project.widgets if widget.parent is None and widget.type == "Label"),
                None,
            )
            drag_target = next(
                (widget for widget in app.project.widgets if widget.type == "Card"),
                None,
            )
            if drag_source is not None and drag_target is not None:
                original_pointery = app.winfo_pointery
                app.winfo_pointery = lambda: (
                    app._tree_rows[drag_target.id].winfo_rooty()
                    + app._tree_rows[drag_target.id].winfo_height() // 2
                )
                app.refresh_tree()
                app._tree_drag_moved = True
                check("tree drop", lambda: app._tree_release(drag_source.id, None))
                app.winfo_pointery = original_pointery
                if drag_source.parent != drag_target.id:
                    failures.append(("tree drop", "the widget did not change parent"))

            # Every widget, every property type, plus a real property switch.
            app.on_zoom("50%")
            for spec in WIDGET_SPECS:
                app.selected_id = None
                check(f"add {spec.type}", lambda name=spec.type: app.add_widget(name))
                node = app.project.find(app.selected_id)
                if node is None:
                    failures.append((f"add {spec.type}", "widget was not selected"))
                    continue

                for prop in spec.properties:
                    if prop.is_geometry:
                        continue
                    if prop.kind == "bool":
                        value = True
                    elif prop.kind == "int":
                        value = 10
                    elif prop.kind == "choice":
                        value = prop.choices[-1]
                    elif prop.kind == "color":
                        value = "#336699"
                    elif prop.kind == "multiline":
                        value = "One\nTwo"
                    else:
                        value = "Test"
                    check(
                        f"{spec.type}.{prop.key}={value!r}",
                        lambda n=node.id, p=prop.key, v=value: app.apply_prop(n, p, v),
                    )
                    if prop.optional:
                        check(
                            f"{spec.type}.{prop.key}=theme",
                            lambda n=node.id, p=prop.key: app.apply_prop(n, p, None),
                        )

                app.selected_id = node.id
                check(f"{spec.type} properties", app.refresh_properties)
                for switch in _find_widgets(app.properties_frame, ctk.CTkSwitch):
                    check(f"{spec.type} property switch", switch.invoke)

            # Direct property/editor operations that the property panel uses.
            button = next(
                (widget for widget in app.project.widgets if widget.type == "Button"),
                None,
            )
            if button is not None:
                other = app.project.add_widget("Object")
                other.x = 77
                app.selected_id = other.id
                check(
                    "apply geometry to owner",
                    lambda: app.apply_geometry(button.id, "x", FakeEntry("32")),
                )
                if other.x != 77:
                    failures.append(("apply geometry to owner", "selection was changed"))
                app.selected_id = button.id
                check(
                    "apply int prop",
                    lambda: app.apply_int_prop(button.id, "radius", FakeEntry("8"), True),
                )
                check(
                    "apply event",
                    lambda: app.apply_event(button.id, "CLICKED", "result = 1"),
                )
                check(
                    "pick prop colour",
                    lambda: app.pick_prop_color(button.id, "text_color", FakeEntry("#abcdef")),
                )
                if "result = 1" not in export(app.project):
                    failures.append(("apply event", "event code is missing from the export"))

            # Drag/resize starts from the canvas hit test.
            if app.project.widgets:
                app.selected_id = app.project.widgets[0].id
                app.refresh()
                rect = app.selected_canvas_rect()
                if rect is not None:
                    start_x = rect[0] + rect[2] / 2
                    start_y = rect[1] + rect[3] / 2

                    class CanvasEvent:
                        def __init__(self, x: float, y: float) -> None:
                            self.x = int(x)
                            self.y = int(y)

                    check("canvas press", lambda: app.on_press(CanvasEvent(start_x, start_y)))
                    if app._drag:
                        moved = app.project.find(app._drag["widget"])
                        before = (moved.x, moved.y) if moved else None
                        check(
                            "canvas drag",
                            lambda: app.on_drag(CanvasEvent(start_x + 16, start_y + 16)),
                        )
                        check("canvas release", lambda: app.on_release(None))
                        if moved is not None and before == (moved.x, moved.y):
                            failures.append(("canvas drag", "the widget did not move"))

            # Container nesting still works after the root-level additions.
            container = app.project.add_widget("Card")
            if button is not None:
                check("reparent", lambda: app.apply_parent(button.id, container.id))
                if button.parent != container.id:
                    failures.append(("reparent", "the button was not moved into the card"))
            app.selected_id = container.id
            check("add into container", lambda: app.add_widget("Label"))
            if app.project.find(app.selected_id) is None or app.project.find(app.selected_id).parent != container.id:
                failures.append(("add into container", "child is not inside the card"))

            chip = next(
                (widget for widget in app.project.widgets if widget.type == "Chip"),
                None,
            )
            if chip is not None:
                app.selected_id = chip.id
                app.refresh_properties()
                check(
                    "chip selected",
                    lambda: app.apply_prop(chip.id, "checked", True),
                )
                if not chip.props.get("checked"):
                    failures.append(("chip selected", "checked did not become True"))

            app.selected_id = app.project.widgets[0].id if app.project.widgets else None
            check("duplicate", app.duplicate_selected)
            check("undo duplicate", app.undo)
            check("nudge", lambda: app.nudge(8, 8))
            check("delete", app.delete_selected)
            check("undo delete", app.undo)

            check("save project", app.save_project)
            if not project_path.exists():
                failures.append(("save project", "file was not written"))
            check("open project", app.open_project)
            check("export lua", app.export_lua)
            if not lua_path.exists():
                failures.append(("export lua", "file was not written"))

            # Render side and split layouts at every zoom without a window.
            for mode, align in (("Auto", "Left"), ("Custom", "Right")):
                app.project.theme.bars_mode = mode.lower()
                app.project.theme.bars_align = align.lower()
                app.project.theme.bars_landscape = "side"
                for zoom in ("50%", "65%", "100%"):
                    app.on_zoom(zoom)
                    check(f"render {mode} {align} {zoom}", app.render_only)
    finally:
        messagebox.askyesno = old_askyesno
        messagebox.showerror = old_showerror
        filedialog.askopenfilename = old_open
        filedialog.asksaveasfilename = old_save
        colorchooser.askcolor = old_color
        simpledialog.askstring = old_askstring
        SETTINGS_FILE = old_settings_file
        try:
            app.destroy()
        except Exception:  # noqa: BLE001
            pass

    if failures:
        print(f"designer UI test: {len(failures)} failure(s)")
        for label, detail in failures:
            print(f"\n--- {label} ---\n{detail}")
        return 1

    print("designer UI test OK")
    return 0


def _find_widgets(parent, widget_type):
    found = []
    for child in parent.winfo_children():
        if isinstance(child, widget_type):
            found.append(child)
        found.extend(_find_widgets(child, widget_type))
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="Run model/export tests without a window")
    parser.add_argument("--ui-test", action="store_true", help="Drive the real Tk window through every control")
    parser.add_argument("--demo-export", type=Path, help="Write the demo project as Lua and exit")
    parser.add_argument("--open", type=Path, help="Open a .luadesign.json project at startup")
    parser.add_argument("--screenshot", type=Path, help="Capture the window and exit (smoke test)")
    parser.add_argument(
        "--profile",
        choices=[profile.id for profile in PROFILES],
        help="Start with a device profile (for screenshots)",
    )
    parser.add_argument(
        "--appearance",
        help="Override spacing,size,font-size for screenshots, e.g. 80,80,28",
    )
    parser.add_argument("--mono", action="store_true", help="Start with the mono theme")
    parser.add_argument("--zoom", choices=("50%", "65%", "75%", "100%"), help="Preview zoom")
    parser.add_argument("--maximized", action="store_true", help="Start maximized (for screenshots)")
    parser.add_argument("--surface", help="Override the surface colour for screenshots")
    parser.add_argument("--bars", choices=("auto", "custom"), help="System bars mode")
    parser.add_argument("--portrait-layout", choices=("split", "side"))
    parser.add_argument("--landscape-layout", choices=("split", "side"))
    parser.add_argument("--align", choices=("left", "right"))
    parser.add_argument(
        "--screenshot-tab",
        choices=("Tree", "Properties", "Code"),
        default="Tree",
        help="Tab to show in the screenshot",
    )
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if args.ui_test:
        return ui_test()

    if args.demo_export:
        args.demo_export.write_text(export(demo_project()), encoding="utf-8")
        print(f"designer demo: {args.demo_export}")
        return 0

    app = LuaDesigner(load_saved=not bool(args.screenshot or args.open))
    app.protocol("WM_DELETE_WINDOW", app.on_close)

    if args.open:
        app.load_project(args.open)

    if args.profile:
        profile = find_profile(args.profile)
        old_size = app.content_area()
        app.profile = profile
        app.project.profile = profile.id
        app.project.theme = profile.default_theme()
        app.profile_menu.set(profile.name)
        app.sync_theme_controls()
        app.theme = MaterialTheme(app.project.theme, app.profile.dpi)
        app.project.scale_to(old_size, app.content_area())
        app.selected_id = None
        app.refresh()

    if args.appearance:
        try:
            spacing, size, font_size = (int(part) for part in args.appearance.split(","))
        except ValueError:
            parser.error("--appearance expects three comma-separated integers")
        app.project.theme.spacing = spacing
        app.project.theme.size = size
        app.project.theme.font_size = font_size
        app.sync_theme_controls()
        app.apply_theme()

    if args.mono:
        app.project.theme.mono = True
        app.sync_theme_controls()
        app.apply_theme()

    if (
        args.surface is not None
        or args.bars
        or args.portrait_layout
        or args.landscape_layout
        or args.align
    ):
        if args.surface is not None:
            app.project.theme.surface = args.surface
        if args.bars:
            app.project.theme.bars_mode = args.bars
        if args.portrait_layout:
            app.project.theme.bars_portrait = args.portrait_layout
        if args.landscape_layout:
            app.project.theme.bars_landscape = args.landscape_layout
        if args.align:
            app.project.theme.bars_align = args.align
        app.sync_theme_controls()
        app.apply_theme()

    if args.zoom:
        app.on_zoom(args.zoom)

    if args.maximized:
        app.state("zoomed")

    if args.screenshot:
        def select_demo() -> None:
            app.select("card_1")
            try:
                app.tabs.set(args.screenshot_tab)
            except Exception:  # noqa: BLE001 - older CTk tab API
                pass

        def capture() -> None:
            try:
                # Flush pending geometry/redraw work; without this the grab can capture the
                # frame from before the selected tab and palette were drawn. Bring the window
                # to the front first: ImageGrab reads the screen, so a window behind the
                # terminal would otherwise screenshot the terminal.
                app.lift()
                app.focus_force()
                try:
                    app.attributes("-topmost", True)
                except Exception:  # noqa: BLE001 - not every window manager supports it
                    pass
                app.update_idletasks()
                app.update()
                from PIL import ImageGrab

                x = app.winfo_rootx()
                y = app.winfo_rooty()
                width = app.winfo_width()
                height = app.winfo_height()
                ImageGrab.grab(bbox=(x, y, x + width, y + height)).save(args.screenshot)
                print(f"designer screenshot: {args.screenshot}")
            finally:
                app.after(100, app.destroy)

        app.after(300, select_demo)
        app.after(1400, capture)

    app.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
