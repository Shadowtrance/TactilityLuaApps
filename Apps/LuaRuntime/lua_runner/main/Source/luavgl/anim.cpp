// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

#include <lvgl/widgets/sliderbox.h>

#include <cstring>

extern "C" {
#include <lauxlib.h>
}

/**
 * Numeric widget animations.
 *
 * A script describes an animation with `lvgl.Anim(target, { ... })`, then calls `start()`.
 * The per-frame callback is C, not Lua: it writes one property through LVGL's setter for
 * that property. That avoids entering the interpreter from the animation timer sixty times
 * a second, and keeps the widget access on the same task LVGL already owns.
 *
 * There is deliberately no Lua `exec` callback yet. Completion/focus callbacks are the
 * next step; the numeric surface covers fades, slides, resizes, colour transitions and
 * value sweeps without the interpreter in the frame loop.
 *
 * Lifetime:
 *  - A deleted widget stops its animations via on_object_deleted().
 *  - luavgl_detach_all() stops animations on every live widget it owns.
 *  - The handle guards `start()`/`stop()` with the runtime's live set, so using a handle
 *    after its target has gone raises instead of dereferencing freed memory.
 */

namespace {

inline constexpr auto* ANIM_METATABLE = "lvgl.Anim";

enum class AnimProperty {
    X,
    Y,
    Width,
    Height,
    Opa,
    BgOpa,
    Radius,
    BorderWidth,
    TextOpa,
    TransformRotation,
    TransformScaleX,
    TransformScaleY,
    TransformWidth,
    TransformHeight,
    BgColor,
    TextColor,
    BorderColor,
    Value,
};

void exec_x(void* var, int32_t value) {
    lv_obj_set_x(static_cast<lv_obj_t*>(var), value);
}

void exec_y(void* var, int32_t value) {
    lv_obj_set_y(static_cast<lv_obj_t*>(var), value);
}

void exec_width(void* var, int32_t value) {
    lv_obj_set_width(static_cast<lv_obj_t*>(var), value);
}

void exec_height(void* var, int32_t value) {
    lv_obj_set_height(static_cast<lv_obj_t*>(var), value);
}

void exec_opa(void* var, int32_t value) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(value), LV_PART_MAIN);
}

void exec_bg_opa(void* var, int32_t value) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(value), LV_PART_MAIN);
}

void exec_radius(void* var, int32_t value) {
    lv_obj_set_style_radius(static_cast<lv_obj_t*>(var), value, LV_PART_MAIN);
}

void exec_border_width(void* var, int32_t value) {
    lv_obj_set_style_border_width(static_cast<lv_obj_t*>(var), value, LV_PART_MAIN);
}

void exec_text_opa(void* var, int32_t value) {
    lv_obj_set_style_text_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(value), LV_PART_MAIN);
}

void exec_transform_rotation(void* var, int32_t value) {
    lv_obj_set_style_transform_rotation(static_cast<lv_obj_t*>(var), value, LV_PART_MAIN);
}

void exec_transform_scale_x(void* var, int32_t value) {
    lv_obj_set_style_transform_scale_x(static_cast<lv_obj_t*>(var), value, LV_PART_MAIN);
}

void exec_transform_scale_y(void* var, int32_t value) {
    lv_obj_set_style_transform_scale_y(static_cast<lv_obj_t*>(var), value, LV_PART_MAIN);
}

void exec_transform_width(void* var, int32_t value) {
    lv_obj_set_style_transform_width(static_cast<lv_obj_t*>(var), value, LV_PART_MAIN);
}

void exec_transform_height(void* var, int32_t value) {
    lv_obj_set_style_transform_height(static_cast<lv_obj_t*>(var), value, LV_PART_MAIN);
}

void exec_bg_color(void* var, int32_t value) {
    lv_obj_set_style_bg_color(static_cast<lv_obj_t*>(var), lv_color_hex(static_cast<uint32_t>(value)), LV_PART_MAIN);
}

void exec_text_color(void* var, int32_t value) {
    lv_obj_set_style_text_color(static_cast<lv_obj_t*>(var), lv_color_hex(static_cast<uint32_t>(value)), LV_PART_MAIN);
}

void exec_border_color(void* var, int32_t value) {
    lv_obj_set_style_border_color(static_cast<lv_obj_t*>(var), lv_color_hex(static_cast<uint32_t>(value)), LV_PART_MAIN);
}

/** Animate the value of whichever value widget this is. */
void exec_value(void* var, int32_t value) {
    auto* object = static_cast<lv_obj_t*>(var);

    if (luavgl_is_sliderbox(object)) {
        lvgl_sliderbox_set_value(object, value, LV_ANIM_OFF);
    } else if (lv_obj_check_type(object, &lv_slider_class)) {
        lv_slider_set_value(object, value, LV_ANIM_OFF);
    } else if (lv_obj_check_type(object, &lv_bar_class)) {
        lv_bar_set_value(object, value, LV_ANIM_OFF);
    } else if (luavgl_is_arc(object)) {
        lv_arc_set_value(object, value);
    } else if (luavgl_is_spinbox(object)) {
        lv_spinbox_set_value(object, value);
    }
}

