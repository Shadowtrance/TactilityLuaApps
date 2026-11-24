"""Generate a Lua script from a designer project."""

from __future__ import annotations

import re
from dataclasses import dataclass

from model import Project, WidgetNode, parse_grid_tracks
from widgets import SPEC_BY_TYPE

LUA_KEYWORDS = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
    "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
    "true", "until", "while",
}


def lua_string(value: object) -> str:
    text = str(value)
    text = text.replace("\\", "\\\\").replace('"', '\\"')
    text = text.replace("\r", "").replace("\n", "\\n")
    return f'"{text}"'


def lua_color(value: object) -> str:
    text = str(value).strip()
    if not text:
        return ""
    if text.startswith("#") and len(text) == 7:
        return "0x" + text[1:].lower()
    return text


def _int_prop(props: dict, key: str, default: int, minimum: int | None = None, maximum: int | None = None) -> int:
    try:
        value = int(props.get(key, default))
    except (TypeError, ValueError):
        value = default
    if minimum is not None:
        value = max(minimum, value)
    if maximum is not None:
        value = min(maximum, value)
    return value


def _csv(value: object) -> list[str]:
    return [part.strip() for part in str(value or "").replace(";", ",").split(",") if part.strip()]


def _int_csv(value: object) -> list[int]:
    numbers: list[int] = []
    for part in _csv(value):
        try:
            numbers.append(int(part))
        except ValueError:
            continue
    return numbers


def _float_csv(value: object) -> list[float]:
    numbers: list[float] = []
    for part in _csv(value):
        try:
            numbers.append(float(part))
        except ValueError:
            continue
    return numbers


def _string_matrix(value: object) -> list[list[str]]:
    rows: list[list[str]] = []
    for line in str(value or "").splitlines():
        line = line.strip()
        if line:
            rows.append(_csv(line))
    return rows


def _cell_matrix(value: object) -> list[list[str]]:
    rows: list[list[str]] = []
    for line in str(value or "").splitlines():
        if line.strip():
            rows.append([cell.strip() for cell in line.split("|")])
    return rows


def variable_name(widget: WidgetNode) -> str:
    name = re.sub(r"[^A-Za-z0-9_]", "_", widget.id)
    if not name or name[0].isdigit():
        name = f"w_{name}"
    if name in LUA_KEYWORDS:
        name = f"w_{name}"
    return name


def _prop_value(key: str, value: object) -> str:
    if key == "text" and isinstance(value, str) and value.startswith("lvgl.ICON."):
        return value
    if key in ("bg_color", "border_color", "text_color", "led_color"):
        return lua_color(value)
    if key == "bg_opa":
        return f"lvgl.OPA.{value}"
    if key == "long_mode":
        # "NONE" means "leave the widget default"; constraining it to
        # lvgl.LONG.NONE would reference a constant that does not exist.
        return "" if value == "NONE" else f"lvgl.LONG.{value}"
    if key == "text_align":
        return "" if value == "LEFT" else f"lvgl.TEXT_ALIGN.{value}"
    if key == "text_font":
        return f"'{value}'"
    if key == "variant":
        return f"lvgl.ICON_BUTTON.{value}"
    if key in ("checked", "one_line", "password_mode", "vertical", "on", "label_show"):
        return "true" if value else "false"
    if isinstance(value, str):
        return lua_string(value)
    return str(value)


def _size(key: str, pct_key: str, value: int, props: dict) -> str:
    if props.get(f"{key}_content"):
        return f"{key} = lvgl.SIZE_CONTENT"
    try:
        pct = max(0, min(100, int(props.get(pct_key, 0) or 0)))
    except (TypeError, ValueError):
        pct = 0
    return f"{key} = lvgl.PCT({pct})" if pct else f"{key} = {value}"


def _flex_grow(project: Project, widget: WidgetNode) -> int:
    """Flex grow only makes sense inside a fixed-size flex parent.

    A content-sized flex parent derives its main size from the children; letting a child
    grow would make that size depend on itself, and LVGL resolves the cycle to zero.
    """
    props = widget.props or {}
    try:
        grow = max(0, int(props.get("flex_grow", 0) or 0))
    except (TypeError, ValueError):
        grow = 0
    if grow == 0:
        return 0

    parent = project.find(widget.parent)
    if parent is None:
        return 0
    parent_props = parent.props or {}
    if str(parent_props.get("layout", "NONE") or "NONE") != "FLEX":
        return 0

    row = str(parent_props.get("flex_flow", "ROW") or "ROW").startswith("ROW")
    content_key = "w_content" if row else "h_content"
    if parent_props.get(content_key):
        return 0
    return grow


