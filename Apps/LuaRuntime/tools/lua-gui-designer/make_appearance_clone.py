"""Build the 1:1 Appearance Settings main-view clone as a designer project.

The tree mirrors `Tactility/Source/app/appearancesettings/AppearanceSettings.cpp`
for the view that opens first: Theme, Theme Colors, System Bars and Fonts. The
sub-views that the buttons open are not part of it.

Sizes are stored so the designer preview matches the device: LVGL's content
sizing is used on the device (`w_content`/`h_content`), and these numbers only
give the preview a box to draw. They were measured on the Tab5 at spacing 80,
size 80 and font size 28, and the text widths come from the same Adwaita Sans
file the firmware embeds.
"""

from pathlib import Path

import fonts
from lua_export import export
from model import Project
from profiles import find_profile
from theme import MaterialTheme, ThemeSettings

PALETTE_ICON = "\ue40a"  # Material Symbols "palette"
MINUS_ICON = "\uf068"  # LV_SYMBOL_MINUS, like the C++ app
PLUS_ICON = "\uf067"  # LV_SYMBOL_PLUS, like the C++ app

CHIP_PAD = 25  # theme chip pad hor at spacing 80
CHIP_HEIGHT = 58
BUTTON_PAD = 35  # theme control pad hor at size 80
BUTTON_HEIGHT = 64
LABEL_HEIGHT = 36
SMALL_LABEL_HEIGHT = 27

project = Project.new()
project.profile = "tab5-landscape"
project.theme = ThemeSettings(
    dark=True,
    primary="#00bbff",
    secondary="#ee81c3",
    error="#ef6c00",
    spacing=80,
    size=80,
    font_size=28,
    bars_mode="custom",
    bars_portrait="split",
    bars_landscape="split",
    bars_align="left",
)

profile = find_profile(project.profile)
material = MaterialTheme(project.theme, profile.dpi)
CARD_PAD = material.container_pad()  # 23 at spacing 80
CARD_GAP = material.container_gap()  # 18 at spacing 80

_text_font = fonts.text_font(project.theme.font_size)
_small_font = fonts.text_font(round(project.theme.font_size * 0.75))
_icon_font = fonts.icon_font(round(project.theme.font_size * 1.25))


def text_width(text: str, font=None) -> int:
    return round((font or _text_font).getlength(text))


def add(type_name: str, parent, x=0, y=0, w=100, h=40, **props):
    widget = project.add_widget(type_name, getattr(parent, "id", parent))
    widget.x, widget.y, widget.w, widget.h = x, y, w, h
    widget.props.update(props)
    return widget


def object(parent, w_pct=100, h_content=True, **props):
    return add(
        "Object",
        parent,
        w=w_pct,
        h=LABEL_HEIGHT,
        w_pct=w_pct,
        h_content=h_content,
        bg_opa="TRANSP",
        border_width=0,
        radius=0,
        pad_all=0,
        **props,
    )


def label(parent, text, small=False):
    height = SMALL_LABEL_HEIGHT if small else LABEL_HEIGHT
    font = "small" if small else "default"
    builder = _small_font if small else _text_font
    return add(
        "Label",
        parent,
        w=text_width(text, builder) + 2,
        h=height,
        w_content=True,
        h_content=True,
        text=text,
        text_font=font,
    )


def label_grow(parent, text):
    widget = label(parent, text)
    widget.props["flex_grow"] = 1
    return widget


def row(parent, wrap=False):
    return object(
        parent,
        layout="FLEX",
        flex_flow="ROW_WRAP" if wrap else "ROW",
        flex_main="START",
        flex_cross="CENTER",
        flex_track="CENTER",
        pad_row=0,
        pad_column=CARD_GAP,
    )


def labeled_row(parent, title):
    line = row(parent, wrap=True)
    label_grow(line, title)
    return line


def chip(parent, text, checked=False):
    return add(
        "Chip",
        parent,
        w=text_width(text) + 2 * CHIP_PAD,
        h=CHIP_HEIGHT,
        w_content=True,
        h_content=True,
        text=text,
        text_font="default",
        checked=checked,
    )


def button(parent, text, font="default", width=None):
    if width is None:
        width = text_width(text, _icon_font if font == "icon" else None) + 2 * BUTTON_PAD
    return add(
        "Button",
        parent,
        w=width,
        h=BUTTON_HEIGHT,
        w_content=True,
        h_content=True,
        text=text,
        text_font=font,
    )


def card(parent):
    return add(
        "Card",
        parent,
        w=100,
        h=CHIP_HEIGHT,
        w_pct=100,
        h_content=True,
        layout="FLEX",
        flex_flow="COLUMN",
        flex_main="START",
        flex_cross="START",
        flex_track="START",
        pad_all=CARD_PAD,
        pad_row=CARD_GAP,
        pad_column=CARD_GAP,
        bg_opa="COVER",
    )


