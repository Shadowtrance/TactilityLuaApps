-- @version 0.1.0
-- @code 1
--
-- A complete network app: multi-file modules, an asynchronous Open-Meteo request,
-- JSON decoding, a real toolbar action for settings, and a settings view for the
-- location (presets or custom coordinates), units and refresh interval.

local cities = require("lib.cities")
local weather = require("lib.weather")

local SETTINGS_NAMESPACE = "weather"
local WEEKDAYS = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }
local CUSTOM_OPTION = #cities + 1
local CUSTOM_NAME = "Custom location"

-- Settings ---------------------------------------------------------------------------------

local saved_unit = tactility.settings.get(SETTINGS_NAMESPACE, "unit", "C")
local unit = saved_unit == "F" and "F" or "C"

local refresh_minutes = tonumber(tactility.settings.get(SETTINGS_NAMESPACE, "refresh", "10")) or 10
if refresh_minutes ~= 5 and refresh_minutes ~= 10 and refresh_minutes ~= 30 then
    refresh_minutes = 10
end

local city_index = math.floor(tonumber(tactility.settings.get(SETTINGS_NAMESPACE, "city", "1")) or 1)
if city_index < 1 or city_index > #cities then
    city_index = 1
end

local using_custom = tactility.settings.get(SETTINGS_NAMESPACE, "custom", "0") == "1"
local custom_name = tactility.settings.get(SETTINGS_NAMESPACE, "name", CUSTOM_NAME)
local custom_latitude = tonumber(tactility.settings.get(SETTINGS_NAMESPACE, "latitude", "52.3676")) or 52.3676
local custom_longitude = tonumber(tactility.settings.get(SETTINGS_NAMESPACE, "longitude", "4.9041")) or 4.9041
if custom_name == nil or #custom_name == 0 then
    custom_name = CUSTOM_NAME
end

local last_data = nil

local function active_location()
    if using_custom then
        return { name = custom_name, latitude = custom_latitude, longitude = custom_longitude }
    end
    return cities[city_index]
end

-- Views ------------------------------------------------------------------------------------

local root = lvgl.root()
root:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN, pad_all = lvgl.theme_lerp(6, 14) }
root:add_flag(lvgl.FLAG.SCROLLABLE)

local main_view = lvgl.Object(root, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_row = lvgl.theme_lerp(4, 10),
})

local settings_view = lvgl.Object(root, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_row = lvgl.theme_lerp(4, 10),
})
settings_view:add_flag(lvgl.FLAG.HIDDEN)

-- Main view ---------------------------------------------------------------------------------

local location_label = lvgl.Label(main_view, { text = "--", text_font = "large" })

local current_card = lvgl.Card(main_view, {
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

local display_width, display_height = tactility.display.size()
local forecast_days = (display_height and display_height < 400) and 4 or 5

local forecast_card = lvgl.Card(main_view, {
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

local status_label = lvgl.Label(main_view, {
    text = "",
    text_font = "small",
    text_color = lvgl.theme_color_secondary(),
})

-- Settings view ----------------------------------------------------------------------------

local settings_title = lvgl.Label(settings_view, { text = "Location and display", text_font = "small" })

local city_names = {}
for index, city in ipairs(cities) do
    city_names[index] = city.name
end
city_names[CUSTOM_OPTION] = CUSTOM_NAME

local city_dropdown = lvgl.Dropdown(settings_view, { options = city_names, w = lvgl.PCT(100) })

local search_row = lvgl.Object(settings_view, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
    pad_column = lvgl.theme_lerp(4, 8),
})

local search_field = lvgl.TextArea(search_row, {
    one_line = true,
    placeholder = "Search a place",
    flex_grow = 1,
})

local search_button = lvgl.Button(search_row, {
    w = lvgl.SIZE_CONTENT,
    h = lvgl.theme_lerp(34, 44),
    pad_all = lvgl.theme_lerp(8, 12),
})
lvgl.Label(search_button, { text = "Search", text_font = "small" })

local custom_fields = lvgl.Object(settings_view, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_row = lvgl.theme_lerp(2, 6),
})

local name_row = lvgl.Object(custom_fields, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
})
lvgl.Label(name_row, { text = "Name", text_font = "small" })
local name_field = lvgl.TextArea(name_row, {
    w = lvgl.PCT(55),
    one_line = true,
    text = custom_name,
})

local latitude_row = lvgl.Object(custom_fields, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
})
lvgl.Label(latitude_row, { text = "Latitude", text_font = "small" })
local latitude_field = lvgl.TextArea(latitude_row, {
    w = lvgl.PCT(55),
    one_line = true,
    text = string.format("%.4f", custom_latitude),
})

