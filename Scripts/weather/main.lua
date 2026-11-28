-- @version 0.2.0
-- @code 2
--
-- Weather with a paged, icon-led layout: a big "now" page, a forecast page with
-- a temperature chart, and the settings view behind the toolbar gear. The icons
-- are PNG assets bundled with the package, so they are not limited to the shared
-- icon font. The location comes from the built-in place search and is saved.

local weather = require("lib.weather")

local SETTINGS_NAMESPACE = "weather"
local WEEKDAYS = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }
local DEFAULT_NAME = "Amsterdam"
local DEFAULT_LATITUDE = 52.3676
local DEFAULT_LONGITUDE = 4.9041
local script_directory = ... or "."

-- Settings ---------------------------------------------------------------------------------

local saved_unit = tactility.settings.get(SETTINGS_NAMESPACE, "unit", "C")
local unit = saved_unit == "F" and "F" or "C"

local refresh_minutes = tonumber(tactility.settings.get(SETTINGS_NAMESPACE, "refresh", "10")) or 10
if refresh_minutes ~= 5 and refresh_minutes ~= 10 and refresh_minutes ~= 30 then
    refresh_minutes = 10
end

local custom_name = tactility.settings.get(SETTINGS_NAMESPACE, "name", DEFAULT_NAME)
local custom_latitude = tonumber(tactility.settings.get(SETTINGS_NAMESPACE, "latitude", tostring(DEFAULT_LATITUDE))) or DEFAULT_LATITUDE
local custom_longitude = tonumber(tactility.settings.get(SETTINGS_NAMESPACE, "longitude", tostring(DEFAULT_LONGITUDE))) or DEFAULT_LONGITUDE
if custom_name == nil or #custom_name == 0 then
    custom_name = DEFAULT_NAME
end

local last_data = nil

local function active_location()
    return { name = custom_name, latitude = custom_latitude, longitude = custom_longitude }
end

-- Appearance -------------------------------------------------------------------------------

local display_width = tactility.display.size() or 320
local wide = display_width >= 600
local pad = wide and 14 or 8
local gap = wide and 10 or 6
-- PNG decoding is not cached by the firmware, so keep the decoded area small:
-- every scroll frame decodes these again.
local hero_icon_size = wide and 96 or 64
local forecast_icon_size = wide and 32 or 26
local metric_icon_size = 20
local chart_height = wide and 170 or 84
local chart_points = wide and 24 or 12
local forecast_days = 5

local asset_cache = {}

local function asset_path(relative)
    local cached = asset_cache[relative]
    if cached ~= nil then
        return cached
    end

    -- `...` is the package root for an installed app (assets live in lua/assets/)
    -- and the script's folder when the script runs from a directory (assets/).
    local packaged = script_directory .. "/lua/assets/" .. relative
    local uninstalled = script_directory .. "/assets/" .. relative
    local path = tactility.fs.exists(packaged) and packaged or uninstalled
    asset_cache[relative] = path
    return path
end

local function icon_source(base, size)
    return asset_path("weather/" .. base .. "-" .. size .. ".png")
end

-- Temperatures are drawn with digit images instead of a scaled label: LVGL's
-- transform scale renders through an offscreen layer, which costs memory and
-- draws outside the label's layout box, so a label cannot carry a big number.
local temp_font_size = wide and 64 or 40

local function digit_source(name)
    return asset_path("temp/" .. name .. "-" .. temp_font_size .. ".png")
end

local function weather_icon(code, is_day)
    code = tonumber(code) or 0
    local day = is_day ~= 0
    if code == 0 then
        return day and "sun" or "moon"
    end
    if code <= 2 then
        return day and "partly_cloudy" or "partly_cloudy_night"
    end
    if code == 3 then
        return "cloud"
    end
    if code == 45 or code == 48 then
        return "fog"
    end
    if code >= 51 and code <= 57 then
        return "drizzle"
    end
    if code >= 61 and code <= 67 then
        return "rain"
    end
    if code >= 71 and code <= 77 then
        return "snow"
    end
    if code >= 80 and code <= 82 then
        return "rain"
    end
    if code >= 85 and code <= 86 then
        return "snow"
    end
    if code >= 95 then
        return "thunder"
    end
    return "cloud"
