-- luavgl regression smoke test.
--
-- Run it from the Lua app (or package it). It exercises the object lifetime,
-- widget, style, event and timer paths that the old C++ suite covered. The
-- result line reports how many checks passed; errors raise with the check name.

local checks = 0
local content

local function check(condition, message)
    checks = checks + 1
    if not condition then
        error("luavgl smoke: " .. (message or ("check " .. checks .. " failed")), 2)
    end
end

function on_show()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN, pad_all = 8, pad_row = 6 }

    content = lvgl.Object(screen, {
        w = lvgl.PCT(100),
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        pad_all = 8,
        pad_row = 6,
    })
    content:add_style(lvgl.Style{ bg_color = 0x1c1c28, border_width = 0, radius = 8, pad_all = 8 })

    local label = lvgl.Label(content, { text = "luavgl smoke test", w = lvgl.PCT(100) })
    check(label:text() == "luavgl smoke test", "label text")

    -- Textarea lifecycle: the old editor crashed when one was deleted, then the
    -- app closed. Create, clean the parent, recreate, edit, delete.
    local holder = lvgl.Object(content, { w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT })
    local area = lvgl.TextArea(holder, { w = lvgl.PCT(100), h = 80, text = "first" })
    check(area:text() == "first", "textarea created")
    holder:clean()
    check(not area:is_valid(), "cleaned textarea invalidated")

    area = lvgl.TextArea(holder, { text = "second" })
    check(area:text() == "second", "textarea recreated")
    area:set_cursor(1)
    area:insert("!")
    check(area:text() == "!second", "textarea insert at cursor")
    area:delete_char()
    check(area:text() == "second", "textarea delete_char")
    area:delete()
    check(not area:is_valid(), "textarea deleted")

    -- Widget values and checked state.
    local bar = lvgl.Bar(content, { w = lvgl.PCT(100), h = 8, min = 0, max = 10, value = 5 })
    check(bar:value() == 5, "bar value")
    local slider = lvgl.Slider(content, { w = lvgl.PCT(100), value = 3 })
    check(slider:value() == 3, "slider value")

    local switch = lvgl.Switch(content)
    switch:set_checked(true)
    check(switch:is_checked(), "switch checked")

    local checkbox = lvgl.Checkbox(content, { text = "check", checked = true })
    check(checkbox:is_checked(), "checkbox checked")
    check(checkbox:text() == "check", "checkbox text")

    -- Event callback and a timer. Neither fires during on_show, but arming them
    -- is what leaves callbacks pointing into the Lua state at teardown time.
    local taps = 0
    local button = lvgl.Button(content, { w = 120, h = 40 })
    lvgl.Label(button, { text = "tap" })
    button:on(lvgl.EVENT.CLICKED, function()
        taps = taps + 1
    end)

    local ticks = 0
    local timer
    timer = lvgl.Timer{
        period = 100,
        callback = function()
            ticks = ticks + 1
            if ticks >= 2 then
                timer:delete()
            end
        end,
    }

    lvgl.Label(screen, { text = "luavgl smoke: PASS (" .. checks .. " checks)" })
    result = "luavgl smoke PASS (" .. checks .. " checks)"
end

function on_destroy()
    -- Deleting a widget tree during teardown is the other half of the old crash:
    -- the delete callbacks must not outlive the Lua state they point at.
    if content and content:is_valid() then
        content:clean()
    end
end
