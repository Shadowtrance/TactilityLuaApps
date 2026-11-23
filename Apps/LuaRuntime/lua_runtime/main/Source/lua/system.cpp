// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <tactility/memory.h>
#include <tactility/module.h>

#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

extern "C" {
#include <lauxlib.h>
}

namespace {

int push_failure(lua_State* state, const char* reason) {
    lua_pushnil(state);
    lua_pushstring(state, reason);
    return 2;
}

int push_usage(lua_State* state, size_t total, size_t free) {
    const size_t used = total > free ? total - free : 0;
    const int percent = total > 0 ? static_cast<int>((used * 100 + total / 2) / total) : 0;

    lua_createtable(state, 0, 4);
    lua_pushinteger(state, static_cast<lua_Integer>(free));
    lua_setfield(state, -2, "free");
    lua_pushinteger(state, static_cast<lua_Integer>(total));
    lua_setfield(state, -2, "total");
    lua_pushinteger(state, static_cast<lua_Integer>(used));
    lua_setfield(state, -2, "used");
    lua_pushinteger(state, percent);
    lua_setfield(state, -2, "percent");
    return 1;
}

/**
 * `system.memory() -> { internal = usage, external = usage }`
 *
 * External is all zeroes on a board without PSRAM, so a UI can still show the
 * card and report "not available" instead of branching on a capability query.
 */
int system_memory(lua_State* state) {
    lua_createtable(state, 0, 2);

    push_usage(state, memory_heap_total(), memory_heap_free());
    lua_setfield(state, -2, "internal");

    push_usage(state, memory_external_total(), memory_external_free());
    lua_setfield(state, -2, "external");

    return 1;
}

/** `system.storage(path?) -> { total, free, used, percent }`, defaulting to /sdcard. */
int system_storage(lua_State* state) {
    const char* path = luaL_optstring(state, 1, "/sdcard");

    // Resolved by name so a firmware without the export still loads the runtime. The
    // capability starts working as soon as the symbol appears in the global table.
    using FatInfoFunction = int (*)(const char*, uint64_t*, uint64_t*);
    uintptr_t address = 0;
    if (!module_resolve_symbol_global("esp_vfs_fat_info", &address) || address == 0) {
        return push_failure(state, "storage info is not exported by this firmware");
    }
    auto* fat_info = reinterpret_cast<FatInfoFunction>(address);

    uint64_t total = 0;
    uint64_t free = 0;
    if (fat_info(path, &total, &free) != 0) {
        return push_failure(state, "storage info unavailable");
    }

    const uint64_t used = total > free ? total - free : 0;

    lua_createtable(state, 0, 4);
    lua_pushinteger(state, static_cast<lua_Integer>(total));
    lua_setfield(state, -2, "total");
    lua_pushinteger(state, static_cast<lua_Integer>(free));
    lua_setfield(state, -2, "free");
    lua_pushinteger(state, static_cast<lua_Integer>(used));
    lua_setfield(state, -2, "used");
    lua_pushinteger(state, total > 0 ? static_cast<lua_Integer>((used * 100 + total / 2) / total) : 0);
    lua_setfield(state, -2, "percent");
    return 1;
}

#ifdef ESP_PLATFORM
const char* task_state_name(eTaskState state) {
    switch (state) {
        case eRunning: return "running";
        case eReady: return "ready";
        case eBlocked: return "blocked";
        case eSuspended: return "suspended";
        case eDeleted: return "deleted";
        default: return "invalid";
    }
}
#endif

/** `system.tasks() -> { { name, state, priority, ... }, ... }` */
int system_tasks(lua_State* state) {
#ifdef ESP_PLATFORM
    const UBaseType_t count = uxTaskGetNumberOfTasks();
    if (count == 0) {
        lua_newtable(state);
        return 1;
    }

    auto* tasks = static_cast<TaskStatus_t*>(malloc(sizeof(TaskStatus_t) * count));
    if (tasks == nullptr) {
        return push_failure(state, "out of memory for the task list");
    }

    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    const UBaseType_t actual = uxTaskGetSystemState(tasks, count, &total_runtime);

    lua_createtable(state, static_cast<int>(actual), 0);
    for (UBaseType_t index = 0; index < actual; index++) {
        const TaskStatus_t& task = tasks[index];

        lua_createtable(state, 0, 7);
        lua_pushstring(state, task.pcTaskName != nullptr ? task.pcTaskName : "(unnamed)");
        lua_setfield(state, -2, "name");
        lua_pushstring(state, task_state_name(task.eCurrentState));
        lua_setfield(state, -2, "state");
        lua_pushinteger(state, static_cast<lua_Integer>(task.uxCurrentPriority));
        lua_setfield(state, -2, "priority");
        lua_pushinteger(state, static_cast<lua_Integer>(task.uxBasePriority));
        lua_setfield(state, -2, "base_priority");
        lua_pushinteger(state, static_cast<lua_Integer>(task.usStackHighWaterMark));
        lua_setfield(state, -2, "stack_free");
        lua_pushinteger(state, static_cast<lua_Integer>(task.ulRunTimeCounter));
        lua_setfield(state, -2, "runtime");
        lua_pushinteger(state, static_cast<lua_Integer>(task.xTaskNumber));
        lua_setfield(state, -2, "number");
        lua_seti(state, -2, static_cast<lua_Integer>(index) + 1);
    }

    free(tasks);
    return 1;
#else
    return push_failure(state, "task info is not available on this platform");
#endif
}

const luaL_Reg system_functions[] = {
    { "memory", system_memory },
    { "storage", system_storage },
    { "tasks", system_tasks },
    { nullptr, nullptr }
};

} // namespace

void lua_bindings_open_system(lua_State* state) {
    luaL_newlib(state, system_functions);
    lua_setfield(state, -2, "system"); // tactility.system
}
