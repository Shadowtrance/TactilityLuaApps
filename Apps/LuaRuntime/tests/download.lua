-- HTTPS download to a file, with progress.
--
-- The callback logs its own SWEEPRESULT because the transfer can outlive the
-- harness timer. Cancellation is documented but not automated: the client can
-- only cancel between I/O steps, so cancelling during a TLS connect takes the
-- connect timeout.

local install_directory = ...
local path = install_directory .. "/download-test.json"
local progress_events = 0

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
result = "download started"
