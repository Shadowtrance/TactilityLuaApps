-- The toolbar path bar: segments in the toolbar, path updates and parent lookup.

local toolbar = tactility.ui.toolbar()
assert(toolbar ~= nil, "runner toolbar")

local pressed = nil
local rebuilt = 0
local bar = lvgl.PathBar(toolbar, {
    path = "/sdcard/tactility/app",
    on_pressed = function(path, from_keys)
        pressed = { path = path, from_keys = from_keys }
    end,
    on_rebuilt = function()
        rebuilt = rebuilt + 1
    end,
})

assert(bar ~= nil, "path bar")
assert(bar:parent_segment() ~= nil, "parent segment")

bar:set_path("/sdcard")
assert(bar:parent_segment() ~= nil, "parent segment after set_path")
bar:focus_parent_segment()

result = string.format("path bar PASS: rebuilt %d, pressed %s", rebuilt, tostring(pressed))
