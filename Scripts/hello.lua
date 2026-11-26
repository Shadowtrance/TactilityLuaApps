-- A minimal packaged Lua app: one label, centered on the page.
-- The runner's root is a flex column, so a full-size absolute page gives
-- `align` a stable surface to center in.
--
-- @version 0.1.2
-- @code 3
function on_show()
    local root = lvgl.root()
    local page = lvgl.Object(root, {
        w = lvgl.PCT(100),
        h = lvgl.PCT(100),
        layout = lvgl.LAYOUT.NONE,
        pad_all = 0,
        border_width = 0,
        bg_opa = lvgl.OPA.TRANSP,
    })
    lvgl.Label(page, {
        text = "Hello from Lua!",
        text_font = "large",
        align = lvgl.ALIGN.CENTER,
    })
    result = "hello"
end
