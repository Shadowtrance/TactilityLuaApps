// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

#include <cstring>

extern "C" {
#include <lauxlib.h>
}

namespace {

/** Registry key for the lv_obj_t* -> handle map. Its address is the key. */
char OBJECT_REGISTRY_KEY = 0;

/**
 * Registry key for the live wrapped objects.
 *
 * The map above holds handles weakly so dropped handles can be collected, which means it
 * cannot be used to find what still needs detaching at shutdown. This table keeps the
 * `lv_obj_t*` address (light userdata, so it pins nothing) mapped to the handle's
 * generation. The delete callback removes the entry while the widget is still valid, so
 * shutdown never has to ask LVGL whether a remembered address is alive: in LVGL 9.6
 * `lv_obj_is_valid()` is a deprecated alias for `lv_obj_is_in_widget_tree()`, which
 * dereferences the pointer and crashes on freed memory.
 */
char TRACKED_OBJECTS_KEY = 0;

/** Registry key for widgets carrying Lua event callbacks. See luavgl_track_event_target. */
char EVENT_TARGETS_KEY = 0;

/** Registry key for the default parent. */
char ROOT_KEY = 0;

uint32_t generation_counter = 0;

/** Pushes the weak-valued table mapping objects to their handles. */
void push_object_registry(lua_State* state) {
    lua_pushlightuserdata(state, &OBJECT_REGISTRY_KEY);
    lua_rawget(state, LUA_REGISTRYINDEX);

    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);

        lua_newtable(state);

        // Weak values: a handle the script has dropped can be collected even though the
        // widget is still alive. Without this the map would pin every handle ever made
        // for the lifetime of the runtime.
        lua_newtable(state);
        lua_pushstring(state, "v");
        lua_setfield(state, -2, "__mode");
        lua_setmetatable(state, -2);

        lua_pushlightuserdata(state, &OBJECT_REGISTRY_KEY);
        lua_pushvalue(state, -2);
        lua_rawset(state, LUA_REGISTRYINDEX);
    }
}

/** Pushes the table of live `lv_obj_t*` addresses and their generations. */
void push_tracked_objects(lua_State* state) {
    lua_pushlightuserdata(state, &TRACKED_OBJECTS_KEY);
    lua_rawget(state, LUA_REGISTRYINDEX);

    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);

        lua_newtable(state);

        lua_pushlightuserdata(state, &TRACKED_OBJECTS_KEY);
        lua_pushvalue(state, -2);
        lua_rawset(state, LUA_REGISTRYINDEX);
    }
}

/**
 * Runs when LVGL destroys the widget, from inside lv_obj_delete().
 *
 * Nulls the handle so later use raises, and drops the registry entry so the address can
 * be reused safely. The Lua state comes through user_data because LVGL callbacks carry no
 * other context.
 */
void on_object_deleted(lv_event_t* event) {
    auto* state = static_cast<lua_State*>(lv_event_get_user_data(event));
    auto* object = static_cast<lv_obj_t*>(lv_event_get_current_target(event));

    // Animations write through the widget pointer on every frame, so stop them before
    // LVGL frees it.
    lv_anim_delete(object, nullptr);

    push_object_registry(state);

    lua_pushlightuserdata(state, object);
    lua_rawget(state, -2);

    if (auto* handle = static_cast<ObjectHandle*>(lua_touserdata(state, -1))) {
        handle->object = nullptr;
    }
    lua_pop(state, 1);

    lua_pushlightuserdata(state, object);
    lua_pushnil(state);
    lua_rawset(state, -3);

    lua_pop(state, 1); // object registry

    // The widget is still valid here, so this is the last safe moment to forget it.
    // LVGL runs delete callbacks before freeing, so the address cannot already belong to
    // a new widget; the entry is removed unconditionally even if the handle was collected.
    push_tracked_objects(state);
    lua_pushlightuserdata(state, object);
    lua_pushnil(state);
    lua_rawset(state, -3);
    lua_pop(state, 1); // tracked objects
}

// Methods -------------------------------------------------------------------------------

int object_set(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);

    luavgl_apply_properties(state, object, 2);

    lua_settop(state, 1); // return self, so calls can chain
    return 1;
}

int object_delete(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);

    // on_object_deleted() runs synchronously from here and clears the handle
    lv_obj_delete(object);
    return 0;
}

