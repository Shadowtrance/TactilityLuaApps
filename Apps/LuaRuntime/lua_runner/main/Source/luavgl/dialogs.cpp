// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

#include <lua/runtime.h>

#include <app/scheduler.h>

#include <lvgl_window_manager/dialog.h>

#include <tactility/error.h>
#include <tactility/log.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

extern "C" {
#include <lauxlib.h>
}

/**
 * `tactility.ui.alert/input/select`
 *
 * The window manager's own modal dialogs, so an app does not have to build a view for a
 * confirmation, a single-line prompt or a short choice. They run on a task of their own
 * and call back without the LVGL lock; this file only copies the result there. The
 * runner's app loop then delivers the Lua callback with the LVGL lock held.
 *
 * A returned handle can dismiss the dialog. The handle is pinned until the result is
 * delivered, so ignoring the return value does not cancel the dialog.
 */

namespace {

constexpr auto* TAG = "luavgl.dialog";
constexpr auto* DIALOG_METATABLE = "tactility.ui.Dialog";

constexpr int MAX_BUTTONS = 8;
constexpr int MAX_ITEMS = 32;

/** Registry key for the live dialogs, so shutdown can abandon them all. */
char DIALOGS_KEY = 0;

/** Set by `tactility.ui.dismiss()`, handled by the runner's app loop. */
std::atomic<bool> GLOBAL_DISMISS_REQUESTED{false};

enum class DialogKind {
    Alert,
    Input,
    Select,
};

struct DialogRequest {
    lua_State* state = nullptr;
    int callback_ref = LUA_NOREF;
    int pin_ref = LUA_NOREF;
    DialogKind kind = DialogKind::Alert;
    AppInstanceId parent = 0;

    std::string title;
    std::string message;
    std::vector<std::string> buttons;
    std::vector<std::string> items;
    std::vector<std::string> icons;
    std::string prefilled;
    std::string ok_label;
    std::string cancel_label;

    bool start_requested = true;
    bool dialog_started = false;
    bool dismissed = false;
    bool finished = false;
    bool abandoned = false;
    std::atomic<bool> dismiss_requested{false};
    std::atomic<bool> result_ready{false};

    /** Guards the fields below: the result arrives on the dialog's task. */
    std::mutex mutex;
    bool result_dismissed = false;
    bool result_confirmed = false;
    int32_t result_index = 0;
    std::string result_text;
};

struct DialogHandle {
    DialogRequest* request;
};

void push_dialogs(lua_State* state) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, &DIALOGS_KEY);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &DIALOGS_KEY);
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

const char* read_string_array(
    lua_State* state,
    int table_index,
    const char* key,
    std::vector<std::string>& values,
    int maximum,
    bool required
) {
    lua_getfield(state, table_index, key);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return required ? "the list is missing" : nullptr;
    }
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return "the list must be a table";
    }

    const lua_Integer count = luaL_len(state, -1);
    if (count > maximum) {
        lua_pop(state, 1);
        return "too many entries";
    }
    if (required && count == 0) {
        lua_pop(state, 1);
        return "the list is empty";
    }

    values.reserve(static_cast<size_t>(count));
    for (lua_Integer index = 1; index <= count; index++) {
        lua_geti(state, -1, index);
        const char* text = luaL_checkstring(state, -1);
        values.emplace_back(text);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return nullptr;
}

/** Removes the request from the live table and releases its Lua references. */
void release_request(DialogRequest* request) {
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

    push_dialogs(state);
    lua_pushlightuserdata(state, request);
    lua_pushnil(state);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

/** See picker.cpp: the request may still be referenced by a dialog task. */
void abandon_request(DialogRequest* request) {
    if (request->finished) {
        return;
    }
    request->finished = true;
    request->abandoned = true;
}

void call_nil(lua_State* state, const char* reason) {
    lua_pushnil(state);
    lua_pushstring(state, reason);
    if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
        LOG_E(TAG, "dialog callback: %s", lua_tostring(state, -1));
        lua_pop(state, 1);
    }
}

