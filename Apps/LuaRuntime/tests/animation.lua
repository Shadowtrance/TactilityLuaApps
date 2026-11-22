-- Numeric animation test.
--
-- The frame callback is C: the script describes one property and LVGL writes it.
-- A short timer checks the finished value, then the app can be closed.

local checks = 0
local timer

local function check(condition, message)
    checks = checks + 1
    if not condition then
        error("animation: " .. (message or ("check " .. checks .. " failed")), 2)
    end
end

local function run_tests()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN, pad_all = 8, pad_row = 6 }

    lvgl.Label(screen, { text = "Animation test" })

    local box = lvgl.Object(screen, {
        w = 60,
        h = 60,
        x = 20,
        y = 20,
        bg_color = 0x00aaff,
        radius = 8,
    })
    -- The screen is a flex column, so the box needs to opt out for x to be meaningful.
    box:add_flag(lvgl.FLAG.IGNORE_LAYOUT)

    local bar = lvgl.Bar(screen, {
        w = 260,
        h = 24,
        min = 0,
        max = 100,
        value = 0,
    })

    check(lvgl.ANIM_PATH.LINEAR ~= nil, "path linear")
    check(lvgl.ANIM_PATH.EASE_IN_OUT ~= nil, "path ease in out")
    check(lvgl.ANIM_INFINITE ~= nil, "infinite constant")

    local move = lvgl.Anim(box, {
        property = "x",
        from = 20,
        to = 220,
        time = 250,
        path = lvgl.ANIM_PATH.LINEAR,
    })

    check(not move:is_started(), "not started")
    check(move:target() == box, "target")

    move:start()
    check(move:is_started(), "move started")

    local stopped = lvgl.Anim(box, {
        property = "y",
        from = 20,
        to = 200,
        time = 5000,
    })
    stopped:start()
    check(stopped:is_started(), "stopped started")
    stopped:stop()
    check(not stopped:is_started(), "stop")

    local fill = lvgl.Anim(bar, {
        property = "value",
        from = 0,
        to = 100,
        time = 200,
        path = lvgl.ANIM_PATH.EASE_OUT,
    })
    fill:start()
    check(fill:is_started(), "fill started")

    local fade = lvgl.Anim(box, {
        property = "bg_color",
        from = 0x00aaff,
        to = 0xff5500,
        time = 200,
    })
    fade:start()

    local doomed = lvgl.Object(screen, { w = 20, h = 20 })
    local doomed_anim = lvgl.Anim(doomed, {
        property = "w",
        from = 20,
        to = 200,
        time = 5000,
    })
    doomed_anim:start()
    check(doomed_anim:is_started(), "doomed started")
    doomed:delete()
    check(not doomed_anim:is_started(), "delete stops animation")
    check(doomed_anim:target() == nil, "deleted target")

    local ok, error_message = pcall(function()
        return lvgl.Anim(box, { property = "not_a_property", from = 0, to = 1 })
    end)
    check(not ok and tostring(error_message):find("unknown animation property") ~= nil, "unknown property")

    local status = lvgl.Label(screen, { text = "animation: running" })

    timer = lvgl.Timer{
        period = 600,
        callback = function()
            timer:delete()

            local value_ok = bar:value() == 100
            local x_ok = box:position() >= 219
            if not (value_ok and x_ok) then
                status:set{ text = "animation: FAIL (value=" .. bar:value() .. ")" }
                result = "animation FAIL"
                return
            end

            checks = checks + 1
            status:set{ text = "animation: PASS (" .. checks .. " checks)" }
            result = "animation PASS (" .. checks .. " checks)"
        end,
    }

    lvgl.Label(screen, { text = "Close this app after the result appears." })
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
