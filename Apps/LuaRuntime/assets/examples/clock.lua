-- A second script, to prove one launcher entry per file.

local now = tactility.time.now()
local clock

if now then
    local t = tactility.time.date(now)
    clock = string.format("%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.min, t.sec)
else
    clock = "clock not set"
end

result = string.format(
    "Clock\n\n%s\nuptime: %d s\ndevices: %d",
    clock,
    math.floor(tactility.time.uptime() / 1000),
    #tactility.device.list()
)

tactility.log.info("luaproof", "clock script ran")
