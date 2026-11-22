-- Focus timer: a Pomodoro-style 25/5 cycle on a themed arc.
--
-- The arc drains while the timer runs. Every fourth focus session gets a long
-- break. Everything uses theme fonts, colours and spacing, so it also follows
-- the density and light/dark settings.

local root = lvgl.root()
root:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN }

local FOCUS_SECONDS = 25 * 60
local SHORT_BREAK_SECONDS = 5 * 60
local LONG_BREAK_SECONDS = 15 * 60

local gap = lvgl.theme_lerp(4, 16)

local header = lvgl.Object(root, {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
lvgl.Label(header, { text = "Focus", text_font = "large", flex_grow = 1 })
local sessions_label = lvgl.Label(header, { text = "Sessions 0", text_font = "small" })

local stage = lvgl.Object(root, {
    w = lvgl.PCT(100), flex_grow = 1,
    layout = lvgl.LAYOUT.NONE,
    pad_all = 0,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})

local short_side = math.min(lvgl.HOR_RES(), lvgl.VER_RES())
local arc_size = math.max(96, math.min(420, math.floor(short_side * 0.55)))
local arc = lvgl.Arc(stage, {
    w = arc_size, h = arc_size,
    min = 0, max = FOCUS_SECONDS, value = FOCUS_SECONDS,
    align = lvgl.ALIGN.CENTER,
})

local centre = lvgl.Object(stage, {
    w = lvgl.SIZE_CONTENT, h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    flex_align = { main = lvgl.FLEX_ALIGN.CENTER, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0, pad_row = 0,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
    align = lvgl.ALIGN.CENTER,
})
local time_label = lvgl.Label(centre, {
    text = "25:00",
    text_font = arc_size >= 220 and "large" or "default",
})
local mode_label = lvgl.Label(centre, { text = "Focus", text_font = "small" })

local controls = lvgl.Object(root, {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.CENTER, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
local start_button = lvgl.Button(controls, { w = lvgl.SIZE_CONTENT, h = lvgl.SIZE_CONTENT })
local start_label = lvgl.Label(start_button, { text = "Start", align = lvgl.ALIGN.CENTER })
local reset_button = lvgl.TextButton(controls)
lvgl.Label(reset_button, { text = "Reset", align = lvgl.ALIGN.CENTER })

local mode = "Focus"
local total = FOCUS_SECONDS
local remaining = FOCUS_SECONDS
local running = false
local sessions = 0
local timer

local function format_time(seconds)
    return string.format("%02d:%02d", math.floor(seconds / 60), seconds % 60)
end

local function render()
    arc:set{ max = total, value = remaining }
    time_label:set{ text = format_time(remaining) }
    mode_label:set{ text = mode }
    sessions_label:set{ text = "Sessions " .. sessions }
end

local function set_mode(new_mode, seconds)
    mode = new_mode
    total = seconds
    remaining = seconds
    running = false
    timer:pause()
    start_label:set{ text = "Start" }
    render()
end

local function tick()
    if remaining <= 0 then
        return
    end
    remaining = remaining - 1
    if remaining == 0 then
        if mode == "Focus" then
            sessions = sessions + 1
            local long_break = sessions % 4 == 0
            set_mode(long_break and "Long break" or "Break", long_break and LONG_BREAK_SECONDS or SHORT_BREAK_SECONDS)
        else
            set_mode("Focus", FOCUS_SECONDS)
        end
        return
    end
    render()
end

timer = lvgl.Timer{ period = 1000, callback = tick, paused = true }
local function start_pause()
    running = not running
    if running then
        timer:resume()
    else
        timer:pause()
    end
    start_label:set{ text = running and "Pause" or "Start" }
end

start_button:on(lvgl.EVENT.CLICKED, start_pause)

reset_button:on(lvgl.EVENT.CLICKED, function()
    set_mode("Focus", FOCUS_SECONDS)
end)

render()
result = "focus timer"
