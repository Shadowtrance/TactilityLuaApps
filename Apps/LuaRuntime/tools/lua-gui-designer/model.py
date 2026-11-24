"""Project model for the Lua GUI designer."""

from __future__ import annotations

import copy
import json
import math
import re
from dataclasses import dataclass, field
from pathlib import Path

from fonts import fonts_available, line_height, measure_text
from theme import MaterialTheme, ThemeSettings
from widgets import SPEC_BY_TYPE

# Widget types whose theme style reserves room for a focus ring with `marginAll`.
# Flex layouts include those margins, so the preview has to as well.
MARGIN_TYPES = frozenset(("Button", "Chip", "Switch", "TextButton"))
_THEME_CACHE: dict[tuple, MaterialTheme] = {}


def parse_grid_tracks(value: object) -> list[tuple[str, int]]:
    """Parse `"1fr, 2fr, content"` into track kinds and values.

    Accepted tokens are a pixel integer (`80`, `80px`), `content`/`auto`, or a
    fraction (`1fr`). Invalid tokens fall back to a small pixel track so a typo
    in the property panel cannot make the preview disappear.
    """
    text = str(value or "").strip().lower()
    if not text:
        return []

    parts = [part.strip() for part in re.split(r"[,;]", text) if part.strip()]
    if len(parts) == 1:
        parts = text.split()

    tracks: list[tuple[str, int]] = []
    for part in parts:
        part = part.replace(" ", "")
        if part in ("content", "auto", "min"):
            tracks.append(("content", 0))
        elif part.endswith("fr"):
            try:
                fraction = max(1, int(round(float(part[:-2]))))
            except ValueError:
                fraction = 1
            tracks.append(("fr", fraction))
        elif part.endswith("px"):
            try:
                tracks.append(("px", max(0, int(float(part[:-2])))))
            except ValueError:
                tracks.append(("px", 80))
        else:
            try:
                tracks.append(("px", max(0, int(float(part)))))
            except ValueError:
                tracks.append(("px", 80))

    return tracks


def scale_grid_tracks(value: object, factor: float) -> str:
    """Scale pixel tracks in a grid template, leaving fractions/content alone."""
    tracks = parse_grid_tracks(value)
    if not tracks:
        return str(value or "")
    parts = []
    for kind, number in tracks:
        if kind == "px":
            parts.append(str(max(0, int(round(number * factor)))))
        elif kind == "fr":
            parts.append(f"{number}fr")
        else:
            parts.append("content")
    return ", ".join(parts)


def resolve_grid_tracks(
    tracks: list[tuple[str, int]],
    available: int,
    gap: int,
    content_sizes: list[int],
    align: str,
) -> tuple[list[int], list[int], int]:
    """Resolve sizes and offsets for one grid axis."""
    count = len(tracks)
    if count == 0:
        return [], [], 0

    gap = max(0, gap)
    sizes = [0] * count
    fractions = [0] * count
    for index, (kind, value) in enumerate(tracks):
        if kind == "fr":
            fractions[index] = max(1, value)
        elif kind == "content":
            measured = content_sizes[index] if index < len(content_sizes) else 0
            sizes[index] = max(1, measured)
        else:
            sizes[index] = max(0, value)

    used_gap = gap * max(0, count - 1)
    free = max(0, available - used_gap - sum(sizes))
    total_fr = sum(fractions)

    if total_fr:
        distributed = 0
        for index, fraction in enumerate(fractions):
            if fraction:
                sizes[index] = max(1, round(free * fraction / total_fr))
                distributed += sizes[index]
        remainder = free - distributed
        if remainder:
            for index, fraction in enumerate(fractions):
                if fraction:
                    sizes[index] = max(1, sizes[index] + remainder)
                    break
        start = 0
        extra_gap = 0
    else:
        if align == "CENTER":
            start = free // 2
            extra_gap = 0
        elif align == "END":
            start = free
            extra_gap = 0
        elif align == "SPACE_BETWEEN" and count > 1:
            start = 0
            extra_gap = free // (count - 1)
        elif align == "SPACE_AROUND":
            extra_gap = free // count
            start = extra_gap // 2
        elif align == "SPACE_EVENLY":
            extra_gap = free // (count + 1)
            start = extra_gap
        else:
            start = 0
            extra_gap = 0

    offsets: list[int] = []
    position = start
    for size in sizes:
        offsets.append(position)
        position += size + gap + extra_gap

    return sizes, offsets, gap + extra_gap


