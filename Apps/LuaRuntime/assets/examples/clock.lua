-- A live clock: the time updates every second inside a themed card, with the
-- full date and the device uptime. Before the clock is set (no NTP/RTC yet) it
-- says so instead of showing a date in 1970.

local WEEKDAYS = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
}
local MONTHS = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
}

local root = lvgl.root()
root:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN }

local card = lvgl.Card(root, {
    w = lvgl.PCT(100), flex_grow = 1,
    layout = lvgl.LAYOUT.FLEX,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    flex_align = { main = lvgl.FLEX_ALIGN.CENTER, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = lvgl.theme_lerp(12, 28),
    pad_row = lvgl.theme_lerp(2, 10),
})

local time_label = lvgl.Label(card, {
    text = "00:00:00",
    text_font = "large",
    text_letter_space = lvgl.theme_lerp(0, 2),
    text_color = lvgl.theme_color_primary(),
})
-- The theme's "large" font is still modest on a tablet, so draw the time at
-- twice its natural size. The transform doesn't change the layout box, so the
-- label also gets matching margins to reserve the extra height.
time_label:set{
    transform_scale_x = 512,
    transform_scale_y = 512,
}

local date_label = lvgl.Label(card, {
    text = "Waiting for the clock",
})

local uptime_label = lvgl.Label(card, {
    text = "",
    text_font = "small",
})

local function format_uptime(milliseconds)
    local total = math.floor(milliseconds / 1000)
    local days = math.floor(total / 86400)
    local hours = math.floor(total % 86400 / 3600)
    local minutes = math.floor(total % 3600 / 60)
    local seconds = total % 60
    if days > 0 then
        return string.format("Uptime %dd %02d:%02d:%02d", days, hours, minutes, seconds)
    end
    return string.format("Uptime %02d:%02d:%02d", hours, minutes, seconds)
end

local function update()
    uptime_label:set{ text = format_uptime(tactility.time.uptime()) }

    local now, reason = tactility.time.now()
    if not now then
        time_label:set{ text = "--:--:--" }
        date_label:set{ text = reason or "clock not set" }
        return
    end

    local t = tactility.time.date(now)
    time_label:set{
        text = string.format("%02d:%02d:%02d", t.hour, t.min, t.sec),
    }
    date_label:set{
        text = string.format(
            "%s, %d %s %d",
            WEEKDAYS[t.wday] or "",
            t.day,
            MONTHS[t.month] or "",
            t.year
        ),
    }
end

-- The time text only changes digit for digit, so one measurement with a
-- full-width sample fixes the box for every reading. Pivot at the centre so
-- the transform grows evenly instead of from the top-left corner.
card:update_layout()
local time_width, time_height = time_label:size()
time_label:set{
    w = time_width,
    h = time_height,
    text_align = lvgl.TEXT_ALIGN.CENTER,
    margin_top = math.floor(time_height / 2),
    margin_bottom = math.floor(time_height / 2),
    transform_pivot_x = math.floor(time_width / 2),
    transform_pivot_y = math.floor(time_height / 2),
}
update()
lvgl.Timer{ period = 1000, callback = update }
result = "clock"
