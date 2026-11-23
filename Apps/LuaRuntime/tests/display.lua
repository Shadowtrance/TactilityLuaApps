-- Display capability: panel geometry, shape and backlight control.
local checks = 0

local function check(condition, message)
    checks = checks + 1
    if not condition then
        error("display: " .. (message or ("check " .. checks .. " failed")), 2)
    end
end

local display = tactility.display
check(display ~= nil, "module")
check(display.is_available(), "display available")

local name = display.name()
check(type(name) == "string" and #name > 0, "name")

local width, height = display.size()
check(type(width) == "number" and width > 0, "width")
check(type(height) == "number" and height > 0, "height")

local shape = display.shape()
check(type(shape) == "table", "shape table")
check(
    shape.shape == "rectangle" or shape.shape == "rounded" or shape.shape == "circle",
    "shape value"
)
check(type(shape.radius) == "number" and shape.radius >= 0, "shape radius")

if display.has_backlight() then
    local minimum, maximum = display.brightness_range()
    check(type(minimum) == "number" and type(maximum) == "number", "brightness range")
    check(minimum <= maximum, "range order")

    local raw = display.brightness()
    check(type(raw) == "number", "raw brightness")
    check(raw >= minimum and raw <= maximum, "raw brightness in range")

    local original = display.brightness_percent()
    check(type(original) == "number" and original >= 0 and original <= 100, "percent")

    check(display.set_brightness_percent(original) == true, "set same percent")
    check(display.set_brightness_percent(60) == true, "set 60 percent")
    check(display.set_brightness_percent(original) == true, "restore percent")
else
    local value, reason = display.brightness_percent()
    check(value == nil and reason == "no backlight", "no-backlight reason")
end

result = "display: PASS (" .. checks .. " checks)"
