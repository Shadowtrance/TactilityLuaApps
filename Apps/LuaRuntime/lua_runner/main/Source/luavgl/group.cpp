// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

extern "C" {
#include <lauxlib.h>
}

/**
 * Input groups: `lvgl.Group()` plus focus navigation.
 *
 * Groups are what makes a device with an encoder or keyboard usable without a pointer:
 * LVGL sends key events to the focused object. The binding keeps only the group handle;
 * adding and removing widgets is done through ordinary object handles.
 *
 * Lifetime: the group is deleted with the runtime, because an input device may still
 * point at it after the script has dropped the handle. Deleting a group is safe at any
 * point - LVGL clears the group from every input device and from the default slot.
 */

namespace {

inline constexpr auto* GROUP_METATABLE = "lvgl.Group";

/** Registry key for the array of group handles created by this runtime. */
char GROUPS_KEY = 0;

using GroupHandle = lv_group_t*;

void push_groups(lua_State* state) {
    lua_pushlightuserdata(state, &GROUPS_KEY);
    if (lua_rawget(state, LUA_REGISTRYINDEX) == LUA_TTABLE) {
        return;
    }

    lua_pop(state, 1);
    lua_newtable(state);

    lua_pushlightuserdata(state, &GROUPS_KEY);
    lua_pushvalue(state, -2);
    lua_rawset(state, LUA_REGISTRYINDEX);
}

GroupHandle check_group(lua_State* state, int index) {
    auto** handle = static_cast<GroupHandle*>(luaL_checkudata(state, index, GROUP_METATABLE));

    if (*handle == nullptr) {
        luaL_error(state, "group has been deleted");
    }

    return *handle;
}

/**
 * Input devices a group attached to, with the groups to restore on detach.
 *
 * The firmware owns the launcher's keyboard focus group. An app that takes over the
 * keyboard must put the previous group back when it closes, or the launcher loses
 * keyboard navigation.
 */
struct GroupInput {
    struct Entry {
        lv_indev_t* indev;
        lv_group_t* previous;
    };

