// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

#include <lua/runtime.h>

#include <app/scheduler.h>

#include <lvgl_window_manager/dialog.h>

#include <tactility/error.h>
#include <tactility/log.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

extern "C" {
#include <lauxlib.h>
}

/**
 * `tactility.fs.pick_file/pick_files/pick_folder`
 *
 * Shows the window manager's own file dialog. Unlike the old implementation this does
 * not start another app: the dialog is modal over the runner's window, so there is no
 * child app to start, stop or stream a result from. The dialog runs its own task and
 * calls back without the LVGL lock; this file only copies the paths there. The runner's
 * app loop then delivers the Lua callback with the LVGL lock held, so the callback can
 * create widgets.
 *
 * The handle is pinned in the registry until the request finishes, so a script that
 * ignores the return value still gets its callback.
 */

namespace {

constexpr auto* TAG = "luavgl.picker";
constexpr auto* PICKER_METATABLE = "tactility.fs.Picker";

constexpr int MAX_EXTENSIONS = 8;
constexpr size_t EXTENSION_LENGTH = 48;
constexpr size_t MAX_RESULT_PATHS = 64;

/** Registry key for the live pickers, so shutdown can cancel them all. */
char PICKERS_KEY = 0;

enum class PickerMode {
    File,
    Files,
    Folder,
};

struct PickerRequest {
    lua_State* state = nullptr;
    int callback_ref = LUA_NOREF;
    int pin_ref = LUA_NOREF;
    PickerMode mode = PickerMode::File;
    AppInstanceId parent = 0;

    std::vector<std::string> extensions;
    std::string path;
    std::string name;
    bool allow_new = false;

    bool start_requested = true;
    bool dialog_started = false;
    bool dismissed = false;
    bool finished = false;
    bool abandoned = false;
    std::atomic<bool> cancel_requested{false};
    std::atomic<bool> result_ready{false};

    /** Guards the fields below: the result arrives on the dialog's task. */
    std::mutex mutex;
    bool result_dismissed = false;
    std::vector<std::string> result_paths;
};

struct PickerHandle {
    PickerRequest* request;
};

void push_pickers(lua_State* state) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, &PICKERS_KEY);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &PICKERS_KEY);
    }
}

int push_failure(lua_State* state, const char* message) {
    lua_pushnil(state);
    lua_pushstring(state, message);
    return 2;
}

bool read_string_field(lua_State* state, int table_index, const char* key, std::string& value) {
    lua_getfield(state, table_index, key);
    const bool is_string = lua_isstring(state, -1) != 0;
    if (is_string) {
        size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        value.assign(text, length);
    }
    lua_pop(state, 1);
    return is_string;
}

