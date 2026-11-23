"""Supersampled Pillow renderer for the Lua GUI designer.

Tk's canvas primitives are aliased and its rounded rectangles are hard to get
right at arbitrary zoom. The preview is drawn with Pillow at 2x and downsampled,
so curves and text stay smooth. The canvas only displays the resulting image;
selection handles are part of the same image, so hit-testing stays in the same
coordinate space.

Geometry follows the firmware:

- the status bar and toolbar heights come from `theme.py`;
- the root content object has the theme's container padding, and the exported
  canvas fills the area inside it;
- widget coordinates are relative to that canvas.
"""

from __future__ import annotations

import datetime

from PIL import Image, ImageDraw, ImageTk

from fonts import icon_font as _icon_font
from icons import SHARED_ICONS
from fonts import text_font
from model import Project, WidgetNode
from profiles import DeviceProfile
from theme import MaterialTheme, lerp, mix

MARGIN = 28
HANDLE = 7
SUPERSAMPLE = 2

ICON_CLOSE = "\ue5cd"
ICON_WIFI = "\ue63e"
ICON_BATTERY = "\uf24f"


def _font(theme: MaterialTheme, kind: str, scale: float, bold: bool = False):
    size = max(7, round(theme.font_px(kind) * scale))
    # `icon` is the firmware's shared Material Symbols font, not a text face.
    if kind == "icon":
        icon = _icon_font(size)
        if icon is not None:
            return icon
    return text_font(size, bold)


def _rounded(
    draw: ImageDraw.ImageDraw,
    x1: float,
    y1: float,
    x2: float,
    y2: float,
    radius: float,
    *,
    fill: str | None = None,
    outline: str | None = None,
    width: int = 1,
) -> None:
    if x2 < x1:
        x1, x2 = x2, x1
    if y2 < y1:
        y1, y2 = y2, y1
    radius = max(0, min(radius, (x2 - x1) / 2, (y2 - y1) / 2))
    draw.rounded_rectangle(
        [x1, y1, x2, y2],
        radius=radius,
        fill=fill,
        outline=outline,
        width=max(1, width) if outline else 0,
    )


def _dashed_rect(
    draw: ImageDraw.ImageDraw,
    x1: float,
    y1: float,
    x2: float,
    y2: float,
    color: str,
    width: int = 2,
    dash: int = 7,
) -> None:
    def segments(a: float, b: float):
        position = a
        while position < b:
            yield position, min(position + dash, b)
            position += dash * 2

    for start, end in segments(x1, x2):
        draw.line([start, y1, end, y1], fill=color, width=width)
        draw.line([start, y2, end, y2], fill=color, width=width)
    for start, end in segments(y1, y2):
        draw.line([x1, start, x1, end], fill=color, width=width)
        draw.line([x2, start, x2, end], fill=color, width=width)


def _text(
    draw: ImageDraw.ImageDraw,
    x: float,
    y: float,
    text: object,
    font,
    fill: str,
    anchor: str,
) -> None:
    text = str(text)
    if text:
        draw.text((x, y), text, font=font, fill=fill, anchor=anchor)


def _fit(draw: ImageDraw.ImageDraw, text: str, font, max_width: float) -> str:
    if max_width <= 0 or draw.textlength(text, font=font) <= max_width:
        return text
    ellipsis = "…"
    while text and draw.textlength(text + ellipsis, font=font) > max_width:
        text = text[:-1]
    return text + ellipsis


def _wrap(draw: ImageDraw.ImageDraw, text: str, font, max_width: float) -> list[str]:
    words = text.split()
    if not words:
        return [""]
    lines: list[str] = []
    current = ""
    for word in words:
        candidate = word if not current else f"{current} {word}"
        if draw.textlength(candidate, font=font) <= max_width or not current:
            current = candidate
        else:
            lines.append(current)
            current = word
    if current:
        lines.append(current)
    return lines


# LVGL's built-in symbols live in a private-use range that Adwaita Sans lacks, so
# they would show as boxes in the preview. Draw the two that the designer uses.
LV_SYMBOL_PLUS = "\uf067"
LV_SYMBOL_MINUS = "\uf068"


def _draw_symbol(
    draw: ImageDraw.ImageDraw,
    text: str,
    cx: float,
    cy: float,
    theme: MaterialTheme,
    scale: float,
    fill: str,
) -> bool:
    if text not in (LV_SYMBOL_PLUS, LV_SYMBOL_MINUS):
        return False
    size = max(7, round(theme.font_px("default") * scale))
    arm = size * 0.32
    thickness = max(2.0, size * 0.1)
    draw.rounded_rectangle(
        [cx - arm, cy - thickness / 2, cx + arm, cy + thickness / 2],
        radius=thickness / 2,
        fill=fill,
    )
    if text == LV_SYMBOL_PLUS:
        draw.rounded_rectangle(
            [cx - thickness / 2, cy - arm, cx + thickness / 2, cy + arm],
            radius=thickness / 2,
            fill=fill,
        )
    return True


def _fill_for(theme: MaterialTheme, props: dict, fallback: str | None) -> str | None:
    color = str(props.get("bg_color", "") or "")
    opa = str(props.get("bg_opa", "COVER") or "COVER")
    if opa == "TRANSP":
        return None
    if color:
        return mix(color, theme.surface_container_low, 50) if opa == "50" else color
    return fallback


def _stroke_width(props: dict, default: int = 1) -> int:
    value = props.get("border_width")
    if value is None or value == "":
        return default
    try:
        value = int(value)
    except (TypeError, ValueError):
        return default
    return default if value < 0 else value


