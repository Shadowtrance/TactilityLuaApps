-- System Info: a Lua counterpart of the firmware's System Info app.
--
-- It uses the Material 3 Tabs and Statusline widgets, the display capability and
-- the system memory/storage readings. Tab panes are created once and toggled with
-- the hidden flag, so switching tabs never rebuilds the layout.

local root = lvgl.root()
root:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN, pad_all = 0 }

local gap = lvgl.theme_lerp(4, 10)

local function bytes(value)
    if value >= 1024 * 1024 * 1024 then
        return string.format("%.1f GB", value / (1024 * 1024 * 1024))
    elseif value >= 1024 * 1024 then
        return string.format("%.1f MB", value / (1024 * 1024))
    elseif value >= 1024 then
        return string.format("%.0f KB", value / 1024)
    end
    return string.format("%d B", value)
end

local function uptime_text(milliseconds)
    local total = math.floor(milliseconds / 1000)
    local days = math.floor(total / 86400)
    local hours = math.floor(total % 86400 / 3600)
    local minutes = math.floor(total % 3600 / 60)
    local seconds = total % 60
    if days > 0 then
        return string.format("%dd %02d:%02d:%02d", days, hours, minutes, seconds)
    end
    return string.format("%02d:%02d:%02d", hours, minutes, seconds)
end

local function usage_card(parent, icon, title)
    local card = lvgl.Card(parent, {
        w = lvgl.PCT(100),
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        pad_all = lvgl.theme_lerp(8, 18),
        pad_row = gap,
    })

    local header = lvgl.Object(card, {
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.ROW,
        flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
        pad_all = 0, pad_column = gap,
        bg_opa = lvgl.OPA.TRANSP, border_width = 0,
    })
    lvgl.Label(header, {
        text = icon, text_font = "icon", text_color = lvgl.theme_color_primary(),
    })
    lvgl.Label(header, { text = title, text_font = "large", flex_grow = 1 })
    local percent_label = lvgl.Label(header, { text = "--", text_font = "small" })

    local bar = lvgl.Bar(card, {
        w = lvgl.PCT(100),
        h = lvgl.theme_lerp(8, 12),
        min = 0,
        max = 100,
        value = 0,
    })
    local detail = lvgl.Label(card, {
        text = "...",
        w = lvgl.PCT(100),
        text_font = "small",
        long_mode = lvgl.LONG.WRAP,
    })

    return {
        update = function(free, total, used, note)
            if note ~= nil then
                bar:add_flag(lvgl.FLAG.HIDDEN)
                percent_label:set{ text = "--" }
                detail:set{ text = note }
                return
            end

            bar:remove_flag(lvgl.FLAG.HIDDEN)
            local percent = total > 0 and math.floor((used * 100 + total / 2) / total) or 0
            bar:set{ value = percent }
            percent_label:set{ text = string.format("%d%%", percent) }
            if total == 0 then
                detail:set{ text = "Not available" }
            else
                detail:set{ text = string.format("%s free of %s", bytes(free), bytes(total)) }
            end
        end,
    }
end

local function info_row(parent, label, value)
    local row = lvgl.Object(parent, {
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.ROW,
        flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
        pad_all = 0, pad_column = gap,
        bg_opa = lvgl.OPA.TRANSP, border_width = 0,
    })
    lvgl.Label(row, { text = label, text_font = "small", flex_grow = 1 })
    local value_label = lvgl.Label(row, { text = value, text_font = "small" })
    return value_label
end

local statusline = lvgl.Statusline(root, {
    text = "Starting...",
    level = lvgl.STATUSLINE_LEVEL.OK,
})

local tabs = lvgl.Tabs(root, { mode = lvgl.TABS_MODE.FIXED })
tabs:add_tab_with_icon(lvgl.ICON.MEMORY, "Memory")
tabs:add_tab_with_icon(lvgl.ICON.HARD_DISK, "Storage")
tabs:add_tab_with_icon(lvgl.ICON.INFO, "About")

local panes = {}
local pane_container = lvgl.Object(root, {
    w = lvgl.PCT(100), flex_grow = 1,
    layout = lvgl.LAYOUT.NONE,
    pad_all = 0,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})

for index = 1, 3 do
    panes[index] = lvgl.Object(pane_container, {
        w = lvgl.PCT(100), h = lvgl.PCT(100),
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        pad_all = lvgl.theme_lerp(6, 14),
        pad_row = gap,
        bg_opa = lvgl.OPA.TRANSP, border_width = 0,
    })
end

local internal_card = usage_card(panes[1], lvgl.ICON.MEMORY, "Internal memory")
local external_card = usage_card(panes[1], lvgl.ICON.MEMORY, "External memory")
local storage_card = usage_card(panes[2], lvgl.ICON.HARD_DISK, "SD card")

local display = tactility.display
local about_card = lvgl.Card(panes[3], {
    w = lvgl.PCT(100),
    flex_grow = 1,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_all = lvgl.theme_lerp(8, 18),
    pad_row = gap,
})
local width, height = display.size()
local shape = display.shape()
info_row(about_card, "Display", display.name() or "unknown")
info_row(about_card, "Resolution", string.format("%d x %d", lvgl.HOR_RES(), lvgl.VER_RES()))
info_row(about_card, "Panel", string.format("%d x %d", width, height))
info_row(about_card, "Shape", string.format("%s (radius %d)", shape.shape, shape.radius))
info_row(about_card, "Devices", tostring(#tactility.device.list()))
local about_brightness = info_row(about_card, "Brightness", "--")
local about_uptime = info_row(about_card, "Uptime", "--")
info_row(about_card, "Lua", _VERSION or "unknown")
info_row(about_card, "Runtime API", tostring(_LUA_RUNTIME_API or "?"))

local function select_pane(index)
    for i, pane in ipairs(panes) do
        if i == index then
            pane:remove_flag(lvgl.FLAG.HIDDEN)
        else
            pane:add_flag(lvgl.FLAG.HIDDEN)
        end
    end
end

tabs:on(lvgl.EVENT.VALUE_CHANGED, function()
    select_pane(tabs:selected() or 1)
end)

select_pane(1)

local function refresh()
    local memory = tactility.system.memory()
    internal_card.update(memory.internal.free, memory.internal.total, memory.internal.used)
    if memory.external.total > 0 then
        external_card.update(memory.external.free, memory.external.total, memory.external.used)
    else
        external_card.update(0, 0, 0, "No external memory on this device")
    end

    local storage, storage_reason = tactility.system.storage("/sdcard")
    if storage ~= nil then
        storage_card.update(storage.free, storage.total, storage.used)
        statusline:set_text("System running normally")
        statusline:set_level(lvgl.STATUSLINE_LEVEL.OK)
    else
        storage_card.update(0, 0, 0, storage_reason)
        statusline:set_text("Storage info needs a newer firmware")
        statusline:set_level(lvgl.STATUSLINE_LEVEL.WARNING)
    end

    if memory.internal.total > 0 and memory.internal.percent >= 90 then
        statusline:set_text("Internal memory is running low")
        statusline:set_level(lvgl.STATUSLINE_LEVEL.WARNING)
    end

    about_uptime:set{ text = uptime_text(tactility.time.uptime()) }
    if display.has_backlight() then
        local percent = display.brightness_percent()
        about_brightness:set{ text = percent ~= nil and (percent .. "%") or "--" }
    else
        about_brightness:set{ text = "none" }
    end
end

refresh()
lvgl.Timer{ period = 1000, callback = refresh }
result = "system info"
