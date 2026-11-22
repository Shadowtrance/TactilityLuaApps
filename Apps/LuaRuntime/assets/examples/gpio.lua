-- Live GPIO pin states, written entirely in Lua.
--
-- The Lua counterpart of the C GPIO app: a centred block of squares, one per
-- pin, filled when high and outlined when low. It uses the capability API only
-- (count/level), so it reads pins without claiming them.

local COLUMNS = 10
local SQUARE_SIZE = 24
local SQUARE_SPACING = 4

local controller
local timer
local squares = {}
local states = {}

local function find_controller()
    for _, name in ipairs(tactility.device.list()) do
        local device = tactility.device.find(name)
        if device then
            if device:type() == "gpio-controller" and device:is_ready() then
                return device
            end
            device:release()
        end
    end
    return nil
end

local function row_style()
    return {
        w = lvgl.SIZE_CONTENT,
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.ROW,
        flex_align = {
            main = lvgl.FLEX_ALIGN.START,
            cross = lvgl.FLEX_ALIGN.CENTER,
            track = lvgl.FLEX_ALIGN.CENTER,
        },
        pad_all = 0,
        pad_column = SQUARE_SPACING,
        border_width = 0,
        radius = 0,
        bg_opa = lvgl.OPA.TRANSP,
    }
end

local function add_square(row, added, secondary)
    -- pad_all = 0 matters: with the theme's default padding the square's inner
    -- content area leaves a small background fragment visible in the corner.
    -- The C GPIO app sets the same thing on its squares.
    local square = lvgl.Object(row, {
        w = SQUARE_SIZE,
        h = SQUARE_SIZE,
        pad_all = 0,
        radius = 0,
        border_width = 1,
        border_color = secondary,
        bg_color = secondary,
        bg_opa = lvgl.OPA.TRANSP,
    })
    squares[added] = square
end

local function build_grid(parent, pin_count, secondary)
    local grid = lvgl.Object(parent, {
        w = lvgl.SIZE_CONTENT,
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        flex_align = {
            main = lvgl.FLEX_ALIGN.CENTER,
            cross = lvgl.FLEX_ALIGN.CENTER,
            track = lvgl.FLEX_ALIGN.CENTER,
        },
        pad_all = 0,
        pad_row = SQUARE_SPACING,
        border_width = 0,
        radius = 0,
        bg_opa = lvgl.OPA.TRANSP,
    })

    local row
    local column = 0
    local added = 0

    for pin = 0, pin_count - 1 do
        if column == 0 then
            row = lvgl.Object(grid, row_style())
            lvgl.Label(row, { text = string.format("%02d", pin) })
        end

        add_square(row, added + 1, secondary)
        added = added + 1
        column = column + 1

        if column == COLUMNS then
            lvgl.Label(row, { text = string.format("%02d", pin) })
            column = 0
        end
    end
end

local function refresh()
    if not controller then
        return
    end

    for pin = 0, #squares - 1 do
        local high = tactility.gpio.level(controller, pin) and true or false
        -- Only touch a square when its state changes. Repainting every square
        -- on every tick is visible as flicker on the panel.
        if states[pin + 1] ~= high then
            states[pin + 1] = high
            local square = squares[pin + 1]
            if square:is_valid() then
                square:set{ bg_opa = high and lvgl.OPA.COVER or lvgl.OPA.TRANSP }
            end
        end
    end
end

function on_show()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN }

    controller = find_controller()
    if not controller then
        lvgl.Label(screen, { text = "No GPIO controller found" })
        return
    end

    local pin_count = tactility.gpio.count(controller)
    if not pin_count then
        lvgl.Label(screen, { text = "GPIO controller reports no pins" })
        controller:release()
        controller = nil
        return
    end

    local content = lvgl.Object(screen, {
        w = lvgl.PCT(100),
        flex_grow = 1,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        flex_align = {
            main = lvgl.FLEX_ALIGN.CENTER,
            cross = lvgl.FLEX_ALIGN.CENTER,
            track = lvgl.FLEX_ALIGN.CENTER,
        },
        pad_all = 0,
        border_width = 0,
        radius = 0,
        bg_opa = lvgl.OPA.TRANSP,
    })

    build_grid(content, pin_count, lvgl.theme_color_secondary())
    refresh()

    timer = lvgl.Timer{ period = 100, callback = refresh }
end

function on_destroy()
    if timer then
        timer:delete()
        timer = nil
    end
    if controller then
        controller:release()
        controller = nil
    end
    squares = {}
    states = {}
end
