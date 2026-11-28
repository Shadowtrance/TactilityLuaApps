-- Input group and focus navigation test.
--
-- Groups are what direct encoders and keyboards to a set of focusable widgets.
-- Close the app afterwards: the runtime must delete the group and clear it from
-- the default slot without crashing.

local checks = 0

local function check(condition, message)
    checks = checks + 1
    if not condition then
        error("group: " .. (message or ("check " .. checks .. " failed")), 2)
    end
end

local function run_tests()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN, pad_all = 8, pad_row = 6 }

    lvgl.Label(screen, { text = "Input group / focus test" })

    local app_group = lvgl.app_group()
    check(app_group ~= nil, "app group is available")
    check(type(app_group:count()) == "number", "app group count")

    local group = lvgl.Group()

    local buttons = {}
    for i = 1, 3 do
        buttons[i] = lvgl.Button(screen, { w = 220, h = 48 })
        lvgl.Label(buttons[i], {
            text = "Focus " .. i,
            text_align = lvgl.TEXT_ALIGN.CENTER,
            w = lvgl.PCT(100),
        })
        group:add(buttons[i])
    end

    check(group:count() == 3, "group count")
    check(group:obj(1) == buttons[1] and group:obj(3) == buttons[3], "group obj")
    check(group:obj(4) == nil, "group obj bounds")

    group:focus(buttons[2])
    check(group:focused() == buttons[2], "focus object")

    group:next()
    check(group:focused() == buttons[3], "focus next")

    group:prev()
    check(group:focused() == buttons[2], "focus prev")

    group:set_wrap(true)
    check(group:wrap(), "wrap enabled")
    group:set_wrap(false)
    check(not group:wrap(), "wrap disabled")

    check(not group:editing(), "editing default")
    group:set_editing(true)
    check(group:editing(), "editing set")
    group:set_editing(false)
    check(not group:editing(), "editing cleared")

    group:freeze(true)
    group:next()
    check(group:focused() == buttons[2], "focus freeze")
    group:freeze(false)
    group:next()
    check(group:focused() == buttons[3], "focus unfreeze")

    group:remove(buttons[2])
    check(group:count() == 2, "group remove")

    group:set_default()
    check(group:is_default(), "group default")

    local attached, input_count = group:attach_input()
    check(attached == group and input_count > 0, "attach input")
    check(input_count <= 8, "attach input cap")
    group:detach_input()
    local reattached, reattached_count = group:attach_input()
    check(reattached == group and reattached_count > 0, "reattach input")

    lvgl.Label(screen, { text = "group: PASS (" .. checks .. " checks)" })

    result = "group PASS (" .. checks .. " checks)"
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
