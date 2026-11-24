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

local config, config_reason = tactility.uart.config(controller)
local config_note
if config ~= nil then
    assert(type(config) == "table", "config should return a table")
    assert(type(config.baud_rate) == "number" and config.baud_rate > 0, "config baud_rate")
    assert(config.data_bits >= 5 and config.data_bits <= 8, "config data_bits")
    assert(config.parity == "none" or config.parity == "even" or config.parity == "odd", "config parity")
    assert(config.stop_bits == 1 or config.stop_bits == 1.5 or config.stop_bits == 2, "config stop_bits")
    config_note = string.format("%d baud", config.baud_rate)
else
    assert(type(config_reason) == "string" and #config_reason > 0, "config reason")
    config_note = "no config: " .. config_reason
end

local invalid_ok = pcall(tactility.uart.set_config, controller, { parity = "invalid" })
assert(not invalid_ok, "an invalid parity should raise")

local note = string.format(
    "uart PASS: %s, open=%s, available=%s, %s",
    controller_name,
    tostring(open),
    available and tostring(available) or "n/a",
    config_note
)

if not open then
    -- Same configuration, so the hardware is unaffected; the controller's own
    -- driver rejects this when the port is open.
    if config ~= nil then
        local applied, applied_reason = tactility.uart.set_config(controller, config)
        assert(applied == true or (applied == nil and type(applied_reason) == "string"), "set_config result")
        if applied then
            local after = tactility.uart.config(controller)
            assert(
                after.baud_rate == config.baud_rate
                    and after.data_bits == config.data_bits
                    and after.parity == config.parity
                    and after.stop_bits == config.stop_bits,
                "set_config round trip"
            )
        end
    end

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
