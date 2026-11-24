-- ADC capability smoke test: controller discovery and raw channel reads.
--
-- A board without a configured channel still exercises the API and the error
-- path; the test does not assume a particular devicetree channel.

local controller
local controller_name
for _, name in ipairs(tactility.device.list()) do
    local device = tactility.device.find(name)
    if device then
        if device:type() == "adc-controller" then
            controller = device
            controller_name = name
            break
        end
        device:release()
    end
end

assert(tactility.adc, "tactility.adc missing")
assert(tactility.adc.api == 1, "unexpected adc api")

if controller then
    local raw, reason = tactility.adc.read_raw(controller, 0, 50)
    assert(raw == nil or type(raw) == "number", reason or "raw read should be a number")

    local missing, missing_reason = tactility.adc.read_raw(controller, 255, 50)
    assert(missing == nil, "invalid channel should not return a value")
    assert(type(missing_reason) == "string", "invalid channel should return a reason")

    controller:release()
    result = string.format(
        "adc PASS: %s, channel 0 -> %s",
        controller_name,
        raw and tostring(raw) or "unconfigured"
    )
else
    result = "adc PASS: no adc-controller"
end
