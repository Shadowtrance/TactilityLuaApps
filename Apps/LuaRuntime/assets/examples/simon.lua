-- Simon: watch the sequence, repeat it, and it grows by one every round.
--
-- The four pads use the theme's accents so the game follows light/dark and the
-- user's primary/secondary colours. On monochrome displays the pads also show
-- their number, so the game is still playable without colour.

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

local gap = lvgl.theme_lerp(2, 10)
local colors = {
    lvgl.theme_color_primary(),
    lvgl.theme_color_secondary(),
    lvgl.theme_color_error(),
    mix(lvgl.theme_color_primary(), lvgl.theme_color_secondary(), 50),
}
-- Dim pads at rest and the full accent while flashing: the contrast is what
-- makes the sequence readable at a glance.
local dim = {}
local highlight = {}
for index = 1, #colors do
    dim[index] = mix(colors[index], 0x000000, 60)
    highlight[index] = colors[index]
end

local sequence = {}
local score = 0
local best = 0
local input_index = 1
local accepting = false
local generation = 0
local press
local start_game

local header = lvgl.Object(root, {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
lvgl.Label(header, { text = "Simon", text_font = "large", flex_grow = 1 })

local start_button = lvgl.TextButton(header)
local start_label = lvgl.Label(start_button, { text = "Start", align = lvgl.ALIGN.CENTER })

local status_row = lvgl.Object(root, {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
local status_label = lvgl.Label(status_row, { text = "Tap Start", flex_grow = 1 })
local best_label = lvgl.Label(status_row, { text = "Best 0", text_font = "small" })

local grid = lvgl.Object(root, {
    w = lvgl.PCT(100), flex_grow = 1,
    layout = lvgl.LAYOUT.GRID,
    pad_all = 0, pad_row = gap, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
grid:grid({ lvgl.GRID_FR(1), lvgl.GRID_FR(1) }, { lvgl.GRID_FR(1), lvgl.GRID_FR(1) })

local mono = lvgl.theme_mono()
local pads = {}
for index = 1, 4 do
    local column = (index - 1) % 2
    local row = math.floor((index - 1) / 2)
    local pad = lvgl.Button(grid, {
        bg_color = dim[index],
        bg_opa = lvgl.OPA.COVER,
        border_width = 0,
        radius = lvgl.theme_lerp(8, 24),
        pad_all = 0,
    })
    pad:grid_cell(
        lvgl.GRID_ALIGN.STRETCH, column, 1,
        lvgl.GRID_ALIGN.STRETCH, row, 1
    )
    pad:add_style(
        lvgl.Style{ bg_color = highlight[index], bg_opa = lvgl.OPA.COVER },
        lvgl.PART.MAIN | lvgl.STATE.PRESSED
    )
    if mono then
        lvgl.Label(pad, {
            text = tostring(index),
            text_font = "large",
            align = lvgl.ALIGN.CENTER,
            text_color = text_on(dim[index]),
        })
    end
    pads[index] = pad
    pad:on(lvgl.EVENT.CLICKED, function()
        if accepting then
            press(index)
        end
    end)
end

start_button:on(lvgl.EVENT.CLICKED, function()
    start_game()
end)

local function set_status(text)
    status_label:set{ text = text }
end

local function update_best()
    best_label:set{ text = "Best " .. best }
end

local function set_lit(index, lit)
    pads[index]:set{ bg_color = lit and highlight[index] or dim[index] }
end

local function flash(index)
    set_lit(index, true)
    lvgl.Timer{
        period = 200,
        repeat_count = 1,
        callback = function()
            set_lit(index, false)
        end,
    }
end

local function play_sequence(gen)
    local current = 0

    local function next_step()
        if gen ~= generation then
            return
        end
        current = current + 1
        if current > #sequence then
            input_index = 1
            accepting = true
            set_status("Your turn")
            return
        end
        local index = sequence[current]
        set_lit(index, true)
        lvgl.Timer{
            period = 400,
            repeat_count = 1,
            callback = function()
                if gen ~= generation then
                    return
                end
                set_lit(index, false)
                lvgl.Timer{
                    period = 200,
                    repeat_count = 1,
                    callback = next_step,
                }
            end,
        }
    end

    next_step()
end

local function begin_round()
    accepting = false
    set_status("Round " .. #sequence .. " - Watch...")
    local gen = generation
    lvgl.Timer{
        period = 800,
        repeat_count = 1,
        callback = function()
            if gen == generation then
                play_sequence(gen)
            end
        end,
    }
end

local function game_over()
    generation = generation + 1
    accepting = false
    if score > best then
        best = score
        update_best()
    end
    set_status("Wrong! Rounds: " .. score .. " - tap Retry")
    start_label:set{ text = "Retry" }
    for index = 1, 4 do
        set_lit(index, false)
    end
end

local function next_round()
    score = score + 1
    if score > best then
        best = score
        update_best()
    end
    sequence[#sequence + 1] = math.random(1, 4)
    set_status("Correct! Round " .. (#sequence) .. " next...")
    local gen = generation
    lvgl.Timer{
        period = 1000,
        repeat_count = 1,
        callback = function()
            if gen == generation then
                begin_round()
            end
        end,
    }
end

press = function(index)
    flash(index)
    if sequence[input_index] ~= index then
        game_over()
        return
    end
    input_index = input_index + 1
    if input_index > #sequence then
        accepting = false
        next_round()
    end
end

start_game = function()
    generation = generation + 1
    sequence = { math.random(1, 4) }
    score = 0
    input_index = 1
    accepting = false
    start_label:set{ text = "Restart" }
    begin_round()
end

math.randomseed(math.floor(tactility.time.uptime()))
update_best()
result = "simon"