void deliver_result(DialogRequest* request) {
    if (request->finished) {
        return;
    }

    bool dismissed = false;
    bool confirmed = false;
    int32_t index = 0;
    std::string text;
    {
        std::lock_guard<std::mutex> lock(request->mutex);
        dismissed = request->result_dismissed;
        confirmed = request->result_confirmed;
        index = request->result_index;
        text = request->result_text;
    }

    lua_State* state = request->state;
    if (request->callback_ref == LUA_NOREF) {
        release_request(request);
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, request->callback_ref);
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        release_request(request);
        return;
    }

    const bool cancelled = dismissed ||
        (request->kind == DialogKind::Input && !confirmed) ||
        (request->kind == DialogKind::Select && index < 0);
    if (cancelled) {
        call_nil(state, "cancelled");
    } else if (request->kind == DialogKind::Input) {
        lua_pushlstring(state, text.data(), text.size());
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            LOG_E(TAG, "dialog callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    } else {
        // Alert and select use a 1-based Lua index.
        lua_pushinteger(state, static_cast<lua_Integer>(index) + 1);
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            LOG_E(TAG, "dialog callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    }

    release_request(request);
}

void deliver_failure(DialogRequest* request, const char* reason) {
    if (request->finished) {
        return;
    }

    lua_State* state = request->state;
    if (request->callback_ref == LUA_NOREF) {
        release_request(request);
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, request->callback_ref);
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        release_request(request);
        return;
    }

    call_nil(state, reason);
    release_request(request);
}

/** Called on the dialog's task, without the LVGL lock. Only copies C data. */
void on_alert_result(int32_t button_index, void* user_data) {
    auto* request = static_cast<DialogRequest*>(user_data);
    std::lock_guard<std::mutex> lock(request->mutex);
    if (request->abandoned) {
        return;
    }
    request->result_dismissed = (button_index == WINDOW_DIALOG_DISMISSED);
    request->result_index = button_index;
    request->result_ready.store(true);
}

void on_input_result(bool confirmed, const char* text, void* user_data) {
    auto* request = static_cast<DialogRequest*>(user_data);
    std::lock_guard<std::mutex> lock(request->mutex);
    if (request->abandoned) {
        return;
    }
    request->result_dismissed = !confirmed;
    request->result_confirmed = confirmed;
    request->result_text = (text != nullptr) ? text : "";
    request->result_ready.store(true);
}

void on_select_result(int32_t item_index, void* user_data) {
    auto* request = static_cast<DialogRequest*>(user_data);
    std::lock_guard<std::mutex> lock(request->mutex);
    if (request->abandoned) {
        return;
    }
    request->result_dismissed = (item_index == WINDOW_DIALOG_DISMISSED);
    request->result_index = item_index;
    request->result_ready.store(true);
}

error_t start_dialog(DialogRequest* request) {
    switch (request->kind) {
        case DialogKind::Alert: {
            std::vector<const char*> buttons;
            buttons.reserve(request->buttons.size());
            for (const auto& button : request->buttons) {
                buttons.push_back(button.c_str());
            }
            return window_manager_alert_dialog(
                request->parent,
                request->title.c_str(),
                request->message.c_str(),
                buttons.data(),
                static_cast<uint32_t>(buttons.size()),
                on_alert_result,
                request
            );
        }
        case DialogKind::Input:
            return window_manager_input_dialog(
                request->parent,
                request->title.c_str(),
                request->message.c_str(),
                request->prefilled.empty() ? nullptr : request->prefilled.c_str(),
                request->ok_label.c_str(),
                request->cancel_label.c_str(),
                on_input_result,
                request
            );
        case DialogKind::Select: {
            std::vector<const char*> items;
            items.reserve(request->items.size());
            for (const auto& item : request->items) {
                items.push_back(item.c_str());
            }
            if (request->icons.empty()) {
                return window_manager_selection_dialog(
                    request->parent,
                    request->title.c_str(),
                    items.data(),
                    static_cast<uint32_t>(items.size()),
                    on_select_result,
                    request
                );
            }
            std::vector<const char*> icons;
            icons.reserve(request->icons.size());
            for (const auto& icon : request->icons) {
                icons.push_back(icon.c_str());
            }
            return window_manager_selection_dialog_with_icons(
                request->parent,
                request->title.c_str(),
                items.data(),
                icons.data(),
                static_cast<uint32_t>(items.size()),
                on_select_result,
                request
            );
        }
    }
    return ERROR_INVALID_ARGUMENT;
}

int dialog_dismiss(lua_State* state) {
    auto* handle = static_cast<DialogHandle*>(luaL_checkudata(state, 1, DIALOG_METATABLE));
    if (handle->request != nullptr && !handle->request->finished) {
        handle->request->dismiss_requested.store(true);
    }
    lua_pushboolean(state, 1);
    return 1;
}

int dialog_is_active(lua_State* state) {
    auto* handle = static_cast<DialogHandle*>(luaL_checkudata(state, 1, DIALOG_METATABLE));
    lua_pushboolean(state, handle->request != nullptr && !handle->request->finished);
    return 1;
}

int dialog_gc(lua_State* state) {
    auto* handle = static_cast<DialogHandle*>(luaL_checkudata(state, 1, DIALOG_METATABLE));
    if (handle->request != nullptr) {
        if (!handle->request->abandoned) {
            release_request(handle->request);
            delete handle->request;
        }
        handle->request = nullptr;
    }
    return 0;
}

const luaL_Reg dialog_methods[] = {
    { "dismiss", dialog_dismiss },
    { "is_active", dialog_is_active },
    { nullptr, nullptr }
};

int push_dialog_handle(lua_State* state, DialogRequest* request) {
    auto* handle = static_cast<DialogHandle*>(lua_newuserdatauv(state, sizeof(DialogHandle), 0));
    handle->request = request;
    luaL_setmetatable(state, DIALOG_METATABLE);

    lua_pushvalue(state, -1);
    request->pin_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    push_dialogs(state);
    lua_pushlightuserdata(state, request);
    lua_pushvalue(state, -3);
    lua_rawset(state, -3);
    lua_pop(state, 1);

    return 1;
}

int ui_alert(lua_State* state) {
    auto* request = new (std::nothrow) DialogRequest();
    if (request == nullptr) {
        return push_failure(state, "out of memory");
    }
    request->state = state;
    request->kind = DialogKind::Alert;
    request->title = "Alert";
    request->buttons.emplace_back("OK");

    if (lua_isstring(state, 1)) {
        size_t length = 0;
        const char* message = lua_tolstring(state, 1, &length);
        request->message.assign(message, length);
        if (lua_isfunction(state, 2)) {
            lua_pushvalue(state, 2);
            request->callback_ref = luaL_ref(state, LUA_REGISTRYINDEX);
        } else if (!lua_isnoneornil(state, 2)) {
            delete request;
            return push_failure(state, "callback must be a function");
        }
    } else {
        if (!lua_istable(state, 1)) {
            delete request;
            return push_failure(state, "options must be a table");
        }
        read_string_field(state, 1, "title", request->title);
        read_string_field(state, 1, "message", request->message);
        request->buttons.clear();
        const char* error = read_string_array(state, 1, "buttons", request->buttons, MAX_BUTTONS, false);
        if (error != nullptr) {
            delete request;
            return push_failure(state, error);
        }
        if (request->buttons.empty()) {
            request->buttons.emplace_back("OK");
        }
        if (lua_isfunction(state, 2)) {
            lua_pushvalue(state, 2);
            request->callback_ref = luaL_ref(state, LUA_REGISTRYINDEX);
        } else if (!lua_isnoneornil(state, 2)) {
            delete request;
            return push_failure(state, "callback must be a function");
        }
    }

    request->parent = app_scheduler_current_app_id();
    if (request->parent == 0) {
        if (request->callback_ref != LUA_NOREF) {
            luaL_unref(state, LUA_REGISTRYINDEX, request->callback_ref);
        }
        delete request;
        return push_failure(state, "dialogs need a running app");
    }

    return push_dialog_handle(state, request);
}

int ui_input(lua_State* state) {
    if (!lua_istable(state, 1)) {
        return push_failure(state, "options must be a table");
    }

    auto* request = new (std::nothrow) DialogRequest();
    if (request == nullptr) {
        return push_failure(state, "out of memory");
    }
    request->state = state;
    request->kind = DialogKind::Input;
    request->title = "Input";
    request->ok_label = "OK";
    request->cancel_label = "Cancel";

    read_string_field(state, 1, "title", request->title);
    read_string_field(state, 1, "message", request->message);
    read_string_field(state, 1, "text", request->prefilled);
    read_string_field(state, 1, "ok", request->ok_label);
    read_string_field(state, 1, "cancel", request->cancel_label);

    if (lua_isfunction(state, 2)) {
        lua_pushvalue(state, 2);
        request->callback_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    } else if (!lua_isnoneornil(state, 2)) {
        delete request;
        return push_failure(state, "callback must be a function");
    }

    request->parent = app_scheduler_current_app_id();
    if (request->parent == 0) {
        if (request->callback_ref != LUA_NOREF) {
            luaL_unref(state, LUA_REGISTRYINDEX, request->callback_ref);
        }
        delete request;
        return push_failure(state, "dialogs need a running app");
    }

    return push_dialog_handle(state, request);
}

int ui_select(lua_State* state) {
    if (!lua_istable(state, 1)) {
        return push_failure(state, "options must be a table");
    }

    auto* request = new (std::nothrow) DialogRequest();
    if (request == nullptr) {
        return push_failure(state, "out of memory");
    }
    request->state = state;
    request->kind = DialogKind::Select;
    request->title = "Select";

    read_string_field(state, 1, "title", request->title);

    const char* error = read_string_array(state, 1, "items", request->items, MAX_ITEMS, true);
    if (error != nullptr) {
        delete request;
        return push_failure(state, error);
    }
    error = read_string_array(state, 1, "icons", request->icons, MAX_ITEMS, false);
    if (error != nullptr) {
        delete request;
        return push_failure(state, error);
    }
    if (!request->icons.empty() && request->icons.size() != request->items.size()) {
        delete request;
        return push_failure(state, "icons must match items");
    }

    if (lua_isfunction(state, 2)) {
        lua_pushvalue(state, 2);
        request->callback_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    } else if (!lua_isnoneornil(state, 2)) {
        delete request;
        return push_failure(state, "callback must be a function");
    }

    request->parent = app_scheduler_current_app_id();
    if (request->parent == 0) {
        if (request->callback_ref != LUA_NOREF) {
            luaL_unref(state, LUA_REGISTRYINDEX, request->callback_ref);
        }
        delete request;
        return push_failure(state, "dialogs need a running app");
    }

    return push_dialog_handle(state, request);
}

int ui_dismiss(lua_State* state) {
    GLOBAL_DISMISS_REQUESTED.store(true);
    lua_pushboolean(state, 1);
    return 1;
}

const luaL_Reg dialog_functions[] = {
    { "alert", ui_alert },
    { "input", ui_input },
    { "select", ui_select },
    { "dismiss", ui_dismiss },
    { nullptr, nullptr }
};

} // namespace

