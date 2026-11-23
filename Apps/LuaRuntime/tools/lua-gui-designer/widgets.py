"""Widget catalog and property schemas for the Lua GUI designer."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class PropertySpec:
    key: str
    label: str
    kind: str  # text | int | bool | color | choice | multiline
    default: object = ""
    choices: tuple[str, ...] = ()
    minimum: int | None = None
    maximum: int | None = None
    optional: bool = False

    @property
    def is_geometry(self) -> bool:
        return self.key in ("x", "y", "w", "h")


@dataclass(frozen=True)
class WidgetSpec:
    type: str
    title: str
    category: str
    default_width: int
    default_height: int
    container: bool = False
    summary: str = ""
    properties: tuple[PropertySpec, ...] = ()
    events: tuple[str, ...] = ()

    def default_props(self) -> dict:
        return {
            prop.key: prop.default
            for prop in self.properties
            if not prop.is_geometry and not (prop.optional and prop.default is None)
        }


GRID_ALIGNMENTS = (
    "START", "CENTER", "END", "STRETCH",
    "SPACE_EVENLY", "SPACE_AROUND", "SPACE_BETWEEN",
)

GEOMETRY = (
    PropertySpec("x", "X", "int", 16, minimum=-4096, maximum=4096),
    PropertySpec("y", "Y", "int", 16, minimum=-4096, maximum=4096),
    PropertySpec("w", "Width", "int", 160, minimum=8, maximum=4096),
    PropertySpec("h", "Height", "int", 40, minimum=8, maximum=4096),
    PropertySpec("w_pct", "Width % (0 = pixels)", "int", 0, minimum=0, maximum=100),
    PropertySpec("h_pct", "Height % (0 = pixels)", "int", 0, minimum=0, maximum=100),
    PropertySpec("w_content", "Width = content", "bool", False),
    PropertySpec("h_content", "Height = content", "bool", False),
    PropertySpec("flex_grow", "Flex grow", "int", 0, minimum=0, maximum=16),
    # Only shown when the selected widget's parent is a grid.
    PropertySpec("grid_col", "Grid column (0-based)", "int", 0, minimum=0, maximum=64),
    PropertySpec("grid_row", "Grid row (0-based)", "int", 0, minimum=0, maximum=64),
    PropertySpec("grid_col_span", "Column span", "int", 1, minimum=1, maximum=64),
    PropertySpec("grid_row_span", "Row span", "int", 1, minimum=1, maximum=64),
    PropertySpec("cell_col_align", "Cell column align", "choice", "STRETCH", GRID_ALIGNMENTS),
    PropertySpec("cell_row_align", "Cell row align", "choice", "STRETCH", GRID_ALIGNMENTS),
)

FONTS = ("small", "default", "large", "icon")
LONG_MODES = ("NONE", "WRAP", "DOTS", "SCROLL", "SCROLL_CIRCULAR", "CLIP")
TEXT_ALIGNMENTS = ("LEFT", "CENTER", "RIGHT")
OPACITIES = ("COVER", "TRANSP", "50")
VARIANTS = ("STANDARD", "FILLED", "TONAL")

TEXT_PROPS = (
    PropertySpec("text", "Text", "text", "Label"),
    PropertySpec("text_color", "Text colour", "color", ""),
    PropertySpec("text_font", "Font", "choice", "default", FONTS),
    PropertySpec("text_align", "Text align", "choice", "LEFT", TEXT_ALIGNMENTS),
    PropertySpec("long_mode", "Long mode", "choice", "NONE", LONG_MODES),
)

# `None` means "use the theme's shape". The exporter omits it, so the widget
# inherits the same radius as a C++ app and follows the size setting.
RADIUS = PropertySpec("radius", "Radius", "int", None, minimum=0, maximum=512, optional=True)

SURFACE_PROPS = (
    PropertySpec("bg_color", "Background", "color", ""),
    PropertySpec("bg_opa", "Background opacity", "choice", "COVER", OPACITIES),
    PropertySpec("border_width", "Border width", "int", None, minimum=0, maximum=16, optional=True),
    PropertySpec("border_color", "Border colour", "color", ""),
    RADIUS,
    PropertySpec("pad_all", "Padding", "int", 0, minimum=0, maximum=64),
)

FLEX_ALIGNMENTS = ("START", "CENTER", "END", "SPACE_EVENLY", "SPACE_AROUND", "SPACE_BETWEEN")
FLEX_PROPS = (
    PropertySpec("layout", "Layout", "choice", "NONE", ("NONE", "FLEX", "GRID")),
    PropertySpec("flex_flow", "Flex flow", "choice", "ROW", ("ROW", "COLUMN", "ROW_WRAP", "COLUMN_WRAP")),
    PropertySpec("flex_main", "Flex main", "choice", "START", FLEX_ALIGNMENTS),
    PropertySpec("flex_cross", "Flex cross", "choice", "START", FLEX_ALIGNMENTS),
    PropertySpec("flex_track", "Flex track", "choice", "START", FLEX_ALIGNMENTS),
    PropertySpec("pad_row", "Row gap", "int", 0, minimum=0, maximum=64),
    PropertySpec("pad_column", "Column gap", "int", 0, minimum=0, maximum=64),
    # Only shown when this container's layout is GRID.
    PropertySpec("grid_columns", "Grid columns", "text", "1fr, 1fr"),
    PropertySpec("grid_rows", "Grid rows", "text", "1fr"),
    PropertySpec("grid_col_align", "Grid column align", "choice", "START", FLEX_ALIGNMENTS),
    PropertySpec("grid_row_align", "Grid row align", "choice", "START", FLEX_ALIGNMENTS),
)

VALUE_PROPS = (
    PropertySpec("min", "Minimum", "int", 0),
    PropertySpec("max", "Maximum", "int", 100),
    PropertySpec("value", "Value", "int", 0),
)


def _spec(
    type_name: str,
    title: str,
    category: str,
    width: int,
    height: int,
    *,
    container: bool = False,
    summary: str = "",
    properties: tuple[PropertySpec, ...] = (),
    events: tuple[str, ...] = (),
) -> WidgetSpec:
    return WidgetSpec(
        type=type_name,
        title=title,
        category=category,
        default_width=width,
        default_height=height,
        container=container,
        summary=summary,
        properties=GEOMETRY + properties,
        events=events,
    )


WIDGET_SPECS: tuple[WidgetSpec, ...] = (
    _spec(
        "Card",
        "Card",
        "Containers",
        360,
        200,
        container=True,
        summary="Themed surface for grouping content",
        properties=(
            SURFACE_PROPS[0],
            SURFACE_PROPS[1],
            SURFACE_PROPS[2],
            SURFACE_PROPS[3],
            SURFACE_PROPS[4],
            PropertySpec("pad_all", "Padding", "int", 12, minimum=0, maximum=64),
            *FLEX_PROPS,
        ),
    ),
    _spec(
        "Object",
        "Container",
        "Containers",
        320,
        180,
        container=True,
        summary="Plain flex/absolute container",
        properties=(
            SURFACE_PROPS[0],
            PropertySpec("bg_opa", "Background opacity", "choice", "TRANSP", OPACITIES),
            *SURFACE_PROPS[2:],
            *FLEX_PROPS,
        ),
    ),
    _spec(
        "Label",
        "Label",
        "Text",
        180,
        28,
        summary="Single-style text",
        properties=TEXT_PROPS,
    ),
    _spec(
        "Button",
        "Button",
        "Actions",
        140,
        40,
        summary="Filled button",
        properties=TEXT_PROPS + (RADIUS,),
        events=("CLICKED",),
    ),
    _spec(
        "Chip",
        "Chip",
        "Actions",
        120,
        40,
        summary="Compact selectable choice",
        properties=TEXT_PROPS + (
            SURFACE_PROPS[0],
            SURFACE_PROPS[2],
            SURFACE_PROPS[3],
            RADIUS,
            PropertySpec("checked", "Selected", "bool", False),
        ),
        events=("CLICKED",),
    ),
    _spec(
        "IconButton",
        "Icon button",
        "Actions",
        56,
        56,
        summary="Icon or short text action",
        properties=TEXT_PROPS + (
            RADIUS,
            PropertySpec("variant", "Variant", "choice", "STANDARD", VARIANTS),
        ),
        events=("CLICKED",),
    ),
    _spec(
        "TextButton",
        "Text button",
        "Actions",
        120,
        40,
        summary="Low-emphasis text action",
        properties=TEXT_PROPS + (RADIUS,),
        events=("CLICKED",),
    ),
    _spec(
        "SliderBox",
        "Slider box",
        "Input",
        300,
        48,
        summary="Slider with - / + buttons and a value",
        properties=(
            PropertySpec("min", "Minimum", "int", 0),
            PropertySpec("max", "Maximum", "int", 100),
            PropertySpec("step", "Step", "int", 5, minimum=1),
            PropertySpec("value", "Value", "int", 50),
        ),
        events=("VALUE_CHANGED",),
    ),
    _spec(
        "Spinbox",
        "Spinbox",
        "Input",
        120,
        64,
        summary="Number field that clamps to a range",
        properties=(
            PropertySpec("min", "Minimum", "int", 0),
            PropertySpec("max", "Maximum", "int", 100),
            PropertySpec("step", "Step", "int", 1, minimum=1),
            PropertySpec("digits", "Digits", "int", 2, minimum=1, maximum=9),
            PropertySpec("decimals", "Decimals", "int", 0, minimum=0, maximum=9),
            PropertySpec("value", "Value", "int", 0),
            TEXT_PROPS[1],
            TEXT_PROPS[2],
            RADIUS,
        ),
        events=("VALUE_CHANGED",),
    ),
    _spec(
        "Slider",
        "Slider",
        "Input",
        200,
        20,
        summary="Continuous value",
        properties=VALUE_PROPS,
        events=("VALUE_CHANGED",),
    ),
    _spec(
        "Switch",
        "Switch",
        "Input",
        60,
        32,
        summary="Boolean toggle",
        properties=(
            PropertySpec("checked", "On", "bool", False),
        ),
        events=("VALUE_CHANGED",),
    ),
    _spec(
        "Checkbox",
        "Checkbox",
        "Input",
        180,
        28,
        summary="Boolean with a label",
        properties=TEXT_PROPS + (
            PropertySpec("checked", "Checked", "bool", False),
        ),
        events=("VALUE_CHANGED",),
    ),
    _spec(
        "Dropdown",
        "Dropdown",
        "Input",
        200,
        40,
        summary="One choice from a list",
        properties=(
            PropertySpec("options", "Options (one per line)", "multiline", "One\nTwo\nThree"),
            PropertySpec("value", "Selected (1-based)", "int", 1, minimum=1),
            RADIUS,
        ),
        events=("VALUE_CHANGED",),
    ),
    _spec(
        "Roller",
        "Roller",
        "Input",
        160,
        120,
        summary="Scrollable choice wheel",
        properties=(
            PropertySpec("options", "Options (one per line)", "multiline", "One\nTwo\nThree"),
            PropertySpec("value", "Selected (1-based)", "int", 1, minimum=1),
            RADIUS,
        ),
        events=("VALUE_CHANGED",),
    ),
    _spec(
        "TextArea",
        "Text area",
        "Input",
        260,
        120,
        summary="Editable text",
        properties=(
            PropertySpec("text", "Text", "text", ""),
            PropertySpec("placeholder", "Placeholder", "text", "Type here"),
            PropertySpec("one_line", "One line", "bool", False),
            PropertySpec("password_mode", "Password mode", "bool", False),
            PropertySpec("text_color", "Text colour", "color", ""),
            PropertySpec("text_font", "Font", "choice", "default", FONTS),
            PropertySpec("text_align", "Text align", "choice", "LEFT", TEXT_ALIGNMENTS),
            RADIUS,
        ),
    ),
    _spec(
        "Arc",
        "Arc",
        "Indicators",
        120,
        120,
        summary="Circular value indicator",
        properties=VALUE_PROPS + (
            PropertySpec("arc_start", "Start angle", "int", 135, minimum=0, maximum=360),
            PropertySpec("arc_end", "End angle", "int", 45, minimum=0, maximum=360),
        ),
        events=("VALUE_CHANGED",),
    ),
    _spec(
        "Line",
        "Line",
        "Media",
        200,
        120,
        summary="Polyline from x,y points",
        properties=(
            PropertySpec("points", "Points (one x,y per line)", "multiline", "10,100\n60,30\n120,90\n180,10\n190,110"),
        ),
    ),
    _spec(
        "LED",
        "LED",
        "Indicators",
        24,
        24,
        summary="Single-colour indicator light",
        properties=(
            PropertySpec("led_color", "Colour", "color", ""),
            PropertySpec("brightness", "Brightness (0-255)", "int", 255, minimum=0, maximum=255),
            PropertySpec("on", "On", "bool", True),
        ),
    ),
    _spec(
        "Scale",
        "Scale",
        "Indicators",
        200,
        48,
        summary="Ticks and labels for a range",
        properties=(
            PropertySpec("min", "Minimum", "int", 0),
            PropertySpec("max", "Maximum", "int", 100),
            PropertySpec("tick_count", "Tick count", "int", 21, minimum=2),
            PropertySpec("major_tick_every", "Major tick every", "int", 5, minimum=1),
            PropertySpec("label_show", "Show labels", "bool", True),
            PropertySpec("scale_angle", "Angle range", "int", 240, minimum=1, maximum=360),
            PropertySpec("scale_rotation", "Rotation", "int", 0, minimum=0, maximum=360),
            PropertySpec("scale_mode", "Mode", "choice", "HORIZONTAL_TOP", (
                "HORIZONTAL_TOP", "HORIZONTAL_BOTTOM", "VERTICAL_LEFT",
                "VERTICAL_RIGHT", "ROUND_INNER", "ROUND_OUTER",
            )),
        ),
    ),
    _spec(
        "ImageButton",
        "Image button",
        "Actions",
        80,
        80,
        summary="Button with an image source",
        properties=(
            PropertySpec("src", "Image source", "text", ""),
            RADIUS,
        ),
        events=("CLICKED",),
    ),
    _spec(
        "Tabs",
        "Tabs",
        "Navigation",
        360,
        52,
        summary="Material 3 tab bar",
        properties=(
            PropertySpec("items", "Tabs (ICON|Label or Label)", "multiline", "MEMORY|Memory\nHARD_DISK|Storage\nINFO|About"),
            PropertySpec("mode", "Mode", "choice", "FIXED", ("FIXED", "SCROLLABLE", "WRAP")),
            PropertySpec("selected", "Selected (1-based)", "int", 1, minimum=1),
        ),
        events=("VALUE_CHANGED",),
    ),
    _spec(
        "Statusline",
        "Statusline",
        "Indicators",
        320,
        28,
        summary="Status text with a level dot",
        properties=(
            PropertySpec("text", "Text", "text", "All good"),
            PropertySpec("level", "Level", "choice", "OK", ("OK", "WARNING", "ERROR")),
        ),
    ),
    _spec(
        "Bar",
        "Bar",
        "Indicators",
        200,
        12,
        summary="Progress or level",
        properties=VALUE_PROPS,
    ),
    _spec(
        "List",
        "List",
        "Containers",
        320,
        220,
        container=False,
        summary="Rows with dividers",
        properties=(
            PropertySpec("items", "Rows (one per line)", "multiline", "Section\nItem one\nItem two"),
            RADIUS,
        ),
    ),
    _spec(
        "PageIndicator",
        "Page indicator",
        "Indicators",
        220,
        24,
        summary="Dots or page number",
        properties=(
            PropertySpec("page_count", "Pages", "int", 5, minimum=1),
            PropertySpec("page", "Current page (0-based)", "int", 0, minimum=0),
            PropertySpec("vertical", "Vertical", "bool", False),
        ),
    ),
    _spec(
        "StepIndicator",
        "Step indicator",
        "Indicators",
        260,
        12,
        summary="Progress through steps",
        properties=(
            PropertySpec("step_count", "Steps", "int", 4, minimum=1),
            PropertySpec("finished_count", "Finished steps", "int", 0, minimum=0),
        ),
    ),
    _spec(
        "Badge",
        "Badge",
        "Indicators",
        16,
        16,
        summary="Small marker dot",
    ),
    _spec(
        "Spinner",
        "Spinner",
        "Indicators",
        48,
        48,
        summary="Busy indicator",
    ),
    _spec(
        "Image",
        "Image",
        "Media",
        120,
        120,
        summary="Image placeholder",
        properties=(
            PropertySpec("src", "Source", "text", ""),
            RADIUS,
        ),
    ),
)

SPEC_BY_TYPE = {spec.type: spec for spec in WIDGET_SPECS}
CATEGORIES = tuple(dict.fromkeys(spec.category for spec in WIDGET_SPECS))


def themed_default_size(spec: WidgetSpec, theme) -> tuple[int, int]:
    """A new widget's size, following the theme's control metrics.

    The static defaults are sensible at a desktop-ish scale; this grows them
    with the device's DPI and appearance settings so a new Button or Switch
    looks like the firmware's default instead of a tiny desktop widget.
    """
    width, height = spec.default_width, spec.default_height
    font = theme.font_px("default")
    kind = spec.type

    if kind == "Label":
        height = max(height, font + 4)
    elif kind in ("Button", "TextButton"):
        height = max(height, font + 2 * theme.control_pad_ver() + 2)
    elif kind == "IconButton":
        side = max(48, theme.toolbar_action_size())
        width = height = side
    elif kind == "Chip":
        height = max(height, font + 2 * theme.chip_pad_ver())
    elif kind == "Checkbox":
        height = max(height, font + 4)
    elif kind == "Switch":
        width, height = theme.switch_size()
    elif kind == "Bar":
        height = max(height, theme.dp(8))
    elif kind == "PageIndicator":
        height = max(height, theme.dp(8) + 10)
    elif kind == "StepIndicator":
        height = max(height, theme.dp(6) + 6)
    elif kind == "Badge":
        side = lerp_px(4, theme.dp(6), theme.settings.size)
        width = height = max(8, side)
    elif kind == "Spinner":
        side = max(24, theme.dp(48))
        width = height = side
    elif kind in ("Dropdown", "TextArea"):
        height = max(height, font + 2 * theme.field_pad_ver() + 2)

    return max(8, int(width)), max(8, int(height))


def lerp_px(compact: int, regular: int, percent: int) -> int:
    percent = max(0, min(100, percent))
    return int(compact + ((regular - compact) * percent) // 100)
