-- I2C capability smoke test: controller discovery and address probing.
-- No register writes, so nothing on the bus is modified.

local controllers = {}
for _, name in ipairs(tactility.device.list()) do
    local device = tactility.device.find(name)
    if device then
        if device:type() == "i2c-controller" and device:is_ready() then
            controllers[#controllers + 1] = device
        else
            device:release()
        end
    end
end

assert(#controllers > 0, "no i2c-controller device")
assert(tactility.i2c.api == 1, "unexpected i2c api")

local addresses = { 0x10, 0x32, 0x36, 0x40, 0x41, 0x43, 0x44, 0x68 }
local found = {}
local responsive_controller
local responsive_address

for _, controller in ipairs(controllers) do
    for _, address in ipairs(addresses) do
        if tactility.i2c.probe(controller, address, 50) then
            found[#found + 1] = string.format("%s:0x%02X", controller:name(), address)
            if responsive_controller == nil then
                responsive_controller = controller
                responsive_address = address
            end
        end
    end
end

local missing, reason = tactility.i2c.probe(controllers[1], 0x7F, 20)
assert(missing == nil, "unused address should not respond")
assert(type(reason) == "string", "probe failure should return a reason")

-- Block transfers use Lua strings as buffers. Not every board has one of the known
-- devices, so only exercise the success path when an address answered; the failure
-- path below still runs without hardware.
if responsive_controller ~= nil then
    local block, block_error = tactility.i2c.read(responsive_controller, responsive_address, 4, 100)
    assert(type(block) == "string" and #block == 4, block_error or "block read failed")
end

local missing_block, block_reason = tactility.i2c.read(controllers[1], 0x7F, 2, 20)
assert(missing_block == nil, "block read on an unused address should fail")
assert(type(block_reason) == "string", "block read failure should return a reason")

local written, write_reason = tactility.i2c.write(controllers[1], 0x7F, "ab", 20)
assert(written == nil, "write on an unused address should fail")
assert(type(write_reason) == "string", "write failure should return a reason")

local combined, combined_reason = tactility.i2c.write_read(controllers[1], 0x7F, "\0", 2, 20)
assert(combined == nil, "write_read on an unused address should fail")
assert(type(combined_reason) == "string", "write_read failure should return a reason")


-- A controller's children carry the devicetree address used by the scanner grid.
local children = controllers[1]:children()
local child_addresses = 0
for _, name in ipairs(children) do
    local child = tactility.device.find(name)
    if child then
        assert(type(child:address()) == "number", "child should have an address")
        child_addresses = child_addresses + 1
        child:release()
    end
end

for _, controller in ipairs(controllers) do
    controller:release()
end

result = string.format(
    "i2c PASS: %d controller(s), %d device(s)%s, %d child address(es), 0x7F -> %s",
    #controllers,
    #found,
    #found > 0 and (" (" .. table.concat(found, ", ") .. ")") or "",
    child_addresses,
    reason
)
