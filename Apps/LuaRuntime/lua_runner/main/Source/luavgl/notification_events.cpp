// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

#include <notification/notification.h>

#include <tactility/concurrent/task_event_group.h>
#include <tactility/error.h>
#include <tactility/log.h>

#include <cstdint>

extern "C" {
#include <lauxlib.h>
}

/**
 * `tactility.notification.subscribe(callback)`
 *
 * The notification module queues events per caller-owned subscription and signals an
 * event group; it does not call into Lua. This binding polls that queue from an LVGL
 * timer, so the callback runs on the LVGL task like every other script callback and
 * never from the task that posted the notification.
 *
 * A subscription owns its timer, so it is pinned in the registry until it is cancelled
 * or the runtime is detached. Dropping the handle does not silently keep a timer alive
 * against a closed state the way a widget-less timer would.
 */

namespace {

constexpr auto* TAG = "luavgl.notification";
constexpr auto* SUBSCRIPTION_METATABLE = "tactility.NotificationSubscription";

/** How often queued events are handed to the script. Notifications are not latency critical. */
constexpr uint32_t POLL_PERIOD_MS = 200;

/** Registry key for the live subscriptions, so shutdown can cancel them all. */
char SUBSCRIPTIONS_KEY = 0;

struct SubscriptionHandle {
    lua_State* state;
    struct TaskEventGroup event_group;
    struct NotificationSubscription subscription;
    lv_timer_t* timer;
    /** Registry reference to the Lua callback, or LUA_NOREF once cancelled. */
    int callback_ref;
    bool active;
};

void push_subscriptions(lua_State* state) {
    lua_pushlightuserdata(state, &SUBSCRIPTIONS_KEY);
    if (lua_rawget(state, LUA_REGISTRYINDEX) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);

        lua_pushlightuserdata(state, &SUBSCRIPTIONS_KEY);
        lua_pushvalue(state, -2);
        lua_rawset(state, LUA_REGISTRYINDEX);
    }
}

const char* event_type_name(enum NotificationEventType type) {
    switch (type) {
        case NOTIFICATION_EVENT_POSTED: return "posted";
        case NOTIFICATION_EVENT_DISMISSED: return "dismissed";
        case NOTIFICATION_EVENT_CLEARED: return "cleared";
        case NOTIFICATION_EVENT_UPDATED: return "updated";
        default: return "unknown";
    }
}

void push_event(lua_State* state, const struct NotificationEvent& event) {
    lua_createtable(state, 0, 6);

    lua_pushinteger(state, static_cast<lua_Integer>(event.id));
    lua_setfield(state, -2, "id");

    lua_pushstring(state, event_type_name(event.type));
    lua_setfield(state, -2, "type");

    lua_pushinteger(state, static_cast<lua_Integer>(event.timestamp));
    lua_setfield(state, -2, "timestamp");

    lua_pushinteger(state, static_cast<lua_Integer>(event.order));
    lua_setfield(state, -2, "order");

    lua_pushboolean(state, event.bumped);
    lua_setfield(state, -2, "bumped");
}

/** Cancels the subscription. Safe to call from the subscription's own callback. */
void destroy_subscription(SubscriptionHandle* handle) {
    if (!handle->active) {
        return;
    }
    handle->active = false;

    if (handle->timer != nullptr) {
        lv_timer_delete(handle->timer);
        handle->timer = nullptr;
    }

    notification_unsubscribe(&handle->subscription);
    task_event_group_destruct(&handle->event_group);

    lua_State* state = handle->state;
    if (handle->callback_ref != LUA_NOREF) {
        luaL_unref(state, LUA_REGISTRYINDEX, handle->callback_ref);
        handle->callback_ref = LUA_NOREF;
    }

    push_subscriptions(state);
    lua_pushlightuserdata(state, handle);
    lua_pushnil(state);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

void on_poll(lv_timer_t* timer) {
    auto* handle = static_cast<SubscriptionHandle*>(lv_timer_get_user_data(timer));
    if (handle == nullptr || !handle->active) {
        return;
    }

    lua_State* state = handle->state;

    struct NotificationEvent event;
    while (notification_poll(&handle->subscription, &event) == ERROR_NONE) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, handle->callback_ref);
        if (lua_type(state, -1) != LUA_TFUNCTION) {
            lua_pop(state, 1);
            continue;
        }

        push_event(state, event);
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            LOG_E(TAG, "notification callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }

        // The callback may have cancelled the subscription, which deletes this timer. The
        // handle is still pinned until then, but nothing may touch the timer afterwards.
        if (!handle->active) {
            return;
        }
    }
}

SubscriptionHandle* check_subscription(lua_State* state, int index) {
    return static_cast<SubscriptionHandle*>(luaL_checkudata(state, index, SUBSCRIPTION_METATABLE));
}

int subscription_cancel(lua_State* state) {
    destroy_subscription(check_subscription(state, 1));
    return 0;
}

int subscription_is_active(lua_State* state) {
    lua_pushboolean(state, check_subscription(state, 1)->active);
    return 1;
}

