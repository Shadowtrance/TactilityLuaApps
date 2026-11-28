-- The native dialogs: option validation and dismissing an alert from its handle.
--
-- The runner's app loop delivers the callback with the LVGL lock held, shortly
-- after the dialog task returns. The harness timer reports the final result.

local ui = tactility.ui
assert(type(ui) == "table", "tactility.ui")
assert(type(ui.alert) == "function", "ui.alert")
assert(type(ui.input) == "function", "ui.input")
assert(type(ui.select) == "function", "ui.select")
assert(type(ui.dismiss) == "function", "ui.dismiss")

local callback = function() end

local handle, reason = ui.select({ title = "Empty", items = {} }, callback)
assert(handle == nil and reason ~= nil, "empty items must be rejected")

handle, reason = ui.select({ title = "Mismatch", items = { "A" }, icons = { "x", "y" } }, callback)
assert(handle == nil and reason ~= nil, "mismatched icons must be rejected")

handle, reason = ui.input({ title = "Bad callback" }, "not a function")
assert(handle == nil and reason ~= nil, "non-function callback must be rejected")

local delivered = false
handle = ui.alert({ title = "Dialog test", message = "This closes itself", buttons = { "OK" } },
    function(index, why)
        assert(index == nil, "a dismissed alert has no index")
        assert(why == "cancelled", "dismiss reason: " .. tostring(why))
        delivered = true
        result = "dialogs PASS: alert dismissed"
    end)
assert(handle ~= nil, "alert did not return a handle")
assert(handle:is_active(), "alert should be active before the loop runs")
handle:dismiss()

result = "dialogs started"
