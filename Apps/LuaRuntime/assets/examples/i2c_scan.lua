-- I2C scanner, written entirely in Lua.
--
-- Mirrors the built-in scanner: a row of bus chips and a grid card with the
-- address in the first column and the devicetree device names in the second.
-- Every bus is scanned once while the window is being built, then the chip
-- buttons just show or hide the matching card. Rebuilding a grid after the
-- window has been laid out corrupts teardown in this LVGL build, so switching
-- never creates widgets.

local FIRST_ADDRESS = 0x08
local LAST_ADDRESS = 0x77
local PROBE_TIMEOUT = 10
-- A disturbed probe can report a device that isn't there; the C scanner treats
-- an address as present only when several probes in a row acknowledge it.
local PROBE_COUNT = 3

local controllers = {}
local chips = {}
local results = {}
local screen

local function select_chip(index)
    for i, chip in ipairs(chips) do
        chip:set_checked(i == index)
    end

    for i, card in ipairs(results) do
        if i == index then
            card:remove_flag(lvgl.FLAG.HIDDEN)
        else
            card:add_flag(lvgl.FLAG.HIDDEN)
        end
    end
end

-- The devicetree children on the bus, keyed by their address.
local function device_names(controller)
    local names = {}
    for _, name in ipairs(controller:children()) do
        local child = tactility.device.find(name)
        if child then
            local address = child:address()
            names[address] = names[address] and (names[address] .. ', ' .. name) or name
            child:release()
        end
    end
    return names
end

local function scan_controller(controller)
    local found = {}
    for address = FIRST_ADDRESS, LAST_ADDRESS do
        local present = true
        for _ = 1, PROBE_COUNT do
            if not tactility.i2c.probe(controller, address, PROBE_TIMEOUT) then
                present = false
                break
            end
        end
        if present then
            found[#found + 1] = address
        end
    end
    return found
end

local function build_results(controller)
    local names = device_names(controller)
    local found = scan_controller(controller)

    local card = lvgl.Card(screen, {
        w = lvgl.PCT(100),
        h = lvgl.SIZE_CONTENT,
    })

    local rows = {}
    for i = 1, #found + 1 do
        rows[i] = lvgl.GRID_CONTENT
    end
    card:grid({ lvgl.GRID_CONTENT, lvgl.GRID_FR(1) }, rows)

    local title = lvgl.Label(card, {
        text = #found == 0 and 'No devices found'
            or string.format('%d device%s found', #found, #found == 1 and '' or 's'),
    })
    title:grid_cell(lvgl.GRID_ALIGN.START, 0, 2, lvgl.GRID_ALIGN.CENTER, 0, 1)

    for i, address in ipairs(found) do
        local address_label = lvgl.Label(card, { text = string.format('0x%02X', address) })
        address_label:grid_cell(lvgl.GRID_ALIGN.START, 0, 1, lvgl.GRID_ALIGN.START, i, 1)

        local names_label = lvgl.Label(card, {
            text = names[address] or '',
            w = lvgl.PCT(100),
            long_mode = lvgl.LONG.WRAP,
        })
        names_label:grid_cell(lvgl.GRID_ALIGN.STRETCH, 1, 1, lvgl.GRID_ALIGN.START, i, 1)
    end

    return card, #found
end

function on_show()
    screen = lvgl.root()
    screen:set{
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        flex_align = {
            main = lvgl.FLEX_ALIGN.START,
            cross = lvgl.FLEX_ALIGN.CENTER,
            track = lvgl.FLEX_ALIGN.START,
        },
    }

    for _, name in ipairs(tactility.device.list()) do
        local device = tactility.device.find(name)
        if device then
            if device:type() == 'i2c-controller' and device:is_ready() then
                controllers[#controllers + 1] = device
            else
                device:release()
            end
        end
    end

    if #controllers == 0 then
        lvgl.Label(screen, { text = 'I2C interface not available' })
        return
    end

    -- The chips' margins leave room for their focus rings and space them apart.
    local chips_row = lvgl.Object(screen, {
        w = lvgl.SIZE_CONTENT,
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.ROW,
        pad_all = 0,
        pad_column = 0,
        border_width = 0,
        bg_opa = lvgl.OPA.TRANSP,
    })

    for index, controller in ipairs(controllers) do
        -- A fresh local per chip: every handler must keep its own bus index.
        local bus_index = index
        local chip = lvgl.Chip(chips_row, { text = controller:name() })
        chip:on(lvgl.EVENT.CLICKED, function()
            select_chip(bus_index)
        end)
        chips[index] = chip
    end

    local summary
    for index, controller in ipairs(controllers) do
        local card, count = build_results(controller)
        results[index] = card
        if index ~= 1 then
            card:add_flag(lvgl.FLAG.HIDDEN)
        end
        if index == 1 then
            summary = string.format('I2C scan %s: %d device(s)', controller:name(), count)
        end
    end

    select_chip(1)
    result = summary
end

function on_destroy()
    for _, controller in ipairs(controllers) do
        controller:release()
    end
    controllers = {}
end
