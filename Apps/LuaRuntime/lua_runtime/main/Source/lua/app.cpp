// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <app/install.h>
#include <app/manager.h>
#include <app/scheduler.h>
#include <app/start.h>

#include <tactility/error.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

extern "C" {
#include <lauxlib.h>
}

namespace {

constexpr int MAX_ARGUMENTS = 16;

int push_failure(lua_State* state, const char* reason) {
    lua_pushnil(state);
    lua_pushstring(state, reason);
    return 2;
}

const char* category_name(enum AppCategory category) {
    switch (category) {
        case APP_CATEGORY_SYSTEM: return "system";
        case APP_CATEGORY_SETTINGS: return "settings";
        default: return "user";
    }
}

struct ManifestRow {
    AppId id;
    char name[APP_MANIFEST_NAME_LENGTH + 1];
    enum AppCategory category;
    bool hidden;
};

struct ManifestCount {
    size_t count;
};

/** The visitor runs with the app ledger locked: no Lua allocation and no app_manager_*(). */
void count_manifest(const struct AppManifest* manifest, void* context) {
    (void)manifest;
    static_cast<ManifestCount*>(context)->count++;
}

struct ManifestCopy {
    ManifestRow* rows;
    size_t capacity;
    size_t count;
};

void copy_manifest(const struct AppManifest* manifest, void* context) {
    auto* copy = static_cast<ManifestCopy*>(context);
    if (copy->count >= copy->capacity) {
        return;
    }

    auto& row = copy->rows[copy->count++];
    std::memcpy(row.id, manifest->id, sizeof(row.id));
    std::snprintf(row.name, sizeof(row.name), "%s", manifest->name);
    row.category = manifest->category;
    row.hidden = (manifest->flags & APP_MANIFEST_FLAG_HIDDEN) != 0;
}

} // namespace

/** `tactility.app.list()` - every registered app, hidden ones included. */
int app_list(lua_State* state) {
    ManifestCount counter = {};
    app_manager_for_each_manifest(count_manifest, &counter);
    if (counter.count == 0) {
        lua_newtable(state);
        return 1;
    }

    // Heap, not the app task's small stack: an AppManifest row is ~70 bytes.
    auto* rows = static_cast<ManifestRow*>(std::malloc(counter.count * sizeof(ManifestRow)));
    if (rows == nullptr) {
        return push_failure(state, "out of memory");
    }

    ManifestCopy copy = { rows, counter.count, 0 };
    app_manager_for_each_manifest(copy_manifest, &copy);

    lua_createtable(state, static_cast<int>(copy.count), 0);
    for (size_t index = 0; index < copy.count; index++) {
        const auto& row = rows[index];

        lua_createtable(state, 0, 4);
        lua_pushstring(state, row.id);
        lua_setfield(state, -2, "id");
        lua_pushstring(state, row.name);
        lua_setfield(state, -2, "name");
        lua_pushstring(state, category_name(row.category));
        lua_setfield(state, -2, "category");
        lua_pushboolean(state, row.hidden);
        lua_setfield(state, -2, "hidden");
        lua_seti(state, -2, static_cast<lua_Integer>(index) + 1);
    }

    std::free(rows);
    return 1;
}

/** `tactility.app.is_running(id)` */
int app_is_running(lua_State* state) {
    lua_pushboolean(state, app_manager_is_running(luaL_checkstring(state, 1)));
    return 1;
}

/** `tactility.app.current()` - the topmost active app id, or nil. */
int app_current(lua_State* state) {
    AppId id;
    if (app_manager_get_topmost_app_id(id, sizeof(id)) != ERROR_NONE) {
        return push_failure(state, "no app is active");
    }
    lua_pushstring(state, id);
    return 1;
}

/**
 * `tactility.app.start(id, args?) -> instance_id | nil, reason`
 *
 * The new app runs normally (its own task and window); this call returns as soon as
 * it is scheduled. Arguments are copied by the app module before it returns.
 */
