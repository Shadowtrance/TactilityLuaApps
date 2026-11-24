-- UART capability smoke test: discovery and safe introspection.
--
-- A controller owned by a service (GPS, radio) must not be opened or read, so the
-- test only exercises the error paths when is_open() is false.

local controller
local controller_name
for _, name in ipairs(tactility.device.list()) do
    local device = tactility.device.find(name)
    if device then
        if device:type() == "uart-controller" then
            controller = device
            controller_name = name
            break
        end
        device:release()
    end
end

assert(tactility.uart, "tactility.uart missing")
assert(tactility.uart.api == 1, "unexpected uart api")

if not controller then
    result = "uart PASS: no uart-controller"
    return
end

local open = tactility.uart.is_open(controller)
assert(type(open) == "boolean", "is_open should be a boolean")

local available, available_reason = tactility.uart.available(controller)
assert(available == nil or type(available) == "number", available_reason or "available should be a number")

local note = string.format(
    "uart PASS: %s, open=%s, available=%s",
    controller_name,
    tostring(open),
    available and tostring(available) or "n/a"
)

if not open then
    local value, reason = tactility.uart.read_byte(controller, 1)
    assert(value == nil and type(reason) == "string", "closed read_byte should return nil, reason")

    local chunk, chunk_reason = tactility.uart.read_bytes(controller, 4, 1)
    assert(chunk == nil and type(chunk_reason) == "string", "closed read_bytes should return nil, reason")

    local until_chunk, until_reason = tactility.uart.read_until(controller, 10, 8, 1)
    assert(until_chunk == nil and type(until_reason) == "string", "closed read_until should return nil, reason")

    local wrote, write_reason = tactility.uart.write_byte(controller, 0, 1)
    assert(wrote == nil and type(write_reason) == "string", "closed write_byte should return nil, reason")

    local written, written_reason = tactility.uart.write_bytes(controller, "", 1)
    assert(written == true or (written == nil and type(written_reason) == "string"), "write_bytes result")

    note = note .. ", closed error paths"
end

controller:release()
result = note