@dataclass
class WidgetNode:
    id: str
    type: str
    parent: str | None
    x: int
    y: int
    w: int
    h: int
    props: dict = field(default_factory=dict)
    events: dict[str, str] = field(default_factory=dict)

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "type": self.type,
            "parent": self.parent,
            "x": self.x,
            "y": self.y,
            "w": self.w,
            "h": self.h,
            "props": self.props,
            "events": self.events,
        }

    @classmethod
    def from_dict(cls, data: dict) -> "WidgetNode":
        return cls(
            id=str(data["id"]),
            type=str(data["type"]),
            parent=data.get("parent"),
            x=int(data.get("x", 0)),
            y=int(data.get("y", 0)),
            w=int(data.get("w", 100)),
            h=int(data.get("h", 40)),
            props=dict(data.get("props", {})),
            events=dict(data.get("events", {})),
        )


@dataclass
class Project:
    profile: str = "tab5-landscape"
    theme: ThemeSettings = field(default_factory=ThemeSettings)
    widgets: list[WidgetNode] = field(default_factory=list)

    @classmethod
    def new(cls) -> "Project":
        return cls()

    # ---- lookups ----------------------------------------------------------

    def find(self, widget_id: str | None) -> WidgetNode | None:
        if widget_id is None:
            return None
        for widget in self.widgets:
            if widget.id == widget_id:
                return widget
        return None

    def children(self, parent_id: str | None) -> list[WidgetNode]:
        return [widget for widget in self.widgets if widget.parent == parent_id]

    def descendants(self, parent_id: str) -> list[WidgetNode]:
        result: list[WidgetNode] = []
        for child in self.children(parent_id):
            result.append(child)
            result.extend(self.descendants(child.id))
        return result

    def is_container(self, widget_id: str | None) -> bool:
        widget = self.find(widget_id)
        if widget is None:
            return True
        spec = SPEC_BY_TYPE.get(widget.type)
        return bool(spec and spec.container)

    def _material(self) -> MaterialTheme:
        """A theme instance for measurement, cached on the geometry inputs."""
        key = (
            self.profile,
            self.theme.mono,
            self.theme.font_size,
            self.theme.spacing,
            self.theme.size,
        )
        cached = _THEME_CACHE.get(key)
        if cached is None:
            from profiles import find_profile

            cached = MaterialTheme(self.theme, find_profile(self.profile).dpi)
            if len(_THEME_CACHE) > 32:
                _THEME_CACHE.clear()
            _THEME_CACHE[key] = cached
        return cached

    def margin_for(self, widget: WidgetNode | None) -> int:
        """The theme's focus-ring room around a control, in pixels.

        The firmware styles chips, buttons and switches with
        `marginAll: @focus-width + @focus-offset`. Flex layouts add that room
        around the widget, so the preview does too. Widgets without it return 0.
        """
        if widget is None or widget.type not in MARGIN_TYPES:
            return 0
        return self._material().control_margin()

    def measure_widget(self, widget: WidgetNode) -> tuple[int, int] | None:
        """The content size a leaf widget asks for, or None when it has no text size.

        Mirrors LVGL's content sizing for labels and controls. Text is measured
        with the firmware's font, so switching profiles or a theme's font size
        reflows content-sized widgets instead of scaling stale boxes.
        """
        props = widget.props or {}
        if not (props.get("w_content") or props.get("h_content")):
            return None
        if not fonts_available():
            return None

        theme = self._material()
        font_kind = str(props.get("text_font", "default") or "default")
        size = theme.font_px(font_kind)
        fallback = {"IconButton": "OK", "Button": "Button", "TextButton": "Button"}.get(
            widget.type, ""
        )
        text = str(props.get("text") or fallback)

        if widget.type == "Label":
            width = measure_text(text, size) + 2
            height = line_height(size)
        elif widget.type == "Chip":
            width = measure_text(text, size) + 2 * theme.chip_pad_hor()
            height = line_height(size) + 2 * theme.chip_pad_ver()
        elif widget.type in ("Button", "TextButton", "IconButton"):
            width = measure_text(text, size) + 2 * theme.control_pad_hor()
            height = line_height(size) + 2 * theme.control_pad_ver()
        elif widget.type == "Switch":
            width, height = theme.switch_size()
        else:
            return None
        return max(1, int(width)), max(1, int(height))

    def ordered(self) -> list[WidgetNode]:
        """Parents before children, preserving list order for siblings."""
        result: list[WidgetNode] = []
        visited: set[str] = set()

        def visit(parent: str | None) -> None:
            for widget in self.widgets:
                if widget.parent != parent or widget.id in visited:
                    continue
                visited.add(widget.id)
                result.append(widget)
                visit(widget.id)

        visit(None)
        # Orphans (corrupt file) stay visible rather than disappearing.
        for widget in self.widgets:
            if widget.id not in visited:
                result.append(widget)
        return result

    def abs_rect(self, widget: WidgetNode) -> tuple[int, int, int, int]:
        """Absolute content coordinates, including ancestor padding."""
        x, y = 0, 0
        node: WidgetNode | None = widget
        chain: list[WidgetNode] = []
        while node is not None:
            chain.append(node)
            node = self.find(node.parent)
        for node in reversed(chain):
            parent = self.find(node.parent)
            pad = content_offset(self, parent) if parent is not None else 0
            x += node.x + pad
            y += node.y + pad
        return x, y, widget.w, widget.h

    def out_of_bounds(self, width: int, height: int) -> list[WidgetNode]:
        """Widgets whose absolute box doesn't fit the content canvas.

        A top-level content-sized container that is taller than the canvas is a
        scrolling page: it is meant to overflow, so only horizontal overflow and
        negative positions are reported for it and its children.
        """
        result: list[WidgetNode] = []
        rects = self.layout_rects(width, height)
        scrolling: set[str] = set()
        for widget in self.children(None):
            props = widget.props or {}
            if not props.get("h_content"):
                continue
            rect = rects.get(widget.id, (0, 0, 0, 0))
            if rect[3] > height:
                scrolling.add(widget.id)
                scrolling.update(child.id for child in self.descendants(widget.id))
        for widget in self.ordered():
            x, y, w, h = rects.get(widget.id, (0, 0, 0, 0))
            vertical = 0 if widget.id in scrolling else y + h - height
            if x < 0 or y < 0 or x + w > width or vertical > 0:
                result.append(widget)
        return result

    def page_height(self, width: int, height: int) -> int:
        """Height of the whole page, including what scrolls below the canvas."""
        rects = self.layout_rects(width, height)
        bottom = height
        for widget in self.children(None):
            _, y, _, h = rects.get(widget.id, (0, 0, 0, 0))
            bottom = max(bottom, y + h)
        return bottom

    def layout_rects(self, width: int, height: int) -> dict[str, tuple[int, int, int, int]]:
        """Resolve widget boxes in canvas coordinates.

        Absolute children keep their x/y. Percentage width/height resolve
        against the parent's content box. Flex parents arrange their children
        on the main/cross axes. This is the single layout pass the preview,
        hit-testing and out-of-bounds checks share.
        """
        rects: dict[str, tuple[int, int, int, int]] = {}

        def size_of(child: WidgetNode, content_w: int, content_h: int) -> tuple[int, int]:
            props = child.props or {}
            try:
                w_pct = max(0, min(100, int(props.get("w_pct", 0) or 0)))
            except (TypeError, ValueError):
                w_pct = 0
            try:
                h_pct = max(0, min(100, int(props.get("h_pct", 0) or 0)))
            except (TypeError, ValueError):
                h_pct = 0
            w = round(content_w * w_pct / 100) if w_pct else child.w
            h = round(content_h * h_pct / 100) if h_pct else child.h

            # A content-sized absolute container follows its children's bounds. Flex and
            # grid content sizing depends on their layout algorithm; keep the stored box
            # there until that pass runs. Leaves are measured from their text, so a
            # profile or theme change reflows them instead of reusing stale sizes.
            layout = str(props.get("layout", "NONE") or "NONE")
            children = self.children(child.id)
            if not children:
                measured = self.measure_widget(child)
                if measured is not None:
                    if props.get("w_content"):
                        w = measured[0]
                    if props.get("h_content"):
                        h = measured[1]
            if children and (props.get("w_content") or props.get("h_content")):
                pad = content_offset(self, child)
                inner_w = max(1, w - 2 * pad)
                inner_h = max(1, h - 2 * pad)

                if layout == "FLEX":
                    flow = str(props.get("flex_flow", "ROW") or "ROW")
                    row = flow.startswith("ROW")
                    gap = max(0, int(props.get("pad_column" if row else "pad_row", 0) or 0))
                    sizes = [size_of(kid, inner_w, inner_h) for kid in children]
                    margins = [self.margin_for(kid) for kid in children]
                    if props.get("w_content"):
                        content_w = (
                            sum(size[0] + 2 * margin for size, margin in zip(sizes, margins))
                            + gap * max(0, len(sizes) - 1)
                            if row
                            else max(size[0] + 2 * margin for size, margin in zip(sizes, margins))
                        )
                        w = max(1, content_w + 2 * pad)
                        inner_w = max(1, w - 2 * pad)
                    if props.get("h_content"):
                        content_h = (
                            max(size[1] + 2 * margin for size, margin in zip(sizes, margins))
                            if row
                            else sum(size[1] + 2 * margin for size, margin in zip(sizes, margins))
                            + gap * max(0, len(sizes) - 1)
                        )
                        h = max(1, content_h + 2 * pad)
                elif layout != "GRID":
                    if props.get("w_content"):
                        w = max(1, max(
                            kid.x + size_of(kid, inner_w, inner_h)[0] for kid in children
                        ) + 2 * pad)
                        inner_w = max(1, w - 2 * pad)
                    if props.get("h_content"):
                        h = max(1, max(
                            kid.y + size_of(kid, inner_w, inner_h)[1] for kid in children
                        ) + 2 * pad)

            return max(1, w), max(1, h)

        def alignment_offset(align: str, free: int) -> int:
            if align == "CENTER":
                return free // 2
            if align == "END":
                return free
            return 0

        def place_flex(
            parent: WidgetNode,
            children: list[WidgetNode],
            content_x: int,
            content_y: int,
            content_w: int,
            content_h: int,
        ) -> dict[str, tuple[int, int, int, int]]:
            props = parent.props or {}
            flow = str(props.get("flex_flow", "ROW") or "ROW")
            row = flow.startswith("ROW")
            wrap = flow.endswith("WRAP")
            main_align = str(props.get("flex_main", "START") or "START")
            cross_align = str(props.get("flex_cross", "START") or "START")
            try:
                pad_row = max(0, int(props.get("pad_row", 0) or 0))
                pad_column = max(0, int(props.get("pad_column", 0) or 0))
            except (TypeError, ValueError):
                pad_row = pad_column = 0
            gap_main = pad_column if row else pad_row
            gap_cross = pad_row if row else pad_column

            content_main = bool(props.get("w_content" if row else "h_content"))
            items = []
            for child in children:
                child_w, child_h = size_of(child, content_w, content_h)
                main = child_w if row else child_h
                cross = child_h if row else child_w
                # A grow item inside a content-sized main axis is circular: the
                # parent would size itself from the child that fills the parent.
                grow = (
                    0
                    if content_main
                    else max(0, int((child.props or {}).get("flex_grow", 0) or 0))
                )
                items.append(
                    {
                        "child": child,
                        "w": child_w,
                        "h": child_h,
                        "main": main,
                        "cross": cross,
                        "grow": grow,
                        "margin": self.margin_for(child),
                    }
                )

            content_main = content_w if row else content_h
            content_cross = content_h if row else content_w
            result: dict[str, tuple[int, int, int, int]] = {}

            def finalize(line: list[dict], line_cross: int, cross_pos: int) -> None:
                occupied = sum(item["main"] + 2 * item["margin"] for item in line)
                total_main = occupied + gap_main * max(0, len(line) - 1)
                # LVGL sizes a grow item from the space the other items leave.
                # Its own width (and even LV_SIZE_CONTENT) is ignored; only the
                # theme's min_width clamps the result, which is 0 by default.
                growers = sum(item["grow"] for item in line)
                if growers:
                    fixed = sum(
                        item["main"] + 2 * item["margin"]
                        for item in line
                        if item["grow"] == 0
                    )
                    grow_space = max(
                        0,
                        content_main
                        - fixed
                        - gap_main * max(0, len(line) - 1),
                    )
                    for item in line:
                        if item["grow"]:
                            item["main"] = max(
                                1, round(grow_space * item["grow"] / growers)
                            )
                    occupied = sum(item["main"] + 2 * item["margin"] for item in line)
                    total_main = occupied + gap_main * max(0, len(line) - 1)
                free = max(0, content_main - total_main)
                if main_align == "SPACE_BETWEEN" and len(line) > 1 and free:
                    start = 0
                    extra = free // (len(line) - 1)
                elif main_align == "SPACE_AROUND" and line and free:
                    extra = free // len(line)
                    start = extra // 2
                elif main_align == "SPACE_EVENLY" and line:
                    extra = free // (len(line) + 1)
                    start = extra
                else:
                    extra = gap_main
                    start = alignment_offset(main_align, free)

                main_pos = start
                for item in line:
                    margin = item["margin"]
                    cross = item["cross"]
                    if cross_align == "STRETCH":
                        cross = max(1, line_cross - 2 * margin)
                    box = cross + 2 * margin
                    if cross_align == "CENTER":
                        cross_offset = margin + max(0, (line_cross - box) // 2)
                    elif cross_align == "END":
                        cross_offset = max(0, line_cross - box) + margin
                    else:
                        cross_offset = margin
                    if row:
                        x = content_x + main_pos + margin
                        y = content_y + cross_pos + cross_offset
                        w, h = item["main"], cross
                    else:
                        x = content_x + cross_pos + cross_offset
                        y = content_y + main_pos + margin
                        w, h = cross, item["main"]
                    result[item["child"].id] = (x, y, max(1, w), max(1, h))
                    main_pos += item["main"] + 2 * margin + extra

            if not wrap:
                if cross_align == "STRETCH":
                    line_cross = content_cross
                else:
                    line_cross = max(
                        (item["cross"] + 2 * item["margin"] for item in items),
                        default=0,
                    )
                cross_pos = alignment_offset(
                    cross_align if cross_align in ("CENTER", "END") else "START",
                    max(0, content_cross - line_cross),
                )
                finalize(items, line_cross, cross_pos)
                return result

            lines: list[list[dict]] = []
            current: list[dict] = []
            used = 0
            for item in items:
                outer = item["main"] + 2 * item["margin"]
                needed = outer + (gap_main if current else 0)
                if current and used + needed > content_main:
                    lines.append(current)
                    current = []
                    used = 0
                current.append(item)
                used += outer + (gap_main if len(current) > 1 else 0)
            if current:
                lines.append(current)

            line_crosses = [
                max(item["cross"] + 2 * item["margin"] for item in line) for line in lines
            ]
            total_cross = sum(line_crosses) + gap_cross * max(0, len(lines) - 1)
            cross_pos = alignment_offset(
                cross_align if cross_align in ("CENTER", "END") else "START",
                max(0, content_cross - total_cross),
            )
            for line, line_cross in zip(lines, line_crosses):
                finalize(line, line_cross, cross_pos)
                cross_pos += line_cross + gap_cross
            return result

        def grid_cell(child: WidgetNode, count: int, axis: str) -> tuple[int, int]:
            props = child.props or {}
            key = "grid_col" if axis == "col" else "grid_row"
            span_key = "grid_col_span" if axis == "col" else "grid_row_span"
            try:
                position = int(props.get(key, 0) or 0)
            except (TypeError, ValueError):
                position = 0
            try:
                span = int(props.get(span_key, 1) or 1)
            except (TypeError, ValueError):
                span = 1
            position = max(0, min(count - 1, position))
            span = max(1, min(count - position, span))
            return position, span

        def place_grid(
            parent: WidgetNode,
            children: list[WidgetNode],
            content_x: int,
            content_y: int,
            content_w: int,
            content_h: int,
        ) -> dict[str, tuple[int, int, int, int]]:
            props = parent.props or {}
            columns = parse_grid_tracks(props.get("grid_columns", "1fr, 1fr")) or [("fr", 1)]
            rows = parse_grid_tracks(props.get("grid_rows", "1fr")) or [("fr", 1)]
            try:
                gap_column = max(0, int(props.get("pad_column", 0) or 0))
                gap_row = max(0, int(props.get("pad_row", 0) or 0))
            except (TypeError, ValueError):
                gap_column = gap_row = 0

            column_content = [0] * len(columns)
            row_content = [0] * len(rows)
            for child in children:
                child_w, child_h = size_of(child, content_w, content_h)
                column, column_span = grid_cell(child, len(columns), "col")
                row, row_span = grid_cell(child, len(rows), "row")
                column_share = max(1, math.ceil(child_w / column_span))
                row_share = max(1, math.ceil(child_h / row_span))
                for index in range(column, column + column_span):
                    column_content[index] = max(column_content[index], column_share)
                for index in range(row, row + row_span):
                    row_content[index] = max(row_content[index], row_share)

            column_sizes, column_offsets, column_gap = resolve_grid_tracks(
                columns,
                content_w,
                gap_column,
                column_content,
                str(props.get("grid_col_align", "START") or "START"),
            )
            row_sizes, row_offsets, row_gap = resolve_grid_tracks(
                rows,
                content_h,
                gap_row,
                row_content,
                str(props.get("grid_row_align", "START") or "START"),
            )

            result: dict[str, tuple[int, int, int, int]] = {}
            for child in children:
                child_w, child_h = size_of(child, content_w, content_h)
                column, column_span = grid_cell(child, len(columns), "col")
                row, row_span = grid_cell(child, len(rows), "row")

                cell_x = content_x + column_offsets[column]
                cell_y = content_y + row_offsets[row]
                cell_w = sum(column_sizes[column : column + column_span]) + column_gap * (column_span - 1)
                cell_h = sum(row_sizes[row : row + row_span]) + row_gap * (row_span - 1)

                child_props = child.props or {}
                column_align = str(child_props.get("cell_col_align", "STRETCH") or "STRETCH")
                row_align = str(child_props.get("cell_row_align", "STRETCH") or "STRETCH")

                if column_align == "STRETCH":
                    x, w = cell_x, cell_w
                else:
                    w = min(child_w, cell_w)
                    if column_align == "CENTER":
                        x = cell_x + (cell_w - w) // 2
                    elif column_align == "END":
                        x = cell_x + cell_w - w
                    else:
                        x = cell_x

                if row_align == "STRETCH":
                    y, h = cell_y, cell_h
                else:
                    h = min(child_h, cell_h)
                    if row_align == "CENTER":
                        y = cell_y + (cell_h - h) // 2
                    elif row_align == "END":
                        y = cell_y + cell_h - h
                    else:
                        y = cell_y

                result[child.id] = (x, y, max(1, w), max(1, h))
            return result

        def visit(parent_id: str | None, content_x: int, content_y: int, content_w: int, content_h: int) -> None:
            children = self.children(parent_id)
            parent = self.find(parent_id)
            layout = "NONE"
            if parent is not None:
                layout = str((parent.props or {}).get("layout", "NONE") or "NONE")

            # A Tabview's children live in the active tab's content, below or above
            # the tab bar. The stored child coordinates are relative to that content.
            if parent is not None and parent.type == "Tabview":
                try:
                    bar = max(0, int((parent.props or {}).get("tab_bar_size", 44) or 44))
                except (TypeError, ValueError):
                    bar = 44
                bar = min(content_h, bar)
                position = str((parent.props or {}).get("tab_bar_position", "TOP") or "TOP")
                if position == "BOTTOM":
                    content_h = max(1, content_h - bar)
                else:
                    content_y += bar
                    content_h = max(1, content_h - bar)

            if parent is not None and layout == "FLEX" and children:
                placed = place_flex(parent, children, content_x, content_y, content_w, content_h)
                for child in children:
                    x, y, w, h = placed[child.id]
                    rects[child.id] = (x, y, w, h)
                    pad = content_offset(self, child)
                    visit(child.id, x + pad, y + pad, max(1, w - 2 * pad), max(1, h - 2 * pad))
                return
            if parent is not None and layout == "GRID" and children:
                placed = place_grid(parent, children, content_x, content_y, content_w, content_h)
                for child in children:
                    x, y, w, h = placed[child.id]
                    rects[child.id] = (x, y, w, h)
                    pad = content_offset(self, child)
                    visit(child.id, x + pad, y + pad, max(1, w - 2 * pad), max(1, h - 2 * pad))
                return

            for child in children:
                w, h = size_of(child, content_w, content_h)
                x = content_x + child.x
                y = content_y + child.y
                rects[child.id] = (x, y, max(1, w), max(1, h))
                pad = content_offset(self, child)
                visit(
                    child.id,
                    x + pad,
                    y + pad,
                    max(1, w - 2 * pad),
                    max(1, h - 2 * pad),
                )

        visit(None, 0, 0, width, height)
        return rects

    def scale(self, factor_x: float, factor_y: float) -> None:
        """Scale every widget's position and size, parents included.

        Style values (padding, border width) are theme-driven, so they are not
        scaled; a profile change remaps the theme paddings instead.
        """
        for widget in self.widgets:
            widget.x = int(round(widget.x * factor_x))
            widget.y = int(round(widget.y * factor_y))
            props = widget.props or {}
            # Content-sized axes are derived from text and children, so scaling
            # them would only make the stored box disagree with the content.
            if not props.get("w_content"):
                widget.w = max(8, int(round(widget.w * factor_x)))
            if not props.get("h_content"):
                widget.h = max(8, int(round(widget.h * factor_y)))
            for key in ("grid_columns", "grid_rows"):
                value = props.get(key)
                if value:
                    props[key] = scale_grid_tracks(value, factor_x)

    def apply_theme_paddings(self, old_theme: MaterialTheme, new_theme: MaterialTheme) -> None:
        """Move container padding and gaps that came from the old theme to the new one.

        A container's padding is often the theme's container token (the designer
        stores concrete pixels). When the theme changes — a profile switch or a
        spacing slider — a value that still matches the old token is presumed to
        be theme-derived and follows the new theme. Custom values are left alone.
        """
        old_pad, old_gap = old_theme.container_pad(), old_theme.container_gap()
        new_pad, new_gap = new_theme.container_pad(), new_theme.container_gap()
        if old_pad == new_pad and old_gap == new_gap:
            return
        for widget in self.widgets:
            props = widget.props or {}
            if old_pad != new_pad and props.get("pad_all") == old_pad:
                props["pad_all"] = new_pad
            if old_gap != new_gap:
                if props.get("pad_row") == old_gap:
                    props["pad_row"] = new_gap
                if props.get("pad_column") == old_gap:
                    props["pad_column"] = new_gap

    def scale_to(self, old_size: tuple[int, int], new_size: tuple[int, int]) -> None:
        """Uniformly scale into a new content size, keeping the top-left anchor."""
        old_w, old_h = max(1, old_size[0]), max(1, old_size[1])
        new_w, new_h = max(1, new_size[0]), max(1, new_size[1])
        factor = min(new_w / old_w, new_h / old_h)
        self.scale(factor, factor)

    # ---- mutations --------------------------------------------------------

    def _next_id(self, type_name: str) -> str:
        prefix = type_name.lower()
        index = 1
        existing = {widget.id for widget in self.widgets}
        while True:
            candidate = f"{prefix}_{index}"
            if candidate not in existing:
                return candidate
            index += 1

    def add_widget(self, type_name: str, parent: str | None = None) -> WidgetNode:
        spec = SPEC_BY_TYPE.get(type_name) or SPEC_BY_TYPE["Object"]
        widget_id = self._next_id(type_name)
        siblings = self.children(parent)
        offset = 16 + 12 * len(siblings)
        widget = WidgetNode(
            id=widget_id,
            type=spec.type,
            parent=parent,
            x=offset,
            y=offset,
            w=spec.default_width,
            h=spec.default_height,
            props=spec.default_props(),
        )
        self.widgets.append(widget)
        self.place_widget_in_grid(widget)
        return widget

    def place_widget_in_grid(self, widget: WidgetNode) -> None:
        """Place a widget in the next free cell when its parent is a grid."""
        parent = self.find(widget.parent)
        if parent is None or str((parent.props or {}).get("layout", "NONE")) != "GRID":
            return

        columns = parse_grid_tracks((parent.props or {}).get("grid_columns", "1fr, 1fr"))
        count = max(1, len(columns) or 1)
        siblings = self.children(parent.id)
        try:
            index = siblings.index(widget)
        except ValueError:
            index = len(siblings) - 1
        index = max(0, index)

        row = index // count
        rows = parse_grid_tracks((parent.props or {}).get("grid_rows", "1fr"))
        if row >= len(rows):
            existing = str((parent.props or {}).get("grid_rows", "1fr") or "1fr").strip()
            extra = ", ".join(["1fr"] * (row + 1 - len(rows)))
            parent.props["grid_rows"] = f"{existing}, {extra}".strip(", ")

        widget.props["grid_col"] = index % count
        widget.props["grid_row"] = row
        widget.props.setdefault("grid_col_span", 1)
        widget.props.setdefault("grid_row_span", 1)
        widget.props.setdefault("cell_col_align", "STRETCH")
        widget.props.setdefault("cell_row_align", "STRETCH")

    def ensure_grid_layout(self, widget_id: str) -> bool:
        """Give a container valid tracks and a cell for every current child."""
        widget = self.find(widget_id)
        if widget is None or not self.is_container(widget_id):
            return False

        props = widget.props
        if not parse_grid_tracks(props.get("grid_columns")):
            props["grid_columns"] = "1fr, 1fr"
        if not parse_grid_tracks(props.get("grid_rows")):
            props["grid_rows"] = "1fr"

        columns = parse_grid_tracks(props.get("grid_columns"))
        count = max(1, len(columns) or 1)
        children = self.children(widget_id)
        needed_rows = max(1, math.ceil(len(children) / count))
        rows = parse_grid_tracks(props.get("grid_rows"))
        if needed_rows > len(rows):
            extra = ", ".join(["1fr"] * (needed_rows - len(rows)))
            props["grid_rows"] = f"{props['grid_rows']}, {extra}".strip(", ")

        for index, child in enumerate(children):
            child.props["grid_col"] = index % count
            child.props["grid_row"] = index // count
            child.props.setdefault("grid_col_span", 1)
            child.props.setdefault("grid_row_span", 1)
            child.props.setdefault("cell_col_align", "STRETCH")
            child.props.setdefault("cell_row_align", "STRETCH")
        return True

    def remove_widget(self, widget_id: str) -> None:
        doomed = {widget_id} | {widget.id for widget in self.descendants(widget_id)}
        self.widgets = [widget for widget in self.widgets if widget.id not in doomed]

    def duplicate_widget(self, widget_id: str) -> WidgetNode | None:
        source = self.find(widget_id)
        if source is None:
            return None

        clone = copy.deepcopy(source)
        clone.id = self._next_id(source.type)
        clone.x += 16
        clone.y += 16
        self.widgets.append(clone)
        self.place_widget_in_grid(clone)
        return clone

    def reparent(self, widget_id: str, parent: str | None) -> bool:
        widget = self.find(widget_id)
        if widget is None:
            return False
        if parent == widget_id or (parent and parent in {w.id for w in self.descendants(widget_id)}):
            return False
        if parent is not None and not self.find(parent):
            return False
        widget.parent = parent
        self.place_widget_in_grid(widget)
        return True

    def move_widget(
        self,
        widget_id: str,
        parent: str | None,
        after_id: str | None = None,
    ) -> bool:
        """Reparent and/or reorder a widget, keeping its canvas position."""
        widget = self.find(widget_id)
        if widget is None:
            return False
        if parent == widget_id or (parent and parent in {w.id for w in self.descendants(widget_id)}):
            return False
        if parent is not None and (not self.find(parent) or not self.is_container(parent)):
            return False

        anchor = self.find(after_id) if after_id else None
        if after_id is not None and anchor is None:
            return False
        if anchor is not None:
            parent = anchor.parent

        absolute = self.abs_rect(widget)
        self.widgets.remove(widget)
        widget.parent = parent

        if anchor is not None:
            index = next(i for i, item in enumerate(self.widgets) if item.id == anchor.id)
            self.widgets.insert(index + 1, widget)
        else:
            self.widgets.append(widget)

        new_parent = self.find(parent)
        base_x = base_y = 0
        if new_parent is not None:
            parent_x, parent_y, _w, _h = self.abs_rect(new_parent)
            pad = content_offset(self, new_parent)
            base_x = parent_x + pad
            base_y = parent_y + pad
        widget.x = absolute[0] - base_x
        widget.y = absolute[1] - base_y
        self.place_widget_in_grid(widget)
        return True

    def move_to_end(self, widget_id: str) -> bool:
        widget = self.find(widget_id)
        if widget is None:
            return False
        self.widgets.remove(widget)
        self.widgets.append(widget)
        return True

    def move_to_start(self, widget_id: str) -> bool:
        widget = self.find(widget_id)
        if widget is None:
            return False
        self.widgets.remove(widget)
        self.widgets.insert(0, widget)
        return True

    def rename_widget(self, old_id: str, new_id: str) -> bool:
        if old_id == new_id or self.find(new_id) is not None:
            return False
        widget = self.find(old_id)
        if widget is None:
            return False
        widget.id = new_id
        for item in self.widgets:
            if item.parent == old_id:
                item.parent = new_id
        return True

    # ---- persistence ------------------------------------------------------

    def to_dict(self) -> dict:
        return {
            "version": 1,
            "profile": self.profile,
            "theme": self.theme.to_dict(),
            "widgets": [widget.to_dict() for widget in self.widgets],
        }

    def save(self, path: Path) -> None:
        path.write_text(json.dumps(self.to_dict(), indent=2) + "\n", encoding="utf-8")

    @classmethod
    def load(cls, path: Path) -> "Project":
        data = json.loads(path.read_text(encoding="utf-8"))
        profile_id = str(data.get("profile", "tab5-landscape"))
        theme_data = dict(data.get("theme", {}))
        theme = ThemeSettings.from_dict(theme_data)
        # Projects saved before these keys existed inherit the profile defaults.
        from profiles import find_profile  # local import keeps the module graph acyclic

        defaults = find_profile(profile_id).default_theme()
        if "font_size" not in theme_data:
            theme.font_size = defaults.font_size
        if "mono" not in theme_data:
            theme.mono = defaults.mono
        if "spacing" not in theme_data:
            theme.spacing = defaults.spacing
        if "size" not in theme_data:
            theme.size = defaults.size
        project = cls(profile=profile_id, theme=theme)
        project.widgets = [WidgetNode.from_dict(item) for item in data.get("widgets", [])]
        project._repair()
        return project

    def _repair(self) -> None:
        known = {widget.id for widget in self.widgets}
        for widget in self.widgets:
            if widget.parent not in known and widget.parent is not None:
                widget.parent = None
            # Older projects stored -1 for "theme default" radius/border.
            for key in ("radius", "border_width"):
                value = widget.props.get(key)
                if value in ("", None):
                    widget.props.pop(key, None)
                else:
                    try:
                        if int(value) < 0:
                            widget.props.pop(key, None)
                    except (TypeError, ValueError):
                        widget.props.pop(key, None)


def content_offset(project: Project, widget: WidgetNode | None) -> int:
    """Padding plus border between a container's edge and its children."""
    if widget is None:
        return 0
    spec = SPEC_BY_TYPE.get(widget.type)
    if spec is None or not spec.container:
        return 0
    try:
        pad = max(0, int(widget.props.get("pad_all", 0) or 0))
    except (TypeError, ValueError):
        pad = 0

    border = widget.props.get("border_width")
    if border is None or border == "":
        theme = project._material()
        border = theme.card_border_width() if widget.type == "Card" else theme.container_border_width()
    try:
        border = max(0, int(border))
    except (TypeError, ValueError):
        border = 0
    return pad + border