int app_start(lua_State* state) {
    const char* id = luaL_checkstring(state, 1);

    struct AppStartContext context = {};
    error_t error = app_start_context_from_id(id, &context);
    if (error != ERROR_NONE) {
        return push_failure(state, error_to_string(error));
    }

    const char* arguments[MAX_ARGUMENTS + 1] = {};
    int argument_count = 0;
    if (!lua_isnoneornil(state, 2)) {
        luaL_checktype(state, 2, LUA_TTABLE);
        const lua_Integer count = luaL_len(state, 2);
        if (count > MAX_ARGUMENTS) {
            return luaL_error(state, "at most %d arguments are supported", MAX_ARGUMENTS);
        }
        for (lua_Integer index = 1; index <= count; index++) {
            lua_geti(state, 2, index);
            arguments[argument_count++] = luaL_checkstring(state, -1);
            lua_pop(state, 1);
        }
    }
    arguments[argument_count] = nullptr;
    if (argument_count > 0) {
        app_start_context_set_arguments(&context, arguments);
    }

    AppInstanceId instance_id = 0;
    error = app_start_with_context(&context, &instance_id);
    if (error != ERROR_NONE) {
        return push_failure(state, error_to_string(error));
    }

    lua_pushinteger(state, static_cast<lua_Integer>(instance_id));
    return 1;
}

/**
 * `tactility.app.stop(instance_id_or_id) -> true | nil, reason`
 *
 * Accepts the instance id returned by start() or an app id, in which case the most
 * recently started running instance is stopped. Refuses to stop the app that runs the script.
 */
int app_stop(lua_State* state) {
    AppInstanceId instance_id = 0;
    if (lua_type(state, 1) == LUA_TSTRING) {
        const char* id = luaL_checkstring(state, 1);
        const error_t error = app_manager_find_running_instance(id, &instance_id);
        if (error != ERROR_NONE) {
            return push_failure(state, error_to_string(error));
        }
    } else {
        const lua_Integer value = luaL_checkinteger(state, 1);
        if (value <= 0 || static_cast<uint64_t>(value) > UINT32_MAX) {
            return push_failure(state, "invalid instance id");
        }
        instance_id = static_cast<AppInstanceId>(value);
    }

    if (instance_id == app_scheduler_current_app_id()) {
        return push_failure(state, "cannot stop the current app");
    }

    const error_t error = app_manager_stop(instance_id);
    if (error != ERROR_NONE) {
        return push_failure(state, error_to_string(error));
    }

    lua_pushboolean(state, true);
    return 1;
}

/** `tactility.app.install(path) -> true | nil, reason` - installs a .app package. */
int app_install_package(lua_State* state) {
    const char* path = luaL_checkstring(state, 1);
    const error_t error = ::app_install(path);
    if (error != ERROR_NONE) {
        return push_failure(state, error_to_string(error));
    }
    lua_pushboolean(state, true);
    return 1;
}

/** `tactility.app.uninstall(package_id) -> true | nil, reason` */
int app_uninstall_package(lua_State* state) {
    const char* id = luaL_checkstring(state, 1);
    if (std::strcmp(id, "tactility.luaruntime") == 0) {
        return push_failure(state, "cannot uninstall the running Lua runtime");
    }
    const error_t error = ::app_uninstall(id);
    if (error != ERROR_NONE) {
        return push_failure(state, error_to_string(error));
    }
    lua_pushboolean(state, true);
    return 1;
}

namespace {

const luaL_Reg app_functions[] = {
    { "list", app_list },
    { "is_running", app_is_running },
    { "current", app_current },
    { "start", app_start },
    { "stop", app_stop },
    { "install", app_install_package },
    { "uninstall", app_uninstall_package },
    { nullptr, nullptr }
};

} // namespace

void lua_bindings_open_app(lua_State* state) {
    luaL_newlib(state, app_functions);
    lua_setfield(state, -2, "app");
}
