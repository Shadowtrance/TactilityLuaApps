// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <notification/notification.h>

#include <tactility/error.h>

#include <string>

extern "C" {
#include <lauxlib.h>
}

namespace {

/**
 * Notifications from a script.
 *
 * The C API is context based: a context is filled in, then sent. The Lua table maps
 * directly onto that shape. The Bundle data and the tap action are intentionally not
 * exposed yet:
 *
 *  - the Bundle needs a `tactility.bundle` binding, and
 *  - an action is a C function that must stay valid for as long as the notification can
 *    be listed. A script's function belongs to a Lua state that can be closed while the
 *    notification is still on screen, so that needs a dispatch design first.
 */
void push_severity(lua_State* state, enum NotificationSeverity severity) {
    const char* name = "info";
    if (severity == NOTIFICATION_SEVERITY_WARNING) {
        name = "warning";
    } else if (severity == NOTIFICATION_SEVERITY_ERROR) {
        name = "error";
    }
    lua_pushstring(state, name);
}

void push_notification(lua_State* state, const struct Notification& notification) {
    lua_createtable(state, 0, 12);

    lua_pushinteger(state, notification.id);
    lua_setfield(state, -2, "id");

    lua_pushstring(
        state,
        notification.source_type == NOTIFICATION_SOURCE_SERVICE ? "service" : "app"
    );
    lua_setfield(state, -2, "source_type");

    lua_pushstring(state, notification.source_id);
    lua_setfield(state, -2, "source_id");

    lua_pushstring(state, notification.title);
    lua_setfield(state, -2, "title");

    lua_pushstring(state, notification.message);
    lua_setfield(state, -2, "message");

    push_severity(state, notification.severity);
    lua_setfield(state, -2, "severity");

    lua_pushinteger(state, static_cast<lua_Integer>(notification.wall_time));
    lua_setfield(state, -2, "wall_time");

    lua_pushinteger(state, static_cast<lua_Integer>(notification.uptime_ms));
    lua_setfield(state, -2, "uptime_ms");

    lua_pushboolean(state, notification.sticky);
    lua_setfield(state, -2, "sticky");

    lua_pushboolean(state, notification.data != nullptr);
    lua_setfield(state, -2, "has_data");

    lua_pushboolean(state, notification.action != nullptr);
    lua_setfield(state, -2, "has_action");
}

/** `tactility.notification.send{ message, title?, severity?, sticky?, source?, service? }` */
int notification_send_binding(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);

    lua_getfield(state, 1, "message");
    const char* message = luaL_checkstring(state, -1);
    lua_pop(state, 1);

    lua_getfield(state, 1, "title");
    const char* title = lua_isnil(state, -1) ? nullptr : luaL_checkstring(state, -1);
    std::string title_copy = title != nullptr ? title : "";
    lua_pop(state, 1);

    lua_getfield(state, 1, "source");
    const char* source = lua_isnil(state, -1) ? "lua" : luaL_checkstring(state, -1);
    std::string source_copy = source;
    lua_pop(state, 1);

    lua_getfield(state, 1, "severity");
    const auto severity = static_cast<enum NotificationSeverity>(
        lua_isnil(state, -1) ? NOTIFICATION_SEVERITY_INFO : luaL_checkinteger(state, -1)
    );
    lua_pop(state, 1);

    if (severity < NOTIFICATION_SEVERITY_INFO || severity > NOTIFICATION_SEVERITY_ERROR) {
        return luaL_error(state, "severity must be tactility.notification.SEVERITY.*");
    }

    lua_getfield(state, 1, "sticky");
    const bool sticky = lua_toboolean(state, -1);
    lua_pop(state, 1);

    lua_getfield(state, 1, "service");
    const bool service = lua_toboolean(state, -1);
    lua_pop(state, 1);

    struct NotificationContext context = service
        ? notification_context_for_service(source_copy.c_str(), message)
        : notification_context_for_app(source_copy.c_str(), message);

    if (!title_copy.empty()) {
        notification_context_set_title(&context, title_copy.c_str());
    }
    notification_context_set_severity(&context, severity);
    notification_context_set_sticky(&context, sticky);

