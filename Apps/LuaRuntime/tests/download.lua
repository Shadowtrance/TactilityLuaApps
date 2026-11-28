-- HTTPS download to a file, with progress and cancellation.
--
-- The first download is cancelled immediately; its callback then starts the real
-- one, so both paths run in sequence without two large transfers at once. The
-- callback logs its own SWEEPRESULT because the transfer can outlive the harness
-- timer.

local install_directory = ...
local path = install_directory .. "/download-test.json"
local cancel_path = install_directory .. "/download-cancel.json"
local progress_events = 0

local function start_real()
    local handle = tactility.http.download({
        url = "https://api.open-meteo.com/v1/forecast?latitude=52.52&longitude=13.41"
            .. "&current=temperature_2m&hourly=temperature_2m",
        path = path,
        progress = function(received, total)
            progress_events = progress_events + 1
            assert(type(received) == "number" and received >= 0, "progress received")
            assert(type(total) == "number" and total >= 0, "progress total")
        end,
    }, function(response, reason)
        local outcome
        if response == nil then
            outcome = "download FAIL: " .. tostring(reason)
        elseif response.status ~= 200 then
            outcome = "download FAIL: status " .. tostring(response.status)
        else
            local text, read_error = tactility.fs.read(path)
            local data = text and tactility.json.decode(text)
            if text == nil then
                outcome = "download FAIL: " .. tostring(read_error)
            elseif data == nil or type(data.current) ~= "table" then
                outcome = "download FAIL: unexpected payload"
            elseif response.length ~= #text then
                outcome = string.format(
                    "download FAIL: length %d, file %d",
                    response.length,
                    #text
                )
            else
                outcome = string.format(
                    "download PASS: %d, %d bytes, %d progress events",
                    response.status,
                    response.length,
                    progress_events
                )
            end
        end
        result = outcome
        tactility.log.info("SWEEPRESULT", outcome)
    end)

    assert(handle ~= nil, "download did not return a handle")
end

local cancel_handle = tactility.http.download({
    url = "https://api.open-meteo.com/v1/forecast?latitude=52.52&longitude=13.41&current=temperature_2m",
    path = cancel_path,
}, function(response, reason)
    if response == nil and reason == "cancelled" then
        start_real()
    else
        local outcome = "download FAIL: cancel -> " .. tostring(reason)
        result = outcome
        tactility.log.info("SWEEPRESULT", outcome)
    end
end)

assert(cancel_handle ~= nil, "cancel download did not return a handle")
cancel_handle:cancel()
result = "download started"
