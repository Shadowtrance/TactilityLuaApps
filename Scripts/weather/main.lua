-- @version 0.1.0
-- @code 1
--
-- A small but complete network app: a city picker, an async Open-Meteo request,
-- JSON decoding and a themed current + 5-day forecast. The network call runs on
-- the runtime's HTTP worker, so the UI stays responsive.

local cities = require("lib.cities")
local weather = require("lib.weather")

local SETTINGS_NAMESPACE = "weather"
local WEEKDAYS = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }

local saved_unit = tactility.settings.get(SETTINGS_NAMESPACE, "unit", "C")
local unit = saved_unit == "F" and "F" or "C"
local refresh_minutes = tonumber(tactility.settings.get(SETTINGS_NAMESPACE, "refresh", "10")) or 10
if refresh_minutes ~= 5 and refresh_minutes ~= 10 and refresh_minutes ~= 30 then
    refresh_minutes = 10
end
local last_data = nil

local root = lvgl.root()
root:set{
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_all = lvgl.theme_lerp(6, 14),
    pad_row = lvgl.theme_lerp(4, 10),
}
root:add_flag(lvgl.FLAG.SCROLLABLE)

-- Header ---------------------------------------------------------------------------------

local controls = lvgl.Object(root, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
    pad_column = lvgl.theme_lerp(6, 10),
})

local city_names = {}
for index, city in ipairs(cities) do
    city_names[index] = city.name
end

local city_dropdown = lvgl.Dropdown(controls, {
    options = city_names,
    flex_grow = 1,
})

local refresh_button = lvgl.IconButton(controls, {
    variant = lvgl.ICON_BUTTON.FILLED,
})
lvgl.Label(refresh_button, { text = lvgl.ICON.REFRESH, text_font = "icon" })

local settings_button = lvgl.IconButton(controls, {
    variant = lvgl.ICON_BUTTON.STANDARD,
})
lvgl.Label(settings_button, { text = lvgl.ICON.SETTINGS, text_font = "icon" })

-- Settings panel -------------------------------------------------------------------------

local settings_panel = lvgl.Card(root, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    pad_all = lvgl.theme_lerp(8, 16),
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_row = lvgl.theme_lerp(4, 8),
})
settings_panel:add_flag(lvgl.FLAG.HIDDEN)
local settings_visible = false

lvgl.Label(settings_panel, { text = "Settings", text_font = "small" })

local units_row = lvgl.Object(settings_panel, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
})
lvgl.Label(units_row, { text = "Units", text_font = "small" })
local units_dropdown = lvgl.Dropdown(units_row, {
    options = { "Celsius", "Fahrenheit" },
    w = lvgl.PCT(55),
})
units_dropdown:set{ value = unit == "F" and 2 or 1 }

local interval_row = lvgl.Object(settings_panel, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
})
lvgl.Label(interval_row, { text = "Refresh every", text_font = "small" })
local interval_dropdown = lvgl.Dropdown(interval_row, {
    options = { "5 minutes", "10 minutes", "30 minutes" },
    w = lvgl.PCT(55),
})
interval_dropdown:set{ value = refresh_minutes == 5 and 1 or (refresh_minutes == 30 and 3 or 2) }

-- Current conditions ---------------------------------------------------------------------

local width, height = tactility.display.size()
local forecast_days = (height and height < 400) and 4 or 5

local current_card = lvgl.Card(root, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    pad_all = lvgl.theme_lerp(8, 16),
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
})

local current_row = lvgl.Object(current_card, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
})

local temperature_label = lvgl.Label(current_row, {
    text = "--°",
    text_font = "large",
    text_color = lvgl.theme_color_primary(),
})

local current_details = lvgl.Object(current_row, {
    h = lvgl.SIZE_CONTENT,
    flex_grow = 1,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    flex_align = { main = lvgl.FLEX_ALIGN.CENTER, cross = lvgl.FLEX_ALIGN.END },
})

local condition_label = lvgl.Label(current_details, {
    text = "Waiting for data",
    w = lvgl.PCT(100),
    text_align = lvgl.TEXT_ALIGN.RIGHT,
})

local detail_label = lvgl.Label(current_details, {
    text = "",
    text_font = "small",
    w = lvgl.PCT(100),
    long_mode = lvgl.LONG.WRAP,
    text_align = lvgl.TEXT_ALIGN.RIGHT,
})

-- Forecast -------------------------------------------------------------------------------

local forecast_card = lvgl.Card(root, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    pad_all = lvgl.theme_lerp(8, 16),
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_row = lvgl.theme_lerp(2, 6),
})

lvgl.Label(forecast_card, { text = "Next " .. forecast_days .. " days", text_font = "small" })

local forecast_rows = {}
for index = 1, forecast_days do
    local row = lvgl.Object(forecast_card, {
        w = lvgl.PCT(100),
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.ROW,
        flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
    })
    forecast_rows[index] = {
        day = lvgl.Label(row, { text = "", text_font = "small", w = lvgl.PCT(22) }),
        condition = lvgl.Label(row, {
            text = "",
            text_font = "small",
            w = lvgl.PCT(50),
            long_mode = lvgl.LONG.DOTS,
        }),
        temperatures = lvgl.Label(row, {
            text = "",
            text_font = "small",
            w = lvgl.PCT(28),
            text_align = lvgl.TEXT_ALIGN.RIGHT,
        }),
    }