def _radius_for(props: dict, default: int) -> int:
    value = props.get("radius")
    if value is None or value == "":
        return default
    try:
        value = int(value)
    except (TypeError, ValueError):
        return default
    return default if value < 0 else value


def _value_fraction(props: dict) -> float:
    try:
        minimum = float(props.get("min", 0))
        maximum = float(props.get("max", 100))
        value = float(props.get("value", 0))
    except (TypeError, ValueError):
        return 0.0
    if maximum <= minimum:
        return 0.0
    return max(0.0, min(1.0, (value - minimum) / (maximum - minimum)))


def content_origin(profile: DeviceProfile, theme: MaterialTheme, zoom: float) -> tuple[float, float]:
    """Canvas position in Tk coordinates for widget (0, 0)."""
    app_x, app_y, _app_w, _app_h = theme.app_bounds(profile.width, profile.height)
    pad = theme.content_padding()
    return (
        MARGIN + (app_x + pad) * zoom,
        MARGIN + (app_y + pad) * zoom,
    )


def canvas_size(profile: DeviceProfile, theme: MaterialTheme) -> tuple[int, int]:
    return theme.content_size(profile.width, profile.height)


def render(
    canvas,
    project: Project,
    profile: DeviceProfile,
    theme: MaterialTheme,
    zoom: float,
    selected_id: str | None,
    show_grid: bool = True,
    scroll_y: int = 0,
) -> None:
    final_w = round(profile.width * zoom) + MARGIN * 2
    final_h = round(profile.height * zoom) + MARGIN * 2
    if final_w <= 0 or final_h <= 0:
        return

    scale = zoom * SUPERSAMPLE
    width = final_w * SUPERSAMPLE
    height = final_h * SUPERSAMPLE
    image = Image.new("RGB", (width, height), "#0a0a0a")
    draw = ImageDraw.Draw(image)

    device_x = MARGIN * SUPERSAMPLE
    device_y = MARGIN * SUPERSAMPLE
    device_w = profile.width * scale
    device_h = profile.height * scale

    layout = theme.bars_layout(profile.width, profile.height)
    app_x, app_y, app_w, app_h = theme.app_bounds(profile.width, profile.height)
    statusbar_h = theme.statusbar_height()
    toolbar_h = theme.toolbar_height()
    pad = theme.content_padding()

    # The window surface is the low container tone; the status bar and toolbar
    # deliberately share it in the split layout.
    draw.rectangle(
        [device_x, device_y, device_x + device_w, device_y + device_h],
        fill=theme.surface_container_low,
    )

    if layout == "side":
        strip = theme.side_bar_width()
        strip_x = profile.width - strip if theme.settings.bars_align == "right" else 0
        _draw_side_statusbar(
            draw,
            theme,
            scale,
            device_x + strip_x * scale,
            device_y,
            strip * scale,
            device_h,
        )
        inner_x = (
            device_x + strip_x * scale
            if theme.settings.bars_align == "right"
            else device_x + (strip_x + strip) * scale
        )
        draw.line(
            [inner_x, device_y, inner_x, device_y + device_h],
            fill=theme.ink,
            width=1 * SUPERSAMPLE,
        )
        toolbar_x = app_x * scale
        toolbar_y = 0.0
    else:
        _draw_split_statusbar(
            draw,
            theme,
            scale,
            device_x,
            device_y,
            device_w,
            statusbar_h * scale,
        )
        toolbar_x = 0.0
        toolbar_y = statusbar_h * scale

    _draw_toolbar(
        draw,
        theme,
        scale,
        device_x + toolbar_x,
        device_y + toolbar_y,
        app_w * scale,
        toolbar_h * scale,
    )

    # Canvas origin: the app content area, inside the root's padding.
    canvas_x = device_x + (app_x + pad) * scale
    canvas_y = device_y + (app_y + pad) * scale
    canvas_w = max(1.0, (app_w - 2 * pad) * scale)
    canvas_h = max(1.0, (app_h - 2 * pad) * scale)

    canvas_width, canvas_height = theme.content_size(profile.width, profile.height)
    rects = project.layout_rects(canvas_width, canvas_height)
    scroll_px = max(0, int(scroll_y)) * scale

    # Widgets are drawn on their own layer so a scrolled page is clipped by the
    # content area instead of covering the status bar and toolbar.
    layer = Image.new(
        "RGBA",
        (max(1, int(round(canvas_w))), max(1, int(round(canvas_h)))),
        (0, 0, 0, 0),
    )
    ldraw = ImageDraw.Draw(layer)

    if show_grid:
        _draw_grid(ldraw, theme, scale, 0, 0, canvas_w, canvas_h)

    for widget in project.ordered():
        wx, wy, ww, wh = rects.get(widget.id, (widget.x, widget.y, widget.w, widget.h))
        _draw_widget(
            ldraw,
            project,
            widget,
            wx * scale,
            wy * scale - scroll_px,
            ww * scale,
            wh * scale,
            theme,
            scale,
        )

    for widget in project.out_of_bounds(canvas_width, canvas_height):
        x, y, w, h = rects.get(widget.id, project.abs_rect(widget))
        _dashed_rect(
            ldraw,
            x * scale,
            y * scale - scroll_px,
            (x + w) * scale,
            (y + h) * scale - scroll_px,
            theme.error,
            width=1 * SUPERSAMPLE,
            dash=4 * SUPERSAMPLE,
        )

    selected = project.find(selected_id)
    if selected is not None:
        x, y, w, h = rects.get(selected.id, project.abs_rect(selected))
        sx = x * scale
        sy = y * scale - scroll_px
        _dashed_rect(
            ldraw,
            sx,
            sy,
            sx + w * scale,
            sy + h * scale,
            theme.primary,
            width=2 * SUPERSAMPLE,
            dash=5 * SUPERSAMPLE,
        )
        for hx, hy in _handles(sx, sy, w * scale, h * scale):
            half = HANDLE * SUPERSAMPLE / 2
            ldraw.rectangle(
                [hx - half, hy - half, hx + half, hy + half],
                fill=theme.primary,
                outline=theme.on_primary,
                width=1 * SUPERSAMPLE,
            )

    image.paste(layer, (int(round(canvas_x)), int(round(canvas_y))), layer)

    # Content border last, so widgets don't cover it.
    draw.rectangle(
        [canvas_x, canvas_y, canvas_x + canvas_w, canvas_y + canvas_h],
        outline=theme.outline,
        width=1 * SUPERSAMPLE,
    )

    page_height = project.page_height(canvas_width, canvas_height)
    if page_height > canvas_height:
        max_scroll = page_height - canvas_height
        # The runner's padded content object owns the scrollbar, so it sits on
        # the window edge like a C app: 6 px wide, 6 px in from the sides.
        track_x = device_x + (app_x + app_w - 5) * scale
        track_top = device_y + (app_y + 6) * scale
        track_height = max(1.0, (app_h - 12) * scale)
        bar_height = max(24 * scale, track_height * app_h / (page_height + 2 * pad))
        bar_top = track_top + (track_height - bar_height) * (
            min(scroll_y, max_scroll) / max_scroll if max_scroll else 0
        )
        draw.rounded_rectangle(
            [track_x - 6 * scale, bar_top, track_x, bar_top + bar_height],
            radius=3 * scale,
            fill=theme.scrollbar_bar,
        )

    image = image.resize((final_w, final_h), Image.LANCZOS)
    canvas._image = image  # available to the UI test and diagnostics
    photo = ImageTk.PhotoImage(image)
    canvas._photo = photo  # keep the Tk image alive
    canvas.delete("all")
    canvas.create_image(0, 0, image=photo, anchor="nw")
    canvas.configure(scrollregion=(0, 0, final_w, final_h))


