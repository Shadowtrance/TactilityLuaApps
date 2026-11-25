-- App manager: list apps, check what is running and launch an app.

assert(tactility.app ~= nil, "tactility.app is missing")

local apps = tactility.app.list()
assert(type(apps) == "table", "list() should return a table")
assert(#apps > 0, "no apps were listed")

local found_sweep = false
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
end
assert(found_sweep, "the sweep app is not listed")

assert(tactility.app.is_running("tactility.lua.sweep") == true, "the sweep app should be running")
assert(tactility.app.is_running("definitely.not.an.app") == false, "unknown app is not running")

local current = tactility.app.current()
assert(type(current) == "string" and #current > 0, "current() should return an app id")

local id, reason = tactility.app.start("definitely.not.an.app")
assert(id == nil and type(reason) == "string", "starting an unknown app should fail")

result = string.format("app PASS: %d apps, current %s", #apps, current)
