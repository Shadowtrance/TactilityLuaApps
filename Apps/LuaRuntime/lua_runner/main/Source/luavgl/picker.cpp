// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

#include <lua/runtime.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>
#include <app/start.h>
#include <app/stream.h>

#include <tactility/concurrent/task_event_group.h>
#include <tactility/error.h>
#include <tactility/log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

extern "C" {
#include <lauxlib.h>
}

/**
 * `tactility.fs.pick_file/pick_files/pick_folder`
 *
 * Starts `tactility.fileselection` as a modal child and returns a handle at once.
 * The child's stdout is streamed into a buffer; its APP_EVENT_RESULT is processed
 * by the runner's own loop, on the app task with the LVGL lock held, so the
 * callback can create widgets. Nothing is polled from an LVGL timer: starting or
 * stopping apps from the LVGL task deadlocks against the lock that task holds.
 *
 * The handle is pinned in the registry until the request finishes, so a script
 * that ignores the return value still gets its callback.
 */namespace {

constexpr auto* TAG = "luavgl.picker";
constexpr auto* PICKER_METATABLE = "tactility.fs.Picker";
constexpr auto* FILE_SELECTION_APP_ID = "tactility.fileselection";

constexpr size_t RESULT_CAPACITY = 1024;

constexpr int MAX_EXTENSIONS = 8;
constexpr int MAX_ARGUMENTS = MAX_EXTENSIONS + 4;
constexpr size_t EXTENSION_ARGUMENT_LENGTH = 48;

/** Registry key for the live pickers, so shutdown can cancel them all. */
char PICKERS_KEY = 0;

enum class PickerMode {
    File,
    Files,
    Folder,
};

struct PickerRequest {
    lua_State* state;
    AppInstanceId parent;
    AppInstanceId child;
    AppStream stream;
    TaskEventGroup group;
    AppEventSubscription subscription;
    char buffer[RESULT_CAPACITY];
    char extensions[MAX_EXTENSIONS][EXTENSION_ARGUMENT_LENGTH];
    char max_bytes[32];
    const char* arguments[MAX_ARGUMENTS + 1];
    int argument_count;
    int callback_ref;
    int pin_ref;
    int32_t result_code;
    size_t length;
    bool start_requested;
    bool started;
    bool subscribed;
    bool bound;
    bool cancel_requested;
    bool has_result;
    bool finished;
    bool abandoned;
    PickerMode mode;

    void add_argument(const char* value) {
        if (argument_count < MAX_ARGUMENTS) {
            arguments[argument_count++] = value;
            arguments[argument_count] = nullptr;
        }
    }
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

const char* parse_options(lua_State* state, int index, PickerMode mode, PickerRequest& request) {
    bool allow_new = false;

    // A missing options table is index 0. Lua's release build does not validate
    // that index, so it must never reach the API.
    if (index > 0 && !lua_isnoneornil(state, index)) {
        if (!lua_istable(state, index)) {
            return "options must be a table";
        }

        if (mode != PickerMode::Folder) {
            lua_getfield(state, index, "allow_new");
            allow_new = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
        }

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
                const int written = std::snprintf(
                    request.extensions[extension_index - 1],
                    EXTENSION_ARGUMENT_LENGTH,
                    "--ext=%s",
                    extension
                );
                lua_pop(state, 1);
                if (written < 0 || static_cast<size_t>(written) >= EXTENSION_ARGUMENT_LENGTH) {
                    lua_pop(state, 1);
                    return "extension too long";
                }
            }
        }
        lua_pop(state, 1);

        for (int extension_index = 0; extension_index < MAX_EXTENSIONS; extension_index++) {
            if (request.extensions[extension_index][0] != '\0') {
                request.add_argument(request.extensions[extension_index]);
            }
        }
    }

    if (allow_new) {
        request.add_argument("--new");
    }
    if (mode == PickerMode::Folder) {
        request.add_argument("--folder");
        request.add_argument("--no-files");
    }
    if (mode == PickerMode::Files) {
        request.add_argument("--multiple");
    }

    std::snprintf(
        request.max_bytes,
        sizeof(request.max_bytes),
        "--max-bytes=%u",
        static_cast<unsigned>(RESULT_CAPACITY)
    );
    request.add_argument(request.max_bytes);

    return nullptr;
}

void push_paths(lua_State* state, const PickerRequest& request) {
    lua_newtable(state);
    lua_Integer index = 1;
    size_t start = 0;
    for (size_t position = 0; position <= request.length; position++) {
        if (position == request.length || request.buffer[position] == '\n') {
            const size_t item_length = position - start;
            if (item_length > 0) {
                lua_pushlstring(state, request.buffer + start, item_length);
                lua_seti(state, -2, index++);
            }
            start = position + 1;
        }
    }
}