def _draw_split_statusbar(
    draw: ImageDraw.ImageDraw,
    theme: MaterialTheme,
    scale: float,
    x: float,
    y: float,
    w: float,
    h: float,
) -> None:
    time_text = datetime.datetime.now().strftime("%H:%M")
    _text(
        draw,
        x + 6 * scale,
        y + h / 2,
        time_text,
        _font(theme, "default", scale),
        theme.text,
        "lm",
    )

    icon = theme.statusbar_icon_px() * scale
    icon_gap = theme.statusbar_icon_gap() * scale
    right = x + w - 4 * scale
    centre_y = y + h / 2
    icon_font = _icon_font(round(icon))
    if icon_font is not None:
        _text(draw, right - icon / 2, centre_y, ICON_BATTERY, icon_font, theme.text, "mm")
        _text(
            draw,
            right - icon - icon_gap - icon / 2,
            centre_y,
            ICON_WIFI,
            icon_font,
            theme.text,
            "mm",
        )
    else:
        _draw_battery(
            draw,
            right - icon,
            centre_y - icon / 2,
            right,
            centre_y + icon / 2,
            theme.text,
            scale,
        )
        wifi_right = right - icon - icon_gap
        _draw_wifi(
            draw,
            wifi_right - icon,
            centre_y - icon / 2,
            wifi_right,
            centre_y + icon / 2,
            theme.text,
            scale,
        )


def _draw_side_statusbar(
    draw: ImageDraw.ImageDraw,
    theme: MaterialTheme,
    scale: float,
    x: float,
    y: float,
    w: float,
    h: float,
) -> None:
    draw.rectangle([x, y, x + w, y + h], fill=theme.side_bar_color)

    pad_top = lerp(0, 4, theme.settings.spacing) * scale
    small = _font(theme, "small", scale)
    time_text = datetime.datetime.now().strftime("%H:%M")
    _text(
        draw,
        x + w / 2,
        y + pad_top + theme.font_px("small") * scale / 2,
        time_text,
        small,
        theme.text,
        "mm",
    )

    icon = theme.statusbar_icon_px() * scale
    gap = theme.statusbar_icon_gap() * scale
    top = y + pad_top + theme.font_px("small") * scale + 10 * scale
    icon_font = _icon_font(round(icon))
    if icon_font is not None:
        _text(draw, x + w / 2, top + icon / 2, ICON_WIFI, icon_font, theme.text, "mm")
        _text(
            draw,
            x + w / 2,
            top + icon + gap + icon / 2,
            ICON_BATTERY,
            icon_font,
            theme.text,
            "mm",
        )
    else:
        _draw_wifi(
            draw,
            x + (w - icon) / 2,
            top,
            x + (w + icon) / 2,
            top + icon,
            theme.text,
            scale,
        )
        top += icon + gap
        _draw_battery(
            draw,
            x + (w - icon) / 2,
            top,
            x + (w + icon) / 2,
            top + icon,
            theme.text,
            scale,
        )


