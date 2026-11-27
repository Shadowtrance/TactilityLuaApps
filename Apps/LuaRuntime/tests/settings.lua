-- Settings store: multiple keys, replacement, defaults and removal.
--
-- Multiple writes in one namespace are deliberate: an earlier store implementation
-- renamed over an existing file, which FAT refused, so only the first key survived.

local name_space = "lua.test.settings"
local settings = tactility.settings

settings.clear(name_space)

local function set(key, value)
    local ok, reason = settings.set(name_space, key, value)
    assert(ok == true, key .. ": " .. tostring(reason))
end

set("one", "1")
set("two", "2")
set("three", "3")

assert(settings.get(name_space, "one") == "1", "first key")
assert(settings.get(name_space, "two") == "2", "second key")
assert(settings.get(name_space, "three") == "3", "third key")

set("two", "two-updated")
assert(settings.get(name_space, "two") == "two-updated", "replace")

local keys = settings.keys(name_space)
assert(type(keys) == "table", "keys")
local found = {}
for _, key in ipairs(keys) do
    found[key] = true
end
assert(found.one and found.two and found.three, "every key is listed")

assert(settings.has(name_space, "one") == true, "has")
assert(settings.remove(name_space, "two") == true, "remove")
assert(settings.has(name_space, "two") == false, "removed key is gone")
assert(settings.get(name_space, "two", "fallback") == "fallback", "default after removal")

settings.clear(name_space)
assert(settings.get(name_space, "one", "gone") == "gone", "clear")

result = "settings PASS: multi-key round trip, replace, keys, remove"
