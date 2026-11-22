-- A minimal packaged Lua app: one label, centered on the runner's page.
function on_show()
    local root = lvgl.root()
    lvgl.Label(root, {
        text = "Hello from Lua!",
        text_font = "large",
        align = lvgl.ALIGN.CENTER,
    })
    result = "hello"
end
