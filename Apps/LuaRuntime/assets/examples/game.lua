-- Whack-a-mole: a small game built from Lua buttons, events and a timer.
--
-- Cells keep the theme's button styling. The target is marked with an overlay
-- style using the theme's primary colour instead of replacing the button's
-- surface.

local screen = lvgl.root()
screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN }

local header = lvgl.Object(screen, {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
})

local score_label = lvgl.Label(header, { text = 'Score: 0', flex_grow = 1, text_font = 'large' })

local reset = lvgl.Button(header, { w = 100, h = 40 })
lvgl.Label(reset, { text = 'Reset', align = lvgl.ALIGN.CENTER })

local grid = lvgl.Object(screen, {
    w = lvgl.PCT(100), flex_grow = 1,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
})

local target_style = lvgl.Style{
    bg_color = lvgl.theme_color_primary(),
    bg_opa = lvgl.OPA.COVER,
}

local cells = {}
local target = nil
local score = 0

local function update_score()
    score_label:set{ text = 'Score: ' .. score }
end

local function clear_target()
    if target then
        cells[target]:remove_style(target_style)
    end
end

local function new_target()
    clear_target()
    target = math.random(1, 9)
    cells[target]:add_style(target_style)
end

local function hit(index)
    if index == target then
        score = score + 1
        update_score()
        new_target()
    else
        score = math.max(0, score - 1)
        update_score()
    end
end

for row_index = 1, 3 do
    local row = lvgl.Object(grid, {
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.ROW,
    })

    for column_index = 1, 3 do
        local index = (row_index - 1) * 3 + column_index
        local cell = lvgl.Button(row, { flex_grow = 1, h = 90 })
        cell:on(lvgl.EVENT.CLICKED, function()
            hit(index)
        end)
        cells[index] = cell
    end
end

reset:on(lvgl.EVENT.CLICKED, function()
    clear_target()
    target = nil
    score = 0
    update_score()
    new_target()
end)

math.randomseed(math.floor(tactility.time.uptime()))
new_target()
lvgl.Timer{ period = 1200, callback = new_target }