/**
 * `obj:clean()` - deletes every child, keeping the object itself. Returns the object.
 *
 * The natural way to swap views: a container is emptied and rebuilt rather than every
 * widget being tracked and deleted by hand. Handles for the children are invalidated by
 * the same LV_EVENT_DELETE path as delete(), so a script still holding one gets
 * "widget has been deleted" rather than a dangling pointer.
 */
int object_clean(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);

    lv_obj_clean(object);

    lua_settop(state, 1);
    return 1;
}

int object_is_valid(lua_State* state) {
    auto* handle = static_cast<ObjectHandle*>(luaL_checkudata(state, 1, LUAVGL_OBJECT_METATABLE));
    lua_pushboolean(state, handle->object != nullptr);
    return 1;
}

int object_get_parent(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);

    auto* parent = lv_obj_get_parent(object);
    if (parent == nullptr) {
        lua_pushnil(state);
    } else {
        luavgl_push_object(state, parent);
    }
    return 1;
}

int object_get_children(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);

    lua_newtable(state);

    const uint32_t count = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < count; i++) {
        luavgl_push_object(state, lv_obj_get_child(object, static_cast<int32_t>(i)));
        lua_seti(state, -2, static_cast<lua_Integer>(i + 1));
    }

    return 1;
}

int object_center(lua_State* state) {
    lv_obj_center(luavgl_check_object(state, 1));
    lua_settop(state, 1);
    return 1;
}

int object_child_count(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    lua_pushinteger(state, lv_obj_get_child_count(object));
    return 1;
}

int object_child(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    const auto index = static_cast<int32_t>(luaL_checkinteger(state, 2));
    const auto count = static_cast<int32_t>(lv_obj_get_child_count(object));

    if (index < 1 || index > count) {
        lua_pushnil(state);
        return 1;
    }

    luavgl_push_object(state, lv_obj_get_child(object, index - 1));
    return 1;
}

int object_add_state(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    lv_obj_add_state(object, static_cast<lv_state_t>(luaL_checkinteger(state, 2)));
    lua_settop(state, 1);
    return 1;
}

int object_remove_state(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    lv_obj_remove_state(object, static_cast<lv_state_t>(luaL_checkinteger(state, 2)));
    lua_settop(state, 1);
    return 1;
}

int object_has_state(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    lua_pushboolean(state, lv_obj_has_state(object, static_cast<lv_state_t>(luaL_checkinteger(state, 2))));
    return 1;
}

int object_get_state(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    lua_pushinteger(state, static_cast<lua_Integer>(lv_obj_get_state(object)));
    return 1;
}

int object_scroll_to(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    const auto x = static_cast<int32_t>(luaL_checkinteger(state, 2));
    const auto y = static_cast<int32_t>(luaL_checkinteger(state, 3));
    const auto anim = lua_toboolean(state, 4) ? LV_ANIM_ON : LV_ANIM_OFF;
    lv_obj_scroll_to(object, x, y, anim);
    lua_settop(state, 1);
    return 1;
}

int object_scroll_by(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    const auto x = static_cast<int32_t>(luaL_checkinteger(state, 2));
    const auto y = static_cast<int32_t>(luaL_checkinteger(state, 3));
    const auto anim = lua_toboolean(state, 4) ? LV_ANIM_ON : LV_ANIM_OFF;
    lv_obj_scroll_by(object, x, y, anim);
    lua_settop(state, 1);
    return 1;
}

int object_scroll_x(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    lua_pushinteger(state, lv_obj_get_scroll_x(object));
    return 1;
}

int object_scroll_y(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    lua_pushinteger(state, lv_obj_get_scroll_y(object));
    return 1;
}

int object_move_foreground(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    lv_obj_move_foreground(object);
    lua_settop(state, 1);
    return 1;
}

int object_move_background(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    lv_obj_move_background(object);
    lua_settop(state, 1);
    return 1;
}

/**
 * Forces LVGL to lay the subtree out now.
 *
 * Sizes and positions are not readable until a layout pass has run, so a script that sets
 * a size and immediately reads it back gets 0 without this. LVGL does the pass itself once
 * per frame; this is for scripts that need the geometry before then.
 */
int object_update_layout(lua_State* state) {
    lv_obj_update_layout(luavgl_check_object(state, 1));
    lua_settop(state, 1);
    return 1;
}

int object_get_size(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    lua_pushinteger(state, lv_obj_get_width(object));
    lua_pushinteger(state, lv_obj_get_height(object));
    return 2;
}