    Entry entries[8];
    uint32_t count;
};

void detach_group_input(lv_group_t* group) {
    auto* input = static_cast<GroupInput*>(lv_group_get_user_data(group));
    if (input == nullptr) {
        return;
    }

    for (uint32_t i = 0; i < input->count; i++) {
        lv_indev_set_group(input->entries[i].indev, input->entries[i].previous);
    }

    lv_free(input);
    lv_group_set_user_data(group, nullptr);
}

void destroy_group(lv_group_t*& group) {
    if (group == nullptr) {
        return;
    }

    detach_group_input(group);
    lv_group_delete(group);
    group = nullptr;
}

int group_gc(lua_State* state) {
    auto** handle = static_cast<GroupHandle*>(luaL_checkudata(state, 1, GROUP_METATABLE));
    destroy_group(*handle);
    return 0;
}

int group_create(lua_State* state) {
    auto** handle = static_cast<GroupHandle*>(lua_newuserdatauv(state, sizeof(GroupHandle), 0));
    *handle = lv_group_create();

    if (*handle == nullptr) {
        return luaL_error(state, "failed to create group");
    }

    luaL_setmetatable(state, GROUP_METATABLE);

    // Keep the handle alive until teardown: input devices may still reference it.
    push_groups(state);
    lua_pushvalue(state, -2);
    lua_rawseti(state, -2, luaL_len(state, -2) + 1);
    lua_pop(state, 1);

    return 1;
}

int group_add(lua_State* state) {
    lv_group_add_obj(check_group(state, 1), luavgl_check_object(state, 2));

    lua_settop(state, 1);
    return 1;
}

int group_remove(lua_State* state) {
    check_group(state, 1);
    lv_group_remove_obj(luavgl_check_object(state, 2));

    lua_settop(state, 1);
    return 1;
}

int group_remove_all(lua_State* state) {
    lv_group_remove_all_objs(check_group(state, 1));

    lua_settop(state, 1);
    return 1;
}

int group_focus(lua_State* state) {
    check_group(state, 1);
    lv_group_focus_obj(luavgl_check_object(state, 2));

    lua_settop(state, 1);
    return 1;
}

int group_focused(lua_State* state) {
    auto* object = lv_group_get_focused(check_group(state, 1));

    if (object == nullptr) {
        lua_pushnil(state);
    } else {
        luavgl_push_object(state, object);
    }

    return 1;
}

int group_next(lua_State* state) {
    lv_group_focus_next(check_group(state, 1));

    lua_settop(state, 1);
    return 1;
}

int group_prev(lua_State* state) {
    lv_group_focus_prev(check_group(state, 1));

    lua_settop(state, 1);
    return 1;
}

int group_freeze(lua_State* state) {
    luaL_checktype(state, 2, LUA_TBOOLEAN);
    lv_group_focus_freeze(check_group(state, 1), lua_toboolean(state, 2) != 0);

    lua_settop(state, 1);
    return 1;
}

int group_editing(lua_State* state) {
    lua_pushboolean(state, lv_group_get_editing(check_group(state, 1)));
    return 1;
}

int group_set_editing(lua_State* state) {
    luaL_checktype(state, 2, LUA_TBOOLEAN);
    lv_group_set_editing(check_group(state, 1), lua_toboolean(state, 2) != 0);

    lua_settop(state, 1);
    return 1;
}

int group_wrap(lua_State* state) {
    lua_pushboolean(state, lv_group_get_wrap(check_group(state, 1)));
    return 1;
}

int group_set_wrap(lua_State* state) {
    luaL_checktype(state, 2, LUA_TBOOLEAN);
    lv_group_set_wrap(check_group(state, 1), lua_toboolean(state, 2) != 0);

    lua_settop(state, 1);
    return 1;
}

int group_count(lua_State* state) {
    lua_pushinteger(state, lv_group_get_obj_count(check_group(state, 1)));
    return 1;
}

/** `group:obj(index)` - 1-based, like every other Lua index in this binding. */
int group_obj(lua_State* state) {
    auto* group = check_group(state, 1);
    const auto index = luaL_checkinteger(state, 2);
    if (index < 1) {
        return luaL_error(state, "object index is 1-based");
    }

    auto* object = lv_group_get_obj_by_index(group, static_cast<uint32_t>(index - 1));
    if (object == nullptr) {
        lua_pushnil(state);
    } else {
        luavgl_push_object(state, object);
    }

    return 1;
}

int group_set_default(lua_State* state) {
    lv_group_set_default(check_group(state, 1));

    lua_settop(state, 1);
    return 1;
}

/** `group:attach_input()` - direct keypads and encoders at this group. */
int group_attach_input(lua_State* state) {
    auto* group = check_group(state, 1);

    detach_group_input(group);

    auto* input = static_cast<GroupInput*>(lv_malloc(sizeof(GroupInput)));
    if (input == nullptr) {
        return luaL_error(state, "out of memory for input group");
    }
    input->count = 0;

    for (auto* indev = lv_indev_get_next(nullptr); indev != nullptr; indev = lv_indev_get_next(indev)) {
        const auto type = lv_indev_get_type(indev);
        if (type != LV_INDEV_TYPE_KEYPAD && type != LV_INDEV_TYPE_ENCODER) {
            continue;
        }

        if (input->count >= sizeof(input->entries) / sizeof(input->entries[0])) {
            break;
        }

        input->entries[input->count++] = { indev, lv_indev_get_group(indev) };
        lv_indev_set_group(indev, group);
    }

    lv_group_set_user_data(group, input);

    lua_settop(state, 1);
    lua_pushinteger(state, input->count);
    return 2;
}

int group_detach_input(lua_State* state) {
    detach_group_input(check_group(state, 1));

    lua_settop(state, 1);
    return 1;
}

int group_is_default(lua_State* state) {
    lua_pushboolean(state, lv_group_get_default() == check_group(state, 1));
    return 1;
}

int group_delete(lua_State* state) {
    auto** handle = static_cast<GroupHandle*>(luaL_checkudata(state, 1, GROUP_METATABLE));
    destroy_group(*handle);
    return 0;
}

const luaL_Reg group_methods[] = {
    { "add", group_add },
    { "remove", group_remove },
    { "remove_all", group_remove_all },
    { "focus", group_focus },
    { "focused", group_focused },
    { "next", group_next },
    { "prev", group_prev },
    { "freeze", group_freeze },
    { "editing", group_editing },
    { "set_editing", group_set_editing },
    { "wrap", group_wrap },
    { "set_wrap", group_set_wrap },
    { "count", group_count },
    { "obj", group_obj },
    { "set_default", group_set_default },
    { "is_default", group_is_default },
    { "attach_input", group_attach_input },
    { "detach_input", group_detach_input },
    { "delete", group_delete },
    { nullptr, nullptr }
};

} // namespace

void luavgl_open_group(lua_State* state) {
    luaL_newmetatable(state, GROUP_METATABLE);
    lua_newtable(state);

    for (const luaL_Reg* method = group_methods; method->name != nullptr; method++) {
        lua_pushcfunction(state, method->func);
        lua_setfield(state, -2, method->name);
    }

    lua_pushcfunction(state, group_gc);
    lua_setfield(state, -2, "__gc");
    lua_pushcfunction(state, group_gc);
    lua_setfield(state, -2, "__close");

    lua_setfield(state, -2, "__index");
    lua_pop(state, 1); // metatable

    lua_pushcfunction(state, group_create);
    lua_setfield(state, -2, "Group");
}

void luavgl_delete_all_groups(lua_State* state) {
    lua_pushlightuserdata(state, &GROUPS_KEY);
    if (lua_rawget(state, LUA_REGISTRYINDEX) != LUA_TTABLE) {
        lua_pop(state, 1);
        return;
    }

    const lua_Integer count = luaL_len(state, -1);
    for (lua_Integer i = 1; i <= count; i++) {
        lua_rawgeti(state, -1, i);

        if (auto** handle = static_cast<GroupHandle*>(lua_touserdata(state, -1))) {
            destroy_group(*handle);
        }

        lua_pop(state, 1);
    }

    lua_pop(state, 1); // groups

    lua_pushlightuserdata(state, &GROUPS_KEY);
    lua_pushnil(state);
    lua_rawset(state, LUA_REGISTRYINDEX);
}
