-- Line points: LVGL stores the point array by pointer, so the binding owns it.
local root = lvgl.root()
root:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN, pad_all = 8, pad_row = 8 }

lvgl.Label(root, { text = "Line points" })

local line = lvgl.Line(root, {
    w = 300,
    h = 180,
})
line:set_points{ 10, 160, 60, 80, 120, 130, 180, 30, 240, 100, 290, 20 }
line:set_points{ 10, 20, 150, 160, 290, 20 }

result = "line: PASS"