def _draw_toolbar(
    draw: ImageDraw.ImageDraw,
    theme: MaterialTheme,
    scale: float,
    x: float,
    y: float,
    w: float,
    h: float,
) -> None:
    pad_hor = theme.toolbar_pad_hor() * scale
    action = theme.toolbar_action_size() * scale
    close_x = x + pad_hor
    close_y = y + (h - action) / 2
    close_font = _icon_font(round(theme.font_px("default") * 1.25 * scale))
    if close_font is not None:
        _text(
            draw,
            close_x + action / 2,
            close_y + action / 2,
            ICON_CLOSE,
            close_font,
            theme.text,
            "mm",
        )
    else:
        stroke = max(1, round(2 * scale))
        inset = action * 0.32
        draw.line(
            [close_x + inset, close_y + inset, close_x + action - inset, close_y + action - inset],
            fill=theme.text,
            width=stroke,
        )
        draw.line(
            [close_x + action - inset, close_y + inset, close_x + inset, close_y + action - inset],
            fill=theme.text,
            width=stroke,
        )

    title_x = close_x + action + theme.toolbar_gap() * scale
    title_kind = "large" if theme.settings.size >= 50 else "default"
    title_font = _font(theme, title_kind, scale)
    _text(
        draw,
        title_x,
        y + h / 2,
        _fit(draw, "Lua Designer", title_font, max(8.0, x + w - pad_hor - title_x)),
        title_font,
        theme.text,
        "lm",
    )


def _draw_wifi(
    draw: ImageDraw.ImageDraw,
    x1: float,
    y1: float,
    x2: float,
    y2: float,
    color: str,
    scale: float,
) -> None:
    width = max(1, round(2 * scale))
    cx = (x1 + x2) / 2
    bottom = y2 - (y2 - y1) * 0.18
    radius = (x2 - x1) * 0.5
    for index, fraction in enumerate((1.0, 0.66, 0.34)):
        r = radius * fraction
        draw.arc(
            [cx - r, bottom - r, cx + r, bottom + r],
            start=225,
            end=315,
            fill=color,
            width=width,
        )
    dot = max(1.5, (x2 - x1) * 0.06)
    draw.ellipse([cx - dot, bottom - dot, cx + dot, bottom + dot], fill=color)


def _draw_battery(
    draw: ImageDraw.ImageDraw,
    x1: float,
    y1: float,
    x2: float,
    y2: float,
    color: str,
    scale: float,
) -> None:
    width = max(1, round(2 * scale))
    body_h = (y2 - y1) * 0.7
    top = (y1 + y2) / 2 - body_h / 2
    body_right = x2 - max(2, (x2 - x1) * 0.12)
    _rounded(draw, x1, top, body_right, top + body_h, body_h * 0.2, outline=color, width=width)
    nub_w = max(1.5, (x2 - x1) * 0.08)
    draw.rectangle(
        [body_right, top + body_h * 0.28, body_right + nub_w, top + body_h * 0.72],
        fill=color,
    )
    fill_left = x1 + width * 1.6
    fill_right = body_right - width * 1.6
    draw.rectangle(
        [fill_left, top + width * 1.6, fill_right, top + body_h - width * 1.6],
        fill=color,
    )


def _draw_grid(
    draw: ImageDraw.ImageDraw,
    theme: MaterialTheme,
    scale: float,
    x: float,
    y: float,
    w: float,
    h: float,
) -> None:
    step = 8 * scale
    major = step * 4
    if step < 3:
        return
    grid_color = mix(theme.ink, theme.paper, 16) if theme.mono else theme.outline_variant
    for offset in range(0, int(w) + 1, max(1, int(major))):
        draw.line(
            [x + offset, y, x + offset, y + h],
            fill=grid_color,
            width=1 * SUPERSAMPLE,
        )
    for offset in range(0, int(h) + 1, max(1, int(major))):
        draw.line(
            [x, y + offset, x + w, y + offset],
            fill=grid_color,
            width=1 * SUPERSAMPLE,
        )