int object_get_position(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    lua_pushinteger(state, lv_obj_get_x(object));
    lua_pushinteger(state, lv_obj_get_y(object));
    return 2;
}

/**
 * The exposed `lvgl.FLAG` values and their dedicated setters/getters.
 *
 * LVGL 9.6 deprecates `lv_obj_add_flag()`, `lv_obj_remove_flag()` and `lv_obj_has_flag()`
 * in favour of one pair per flag. Keeping the Lua methods is useful (`add_flag` reads
 * better with OR-ed values), but they must call the supported API underneath.
 */
struct ObjectFlag {
    lv_obj_flag_t flag;
    void (*set)(lv_obj_t*, bool);
    bool (*get)(const lv_obj_t*);
};

/** `LV_OBJ_FLAG_SCROLL_CHAIN` is the two directional bits together. */
bool is_scroll_chain(const lv_obj_t* object) {
    return lv_obj_is_scroll_chain_hor(object) && lv_obj_is_scroll_chain_ver(object);
}

const ObjectFlag OBJECT_FLAGS[] = {
    { LV_OBJ_FLAG_HIDDEN, lv_obj_set_hidden, lv_obj_is_hidden },
    { LV_OBJ_FLAG_CLICKABLE, lv_obj_set_clickable, lv_obj_is_clickable },
    { LV_OBJ_FLAG_CLICK_FOCUSABLE, lv_obj_set_click_focusable, lv_obj_is_click_focusable },
    { LV_OBJ_FLAG_CHECKABLE, lv_obj_set_checkable, lv_obj_is_checkable },
    { LV_OBJ_FLAG_SCROLLABLE, lv_obj_set_scrollable, lv_obj_is_scrollable },
    { LV_OBJ_FLAG_SCROLL_ELASTIC, lv_obj_set_scroll_elastic, lv_obj_is_scroll_elastic },
    { LV_OBJ_FLAG_SCROLL_MOMENTUM, lv_obj_set_scroll_momentum, lv_obj_is_scroll_momentum },
    { LV_OBJ_FLAG_SCROLL_CHAIN, lv_obj_set_scroll_chain, is_scroll_chain },
    { LV_OBJ_FLAG_SCROLL_ON_FOCUS, lv_obj_set_scroll_on_focus, lv_obj_is_scroll_on_focus },
    { LV_OBJ_FLAG_SCROLL_WITH_ARROW, lv_obj_set_scroll_with_arrow, lv_obj_is_scroll_with_arrow },
    { LV_OBJ_FLAG_SNAPPABLE, lv_obj_set_snappable, lv_obj_is_snappable },
    { LV_OBJ_FLAG_PRESS_LOCK, lv_obj_set_press_lock, lv_obj_is_press_lock },
    { LV_OBJ_FLAG_EVENT_BUBBLE, lv_obj_set_event_bubble, lv_obj_is_event_bubble },
    { LV_OBJ_FLAG_GESTURE_BUBBLE, lv_obj_set_gesture_bubble, lv_obj_is_gesture_bubble },
    { LV_OBJ_FLAG_ADV_HITTEST, lv_obj_set_adv_hittest, lv_obj_is_adv_hittest },
    { LV_OBJ_FLAG_IGNORE_LAYOUT, lv_obj_set_ignore_layout, lv_obj_is_ignore_layout },
    { LV_OBJ_FLAG_FLOATING, lv_obj_set_floating, lv_obj_is_floating },
    { LV_OBJ_FLAG_OVERFLOW_VISIBLE, lv_obj_set_overflow_visible, lv_obj_is_overflow_visible },
    { LV_OBJ_FLAG_FLEX_IN_NEW_TRACK, lv_obj_set_flex_in_new_track, lv_obj_is_flex_in_new_track },
};

void apply_flags(lua_State* state, lv_obj_t* object, uint32_t flags, bool enabled) {
    uint32_t remaining = flags;

    for (const auto& entry : OBJECT_FLAGS) {
        if ((remaining & entry.flag) == entry.flag) {
            entry.set(object, enabled);
            remaining &= ~static_cast<uint32_t>(entry.flag);
        }
    }

    const struct {
        lv_obj_flag_t flag;
        uint32_t bit;
    } user_flags[] = {
        { LV_OBJ_FLAG_USER_1, 0 },
        { LV_OBJ_FLAG_USER_2, 1 },
        { LV_OBJ_FLAG_USER_3, 2 },
        { LV_OBJ_FLAG_USER_4, 3 },
    };

    for (const auto& entry : user_flags) {
        if (remaining & entry.flag) {
            lv_obj_set_user_flag(object, entry.bit, enabled);
            remaining &= ~static_cast<uint32_t>(entry.flag);
        }
    }

    if (remaining != 0) {
        luaL_error(state, "unsupported flag 0x%08x", static_cast<unsigned>(remaining));
    }
}

