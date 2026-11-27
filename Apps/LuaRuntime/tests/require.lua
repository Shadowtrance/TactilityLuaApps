-- Multi-file scripts: require() resolves modules next to the entry script.
--
-- The test writes its modules into the installed package, so it also proves that
-- the search path is the script's own directory rather than the process cwd.

local root = ...
assert(type(root) == "string" and #root > 0, "install directory")

-- Packaged apps keep main.lua under lua/; loose scripts already are in their own
-- directory. `...` is the install directory for a package and the script directory
-- for a loose script.
local lua_dir = root
if tactility.fs.is_file(root .. "/lua/main.lua") then
    lua_dir = root .. "/lua"
end
assert(tactility.fs.is_directory(lua_dir), "module directory: " .. lua_dir)

assert(type(package) == "table", "package library")
assert(package.path:find(lua_dir, 1, true), "package.path should contain " .. lua_dir)
assert(package.cpath == "", "C modules should be disabled")

local function write(name, source)
    local path = lua_dir .. "/" .. name
    assert(tactility.fs.write(path, source), "write " .. path)
end

write("require_test_a.lua", [[
    return { answer = 42, label = "a" }
]])

write("require_test_b.lua", [[
    local a = require("require_test_a")
    return { answer = a.answer + 1 }
]])

assert(tactility.fs.make_directory(lua_dir .. "/require_test_sub"), "make subdirectory")
write("require_test_sub/leaf.lua", [[
    return { name = "leaf" }
]])

write("require_test_cycle_a.lua", [[
    return require("require_test_cycle_b")
]])
write("require_test_cycle_b.lua", [[
    return require("require_test_cycle_a")
]])

local a = require("require_test_a")
assert(type(a) == "table" and a.answer == 42, "flat module")
assert(require("require_test_a") == a, "modules are cached")

local b = require("require_test_b")
assert(b.answer == 43, "nested require")

local leaf = require("require_test_sub.leaf")
assert(leaf.name == "leaf", "dotted subdirectory module")

local missing_ok, missing_error = pcall(require, "require_test_missing")
assert(not missing_ok, "a missing module should fail")
assert(tostring(missing_error):find("require_test_missing", 1, true), "error names the module")

local cycle_ok = pcall(require, "require_test_cycle_a")
assert(not cycle_ok, "a require cycle should fail")

tactility.fs.remove(lua_dir .. "/require_test_sub/leaf.lua")
tactility.fs.remove(lua_dir .. "/require_test_sub")
tactility.fs.remove(lua_dir .. "/require_test_a.lua")
tactility.fs.remove(lua_dir .. "/require_test_b.lua")
tactility.fs.remove(lua_dir .. "/require_test_cycle_a.lua")
tactility.fs.remove(lua_dir .. "/require_test_cycle_b.lua")

result = "require PASS: flat, nested, dotted, cache, missing, cycle"
