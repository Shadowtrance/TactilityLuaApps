-- Notification capability smoke test. Cleans up the notification it creates.

assert(tactility.notification ~= nil, "tactility.notification is missing")
assert(tactility.notification.SEVERITY.ERROR == 2, "unexpected severity constants")

local message = "Lua notification test"
local id, reason = tactility.notification.send{
    message = message,
    title = "Lua test",
    severity = tactility.notification.SEVERITY.ERROR,
    source = "lua.notification.test",
}
assert(type(id) == "number", reason or "send returned no id")

local notification = tactility.notification.get(id)
assert(notification ~= nil, "get() did not return the notification")
assert(notification.id == id, "wrong notification id")
assert(notification.message == message, "wrong message")
assert(notification.title == "Lua test", "wrong title")
assert(notification.severity == "error", "wrong severity")
assert(notification.source_id == "lua.notification.test", "wrong source")

assert(tactility.notification.count() >= 1, "count should include the notification")

local all = tactility.notification.list()
assert(type(all) == "table", "list() did not return a table")
local found = false
for _, item in ipairs(all) do
    if item.id == id then
        found = true
        break
    end
end
assert(found, "list() does not contain the notification")

assert(tactility.notification.dismiss(id), "dismiss() failed")
local missing, missing_reason = tactility.notification.get(id)
assert(missing == nil, "dismissed notification is still readable")
assert(type(missing_reason) == "string", "missing notification should return a reason")

result = string.format("notification PASS: id %d, %d in history", id, tactility.notification.count())
