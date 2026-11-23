// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <tactility/memory.h>
#include <tactility/module.h>

#include <cstring>

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

const luaL_Reg system_functions[] = {
    { "memory", system_memory },
    { "storage", system_storage },
    { nullptr, nullptr }
};

} // namespace

void lua_bindings_open_system(lua_State* state) {
    luaL_newlib(state, system_functions);
    lua_setfield(state, -2, "system"); // tactility.system
}