void luavgl_open_dialogs(lua_State* state) {
    luaL_newmetatable(state, DIALOG_METATABLE);

    luaL_newlib(state, dialog_methods);
    lua_setfield(state, -2, "__index");

    lua_pushcfunction(state, dialog_gc);
    lua_setfield(state, -2, "__gc");

    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");

    lua_pop(state, 1);

    lua_getglobal(state, "tactility");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        LOG_E(TAG, "tactility table is missing");
        return;
    }
    lua_getfield(state, -1, "ui");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 2);
        LOG_E(TAG, "tactility.ui table is missing");
        return;
    }

    luaL_setfuncs(state, dialog_functions, 0);
    lua_pop(state, 2);
}

extern "C" void luavgl_dialogs_poll(struct LuaRuntime* runtime) {
    if (GLOBAL_DISMISS_REQUESTED.exchange(false)) {
        window_manager_dialog_dismiss();
    }

    auto* state = static_cast<lua_State*>(lua_runtime_get_state(runtime));
    push_dialogs(state);

    // Collect first, because release_request() mutates the table.
    DialogRequest* requests[8];
    size_t count = 0;

    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        lua_pop(state, 1); // the handle; the key is the request pointer
        if (count < sizeof(requests) / sizeof(requests[0])) {
            requests[count++] = static_cast<DialogRequest*>(lua_touserdata(state, -1));
        }
    }
    lua_pop(state, 1);

    for (size_t index = 0; index < count; index++) {
        DialogRequest* request = requests[index];
        if (request == nullptr || request->finished) {
            continue;
        }

        if (request->start_requested) {
            request->start_requested = false;
            if (request->dismiss_requested.load()) {
                deliver_failure(request, "cancelled");
                continue;
            }

            const error_t error = start_dialog(request);
            if (error != ERROR_NONE) {
                LOG_E(TAG, "showing a dialog failed: %s", error_to_string(error));
                deliver_failure(request, "another dialog is open");
                continue;
            }
            request->dialog_started = true;
        }

        if (request->dismiss_requested.load() && request->dialog_started && !request->dismissed) {
            window_manager_dialog_dismiss();
            request->dismissed = true;
        }

        if (request->result_ready.exchange(false)) {
            deliver_result(request);
        }
    }
}

void luavgl_delete_all_dialogs(lua_State* state) {
    push_dialogs(state);

    DialogRequest* requests[8];
    size_t count = 0;

    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        lua_pop(state, 1); // the handle; the key is the request pointer
        if (count < sizeof(requests) / sizeof(requests[0])) {
            requests[count++] = static_cast<DialogRequest*>(lua_touserdata(state, -1));
        }
    }
    lua_pop(state, 1);

    for (size_t index = 0; index < count; index++) {
        abandon_request(requests[index]);
    }
}
