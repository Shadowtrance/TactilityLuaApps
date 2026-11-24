-- SPI capability smoke test: discovery, constants, invalid-CS paths and a
-- persistent handle.
--
-- No real peripheral is selected: an out-of-range CS fails in add_device()
-- before any transaction, and CS_NONE leaves every chip select deselected, so
-- displays and SD cards stay safe.

local controller
local controller_name
for _, name in ipairs(tactility.device.list()) do
    local device = tactility.device.find(name)
    if device then
        if device:type() == "spi-controller" then
            controller = device
            controller_name = name
            break
        end
        device:release()
    end
end

assert(tactility.spi, "tactility.spi missing")
assert(tactility.spi.api == 1, "unexpected spi api")
assert(tactility.spi.CS_NONE == -1, "unexpected CS_NONE")

if not controller then
    result = "spi PASS: no spi-controller"
    return
end

local tx, tx_reason = tactility.spi.transmit(controller, 1000000, 0, 99, "x")
assert(tx == nil and type(tx_reason) == "string", "invalid CS should fail transmit")

local rx, rx_reason = tactility.spi.read(controller, 1000000, 0, 99, 1)
assert(rx == nil and type(rx_reason) == "string", "invalid CS should fail read")

local written, write_reason = tactility.spi.write(controller, 1000000, 0, 99, "x")
assert(written == nil and type(write_reason) == "string", "invalid CS should fail write")

-- Briefly take the bus only when it is free; this exercises the generated
-- lock/unlock bindings without waiting on a driver that is using it.
local locked, lock_reason = tactility.spi.try_lock(controller, 1)
if locked then
    assert(tactility.spi.unlock(controller), "unlock failed")
else
    assert(type(lock_reason) == "string", "try_lock should return a reason")
end

local handle, handle_reason = tactility.spi.open(controller, 1000000, 0, tactility.spi.CS_NONE)
local handle_note
if handle then
    assert(handle:is_open(), "handle should be open")
    local received = handle:transmit("\0")
    assert(type(received) == "string" and #received == 1, "handle transmit")
    assert(handle:write("\0") == true, "handle write")
    local read = handle:read(2)
    assert(type(read) == "string" and #read == 2, "handle read")
    assert(handle:close(), "handle close")
    assert(not handle:is_open(), "handle should be closed")
    assert(handle:close(), "close should be idempotent")

    local raised = not pcall(function() handle:transmit("x") end)
    assert(raised, "a closed handle should raise")
    handle_note = "persistent handle"
else
    assert(type(handle_reason) == "string", "open should return a reason")
    handle_note = "open failed: " .. handle_reason
end

controller:release()
result = string.format(
    "spi PASS: %s, invalid CS -> %s, %s",
    controller_name,
    tx_reason,
    handle_note
)