end

-- Views ------------------------------------------------------------------------------------

local root = lvgl.root()
root:set{
    w = lvgl.PCT(100),
    h = lvgl.PCT(100),
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_all = pad,
    pad_row = 0,
}

local main_view = lvgl.Object(root, {
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    w = lvgl.PCT(100),
    h = lvgl.PCT(100),
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_row = gap,
})

local settings_view = lvgl.Object(root, {
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    pad_all = 0,
    w = lvgl.PCT(100),
    h = lvgl.PCT(100),
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_row = gap,
})
settings_view:add_flag(lvgl.FLAG.HIDDEN)
settings_view:add_flag(lvgl.FLAG.SCROLLABLE)

-- Pages ------------------------------------------------------------------------------------

local tabs = lvgl.Tabs(main_view, {
    w = lvgl.PCT(100),
    mode = lvgl.TABS_MODE.FIXED,
})
tabs:add_tab("Now")
tabs:add_tab("Forecast")

local now_tile = lvgl.Object(main_view, {
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    w = lvgl.PCT(100),
    flex_grow = 1,
    pad_all = 0,
    pad_row = gap,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    scroll_dir = lvgl.DIR.VER,
    scrollbar_mode = lvgl.SCROLLBAR.OFF,
})
now_tile:add_flag(lvgl.FLAG.SCROLLABLE)
local forecast_tile = lvgl.Object(main_view, {
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    w = lvgl.PCT(100),
    flex_grow = 1,
    pad_all = 0,
    pad_row = gap,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    scroll_dir = lvgl.DIR.VER,
    scrollbar_mode = lvgl.SCROLLBAR.OFF,
})
forecast_tile:add_flag(lvgl.FLAG.SCROLLABLE)
forecast_tile:add_flag(lvgl.FLAG.HIDDEN)

tabs:on(lvgl.EVENT.VALUE_CHANGED, function()
    local selected = tabs:selected()
    if selected == 1 then
        now_tile:remove_flag(lvgl.FLAG.HIDDEN)
        forecast_tile:add_flag(lvgl.FLAG.HIDDEN)
    else
        now_tile:add_flag(lvgl.FLAG.HIDDEN)
        forecast_tile:remove_flag(lvgl.FLAG.HIDDEN)
    end
end)

-- Now page ---------------------------------------------------------------------------------

local hero = lvgl.Card(now_tile, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    pad_all = wide and 14 or 6,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_row = wide and 10 or 4,
    scrollbar_mode = lvgl.SCROLLBAR.OFF,
})

local hero_top = lvgl.Object(hero, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    pad_all = 0,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
    pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    scrollbar_mode = lvgl.SCROLLBAR.OFF,
})

local city_label = lvgl.Label(hero_top, { text = "--", text_font = "large" })

local update_row = lvgl.Object(hero_top, {
    h = lvgl.SIZE_CONTENT,
    flex_grow = 1,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.END, cross = lvgl.FLEX_ALIGN.CENTER },
    pad_column = 6,
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    pad_all = 0,
    scrollbar_mode = lvgl.SCROLLBAR.OFF,
})

local spinner = lvgl.Spinner(update_row, {
    w = wide and 24 or 16,
    h = wide and 24 or 16,
})
spinner:add_flag(lvgl.FLAG.HIDDEN)

local updated_label = lvgl.Label(update_row, {
    text = "",
    text_font = "small",
    text_color = lvgl.theme_color_secondary(),
    max_width = wide and 360 or 150,
    long_mode = lvgl.LONG.DOTS,
    text_align = lvgl.TEXT_ALIGN.RIGHT,
})

local hero_row = lvgl.Object(hero, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    pad_all = 0,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.CENTER, cross = lvgl.FLEX_ALIGN.CENTER },
    pad_column = wide and 32 or 10,
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    scrollbar_mode = lvgl.SCROLLBAR.OFF,
})