/** Removes the request from the live table and releases every C resource it owns. */
void release_request(PickerRequest* request) {
    if (request->finished) {
        return;
    }
    request->finished = true;

    // The result means the child already stopped, so its stdout is quiescent.
    if (request->bound) {
        app_stream_unsubscribe(&request->stream);
        request->bound = false;
    }

    if (request->subscribed) {
        app_event_unsubscribe(&request->subscription);
        task_event_group_destruct(&request->group);
        request->subscribed = false;
    }

    lua_State* state = request->state;
    if (state != nullptr) {
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
}

/**
 * Called when the runtime closes with a picker still open: ask the child to close,
 * stop listening, but leave the request allocated. The child may still hold the
 * stream fd, and this can run with the LVGL lock held, where waiting for the child
 * would deadlock.
 */
void abandon_request(PickerRequest* request) {
    if (request->finished) {
        return;
    }
    request->finished = true;
    request->abandoned = true;

    if (request->child != 0 && !request->has_result) {
        app_event_emit_close(request->child);
    }

    if (request->subscribed) {
        app_event_unsubscribe(&request->subscription);
        task_event_group_destruct(&request->group);
        request->subscribed = false;
    }
}

void deliver_result(PickerRequest* request) {
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

    if (request->cancel_requested || request->result_code != 0) {
        const char* reason = request->cancel_requested
            ? "cancelled"
            : (request->result_code == 1 ? "cancelled" : "file selection failed");
        lua_pushnil(state);
        lua_pushstring(state, reason);
        if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
            LOG_E(TAG, "picker callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    } else if (request->length == 0) {
        lua_pushnil(state);
        lua_pushstring(state, "nothing was selected");
        if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
            LOG_E(TAG, "picker callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    } else if (request->mode == PickerMode::Files) {
        push_paths(state, *request);
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            LOG_E(TAG, "picker callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    } else {
        while (request->length > 0 &&
               (request->buffer[request->length - 1] == '\n' ||
                request->buffer[request->length - 1] == '\r')) {
            request->length--;
        }
        lua_pushlstring(state, request->buffer, request->length);
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            LOG_E(TAG, "picker callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    }

    release_request(request);
}

int picker_cancel(lua_State* state) {
    auto* handle = static_cast<PickerHandle*>(luaL_checkudata(state, 1, PICKER_METATABLE));
    if (handle->request != nullptr && !handle->request->finished) {
        // The runner's loop closes the child and delivers the callback. This can be
        // called from an LVGL callback, where touching the app module is unsafe.
        handle->request->cancel_requested = true;
    }
    lua_pushboolean(state, 1);
    return 1;
}int picker_is_active(lua_State* state) {
    auto* handle = static_cast<PickerHandle*>(luaL_checkudata(state, 1, PICKER_METATABLE));
    lua_pushboolean(state, handle->request != nullptr && !handle->request->finished);
    return 1;
}

int picker_gc(lua_State* state) {
    auto* handle = static_cast<PickerHandle*>(luaL_checkudata(state, 1, PICKER_METATABLE));
    if (handle->request != nullptr) {
        if (!handle->request->abandoned) {
            release_request(handle->request);
            std::free(handle->request);
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

    auto* request = static_cast<PickerRequest*>(std::calloc(1, sizeof(PickerRequest)));
    if (request == nullptr) {
        return push_failure(state, "out of memory");
    }
    request->state = state;
    request->parent = parent;
    request->callback_ref = LUA_NOREF;
    request->pin_ref = LUA_NOREF;
    request->mode = mode;

    const char* option_error = parse_options(state, options_index, mode, *request);
    if (option_error != nullptr) {
        std::free(request);
        return push_failure(state, option_error);
    }
    request->start_requested = true;

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
 * Shows a file dialog as a modal child. The callback receives the picked path, or
 * `nil, reason` when the person cancels. `options.extensions` limits the list and
 * `options.allow_new` permits a name that does not exist yet (save-as).
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

/**
 * Starts the child. Called from the runner's app loop, after the parent window
 * exists: starting it while the parent's widgets are still being created closes
 * the parent instead.
 */
error_t start_child(PickerRequest* request) {
    task_event_group_construct(&request->group);
    error_t error = app_event_subscribe_with_app_id(
        &request->subscription,
        &request->group,
        request->parent
    );
    if (error != ERROR_NONE) {
        task_event_group_destruct(&request->group);
        return error;
    }
    request->subscribed = true;

    struct AppStartContext context = {};
    error = app_start_context_from_id(FILE_SELECTION_APP_ID, &context);
    if (error == ERROR_NONE) {
        app_start_context_set_arguments_ext(&context, request->argument_count, request->arguments);

        struct AppStreamBinding binding = {
            .producer_fd = STDOUT_FILENO,
            .stream = &request->stream,
            .buffer = request->buffer,
            .buffer_capacity = RESULT_CAPACITY,
            .event_group = &request->group,
            .window_size = {},
            .alias_fd = -1,
        };
        app_start_context_set_streams(&context, &binding, 1);
        app_start_context_set_parent(&context, request->parent);
        error = app_start_with_context(&context, &request->child);
        request->bound = (error == ERROR_NONE);
    }

    if (error != ERROR_NONE) {
        if (request->bound) {
            app_stream_unsubscribe(&request->stream);
            request->bound = false;
        }
        app_event_unsubscribe(&request->subscription);
        task_event_group_destruct(&request->group);
        request->subscribed = false;
    }

    return error;
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
            if (request->cancel_requested) {
                // Cancelled before the dialog appeared.
                deliver_result(request);
                continue;
            }

            const error_t error = start_child(request);
            if (error != ERROR_NONE) {
                LOG_E(TAG, "starting the file selection app failed: %s", error_to_string(error));
                request->result_code = 2;
                deliver_result(request);
                continue;
            }
            request->started = true;
        }

        if (request->cancel_requested && request->child != 0 && !request->has_result) {
            app_event_emit_close(request->child);
        }

        bool got_result = false;
        bool closed = false;
        struct AppEvent event = {};
        while (app_event_poll(&request->subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_RESULT && event.result.launch_id == request->child) {
                got_result = true;
                request->has_result = true;
                request->result_code = event.result.result;
            } else if (event.type == APP_EVENT_CLOSE) {
                closed = true;
            }
        }

        if (closed) {
            abandon_request(request);
            continue;
        }
        if (!got_result) {
            continue;
        }

        if (request->result_code == 0 && !request->cancel_requested) {
            request->length = app_stream_read(&request->stream, request->buffer, RESULT_CAPACITY);
        }
        deliver_result(request);
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