def _geometry(widget: WidgetNode) -> list[str]:
    props = widget.props or {}
    return [
        f"x = {widget.x}",
        f"y = {widget.y}",
        _size("w", "w_pct", widget.w, props),
        _size("h", "h_pct", widget.h, props),
    ]


def _grid_tracks(value: object) -> str:
    tracks = parse_grid_tracks(value) or [("fr", 1)]
    entries = []
    for kind, number in tracks:
        if kind == "fr":
            entries.append(f"lvgl.GRID_FR({max(1, number)})")
        elif kind == "content":
            entries.append("lvgl.GRID_CONTENT")
        else:
            entries.append(str(max(0, number)))
    return "{ " + ", ".join(entries) + " }"


def _grid_cell_values(project: Project, widget: WidgetNode) -> tuple[int, int, int, int, str, str]:
    props = widget.props or {}
    parent = project.find(widget.parent)
    parent_props = (parent.props or {}) if parent is not None else {}
    columns = parse_grid_tracks(parent_props.get("grid_columns", "1fr, 1fr")) or [("fr", 1)]
    rows = parse_grid_tracks(parent_props.get("grid_rows", "1fr")) or [("fr", 1)]

    def read(key: str, default: int, minimum: int) -> int:
        try:
            return max(minimum, int(props.get(key, default) or default))
        except (TypeError, ValueError):
            return default

    column = min(read("grid_col", 0, 0), max(0, len(columns) - 1))
    row = min(read("grid_row", 0, 0), max(0, len(rows) - 1))
    column_span = max(1, min(read("grid_col_span", 1, 1), len(columns) - column))
    row_span = max(1, min(read("grid_row_span", 1, 1), len(rows) - row))
    column_align = str(props.get("cell_col_align", "STRETCH") or "STRETCH")
    row_align = str(props.get("cell_row_align", "STRETCH") or "STRETCH")
    return column, column_span, row, row_span, column_align, row_align


def _table(entries: list[str], indent: int = 4) -> str:
    if not entries:
        return "{}"
    pad = " " * indent
    inner = " " * (indent + 4)
    return "{\n" + ",\n".join(f"{inner}{entry}" for entry in entries) + f",\n{pad}" + "}"


@dataclass
class _ExportState:
    lines: list[str]
    variables: dict[str, str]
    # Grid setup is applied after the subtree exists. Applying it first makes LVGL
    # measure `LV_GRID_CONTENT` tracks before the children contribute a size.
    deferred_grid: list[str]


def _add_props(entries: list[str], props: dict, keys: tuple[str, ...]) -> None:
    for key in keys:
        value = props.get(key)
        if value is None or value == "":
            continue
        if key in ("radius", "border_width") and isinstance(value, int) and value < 0:
            continue
        expression = _prop_value(key, value)
        if expression:
            entries.append(f"{key} = {expression}")


def _is_icon_text(props: dict) -> bool:
    text = props.get("text")
    return isinstance(text, str) and text.startswith("lvgl.ICON.")


def _text_props(entries: list[str], props: dict) -> None:
    icon = _is_icon_text(props)
    if icon:
        _add_props(entries, props, ("text", "text_color", "text_align", "long_mode"))
        entries.append("text_font = 'icon'")
    else:
        _add_props(
            entries,
            props,
            ("text", "text_color", "text_font", "text_align", "long_mode"),
        )


