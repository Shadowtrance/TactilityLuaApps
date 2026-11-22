-- Composite widget regression test.
--
-- Covers the non-deprecated composites that the firmware enables: Tabview and
-- Tileview. Menu and Msgbox are deliberately not bound: LVGL 9.6 deprecates Menu,
-- and Msgbox is disabled in the firmware configuration.

local checks = 0

local function check(condition, message)
    checks = checks + 1
    if not condition then
        error("widget batch 3: " .. (message or ("check " .. checks .. " failed")), 2)
    end
end

local function run_tests()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.ROW, pad_all = 8, pad_column = 8 }

    local tabview = lvgl.Tabview(screen, {
        w = lvgl.PCT(50),
        h = lvgl.PCT(100),
    })

    local tab_one = tabview:add_tab("One")
    local tab_two = tabview:add_tab("Two")

    check(tabview:tab_count() == 2, "tabview count")
    check(tabview:active() == 1, "tabview initial active")

    lvgl.Label(tab_one, { text = "Tab one", text_align = lvgl.TEXT_ALIGN.CENTER, w = lvgl.PCT(100) })
    lvgl.Label(tab_two, { text = "Tab two", text_align = lvgl.TEXT_ALIGN.CENTER, w = lvgl.PCT(100) })

    -- The framework List shares the add_text method name with nothing now, but keep
    -- this check so the dispatch removal does not silently break it.
    local list = lvgl.List(tab_two, { w = lvgl.PCT(100), h = 160 })
    check(list:add_text("Header") ~= nil, "list add_text")
    check(list:add_button("Item") ~= nil, "list add_button")

    check(tabview:tab_button(1) ~= nil, "tabview tab button")
    check(tabview:content() ~= nil, "tabview content")
    check(tabview:tab_bar() ~= nil, "tabview tab bar")

    tabview:set_active(2)
    check(tabview:active() == 2, "tabview set active")

    tabview:set_tab_text(2, "Second")
    tabview:set{
        active_tab = 1,
        tab_bar_position = lvgl.DIR.BOTTOM,
        tab_bar_size = 44,
    }
    check(tabview:active() == 1, "tabview active property")

    local right = lvgl.Object(screen, {
        w = lvgl.PCT(50),
        h = lvgl.PCT(100),
        layout = lvgl.LAYOUT.FLEX,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        pad_all = 4,
        pad_row = 6,
    })

    local tileview = lvgl.Tileview(right, { w = lvgl.PCT(100), h = 190 })

    local tile_one = tileview:add_tile(0, 0, lvgl.DIR.RIGHT)
    local tile_two = tileview:add_tile(1, 0, lvgl.DIR.LEFT)

    lvgl.Label(tile_one, { text = "Tile 0,0", align = lvgl.ALIGN.CENTER })
    lvgl.Label(tile_two, { text = "Tile 1,0", align = lvgl.ALIGN.CENTER })

    -- LVGL leaves the active tile unset until one is selected.
    check(tileview:active_tile() == nil, "tileview no initial tile")

    tileview:set_tile(tile_one, false)
    check(tileview:active_tile() == tile_one, "tileview set tile")

    tileview:set_tile(tile_two, false)
    check(tileview:active_tile() == tile_two, "tileview second tile")

    tileview:goto_tile(0, 0, false)
    check(tileview:active_tile() == tile_one, "tileview goto tile")

    lvgl.Label(right, { text = "widget batch 3: PASS (" .. checks .. " checks)" })

    result = "widget batch 3 PASS (" .. checks .. " checks)"
end

function on_show()
    local ok, error_message = pcall(run_tests)
    if ok then
        return
    end

    local label = lvgl.Label(lvgl.root(), {
        w = lvgl.PCT(100),
        text_color = 0xff5555,
        text = tostring(error_message),
    })
    label:add_flag(lvgl.FLAG.IGNORE_LAYOUT)
    label:set{ x = 8, y = 80 }
    result = tostring(error_message)
end
