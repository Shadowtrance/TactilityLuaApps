// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <tactility/error.h>
#include <tactility/filesystem/file_system.h>
#include <tactility/memory.h>

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

int push_usage(lua_State* state, uint64_t total, uint64_t free) {
    const uint64_t used = total > free ? total - free : 0;
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

struct UsageLookup {
    const char* path;
    struct FileSystem* file_system;
};

/** Finds the mounted file system whose mount path contains the requested path. */
bool find_mount(FileSystem* file_system, void* context) {
    auto* lookup = static_cast<UsageLookup*>(context);

    char mount_path[128];
    if (file_system_get_path(file_system, mount_path, sizeof(mount_path)) != ERROR_NONE) {
        return true;
    }

    size_t length = std::strlen(mount_path);
    while (length > 1 && mount_path[length - 1] == '/') {
        mount_path[--length] = '\0';
    }
    if (length == 0) {
        return true;
    }

    // A root mount contains every path.
    if (length == 1 && mount_path[0] == '/') {
        lookup->file_system = file_system;
        return false;
    }

    if (std::strncmp(lookup->path, mount_path, length) != 0) {
        return true;
    }
    // "/sdcard2" is not inside "/sdcard".
    const char next = lookup->path[length];
    if (next != '\0' && next != '/') {
        return true;
    }

    lookup->file_system = file_system;
    return false;
}

/** `system.storage(path?) -> { total, free, used, percent }`, defaulting to /sdcard. */
int system_storage(lua_State* state) {
    const char* path = luaL_optstring(state, 1, "/sdcard");

    UsageLookup lookup = { path, nullptr };
    file_system_for_each_mounted(&lookup, find_mount);
    if (lookup.file_system == nullptr) {
        return push_failure(state, "no mounted file system for that path");
    }

    uint64_t total = 0;
    uint64_t free = 0;
    const error_t error = file_system_get_usage(lookup.file_system, &total, &free);
    if (error != ERROR_NONE) {
        return push_failure(state, error_to_string(error));
    }
    if (total == 0) {
        return push_failure(state, "storage usage unavailable");
    }

    return push_usage(state, total, free);
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