struct AnimPropertyEntry {
    const char* name;
    AnimProperty property;
    lv_anim_exec_xcb_t exec;
};

const AnimPropertyEntry ANIM_PROPERTIES[] = {
    { "x", AnimProperty::X, exec_x },
    { "y", AnimProperty::Y, exec_y },
    { "width", AnimProperty::Width, exec_width },
    { "w", AnimProperty::Width, exec_width },
    { "height", AnimProperty::Height, exec_height },
    { "h", AnimProperty::Height, exec_height },
    { "opa", AnimProperty::Opa, exec_opa },
    { "bg_opa", AnimProperty::BgOpa, exec_bg_opa },
    { "radius", AnimProperty::Radius, exec_radius },
    { "border_width", AnimProperty::BorderWidth, exec_border_width },
    { "text_opa", AnimProperty::TextOpa, exec_text_opa },
    { "transform_rotation", AnimProperty::TransformRotation, exec_transform_rotation },
    { "transform_scale_x", AnimProperty::TransformScaleX, exec_transform_scale_x },
    { "transform_scale_y", AnimProperty::TransformScaleY, exec_transform_scale_y },
    { "transform_width", AnimProperty::TransformWidth, exec_transform_width },
    { "transform_height", AnimProperty::TransformHeight, exec_transform_height },
    { "bg_color", AnimProperty::BgColor, exec_bg_color },
    { "text_color", AnimProperty::TextColor, exec_text_color },
    { "border_color", AnimProperty::BorderColor, exec_border_color },
    { "value", AnimProperty::Value, exec_value },
};

struct AnimPathEntry {
    const char* name;
    lv_anim_path_cb_t callback;
};

const AnimPathEntry ANIM_PATHS[] = {
    { "LINEAR", lv_anim_path_linear },
    { "EASE_IN", lv_anim_path_ease_in },
    { "EASE_OUT", lv_anim_path_ease_out },
    { "EASE_IN_OUT", lv_anim_path_ease_in_out },
    { "OVERSHOOT", lv_anim_path_overshoot },
    { "BOUNCE", lv_anim_path_bounce },
    { "STEP", lv_anim_path_step },
};

struct AnimHandle {
    lv_obj_t* target;
    AnimProperty property;
    lv_anim_exec_xcb_t exec;
    int32_t from;
    int32_t to;
    uint32_t time;
    uint32_t delay;
    uint32_t repeat_count;
    uint32_t repeat_delay;
    uint32_t reverse;
    uint32_t reverse_delay;
    lv_anim_path_cb_t path;
};

AnimHandle* check_anim(lua_State* state, int index) {
    return static_cast<AnimHandle*>(luaL_checkudata(state, index, ANIM_METATABLE));
}

const AnimPropertyEntry* find_property(const char* name) {
    // Indexed rather than range-based: on Xtensa the ELF loader maps a one-past-the-end
    // array pointer (the implicit range-for sentinel) to NULL, which makes the loop run
    // past the table. Avoid end pointers in tables that live in .data.rel.ro.
    for (size_t i = 0; i < sizeof(ANIM_PROPERTIES) / sizeof(ANIM_PROPERTIES[0]); i++) {
        if (std::strcmp(name, ANIM_PROPERTIES[i].name) == 0) {
            return &ANIM_PROPERTIES[i];
        }
    }
    return nullptr;
}

int anim_create(lua_State* state) {
    auto* target = luavgl_check_object(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);

    lua_getfield(state, 2, "property");
    const char* property_name = luaL_checkstring(state, -1);
    const AnimPropertyEntry* property = find_property(property_name);
    if (property == nullptr) {
        return luaL_error(state, "unknown animation property '%s'", property_name);
    }
    lua_pop(state, 1);

    auto* handle = static_cast<AnimHandle*>(lua_newuserdatauv(state, sizeof(AnimHandle), 0));
    handle->target = target;
    handle->property = property->property;
    handle->exec = property->exec;
    handle->time = 300;
    handle->delay = 0;
    handle->repeat_count = 0;
    handle->repeat_delay = 0;
    handle->reverse = 0;
    handle->reverse_delay = 0;
    handle->path = lv_anim_path_ease_in_out;

    lua_getfield(state, 2, "from");
    handle->from = static_cast<int32_t>(luaL_checkinteger(state, -1));
    lua_pop(state, 1);

    lua_getfield(state, 2, "to");
    handle->to = static_cast<int32_t>(luaL_checkinteger(state, -1));
    lua_pop(state, 1);

    lua_getfield(state, 2, "time");
    if (!lua_isnil(state, -1)) {
        handle->time = static_cast<uint32_t>(luaL_checkinteger(state, -1));
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "delay");
    if (!lua_isnil(state, -1)) {
        handle->delay = static_cast<uint32_t>(luaL_checkinteger(state, -1));
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "repeat_count");
    if (!lua_isnil(state, -1)) {
        handle->repeat_count = static_cast<uint32_t>(luaL_checkinteger(state, -1));
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "repeat_delay");
    if (!lua_isnil(state, -1)) {
        handle->repeat_delay = static_cast<uint32_t>(luaL_checkinteger(state, -1));
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "reverse");
    if (!lua_isnil(state, -1)) {
        handle->reverse = static_cast<uint32_t>(luaL_checkinteger(state, -1));
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "reverse_delay");
    if (!lua_isnil(state, -1)) {
        handle->reverse_delay = static_cast<uint32_t>(luaL_checkinteger(state, -1));
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "path");
    if (!lua_isnil(state, -1)) {
        if (lua_type(state, -1) != LUA_TLIGHTUSERDATA) {
            return luaL_error(state, "path must be a lvgl.ANIM_PATH value");
        }
        handle->path = reinterpret_cast<lv_anim_path_cb_t>(lua_touserdata(state, -1));
    }
    lua_pop(state, 1);

    luaL_setmetatable(state, ANIM_METATABLE);
    return 1;
}

