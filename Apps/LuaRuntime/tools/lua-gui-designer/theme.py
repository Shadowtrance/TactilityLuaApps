"""Material 3 theme model for the desktop preview.

This mirrors the tokens the firmware uses in
`Modules/lvgl-module/themes/material.lss` and the geometry in
`Modules/lvgl-module/source` closely enough that a designer preview matches the
device layout:

- colours are mixed in the same order as the LSS file;
- `dp` values use LVGL's `(dpi * n + 80) / 160` conversion;
- the status bar and toolbar heights use the same font-size formulas as
  `statusbar.cpp` and `toolbar.cpp`;
- the appearance spacing/size/font-size settings drive the same tokens.

It is still a preview, not an LVGL renderer: text metrics and some widget
internals differ slightly.
"""

from __future__ import annotations

from dataclasses import dataclass

# Neutral tones from material.lss
NEUTRAL_30 = "#474747"
NEUTRAL_50 = "#777777"
NEUTRAL_60 = "#919191"
NEUTRAL_80 = "#c6c6c6"
NEUTRAL_90 = "#e2e2e2"
NEUTRAL_92 = "#e8e8e8"
NEUTRAL_94 = "#eeeeee"
NEUTRAL_96 = "#f3f3f3"
NEUTRAL_98 = "#fafafa"
NEUTRAL_100 = "#ffffff"
DARK_BACKGROUND = "#21222b"


def _rgb(color: str) -> tuple[int, int, int]:
    color = color.strip().lstrip("#")
    if len(color) == 3:
        color = "".join(char * 2 for char in color)
    return int(color[0:2], 16), int(color[2:4], 16), int(color[4:6], 16)


def _hex(rgb: tuple[int, int, int]) -> str:
    return "#{:02x}{:02x}{:02x}".format(*(max(0, min(255, round(value))) for value in rgb))


def mix(top: str, base: str, percent: int) -> str:
    """LSS `mix(top, base, percent)`: `top` covers `base` by `percent`."""
    top_r, top_g, top_b = _rgb(top)
    base_r, base_g, base_b = _rgb(base)
    t = max(0, min(100, percent)) / 100
    return _hex(
        (
            top_r * t + base_r * (1 - t),
            top_g * t + base_g * (1 - t),
            top_b * t + base_b * (1 - t),
        )
    )


def luminance(color: str) -> float:
    r, g, b = _rgb(color)
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255


def lerp(compact: int, regular: int, percent: int) -> int:
    """LVGL's `lvgl_theme_lerp`, including C++ truncation toward zero."""
    percent = max(0, min(100, percent))
    return compact + int((regular - compact) * percent / 100)


