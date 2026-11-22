-- Package asset test.
--
-- The script receives its install directory in `...`. That path is POSIX style, so the
-- LVGL bindings prefix `A:` for the stdio filesystem driver. Package with:
--
--     py tools/lua-app/make_lua_app.py --script tests/assets.lua \
--         --assets tests/assets --name "Lua Assets" --id tactility.lua.assets

local checks = 0

-- `...` is only valid in the main chunk; capture it before defining functions.
local script_directory = ...

local function check(condition, message)
    checks = checks + 1
    if not condition then
        error("assets: " .. (message or ("check " .. checks .. " failed")), 2)
    end
end

local function run_tests()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN, pad_all = 8, pad_row = 8 }

    check(type(script_directory) == "string" and script_directory ~= "", "install directory argument")

    local path = script_directory .. "/assets/baby_idle.png"

    local image = lvgl.Image(screen, { w = 64, h = 64, src = path })
    check(image:is_valid(), "image from package asset")

    local button = lvgl.ImageButton(screen, { w = 64, h = 64 })
    button:set_image_src(lvgl.IMAGEBUTTON.RELEASED, nil, path, nil)
    check(button:is_valid(), "image button from package asset")

    lvgl.Label(screen, {
        text = "assets: PASS (" .. checks .. " checks)\n" .. path,
        text_align = lvgl.TEXT_ALIGN.CENTER,
        w = lvgl.PCT(100),
    })

    result = "assets PASS (" .. checks .. " checks)"
end

function on_show()
    local ok, error_message = pcall(run_tests)
    if ok then
        return
    end

    local label = lvgl.Label(lvgl.root(), {
        w = lvgl.PCT(100),
        text_color = 0xff5555,
        text = tostring(error_message),
    })
    label:add_flag(lvgl.FLAG.IGNORE_LAYOUT)
    label:set{ x = 8, y = 80 }
    result = tostring(error_message)
end