def _draw_widget(
    draw: ImageDraw.ImageDraw,
    project: Project,
    widget: WidgetNode,
    x: float,
    y: float,
    w: float,
    h: float,
    theme: MaterialTheme,
    scale: float,
) -> None:
    props = widget.props or {}

    if widget.type in ("Card", "Object"):
        fill = _fill_for(
            theme,
            props,
            theme.card if widget.type == "Card" else None,
        )
        radius = _radius_for(
            props,
            theme.radius_card() if widget.type == "Card" else 0,
        ) * scale
        default_border = (
            theme.card_border_width()
            if widget.type == "Card"
            else theme.container_border_width()
        )
        border_width = _stroke_width(props, default_border)
        border_color = None if border_width <= 0 else str(
            props.get("border_color", "") or theme.outline
        )
        _rounded(
            draw,
            x,
            y,
            x + w,
            y + h,
            radius,
            fill=fill,
            outline=border_color,
            width=max(1, border_width) * SUPERSAMPLE,
        )
        return

    if widget.type == "Label":
        color = str(props.get("text_color", "") or theme.text)
        font = _font(theme, str(props.get("text_font", "default")), scale)
        text = str(props.get("text", "") or "")
        mode = str(props.get("long_mode", "NONE") or "NONE")
        align = str(props.get("text_align", "LEFT") or "LEFT")
        if mode == "WRAP":
            lines = _wrap(draw, text, font, w)
            line_height = theme.font_px(str(props.get("text_font", "default"))) * scale
            start = y + h / 2 - (len(lines) - 1) * line_height / 2
            for index, line in enumerate(lines):
                _text(draw, x, start + index * line_height, line, font, color, "lm")
        else:
            anchor = {"CENTER": "mm", "RIGHT": "rm"}.get(align, "lm")
            text_x = {"CENTER": x + w / 2, "RIGHT": x + w}.get(align, x)
            if _draw_symbol(draw, text, x + w / 2, y + h / 2, theme, scale, color):
                return
            _text(draw, text_x, y + h / 2, _fit(draw, text, font, w), font, color, anchor)
        return

    if widget.type in ("Button", "TextButton", "IconButton"):
        variant = str(props.get("variant", "STANDARD"))
        border = _stroke_width(props, 0)
        if theme.mono:
            if widget.type == "Button":
                fill, color, border = theme.paper, theme.ink, border or 1
            elif widget.type == "TextButton":
                fill, color = None, theme.ink
            elif variant == "FILLED":
                fill, color = theme.ink, theme.paper
            elif variant == "TONAL":
                fill, color, border = theme.paper, theme.ink, border or 1
            else:
                fill, color = None, theme.ink
            outline = (
                str(props.get("border_color", "") or theme.outline)
                if border > 0
                else None
            )
        else:
            if widget.type == "IconButton":
                if variant == "FILLED":
                    fill, color = theme.primary, theme.on_primary
                elif variant == "TONAL":
                    fill, color = theme.secondary_container, theme.on_secondary_container
                else:
                    fill, color = None, theme.text
            elif widget.type == "TextButton":
                fill, color = None, theme.primary
            else:
                fill, color = _fill_for(theme, props, theme.primary), theme.on_primary
            outline = (
                str(props.get("border_color", "") or theme.outline) if border > 0 else None
            )

        if fill or outline:
            _rounded(
                draw,
                x,
                y,
                x + w,
                y + h,
                _radius_for(props, theme.radius_control()) * scale,
                fill=fill,
                outline=outline,
                width=max(1, border) * SUPERSAMPLE,
            )
        text = str(props.get("text", "") or ("OK" if widget.type == "IconButton" else "Button"))
        if _draw_symbol(draw, text, x + w / 2, y + h / 2, theme, scale, color):
            return
        font = _font(theme, str(props.get("text_font", "default")), scale)
        _text(
            draw,
            x + w / 2,
            y + h / 2,
            _fit(draw, text, font, w - 8 * scale),
            font,
            color,
            "mm",
        )
        return

    if widget.type == "Chip":
        checked = bool(props.get("checked", False))
        if theme.mono:
            default_fill = theme.ink if checked else theme.paper
            default_outline = theme.ink
            default_border = 1
            color = theme.paper if checked else theme.ink
        else:
            default_fill = theme.secondary_container if checked else None
            default_outline = theme.secondary_container if checked else theme.outline
            default_border = 1
            color = theme.on_secondary_container if checked else theme.on_surface_variant
        fill = _fill_for(theme, props, default_fill)
        border = props.get("border_width")
        border = max(0, int(border)) if isinstance(border, int) else default_border
        outline = (
            str(props.get("border_color", "") or default_outline)
            if border > 0
            else None
        )
        _rounded(
            draw,
            x,
            y,
            x + w,
            y + h,
            _radius_for(props, theme.radius_chip()) * scale,
            fill=fill,
            outline=outline,
            width=max(1, border) * SUPERSAMPLE,
        )
        font = _font(theme, str(props.get("text_font", "default")), scale)
        text_color = str(props.get("text_color", "") or color)
        _text(
            draw,
            x + w / 2,
            y + h / 2,
            _fit(draw, str(props.get("text", "") or "Chip"), font, w - 8 * scale),
            font,
            text_color,
            "mm",
        )
        return

    if widget.type == "SliderBox":
        _draw_sliderbox(draw, props, x, y, w, h, theme, scale)
        return

    if widget.type in ("Bar", "Slider"):
        fraction = _value_fraction(props)
        if widget.type == "Bar":
            track_h = max(2.0, h * 0.7)
            top = y + (h - track_h) / 2
            _rounded(
                draw,
                x,
                top,
                x + w,
                top + track_h,
                track_h / 2,
                fill=theme.paper if theme.mono else theme.secondary_container,
                outline=theme.ink if theme.mono else None,
                width=1 * SUPERSAMPLE,
            )
            if fraction > 0:
                _rounded(
                    draw,
                    x,
                    top,
                    x + w * fraction,
                    top + track_h,
                    track_h / 2,
                    fill=theme.ink if theme.mono else theme.primary,
                )
        else:
            track_h = max(2.0, theme.dp(8) * scale)
            top = y + (h - track_h) / 2
            _rounded(
                draw,
                x,
                top,
                x + w,
                top + track_h,
                track_h / 2,
                fill=theme.paper if theme.mono else theme.secondary_container,
                outline=theme.ink if theme.mono else None,
                width=1 * SUPERSAMPLE,
            )
            if fraction > 0:
                _rounded(
                    draw,
                    x,
                    top,
                    x + w * fraction,
                    top + track_h,
                    track_h / 2,
                    fill=theme.ink if theme.mono else theme.primary,
                )
            knob = max(8.0, track_h * 1.8)
            cx = x + w * fraction
            cy = y + h / 2
            draw.ellipse(
                [cx - knob / 2, cy - knob / 2, cx + knob / 2, cy + knob / 2],
                fill=theme.ink if theme.mono else theme.primary,
            )
        return

    if widget.type == "Switch":
        checked = bool(props.get("checked", False))
        if theme.mono:
            track_fill = theme.ink if checked else theme.paper
            track_outline = theme.ink
            knob_fill = theme.paper if checked else theme.ink
        else:
            track_fill = theme.primary if checked else theme.surface_container_highest
            track_outline = None if checked else theme.outline
            knob_fill = theme.on_primary if checked else theme.text_muted
        _rounded(
            draw,
            x,
            y,
            x + w,
            y + h,
            h / 2,
            fill=track_fill,
            outline=track_outline,
            width=1 * SUPERSAMPLE,
        )
        knob = h * 0.78
        cx = x + (w - h / 2) if checked else x + h / 2
        cy = y + h / 2
        draw.ellipse(
            [cx - knob / 2, cy - knob / 2, cx + knob / 2, cy + knob / 2],
            fill=knob_fill,
        )
        return

    if widget.type == "Checkbox":
        checked = bool(props.get("checked", False))
        box = min(h, theme.font_px("default") * scale)
        box_y = y + (h - box) / 2
        _rounded(
            draw,
            x,
            box_y,
            x + box,
            box_y + box,
            theme.radius_field() * scale,
            fill=(theme.ink if checked else theme.paper) if theme.mono else (
                theme.primary if checked else None
            ),
            outline=theme.ink if theme.mono else (None if checked else theme.outline),
            width=1 * SUPERSAMPLE,
        )
        if checked:
            check = max(1, round(2 * scale))
            draw.line(
                [
                    x + box * 0.25,
                    box_y + box * 0.52,
                    x + box * 0.44,
                    box_y + box * 0.72,
                    x + box * 0.76,
                    box_y + box * 0.28,
                ],
                fill=theme.paper if theme.mono else theme.on_primary,
                width=check,
                joint="curve",
            )
        font = _font(theme, str(props.get("text_font", "default")), scale)
        _text(
            draw,
            x + box + theme.gap() * scale,
            y + h / 2,
            _fit(draw, str(props.get("text", "") or "Checkbox"), font, w - box - theme.gap() * scale),
            font,
            theme.ink if theme.mono else theme.text,
            "lm",
        )
        return

    if widget.type in ("Dropdown", "Roller"):
        _rounded(
            draw,
            x,
            y,
            x + w,
            y + h,
            _radius_for(props, theme.radius_field()) * scale,
            fill=theme.paper if theme.mono else theme.surface_container,
            outline=theme.ink if theme.mono else theme.outline_variant,
            width=1 * SUPERSAMPLE,
        )
        options = [line for line in str(props.get("options", "")).splitlines() if line.strip()]
        index = max(1, int(props.get("value", 1))) - 1
        selected = options[index] if 0 <= index < len(options) else "Select"
        font = _font(theme, "default", scale)
        _text(
            draw,
            x + theme.field_pad_hor() * scale,
            y + h / 2,
            _fit(draw, selected, font, w - theme.dp(24) * scale),
            font,
            theme.ink if theme.mono else theme.text,
            "lm",
        )
        if widget.type == "Dropdown":
            chevron = max(1, round(2 * scale))
            cx = x + w - theme.field_pad_hor() * scale
            cy = y + h / 2
            chevron_color = theme.ink if theme.mono else theme.text_muted
            draw.line([cx - 4 * scale, cy - 2 * scale, cx, cy + 2 * scale], fill=chevron_color, width=chevron)
            draw.line([cx, cy + 2 * scale, cx + 4 * scale, cy - 2 * scale], fill=chevron_color, width=chevron)
        return

    if widget.type == "Spinbox":
        _rounded(
            draw,
            x,
            y,
            x + w,
            y + h,
            _radius_for(props, theme.radius_field()) * scale,
            fill=theme.paper if theme.mono else theme.surface_container,
            outline=theme.ink if theme.mono else theme.outline_variant,
            width=1 * SUPERSAMPLE,
        )
        _text(
            draw,
            x + w / 2,
            y + h / 2,
            str(props.get("value", 0)),
            _font(theme, str(props.get("text_font", "default")), scale),
            theme.ink if theme.mono else theme.text,
            "mm",
        )
        return

    if widget.type == "TextArea":
        _rounded(
            draw,
            x,
            y,
            x + w,
            y + h,
            _radius_for(props, theme.radius_field()) * scale,
            fill=theme.paper if theme.mono else theme.surface_container,
            outline=theme.ink if theme.mono else theme.outline_variant,
            width=1 * SUPERSAMPLE,
        )
        text = str(props.get("text", "") or str(props.get("placeholder", "") or ""))
        color = (
            theme.ink
            if theme.mono
            else (theme.text if str(props.get("text", "")) else theme.text_muted)
        )
        font = _font(theme, str(props.get("text_font", "default")), scale)
        align = str(props.get("text_align", "LEFT") or "LEFT")
        if align == "CENTER":
            text_x, text_y, anchor = x + w / 2, y + h / 2, "mm"
        elif align == "RIGHT":
            text_x, text_y, anchor = x + w - theme.field_pad_hor() * scale, y + h / 2, "rm"
        else:
            text_x, text_y, anchor = (
                x + theme.field_pad_hor() * scale,
                y + theme.field_pad_ver() * scale,
                "la",
            )
        _text(
            draw,
            text_x,
            text_y,
            _fit(draw, text, font, w - 2 * theme.field_pad_hor() * scale),
            font,
            color,
            anchor,
        )
        return

    if widget.type == "Tabs":
        rows = [line.strip() for line in str(props.get("items", "")).splitlines() if line.strip()]
        if not rows:
            rows = ["One", "Two", "Three"]
        selected = max(0, min(len(rows) - 1, int(props.get("selected", 1)) - 1))
        tab_width = w / len(rows)
        regular = _font(theme, "default", scale)
        small = _font(theme, "small", scale)
        icon_scale = _font(theme, "icon", scale)

        draw.line(
            [x, y + h - 1, x + w, y + h - 1],
            fill=theme.ink if theme.mono else theme.outline_variant,
            width=1 * SUPERSAMPLE,
        )
        for index, row in enumerate(rows):
            icon_name, _, label = row.partition("|")
            if not label:
                label = icon_name
                icon_name = ""
            tx = x + index * tab_width
            center = tx + tab_width / 2
            color = theme.primary if index == selected else theme.text_muted
            if theme.mono:
                color = theme.ink if index == selected else theme.text_muted
            glyph = SHARED_ICONS.get(icon_name.strip().upper())
            max_text_width = tab_width - 8 * scale
            if glyph:
                _text(
                    draw,
                    center,
                    y + h * 0.32,
                    glyph,
                    icon_scale,
                    color,
                    "mm",
                )
                _text(
                    draw,
                    center,
                    y + h * 0.72,
                    _fit(draw, label, small, max_text_width),
                    small,
                    color,
                    "mm",
                )
            else:
                _text(
                    draw,
                    center,
                    y + h / 2,
                    _fit(draw, label, regular, max_text_width),
                    regular,
                    color,
                    "mm",
                )
            if index == selected:
                thickness = max(2.0, theme.dp(3) * scale)
                draw.rectangle(
                    [tx, y + h - thickness, tx + tab_width, y + h],
                    fill=theme.primary,
                )
        return

    if widget.type == "Statusline":
        level = str(props.get("level", "OK") or "OK")
        text = str(props.get("text", "") or "All good")
        dot = max(4.0, theme.dp(7) * scale)
        dot_x = x + theme.dp(4) * scale
        center_y = y + h / 2
        if theme.mono:
            dot_color = theme.ink
            text_color = theme.ink
        else:
            dot_color = {
                "OK": "#81c784",
                "WARNING": "#ffb74d",
                "ERROR": theme.error,
            }.get(level, "#81c784")
            text_color = theme.text_muted
        draw.ellipse(
            [dot_x, center_y - dot / 2, dot_x + dot, center_y + dot / 2],
            fill=dot_color,
        )
        text_x = dot_x + dot + theme.dp(6) * scale
        _text(
            draw,
            text_x,
            center_y,
            _fit(draw, text, _font(theme, "small", scale), x + w - text_x),
            _font(theme, "small", scale),
            text_color,
            "lm",
        )
        return

    if widget.type == "List":
        _rounded(
            draw,
            x,
            y,
            x + w,
            y + h,
            _radius_for(props, theme.radius_card()) * scale,
            fill=theme.paper if theme.mono else theme.card,
            outline=theme.ink if theme.mono else theme.outline_variant,
            width=1 * SUPERSAMPLE,
        )
        rows = [line for line in str(props.get("items", "")).splitlines() if line.strip()]
        if not rows:
            rows = ["Item one", "Item two"]
        row_h = min(
            max(theme.font_px("default") * scale + 2 * theme.list_item_pad_ver() * scale, h / max(1, len(rows))),
            h,
        )
        regular = _font(theme, "default", scale)
        small = _font(theme, "small", scale)
        for index, row in enumerate(rows):
            row_y = y + index * row_h
            if row_y + row_h > y + h:
                break
            header = index == 0 and len(rows) > 1
            _text(
                draw,
                x + theme.list_item_pad_hor() * scale,
                row_y + row_h / 2,
                _fit(
                    draw,
                    row,
                    small if header else regular,
                    w - 2 * theme.list_item_pad_hor() * scale,
                ),
                small if header else regular,
                theme.ink if theme.mono else (theme.text_muted if header else theme.text),
                "lm",
            )
            if index < len(rows) - 1 and row_y + row_h < y + h:
                draw.line(
                    [
                        x + theme.list_item_pad_hor() * scale,
                        row_y + row_h,
                        x + w - theme.list_item_pad_hor() * scale,
                        row_y + row_h,
                    ],
                    fill=theme.outline if theme.mono else theme.divider,
                    width=1 * SUPERSAMPLE,
                )
        return

    if widget.type == "PageIndicator":
        count = max(1, int(props.get("page_count", 5)))
        page = max(0, min(count - 1, int(props.get("page", 0))))
        vertical = bool(props.get("vertical", False))
        dot = max(3.0, theme.dp(8) * scale)
        step = (h if vertical else w) / count
        for index in range(count):
            cx = x + w / 2 if vertical else x + step * (index + 0.5)
            cy = y + step * (index + 0.5) if vertical else y + h / 2
            fill = theme.ink if theme.mono else (
                theme.text_muted if index == page else theme.outline_variant
            )
            if theme.mono and index != page:
                draw.ellipse(
                    [cx - dot / 2, cy - dot / 2, cx + dot / 2, cy + dot / 2],
                    fill=theme.paper,
                    outline=theme.ink,
                    width=1 * SUPERSAMPLE,
                )
            else:
                draw.ellipse(
                    [cx - dot / 2, cy - dot / 2, cx + dot / 2, cy + dot / 2],
                    fill=fill,
                )
        return

    if widget.type == "StepIndicator":
        count = max(1, int(props.get("step_count", 4)))
        finished = max(0, min(count, int(props.get("finished_count", 0))))
        gap = theme.dp(6) * scale
        thickness = max(2.0, theme.dp(6) * scale)
        seg_w = max(2.0, (w - gap * (count - 1)) / count)
        top = y + (h - thickness) / 2
        unfinished = theme.paper if theme.mono else mix("#000000", theme.secondary, 50)
        for index in range(count):
            sx = x + index * (seg_w + gap)
            done = index < finished
            _rounded(
                draw,
                sx,
                top,
                sx + seg_w,
                top + thickness,
                thickness / 2,
                fill=(theme.ink if done else theme.paper) if theme.mono else (
                    theme.secondary if done else unfinished
                ),
                outline=theme.ink if theme.mono and not done else None,
                width=1 * SUPERSAMPLE,
            )
        return

    if widget.type == "Badge":
        draw.ellipse(
            [x, y, x + w, y + h],
            fill=theme.ink if theme.mono else theme.secondary,
        )
        return

    if widget.type == "Spinner":
        stroke = max(2, round(theme.dp(3) * scale))
        draw.arc(
            [x, y, x + w, y + h],
            start=30,
            end=310,
            fill=theme.ink if theme.mono else theme.primary,
            width=stroke,
        )
        return

    if widget.type == "Image":
        _rounded(
            draw,
            x,
            y,
            x + w,
            y + h,
            _radius_for(props, theme.radius_card()) * scale,
            fill=theme.paper if theme.mono else theme.surface_container_high,
            outline=theme.ink if theme.mono else theme.outline_variant,
            width=1 * SUPERSAMPLE,
        )
        _text(
            draw,
            x + w / 2,
            y + h / 2,
            "image",
            _font(theme, "small", scale),
            theme.ink if theme.mono else theme.text_muted,
            "mm",
        )
        return


