-- A live system dashboard, built entirely from Lua widgets.
--
-- The cards are the theme's Card class and the values use theme fonts and
-- colours, so the dashboard follows light/dark, mono and density settings.

local screen = lvgl.root()
screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN }
screen:add_flag(lvgl.FLAG.SCROLLABLE)

local function card(title)
    local box = lvgl.Card(screen, {
        w = lvgl.PCT(100),
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
    })
    lvgl.Label(box, { text = title, text_font = 'small' })
    return box
end

local battery_card = card('Battery')
local battery_bar = lvgl.Bar(battery_card, { w = lvgl.PCT(100), h = 10, min = 0, max = 100, value = 0 })
local battery_value = lvgl.Label(battery_card, { text = '...', text_font = 'large' })

local network_card = card('Network')
local network_value = lvgl.Label(network_card, {
    text = '...', w = lvgl.PCT(100), long_mode = lvgl.LONG.WRAP,
})
local ip_value = lvgl.Label(network_card, { text = '' })

local system_card = card('System')
local system_value = lvgl.Label(system_card, { text = '...', w = lvgl.PCT(100), long_mode = lvgl.LONG.WRAP })

local clock_card = card('Clock')
local clock_value = lvgl.Label(clock_card, { text = '...', text_font = 'large' })

local function refresh()
    local pct = nil
    if tactility.power.is_available() then
        pct = tactility.power.capacity()
    end
    battery_bar:set{ value = pct or 0 }
    battery_value:set{ text = pct and (pct .. '%') or 'n/a' }

    if tactility.wifi.is_available() then
        local state = tactility.wifi.station_state()
        local ssid = tactility.wifi.ssid()
        network_value:set{ text = ssid and (state .. ' / ' .. ssid) or state }
        ip_value:set{ text = tactility.wifi.ip_address() or 'no address' }
    else
        network_value:set{ text = 'Wi-Fi unavailable' }
        ip_value:set{ text = '' }
    end

    local now = tactility.time.now()
    if now then
        local t = tactility.time.date(now)
        clock_value:set{ text = string.format('%02d:%02d:%02d', t.hour, t.min, t.sec) }
    else
        clock_value:set{ text = 'clock not set' }
    end

    local mounts = tactility.fs.mounts() or {}
    system_value:set{
        text = string.format(
            '%d devices / %s / heap %d KB',
            #tactility.device.list(),
            table.concat(mounts, ' '),
            math.floor(tactility.heap_used() / 1024)
        ),
    }
end

refresh()
lvgl.Timer{ period = 1000, callback = refresh }
