"""Write a grid project and its generated Lua for the on-device check."""

from pathlib import Path

from lua_export import export
from model import Project

project = Project.new()
card = project.add_widget("Object")
card.x, card.y, card.w, card.h = 40, 60, 620, 300
card.props.update(
    {
        "layout": "GRID",
        "grid_columns": "1fr, 1fr",
        "grid_rows": "80, content",
        "grid_col_align": "START",
        "grid_row_align": "START",
        "pad_all": 12,
        "pad_row": 8,
        "pad_column": 8,
        "bg_color": "#202030",
        "border_width": 1,
        "border_color": "#445566",
        "radius": 12,
    }
)

first = project.add_widget("Button", card.id)
first.props.update({"text": "Cell 0,0", "grid_col": 0, "grid_row": 0})
second = project.add_widget("Button", card.id)
second.props.update({"text": "Cell 1,0", "grid_col": 1, "grid_row": 0})
wide = project.add_widget("Label", card.id)
wide.props.update({"text": "Span 2", "grid_col": 0, "grid_row": 1, "grid_col_span": 2})

output = Path(__file__).resolve().parents[2] / "build" / "grid_designer.lua"
output.write_text(export(project), encoding="utf-8")
print(output)
