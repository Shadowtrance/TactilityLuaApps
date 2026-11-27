-- Filesystem bindings and the picker's option validation.
--
-- The picker itself needs a person to choose something, so this only covers the
-- paths that fail before a window opens. Run the selection flow by hand.

local install_directory = ...
assert(type(install_directory) == "string" and #install_directory > 0, "install directory")

local path = install_directory .. "/fs_test.txt"
local content = "hello\nfrom tactility.fs"

assert(tactility.fs.write(path, content) == true, "write")
assert(tactility.fs.exists(path) == true, "exists after write")
assert(tactility.fs.is_file(path) == true, "is_file")
assert(tactility.fs.is_directory(path) == false, "a file is not a directory")
assert(tactility.fs.size(path) == #content, "size")
assert(tactility.fs.read(path) == content, "read")
assert(tactility.fs.append(path, "!") == true, "append")
assert(tactility.fs.read(path) == content .. "!", "read after append")

local entries = tactility.fs.list(install_directory)
assert(type(entries) == "table", "list returns a table")
local found = false
for _, entry in ipairs(entries) do
    assert(type(entry.name) == "string", "entry name")
    assert(type(entry.directory) == "boolean", "entry directory flag")
    if entry.name == "fs_test.txt" then
        found = true
    end
end
assert(found, "the test file is listed")

assert(tactility.fs.remove(path) == true, "remove")
assert(tactility.fs.exists(path) == false, "gone after remove")

local mounts = tactility.fs.mounts()
assert(type(mounts) == "table" and #mounts > 0, "at least one mount")

-- These fail while reading the options, before the selection window opens.
local ignored = function() end
local picked, reason = tactility.fs.pick_file({ extensions = "txt" }, ignored)
assert(picked == nil and reason == "extensions must be a table", tostring(reason))

local too_many = { "a", "b", "c", "d", "e", "f", "g", "h", "i" }
picked, reason = tactility.fs.pick_files({ extensions = too_many }, ignored)
assert(picked == nil and reason == "too many extensions", tostring(reason))

result = string.format("fs PASS: %d mounts, picker option errors", #mounts)
