-- Proof step 3: reach the kernel from a script carried by an external module.

local devices = tactility.device.list()
local device_count = devices and #devices or 0

local battery = "n/a"
if tactility.power.is_available() then
    local pct = tactility.power.capacity()
    if pct then
        battery = pct .. "%"
    end
end

local mounts = tactility.fs.mounts() or {}
local uptime = math.floor(tactility.time.uptime() / 1000)

result = string.format(
    "Lua %s\n\ndevices: %d\nbattery: %s\nmounts: %s\nuptime: %d s",
    _VERSION,
    device_count,
    battery,
    table.concat(mounts, " "),
    uptime
)

tactility.log.info("luaproof", "script ran")
