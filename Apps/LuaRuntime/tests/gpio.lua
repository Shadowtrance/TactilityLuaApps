-- GPIO capability + generated binding test.
--
-- Finds the GPIO controller through tactility.device, checks the generated
-- count() glue and the error path of acquire(). It deliberately does not drive
-- a real pin: which pins are safe depends on the board's devicetree.

local controller
for _, name in ipairs(tactility.device.list()) do
    local device = tactility.device.find(name)
    if device then
        local kind = device:type()
        if kind == "gpio-controller" then
            controller = device
            break
        end
        device:release()
    end
end

assert(controller, "no gpio-controller device")
assert(tactility.gpio, "tactility.gpio missing")
assert(tactility.gpio.api == 1, "unexpected gpio api")

local count, count_reason = tactility.gpio.count(controller)
assert(count and count > 0, "pin count: " .. tostring(count_reason))

local pin, reason = tactility.gpio.acquire(controller, 200, tactility.gpio.OUTPUT)
assert(pin == nil, "invalid pin should not be acquired")
assert(type(reason) == "string", "invalid pin should return a reason")

controller:release()

result = string.format(
    "gpio PASS: %d pins, api %d, constants %d/%d/%d, invalid pin -> %s",
    count,
    tactility.gpio.api,
    tactility.gpio.INPUT,
    tactility.gpio.OUTPUT,
    tactility.gpio.PULL_UP,
    reason
)