local current_icon = lvgl.Image(hero_row, {
    w = hero_icon_size,
    h = hero_icon_size,
})

local temperature_row = lvgl.Object(hero_row, {
    h = lvgl.SIZE_CONTENT,
    flex_grow = 1,
    pad_all = 0,
    pad_column = 1,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.END, cross = lvgl.FLEX_ALIGN.CENTER },
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    scrollbar_mode = lvgl.SCROLLBAR.OFF,
})

local sign_slot = lvgl.Image(temperature_row, { src = digit_source("minus") })
local tens_slot = lvgl.Image(temperature_row, { src = digit_source("0") })
local ones_slot = lvgl.Image(temperature_row, { src = digit_source("0") })
local degree_slot = lvgl.Image(temperature_row, { src = digit_source("degree") })
sign_slot:add_flag(lvgl.FLAG.HIDDEN)
tens_slot:add_flag(lvgl.FLAG.HIDDEN)

-- Shows a temperature like "-7°" by sourcing one digit image per character.
local function show_temperature(text)
    local negative = text:sub(1, 1) == "-"
    local digits = (negative and text:sub(2) or text):match("^(%d+)") or "0"

    if negative then
        sign_slot:set{ src = digit_source("minus") }
        sign_slot:remove_flag(lvgl.FLAG.HIDDEN)
    else
        sign_slot:add_flag(lvgl.FLAG.HIDDEN)
    end

    if #digits > 1 then
        tens_slot:set{ src = digit_source(digits:sub(1, 1)) }
        tens_slot:remove_flag(lvgl.FLAG.HIDDEN)
    else
        tens_slot:add_flag(lvgl.FLAG.HIDDEN)
    end

    ones_slot:set{ src = digit_source(digits:sub(-1)) }
    degree_slot:set{ src = digit_source("degree") }
end

local condition_row = lvgl.Object(hero, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    pad_all = 0,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
    pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    scrollbar_mode = lvgl.SCROLLBAR.OFF,
})

local condition_label = lvgl.Label(condition_row, {
    text = "Waiting for data",
    text_font = "default",
    long_mode = lvgl.LONG.WRAP,
    flex_grow = 1,
})

local feels_label = lvgl.Label(condition_row, {
    text = "",
    text_font = "small",
    text_color = lvgl.theme_color_secondary(),
    long_mode = lvgl.LONG.DOTS,
})

local details_row = lvgl.Object(hero, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    pad_all = 0,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
    pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    scrollbar_mode = lvgl.SCROLLBAR.OFF,
})

local high_label = lvgl.Label(details_row, { text = "", text_font = "small" })
local low_label = lvgl.Label(details_row, { text = "", text_font = "small" })

local metrics_card = lvgl.Card(now_tile, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    pad_all = wide and 10 or 8,
    pad_row = wide and 6 or 4,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    scrollbar_mode = lvgl.SCROLLBAR.OFF,
})

local function make_metric_row(parent, icon, caption)
    local row = lvgl.Object(parent, {
        w = lvgl.PCT(100),
        h = lvgl.SIZE_CONTENT,
        pad_all = 0,
        flex_flow = lvgl.FLEX_FLOW.ROW,
        flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER },
        pad_column = wide and 10 or 6,
        bg_opa = lvgl.OPA.TRANSP,
        border_width = 0,
        min_height = 0,
        scrollbar_mode = lvgl.SCROLLBAR.OFF,
    })
    lvgl.Image(row, { w = metric_icon_size, h = metric_icon_size, src = icon })
    lvgl.Label(row, { text = caption, text_font = "small", flex_grow = 1 })
    local value = lvgl.Label(row, {
        text = "--",
        text_font = wide and "default" or "small",
        text_color = lvgl.theme_color_primary(),
    })
    return { value = value }
end

local humidity_metric = make_metric_row(metrics_card, icon_source("humidity", metric_icon_size), "Humidity")
local wind_metric = make_metric_row(metrics_card, icon_source("wind", metric_icon_size), "Wind")
local precipitation_metric = make_metric_row(
    metrics_card,
    icon_source("precipitation", metric_icon_size),
    "Precipitation"
)

