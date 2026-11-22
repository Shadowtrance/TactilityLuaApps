-- Teardown regression for the old "Load access fault" crash.
--
-- The retired Lua editor created a text area, went back to its list (which
-- cleaned the text area) and then closed the app. This script recreates that
-- sequence and leaves the app open: close it (or start another app) and the
-- device must stay up. The runner logs "LuaRunner: stopped" without a panic.

local content

function on_show()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN, pad_all = 8, pad_row = 6 }

    content = lvgl.Object(screen, {
        w = lvgl.PCT(100),
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        pad_row = 6,
    })

    -- A widget deleted before close leaves a stale address in the runtime. Detach must
    -- not ask LVGL to validate freed memory: lv_obj_is_valid() is a deprecated alias for
    -- lv_obj_is_in_widget_tree(), which walks the pointer and faults.
    local doomed = lvgl.Object(screen, { w = 40, h = 40 })
    doomed:delete()
    assert(not doomed:is_valid(), "deleted widget must be invalid")

    lvgl.Label(content, { text = "Teardown test" })
    lvgl.TextArea(content, { w = lvgl.PCT(100), h = 100, text = "first editor" })

    -- The old editor's Back: delete the text area, rebuild the view.
    content:clean()
    lvgl.TextArea(content, { w = lvgl.PCT(100), h = 100, text = "second editor" })
    lvgl.Label(screen, { text = "Close this app; it must not crash." })

    result = "teardown test armed"
end

function on_destroy()
    -- Also delete the remaining widget tree while Lua is still alive.
    if content and content:is_valid() then
        content:clean()
    end
end
