-- App manager: list apps, check what is running and launch an app.

assert(tactility.app ~= nil, "tactility.app is missing")

local apps = tactility.app.list()
assert(type(apps) == "table", "list() should return a table")
assert(#apps > 0, "no apps were listed")

local found_sweep = false
local found_selection = false
for _, app in ipairs(apps) do
    assert(type(app.id) == "string" and #app.id > 0, "app id")
    assert(type(app.name) == "string", "app name")
    assert(
        app.category == "system" or app.category == "settings" or app.category == "user",
        "app category"
    )
    assert(type(app.hidden) == "boolean", "app hidden flag")
    if app.id == "tactility.lua.sweep" then
        found_sweep = true
    end
    if app.id == "tactility.fileselection" then
        found_selection = true
    end
end
assert(found_sweep, "the sweep app is not listed")
assert(found_selection, "the file selection app is not listed")

assert(tactility.app.is_running("tactility.lua.sweep") == true, "the sweep app should be running")
assert(tactility.app.is_running("definitely.not.an.app") == false, "unknown app is not running")

local current = tactility.app.current()
assert(type(current) == "string" and #current > 0, "current() should return an app id")

local id, reason = tactility.app.start("definitely.not.an.app")
assert(id == nil and type(reason) == "string", "starting an unknown app should fail")

local stopped, stop_reason = tactility.app.stop("definitely.not.an.app")
assert(stopped == nil and type(stop_reason) == "string", "stopping an unknown app should fail")

-- Stopping the app that runs the script is refused.
local self_stopped, self_stop_reason = tactility.app.stop("tactility.lua.sweep")
assert(
    self_stopped == nil and self_stop_reason == "cannot stop the current app",
    "stopping the current app should be refused"
)

result = string.format("app PASS: %d apps, current %s, stop guard", #apps, current)