end

local status_label = lvgl.Label(root, {
    text = "",
    text_font = "small",
    text_color = lvgl.theme_color_secondary(),
})

-- Data -----------------------------------------------------------------------------------

local function round(value)
    return math.floor((tonumber(value) or 0) + 0.5)
end

local function format_temperature(celsius)
    local value = tonumber(celsius) or 0
    if unit == "F" then
        value = value * 9 / 5 + 32
    end
    return string.format("%d°", round(value))
end

local function format_wind(speed)
    if unit == "F" then
        return round((tonumber(speed) or 0) * 0.621371), "mph"
    end
    return round(speed), "km/h"
end

local function weekday(iso_date)
    local year, month, day = iso_date:match("^(%d+)-(%d+)-(%d+)$")
    if not year then
        return iso_date
    end
    local timestamp = os.time({
        year = tonumber(year),
        month = tonumber(month),
        day = tonumber(day),
        hour = 12,
        min = 0,
        sec = 0,
    })
    local broken = timestamp and tactility.time.date(timestamp)
    return (broken and WEEKDAYS[broken.wday]) or iso_date
end

local function update_status(text, color)
    status_label:set{ text = text, text_color = color or lvgl.theme_color_secondary() }
end

local function render(data)
    local current = data.current
    local wind, wind_unit = format_wind(current.wind_speed_10m)
    temperature_label:set{ text = format_temperature(current.temperature_2m) }
    condition_label:set{ text = weather.condition(current.weather_code) }
    detail_label:set{
        text = string.format(
            "Feels %s\nHumidity %d%%   Wind %d %s",
            format_temperature(current.apparent_temperature),
            round(current.relative_humidity_2m),
            wind,
            wind_unit
        ),
    }

    for index = 1, forecast_days do
        local row = forecast_rows[index]
        local time = data.daily.time and data.daily.time[index]
        local code = data.daily.weather_code and data.daily.weather_code[index]
        local maximum = data.daily.temperature_2m_max and data.daily.temperature_2m_max[index]
        local minimum = data.daily.temperature_2m_min and data.daily.temperature_2m_min[index]

        if time and code and maximum and minimum then
            row.day:set{ text = weekday(time) }
            row.condition:set{ text = weather.condition(code) }
            row.temperatures:set{
                text = string.format("%s / %s", format_temperature(maximum), format_temperature(minimum)),
            }
        else
            row.day:set{ text = "—" }
            row.condition:set{ text = "" }
            row.temperatures:set{ text = "" }
        end
    end
end

-- Refresh flow ---------------------------------------------------------------------------

local saved_city = tactility.settings.get(SETTINGS_NAMESPACE, "city", "1")
local city_index = math.floor(tonumber(saved_city) or 1)
if city_index < 1 or city_index > #cities then
    city_index = 1
end
city_dropdown:set{ value = city_index }

local generation = 0

local function refresh()
    generation = generation + 1
    local current_generation = generation
    local city = cities[city_index]

    update_status("Updating " .. city.name .. "...")
    refresh_button:add_state(lvgl.STATE.DISABLED)

    weather.fetch(city, function(data, error)
        if current_generation ~= generation then
            return -- a newer city or refresh won
        end
        refresh_button:remove_state(lvgl.STATE.DISABLED)

        if data == nil then
            update_status("Offline: " .. tostring(error), 0xff6b6b)
            return
        end

        last_data = data
        render(data)
        local now = tactility.time.now()
        if now then
            local broken = tactility.time.date(now)
            update_status(string.format("Updated %02d:%02d", broken.hour, broken.min))
        else
            update_status("Updated just now")
        end
    end)
end

refresh_button:on(lvgl.EVENT.CLICKED, refresh)

settings_button:on(lvgl.EVENT.CLICKED, function()
    settings_visible = not settings_visible
    if settings_visible then
        settings_panel:remove_flag(lvgl.FLAG.HIDDEN)
    else
        settings_panel:add_flag(lvgl.FLAG.HIDDEN)
    end
end)

units_dropdown:on(lvgl.EVENT.VALUE_CHANGED, function()
    local selected_unit = units_dropdown:value() == 2 and "F" or "C"
    if selected_unit == unit then
        return
    end
    unit = selected_unit
    tactility.settings.set(SETTINGS_NAMESPACE, "unit", unit)
    if last_data then
        render(last_data)
    end
end)

interval_dropdown:on(lvgl.EVENT.VALUE_CHANGED, function()
    local options = { 5, 10, 30 }
    local minutes = options[interval_dropdown:value()] or 10
    if minutes == refresh_minutes then
        return
    end
    refresh_minutes = minutes
    tactility.settings.set(SETTINGS_NAMESPACE, "refresh", tostring(minutes))
    if __weather_refresh_timer then
        __weather_refresh_timer:set_period(minutes * 60 * 1000)
    end
end)

city_dropdown:on(lvgl.EVENT.VALUE_CHANGED, function()
    local selected = city_dropdown:value()
    if selected == city_index then
        return
    end
    city_index = selected
    tactility.settings.set(SETTINGS_NAMESPACE, "city", tostring(city_index))
    refresh()
end)

-- Kept in a global so the timer is not collected while the app is open.
__weather_refresh_timer = lvgl.Timer{
    period = refresh_minutes * 60 * 1000,
    callback = refresh,
}

refresh()
