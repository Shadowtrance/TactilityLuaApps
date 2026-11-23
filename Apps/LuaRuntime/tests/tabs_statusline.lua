-- Material 3 tabs and statusline: the widgets the firmware's newer apps use.
-- Covers the shared icon table too, since tabs with icons depend on it.

local checks = 0

local function check(condition, message)
    checks = checks + 1
    if not condition then
        error("tabs/statusline: " .. (message or ("check " .. checks .. " failed")), 2)
    end
end

local root = lvgl.root()
root:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN, pad_all = 8, pad_row = 8 }

check(lvgl.ICON.INFO ~= nil, "shared icon INFO")
check(lvgl.ICON.MEMORY ~= nil, "shared icon MEMORY")
check(lvgl.ICON.HARD_DISK ~= nil, "shared icon HARD_DISK")
check(lvgl.TABS_MODE.FIXED ~= nil, "tabs mode FIXED")
check(lvgl.STATUSLINE_LEVEL.WARNING ~= nil, "statusline level WARNING")

local statusline = lvgl.Statusline(root, {
    text = "All good",
    level = lvgl.STATUSLINE_LEVEL.OK,
})
statusline:set_text("Needs attention")
statusline:set_level(lvgl.STATUSLINE_LEVEL.WARNING)
statusline:set_level(lvgl.STATUSLINE_LEVEL.ERROR)
statusline:set_level(lvgl.STATUSLINE_LEVEL.OK)

local tabs = lvgl.Tabs(root, { mode = lvgl.TABS_MODE.FIXED })
tabs:add_tab("One")
tabs:add_tab_with_icon(lvgl.ICON.MEMORY, "Memory")
tabs:add_tab_with_icon(lvgl.ICON.HARD_DISK, "Storage")

check(tabs:selected() == 1, "first tab selected")
tabs:set_selected(2)
check(tabs:selected() == 2, "set_selected")
tabs:set_tab_text(1, "First")
tabs:set_tab_badge(2, true)
tabs:set_tab_badge(2, false)

-- Hiding the selected tab selects the first visible one, without an event.
tabs:set_tab_hidden(2, true)
check(tabs:selected() == 1, "hidden selected tab falls back")
tabs:set_tab_hidden(2, false)

tabs:set_mode(lvgl.TABS_MODE.WRAP)
tabs:set_mode(lvgl.TABS_MODE.SCROLLABLE)
tabs:set_mode(lvgl.TABS_MODE.FIXED)

statusline:set_text("tabs/statusline: PASS (" .. checks .. " checks)")
result = "tabs/statusline: PASS (" .. checks .. " checks)"