-- Forecast page ----------------------------------------------------------------------------

local forecast_title = lvgl.Label(forecast_tile, {
    text = forecast_days .. "-day forecast",
    text_font = "small",
    text_color = lvgl.theme_color_secondary(),
})

local chart_card = lvgl.Card(forecast_tile, {
    w = lvgl.PCT(100),
    h = chart_height,
    pad_all = wide and 14 or 6,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_row = 2,
})
chart_card:add_flag(lvgl.FLAG.HIDDEN)

local chart_header = lvgl.Object(chart_card, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    pad_all = 0,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
})

local chart_title = lvgl.Label(chart_header, {
    text = "Temperature, next " .. chart_points .. " hours",
    text_font = "small",
    text_color = lvgl.theme_color_secondary(),
})

local chart_range = lvgl.Label(chart_header, {
    text = "",
    text_font = "small",
    text_color = lvgl.theme_color_primary(),
})

local chart = lvgl.Chart(chart_card, {
    w = lvgl.PCT(100),
    flex_grow = 1,
    chart_type = lvgl.CHART_TYPE.LINE,
    point_count = chart_points,
    div_lines = { h = 2, v = 3 },
    axis_range = { min = 0, max = 1 },
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    scrollbar_mode = lvgl.SCROLLBAR.OFF,
})
local chart_series = chart:add_series(lvgl.theme_color_primary())

local day_rows = {}
for index = 1, forecast_days do
    local row = lvgl.Card(forecast_tile, {
        w = lvgl.PCT(100),
        h = lvgl.SIZE_CONTENT,
        min_height = 0,
        pad_all = wide and 10 or 5,
        flex_flow = lvgl.FLEX_FLOW.ROW,
        flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER },
        pad_column = gap,
        scrollbar_mode = lvgl.SCROLLBAR.OFF,
    })
    local day = lvgl.Label(row, { text = "", text_font = "small", w = wide and 100 or 44 })
    local icon = lvgl.Image(row, { w = forecast_icon_size, h = forecast_icon_size })
    local condition = lvgl.Label(row, {
        text = "",
        text_font = "small",
        flex_grow = 1,
        max_width = wide and 500 or 140,
        long_mode = lvgl.LONG.CLIP,
    })
    -- The temperatures are pinned to the right edge instead of relying on the
    -- flex row to push them there, which left a gap on some themes.
    local temperatures = lvgl.Object(row, {
        bg_opa = lvgl.OPA.TRANSP,
        border_width = 0,
        min_height = 0,
        pad_all = 0,
        w = lvgl.SIZE_CONTENT,
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        flex_align = { main = lvgl.FLEX_ALIGN.CENTER, cross = lvgl.FLEX_ALIGN.END },
        pad_row = 0,
        align = lvgl.ALIGN.RIGHT_MID,
    })
    temperatures:add_flag(lvgl.FLAG.IGNORE_LAYOUT)
    local high = lvgl.Label(temperatures, { text = "", text_font = "small", text_color = lvgl.theme_color_primary() })
    local low = lvgl.Label(temperatures, { text = "", text_font = "small", text_color = lvgl.theme_color_secondary() })
    day_rows[index] = { card = row, day = day, icon = icon, condition = condition, high = high, low = low }
end

-- Settings view ----------------------------------------------------------------------------

local settings_card = lvgl.Card(settings_view, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    pad_all = pad,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_row = gap,
})

local location_title = lvgl.Label(settings_card, {
    text = "Location",
    text_font = "small",
    text_color = lvgl.theme_color_primary(),
})

local field_label_width = wide and 150 or 82

local search_row = lvgl.Object(settings_card, {
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    pad_all = 0,
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.SPACE_BETWEEN, cross = lvgl.FLEX_ALIGN.CENTER },
    pad_column = gap,
})

local search_field = lvgl.TextArea(search_row, {
    one_line = true,
    placeholder = "Search a place",
    flex_grow = 1,
})