    uint32_t id = 0;
    const error_t error = notification_send(&context, &id);
    if (error != ERROR_NONE) {
        notification_context_destroy(&context);
        lua_pushnil(state);
        lua_pushstring(state, error_to_string(error));
        return 2;
    }

    lua_pushinteger(state, id);
    return 1;
}

/** `tactility.notification.list()` - newest first. */
int notification_list_binding(lua_State* state) {
    struct Notification notifications[NOTIFICATION_HISTORY_CAPACITY] = {};
    size_t count = 0;

    const error_t error = notification_get_all(
        notifications,
        NOTIFICATION_HISTORY_CAPACITY,
        &count
    );
    if (error != ERROR_NONE) {
        lua_pushnil(state);
        lua_pushstring(state, error_to_string(error));
        return 2;
    }

    lua_createtable(state, static_cast<int>(count), 0);
    for (size_t i = 0; i < count; i++) {
        push_notification(state, notifications[i]);
        lua_seti(state, -2, static_cast<lua_Integer>(i) + 1);
        notification_release(&notifications[i]);
    }

    return 1;
}

/** `tactility.notification.get(id)` */
int notification_get_binding(lua_State* state) {
    const auto id = static_cast<uint32_t>(luaL_checkinteger(state, 1));
    struct Notification notification = {};

    const error_t error = notification_get(id, &notification);
    if (error != ERROR_NONE) {
        lua_pushnil(state);
        lua_pushstring(state, error_to_string(error));
        return 2;
    }

    push_notification(state, notification);
    notification_release(&notification);
    return 1;
}

int notification_dismiss_binding(lua_State* state) {
    const auto id = static_cast<uint32_t>(luaL_checkinteger(state, 1));

    const error_t error = notification_dismiss(id);
    if (error != ERROR_NONE) {
        lua_pushnil(state);
        lua_pushstring(state, error_to_string(error));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

int notification_dismiss_all_binding(lua_State* state) {
    notification_dismiss_all();
    lua_pushboolean(state, true);
    return 1;
}

/** Dismisses the notifications that are not sticky, leaving the sender's own alone. */
int notification_dismiss_non_sticky_binding(lua_State* state) {
    notification_dismiss_non_sticky();
    lua_pushboolean(state, true);
    return 1;
}

int notification_count_binding(lua_State* state) {
    lua_pushinteger(state, static_cast<lua_Integer>(notification_get_count()));
    return 1;
}

const luaL_Reg notification_functions[] = {
    { "send", notification_send_binding },
    { "list", notification_list_binding },
    { "get", notification_get_binding },
    { "dismiss", notification_dismiss_binding },
    { "dismiss_all", notification_dismiss_all_binding },
    { "dismiss_non_sticky", notification_dismiss_non_sticky_binding },
    { "count", notification_count_binding },
    { nullptr, nullptr }
};

/** Adds `SEVERITY` and `SOURCE` to the notification table on top of the stack. */
void add_notification_constants(lua_State* state) {
    lua_newtable(state);
    lua_pushinteger(state, NOTIFICATION_SEVERITY_INFO);
    lua_setfield(state, -2, "INFO");
    lua_pushinteger(state, NOTIFICATION_SEVERITY_WARNING);
    lua_setfield(state, -2, "WARNING");
    lua_pushinteger(state, NOTIFICATION_SEVERITY_ERROR);
    lua_setfield(state, -2, "ERROR");
    lua_setfield(state, -2, "SEVERITY");

    lua_newtable(state);
    lua_pushinteger(state, NOTIFICATION_SOURCE_APP);
    lua_setfield(state, -2, "APP");
    lua_pushinteger(state, NOTIFICATION_SOURCE_SERVICE);
    lua_setfield(state, -2, "SERVICE");
    lua_setfield(state, -2, "SOURCE");
}

} // namespace

void lua_bindings_open_notification(lua_State* state) {
    luaL_newlib(state, notification_functions);
    add_notification_constants(state);
    lua_setfield(state, -2, "notification");
}
