-- Asynchronous file picker: open, cancel, and receive the callback.
--
-- The happy path needs a person to pick a path. This covers registration, the
-- handle, cancellation, the callback and the request release.

local screen = lvgl.root()
local status = lvgl.Label(screen, { text = "Opening the picker", w = lvgl.PCT(100) })

local picker
picker = tactility.fs.pick_folder(function(path, reason)
    assert(path == nil, "cancel should not return a path")
    assert(reason == "cancelled", "cancel reason: " .. tostring(reason))
    status:set{ text = "Cancelled" }
    result = "picker PASS: cancelled"
    tactility.log.info("PICKRESULT", result)
end)

assert(picker ~= nil, "pick_folder did not return a handle")
assert(picker:is_active() == true, "the picker should be active")

function on_show()
    assert(picker:cancel() == true, "cancel")
end
