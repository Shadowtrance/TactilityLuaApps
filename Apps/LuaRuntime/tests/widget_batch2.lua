-- Widget batch 2 regression test.
--
-- Exercises LED, Table, Scale, ImageButton, Chart, Calendar, Keyboard and
-- ButtonMatrix constructors, properties, methods and constants.

local checks = 0

local function check(condition, message)
    checks = checks + 1
    if not condition then
        error("widget batch 2: " .. (message or ("check " .. checks .. " failed")), 2)
    end
end

local function run_tests()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.ROW, pad_all = 8, pad_column = 8 }

    local left = lvgl.Object(screen, {
        w = lvgl.PCT(50),
        h = lvgl.PCT(100),
        layout = lvgl.LAYOUT.FLEX,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        pad_all = 4,
        pad_row = 4,
    })

    local right = lvgl.Object(screen, {
        w = lvgl.PCT(50),
        h = lvgl.PCT(100),
        layout = lvgl.LAYOUT.FLEX,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        pad_all = 4,
        pad_row = 4,
    })

    -- Table -----------------------------------------------------------------------------
    local table_widget = lvgl.Table(left, {
        w = lvgl.PCT(100),
        h = 110,
        rows = 3,
        columns = 2,
        column_widths = { 90, 120 },
        selected_cell = { row = 2, column = 1 },
    })

    local rows, columns = table_widget:dimensions()
    check(rows == 3 and columns == 2, "table dimensions")

    table_widget:set_cell(1, 1, "Name")
    table_widget:set_cell(1, 2, "Value")
    check(table_widget:cell(1, 1) == "Name", "table cell text")

    table_widget:set_cell_ctrl(1, 1, lvgl.TABLE_CTRL.TEXT_CROP)
    check(table_widget:has_cell_ctrl(1, 1, lvgl.TABLE_CTRL.TEXT_CROP), "table cell ctrl")
    table_widget:clear_cell_ctrl(1, 1, lvgl.TABLE_CTRL.TEXT_CROP)
    check(not table_widget:has_cell_ctrl(1, 1, lvgl.TABLE_CTRL.TEXT_CROP), "table clear ctrl")

    local selected_row, selected_column = table_widget:selected_cell()
    check(selected_row == 2 and selected_column == 1, "table selected cell")

    local ok, error_message = pcall(function()
        table_widget:set_cell(9, 9, "out")
    end)
    check(not ok and error_message:find("out of bounds") ~= nil, "table bounds")

    -- Chart -----------------------------------------------------------------------------
    local chart = lvgl.Chart(left, {
        w = lvgl.PCT(100),
        h = 120,
        chart_type = lvgl.CHART_TYPE.LINE,
        point_count = 8,
        update_mode = lvgl.CHART_UPDATE.SHIFT,
        div_lines = { h = 3, v = 4 },
        axis_range = { min = 0, max = 100 },
    })

    local series = chart:add_series(0x00aaff)
    check(series ~= nil, "chart series")

    chart:set_values(series, { 10, 20, 30, 40 })
    chart:next_value(series, 50)
    chart:set_all_values(series, 25)
    chart:set_series_color(series, 0xff5500)
    chart:hide_series(series, true)
    chart:hide_series(series, false)
    chart:refresh()
    check(chart:is_valid(), "chart methods")

    local second = chart:add_series(0xff0000, lvgl.CHART_AXIS.SECONDARY_Y)
    chart:remove_series(second)
    check(chart:is_valid(), "chart remove series")

    -- Calendar --------------------------------------------------------------------------
    local calendar = lvgl.Calendar(left, {
        w = lvgl.PCT(100),
        h = 200,
        today = { year = 2026, month = 11, day = 22 },
        shown_date = { year = 2026, month = 11 },
        day_names = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" },
    })

    local today = calendar:today()
    check(today.year == 2026 and today.month == 11 and today.day == 22, "calendar today")

    local shown = calendar:shown()
    check(shown.year == 2026 and shown.month == 11, "calendar shown")

    check(calendar:pressed() == nil, "calendar pressed")

    -- LED -------------------------------------------------------------------------------
    local led = lvgl.LED(right, {
        w = 36,
        h = 36,
        led_color = 0x00ff00,
        brightness = 180,
        on = true,
    })
    check(led:brightness() == 180, "led brightness")
    led:led_toggle()
    led:led_off()
    led:led_on()
    check(led:is_valid(), "led methods")

    -- Scale -----------------------------------------------------------------------------
    local scale = lvgl.Scale(right, {
        w = lvgl.PCT(100),
        h = 120,
        range = { min = 0, max = 100 },
        tick_count = 21,
        major_tick_every = 5,
        label_show = true,
        scale_mode = lvgl.SCALE_MODE.HORIZONTAL_BOTTOM,
        scale_angle = 270,
        scale_rotation = 0,
        text_src = { "0", "25", "50", "75", "100" },
    })

    local section = scale:add_section(60, 100)
    check(section ~= nil, "scale section")
    scale:set_section_range(section, 50, 100)

    local section_style = lvgl.Style{
        bg_color = 0x00ff00,
        bg_opa = lvgl.OPA.COVER,
    }
    scale:set_section_style(section, lvgl.PART.MAIN, section_style)
    check(scale:is_valid(), "scale methods")

    -- Image button ----------------------------------------------------------------------
    local image_button = lvgl.ImageButton(right, { w = 100, h = 50 })
    image_button:set_image_src(lvgl.IMAGEBUTTON.RELEASED, nil, lvgl.spinner_image(), nil)
    image_button:set_image_state(lvgl.IMAGEBUTTON.RELEASED)
    check(image_button:is_valid(), "image button")

    -- Keyboard --------------------------------------------------------------------------
    local textarea = lvgl.TextArea(right, {
        w = 220,
        h = 40,
        one_line = true,
        placeholder = "number",
    })

    local keyboard = lvgl.Keyboard(right, {
        w = lvgl.PCT(100),
        h = 170,
        textarea = textarea,
        keyboard_mode = lvgl.KEYBOARD_MODE.NUMBER,
        popovers = true,
    })

    check(keyboard:textarea() == textarea, "keyboard textarea")
    check(keyboard:keyboard_mode() == lvgl.KEYBOARD_MODE.NUMBER, "keyboard mode")
    check(keyboard:keyboard_popovers(), "keyboard popovers")

    -- Button matrix ---------------------------------------------------------------------
    local matrix = lvgl.ButtonMatrix(right, {
        w = lvgl.PCT(100),
        h = 110,
        map = { { "1", "2" }, { "3", "4" } },
        ctrl_map = { lvgl.BUTTONMATRIX_CTRL.CHECKABLE, lvgl.BUTTONMATRIX_CTRL.CHECKABLE,
                     lvgl.BUTTONMATRIX_CTRL.CHECKABLE, lvgl.BUTTONMATRIX_CTRL.CHECKABLE },
        one_checked = true,
        selected_button = 1,
    })

    check(matrix:button_text(1) == "1" and matrix:button_text(4) == "4", "matrix text")
    check(matrix:selected_button() == 1, "matrix initial selection")

    matrix:set_button_ctrl(2, lvgl.BUTTONMATRIX_CTRL.CHECKED)
    check(matrix:has_button_ctrl(2, lvgl.BUTTONMATRIX_CTRL.CHECKED), "matrix button ctrl")
    matrix:clear_button_ctrl(2, lvgl.BUTTONMATRIX_CTRL.CHECKED)
    check(not matrix:has_button_ctrl(2, lvgl.BUTTONMATRIX_CTRL.CHECKED), "matrix clear ctrl")

    matrix:set_button_width(1, 2)
    matrix:select_button(3)
    check(matrix:selected_button() == 3, "matrix select")

    matrix:select_button(nil)
    check(matrix:selected_button() == nil, "matrix clear selection")

    lvgl.Label(left, { text = "widget batch 2: PASS (" .. checks .. " checks)" })

    result = "widget batch 2 PASS (" .. checks .. " checks)"
end

function on_show()
    local ok, error_message = pcall(run_tests)
    if ok then
        return
    end

    -- Surface the failure without a serial console: a script error otherwise leaves
    -- whatever was built so far on screen and aborts silently.
    local label = lvgl.Label(lvgl.root(), {
        w = lvgl.PCT(100),
        text_color = 0xff5555,
        text = tostring(error_message),
    })
    label:add_flag(lvgl.FLAG.IGNORE_LAYOUT)
    label:set{ x = 8, y = 80 }
    result = tostring(error_message)
end
