// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

#include <lvgl/widgets/path_bar.h>

#include <tactility/log.h>

#include <cstring>
#include <new>
#include <string>

extern "C" {
#include <lauxlib.h>
}

/**
 * `lvgl.PathBar(toolbar, { path = "/sdcard", on_pressed = fn, on_rebuilt = fn })`
 *
 * The firmware's path bar lives in a toolbar's title area. It is a hidden object:
 * the segment buttons are children of the toolbar and are rebuilt with it.
 *
 * Press and rebuild callbacks are queued with lv_async_call(), because a Lua
 * handler may change the path or delete widgets; the firmware must not continue
 * using the segment that was pressed. The queue is cancelled when the runtime
 * closes.
 */

namespace {

constexpr auto* TAG = "luavgl.path_bar";

char PATH_BARS_KEY = 0;

struct PathBarRequest {
    lua_State* state = nullptr;
    lv_obj_t* bar = nullptr;
    int pressed_ref = LUA_NOREF;
    int rebuilt_ref = LUA_NOREF;
    bool state_alive = true;
    bool cancelled = false;
    PathBarRequest* next = nullptr;
};

PathBarRequest* path_bars = nullptr;

struct PendingPathEvent {
    lua_State* state = nullptr;
    int ref = LUA_NOREF;
    bool rebuilt = false;
    std::string path;
    bool from_keys = false;
    bool cancelled = false;
    PendingPathEvent* next = nullptr;
};

PendingPathEvent* pending_events = nullptr;

void push_path_bars(lua_State* state) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, &PATH_BARS_KEY);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &PATH_BARS_KEY);
    }
}

PathBarRequest* find_path_bar(lua_State* state, lv_obj_t* bar) {
    push_path_bars(state);
    lua_pushlightuserdata(state, bar);
    const int type = lua_rawget(state, -2);
    PathBarRequest* request = nullptr;
    if (type == LUA_TLIGHTUSERDATA) {
        request = static_cast<PathBarRequest*>(lua_touserdata(state, -1));
    }
    lua_pop(state, 2);
    return request;
}

void add_path_bar(lua_State* state, PathBarRequest* request) {
    push_path_bars(state);
    lua_pushlightuserdata(state, request->bar);
    lua_pushlightuserdata(state, request);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

void remove_path_bar(lua_State* state, lv_obj_t* bar) {
    push_path_bars(state);
    lua_pushlightuserdata(state, bar);
    lua_pushnil(state);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

void unlink_request(PathBarRequest* request) {
    for (PathBarRequest** link = &path_bars; *link != nullptr; link = &(*link)->next) {
        if (*link == request) {
            *link = request->next;
            return;
        }
    }
}

void unref(int& ref, lua_State* state) {
    if (ref != LUA_NOREF) {
        luaL_unref(state, LUA_REGISTRYINDEX, ref);
        ref = LUA_NOREF;
    }
}

void free_request(PathBarRequest* request) {
    if (request->state_alive && request->state != nullptr) {
        unref(request->pressed_ref, request->state);
        unref(request->rebuilt_ref, request->state);
        remove_path_bar(request->state, request->bar);
    }
    delete request;
}

void on_deleted(lv_event_t* event) {
    auto* request = static_cast<PathBarRequest*>(lv_event_get_user_data(event));
    unlink_request(request);
    free_request(request);
}

void unlink_pending(PendingPathEvent* event) {
    for (PendingPathEvent** link = &pending_events; *link != nullptr; link = &(*link)->next) {
        if (*link == event) {
            *link = event->next;
            return;
        }
    }
}

void run_pending(void* context) {
    auto* event = static_cast<PendingPathEvent*>(context);
    unlink_pending(event);

    const bool cancelled = event->cancelled;
    lua_State* state = event->state;
    const int ref = event->ref;
    if (cancelled || state == nullptr || ref == LUA_NOREF) {
        delete event;
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        luaL_unref(state, LUA_REGISTRYINDEX, ref);
        delete event;
        return;
    }

    int argument_count = 0;
    if (!event->rebuilt) {
        lua_pushlstring(state, event->path.data(), event->path.size());
        lua_pushboolean(state, event->from_keys);
        argument_count = 2;
    }

    if (lua_pcall(state, argument_count, 0, 0) != LUA_OK) {
        LOG_E(TAG, "path bar callback: %s", lua_tostring(state, -1));
        lua_pop(state, 1);
    }
    luaL_unref(state, LUA_REGISTRYINDEX, ref);
    delete event;
}

int queue_event(PathBarRequest* request, int callback_ref, bool rebuilt, const char* path, bool from_keys) {
    if (request->cancelled || !request->state_alive || callback_ref == LUA_NOREF) {
        return 0;
    }

    lua_State* state = request->state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, callback_ref);
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        return 0;
    }
    const int event_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    auto* event = new (std::nothrow) PendingPathEvent();
    if (event == nullptr) {
        luaL_unref(state, LUA_REGISTRYINDEX, event_ref);
        return 0;
    }
    event->state = state;
    event->ref = event_ref;
    event->rebuilt = rebuilt;
    event->path = (path != nullptr) ? path : "";
    event->from_keys = from_keys;
    event->next = pending_events;
    pending_events = event;
    lv_async_call(run_pending, event);
    return 1;
}

