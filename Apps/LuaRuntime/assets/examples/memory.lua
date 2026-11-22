-- Memory match: flip two cards at a time and find all eight pairs.
--
-- The cards use LVGL's built-in symbols so no bitmap assets are needed, and the
-- palette is derived from the theme accents. A short timer puts mismatched
-- cards back face down; the generation counter stops a pending flip from
-- touching a board that was reset in the meantime.

local PAIR_COUNT = 8
local COLUMNS = 4
local ROWS = 4

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

local function text_on(color)
    local luminance = 0.299 * channel(color, 16) + 0.587 * channel(color, 8) + 0.114 * channel(color, 0)
    return luminance > 140 and 0x000000 or 0xffffff
end

local gap = lvgl.theme_lerp(2, 8)
local radius = lvgl.theme_lerp(6, 16)
local up_color = lvgl.theme_color_primary()
local matched_color = mix(lvgl.theme_color_secondary(), 0x000000, 45)
local down_color = mix(lvgl.theme_color_primary(), 0x000000, 62)
local win_color = lvgl.theme_color_secondary()

local SYMBOLS = {
    lvgl.SYMBOL.OK,
    lvgl.SYMBOL.HOME,
    lvgl.SYMBOL.SETTINGS,
    lvgl.SYMBOL.WIFI,
    lvgl.SYMBOL.BATTERY_FULL,
    lvgl.SYMBOL.SAVE,
    lvgl.SYMBOL.REFRESH,
    lvgl.SYMBOL.TRASH,
}

local cards = {}
local labels = {}
local symbols = {}
local matched = {}
local face_up = {}
local first = nil
local second = nil
local busy = false
local moves = 0
local pairs_found = 0
local best = 0
local won = false
local generation = 0

local header = lvgl.Object(root, {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
lvgl.Label(header, { text = "Memory", text_font = "large", flex_grow = 1 })
local new_button = lvgl.TextButton(header)
lvgl.Label(new_button, { text = "New", align = lvgl.ALIGN.CENTER })

local status_row = lvgl.Object(root, {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
local status_label = lvgl.Label(status_row, { text = "Find all pairs", flex_grow = 1 })
local best_label = lvgl.Label(status_row, { text = "Best -", text_font = "small" })

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
    math.min(area_width, area_height) - inset * 2 - gap * (COLUMNS - 1)
) / COLUMNS)
local board_size = cell_size * COLUMNS + gap * (COLUMNS - 1)
local board = lvgl.Object(board_area, {
    w = board_size, h = board_size,
    layout = lvgl.LAYOUT.GRID,
    pad_all = 0, pad_row = gap, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
local column_tracks = {}
local row_tracks = {}
for _ = 1, COLUMNS do
    column_tracks[#column_tracks + 1] = lvgl.GRID_FR(1)
end
for _ = 1, ROWS do
    row_tracks[#row_tracks + 1] = lvgl.GRID_FR(1)
end
board:grid(column_tracks, row_tracks)

local function update_status()
    local text = string.format("Moves %d   Pairs %d/%d", moves, pairs_found, PAIR_COUNT)
    if won then
        text = string.format("Solved in %d moves!", moves)
    end
    status_label:set{ text = text }
    best_label:set{ text = best > 0 and ("Best " .. best) or "Best -" }
end

local function draw_card(index)
    local card = cards[index]
    if matched[index] then
        card:set{ bg_color = matched_color }
        labels[index]:set{ text = symbols[index], text_color = text_on(matched_color) }
    elseif face_up[index] then
        card:set{ bg_color = up_color }
        labels[index]:set{ text = symbols[index], text_color = text_on(up_color) }
    else
        card:set{ bg_color = down_color }
        labels[index]:set{ text = "?", text_color = text_on(down_color) }
    end
end

local function finish_turn()
    first = nil
    second = nil
    busy = false
    if pairs_found == PAIR_COUNT then
        won = true
        if best == 0 or moves < best then
            best = moves
        end
    end
    update_status()
    if won then
        -- Let the last pair register, then sweep the board in the secondary
        -- accent before the final board is redrawn.
        local gen = generation
        for index = 1, #cards do
            cards[index]:set{ bg_color = win_color }
        end
        lvgl.Timer{
            period = 1200,
            repeat_count = 1,
            callback = function()
                if gen == generation then
                    for index = 1, #cards do
                        draw_card(index)
                    end
                end
            end,
        }
    end
end

local function pick(index)
    if busy or won or matched[index] or face_up[index] then
        return
    end
    face_up[index] = true
    draw_card(index)
    if first == nil then
        first = index
        return
    end
    second = index
    moves = moves + 1
    update_status()
    local a = first
    local b = second
    if symbols[a] == symbols[b] then
        matched[a] = true
        matched[b] = true
        pairs_found = pairs_found + 1
        first = nil
        second = nil
        draw_card(a)
        draw_card(b)
        update_status()
        if pairs_found == PAIR_COUNT then
            finish_turn()
        end
        return
    end
    busy = true
    local gen = generation
    lvgl.Timer{
        period = 750,
        repeat_count = 1,
        callback = function()
            if gen ~= generation then
                return
            end
            face_up[a] = false
            face_up[b] = false
            draw_card(a)
            draw_card(b)
            finish_turn()
        end,
    }
end

local function shuffle(t)
    for i = #t, 2, -1 do
        local j = math.random(1, i)
        t[i], t[j] = t[j], t[i]
    end
end

local function new_game()
    generation = generation + 1
    first = nil
    second = nil
    busy = false
    won = false
    moves = 0
    pairs_found = 0
    local deck = {}
    for pair = 1, PAIR_COUNT do
        deck[#deck + 1] = SYMBOLS[pair]
        deck[#deck + 1] = SYMBOLS[pair]
    end
    shuffle(deck)
    for index = 1, #cards do
        symbols[index] = deck[index]
        matched[index] = false
        face_up[index] = false
        draw_card(index)
    end
    update_status()
end

for index = 1, COLUMNS * ROWS do
    local column = (index - 1) % COLUMNS
    local row = math.floor((index - 1) / COLUMNS)
    local card = lvgl.Button(board, {
        bg_color = down_color,
        bg_opa = lvgl.OPA.COVER,
        border_width = 0,
        shadow_width = 0,
        radius = radius,
        pad_all = 0,
    })
    card:grid_cell(
        lvgl.GRID_ALIGN.STRETCH, column, 1,
        lvgl.GRID_ALIGN.STRETCH, row, 1
    )
    labels[index] = lvgl.Label(card, {
        text = "?",
        text_font = "large",
        align = lvgl.ALIGN.CENTER,
        text_color = text_on(down_color),
    })
    cards[index] = card
    card:on(lvgl.EVENT.CLICKED, function()
        pick(index)
    end)
end

new_button:on(lvgl.EVENT.CLICKED, function()
    new_game()
end)

math.randomseed(math.floor(tactility.time.uptime()))
new_game()
result = "memory"