int subscription_gc(lua_State* state) {
    // Cancelled handles are unpinned and collected normally. An active one is only
    // reachable here if the registry table was replaced, so clean it up defensively.
    destroy_subscription(check_subscription(state, 1));
    return 0;
}

const luaL_Reg subscription_methods[] = {
    { "cancel", subscription_cancel },
    { "is_active", subscription_is_active },
    { nullptr, nullptr }
};

/**
 * `tactility.notification.subscribe(callback) -> subscription | nil, reason`
 *
 * Each event table is `{ id, type, timestamp, order, bumped }`, where `type` is one of
 * `tactility.notification.EVENT.*`. The subscription stays active until `:cancel()` or
 * until the script's runtime closes; events posted outside that window are not kept.
 */
int notification_subscribe_binding(lua_State* state) {
    luaL_checktype(state, 1, LUA_TFUNCTION);

    auto* handle = static_cast<SubscriptionHandle*>(
        lua_newuserdatauv(state, sizeof(SubscriptionHandle), 0)
    );
    handle->state = state;
    handle->timer = nullptr;
    handle->callback_ref = LUA_NOREF;
    handle->active = false;
    task_event_group_construct(&handle->event_group);

    lua_pushvalue(state, 1);
    handle->callback_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    luaL_setmetatable(state, SUBSCRIPTION_METATABLE);

    const error_t error = notification_subscribe(&handle->subscription, &handle->event_group);
    if (error != ERROR_NONE) {
        task_event_group_destruct(&handle->event_group);
        luaL_unref(state, LUA_REGISTRYINDEX, handle->callback_ref);
        handle->callback_ref = LUA_NOREF;
        lua_pushnil(state);
        lua_pushstring(state, error_to_string(error));
        return 2;
    }

    auto* timer = lv_timer_create(on_poll, POLL_PERIOD_MS, handle);
    if (timer == nullptr) {
        notification_unsubscribe(&handle->subscription);
        task_event_group_destruct(&handle->event_group);
        luaL_unref(state, LUA_REGISTRYINDEX, handle->callback_ref);
        handle->callback_ref = LUA_NOREF;
        lua_pushnil(state);
        lua_pushstring(state, "failed to create the event timer");
        return 2;
    }

    // Owned explicitly: an expired or deleted timer must not free itself behind the handle.
    lv_timer_set_auto_delete(timer, false);
    handle->timer = timer;
    handle->active = true;

    // Pinned until cancelled, so a script that drops the handle cannot leave a timer
    // polling into a closed state.
    push_subscriptions(state);
    lua_pushlightuserdata(state, handle);
    lua_pushvalue(state, -3); // the handle userdata
    lua_rawset(state, -3);
    lua_pop(state, 1);

    return 1;
}

/** Adds the subscribe function and the EVENT constants to `tactility.notification`. */
void add_notification_event_api(lua_State* state) {
    lua_getglobal(state, "tactility");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        LOG_E(TAG, "tactility table is missing");
        return;
    }

    lua_getfield(state, -1, "notification");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 2);
        LOG_E(TAG, "tactility.notification table is missing");
        return;
    }

    lua_pushcfunction(state, notification_subscribe_binding);
    lua_setfield(state, -2, "subscribe");

    lua_newtable(state);
    lua_pushstring(state, event_type_name(NOTIFICATION_EVENT_POSTED));
    lua_setfield(state, -2, "POSTED");
    lua_pushstring(state, event_type_name(NOTIFICATION_EVENT_DISMISSED));
    lua_setfield(state, -2, "DISMISSED");
    lua_pushstring(state, event_type_name(NOTIFICATION_EVENT_CLEARED));
    lua_setfield(state, -2, "CLEARED");
    lua_pushstring(state, event_type_name(NOTIFICATION_EVENT_UPDATED));
    lua_setfield(state, -2, "UPDATED");
    lua_setfield(state, -2, "EVENT");

    lua_pop(state, 2);
}

} // namespace

void luavgl_open_notification(lua_State* state) {
    luaL_newmetatable(state, SUBSCRIPTION_METATABLE);

    luaL_newlib(state, subscription_methods);
    lua_setfield(state, -2, "__index");

    lua_pushcfunction(state, subscription_gc);
    lua_setfield(state, -2, "__gc");

    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");

    lua_pop(state, 1);

    add_notification_event_api(state);
}

void luavgl_delete_all_notification_subscriptions(lua_State* state) {
    push_subscriptions(state);

    // Collect first: destroy_subscription() mutates the table, and mutating it during
    // lua_next is undefined.
    SubscriptionHandle* handles[16];
    size_t count = 0;

    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        lua_pop(state, 1); // the handle; the key is the pointer
        if (count < sizeof(handles) / sizeof(handles[0])) {
            handles[count++] = static_cast<SubscriptionHandle*>(lua_touserdata(state, -1));
        }
    }
    lua_pop(state, 1);

    for (size_t i = 0; i < count; i++) {
        destroy_subscription(handles[i]);
    }
}