int object_add_flag(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    apply_flags(state, object, static_cast<uint32_t>(luaL_checkinteger(state, 2)), true);

    lua_settop(state, 1);
    return 1;
}

int object_remove_flag(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    apply_flags(state, object, static_cast<uint32_t>(luaL_checkinteger(state, 2)), false);

    lua_settop(state, 1);
    return 1;
}

int object_has_flag(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    uint32_t remaining = static_cast<uint32_t>(luaL_checkinteger(state, 2));
    bool all = true;

    for (const auto& entry : OBJECT_FLAGS) {
        if ((remaining & entry.flag) == entry.flag) {
            if (!entry.get(object)) {
                all = false;
            }
            remaining &= ~static_cast<uint32_t>(entry.flag);
        }
    }

    const struct {
        lv_obj_flag_t flag;
        uint32_t bit;
    } user_flags[] = {
        { LV_OBJ_FLAG_USER_1, 0 },
        { LV_OBJ_FLAG_USER_2, 1 },
        { LV_OBJ_FLAG_USER_3, 2 },
        { LV_OBJ_FLAG_USER_4, 3 },
    };

    for (const auto& entry : user_flags) {
        if (remaining & entry.flag) {
            if (!lv_obj_get_user_flag(object, entry.bit)) {
                all = false;
            }
            remaining &= ~static_cast<uint32_t>(entry.flag);
        }
    }

    if (remaining != 0) {
        return luaL_error(state, "unsupported flag 0x%08x", static_cast<unsigned>(remaining));
    }

    lua_pushboolean(state, all);
    return 1;
}

int object_tostring(lua_State* state) {
    auto* handle = static_cast<ObjectHandle*>(luaL_checkudata(state, 1, LUAVGL_OBJECT_METATABLE));

    if (handle->object == nullptr) {
        lua_pushstring(state, "lvgl.Object (deleted)");
    } else {
        lua_pushfstring(state, "lvgl.Object (%p)", handle->object);
    }
    return 1;
}

/** Identity is the underlying widget, so two handles on one widget compare equal. */
int object_equals(lua_State* state) {
    auto* left = static_cast<ObjectHandle*>(luaL_testudata(state, 1, LUAVGL_OBJECT_METATABLE));
    auto* right = static_cast<ObjectHandle*>(luaL_testudata(state, 2, LUAVGL_OBJECT_METATABLE));

    lua_pushboolean(state, left != nullptr && right != nullptr &&
                               left->object != nullptr && left->object == right->object);
    return 1;
}

const luaL_Reg object_methods[] = {
    { "set", object_set },
    { "delete", object_delete },
    { "clean", object_clean },
    { "is_valid", object_is_valid },
    { "parent", object_get_parent },
    { "children", object_get_children },
    { "child_count", object_child_count },
    { "child", object_child },
    { "center", object_center },
    { "update_layout", object_update_layout },
    { "size", object_get_size },
    { "position", object_get_position },
    { "add_flag", object_add_flag },
    { "remove_flag", object_remove_flag },
    { "has_flag", object_has_flag },
    { "add_state", object_add_state },
    { "remove_state", object_remove_state },
    { "has_state", object_has_state },
    { "state", object_get_state },
    { "scroll_to", object_scroll_to },
    { "scroll_by", object_scroll_by },
    { "scroll_x", object_scroll_x },
    { "scroll_y", object_scroll_y },
    { "move_foreground", object_move_foreground },
    { "move_background", object_move_background },
    { nullptr, nullptr }
};

} // namespace

uint32_t luavgl_next_generation() {
    return ++generation_counter;
}

bool luavgl_is_object_alive(lua_State* state, lv_obj_t* object) {
    if (object == nullptr) {
        return false;
    }

    push_tracked_objects(state);
    lua_pushlightuserdata(state, object);
    const bool alive = lua_rawget(state, -2) != LUA_TNIL;
    lua_pop(state, 2);
    return alive;
}