def dp_px(dpi: int, value: int) -> int:
    """LVGL's `LV_DPX_CALC`."""
    if value == 0:
        return 0
    return max(1, (dpi * value + 80) // 160)


@dataclass
class ThemeSettings:
    dark: bool = True
    mono: bool = False
    primary: str = "#00bbff"
    secondary: str = "#ee81c3"
    error: str = "#ef6c00"
    surface: str = ""
    spacing: int = 100
    size: int = 100
    font_size: int = 29
    bars_mode: str = "auto"
    bars_portrait: str = "split"
    bars_landscape: str = "side"
    bars_align: str = "left"

    def to_dict(self) -> dict:
        return {
            "dark": self.dark,
            "mono": self.mono,
            "primary": self.primary,
            "secondary": self.secondary,
            "error": self.error,
            "surface": self.surface,
            "spacing": self.spacing,
            "size": self.size,
            "font_size": self.font_size,
            "bars_mode": self.bars_mode,
            "bars_portrait": self.bars_portrait,
            "bars_landscape": self.bars_landscape,
            "bars_align": self.bars_align,
        }

    @classmethod
    def from_dict(cls, data: dict) -> "ThemeSettings":
        legacy_bars = "bars_mode" not in data
        return cls(
            dark=bool(data.get("dark", True)),
            mono=bool(data.get("mono", False)),
            primary=str(data.get("primary", "#00bbff")),
            secondary=str(data.get("secondary", "#ee81c3")),
            error=str(data.get("error", "#ef6c00")),
            surface=str(data.get("surface", "") or ""),
            spacing=int(data.get("spacing", 100)),
            size=int(data.get("size", 100)),
            font_size=int(data.get("font_size", 29)),
            bars_mode="custom" if legacy_bars else str(data.get("bars_mode", "auto")),
            bars_portrait="split" if legacy_bars else str(data.get("bars_portrait", "split")),
            bars_landscape="split" if legacy_bars else str(data.get("bars_landscape", "side")),
            bars_align=str(data.get("bars_align", "left")),
        )


@dataclass
class MaterialTheme:
    settings: ThemeSettings
    dpi: int = 294

    # ---- colour roles -----------------------------------------------------

    @property
    def mono(self) -> bool:
        return self.settings.mono

    @property
    def ink(self) -> str:
        return NEUTRAL_100 if self.settings.dark else "#000000"

    @property
    def paper(self) -> str:
        return "#000000" if self.settings.dark else NEUTRAL_100

    def _tinted(self, neutral: str, amount: int) -> str:
        return mix(self.settings.primary, neutral, amount)

    @property
    def surface_override(self) -> str:
        return "" if self.mono else self.settings.surface

    def _lifted_surface(self, percent: int) -> str:
        lift = NEUTRAL_100 if self.settings.dark else "#000000"
        return mix(lift, self.surface_override, percent)

    def _sunk_surface(self, percent: int) -> str:
        sink = "#000000" if self.settings.dark else NEUTRAL_100
        return mix(sink, self.surface_override, percent)

    @property
    def surface(self) -> str:
        if self.mono:
            return self.paper
        if self.surface_override:
            return self.surface_override
        if self.settings.dark:
            return mix("#000000", DARK_BACKGROUND, 20)
        return self._tinted(NEUTRAL_98, 4)

    @property
    def surface_container_lowest(self) -> str:
        if self.mono:
            return self.paper
        if self.surface_override:
            return self._sunk_surface(25)
        if self.settings.dark:
            return mix("#000000", DARK_BACKGROUND, 35)
        return self._tinted(NEUTRAL_100, 4)

    @property
    def surface_container_low(self) -> str:
        if self.mono:
            return self.paper
        if self.surface_override:
            return self._lifted_surface(4 if self.settings.dark else 2)
        if self.settings.dark:
            return DARK_BACKGROUND
        return self._tinted(NEUTRAL_96, 4)

    @property
    def surface_container(self) -> str:
        if self.mono:
            return self.paper
        if self.surface_override:
            return self._lifted_surface(6 if self.settings.dark else 4)
        if self.settings.dark:
            return mix(NEUTRAL_100, DARK_BACKGROUND, 3)
        return self._tinted(NEUTRAL_94, 4)

    @property
    def surface_container_high(self) -> str:
        if self.mono:
            return self.paper
        if self.surface_override:
            return self._lifted_surface(11 if self.settings.dark else 6)
        if self.settings.dark:
            return mix(NEUTRAL_100, DARK_BACKGROUND, 8)
        return self._tinted(NEUTRAL_92, 4)

    @property
    def surface_container_highest(self) -> str:
        if self.mono:
            return self.paper
        if self.surface_override:
            return self._lifted_surface(16 if self.settings.dark else 8)
        if self.settings.dark:
            return mix(NEUTRAL_100, DARK_BACKGROUND, 13)
        return self._tinted(NEUTRAL_90, 4)

    @property
    def card(self) -> str:
        return self.surface_container_high

    @property
    def field(self) -> str:
        return self.surface_container_high

    @property
    def on_surface(self) -> str:
        if self.mono:
            return self.ink
        return self._tinted(NEUTRAL_90 if self.settings.dark else "#1c1c1c", 4)

    @property
    def on_surface_variant(self) -> str:
        if self.mono:
            return self.ink
        return self._tinted(NEUTRAL_80 if self.settings.dark else NEUTRAL_30, 8)

    @property
    def text(self) -> str:
        return self.on_surface

    @property
    def text_muted(self) -> str:
        return self.on_surface_variant

    @property
    def outline(self) -> str:
        if self.mono:
            return self.ink
        return self._tinted(NEUTRAL_60 if self.settings.dark else NEUTRAL_50, 8)

    @property
    def outline_variant(self) -> str:
        if self.mono:
            return self.ink
        return self._tinted(NEUTRAL_30 if self.settings.dark else NEUTRAL_80, 8)

    @property
    def divider(self) -> str:
        return self.outline_variant

    # ---- accents ----------------------------------------------------------

    @property
    def primary(self) -> str:
        if self.mono:
            return self.ink
        return (
            mix(self.settings.primary, NEUTRAL_100, 55)
            if self.settings.dark
            else self.settings.primary
        )

    @property
    def on_primary(self) -> str:
        if self.mono:
            return self.paper
        return (
            mix(self.settings.primary, "#000000", 50)
            if self.settings.dark
            else NEUTRAL_100
        )

    @property
    def primary_container(self) -> str:
        if self.mono:
            return self.ink
        return (
            mix(self.settings.primary, "#000000", 70)
            if self.settings.dark
            else mix(self.settings.primary, NEUTRAL_100, 25)
        )

    @property
    def on_primary_container(self) -> str:
        if self.mono:
            return self.paper
        return (
            mix(self.settings.primary, NEUTRAL_100, 20)
            if self.settings.dark
            else mix(self.settings.primary, "#000000", 60)
        )

    @property
    def secondary(self) -> str:
        if self.mono:
            return self.ink
        return (
            mix(self.settings.secondary, NEUTRAL_100, 55)
            if self.settings.dark
            else self.settings.secondary
        )

    @property
    def on_secondary(self) -> str:
        if self.mono:
            return self.paper
        return (
            mix(self.settings.secondary, "#000000", 50)
            if self.settings.dark
            else NEUTRAL_100
        )

    @property
    def secondary_container(self) -> str:
        if self.mono:
            return self.ink
        return (
            mix(self.settings.secondary, "#000000", 70)
            if self.settings.dark
            else mix(self.settings.secondary, NEUTRAL_100, 25)
        )

    @property
    def on_secondary_container(self) -> str:
        if self.mono:
            return self.paper
        return (
            mix(self.settings.secondary, NEUTRAL_100, 20)
            if self.settings.dark
            else mix(self.settings.secondary, "#000000", 60)
        )

    @property
    def error(self) -> str:
        if self.mono:
            return self.ink
        return (
            mix(self.settings.error, NEUTRAL_100, 55)
            if self.settings.dark
            else self.settings.error
        )

    @property
    def on_error(self) -> str:
        if self.mono:
            return self.paper
        return (
            mix(self.settings.error, "#000000", 50)
            if self.settings.dark
            else NEUTRAL_100
        )

    # ---- dp and density ---------------------------------------------------

    def dp(self, value: int) -> int:
        return dp_px(self.dpi, value)

    def dp_float(self, value: float) -> int:
        """`LV_DPX` for a value that does not fit an integer number of dp."""
        if value == 0:
            return 0
        return max(1, int((self.dpi * value + 80) / 160))

    def control_margin(self) -> int:
        """The room the theme reserves around controls for their focus ring.

        `chip`, `button`, `text_button` and the switch track carry
        `marginAll: @focus-width + @focus-offset` so the ring is not clipped by
        the parent. Flex and grid layouts include those margins.
        """
        offset = 1 + (2 - 1) * self.settings.size / 100
        return self.dp_float(2 + offset)

    def sp(self, steps: int) -> int:
        """A spacing step on the theme's grid (1 = first step)."""
        sizes = (2, 4, 6, 8, 12) if self.mono else (4, 8, 12, 16, 24)
        index = max(0, min(len(sizes) - 1, steps - 1))
        return self.dp(sizes[index])

    def gap(self) -> int:
        return lerp(2, self.dp(4 if self.mono else 8), self.settings.spacing)

    def container_gap(self) -> int:
        return lerp(2, self.dp(6 if self.mono else 12), self.settings.spacing)

    def container_pad(self) -> int:
        return lerp(2, self.dp(8 if self.mono else 16), self.settings.spacing)

    def control_pad_hor(self) -> int:
        return lerp(2, self.dp(12 if self.mono else 24), self.settings.size)

    def control_pad_ver(self) -> int:
        return lerp(1, self.dp(6 if self.mono else 10), self.settings.size)

    def field_pad_hor(self) -> int:
        return lerp(2, self.dp(6 if self.mono else 16), self.settings.spacing)

    def field_pad_ver(self) -> int:
        return lerp(2, self.dp(6 if self.mono else 8), self.settings.spacing)

    def chip_pad_hor(self) -> int:
        return lerp(3, self.dp(8 if self.mono else 16), self.settings.spacing)

    def chip_pad_ver(self) -> int:
        return lerp(2, self.dp(4 if self.mono else 6), self.settings.spacing)

    def list_item_pad_hor(self) -> int:
        return lerp(2, self.dp(8 if self.mono else 16), self.settings.spacing)

    def list_item_pad_ver(self) -> int:
        return lerp(2, self.dp(6 if self.mono else 12), self.settings.spacing)

    def switch_size(self) -> tuple[int, int]:
        width_dp, height_dp = (40, 24) if self.mono else (52, 32)
        return (
            lerp(25, self.dp(width_dp), self.settings.size),
            lerp(15, self.dp(height_dp), self.settings.size),
        )

    # ---- shapes -----------------------------------------------------------

    @property
    def scrollbar_bar(self) -> str:
        """The scrollbar colour: the outline at the theme's 40% opacity."""
        return mix(self.outline, self.surface_container_low, 40)

    def radius_card(self) -> int:
        return lerp(4, self.dp(8 if self.mono else 12), self.settings.size)

    def radius_control(self) -> int:
        if self.settings.size >= 100:
            return 999
        return lerp(4, self.dp(12 if self.mono else 28), self.settings.size)

    def radius_field(self) -> int:
        return lerp(4, self.dp(4), self.settings.size)

    def radius_chip(self) -> int:
        return lerp(4, self.dp(4 if self.mono else 8), self.settings.size)

    def container_border_width(self) -> int:
        if self.mono:
            return 1
        return lerp(1, 0, self.settings.size)

    def card_border_width(self) -> int:
        if self.mono:
            return 2
        return lerp(1, 0, self.settings.size)

    # ---- fonts and chrome -------------------------------------------------

    def font_px(self, kind: str = "default") -> int:
        size = max(1, self.settings.font_size)
        # "icon" is the shared default icon font, which is 1.25x the text size.
        factors = {"small": 0.75, "default": 1.0, "large": 1.25, "icon": 1.25}
        return max(7, round(size * factors.get(kind, 1.0)))

    def font(self, kind: str = "default") -> tuple[str, int]:
        return ("Segoe UI", self.font_px(kind))

    def statusbar_icon_px(self) -> int:
        return max(1, round(self.settings.font_size * 1.15))

    def statusbar_height(self) -> int:
        icon = self.statusbar_icon_px()
        return icon + 2 * int(icon * 0.1) + 4

    def statusbar_icon_gap(self) -> int:
        return lerp(2, int(self.statusbar_icon_px() * 0.2), self.settings.spacing)

    def toolbar_height(self) -> int:
        default = int(self.settings.font_size * 1.6)
        large = int(round(self.settings.font_size * 1.25) * 2.2)
        return lerp(default, large, self.settings.size)

    def toolbar_pad(self) -> int:
        return lerp(8, int(self.toolbar_height() * 0.2), self.settings.size)

    def toolbar_action_size(self) -> int:
        return max(8, self.toolbar_height() - self.toolbar_pad())

    def toolbar_pad_hor(self) -> int:
        return lerp(0, self.dp(2 if self.mono else 4), self.settings.spacing)

    def toolbar_gap(self) -> int:
        return lerp(2, self.dp(2 if self.mono else 4), self.settings.spacing)

    def content_top(self) -> int:
        return self.statusbar_height() + self.toolbar_height()

    def bars_layout(self, width: int, height: int) -> str:
        """Resolve the firmware's Auto/Custom system-bars layout."""
        landscape = width > height
        if self.settings.bars_mode == "custom":
            return (
                self.settings.bars_landscape
                if landscape
                else self.settings.bars_portrait
            )
        return "side" if landscape else "split"

    def side_bar_width(self) -> int:
        """Approximation of statusbar.cpp's side-strip width."""
        time_width = round(self.font_px("small") * 2.8) + 8
        action_width = self.font_px("default") + 2 * self.control_pad_ver()
        return max(24, max(time_width, action_width) + 4)

    @property
    def side_bar_color(self) -> str:
        if self.mono:
            return self.paper
        return mix("#000000", self.surface_container_low, 20)

    def app_bounds(self, width: int, height: int) -> tuple[int, int, int, int]:
        """The app's content area below its toolbar, in screen coordinates."""
        if self.bars_layout(width, height) == "side":
            strip = self.side_bar_width()
            app_x = 0 if self.settings.bars_align == "right" else strip
            app_w = max(1, width - strip)
            return app_x, self.toolbar_height(), app_w, max(1, height - self.toolbar_height())
        top = self.content_top()
        return 0, top, width, max(1, height - top)

    def content_padding(self) -> int:
        """The root content object's padding, before the exported canvas."""
        return self.container_pad()

    def content_size(self, width: int, height: int) -> tuple[int, int]:
        _x, _y, app_w, app_h = self.app_bounds(width, height)
        pad = self.content_padding()
        return (
            max(1, app_w - 2 * pad),
            max(1, app_h - 2 * pad),
        )