const char* parse_options(lua_State* state, int index, PickerMode mode, PickerRequest& request) {
    // A missing options table is index 0. Lua's release build does not validate
    // that index, so it must never reach the API.
    if (index <= 0 || lua_isnoneornil(state, index)) {
        return nullptr;
    }
    if (!lua_istable(state, index)) {
        return "options must be a table";
    }

    if (mode != PickerMode::Folder) {
        lua_getfield(state, index, "allow_new");
        request.allow_new = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
    }

    read_string_field(state, index, "path", request.path);
    read_string_field(state, index, "name", request.name);

    lua_getfield(state, index, "extensions");
    if (!lua_isnil(state, -1)) {
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            return "extensions must be a table";
        }

        const lua_Integer count = luaL_len(state, -1);
        if (count > MAX_EXTENSIONS) {
            lua_pop(state, 1);
            return "too many extensions";
        }

        for (lua_Integer extension_index = 1; extension_index <= count; extension_index++) {
            lua_geti(state, -1, extension_index);
            const char* extension = luaL_checkstring(state, -1);
            if (std::strlen(extension) >= EXTENSION_LENGTH) {
                lua_pop(state, 2);
                return "extension too long";
            }
            request.extensions.emplace_back(extension);
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    return nullptr;
}

/** Removes the request from the live table and releases its Lua references. */
void release_request(PickerRequest* request) {
    if (request->finished) {
        return;
    }
    request->finished = true;

    lua_State* state = request->state;
    if (state == nullptr) {
        return;
    }

    if (request->callback_ref != LUA_NOREF) {
        luaL_unref(state, LUA_REGISTRYINDEX, request->callback_ref);
        request->callback_ref = LUA_NOREF;
    }
    if (request->pin_ref != LUA_NOREF) {
        luaL_unref(state, LUA_REGISTRYINDEX, request->pin_ref);
        request->pin_ref = LUA_NOREF;
    }

    push_pickers(state);
    lua_pushlightuserdata(state, request);
    lua_pushnil(state);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

/**
 * Called when the runtime closes with a picker still open. The request is left
 * allocated: the dialog task may still be about to call the result function, and
 * window_manager_remove() waits for a running result function. Its data is not
 * reachable from Lua anymore, so the leak is bounded by the app's lifetime.
 */
void abandon_request(PickerRequest* request) {
    if (request->finished) {
        return;
    }
    request->finished = true;
    request->abandoned = true;
}

void deliver_result(PickerRequest* request) {
    if (request->finished) {
        return;
    }

    std::vector<std::string> paths;
    bool dismissed = false;
    {
        std::lock_guard<std::mutex> lock(request->mutex);
        paths = request->result_paths;
        dismissed = request->result_dismissed;
    }

    lua_State* state = request->state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, request->callback_ref);
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        release_request(request);
        return;
    }

    const bool cancelled = request->cancel_requested.load() || dismissed || paths.empty();
    if (cancelled) {
        lua_pushnil(state);
        lua_pushstring(state, "cancelled");
        if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
            LOG_E(TAG, "picker callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    } else if (request->mode == PickerMode::Files) {
        lua_createtable(state, static_cast<int>(paths.size()), 0);
        for (size_t index = 0; index < paths.size(); index++) {
            lua_pushlstring(state, paths[index].data(), paths[index].size());
            lua_seti(state, -2, static_cast<lua_Integer>(index + 1));
        }
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            LOG_E(TAG, "picker callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    } else {
        lua_pushlstring(state, paths[0].data(), paths[0].size());
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            LOG_E(TAG, "picker callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    }

    release_request(request);
}

void deliver_failure(PickerRequest* request, const char* reason) {
    if (request->finished) {
        return;
    }

    lua_State* state = request->state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, request->callback_ref);
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        release_request(request);
        return;
    }

    lua_pushnil(state);
    lua_pushstring(state, reason);
    if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
        LOG_E(TAG, "picker callback: %s", lua_tostring(state, -1));
        lua_pop(state, 1);
    }

    release_request(request);
}

int picker_cancel(lua_State* state) {
    auto* handle = static_cast<PickerHandle*>(luaL_checkudata(state, 1, PICKER_METATABLE));
    if (handle->request != nullptr && !handle->request->finished) {
        // The runner's loop dismisses the dialog and delivers the callback. This can be
        // called from an LVGL callback, where touching the dialog is unsafe.
        handle->request->cancel_requested.store(true);
    }
    lua_pushboolean(state, 1);
    return 1;
}

int picker_is_active(lua_State* state) {
    auto* handle = static_cast<PickerHandle*>(luaL_checkudata(state, 1, PICKER_METATABLE));
    lua_pushboolean(state, handle->request != nullptr && !handle->request->finished);
    return 1;
}

int picker_gc(lua_State* state) {
    auto* handle = static_cast<PickerHandle*>(luaL_checkudata(state, 1, PICKER_METATABLE));
    if (handle->request != nullptr) {
        if (!handle->request->abandoned) {
            release_request(handle->request);
            delete handle->request;
        }
        handle->request = nullptr;
    }
    return 0;
}

const luaL_Reg picker_methods[] = {
    { "cancel", picker_cancel },
    { "is_active", picker_is_active },
    { nullptr, nullptr }
};

/** Shared body for pick_file(), pick_files() and pick_folder(). */
int start_picker(lua_State* state, PickerMode mode) {
    int options_index = 0;
    int callback_index = 0;
    if (lua_isfunction(state, 1)) {
        callback_index = 1;
    } else {
        options_index = 1;
        luaL_checktype(state, 2, LUA_TFUNCTION);
        callback_index = 2;
    }

    const AppInstanceId parent = app_scheduler_current_app_id();
    if (parent == 0) {
        return push_failure(state, "file picking needs a running app");
    }

    auto* request = new (std::nothrow) PickerRequest();
    if (request == nullptr) {
        return push_failure(state, "out of memory");
    }
    request->state = state;
    request->parent = parent;
    request->mode = mode;

    const char* option_error = parse_options(state, options_index, mode, *request);
    if (option_error != nullptr) {
        delete request;
        return push_failure(state, option_error);
    }

    lua_pushvalue(state, callback_index);
    request->callback_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    auto* handle = static_cast<PickerHandle*>(lua_newuserdatauv(state, sizeof(PickerHandle), 0));
    handle->request = request;
    luaL_setmetatable(state, PICKER_METATABLE);

    // Pin the handle so dropping the return value does not cancel the picker.
    lua_pushvalue(state, -1);
    request->pin_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    push_pickers(state);
    lua_pushlightuserdata(state, request);
    lua_pushvalue(state, -3);
    lua_rawset(state, -3);
    lua_pop(state, 1);

    return 1;
}

/**
 * `tactility.fs.pick_file(options?, callback) -> handle | nil, reason`
 *
 * Shows the native file dialog. The callback receives the picked path, or
 * `nil, "cancelled"` when the person cancels. `options.extensions` limits the list,
 * `options.path` sets the starting directory, `options.name` prefills a save-as name
 * and `options.allow_new` permits a name that does not exist yet.
 */
int fs_pick_file(lua_State* state) {
    return start_picker(state, PickerMode::File);
}

/**
 * `tactility.fs.pick_files(options?, callback) -> handle | nil, reason`
 *
 * Like pick_file(), but the callback receives an array of paths.
 */
int fs_pick_files(lua_State* state) {
    return start_picker(state, PickerMode::Files);
}

/**
 * `tactility.fs.pick_folder(callback) -> handle | nil, reason`
 *
 * Shows the folder picker; the callback receives the folder that was shown when
 * Select was pressed.
 */
int fs_pick_folder(lua_State* state) {
    return start_picker(state, PickerMode::Folder);
}

const luaL_Reg picker_functions[] = {
    { "pick_file", fs_pick_file },
    { "pick_files", fs_pick_files },
    { "pick_folder", fs_pick_folder },
    { nullptr, nullptr }
};

void add_picker_api(lua_State* state) {
    lua_getglobal(state, "tactility");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        LOG_E(TAG, "tactility table is missing");
        return;
    }

    lua_getfield(state, -1, "fs");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 2);
        LOG_E(TAG, "tactility.fs table is missing");
        return;
    }

    luaL_setfuncs(state, picker_functions, 0);
    lua_pop(state, 2);
}