void luavgl_track_event_target(lua_State* state, lv_obj_t* object) {
    lua_pushlightuserdata(state, &EVENT_TARGETS_KEY);
    if (lua_rawget(state, LUA_REGISTRYINDEX) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);

        lua_pushlightuserdata(state, &EVENT_TARGETS_KEY);
        lua_pushvalue(state, -2);
        lua_rawset(state, LUA_REGISTRYINDEX);
    }

    // Keyed by the object so re-registering a callback does not add a duplicate entry
    lua_pushlightuserdata(state, object);
    lua_pushboolean(state, 1);
    lua_rawset(state, -3);

    lua_pop(state, 1);
}

/** Iterates the tracked event targets, calling `visit` for each still-live widget. */
void luavgl_for_each_event_target(lua_State* state, void (*visit)(lua_State*, lv_obj_t*)) {
    lua_pushlightuserdata(state, &EVENT_TARGETS_KEY);
    if (lua_rawget(state, LUA_REGISTRYINDEX) != LUA_TTABLE) {
        lua_pop(state, 1);
        return;
    }
    const int targets_index = lua_gettop(state);

    lua_pushnil(state);
    while (lua_next(state, targets_index) != 0) {
        lua_pop(state, 1); // the value; the key is the object

        auto* object = static_cast<lv_obj_t*>(lua_touserdata(state, -1));
        if (object == nullptr) {
            continue;
        }

        // The live set removes an address from itself when LVGL deletes the widget, so
        // this is safe even though the event-target table still remembers the address.
        if (luavgl_is_object_alive(state, object)) {
            visit(state, object);
        }
    }

    lua_pop(state, 1); // targets

    lua_pushlightuserdata(state, &EVENT_TARGETS_KEY);
    lua_pushnil(state);
    lua_rawset(state, LUA_REGISTRYINDEX);
}

