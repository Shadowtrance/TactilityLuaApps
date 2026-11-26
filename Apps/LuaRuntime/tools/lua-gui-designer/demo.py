"""The demo project shown when the designer starts.

Kept separate from `designer.py` so headless tools (the visual regression test)
can build the same scene without importing Tk or Custom2kinter.
"""

from __future__ import annotations

from model import Project
from theme import ThemeSettings


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