def section(title):
    """A title and its card, both children of the content so the row gap applies."""
    label(root, title)
    return card(root)


def swatch(parent, color):
    return add(
        "Object",
        parent,
        w=BUTTON_HEIGHT,
        h=BUTTON_HEIGHT,
        bg_color=color,
        bg_opa="COVER",
        border_width=1,
        border_color=material.text,
        radius=4,
    )


def color_row(parent, title, color):
    line = labeled_row(parent, title)
    # The C++ sizes the swatch like the palette button next to it.
    swatch(line, color)
    button(line, PALETTE_ICON, font="icon")


def font_row(parent, title, font_name):
    line = row(parent)
    texts = object(
        line,
        w_content=True,
        layout="FLEX",
        flex_flow="COLUMN",
        flex_main="START",
        flex_cross="START",
        flex_track="START",
        pad_row=0,
    )
    texts.w = text_width(title)
    texts.props["flex_grow"] = 1
    first = label(texts, title)
    first.props.update({"w_pct": 100, "w_content": False, "long_mode": "SCROLL_CIRCULAR"})
    second = label(texts, font_name, small=True)
    second.props.update({"w_pct": 100, "w_content": False, "long_mode": "SCROLL_CIRCULAR"})
    # "Default" is hidden while the system font is in use, so it is not cloned.
    button(line, "Select")


# Root: the runner's content already has the theme's padding and row gap, and the
# designer canvas starts inside that padding, so the root only spaces the sections.
root = object(
    None,
    w_pct=100,
    h_content=True,
    layout="FLEX",
    flex_flow="COLUMN",
    flex_main="START",
    flex_cross="START",
    flex_track="START",
    pad_row=CARD_GAP,
    pad_column=0,
)

# Theme -------------------------------------------------------------------------------
theme_card = section("Theme")

mode = labeled_row(theme_card, "Mode")
chip(mode, "Light", checked=False)
chip(mode, "Dark", checked=True)

style = labeled_row(theme_card, "Style")
chip(style, "Regular", checked=True)
chip(style, "Mono", checked=False)

label(theme_card, "Spacing (%)")
add(
    "SliderBox",
    theme_card,
    w=100,
    h=BUTTON_HEIGHT,
    w_pct=100,
    min=0,
    max=100,
    step=20,
    value=80,
)
label(theme_card, "Size (%)")
add(
    "SliderBox",
    theme_card,
    w=100,
    h=BUTTON_HEIGHT,
    w_pct=100,
    min=0,
    max=100,
    step=20,
    value=80,
)

animations = labeled_row(theme_card, "Animations")
add("Switch", animations, w=81, h=50, checked=True)

# Theme Colors ------------------------------------------------------------------------
colors_card = section("Theme Colors")
color_row(colors_card, "Primary color", "#00bbff")
color_row(colors_card, "Secondary color", "#ee81c3")
color_row(colors_card, "Surface color", "#131313")
color_row(colors_card, "Error color", "#ef6c00")
# The "Surface tint" row only shows when a surface colour is set, and it is not.

# System Bars -------------------------------------------------------------------------
bars_card = section("System Bars")

align = labeled_row(bars_card, "Align")
chip(align, "Left", checked=True)
chip(align, "Right", checked=False)

bars_layout = labeled_row(bars_card, "Layout")
chip(bars_layout, "Auto", checked=False)
chip(bars_layout, "Custom", checked=True)

portrait = labeled_row(bars_card, "Portrait")
chip(portrait, "Split", checked=True)
chip(portrait, "Side", checked=False)

landscape = labeled_row(bars_card, "Landscape")
chip(landscape, "Split", checked=True)
chip(landscape, "Side", checked=False)

# Fonts -------------------------------------------------------------------------------
fonts_card = section("Fonts")

font_size = row(fonts_card)
label_grow(font_size, "Font size")
button(font_size, MINUS_ICON, width=96)
# A text area with centred text looks like the spinbox without its digit cursor.
add(
    "TextArea",
    font_size,
    w=122,
    h=BUTTON_HEIGHT,
    text="28",
    placeholder="",
    one_line=True,
    text_align="CENTER",
    text_font="default",
)
button(font_size, PLUS_ICON, width=96)

font_row(fonts_card, "Regular font", "System font")
font_row(fonts_card, "Monospace font", "System font")

base = Path(__file__).resolve().parent
project_path = base / "appearance.luadesign.json"
lua_path = base / "appearance.lua"
project.save(project_path)
lua_path.write_text(export(project), encoding="utf-8")
print(project_path)
print(lua_path)
