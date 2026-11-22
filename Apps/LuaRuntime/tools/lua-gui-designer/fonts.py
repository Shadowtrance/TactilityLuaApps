"""Font discovery and text measurement shared by the preview and the model.

The preview and the layout model both need the firmware's Adwaita Sans metrics,
otherwise a label's preview box and its content-sized box disagree. This module
is the single place that finds the font files and caches the Pillow faces.
"""

from __future__ import annotations

import os
from functools import lru_cache
from pathlib import Path

from PIL import ImageFont

ADWAITA_SUBSET = "AdwaitaSans.ttf"
ADWAITA_FULL = "AdwaitaSans-Regular.ttf"
MATERIAL_SYMBOLS = "MaterialSymbolsRounded.ttf"
TEXT_WEIGHT = 550
FONT_FALLBACKS = (
    Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
    Path("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"),
)

# The firmware's font subsets, bundled so the designer works without a Tactility
# source checkout. See fonts/README.md.
BUNDLED_FONTS = Path(__file__).resolve().parent / "fonts"

# Text line height as a factor of the font size. The firmware's dynamic fonts
# report 36 px at size 28 (measured on the Tab5), which is 9/7.
LINE_HEIGHT_FACTOR = 9 / 7


def _find_font_file(name: str, env_name: str) -> Path | None:
    override = os.environ.get(env_name)
    if override:
        path = Path(override)
        if path.exists():
            return path
    for parent in Path(__file__).resolve().parents:
        for candidate in (
            parent / "TactilityWork" / "Tactility" / "Fonts" / name,
            parent / "Tactility" / "Fonts" / name,
            parent / "TactilityWork" / "Data" / "system" / "fonts" / name,
            parent / "Data" / "system" / "fonts" / name,
        ):
            if candidate.exists():
                return candidate
    bundled = BUNDLED_FONTS / name
    if bundled.exists():
        return bundled
    return None


def _find_text_font() -> Path | None:
    override = os.environ.get("TACTILITY_FONT")
    if override:
        path = Path(override)
        if path.exists():
            return path
    # Prefer the exact subset the firmware embeds: AdwaitaSans pinned to weight
    # 550. The full Regular file is a fallback, instanced below.
    for parent in Path(__file__).resolve().parents:
        for candidate in (
            parent / "TactilityWork" / "Data" / "system" / "fonts" / ADWAITA_SUBSET,
            parent / "Data" / "system" / "fonts" / ADWAITA_SUBSET,
            parent / "TactilityWork" / "Tactility" / "Fonts" / ADWAITA_FULL,
            parent / "Tactility" / "Fonts" / ADWAITA_FULL,
        ):
            if candidate.exists():
                return candidate
    bundled = BUNDLED_FONTS / ADWAITA_SUBSET
    if bundled.exists():
        return bundled
    return None


# The firmware and simulator use the Adwaita Sans and Material Symbols assets.
# Use them when the Tactility checkout is next to this project; otherwise keep
# a system fallback for text and draw the icons as vectors.
FONT_REGULAR = _find_text_font()
FONT_ICONS = _find_font_file(MATERIAL_SYMBOLS, "TACTILITY_ICON_FONT")

_FONT_CACHE: dict[tuple[str, int, bool], ImageFont.FreeTypeFont | ImageFont.ImageFont] = {}


def fonts_available() -> bool:
    """Content measurement needs the real text font, not the bitmap fallback."""
    return FONT_REGULAR is not None


@lru_cache(maxsize=64)
def icon_font(size: int):
    size = max(7, size)
    key = ("icon", size, False)
    cached = _FONT_CACHE.get(key)
    if cached is not None:
        return cached
    if FONT_ICONS is None:
        return None
    try:
        font = ImageFont.truetype(str(FONT_ICONS), size)
    except OSError:
        return None
    _FONT_CACHE[key] = font
    return font


def text_font(size: int, bold: bool = False):
    size = max(7, size)
    key = ("text", size, bold)
    cached = _FONT_CACHE.get(key)
    if cached is not None:
        return cached

    for path in (FONT_REGULAR, *FONT_FALLBACKS):
        if path is None:
            continue
        try:
            font = ImageFont.truetype(str(path), size)
            try:
                axes = font.get_variation_axes()
            except OSError:
                axes = []
            if axes:
                values = [
                    TEXT_WEIGHT if b"Weight" in axis["name"] else axis["default"]
                    for axis in axes
                ]
                font.set_variation_by_axes(values)
            _FONT_CACHE[key] = font
            return font
        except OSError:
            continue

    font = ImageFont.load_default()
    _FONT_CACHE[key] = font
    return font


@lru_cache(maxsize=1024)
def measure_text(text: str, size: int) -> int:
    """Advance width of a single line, matching LVGL's text metrics closely."""
    if not text:
        return 0
    return round(text_font(size).getlength(text))


def line_height(size: int) -> int:
    return max(1, round(size * LINE_HEIGHT_FACTOR))
