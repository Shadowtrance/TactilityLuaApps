-- Lights Out: tapping a tile flips it and its four neighbours. Clear the board
-- to win. New boards are built by replaying random taps from an empty board, so
-- every puzzle is solvable.
--
-- The palette reuses Simon's dim/bright treatment: tiles are a dimmed accent at
-- rest and the full accent when lit, which reads well in both themes and on
-- monochrome displays.

local SIZE = 5

local root = lvgl.root()
root:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN }

local function channel(value, shift)
    return math.floor(value / 2 ^ shift) % 256
end

local function mix(color_a, color_b, percent)
    local r = math.floor(channel(color_a, 16) + (channel(color_b, 16) - channel(color_a, 16)) * percent / 100)
    local g = math.floor(channel(color_a, 8) + (channel(color_b, 8) - channel(color_a, 8)) * percent / 100)
    local b = math.floor(channel(color_a, 0) + (channel(color_b, 0) - channel(color_a, 0)) * percent / 100)
    return r * 65536 + g * 256 + b
end

local gap = lvgl.theme_lerp(2, 8)
local radius = lvgl.theme_lerp(4, 12)
local lit_color = lvgl.theme_color_primary()
local off_color = mix(lit_color, 0x000000, 62)
local win_color = lvgl.theme_color_secondary()

local cells = {}
local lit = {}
local moves = 0
local best = 0
local solved = false
local generation = 0
local toggle
local render
local all_off
local update_status
local check_solved
local new_game

local header = lvgl.Object(root, {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
lvgl.Label(header, { text = "Lights Out", text_font = "large", flex_grow = 1 })
local new_button = lvgl.TextButton(header)
lvgl.Label(new_button, { text = "New", align = lvgl.ALIGN.CENTER })

local status_row = lvgl.Object(root, {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
local status_label = lvgl.Label(status_row, { text = "Tap a tile to clear the board", flex_grow = 1 })
local best_label = lvgl.Label(status_row, { text = "Best -", text_font = "small" })

-- A square board sized from the area left below the header, so the tiles stay
-- square in both orientations instead of stretching into wide rectangles.
local board_area = lvgl.Object(root, {
    w = lvgl.PCT(100), flex_grow = 1,
    layout = lvgl.LAYOUT.FLEX,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.CENTER, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
root:update_layout()
local area_width, area_height = board_area:size()
local inset = lvgl.theme_lerp(0, 8)
local cell_size = math.floor((
    math.min(area_width, area_height) - inset * 2 - gap * (SIZE - 1)
) / SIZE)
local board_size = cell_size * SIZE + gap * (SIZE - 1)
local board = lvgl.Object(board_area, {
    w = board_size, h = board_size,
    layout = lvgl.LAYOUT.GRID,
    pad_all = 0, pad_row = gap, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
local tracks = {}
for _ = 1, SIZE do
    tracks[#tracks + 1] = lvgl.GRID_FR(1)
end
board:grid(tracks, tracks)

for row = 1, SIZE do
    cells[row] = {}
    lit[row] = {}
    for column = 1, SIZE do
        lit[row][column] = false
        local cell = lvgl.Object(board, {
            bg_color = off_color,
            bg_opa = lvgl.OPA.COVER,
            border_width = 0,
            radius = radius,
            pad_all = 0,
        })
        cell:grid_cell(
            lvgl.GRID_ALIGN.STRETCH, column - 1, 1,
            lvgl.GRID_ALIGN.STRETCH, row - 1, 1
        )
        cell:add_flag(lvgl.FLAG.CLICKABLE)
        cell:on(lvgl.EVENT.CLICKED, function()
            if solved then
                return
            end
            toggle(row, column)
            moves = moves + 1
            render()
            check_solved()
        end)
        cells[row][column] = cell
    end
end

toggle = function(row, column)
    for row_offset = -1, 1 do
        for column_offset = -1, 1 do
            local is_cross = (row_offset == 0) ~= (column_offset == 0)
            local r = row + row_offset
            local c = column + column_offset
            if is_cross and r >= 1 and r <= SIZE and c >= 1 and c <= SIZE then
                lit[r][c] = not lit[r][c]
            end
        end
    end
end

render = function()
    for row = 1, SIZE do
        for column = 1, SIZE do
            cells[row][column]:set{
                bg_color = lit[row][column] and lit_color or off_color,
            }
        end
    end
end

all_off = function()
    for row = 1, SIZE do
        for column = 1, SIZE do
            if lit[row][column] then
                return false
            end
        end
    end
    return true
end

update_status = function()
    if solved then
        status_label:set{ text = string.format("Solved in %d moves!", moves) }
    else
        status_label:set{ text = string.format("%d moves", moves) }
    end
    best_label:set{ text = best > 0 and ("Best " .. best) or "Best -" }
end

check_solved = function()
    if not all_off() then
        update_status()
        return
    end
    solved = true
    if best == 0 or moves < best then
        best = moves
    end
    status_label:set{ text = string.format("Solved in %d moves!", moves) }
    best_label:set{ text = "Best " .. best }
    -- A short victory sweep so the win is visible without reading the status.
    local gen = generation
    for row = 1, SIZE do
        for column = 1, SIZE do
            cells[row][column]:set{ bg_color = win_color }
        end
    end
    lvgl.Timer{
        period = 900,
        repeat_count = 1,
        callback = function()
            if gen == generation then
                render()
            end
        end,
    }
end

new_game = function()
    generation = generation + 1
    solved = false
    moves = 0
    for row = 1, SIZE do
        for column = 1, SIZE do
            lit[row][column] = false
        end
    end
    -- Replaying taps keeps the board reachable from all-off, i.e. solvable.
    for _ = 1, 12 do
        toggle(math.random(1, SIZE), math.random(1, SIZE))
    end
    if all_off() then
        toggle(1, 1)
    end
    render()
    update_status()
end

new_button:on(lvgl.EVENT.CLICKED, function()
    new_game()
end)

math.randomseed(math.floor(tactility.time.uptime()))
new_game()
result = "lights out"