int anim_start(lua_State* state) {
    auto* handle = check_anim(state, 1);

    if (!luavgl_is_object_alive(state, handle->target)) {
        return luaL_error(state, "animation target has been deleted");
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, handle->target);
    lv_anim_set_exec_cb(&anim, handle->exec);
    lv_anim_set_values(&anim, handle->from, handle->to);
    lv_anim_set_duration(&anim, handle->time);
    lv_anim_set_delay(&anim, handle->delay);
    lv_anim_set_path_cb(&anim, handle->path);

    if (handle->repeat_count != 0) {
        lv_anim_set_repeat_count(&anim, handle->repeat_count);
        lv_anim_set_repeat_delay(&anim, handle->repeat_delay);
    }
    if (handle->reverse != 0) {
        lv_anim_set_reverse_duration(&anim, handle->reverse);
        lv_anim_set_reverse_delay(&anim, handle->reverse_delay);
    }

    lv_anim_start(&anim);

    lua_settop(state, 1);
    return 1;
}

int anim_stop(lua_State* state) {
    auto* handle = check_anim(state, 1);

    if (luavgl_is_object_alive(state, handle->target)) {
        lv_anim_delete(handle->target, handle->exec);
    }

    lua_settop(state, 1);
    return 1;
}

/** True while LVGL still has an animation for this target and property. */
int anim_started(lua_State* state) {
    auto* handle = check_anim(state, 1);

    if (!luavgl_is_object_alive(state, handle->target)) {
        lua_pushboolean(state, false);
        return 1;
    }

    lua_pushboolean(state, lv_anim_get(handle->target, handle->exec) != nullptr);
    return 1;
}

int anim_target(lua_State* state) {
    auto* handle = check_anim(state, 1);

    if (!luavgl_is_object_alive(state, handle->target)) {
        lua_pushnil(state);
    } else {
        luavgl_push_object(state, handle->target);
    }

    return 1;
}

int anim_tostring(lua_State* state) {
    auto* handle = check_anim(state, 1);

    for (size_t i = 0; i < sizeof(ANIM_PROPERTIES) / sizeof(ANIM_PROPERTIES[0]); i++) {
        const auto& entry = ANIM_PROPERTIES[i];
        if (entry.property == handle->property) {
            lua_pushfstring(state, "lvgl.Anim (%s %d -> %d)", entry.name,
                            static_cast<int>(handle->from), static_cast<int>(handle->to));
            return 1;
        }
    }

    lua_pushstring(state, "lvgl.Anim");
    return 1;
}

const luaL_Reg anim_methods[] = {
    { "start", anim_start },
    { "stop", anim_stop },
    { "is_started", anim_started },
    { "target", anim_target },
    { nullptr, nullptr }
};

} // namespace

void luavgl_open_anim(lua_State* state) {
    luaL_newmetatable(state, ANIM_METATABLE);
    lua_newtable(state);

    for (const luaL_Reg* method = anim_methods; method->name != nullptr; method++) {
        lua_pushcfunction(state, method->func);
        lua_setfield(state, -2, method->name);
    }

    lua_pushcfunction(state, anim_tostring);
    lua_setfield(state, -2, "__tostring");
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1); // metatable

    lua_pushcfunction(state, anim_create);
    lua_setfield(state, -2, "Anim");

    lua_createtable(state, 0, sizeof(ANIM_PATHS) / sizeof(ANIM_PATHS[0]));
    // See find_property() above: never let the compiler turn the table end into a
    // one-past-the-end sentinel, because the Xtensa section loader maps that to NULL.
    for (size_t i = 0; i < sizeof(ANIM_PATHS) / sizeof(ANIM_PATHS[0]); i++) {
        const auto& entry = ANIM_PATHS[i];
        lua_pushlightuserdata(state, reinterpret_cast<void*>(entry.callback));
        lua_setfield(state, -2, entry.name);
    }
    lua_setfield(state, -2, "ANIM_PATH");

    lua_pushinteger(state, LV_ANIM_REPEAT_INFINITE);
    lua_setfield(state, -2, "ANIM_INFINITE");
}
