-- Cross-binding smoke test: fs, time, device, settings, power and wifi.
--
-- This is the first slice of the retired Tests/lua-module suite, run on-device.

local mounts = tactility.fs.mounts()
assert(type(mounts) == "table" and #mounts > 0, "fs.mounts")

-- Listing our own scripts folder made this depend on the device already having scripts.
-- A mount root always has entries, so a fresh device exercises the same API.
local entries = tactility.fs.list(mounts[1])
assert(type(entries) == "table" and #entries > 0, "fs.list")

local uptime = tactility.time.uptime()
assert(type(uptime) == "number" and uptime > 0, "time.uptime")

local now = tactility.time.now()
assert(now == nil or type(now) == "number", "time.now")
if now then
    local date = tactility.time.date(now)
    assert(type(date) == "table" and type(date.year) == "number", "time.date")
end

local devices = tactility.device.list()
assert(type(devices) == "table" and #devices > 0, "device.list")
local first = tactility.device.find(devices[1])
assert(first ~= nil, "device.find")
assert(first:name() == devices[1], "device:name")
first:release()

tactility.settings.set("lua.test", "key", "value")
assert(tactility.settings.get("lua.test", "key") == "value", "settings round trip")
assert(tactility.settings.get("lua.test", "missing", "fallback") == "fallback", "settings default")

if tactility.power.is_available() then
    local capacity = tactility.power.capacity()
    assert(capacity == nil or type(capacity) == "number", "power.capacity")
end

if tactility.wifi.is_available() then
    assert(type(tactility.wifi.station_state()) == "string", "wifi.station_state")
end

result = string.format(
    "bindings PASS: %d mounts, %d entries, %d devices, uptime %ds",
    #mounts,
    #entries,
    #devices,
    math.floor(uptime / 1000)
)