def _emit_widget(project: Project, widget: WidgetNode, state: _ExportState) -> None:
    spec = SPEC_BY_TYPE.get(widget.type) or SPEC_BY_TYPE["Object"]
    var = variable_name(widget)
    state.variables[widget.id] = var
    parent = state.variables.get(widget.parent, "canvas") if widget.parent else "canvas"
    props = widget.props or {}

    if widget.type == "SliderBox":
        minimum = int(props.get("min", 0))
        maximum = int(props.get("max", 100))
        step = max(1, int(props.get("step", 5)))
        value = int(props.get("value", minimum))
        state.lines.append(
            f"    local {var} = lvgl.SliderBox({parent}, {minimum}, {maximum}, {step}, {value})"
        )
        grow = _flex_grow(project, widget)
        state.lines.append(
            f"    {var}:set{{ x = {widget.x}, y = {widget.y}, "
            f"{_size('w', 'w_pct', widget.w, props)}, "
            f"{_size('h', 'h_pct', widget.h, props)}"
            + (f", flex_grow = {grow}" if grow > 0 else "")
            + " }"
        )
    elif widget.type == "Spinbox":
        minimum = int(props.get("min", 0))
        maximum = int(props.get("max", 100))
        step = max(1, int(props.get("step", 1)))
        digits = max(1, min(9, int(props.get("digits", 2))))
        decimals = max(0, min(9, int(props.get("decimals", 0))))
        value = int(props.get("value", minimum))
        entries = _geometry(widget)
        entries.append(f"range = {{ min = {minimum}, max = {maximum} }}")
        entries.append(f"spin_step = {step}")
        entries.append(f"digit_format = {{ digits = {digits}, decimals = {decimals} }}")
        entries.append("text_align = lvgl.TEXT_ALIGN.CENTER")
        _add_props(entries, props, ("text_color", "text_font", "radius"))
        state.lines.append(f"    local {var} = lvgl.Spinbox({parent})")
        state.lines.append(f"    {var}:set{{" + ", ".join(entries) + "}")
        # The range and digit format must be in place before the value is set,
        # and Lua table iteration is unordered, so the value comes last.
        state.lines.append(f"    {var}:set{{ value = {value} }}")
    else:
        entries = _geometry(widget)

        if spec.container:
            layout = str(props.get("layout", "NONE") or "NONE")
            entries.append(f"layout = lvgl.LAYOUT.{layout}")
            entries.append(f"pad_all = {int(props.get('pad_all', 0) or 0)}")
            if layout in ("FLEX", "GRID"):
                entries.append(f"pad_row = {int(props.get('pad_row', 0) or 0)}")
                entries.append(f"pad_column = {int(props.get('pad_column', 0) or 0)}")
            if layout == "FLEX":
                flow = str(props.get("flex_flow", "ROW") or "ROW")
                main = str(props.get("flex_main", "START") or "START")
                cross = str(props.get("flex_cross", "START") or "START")
                track = str(props.get("flex_track", "START") or "START")
                entries.append(f"flex_flow = lvgl.FLEX_FLOW.{flow}")
                entries.append(
                    "flex_align = { "
                    f"main = lvgl.FLEX_ALIGN.{main}, "
                    f"cross = lvgl.FLEX_ALIGN.{cross}, "
                    f"track = lvgl.FLEX_ALIGN.{track} "
                    "}"
                )

        grow = _flex_grow(project, widget)
        if grow > 0:
            entries.append(f"flex_grow = {grow}")

        if widget.type in ("Card", "Object"):
            _add_props(
                entries,
                props,
                ("bg_color", "bg_opa", "border_width", "border_color", "radius"),
            )
        elif widget.type in ("Label", "Chip", "Checkbox"):
            _text_props(entries, props)
            _add_props(
                entries,
                props,
                ("checked", "radius", "bg_color", "border_width", "border_color"),
            )
            if widget.type == "Chip" and props.get("bg_color"):
                entries.append("bg_opa = lvgl.OPA.COVER")
        elif widget.type in ("Button", "TextButton", "IconButton"):
            # Buttons are plain containers in LVGL; their caption is a child label.
            _add_props(entries, props, ("variant", "radius"))
        elif widget.type in ("Slider", "Bar"):
            _add_props(entries, props, ("min", "max", "value"))
        elif widget.type == "Switch":
            _add_props(entries, props, ("checked",))
        elif widget.type in ("Dropdown", "Roller"):
            options = [
                line.strip()
                for line in str(props.get("options", "")).splitlines()
                if line.strip()
            ]
            if options:
                entries.append(
                    "options = { " + ", ".join(lua_string(option) for option in options) + " }"
                )
            _add_props(entries, props, ("value", "radius"))
        elif widget.type == "TextArea":
            _add_props(
                entries,
                props,
                ("text", "placeholder", "one_line", "password_mode", "text_color", "text_font", "text_align", "radius"),
            )
        elif widget.type == "List":
            _add_props(entries, props, ("radius",))
        elif widget.type == "Table":
            rows = _int_prop(props, "rows", 3, 1, 100)
            columns = _int_prop(props, "columns", 2, 1, 20)
            entries.append(f"rows = {rows}")
            entries.append(f"columns = {columns}")
            widths = _int_csv(props.get("column_widths"))[:columns]
            if widths:
                entries.append("column_widths = { " + ", ".join(str(width) for width in widths) + " }")
            _add_props(entries, props, ("text_color", "text_font", "radius"))
        elif widget.type == "Chart":
            chart_type = str(props.get("chart_type", "LINE") or "LINE").upper()
            point_count = _int_prop(props, "point_count", 8, 2, 64)
            update_mode = str(props.get("update_mode", "SHIFT") or "SHIFT").upper()
            axis_min = _int_prop(props, "axis_min", 0)
            axis_max = _int_prop(props, "axis_max", 100)
            div_h = _int_prop(props, "div_h", 3, 1, 20)
            div_v = _int_prop(props, "div_v", 4, 1, 20)
            entries.append(f"chart_type = lvgl.CHART_TYPE.{chart_type}")
            entries.append(f"point_count = {point_count}")
            entries.append(f"update_mode = lvgl.CHART_UPDATE.{update_mode}")
            entries.append(f"div_lines = {{ h = {div_h}, v = {div_v} }}")
            entries.append(f"axis_range = {{ min = {axis_min}, max = {axis_max} }}")
        elif widget.type == "Calendar":
            today_year = _int_prop(props, "today_year", 2026, 1970, 2100)
            today_month = _int_prop(props, "today_month", 11, 1, 12)
            today_day = _int_prop(props, "today_day", 24, 1, 31)
            shown_year = _int_prop(props, "shown_year", today_year, 1970, 2100)
            shown_month = _int_prop(props, "shown_month", today_month, 1, 12)
            entries.append(
                f"today = {{ year = {today_year}, month = {today_month}, day = {today_day} }}"
            )
            entries.append(f"shown_date = {{ year = {shown_year}, month = {shown_month} }}")
            day_names = _csv(props.get("day_names"))[:7]
            if day_names:
                entries.append(
                    "day_names = { " + ", ".join(lua_string(name) for name in day_names) + " }"
                )
        elif widget.type == "Keyboard":
            mode = str(props.get("keyboard_mode", "TEXT_LOWER") or "TEXT_LOWER").upper()
            entries.append(f"keyboard_mode = lvgl.KEYBOARD_MODE.{mode}")
            entries.append(f"popovers = {'true' if props.get('popovers') else 'false'}")
        elif widget.type == "ButtonMatrix":
            rows = _string_matrix(props.get("map")) or [["1", "2"], ["3", "4"]]
            rows = [row for row in rows if row]
            entries.append(
                "map = { "
                + ", ".join(
                    "{ " + ", ".join(lua_string(cell) for cell in row) + " }" for row in rows
                )
                + " }"
            )
            if props.get("checkable", True):
                count = sum(len(row) for row in rows)
                entries.append(
                    "ctrl_map = { "
                    + ", ".join("lvgl.BUTTONMATRIX_CTRL.CHECKABLE" for _ in range(count))
                    + " }"
                )
            entries.append(f"one_checked = {'true' if props.get('one_checked') else 'false'}")
            selected = _int_prop(props, "selected_button", 0, 0)
            if selected > 0:
                entries.append(f"selected_button = {selected}")
        elif widget.type == "Tabs":
            mode = str(props.get("mode", "FIXED") or "FIXED")
            entries.append(f"mode = lvgl.TABS_MODE.{mode}")
        elif widget.type == "Statusline":
            entries.append(f"text = {lua_string(str(props.get('text', '') or 'All good'))}")
            level = str(props.get("level", "OK") or "OK")
            entries.append(f"level = lvgl.STATUSLINE_LEVEL.{level}")
        elif widget.type == "Arc":
            _add_props(entries, props, ("min", "max", "value", "arc_start", "arc_end"))
        elif widget.type == "LED":
            _add_props(entries, props, ("led_color", "brightness", "on"))
        elif widget.type == "Scale":
            _add_props(
                entries,
                props,
                ("min", "max", "tick_count", "major_tick_every", "label_show",
                 "scale_angle", "scale_rotation"),
            )
            mode = str(props.get("scale_mode", "HORIZONTAL_TOP") or "HORIZONTAL_TOP")
            entries.append(f"scale_mode = lvgl.SCALE_MODE.{mode}")
        elif widget.type == "ImageButton":
            _add_props(entries, props, ("radius",))
        elif widget.type == "Line":
            pass
        elif widget.type == "Image":
            _add_props(entries, props, ("src", "radius"))
        elif widget.type in ("Badge", "Spinner"):
            pass
        # List/PageIndicator/StepIndicator get their content after construction.

        state.lines.append(f"    local {var} = lvgl.{widget.type}({parent}, {_table(entries)})")

    if spec.container and str(props.get("layout", "NONE") or "NONE") == "GRID":
        columns = _grid_tracks(props.get("grid_columns", "1fr, 1fr"))
        rows = _grid_tracks(props.get("grid_rows", "1fr"))
        state.deferred_grid.append(f"    {var}:grid({columns}, {rows})")
        column_align = str(props.get("grid_col_align", "START") or "START")
        row_align = str(props.get("grid_row_align", "START") or "START")
        if column_align != "START" or row_align != "START":
            state.deferred_grid.append(
                f"    {var}:grid_align(lvgl.GRID_ALIGN.{column_align}, lvgl.GRID_ALIGN.{row_align})"
            )

    parent_widget = project.find(widget.parent)
    if (
        parent_widget is not None
        and str((parent_widget.props or {}).get("layout", "NONE") or "NONE") == "GRID"
    ):
        column, column_span, row, row_span, column_align, row_align = _grid_cell_values(project, widget)
        state.lines.append(
            f"    {var}:grid_cell(lvgl.GRID_ALIGN.{column_align}, {column}, {column_span}, "
            f"lvgl.GRID_ALIGN.{row_align}, {row}, {row_span})"
        )

    if widget.type in ("Button", "TextButton", "IconButton"):
        text = str(props.get("text", "") or ("OK" if widget.type == "IconButton" else "Button"))
        icon = text.startswith("lvgl.ICON.")
        label_entries = [
            f"text = {text if icon else lua_string(text)}",
            "align = lvgl.ALIGN.CENTER",
        ]
        _add_props(label_entries, props, ("text_color",))
        if icon:
            label_entries.append("text_font = 'icon'")
        else:
            _add_props(label_entries, props, ("text_font",))
        state.lines.append(f"    local {var}_label = lvgl.Label({var}, {_table(label_entries)})")

    if widget.type == "List":
        for line in str(props.get("items", "")).splitlines():
            line = line.strip()
            if not line:
                continue
            icon, separator, label = line.partition("|")
            if separator:
                state.lines.append(
                    f"    {var}:add_button("
                    f"{lua_string(label.strip())}, lvgl.ICON.{icon.strip().upper()})"
                )
            else:
                state.lines.append(f"    {var}:add_text({lua_string(line)})")
        state.lines.append(f"    {var}:hide_last_divider()")

    if widget.type == "Table":
        rows = _int_prop(props, "rows", 3, 1, 100)
        columns = _int_prop(props, "columns", 2, 1, 20)
        cells = _cell_matrix(props.get("cells"))
        for row_index, row in enumerate(cells[:rows], start=1):
            for column_index, text in enumerate(row[:columns], start=1):
                if text:
                    state.lines.append(
                        f"    {var}:set_cell({row_index}, {column_index}, {lua_string(text)})"
                    )

    if widget.type == "Chart":
        point_count = _int_prop(props, "point_count", 8, 2, 64)
        values = _float_csv(props.get("values"))[:point_count]
        if values:
            color = lua_color(props.get("series_color") or "#00aaff")
            formatted = ", ".join(str(int(round(value))) for value in values)
            state.lines.append(f"    local {var}_series = {var}:add_series({color})")
            state.lines.append(f"    {var}:set_values({var}_series, {{ {formatted} }})")
            state.lines.append(f"    {var}:refresh()")

    if widget.type == "Tabs":
        for line in str(props.get("items", "")).splitlines():
            line = line.strip()
            if not line:
                continue
            icon, separator, label = line.partition("|")
            if separator:
                state.lines.append(
                    f"    {var}:add_tab_with_icon("
                    f"lvgl.ICON.{icon.strip().upper()}, {lua_string(label.strip())})"
                )
            else:
                state.lines.append(f"    {var}:add_tab({lua_string(line)})")
        selected = int(props.get("selected", 1))
        if selected > 1:
            state.lines.append(f"    {var}:set_selected({selected})")

    if widget.type == "Line":
        values: list[str] = []
        for row in str(props.get("points", "")).splitlines():
            parts = [part.strip() for part in row.replace(";", ",").split(",")]
            if len(parts) < 2:
                continue
            values.extend(parts[:2])
        if len(values) >= 4:
            state.lines.append(f"    {var}:set_points{{ {', '.join(values)} }}")

    if widget.type == "ImageButton":
        src = str(props.get("src", "") or "")
        if src:
            state.lines.append(
                f"    {var}:set_image_src(lvgl.IMAGEBUTTON.RELEASED, {lua_string(src)})"
            )

    if widget.type == "PageIndicator":
        state.lines.append(f"    {var}:set_page_count({int(props.get('page_count', 5))})")
        state.lines.append(f"    {var}:set_page({int(props.get('page', 0))})")
        if props.get("vertical"):
            state.lines.append(f"    {var}:set_vertical(true)")

    if widget.type == "StepIndicator":
        state.lines.append(f"    {var}:set_step_count({int(props.get('step_count', 4))})")
        state.lines.append(f"    {var}:set_finished_count({int(props.get('finished_count', 0))})")

    # Event handlers. Every default event gets an empty stub so the generated
    # file is ready to fill in; explicit event code from the designer wins.
    event_names = list(spec.events)
    for name in widget.events:
        if name not in event_names:
            event_names.append(name)

    for event_name in event_names:
        code = str(widget.events.get(event_name, "") or "").strip()
        body = code if code else "-- TODO"
        body_lines = body.splitlines() or ["-- TODO"]
        state.lines.append(f"    {var}:on(lvgl.EVENT.{event_name}, function()")
        for line in body_lines:
            state.lines.append(f"        {line}" if line.strip() else "")
        state.lines.append("    end)")


