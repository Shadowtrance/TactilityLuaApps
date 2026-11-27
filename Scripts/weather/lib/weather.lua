-- Open-Meteo client: no API key, JSON in, Lua tables out.
local M = {}

local CONDITIONS = {
    [0] = "Clear sky",
    [1] = "Mainly clear",
    [2] = "Partly cloudy",
    [3] = "Overcast",
    [45] = "Fog",
    [48] = "Rime fog",
    [51] = "Light drizzle",
    [53] = "Drizzle",
    [55] = "Heavy drizzle",
    [56] = "Freezing drizzle",
    [57] = "Freezing drizzle",
    [61] = "Light rain",
    [63] = "Rain",
    [65] = "Heavy rain",
    [66] = "Freezing rain",
    [67] = "Freezing rain",
    [71] = "Light snow",
    [73] = "Snow",
    [75] = "Heavy snow",
    [77] = "Snow grains",
    [80] = "Rain showers",
    [81] = "Rain showers",
    [82] = "Heavy showers",
    [85] = "Snow showers",
    [86] = "Snow showers",
    [95] = "Thunderstorm",
    [96] = "Thunder and hail",
    [99] = "Thunder and hail",
}

function M.condition(code)
    return CONDITIONS[code] or ("Weather code " .. tostring(code))
end

local function url_for(city)
    return string.format(
        "https://api.open-meteo.com/v1/forecast" ..
        "?latitude=%.4f&longitude=%.4f" ..
        "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,weather_code,wind_speed_10m" ..
        "&daily=weather_code,temperature_2m_max,temperature_2m_min" ..
        "&timezone=auto&forecast_days=5",
        city.latitude,
        city.longitude
    )
end

-- Fetches the forecast. The callback receives the decoded table, or nil and a
-- reason. It runs on the app loop with the LVGL lock held, so it may update
-- widgets directly.
function M.fetch(city, callback)
    local handle = tactility.http.request({
        url = url_for(city),
        timeout_ms = 20000,
        max_bytes = 65536,
        headers = { Accept = "application/json" },
    }, function(response, reason)
        if response == nil then
            callback(nil, reason)
            return
        end
        if response.status ~= 200 then
            callback(nil, "HTTP " .. tostring(response.status))
            return
        end

        local data, error = tactility.json.decode(response.body)
        if data == nil then
            callback(nil, error)
            return
        end
        if type(data.current) ~= "table" or type(data.daily) ~= "table" then
            callback(nil, "the response has no forecast")
            return
        end
        callback(data)
    end)

    if handle == nil then
        callback(nil, "could not start the request")
    end
    return handle
end

return M
