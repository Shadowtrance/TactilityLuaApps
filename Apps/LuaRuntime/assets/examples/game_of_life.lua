-- Conway's Game of Life: a grid of themed cells, a generation timer and
-- tap-to-edit. The board is sized from the display, so it works in landscape
-- and portrait.

local root = lvgl.root()
root:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN }

local gap = lvgl.theme_lerp(2, 8)

local header = lvgl.Object(root, {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
lvgl.Label(header, { text = "Game of Life", text_font = "large", flex_grow = 1 })
local generation_label = lvgl.Label(header, { text = "Gen 0", text_font = "small" })

local board = lvgl.Card(root, {
    w = lvgl.PCT(100), flex_grow = 1,
    pad_all = 0,
})
local grid = lvgl.Object(board, {
    w = lvgl.PCT(100), h = lvgl.PCT(100),
    layout = lvgl.LAYOUT.GRID,
    pad_all = 0, pad_row = 1, pad_column = 1,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})

local controls = lvgl.Object(root, {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
local start_button = lvgl.Button(controls, { w = lvgl.SIZE_CONTENT, h = lvgl.SIZE_CONTENT })
local start_label = lvgl.Label(start_button, { text = "Start", align = lvgl.ALIGN.CENTER })
local random_button = lvgl.TextButton(controls)
lvgl.Label(random_button, { text = "Random", align = lvgl.ALIGN.CENTER })
local clear_button = lvgl.TextButton(controls)
lvgl.Label(clear_button, { text = "Clear", align = lvgl.ALIGN.CENTER })

-- Roughly square cells: fewer rows on a short display, more in portrait.
local columns = math.max(12, math.min(32, math.floor(lvgl.HOR_RES() / 48)))
local rows = math.max(8, math.min(24, math.floor(lvgl.VER_RES() / 48)))
local column_tracks = {}
local row_tracks = {}
for _ = 1, columns do
    column_tracks[#column_tracks + 1] = lvgl.GRID_FR(1)
end
for _ = 1, rows do
    row_tracks[#row_tracks + 1] = lvgl.GRID_FR(1)
end
grid:grid(column_tracks, row_tracks)

local alive_color = lvgl.theme_color_primary()
local cells = {}
local cells_alive = {}
local generation = 0
local running = false
local timer

for row = 1, rows do
    cells[row] = {}
    cells_alive[row] = {}
    for column = 1, columns do
        cells_alive[row][column] = false
        local cell = lvgl.Object(grid, {
            bg_color = alive_color,
            bg_opa = lvgl.OPA.TRANSP,
            border_width = 0,
            radius = 0,
            pad_all = 0,
        })
        cell:grid_cell(
            lvgl.GRID_ALIGN.STRETCH, column - 1, 1,
            lvgl.GRID_ALIGN.STRETCH, row - 1, 1
        )
        cell:add_flag(lvgl.FLAG.CLICKABLE)
        cells[row][column] = cell
        cell:on(lvgl.EVENT.CLICKED, function()
            local new_value = not cells_alive[row][column]
            cells_alive[row][column] = new_value
            cell:set{ bg_opa = new_value and lvgl.OPA.COVER or lvgl.OPA.TRANSP }
        end)
    end
end

local function render()
    for row = 1, rows do
        for column = 1, columns do
            cells[row][column]:set{
                bg_opa = cells_alive[row][column] and lvgl.OPA.COVER or lvgl.OPA.TRANSP,
            }
        end
    end
end

local function neighbours(row, column)
    local count = 0
    for row_offset = -1, 1 do
        for column_offset = -1, 1 do
            if row_offset ~= 0 or column_offset ~= 0 then
                local r = row + row_offset
                local c = column + column_offset
                if r >= 1 and r <= rows and c >= 1 and c <= columns and cells_alive[r][c] then
                    count = count + 1
                end
            end
        end
    end
    return count
end

local function step()
    local next_state = {}
    for row = 1, rows do
        next_state[row] = {}
        for column = 1, columns do
            local count = neighbours(row, column)
            local alive = cells_alive[row][column]
            next_state[row][column] = count == 3 or (alive and count == 2)
        end
    end
    cells_alive = next_state
    generation = generation + 1
    generation_label:set{ text = "Gen " .. generation }
    render()
end

local function set_running(value)
    running = value
    if running then
        timer:resume()
    else
        timer:pause()
    end
    start_label:set{ text = running and "Pause" or "Start" }
end

local function randomize()
    set_running(false)
    generation = 0
    generation_label:set{ text = "Gen 0" }
    for row = 1, rows do
        for column = 1, columns do
            cells_alive[row][column] = math.random() < 0.3
        end
    end
    render()
end

local function clear()
    set_running(false)
    generation = 0
    generation_label:set{ text = "Gen 0" }
    for row = 1, rows do
        for column = 1, columns do
            cells_alive[row][column] = false
        end
    end
    render()
end

timer = lvgl.Timer{ period = 180, callback = step, paused = true }
start_button:on(lvgl.EVENT.CLICKED, function()
    set_running(not running)
end)
random_button:on(lvgl.EVENT.CLICKED, randomize)
clear_button:on(lvgl.EVENT.CLICKED, clear)

math.randomseed(math.floor(tactility.time.uptime()))
randomize()
result = "game of life"
