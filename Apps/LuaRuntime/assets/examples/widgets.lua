-- Framework widget smoke test: cards, chips, slider boxes, lists and indicators.

function on_show()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN }

    -- Card groups content and uses the theme's card surface.
    local card = lvgl.Card(screen, { w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT })
    lvgl.Label(card, { text = "Card contents" })

    -- Chip: text lives on an internal label, checked is the theme's selected look.
    local chip = lvgl.Chip(screen, { text = "Chip" })
    assert(chip:text() == "Chip", "chip text should round-trip")
    chip:set{ text = "Selected" }
    chip:set_checked(true)
    assert(chip:is_checked(), "chip should accept checked state")
    assert(chip:text() == "Selected", "chip text should update")

    -- SliderBox: range and step are positional, value is a property.
    local sliderbox = lvgl.SliderBox(screen, 25, 300, 5, 100)
    assert(sliderbox:value() == 100, "slider box initial value")
    sliderbox:set{ value = 150 }
    assert(sliderbox:value() == 150, "slider box value set")
    sliderbox:set{ value = 999 }
    assert(sliderbox:value() == 300, "slider box clamps to max")

    -- List: rows, buttons and divider handling.
    local list = lvgl.List(screen, { w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT })
    list:add_text("Section")
    list:add_button("Item one")
    list:add_button("Item two", lvgl.SYMBOL.SETTINGS)
    list:hide_last_divider()

    -- Indicators.
    local pages = lvgl.PageIndicator(screen)
    pages:set_page_count(5)
    pages:set_page(2)
    pages:set_vertical(false)

    local steps = lvgl.StepIndicator(screen, { w = lvgl.PCT(100) })
    steps:set_step_count(4)
    steps:set_finished_count(2)

    -- Buttons and badge.
    local icon = lvgl.IconButton(screen, { variant = lvgl.ICON_BUTTON.FILLED })
    lvgl.Label(icon, { text = lvgl.SYMBOL.OK, text_font = 'icon', align = lvgl.ALIGN.CENTER })
    lvgl.Badge(icon, { align = lvgl.ALIGN.TOP_RIGHT })

    local text_button = lvgl.TextButton(screen)
    lvgl.Label(text_button, { text = "Skip" })

    result = "widgets PASS"
end
