-- System capability: heap and storage usage.
local checks = 0

local function check(condition, message)
    checks = checks + 1
    if not condition then
        error("system: " .. (message or ("check " .. checks .. " failed")), 2)
    end
end

local system = tactility.system
check(system ~= nil, "module")

local memory = system.memory()
check(type(memory) == "table", "memory table")
check(type(memory.internal) == "table", "internal memory")
check(type(memory.external) == "table", "external memory")
for _, heap in ipairs({ memory.internal, memory.external }) do
    check(type(heap.free) == "number", "free")
    check(type(heap.total) == "number", "total")
    check(type(heap.used) == "number", "used")
    check(type(heap.percent) == "number" and heap.percent >= 0 and heap.percent <= 100, "percent")
    check(heap.free + heap.used == heap.total, "free + used = total")
end
check(memory.internal.total > 0, "internal total")
check(memory.internal.free <= memory.internal.total, "internal free <= total")

local storage, storage_reason = system.storage("/sdcard")
local storage_note = "storage"
if storage ~= nil then
    check(type(storage.total) == "number" and storage.total > 0, "storage total")
    check(type(storage.free) == "number" and storage.free <= storage.total, "storage free")
    check(type(storage.used) == "number" and storage.used <= storage.total, "storage used")
    check(type(storage.percent) == "number" and storage.percent >= 0 and storage.percent <= 100, "storage percent")
else
    check(type(storage_reason) == "string" and #storage_reason > 0, "storage reason")
    storage_note = "no storage: " .. storage_reason
end

result = "system: PASS (" .. checks .. " checks) - " .. storage_note
