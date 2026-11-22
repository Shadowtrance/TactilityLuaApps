-- Minimal grid probe, to isolate the grid layout from the I2C scanner.

function on_show()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN }

    local card = lvgl.Card(screen, { w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT })
    card:grid({ lvgl.GRID_CONTENT, lvgl.GRID_FR(1) }, { lvgl.GRID_CONTENT })
    card:grid_align(lvgl.GRID_ALIGN.START, lvgl.GRID_ALIGN.START)

    local label = lvgl.Label(card, { text = 'grid cell' })
    label:grid_cell(lvgl.GRID_ALIGN.START, 0, 1, lvgl.GRID_ALIGN.START, 0, 1)

    local value = lvgl.Label(card, { text = 'second column' })
    value:grid_cell(lvgl.GRID_ALIGN.STRETCH, 1, 1, lvgl.GRID_ALIGN.START, 0, 1)

    result = 'grid ok'
end
