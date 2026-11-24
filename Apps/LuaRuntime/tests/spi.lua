-- SPI capability smoke test: discovery, constants and the invalid-CS paths.
--
-- The test never selects a real peripheral: an out-of-range CS fails in
-- add_device() before any bus transaction, so displays and SD cards stay safe.

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

controller:release()
result = string.format("spi PASS: %s, invalid CS -> %s", controller_name, tx_reason)
