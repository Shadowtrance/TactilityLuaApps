-- HTTPS + JSON through a real API. Needs network access and a set clock.
--
-- The harness reports at 1.8 s, which is usually before TLS finishes, so the
-- callback logs its own SWEEPRESULT; the runner keeps the last one.

local handle = tactility.http.request({
    url = "https://api.open-meteo.com/v1/forecast?latitude=52.52&longitude=13.41&current=temperature_2m",
    timeout_ms = 20000,
    max_bytes = 32768,
    headers = { Accept = "application/json" },
}, function(response, reason)
    if response == nil then
        result = "http FAIL: " .. tostring(reason)
    elseif response.status ~= 200 then
        result = "http FAIL: status " .. tostring(response.status)
    else
        local data, error = tactility.json.decode(response.body)
        if data == nil then
            result = "http FAIL: " .. tostring(error)
        elseif type(data.current) ~= "table" or type(data.current.temperature_2m) ~= "number" then
            result = "http FAIL: unexpected payload"
        else
            result = string.format(
                "http PASS: %d, %.1f C, %d bytes",
                response.status,
                data.current.temperature_2m,
                response.length
            )
        end
    end

    tactility.log.info("SWEEPRESULT", result)
end)

assert(handle ~= nil, "http.request did not return a handle")
result = "http started"