def export(project: Project) -> str:
    state = _ExportState(lines=[], variables={}, deferred_grid=[])
    state.lines.extend([
        "-- Generated by lua-gui-designer. Edit freely after export.",
        "",
        "function on_show()",
        "    local screen = lvgl.root()",
        "",
        "    -- Absolute canvas: the runner's root is a flex column, so this wrapper",
        "    -- gives the designer's coordinates a stable surface.",
        "    local canvas = lvgl.Object(screen, {",
        "        w = lvgl.PCT(100),",
        "        h = lvgl.PCT(100),",
        "        layout = lvgl.LAYOUT.NONE,",
        "        pad_all = 0,",
        "        border_width = 0,",
        "        bg_opa = lvgl.OPA.TRANSP,",
        "    })",
        "",
    ])

    for widget in project.ordered():
        _emit_widget(project, widget, state)

    # Apply grids once every child exists, so `LV_GRID_CONTENT` tracks measure the
    # widgets instead of collapsing to zero.
    state.lines.extend(state.deferred_grid)

    # A content-sized top-level container is a scrolling page. Let the runner's
    # padded content object scroll it, like a C app: that puts the scrollbar on
    # the window edge instead of inside the content padding.
    scrolling_page = any(
        widget.parent is None and (widget.props or {}).get("h_content")
        for widget in project.widgets
    )
    if scrolling_page:
        state.lines.append("    canvas:set{ h = lvgl.SIZE_CONTENT }")
        # The runner's content object is the scroll container. Neither the canvas
        # nor the page may scroll on its own, or a gesture on empty space inside
        # them would stop there instead of reaching the content.
        state.lines.append("    canvas:remove_flag(lvgl.FLAG.SCROLLABLE)")
        for widget in project.widgets:
            if widget.parent is None and (widget.props or {}).get("h_content"):
                variable = state.variables.get(widget.id)
                if variable:
                    state.lines.append(
                        f"    {variable}:remove_flag(lvgl.FLAG.SCROLLABLE)"
                    )

    state.lines.extend([
        "    result = 'Generated by lua-gui-designer'",
        "end",
        "",
    ])
    return "\n".join(state.lines)