local longitude_row = lvgl.Object(custom_fields, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
})
lvgl.Label(longitude_row, { text = "Longitude", text_font = "small" })
local longitude_field = lvgl.TextArea(longitude_row, {
    w = lvgl.PCT(55),
    one_line = true,
    text = string.format("%.4f", custom_longitude),
})

-- The keyboard appears only while a coordinate field is being edited.
local keyboard = lvgl.Keyboard(settings_view, {
    w = lvgl.PCT(100),
    h = lvgl.theme_lerp(140, 230),
})
keyboard:add_flag(lvgl.FLAG.HIDDEN)

local units_row = lvgl.Object(settings_view, {
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

local interval_row = lvgl.Object(settings_view, {
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

local apply_row = lvgl.Object(settings_view, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.END, cross = lvgl.FLEX_ALIGN.CENTER },
})
local apply_button = lvgl.Button(apply_row, {
    w = lvgl.SIZE_CONTENT,
    h = lvgl.theme_lerp(34, 44),
    pad_all = lvgl.theme_lerp(8, 14),
})
lvgl.Label(apply_button, { text = "Apply", text_font = "small" })

local settings_status = lvgl.Label(settings_view, {
    text = "",
    text_font = "small",
    text_color = 0xff6b6b,
})

-- Formatting -------------------------------------------------------------------------------

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

-- Settings view behaviour ------------------------------------------------------------------

local function show_custom_fields(visible)
    if visible then
        custom_fields:remove_flag(lvgl.FLAG.HIDDEN)
    else
        custom_fields:add_flag(lvgl.FLAG.HIDDEN)
        keyboard:add_flag(lvgl.FLAG.HIDDEN)
    end
end

local function sync_settings_view()
    city_dropdown:set{ value = using_custom and CUSTOM_OPTION or city_index }
    name_field:set{ text = custom_name }
    latitude_field:set{ text = string.format("%.4f", using_custom and custom_latitude or cities[city_index].latitude) }
    longitude_field:set{ text = string.format("%.4f", using_custom and custom_longitude or cities[city_index].longitude) }
    search_field:set{ text = "" }
    units_dropdown:set{ value = unit == "F" and 2 or 1 }
    interval_dropdown:set{ value = refresh_minutes == 5 and 1 or (refresh_minutes == 30 and 3 or 2) }
    settings_status:set{ text = "" }
    show_custom_fields(using_custom)
end

local settings_visible = false

local function toggle_settings()
    if settings_visible then
        settings_visible = false
        settings_view:add_flag(lvgl.FLAG.HIDDEN)
        main_view:remove_flag(lvgl.FLAG.HIDDEN)
        keyboard:add_flag(lvgl.FLAG.HIDDEN)
    else
        settings_visible = true
        sync_settings_view()
        main_view:add_flag(lvgl.FLAG.HIDDEN)
        settings_view:remove_flag(lvgl.FLAG.HIDDEN)
    end
end

-- Refresh flow -----------------------------------------------------------------------------

local generation = 0

local function refresh()
    generation = generation + 1
    local current_generation = generation
    local location = active_location()
    location_label:set{ text = location.name }

    update_status("Updating " .. location.name .. "...")

    weather.fetch(location, function(data, error)
        if current_generation ~= generation then
            return -- a newer city or refresh won
        end

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

-- Settings controls ------------------------------------------------------------------------

city_dropdown:on(lvgl.EVENT.VALUE_CHANGED, function()
    local selected = city_dropdown:value()
    show_custom_fields(selected == CUSTOM_OPTION)
    if selected ~= CUSTOM_OPTION then
        latitude_field:set{ text = string.format("%.4f", cities[selected].latitude) }
        longitude_field:set{ text = string.format("%.4f", cities[selected].longitude) }
    end
end)

local function bind_keyboard(field)
    return function()
        keyboard:set_textarea(field)
        keyboard:remove_flag(lvgl.FLAG.HIDDEN)
    end
end

search_field:on(lvgl.EVENT.CLICKED, bind_keyboard(search_field))
name_field:on(lvgl.EVENT.CLICKED, bind_keyboard(name_field))
latitude_field:on(lvgl.EVENT.CLICKED, bind_keyboard(latitude_field))
longitude_field:on(lvgl.EVENT.CLICKED, bind_keyboard(longitude_field))
keyboard:on(lvgl.EVENT.READY, function()
    keyboard:set_textarea(nil)
    keyboard:add_flag(lvgl.FLAG.HIDDEN)
end)

search_button:on(lvgl.EVENT.CLICKED, function()
    local query = search_field:text()
    if #query < 2 then
        settings_status:set{ text = "Type at least two characters", text_color = 0xff6b6b }
        return
    end

    settings_status:set{ text = "Searching..." }
    search_button:add_state(lvgl.STATE.DISABLED)

    weather.geocode(query, function(result, error)
        search_button:remove_state(lvgl.STATE.DISABLED)
        if result == nil then
            settings_status:set{ text = tostring(error), text_color = 0xff6b6b }
            return
        end

        custom_name = result.name
        custom_latitude = result.latitude
        custom_longitude = result.longitude

        city_dropdown:set{ value = CUSTOM_OPTION }
        show_custom_fields(true)
        name_field:set{ text = custom_name }
        latitude_field:set{ text = string.format("%.4f", custom_latitude) }
        longitude_field:set{ text = string.format("%.4f", custom_longitude) }
        settings_status:set{ text = "Found " .. custom_name, text_color = lvgl.theme_color_primary() }
    end)
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

apply_button:on(lvgl.EVENT.CLICKED, function()
    local selected = city_dropdown:value()

    if selected == CUSTOM_OPTION then
        local latitude = tonumber(latitude_field:text())
        local longitude = tonumber(longitude_field:text())
        if latitude == nil or latitude < -90 or latitude > 90 then
            settings_status:set{ text = "Latitude must be between -90 and 90", text_color = 0xff6b6b }
            return
        end
        if longitude == nil or longitude < -180 or longitude > 180 then
            settings_status:set{ text = "Longitude must be between -180 and 180", text_color = 0xff6b6b }
            return
        end

        using_custom = true
        custom_latitude = latitude
        custom_longitude = longitude
        local name = name_field:text()
        if name == nil or #name == 0 then
            name = CUSTOM_NAME
        end
        custom_name = name
        tactility.settings.set(SETTINGS_NAMESPACE, "custom", "1")
        tactility.settings.set(SETTINGS_NAMESPACE, "name", custom_name)
        tactility.settings.set(SETTINGS_NAMESPACE, "latitude", string.format("%.6f", latitude))
        tactility.settings.set(SETTINGS_NAMESPACE, "longitude", string.format("%.6f", longitude))
    else
        using_custom = false
        city_index = selected
        tactility.settings.set(SETTINGS_NAMESPACE, "custom", "0")
        tactility.settings.set(SETTINGS_NAMESPACE, "city", tostring(selected))
    end

    toggle_settings()
    refresh()
end)

-- Toolbar actions --------------------------------------------------------------------------

local toolbar = tactility.ui and tactility.ui.toolbar()
if toolbar then
    lvgl.toolbar.add_icon_action(toolbar, lvgl.ICON.REFRESH, refresh)
    lvgl.toolbar.add_icon_action(toolbar, lvgl.ICON.SETTINGS, toggle_settings)
end

-- Kept in a global so the timer is not collected while the app is open.
__weather_refresh_timer = lvgl.Timer{
    period = refresh_minutes * 60 * 1000,
    callback = refresh,
}

refresh()