local search_button = lvgl.Button(search_row, {
    w = lvgl.SIZE_CONTENT,
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.CENTER, cross = lvgl.FLEX_ALIGN.CENTER },
})
lvgl.Label(search_button, { text = "Search" })

local custom_fields = lvgl.Object(settings_card, {
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    pad_all = 0,
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
    pad_row = wide and 8 or 4,
})

local function make_field_row(caption, text)
    local row = lvgl.Object(custom_fields, {
        bg_opa = lvgl.OPA.TRANSP,
        border_width = 0,
        min_height = 0,
        pad_all = 0,
        w = lvgl.PCT(100),
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.ROW,
        flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER },
        pad_column = gap,
    })
    lvgl.Label(row, { text = caption, text_font = "small", w = field_label_width })
    return lvgl.TextArea(row, { flex_grow = 1, one_line = true, text = text })
end

local name_field = make_field_row("Name", custom_name)
local latitude_field = make_field_row("Latitude", string.format("%.4f", custom_latitude))
local longitude_field = make_field_row("Longitude", string.format("%.4f", custom_longitude))

local display_title = lvgl.Label(settings_card, {
    text = "Units and refresh",
    text_font = "small",
    text_color = lvgl.theme_color_primary(),
})

local units_row = lvgl.Object(settings_card, {
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    pad_all = 0,
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER },
    pad_column = gap,
})
lvgl.Label(units_row, { text = "Units", text_font = "small", w = field_label_width })
local units_dropdown = lvgl.Dropdown(units_row, {
    options = { "Celsius", "Fahrenheit" },
    flex_grow = 1,
})
units_dropdown:set{ value = unit == "F" and 2 or 1 }

local interval_row = lvgl.Object(settings_card, {
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    pad_all = 0,
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER },
    pad_column = gap,
})
lvgl.Label(interval_row, { text = "Refresh every", text_font = "small", w = field_label_width })
local interval_dropdown = lvgl.Dropdown(interval_row, {
    options = { "5 minutes", "10 minutes", "30 minutes" },
    flex_grow = 1,
})
interval_dropdown:set{ value = refresh_minutes == 5 and 1 or (refresh_minutes == 30 and 3 or 2) }

local apply_row = lvgl.Object(settings_card, {
    bg_opa = lvgl.OPA.TRANSP,
    border_width = 0,
    min_height = 0,
    pad_all = 0,
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.END, cross = lvgl.FLEX_ALIGN.CENTER },
})
local apply_button = lvgl.Button(apply_row, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.CENTER, cross = lvgl.FLEX_ALIGN.CENTER },
})
lvgl.Label(apply_button, { text = "Apply" })

local settings_status = lvgl.Label(settings_card, {
    text = "",
    text_font = "small",
    text_color = 0xff6b6b,
    w = lvgl.PCT(100),
    long_mode = lvgl.LONG.WRAP,
})

-- The on-screen keyboard is only for touch-only devices; a hardware keyboard makes
-- it redundant (and on a small screen it would cover the fields).
local hardware_keyboard = lvgl.hardware_keyboard_available()
local keyboard = nil
if not hardware_keyboard then
    keyboard = lvgl.Keyboard(settings_view, {
        w = lvgl.PCT(100),
        h = lvgl.theme_lerp(140, 230),
    })
    keyboard:add_flag(lvgl.FLAG.HIDDEN)
end

-- Formatting -------------------------------------------------------------------------------

local function round(value)
    return math.floor((tonumber(value) or 0) + 0.5)
end

local function convert_temperature(celsius)
    local value = tonumber(celsius) or 0
    if unit == "F" then
        value = value * 9 / 5 + 32
    end
    return value
end

local function format_temperature(celsius)
    return string.format("%d°", round(convert_temperature(celsius)))
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

local function set_updated(text, color)
    updated_label:set{ text = text, text_color = color or lvgl.theme_color_secondary() }
end