def _draw_sliderbox(
    draw: ImageDraw.ImageDraw,
    props: dict,
    x: float,
    y: float,
    w: float,
    h: float,
    theme: MaterialTheme,
    scale: float,
) -> None:
    gap = theme.gap() * scale
    button = min(h, (theme.font_px("default") + 8) * scale)
    cy = y + h / 2
    value = max(
        str(props.get("min", 0)),
        str(props.get("max", 100)),
        key=len,
    )
    value_font = _font(theme, "default", scale)
    value_w = draw.textlength(value, font=value_font) + 8 * scale

    # Minus
    _rounded(
        draw,
        x,
        cy - button / 2,
        x + button,
        cy + button / 2,
        button / 2,
        fill=theme.paper if theme.mono else theme.primary,
        outline=theme.ink if theme.mono else None,
        width=1 * SUPERSAMPLE,
    )
    stroke = max(2, round(button * 0.09))
    line_w = button * 0.42
    minus_color = theme.ink if theme.mono else theme.on_primary
    draw.line(
        [x + button / 2 - line_w / 2, cy, x + button / 2 + line_w / 2, cy],
        fill=minus_color,
        width=stroke,
    )

    # Track and knob
    track_x = x + button + gap
    track_right = x + w - button - value_w - 2 * gap
    track_w = max(4.0, track_right - track_x)
    track_h = max(2.0, theme.dp(8) * scale)
    fraction = _value_fraction(props)
    _rounded(
        draw,
        track_x,
        cy - track_h / 2,
        track_x + track_w,
        cy + track_h / 2,
        track_h / 2,
        fill=theme.paper if theme.mono else theme.secondary_container,
        outline=theme.ink if theme.mono else None,
        width=1 * SUPERSAMPLE,
    )
    if fraction > 0:
        _rounded(
            draw,
            track_x,
            cy - track_h / 2,
            track_x + track_w * fraction,
            cy + track_h / 2,
            track_h / 2,
            fill=theme.ink if theme.mono else theme.primary,
        )
    knob = max(8.0, track_h * 1.8)
    knob_x = track_x + track_w * fraction
    draw.ellipse(
        [knob_x - knob / 2, cy - knob / 2, knob_x + knob / 2, cy + knob / 2],
        fill=theme.ink if theme.mono else theme.primary,
    )

    # Value, then plus
    _text(
        draw,
        x + w - button - gap,
        cy,
        str(props.get("value", 0)),
        value_font,
        theme.ink if theme.mono else theme.text,
        "rm",
    )
    plus_color = theme.ink if theme.mono else theme.on_primary
    plus_cx = x + w - button / 2
    _rounded(
        draw,
        x + w - button,
        cy - button / 2,
        x + w,
        cy + button / 2,
        button / 2,
        fill=theme.paper if theme.mono else theme.primary,
        outline=theme.ink if theme.mono else None,
        width=1 * SUPERSAMPLE,
    )
    draw.line(
        [plus_cx - line_w / 2, cy, plus_cx + line_w / 2, cy],
        fill=plus_color,
        width=stroke,
    )
    draw.line(
        [plus_cx, cy - line_w / 2, plus_cx, cy + line_w / 2],
        fill=plus_color,
        width=stroke,
    )


def _handles(x: float, y: float, w: float, h: float) -> list[tuple[float, float]]:
    return [
        (x, y), (x + w / 2, y), (x + w, y),
        (x + w, y + h / 2),
        (x + w, y + h), (x + w / 2, y + h), (x, y + h),
        (x, y + h / 2),
    ]


def hit_handle(
    canvas_x: float,
    canvas_y: float,
    rect: tuple[float, float, float, float],
    zoom: float,
) -> str | None:
    x, y, w, h = rect
    names = ("nw", "n", "ne", "e", "se", "s", "sw", "w")
    for name, (hx, hy) in zip(names, _handles(x, y, w, h)):
        if abs(canvas_x - hx) <= HANDLE and abs(canvas_y - hy) <= HANDLE:
            return name
    return None