void on_pressed(lv_obj_t* bar, const char* path, bool from_keys, void* user_data) {
    auto* request = static_cast<PathBarRequest*>(user_data);
    queue_event(request, request->pressed_ref, false, path, from_keys);
}

void on_rebuilt(lv_obj_t* bar, void* user_data) {
    auto* request = static_cast<PathBarRequest*>(user_data);
    queue_event(request, request->rebuilt_ref, true, nullptr, false);
}

PathBarRequest* check_request(lua_State* state) {
    lv_obj_t* bar = luavgl_check_object(state, 1);
    PathBarRequest* request = find_path_bar(state, bar);
    if (request == nullptr || request->cancelled) {
        luaL_error(state, "this widget is not a path bar");
    }
    return request;
}

} // namespace

int luavgl_widget_path_bar(lua_State* state) {
    lv_obj_t* toolbar = luavgl_check_object(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);

    auto* request = new (std::nothrow) PathBarRequest();
    if (request == nullptr) {
        return luaL_error(state, "out of memory");
    }
    request->state = state;

    std::string path;
    lua_getfield(state, 2, "path");
    if (lua_isstring(state, -1)) {
        size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        path.assign(text, length);
    }
    lua_pop(state, 1);

    const struct {
        const char* name;
        int* ref;
    } callbacks[] = {
        { "on_pressed", &request->pressed_ref },
        { "on_rebuilt", &request->rebuilt_ref },
    };
    for (const auto& callback : callbacks) {
        lua_getfield(state, 2, callback.name);
        if (lua_isfunction(state, -1)) {
            *callback.ref = luaL_ref(state, LUA_REGISTRYINDEX);
        } else if (!lua_isnil(state, -1)) {
            lua_pop(state, 1);
            delete request;
            return luaL_error(state, "%s must be a function", callback.name);
        } else {
            lua_pop(state, 1);
        }
    }

    request->bar = lvgl_path_bar_create(toolbar, on_pressed, on_rebuilt, request);
    if (request->bar == nullptr) {
        delete request;
        return luaL_error(state, "could not create the path bar");
    }

    lua_pushlstring(state, path.data(), path.size());
    lvgl_path_bar_set_path(request->bar, lua_tostring(state, -1));
    lua_pop(state, 1);

    lv_obj_add_event_cb(request->bar, on_deleted, LV_EVENT_DELETE, request);

    request->next = path_bars;
    path_bars = request;
    add_path_bar(state, request);

    luavgl_push_object(state, request->bar);
    return 1;
}

int luavgl_path_bar_set_path(lua_State* state) {
    PathBarRequest* request = check_request(state);
    const char* path = luaL_checkstring(state, 2);
    lvgl_path_bar_set_path(request->bar, path);
    lua_settop(state, 1);
    return 1;
}

int luavgl_path_bar_focus_parent(lua_State* state) {
    lvgl_path_bar_focus_parent_segment(check_request(state)->bar);
    lua_settop(state, 1);
    return 1;
}

int luavgl_path_bar_parent_segment(lua_State* state) {
    lv_obj_t* segment = lvgl_path_bar_get_parent_segment(check_request(state)->bar);
    if (segment == nullptr) {
        lua_pushnil(state);
    } else {
        luavgl_push_object(state, segment);
    }
    return 1;
}

void luavgl_delete_all_path_bars(lua_State* state) {
    for (PathBarRequest* request = path_bars; request != nullptr; request = request->next) {
        if (request->state != state) {
            continue;
        }
        request->state_alive = false;
        request->cancelled = true;
        request->state = nullptr;
        unref(request->pressed_ref, state);
        unref(request->rebuilt_ref, state);
    }

    for (PendingPathEvent* event = pending_events; event != nullptr; event = event->next) {
        if (event->state != state) {
            continue;
        }
        event->cancelled = true;
        unref(event->ref, state);
        event->state = nullptr;
    }
}