local function set_chart(data)
    local hourly = data.hourly
    if not hourly or not hourly.time or not hourly.temperature_2m then
        chart_card:add_flag(lvgl.FLAG.HIDDEN)
        return
    end

    local current_hour = (data.current and data.current.time or ""):sub(1, 13)
    local start = 1
    for index, timestamp in ipairs(hourly.time) do
        if timestamp:sub(1, 13) >= current_hour then
            start = index
            break
        end
    end

    local values = {}
    local minimum = nil
    local maximum = nil
    local last = math.min(#hourly.temperature_2m, start + chart_points - 1)
    for index = start, last do
        local value = tonumber(hourly.temperature_2m[index])
        if value ~= nil then
            value = convert_temperature(value)
            values[#values + 1] = round(value)
            minimum = minimum == nil and value or math.min(minimum, value)
            maximum = maximum == nil and value or math.max(maximum, value)
        end
    end

    if #values < 2 or minimum == nil then
        chart_card:add_flag(lvgl.FLAG.HIDDEN)
        return
    end

    local low = math.floor(minimum - 2)
    local high = math.ceil(maximum + 2)
    if high - low < 4 then
        high = low + 4
    end
    chart:set{ axis_range = { min = low, max = high } }
    chart:set_values(chart_series, values)
    chart:refresh()
    chart_range:set{
        text = string.format(
            "Low %s   High %s",
            format_temperature(minimum),
            format_temperature(maximum)
        ),
    }
    chart_card:remove_flag(lvgl.FLAG.HIDDEN)
end

local function render(data)
    local current = data.current
    local today_max = data.daily and data.daily.temperature_2m_max and data.daily.temperature_2m_max[1]
    local today_min = data.daily and data.daily.temperature_2m_min and data.daily.temperature_2m_min[1]
    local wind, wind_unit = format_wind(current.wind_speed_10m)

    city_label:set{ text = active_location().name }
    show_temperature(format_temperature(current.temperature_2m))
    condition_label:set{ text = weather.condition(current.weather_code) }
    current_icon:set{ src = icon_source(weather_icon(current.weather_code, current.is_day), hero_icon_size) }

    feels_label:set{ text = "Feels like " .. format_temperature(current.apparent_temperature) }
    if today_max ~= nil then
        high_label:set{ text = "High " .. format_temperature(today_max) }
    else
        high_label:set{ text = "" }
    end
    if today_min ~= nil then
        low_label:set{ text = "Low " .. format_temperature(today_min) }
    else
        low_label:set{ text = "" }
    end

    humidity_metric.value:set{ text = string.format("%d%%", round(current.relative_humidity_2m)) }
    wind_metric.value:set{ text = string.format("%d %s", wind, wind_unit) }
    precipitation_metric.value:set{ text = string.format("%.1f mm", tonumber(current.precipitation) or 0) }

    for index = 1, forecast_days do
        local row = day_rows[index]
        local time = data.daily.time and data.daily.time[index]
        local code = data.daily.weather_code and data.daily.weather_code[index]
        local maximum = data.daily.temperature_2m_max and data.daily.temperature_2m_max[index]
        local minimum = data.daily.temperature_2m_min and data.daily.temperature_2m_min[index]

        if time and code and maximum and minimum then
            row.day:set{ text = index == 1 and "Today" or weekday(time) }
            row.icon:set{ src = icon_source(weather_icon(code, 1), forecast_icon_size) }
            row.condition:set{ text = weather.condition(code) }
            row.high:set{ text = format_temperature(maximum) }
            row.low:set{ text = format_temperature(minimum) }
        else
            row.day:set{ text = "—" }
            row.condition:set{ text = "" }
            row.high:set{ text = "" }
            row.low:set{ text = "" }
        end
    end

    set_chart(data)
end

-- Settings view behaviour ------------------------------------------------------------------

local function sync_settings_view()
    name_field:set{ text = custom_name }
    latitude_field:set{ text = string.format("%.4f", custom_latitude) }
    longitude_field:set{ text = string.format("%.4f", custom_longitude) }
    search_field:set{ text = "" }
    units_dropdown:set{ value = unit == "F" and 2 or 1 }
    interval_dropdown:set{ value = refresh_minutes == 5 and 1 or (refresh_minutes == 30 and 3 or 2) }
    settings_status:set{ text = "" }
end

local settings_visible = false

local function toggle_settings()
    if settings_visible then
        settings_visible = false
        settings_view:add_flag(lvgl.FLAG.HIDDEN)
        main_view:remove_flag(lvgl.FLAG.HIDDEN)
        if keyboard then
            keyboard:add_flag(lvgl.FLAG.HIDDEN)
        end
    else
        settings_visible = true
        sync_settings_view()
        main_view:add_flag(lvgl.FLAG.HIDDEN)
        settings_view:remove_flag(lvgl.FLAG.HIDDEN)
    end
end

local function show_page(index)
    if settings_visible then
        toggle_settings()
    end
    if index == 0 then
        now_tile:remove_flag(lvgl.FLAG.HIDDEN)
        forecast_tile:add_flag(lvgl.FLAG.HIDDEN)
    else
        now_tile:add_flag(lvgl.FLAG.HIDDEN)
        forecast_tile:remove_flag(lvgl.FLAG.HIDDEN)
    end
    tabs:set_selected(index + 1)
end

-- Refresh flow -----------------------------------------------------------------------------

local generation = 0

local function refresh()
    generation = generation + 1
    local current_generation = generation
    local location = active_location()
    city_label:set{ text = location.name }
    set_updated("Updating...")
    spinner:remove_flag(lvgl.FLAG.HIDDEN)

    weather.fetch(location, function(data, error)
        if current_generation ~= generation then
            return
        end

        spinner:add_flag(lvgl.FLAG.HIDDEN)
        if data == nil then
            set_updated("Offline: " .. tostring(error), 0xff6b6b)
            return
        end

        last_data = data
        render(data)
        local now = tactility.time.now()
        if now then
            local broken = tactility.time.date(now)
            set_updated(string.format("Updated %02d:%02d", broken.hour, broken.min))
        else
            set_updated("Updated just now")
        end
    end)
end

-- Settings controls ------------------------------------------------------------------------

local function bind_keyboard(field)
    return function()
        if keyboard then
            keyboard:set_textarea(field)
            keyboard:remove_flag(lvgl.FLAG.HIDDEN)
        end
    end
end

search_field:on(lvgl.EVENT.CLICKED, bind_keyboard(search_field))
name_field:on(lvgl.EVENT.CLICKED, bind_keyboard(name_field))
latitude_field:on(lvgl.EVENT.CLICKED, bind_keyboard(latitude_field))
longitude_field:on(lvgl.EVENT.CLICKED, bind_keyboard(longitude_field))
if keyboard then
    keyboard:on(lvgl.EVENT.READY, function()
        keyboard:set_textarea(nil)
        keyboard:add_flag(lvgl.FLAG.HIDDEN)
    end)
end

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

    custom_latitude = latitude
    custom_longitude = longitude
    local name = name_field:text()
    if name == nil or #name == 0 then
        name = DEFAULT_NAME
    end
    custom_name = name
    tactility.settings.set(SETTINGS_NAMESPACE, "name", custom_name)
    tactility.settings.set(SETTINGS_NAMESPACE, "latitude", string.format("%.6f", latitude))
    tactility.settings.set(SETTINGS_NAMESPACE, "longitude", string.format("%.6f", longitude))

    toggle_settings()
    show_page(0)
    refresh()
end)

-- Keyboard and trackball navigation ---------------------------------------------------------

-- The app group already contains the toolbar and the tabs. Add the cards so the
-- focus can leave the tab bar and the page scrolls to the focused card.
local app_group = lvgl.app_group and lvgl.app_group()
if app_group then
    local function make_focusable(object)
        object:add_flag(lvgl.FLAG.CLICKABLE)
        object:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
        object:add_flag(lvgl.FLAG.SCROLL_ON_FOCUS)
        app_group:add(object)
    end

    make_focusable(hero)
    make_focusable(metrics_card)
    for _, row in ipairs(day_rows) do
        make_focusable(row.card)
    end
end

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


