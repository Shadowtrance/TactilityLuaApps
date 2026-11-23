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

local tasks = system.tasks()
check(type(tasks) == "table", "task table")
check(#tasks > 0, "task count")
for _, task in ipairs(tasks) do
    check(type(task.name) == "string" and #task.name > 0, "task name")
    check(type(task.state) == "string" and #task.state > 0, "task state")
    check(type(task.priority) == "number", "task priority")
    check(type(task.base_priority) == "number", "task base priority")
    check(type(task.stack_free) == "number", "task stack")
    check(type(task.runtime) == "number", "task runtime")
    check(type(task.number) == "number", "task number")
end

result = string.format(
    "system: PASS (%d checks) - %s, %d tasks",
    checks,
    storage_note,
    #tasks
)
