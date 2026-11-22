-- Object/style API regression test.
--
-- Exercises the core LVGL properties and methods that app clones rely on:
-- min/max sizing, margins, padding edges, outline/shadow, text spacing,
-- transforms, states, flags, child access, z-order and scrolling.

local checks = 0
local content

local function check(condition, message)
    checks = checks + 1
    if not condition then
        error("object api: " .. (message or ("check " .. checks .. " failed")), 2)
    end
end

function on_show()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN, pad_all = 8 }

    content = lvgl.Object(screen, {
        w = lvgl.PCT(100),
        h = lvgl.PCT(100),
        layout = lvgl.LAYOUT.FLEX,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        pad_all = 0,
    })

    local card = lvgl.Object(content, {
        w = 360,
        h = 300,
        layout = lvgl.LAYOUT.FLEX,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        pad_all = 8,
        pad_row = 4,
        pad_top = 6,
        pad_bottom = 6,
        pad_left = 10,
        pad_right = 10,
        margin_top = 4,
        margin_left = 2,
        bg_color = 0x202030,
        border_color = 0x445566,
        border_width = 2,
        border_opa = lvgl.OPA.COVER,
        radius = 12,
        min_width = 200,
        max_width = 420,
        min_height = 120,
        max_height = 360,
        outline_width = 2,
        outline_color = 0x00bbff,
        outline_opa = 100,
        outline_pad = 2,
        shadow_width = 8,
        shadow_color = 0x000000,
        shadow_opa = 100,
        shadow_ofs_x = 1,
        shadow_ofs_y = 2,
        shadow_spread = 1,
        text_align = lvgl.TEXT_ALIGN.CENTER,
        scroll_dir = lvgl.DIR.VER,
        scrollbar_mode = lvgl.SCROLLBAR.OFF,
    })
    check(card:is_valid(), "card created")

    card:update_layout()
    local width, height = card:size()
    check(width == 360 and height == 300, "size matches")

    local x, y = card:position()
    check(x >= 0 and y >= 0, "position readable")

    local labels = {}
    for i = 1, 10 do
        labels[i] = lvgl.Label(card, {
            text = "row " .. i,
            w = lvgl.PCT(100),
            h = 28,
            text_align = lvgl.TEXT_ALIGN.CENTER,
            text_letter_space = 1,
            text_line_space = 2,
        })
    end

    check(card:child_count() == 10, "child_count")
    check(card:child(1) == labels[1], "child(1)")
    check(card:child(0) == nil and card:child(11) == nil, "child bounds")

    card:add_flag(lvgl.FLAG.FLOATING)
    check(card:has_flag(lvgl.FLAG.FLOATING), "add_flag/has_flag")
    card:remove_flag(lvgl.FLAG.FLOATING)
    check(not card:has_flag(lvgl.FLAG.FLOATING), "remove_flag")

    -- Combined flags go through the per-flag setters/getters, not the deprecated
    -- lv_obj_add_flag() family.
    local combined = lvgl.FLAG.SNAPPABLE | lvgl.FLAG.PRESS_LOCK
    card:add_flag(combined)
    check(card:has_flag(combined), "combined flags")
    card:remove_flag(combined)
    check(not card:has_flag(combined), "combined flags removed")

    card:add_state(lvgl.STATE.CHECKED)
    check(card:has_state(lvgl.STATE.CHECKED), "add_state/has_state")
    card:remove_state(lvgl.STATE.CHECKED)
    check(not card:has_state(lvgl.STATE.CHECKED), "remove_state")

    labels[10]:move_foreground()
    check(card:child(10) == labels[10], "move_foreground")
    labels[10]:move_background()
    check(card:child(1) == labels[10], "move_background")

    labels[1]:set{
        transform_width = 2,
        transform_height = 2,
        transform_scale_x = 256,
        transform_scale_y = 256,
        transform_rotation = 0,
        transform_pivot_x = 10,
        transform_pivot_y = 10,
        bg_grad_color = 0x303040,
        bg_grad_dir = lvgl.GRAD_DIR.VER,
        border_side = lvgl.BORDER_SIDE.FULL,
        scroll_snap_x = lvgl.SCROLL_SNAP.START,
        scroll_snap_y = lvgl.SCROLL_SNAP.END,
    }
    check(labels[1]:is_valid(), "style properties applied")

    card:update_layout()
    card:scroll_to(0, 100, false)
    check(card:scroll_y() > 0, "scroll_to")
    card:scroll_by(0, -20, false)
    check(card:scroll_y() >= 0, "scroll_by")

    local arc = lvgl.Arc(content, {
        w = 120,
        h = 120,
        min = 0,
        max = 100,
        value = 40,
        arc_bg_start = 135,
        arc_bg_end = 45,
        arc_start = 135,
        arc_end = 45,
        arc_rotation = 0,
    })
    check(arc:value() == 40, "arc value")
    check(arc:is_valid(), "arc created")

    local spin = lvgl.Spinbox(content, {
        w = 180,
        h = 48,
        range = { min = -100, max = 100 },
        spin_step = 5,
        digit_format = { digits = 5, decimals = 0 },
    })
    spin:set{ value = 12 }
    check(spin:value() == 12, "spinbox value")
    check(spin:is_valid(), "spinbox created")

    lvgl.Label(content, { text = "object api: PASS (" .. checks .. " checks)" })
    result = "object api PASS (" .. checks .. " checks)"
end

function on_destroy()
    if content and content:is_valid() then
        content:clean()
    end
end