void luavgl_push_object(lua_State* state, lv_obj_t* object) {
    if (object == nullptr) {
        lua_pushnil(state);
        return;
    }

    push_object_registry(state);

    // Reuse the existing handle when this widget has been wrapped before
    lua_pushlightuserdata(state, object);
    if (lua_rawget(state, -2) == LUA_TUSERDATA) {
        lua_remove(state, -2); // drop the registry, leave the handle
        return;
    }
    lua_pop(state, 1); // the nil

    auto* handle = static_cast<ObjectHandle*>(lua_newuserdatauv(state, sizeof(ObjectHandle), 0));
    handle->object = object;
    handle->generation = luavgl_next_generation();
    luaL_setmetatable(state, LUAVGL_OBJECT_METATABLE);

    // The delete callback is what keeps the handle honest once LVGL frees the widget
    lv_obj_add_event_cb(object, on_object_deleted, LV_EVENT_DELETE, state);

    lua_pushlightuserdata(state, object);
    lua_pushvalue(state, -2);
    lua_rawset(state, -4);

    lua_remove(state, -2); // drop the registry

    // Remember the address (and generation) so luavgl_detach_all() can remove the
    // callback if the runtime is closed while the widget is still alive. The delete
    // callback removes this entry, so shutdown never walks freed addresses.
    push_tracked_objects(state);
    lua_pushlightuserdata(state, object);
    lua_pushinteger(state, handle->generation);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

void luavgl_detach_all(lua_State* state) {
    // Subscriptions own their timers, so cancel them before the general timer sweep.
    luavgl_delete_all_notification_subscriptions(state);
    luavgl_delete_all_pickers(state);
    luavgl_delete_all_dialogs(state);
    luavgl_delete_all_http(state);
    luavgl_delete_all_tile_grids(state);
    luavgl_delete_all_path_bars(state);

    // Timers first: they have no widget to anchor them, so nothing else would ever notice
    // them still running against a freed state.
    luavgl_delete_all_timers(state);

    // Then any event dispatch already queued for the next LVGL cycle
    luavgl_cancel_pending_events(state);

    // Event callbacks: these sit on widgets this runtime may not own (a toolbar belongs
    // to the app's screen), so they outlive it unless removed here.
    luavgl_detach_event_targets(state);

    push_tracked_objects(state);
    const int tracked_index = lua_gettop(state);

    // Every entry is live: on_object_deleted() removes the address from this table before
    // LVGL frees the widget. The delete callback is what made the old array impossible to
    // use safely, because it remembered every address ever wrapped.
    lua_pushnil(state);
    while (lua_next(state, tracked_index) != 0) {
        auto* object = static_cast<lv_obj_t*>(lua_touserdata(state, -2));
        lua_pop(state, 1); // generation; keep the key for lua_next

        if (object == nullptr) {
            continue;
        }

        // Matches on both the callback and the user data, so other runtimes' callbacks on
        // the same widget are left alone.
        lv_obj_remove_event_cb_with_user_data(object, on_object_deleted, state);

        // A running animation writes through the widget pointer on every frame.
        lv_anim_delete(object, nullptr);

        // Styles are held by pointer, not copied, so a widget outliving this runtime would
        // dereference freed style memory on its next redraw. Removing them here is what
        // makes freeing the styles below safe.
        luavgl_remove_styles(state, object);
    }

    lua_pop(state, 1); // tracked objects

    // Groups may still be referenced by input devices. Delete them now, while the widgets
    // they point at are still alive, and before styles are freed.
    luavgl_delete_all_groups(state);

    // Only now that no widget references them
    luavgl_free_all_styles(state);

    // Drop both tables so a reopened binding starts clean
    lua_pushlightuserdata(state, &TRACKED_OBJECTS_KEY);
    lua_pushnil(state);
    lua_rawset(state, LUA_REGISTRYINDEX);

    lua_pushlightuserdata(state, &OBJECT_REGISTRY_KEY);
    lua_pushnil(state);
    lua_rawset(state, LUA_REGISTRYINDEX);
}

lv_obj_t* luavgl_check_object(lua_State* state, int index) {
    auto* handle = static_cast<ObjectHandle*>(luaL_checkudata(state, index, LUAVGL_OBJECT_METATABLE));

    if (handle->object == nullptr) {
        luaL_error(state, "widget has been deleted");
    }

    return handle->object;
}

lv_obj_t* luavgl_get_root(lua_State* state) {
    lua_pushlightuserdata(state, &ROOT_KEY);
    lua_rawget(state, LUA_REGISTRYINDEX);

    auto* root = static_cast<lv_obj_t*>(lua_touserdata(state, -1));
    lua_pop(state, 1);

    // No explicit root: fall back to the active screen, so a bare script still draws
    return root != nullptr ? root : lv_screen_active();
}

void luavgl_set_root(lua_State* state, lv_obj_t* root) {
    lua_pushlightuserdata(state, &ROOT_KEY);
    lua_pushlightuserdata(state, root);
    lua_rawset(state, LUA_REGISTRYINDEX);
}

LuavglConstructorArgs luavgl_parse_constructor_args(lua_State* state) {
    // Argument may be a parent handle, a property table, both, or neither
    LuavglConstructorArgs args{nullptr, 0};

    if (luaL_testudata(state, 1, LUAVGL_OBJECT_METATABLE) != nullptr) {
        args.parent = luavgl_check_object(state, 1);
        if (lua_istable(state, 2)) {
            args.properties_index = lua_absindex(state, 2);
        }
    } else if (lua_istable(state, 1)) {
        args.properties_index = lua_absindex(state, 1);
    }

    if (args.parent == nullptr) {
        args.parent = luavgl_get_root(state);
    }

    return args;
}

int luavgl_create_widget(lua_State* state, WidgetCreate create) {
    const auto args = luavgl_parse_constructor_args(state);

    auto* object = create(args.parent);
    if (object == nullptr) {
        return luaL_error(state, "failed to create widget");
    }

    luavgl_push_object(state, object);

    if (args.properties_index != 0) {
        luavgl_apply_properties(state, object, args.properties_index);
    }

    return 1;
}

void luavgl_open_object(lua_State* state) {
    luaL_newmetatable(state, LUAVGL_OBJECT_METATABLE);

    luaL_newlib(state, object_methods);
    luavgl_add_event_methods(state);  // on()/off(), in events.cpp
    luavgl_add_widget_methods(state); // value()/text()/checked, in widgets.cpp
    luavgl_add_style_methods(state);  // add_style()/remove_style(), in style.cpp
    lua_setfield(state, -2, "__index");

    lua_pushcfunction(state, object_tostring);
    lua_setfield(state, -2, "__tostring");

    lua_pushcfunction(state, object_equals);
    lua_setfield(state, -2, "__eq");

    // No __gc: LVGL owns the widget, and collecting a handle must not delete it. A script
    // that wants the widget gone calls delete() explicitly, exactly as in C.

    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");

    lua_pop(state, 1);
}