/** Called on the dialog's task, without the LVGL lock. Only copies C data. */
void on_file_dialog_result(const char* const* paths, uint32_t path_count, void* user_data) {
    auto* request = static_cast<PickerRequest*>(user_data);

    std::lock_guard<std::mutex> lock(request->mutex);
    if (request->abandoned) {
        return;
    }

    request->result_paths.clear();
    request->result_dismissed = (paths == nullptr || path_count == 0);
    if (!request->result_dismissed) {
        const uint32_t count = path_count < MAX_RESULT_PATHS
            ? path_count
            : static_cast<uint32_t>(MAX_RESULT_PATHS);
        for (uint32_t index = 0; index < count; index++) {
            if (paths[index] != nullptr) {
                request->result_paths.emplace_back(paths[index]);
            }
        }
    }
    request->result_ready.store(true);
}

error_t start_dialog(PickerRequest* request) {
    WindowFileDialogOptions options = {};
    if (request->mode == PickerMode::Folder) {
        options.flags |= WINDOW_FILE_DIALOG_FOLDER | WINDOW_FILE_DIALOG_NO_FILES;
    }
    if (request->mode == PickerMode::Files) {
        options.flags |= WINDOW_FILE_DIALOG_MULTIPLE;
    }
    if (request->allow_new) {
        options.flags |= WINDOW_FILE_DIALOG_NEW_FILE;
    }
    if (!request->path.empty()) {
        options.path = request->path.c_str();
    }
    if (!request->name.empty()) {
        options.name = request->name.c_str();
    }

    std::vector<const char*> extension_pointers;
    extension_pointers.reserve(request->extensions.size());
    for (const auto& extension : request->extensions) {
        extension_pointers.push_back(extension.c_str());
    }
    options.extensions = extension_pointers.empty() ? nullptr : extension_pointers.data();
    options.extension_count = static_cast<uint32_t>(extension_pointers.size());

    return window_manager_file_dialog(request->parent, &options, on_file_dialog_result, request);
}

} // namespace

void luavgl_open_picker(lua_State* state) {
    luaL_newmetatable(state, PICKER_METATABLE);

    luaL_newlib(state, picker_methods);
    lua_setfield(state, -2, "__index");

    lua_pushcfunction(state, picker_gc);
    lua_setfield(state, -2, "__gc");

    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");

    lua_pop(state, 1);

    add_picker_api(state);
}

extern "C" void luavgl_pickers_poll(struct LuaRuntime* runtime) {
    auto* state = static_cast<lua_State*>(lua_runtime_get_state(runtime));
    push_pickers(state);

    // Collect first, because release_request() mutates the table.
    PickerRequest* requests[8];
    size_t count = 0;

    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        lua_pop(state, 1); // the handle; the key is the request pointer
        if (count < sizeof(requests) / sizeof(requests[0])) {
            requests[count++] = static_cast<PickerRequest*>(lua_touserdata(state, -1));
        }
    }
    lua_pop(state, 1);

    for (size_t index = 0; index < count; index++) {
        PickerRequest* request = requests[index];
        if (request == nullptr || request->finished) {
            continue;
        }

        if (request->start_requested) {
            request->start_requested = false;
            if (request->cancel_requested.load()) {
                // Cancelled before the dialog appeared.
                deliver_result(request);
                continue;
            }

            const error_t error = start_dialog(request);
            if (error != ERROR_NONE) {
                LOG_E(TAG, "showing the file dialog failed: %s", error_to_string(error));
                deliver_failure(request, "file selection failed");
                continue;
            }
            request->dialog_started = true;
        }

        if (request->cancel_requested.load() && request->dialog_started && !request->dismissed) {
            window_manager_dialog_dismiss();
            request->dismissed = true;
        }

        if (request->result_ready.exchange(false)) {
            deliver_result(request);
        }
    }
}

void luavgl_delete_all_pickers(lua_State* state) {
    push_pickers(state);

    PickerRequest* requests[8];
    size_t count = 0;

    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        lua_pop(state, 1); // the handle; the key is the request pointer
        if (count < sizeof(requests) / sizeof(requests[0])) {
            requests[count++] = static_cast<PickerRequest*>(lua_touserdata(state, -1));
        }
    }
    lua_pop(state, 1);

    for (size_t index = 0; index < count; index++) {
        abandon_request(requests[index]);
    }
}
