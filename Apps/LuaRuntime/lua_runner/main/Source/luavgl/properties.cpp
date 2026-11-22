// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

#include <lvgl/widgets/sliderbox.h>

#include <cstring>

extern "C" {
#include <lauxlib.h>
}

/**
 * The declarative property setter behind `obj:set{ ... }`.
 *
 * One dispatcher over a name table rather than one binding per LVGL setter. This is the
 * single idea that keeps a widget binding to a few files instead of a few hundred: LVGL
 * has thousands of setters, and a script writes
 *
 *     label:set{ text = "hi", align = lvgl.ALIGN.CENTER, text_color = 0xFF0000 }
 *
 * rather than four separate calls.
 */

namespace {

/** Reads an integer, accepting LVGL's size constants unchanged. */
int32_t to_coordinate(lua_State* state, int index) {
    return static_cast<int32_t>(luaL_checkinteger(state, index));
}

/** Colours are given as 0xRRGGBB integers, which is how a script naturally writes them. */
lv_color_t to_color(lua_State* state, int index) {
    const auto value = static_cast<uint32_t>(luaL_checkinteger(state, index));
    return lv_color_hex(value);
}

using PropertySetter = void (*)(lua_State* state, lv_obj_t* object, int value_index);

struct Property {
    const char* name;
    PropertySetter set;
};

// Geometry ------------------------------------------------------------------------------

void set_width(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_width(object, to_coordinate(state, index));
}

void set_height(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_height(object, to_coordinate(state, index));
}

void set_x(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_x(object, to_coordinate(state, index));
}

void set_y(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_y(object, to_coordinate(state, index));
}

void set_align(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_align(object, static_cast<lv_align_t>(luaL_checkinteger(state, index)));
}

/** `layout = lvgl.LAYOUT.NONE/FLEX/GRID` - mainly to turn flex off for absolute layouts. */
void set_layout(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_layout(object, static_cast<uint32_t>(luaL_checkinteger(state, index)));
}

// Text ----------------------------------------------------------------------------------

/**
 * Only meaningful on widgets with text. Checking the class keeps a typo like
 * `button:set{ text = ... }` from writing through the wrong widget's memory - LVGL's
 * setters do not validate this themselves.
 */
void set_text(lua_State* state, lv_obj_t* object, int index) {
    const char* text = luaL_checkstring(state, index);

    if (luavgl_is_chip(object)) {
        // The chip's class does not create a label, so the constructor does and keeps it
        // as the first child.
        auto* label = lv_obj_get_child(object, 0);
        if (label != nullptr) {
            lv_label_set_text(label, text);
        }
    } else if (lv_obj_check_type(object, &lv_label_class)) {
        lv_label_set_text(object, text);
    } else if (lv_obj_check_type(object, &lv_textarea_class)) {
        lv_textarea_set_text(object, text);
    } else if (lv_obj_check_type(object, &lv_checkbox_class)) {
        lv_checkbox_set_text(object, text);
    } else {
        luaL_error(state, "this widget has no text");
    }
}

void set_long_mode(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_label_class)) {
        luaL_error(state, "long_mode only applies to labels");
    }
    lv_label_set_long_mode(object, static_cast<lv_label_long_mode_t>(luaL_checkinteger(state, index)));
}

// Value widgets ---------------------------------------------------------------------------

/** Slider and bar share a value model; dropdown and roller select by index. */
void set_value(lua_State* state, lv_obj_t* object, int index) {
    const auto value = static_cast<int32_t>(luaL_checkinteger(state, index));

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
    } else if (lv_obj_check_type(object, &lv_dropdown_class)) {
        // 1-based in Lua, 0-based in LVGL
        lv_dropdown_set_selected(object, static_cast<uint32_t>(value - 1));
    } else if (lv_obj_check_type(object, &lv_roller_class)) {
        lv_roller_set_selected(object, static_cast<uint32_t>(value - 1), LV_ANIM_OFF);
    } else {
        luaL_error(state, "this widget has no value");
    }
}

void set_min(lua_State* state, lv_obj_t* object, int index) {
    const auto value = static_cast<int32_t>(luaL_checkinteger(state, index));

    if (luavgl_is_sliderbox(object)) {
        luaL_error(state, "min is fixed when the SliderBox is created");
    } else if (lv_obj_check_type(object, &lv_slider_class)) {
        lv_slider_set_min_value(object, value);
    } else if (lv_obj_check_type(object, &lv_bar_class)) {
        lv_bar_set_min_value(object, value);
    } else if (luavgl_is_arc(object)) {
        lv_arc_set_range(object, value, lv_arc_get_max_value(object));
    } else if (lv_obj_check_type(object, &lv_scale_class)) {
        lv_scale_set_min_value(object, value);
    } else {
        luaL_error(state, "min only applies to sliders, bars, arcs and scales");
    }
}

void set_max(lua_State* state, lv_obj_t* object, int index) {
    const auto value = static_cast<int32_t>(luaL_checkinteger(state, index));

    if (luavgl_is_sliderbox(object)) {
        luaL_error(state, "max is fixed when the SliderBox is created");
    } else if (lv_obj_check_type(object, &lv_slider_class)) {
        lv_slider_set_max_value(object, value);
    } else if (lv_obj_check_type(object, &lv_bar_class)) {
        lv_bar_set_max_value(object, value);
    } else if (luavgl_is_arc(object)) {
        lv_arc_set_range(object, lv_arc_get_min_value(object), value);
    } else if (lv_obj_check_type(object, &lv_scale_class)) {
        lv_scale_set_max_value(object, value);
    } else {
        luaL_error(state, "max only applies to sliders, bars, arcs and scales");
    }
}

void set_range(lua_State* state, lv_obj_t* object, int index) {
    luaL_checktype(state, index, LUA_TTABLE);
    lua_getfield(state, index, "min");
    lua_getfield(state, index, "max");
    const auto min = static_cast<int32_t>(luaL_checkinteger(state, -2));
    const auto max = static_cast<int32_t>(luaL_checkinteger(state, -1));

    if (luavgl_is_arc(object)) {
        lv_arc_set_range(object, min, max);
    } else if (luavgl_is_spinbox(object)) {
        lv_spinbox_set_range(object, min, max);
    } else if (lv_obj_check_type(object, &lv_scale_class)) {
        lv_scale_set_range(object, min, max);
    } else {
        luaL_error(state, "range only applies to arcs, spinboxes and scales");
    }

    lua_pop(state, 2);
}

void set_spin_step(lua_State* state, lv_obj_t* object, int index) {
    if (!luavgl_is_spinbox(object)) {
        luaL_error(state, "spin_step only applies to spinboxes");
    }
    lv_spinbox_set_step(object, static_cast<uint32_t>(luaL_checkinteger(state, index)));
}

void set_digit_format(lua_State* state, lv_obj_t* object, int index) {
    if (!luavgl_is_spinbox(object)) {
        luaL_error(state, "digit_format only applies to spinboxes");
    }
    luaL_checktype(state, index, LUA_TTABLE);
    lua_getfield(state, index, "digits");
    lua_getfield(state, index, "decimals");
    lv_spinbox_set_digit_format(
        object,
        static_cast<uint32_t>(luaL_checkinteger(state, -2)),
        static_cast<uint32_t>(luaL_checkinteger(state, -1))
    );
    lua_pop(state, 2);
}

void set_arc_start(lua_State* state, lv_obj_t* object, int index) {
    if (!luavgl_is_arc(object)) {
        luaL_error(state, "arc_start only applies to arcs");
    }
    lv_arc_set_angles(object, static_cast<uint16_t>(luaL_checkinteger(state, index)),
                      lv_arc_get_angle_end(object));
}

void set_arc_end(lua_State* state, lv_obj_t* object, int index) {
    if (!luavgl_is_arc(object)) {
        luaL_error(state, "arc_end only applies to arcs");
    }
    lv_arc_set_angles(object, lv_arc_get_angle_start(object),
                      static_cast<uint16_t>(luaL_checkinteger(state, index)));
}

void set_arc_bg_start(lua_State* state, lv_obj_t* object, int index) {
    if (!luavgl_is_arc(object)) {
        luaL_error(state, "arc_bg_start only applies to arcs");
    }
    lv_arc_set_bg_angles(object, static_cast<uint16_t>(luaL_checkinteger(state, index)),
                         lv_arc_get_bg_angle_end(object));
}

void set_arc_bg_end(lua_State* state, lv_obj_t* object, int index) {
    if (!luavgl_is_arc(object)) {
        luaL_error(state, "arc_bg_end only applies to arcs");
    }
    lv_arc_set_bg_angles(object, lv_arc_get_bg_angle_start(object),
                         static_cast<uint16_t>(luaL_checkinteger(state, index)));
}

void set_arc_rotation(lua_State* state, lv_obj_t* object, int index) {
    if (!luavgl_is_arc(object)) {
        luaL_error(state, "arc_rotation only applies to arcs");
    }
    lv_arc_set_rotation(object, static_cast<uint16_t>(luaL_checkinteger(state, index)));
}

/**
 * `options` takes a Lua array, so a script writes { "One", "Two" } rather than assembling
 * LVGL's newline-separated string itself.
 */
void set_options(lua_State* state, lv_obj_t* object, int index) {
    const bool is_dropdown = lv_obj_check_type(object, &lv_dropdown_class);
    if (!is_dropdown && !lv_obj_check_type(object, &lv_roller_class)) {
        luaL_error(state, "options only applies to dropdowns and rollers");
    }

    luaL_checktype(state, index, LUA_TTABLE);

    luaL_Buffer buffer;
    luaL_buffinit(state, &buffer);

    const lua_Integer count = luaL_len(state, index);
    for (lua_Integer i = 1; i <= count; i++) {
        if (i > 1) {
            luaL_addchar(&buffer, '\n');
        }

        lua_geti(state, index, i);
        luaL_addstring(&buffer, luaL_tolstring(state, -1, nullptr));
        lua_pop(state, 2); // the tolstring result and the entry
    }

    luaL_pushresult(&buffer);

    if (is_dropdown) {
        lv_dropdown_set_options(object, lua_tostring(state, -1));
    } else {
        lv_roller_set_options(object, lua_tostring(state, -1), LV_ROLLER_MODE_NORMAL);
    }

    lua_pop(state, 1);
}

void set_checked(lua_State* state, lv_obj_t* object, int index) {
    luaL_checktype(state, index, LUA_TBOOLEAN);

    if (!lv_obj_check_type(object, &lv_switch_class) &&
        !lv_obj_check_type(object, &lv_checkbox_class) &&
        !luavgl_is_chip(object)) {
        luaL_error(state, "checked only applies to switches, checkboxes and chips");
    }

    if (lua_toboolean(state, index)) {
        lv_obj_add_state(object, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(object, LV_STATE_CHECKED);
    }
}

/** Textarea-only conveniences. */
void set_placeholder(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_textarea_class)) {
        luaL_error(state, "placeholder only applies to text areas");
    }
    lv_textarea_set_placeholder_text(object, luaL_checkstring(state, index));
}

void set_one_line(lua_State* state, lv_obj_t* object, int index) {
    luaL_checktype(state, index, LUA_TBOOLEAN);

    if (!lv_obj_check_type(object, &lv_textarea_class)) {
        luaL_error(state, "one_line only applies to text areas");
    }
    lv_textarea_set_one_line(object, lua_toboolean(state, index) != 0);
}

void set_password_mode(lua_State* state, lv_obj_t* object, int index) {
    luaL_checktype(state, index, LUA_TBOOLEAN);

    if (!lv_obj_check_type(object, &lv_textarea_class)) {
        luaL_error(state, "password_mode only applies to text areas");
    }
    lv_textarea_set_password_mode(object, lua_toboolean(state, index) != 0);
}

/** An image source is a path the LVGL filesystem layer resolves, or a symbol string. */
void set_source(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_image_class)) {
        luaL_error(state, "src only applies to images");
    }
    lv_image_set_src(object, luavgl_lvgl_path(state, index));
}

// Batch 2 widgets ---------------------------------------------------------------------------

/**
 * Replaces the string block a property saved on `object`.
 *
 * LVGL keeps pointers to maps, tick labels and day names rather than copying them, so the
 * constructor copies them into one block and stores it in the widget's user data. The
 * previous block is freed here; the last one is left for the runtime's lifetime, matching
 * `obj:grid()` - a delete callback would run from this ELF after the app may have unloaded.
 */
void replace_owned_block(lv_obj_t* object, void* block) {
    void* previous = lv_obj_get_user_data(object);
    lv_obj_set_user_data(object, block);

    if (previous != nullptr) {
        lv_free(previous);
    }
}

void set_led_color(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_led_class)) {
        luaL_error(state, "led_color only applies to LEDs");
    }
    lv_led_set_color(object, to_color(state, index));
}

void set_brightness(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_led_class)) {
        luaL_error(state, "brightness only applies to LEDs");
    }
    lv_led_set_brightness(object, static_cast<uint8_t>(luaL_checkinteger(state, index)));
}

void set_led_on(lua_State* state, lv_obj_t* object, int index) {
    luaL_checktype(state, index, LUA_TBOOLEAN);

    if (!lv_obj_check_type(object, &lv_led_class)) {
        luaL_error(state, "on only applies to LEDs");
    }

    if (lua_toboolean(state, index)) {
        lv_led_on(object);
    } else {
        lv_led_off(object);
    }
}

void set_rows(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_table_class)) {
        luaL_error(state, "rows only applies to tables");
    }
    lv_table_set_row_count(object, static_cast<uint32_t>(luaL_checkinteger(state, index)));
}

void set_columns(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_table_class)) {
        luaL_error(state, "columns only applies to tables");
    }
    lv_table_set_column_count(object, static_cast<uint32_t>(luaL_checkinteger(state, index)));
}

void set_column_widths(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_table_class)) {
        luaL_error(state, "column_widths only applies to tables");
    }
    luaL_checktype(state, index, LUA_TTABLE);

    const lua_Integer count = luaL_len(state, index);
    for (lua_Integer i = 1; i <= count; i++) {
        lua_rawgeti(state, index, i);
        lv_table_set_column_width(
            object,
            static_cast<uint32_t>(i - 1),
            static_cast<int32_t>(luaL_checkinteger(state, -1))
        );
        lua_pop(state, 1);
    }
}

void set_selected_cell(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_table_class)) {
        luaL_error(state, "selected_cell only applies to tables");
    }
    luaL_checktype(state, index, LUA_TTABLE);

    lua_getfield(state, index, "row");
    lua_getfield(state, index, "column");
    lv_table_set_selected_cell(
        object,
        static_cast<uint16_t>(luaL_checkinteger(state, -2) - 1),
        static_cast<uint16_t>(luaL_checkinteger(state, -1) - 1)
    );
    lua_pop(state, 2);
}

void set_tick_count(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_scale_class)) {
        luaL_error(state, "tick_count only applies to scales");
    }
    lv_scale_set_total_tick_count(object, static_cast<uint32_t>(luaL_checkinteger(state, index)));
}

void set_major_tick_every(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_scale_class)) {
        luaL_error(state, "major_tick_every only applies to scales");
    }
    lv_scale_set_major_tick_every(object, static_cast<uint32_t>(luaL_checkinteger(state, index)));
}

void set_label_show(lua_State* state, lv_obj_t* object, int index) {
    luaL_checktype(state, index, LUA_TBOOLEAN);

    if (!lv_obj_check_type(object, &lv_scale_class)) {
        luaL_error(state, "label_show only applies to scales");
    }
    lv_scale_set_label_show(object, lua_toboolean(state, index) != 0);
}

void set_scale_angle(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_scale_class)) {
        luaL_error(state, "scale_angle only applies to scales");
    }
    lv_scale_set_angle_range(object, static_cast<uint32_t>(luaL_checkinteger(state, index)));
}

void set_scale_rotation(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_scale_class)) {
        luaL_error(state, "scale_rotation only applies to scales");
    }
    lv_scale_set_rotation(object, static_cast<int32_t>(luaL_checkinteger(state, index)));
}

void set_scale_mode(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_scale_class)) {
        luaL_error(state, "scale_mode only applies to scales");
    }
    lv_scale_set_mode(object, static_cast<lv_scale_mode_t>(luaL_checkinteger(state, index)));
}

/** `text_src` takes an array of labels for the major ticks. */
void set_text_src(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_scale_class)) {
        luaL_error(state, "text_src only applies to scales");
    }
    luaL_checktype(state, index, LUA_TTABLE);

    const lua_Integer count = luaL_len(state, index);

    size_t bytes = 0;
    for (lua_Integer i = 1; i <= count; i++) {
        lua_rawgeti(state, index, i);
        bytes += std::strlen(luaL_checkstring(state, -1)) + 1;
        lua_pop(state, 1);
    }

    auto* block = static_cast<char*>(
        lv_malloc(sizeof(const char*) * static_cast<size_t>(count + 1) + bytes)
    );
    if (block == nullptr) {
        luaL_error(state, "out of memory for scale labels");
    }

    auto** labels = reinterpret_cast<const char**>(block);
    char* cursor = block + sizeof(const char*) * static_cast<size_t>(count + 1);

    for (lua_Integer i = 1; i <= count; i++) {
        lua_rawgeti(state, index, i);
        const char* label = luaL_checkstring(state, -1);
        const size_t length = std::strlen(label) + 1;
        std::memcpy(cursor, label, length);
        labels[i - 1] = cursor;
        cursor += length;
        lua_pop(state, 1);
    }
    labels[count] = nullptr;

    lv_scale_set_text_src(object, labels);
    replace_owned_block(object, block);
}

void set_post_draw(lua_State* state, lv_obj_t* object, int index) {
    luaL_checktype(state, index, LUA_TBOOLEAN);

    if (!lv_obj_check_type(object, &lv_scale_class)) {
        luaL_error(state, "post_draw only applies to scales");
    }
    lv_scale_set_post_draw(object, lua_toboolean(state, index) != 0);
}

void set_ticks_on_top(lua_State* state, lv_obj_t* object, int index) {
    luaL_checktype(state, index, LUA_TBOOLEAN);

    if (!lv_obj_check_type(object, &lv_scale_class)) {
        luaL_error(state, "ticks_on_top only applies to scales");
    }
    lv_scale_set_draw_ticks_on_top(object, lua_toboolean(state, index) != 0);
}

void set_chart_type(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_chart_class)) {
        luaL_error(state, "chart_type only applies to charts");
    }
    lv_chart_set_type(object, static_cast<lv_chart_type_t>(luaL_checkinteger(state, index)));
}

void set_point_count(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_chart_class)) {
        luaL_error(state, "point_count only applies to charts");
    }
    lv_chart_set_point_count(object, static_cast<uint32_t>(luaL_checkinteger(state, index)));
}

void set_update_mode(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_chart_class)) {
        luaL_error(state, "update_mode only applies to charts");
    }
    lv_chart_set_update_mode(object, static_cast<lv_chart_update_mode_t>(luaL_checkinteger(state, index)));
}

/** `div_lines = { h = 4, v = 6 }` - background grid lines. */
void set_div_lines(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_chart_class)) {
        luaL_error(state, "div_lines only applies to charts");
    }
    luaL_checktype(state, index, LUA_TTABLE);

    lua_getfield(state, index, "h");
    lua_getfield(state, index, "v");
    lv_chart_set_div_line_count(
        object,
        static_cast<uint32_t>(luaL_optinteger(state, -2, 0)),
        static_cast<uint32_t>(luaL_optinteger(state, -1, 0))
    );
    lua_pop(state, 2);
}

/** `axis_range = { axis = lvgl.CHART_AXIS.PRIMARY_Y, min = 0, max = 100 }`. */
void set_axis_range(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_chart_class)) {
        luaL_error(state, "axis_range only applies to charts");
    }
    luaL_checktype(state, index, LUA_TTABLE);

    lua_getfield(state, index, "axis");
    lua_getfield(state, index, "min");
    lua_getfield(state, index, "max");
    lv_chart_set_axis_range(
        object,
        static_cast<lv_chart_axis_t>(luaL_optinteger(state, -3, LV_CHART_AXIS_PRIMARY_Y)),
        static_cast<int32_t>(luaL_checkinteger(state, -2)),
        static_cast<int32_t>(luaL_checkinteger(state, -1))
    );
    lua_pop(state, 3);
}

void set_today(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_calendar_class)) {
        luaL_error(state, "today only applies to calendars");
    }
    luaL_checktype(state, index, LUA_TTABLE);

    lua_getfield(state, index, "year");
    lua_getfield(state, index, "month");
    lua_getfield(state, index, "day");
    lv_calendar_set_today_date(
        object,
        static_cast<uint32_t>(luaL_checkinteger(state, -3)),
        static_cast<uint32_t>(luaL_checkinteger(state, -2)),
        static_cast<uint32_t>(luaL_checkinteger(state, -1))
    );
    lua_pop(state, 3);
}

/** `shown_date = { year = 2026, month = 11 }`, with an optional day. */
void set_shown_date(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_calendar_class)) {
        luaL_error(state, "shown_date only applies to calendars");
    }
    luaL_checktype(state, index, LUA_TTABLE);

    lua_getfield(state, index, "year");
    lua_getfield(state, index, "month");
    lv_calendar_set_month_shown(
        object,
        static_cast<uint32_t>(luaL_checkinteger(state, -2)),
        static_cast<uint32_t>(luaL_checkinteger(state, -1))
    );
    lua_pop(state, 2);
}

/** `day_names` takes exactly seven labels, starting at the calendar's first weekday. */
void set_day_names(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_calendar_class)) {
        luaL_error(state, "day_names only applies to calendars");
    }
    luaL_checktype(state, index, LUA_TTABLE);
    if (luaL_len(state, index) != 7) {
        luaL_error(state, "day_names needs exactly 7 names");
    }

    constexpr lua_Integer count = 7;

    size_t bytes = 0;
    for (lua_Integer i = 1; i <= count; i++) {
        lua_rawgeti(state, index, i);
        bytes += std::strlen(luaL_checkstring(state, -1)) + 1;
        lua_pop(state, 1);
    }

    auto* block = static_cast<char*>(lv_malloc(sizeof(const char*) * count + bytes));
    if (block == nullptr) {
        luaL_error(state, "out of memory for day names");
    }

    auto** names = reinterpret_cast<const char**>(block);
    char* cursor = block + sizeof(const char*) * count;

    for (lua_Integer i = 1; i <= count; i++) {
        lua_rawgeti(state, index, i);
        const char* name = luaL_checkstring(state, -1);
        const size_t length = std::strlen(name) + 1;
        std::memcpy(cursor, name, length);
        names[i - 1] = cursor;
        cursor += length;
        lua_pop(state, 1);
    }

    lv_calendar_set_day_names(object, names);
    replace_owned_block(object, block);
}

void set_textarea(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_keyboard_class)) {
        luaL_error(state, "textarea only applies to keyboards");
    }

    lv_obj_t* textarea = nullptr;
    if (!lua_isnoneornil(state, index)) {
        if (luaL_testudata(state, index, LUAVGL_OBJECT_METATABLE) == nullptr) {
            luaL_error(state, "textarea expects a text area widget or nil");
        }
        textarea = luavgl_check_object(state, index);
    }

    lv_keyboard_set_textarea(object, textarea);
}

void set_keyboard_mode(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_keyboard_class)) {
        luaL_error(state, "keyboard_mode only applies to keyboards");
    }
    lv_keyboard_set_mode(object, static_cast<lv_keyboard_mode_t>(luaL_checkinteger(state, index)));
}

void set_popovers(lua_State* state, lv_obj_t* object, int index) {
    luaL_checktype(state, index, LUA_TBOOLEAN);

    if (!lv_obj_check_type(object, &lv_keyboard_class)) {
        luaL_error(state, "popovers only applies to keyboards");
    }
    lv_keyboard_set_popovers(object, lua_toboolean(state, index) != 0);
}

/**
 * `map` takes either rows of labels or a flat LVGL-style array.
 *
 *     map = { { "7", "8", "9" }, { "4", "5", "6" } }
 *     map = { "7", "8", "9", "\n", "4", "5", "6", "" }
 *
 * Rows are joined with "\n"; the terminator LVGL expects is appended automatically.
 */
void set_map(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_buttonmatrix_class)) {
        luaL_error(state, "map only applies to button matrices");
    }
    luaL_checktype(state, index, LUA_TTABLE);

    const int absolute_index = lua_absindex(state, index);
    lua_newtable(state);
    const int flat_index = lua_gettop(state);

    int count = 0;
    const lua_Integer rows = luaL_len(state, absolute_index);

    for (lua_Integer row = 1; row <= rows; row++) {
        lua_rawgeti(state, absolute_index, row);

        if (lua_istable(state, -1)) {
            const lua_Integer columns = luaL_len(state, -1);
            for (lua_Integer column = 1; column <= columns; column++) {
                lua_rawgeti(state, -1, column);
                if (!lua_isstring(state, -1)) {
                    luaL_error(state, "map rows must contain strings");
                }
                lua_rawseti(state, flat_index, ++count);
            }
            lua_pop(state, 1); // row table

            if (row < rows) {
                lua_pushliteral(state, "\n");
                lua_rawseti(state, flat_index, ++count);
            }
        } else {
            if (!lua_isstring(state, -1)) {
                luaL_error(state, "map entries must be strings or row tables");
            }
            lua_pushvalue(state, -1);
            lua_rawseti(state, flat_index, ++count);
            lua_pop(state, 1);
        }
    }

    // LVGL terminates its map on an empty string; add one if the script did not.
    bool already_terminated = false;
    if (count > 0) {
        lua_rawgeti(state, flat_index, count);
        const char* last = lua_tostring(state, -1);
        already_terminated = last != nullptr && last[0] == '\0';
        lua_pop(state, 1);
    }
    if (!already_terminated) {
        lua_pushliteral(state, "");
        lua_rawseti(state, flat_index, ++count);
    }

    size_t bytes = 0;
    for (int i = 1; i <= count; i++) {
        lua_rawgeti(state, flat_index, i);
        bytes += std::strlen(luaL_checkstring(state, -1)) + 1;
        lua_pop(state, 1);
    }

    auto* block = static_cast<char*>(lv_malloc(sizeof(const char*) * static_cast<size_t>(count + 1) + bytes));
    if (block == nullptr) {
        luaL_error(state, "out of memory for button map");
    }

    auto** entries = reinterpret_cast<const char**>(block);
    char* cursor = block + sizeof(const char*) * static_cast<size_t>(count + 1);

    for (int i = 1; i <= count; i++) {
        lua_rawgeti(state, flat_index, i);
        const char* text = luaL_checkstring(state, -1);
        const size_t length = std::strlen(text) + 1;
        std::memcpy(cursor, text, length);
        entries[i - 1] = cursor;
        cursor += length;
        lua_pop(state, 1);
    }
    entries[count] = nullptr;

    lv_buttonmatrix_set_map(object, entries);
    replace_owned_block(object, block);

    lua_pop(state, 1); // flat table
}

/**
 * `ctrl_map` takes an array of `lvgl.BUTTONMATRIX_CTRL` values, one per button.
 *
 * LVGL copies exactly `btn_cnt` entries, so the array is built to that length instead of
 * handing it the Lua table's length.
 */
void set_ctrl_map(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_buttonmatrix_class)) {
        luaL_error(state, "ctrl_map only applies to button matrices");
    }
    luaL_checktype(state, index, LUA_TTABLE);

    const char* const* map = lv_buttonmatrix_get_map(object);
    uint32_t button_count = 0;
    if (map != nullptr) {
        for (uint32_t i = 0; map[i] != nullptr && map[i][0] != '\0'; i++) {
            if (std::strcmp(map[i], "\n") != 0) {
                button_count++;
            }
        }
    }

    if (button_count == 0) {
        return;
    }

    auto* controls = static_cast<lv_buttonmatrix_ctrl_t*>(
        lv_malloc(sizeof(lv_buttonmatrix_ctrl_t) * button_count)
    );
    if (controls == nullptr) {
        luaL_error(state, "out of memory for button controls");
    }

    for (uint32_t i = 0; i < button_count; i++) {
        controls[i] = LV_BUTTONMATRIX_CTRL_NONE;
    }

    const lua_Integer count = luaL_len(state, index);
    for (lua_Integer i = 1; i <= count && i <= static_cast<lua_Integer>(button_count); i++) {
        lua_rawgeti(state, index, i);
        controls[i - 1] = static_cast<lv_buttonmatrix_ctrl_t>(luaL_checkinteger(state, -1));
        lua_pop(state, 1);
    }

    lv_buttonmatrix_set_ctrl_map(object, controls);
    lv_free(controls);
}

void set_one_checked(lua_State* state, lv_obj_t* object, int index) {
    luaL_checktype(state, index, LUA_TBOOLEAN);

    if (!lv_obj_check_type(object, &lv_buttonmatrix_class)) {
        luaL_error(state, "one_checked only applies to button matrices");
    }
    lv_buttonmatrix_set_one_checked(object, lua_toboolean(state, index) != 0);
}

/** `selected_button` is 1-based; 0 clears the selection. */
void set_selected_button(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_buttonmatrix_class)) {
        luaL_error(state, "selected_button only applies to button matrices");
    }

    const auto value = luaL_checkinteger(state, index);
    lv_buttonmatrix_set_selected_button(
        object,
        value <= 0 ? LV_BUTTONMATRIX_BUTTON_NONE : static_cast<uint32_t>(value - 1)
    );
}

// Composite widgets -------------------------------------------------------------------------

/** `active_tab` is 1-based; 0 hides every tab. */
void set_active_tab(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_tabview_class)) {
        luaL_error(state, "active_tab only applies to tabviews");
    }

    const auto value = luaL_checkinteger(state, index);
    if (value <= 0) {
        luaL_error(state, "active_tab is 1-based");
    }

    lv_tabview_set_active(object, static_cast<uint32_t>(value - 1), LV_ANIM_OFF);
}

void set_tab_bar_position(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_tabview_class)) {
        luaL_error(state, "tab_bar_position only applies to tabviews");
    }
    lv_tabview_set_tab_bar_position(object, static_cast<lv_dir_t>(luaL_checkinteger(state, index)));
}

void set_tab_bar_size(lua_State* state, lv_obj_t* object, int index) {
    if (!lv_obj_check_type(object, &lv_tabview_class)) {
        luaL_error(state, "tab_bar_size only applies to tabviews");
    }
    lv_tabview_set_tab_bar_size(object, static_cast<int32_t>(luaL_checkinteger(state, index)));
}



// Style -----------------------------------------------------------------------------------

void set_bg_color(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_bg_color(object, to_color(state, index), LV_PART_MAIN);
}

void set_bg_opa(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_bg_opa(object, static_cast<lv_opa_t>(luaL_checkinteger(state, index)), LV_PART_MAIN);
}

void set_text_color(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_text_color(object, to_color(state, index), LV_PART_MAIN);
}

/** Named sizes rather than raw faces, so a script stays legible across display densities. */
void set_text_font(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_text_font(object, luavgl_check_font(state, index), LV_PART_MAIN);
}

void set_border_color(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_border_color(object, to_color(state, index), LV_PART_MAIN);
}

void set_border_width(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_border_width(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_radius(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_radius(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_pad_all(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_pad_all(object, to_coordinate(state, index), LV_PART_MAIN);
}

/**
 * The gap between flex children.
 *
 * Distinct from pad_all, which is the space inside the container's edges: a column with
 * pad_all = 0 still separates its children by the theme's default row padding, which is
 * what makes a Lua-built screen look inset compared with a C++ one.
 */
void set_pad_row(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_pad_row(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_pad_column(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_pad_column(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_opa(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_opa(object, static_cast<lv_opa_t>(luaL_checkinteger(state, index)), LV_PART_MAIN);
}

// Sizing and spacing ----------------------------------------------------------------

void set_min_width(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_min_width(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_min_height(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_min_height(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_max_width(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_max_width(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_max_height(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_max_height(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_pad_top(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_pad_top(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_pad_bottom(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_pad_bottom(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_pad_left(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_pad_left(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_pad_right(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_pad_right(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_margin_top(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_margin_top(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_margin_bottom(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_margin_bottom(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_margin_left(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_margin_left(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_margin_right(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_margin_right(object, to_coordinate(state, index), LV_PART_MAIN);
}

// Borders, outlines and shadows ------------------------------------------------------

void set_border_opa(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_border_opa(object, static_cast<lv_opa_t>(luaL_checkinteger(state, index)), LV_PART_MAIN);
}

void set_border_side(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_border_side(object, static_cast<lv_border_side_t>(luaL_checkinteger(state, index)), LV_PART_MAIN);
}

void set_outline_width(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_outline_width(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_outline_color(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_outline_color(object, to_color(state, index), LV_PART_MAIN);
}

void set_outline_opa(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_outline_opa(object, static_cast<lv_opa_t>(luaL_checkinteger(state, index)), LV_PART_MAIN);
}

void set_outline_pad(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_outline_pad(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_shadow_width(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_shadow_width(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_shadow_color(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_shadow_color(object, to_color(state, index), LV_PART_MAIN);
}

void set_shadow_opa(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_shadow_opa(object, static_cast<lv_opa_t>(luaL_checkinteger(state, index)), LV_PART_MAIN);
}

void set_shadow_ofs_x(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_shadow_offset_x(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_shadow_ofs_y(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_shadow_offset_y(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_shadow_spread(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_shadow_spread(object, to_coordinate(state, index), LV_PART_MAIN);
}

// Gradients and transforms -----------------------------------------------------------

void set_bg_grad_color(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_bg_grad_color(object, to_color(state, index), LV_PART_MAIN);
}

void set_bg_grad_dir(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_bg_grad_dir(object, static_cast<lv_grad_dir_t>(luaL_checkinteger(state, index)), LV_PART_MAIN);
}

void set_transform_width(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_transform_width(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_transform_height(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_transform_height(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_transform_scale_x(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_transform_scale_x(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_transform_scale_y(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_transform_scale_y(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_transform_rotation(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_transform_rotation(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_transform_pivot_x(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_transform_pivot_x(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_transform_pivot_y(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_transform_pivot_y(object, to_coordinate(state, index), LV_PART_MAIN);
}

// Text spacing and alignment ---------------------------------------------------------

void set_text_align(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_text_align(object, static_cast<lv_text_align_t>(luaL_checkinteger(state, index)), LV_PART_MAIN);
}

void set_text_letter_space(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_text_letter_space(object, to_coordinate(state, index), LV_PART_MAIN);
}

void set_text_line_space(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_style_text_line_space(object, to_coordinate(state, index), LV_PART_MAIN);
}

// Scrolling --------------------------------------------------------------------------

void set_scroll_dir(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_scroll_dir(object, static_cast<lv_dir_t>(luaL_checkinteger(state, index)));
}

void set_scrollbar_mode(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_scrollbar_mode(object, static_cast<lv_scrollbar_mode_t>(luaL_checkinteger(state, index)));
}

void set_scroll_snap_x(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_scroll_snap_x(object, static_cast<lv_scroll_snap_t>(luaL_checkinteger(state, index)));
}

void set_scroll_snap_y(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_scroll_snap_y(object, static_cast<lv_scroll_snap_t>(luaL_checkinteger(state, index)));
}

// Layout ----------------------------------------------------------------------------------

void set_flex_flow(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_flex_flow(object, static_cast<lv_flex_flow_t>(luaL_checkinteger(state, index)));
}

/** `flex_align = { main = ..., cross = ..., track = ... }`, each optional. */
void set_flex_align(lua_State* state, lv_obj_t* object, int index) {
    luaL_checktype(state, index, LUA_TTABLE);

    lua_getfield(state, index, "main");
    lua_getfield(state, index, "cross");
    lua_getfield(state, index, "track");

    lv_obj_set_flex_align(
        object,
        static_cast<lv_flex_align_t>(luaL_optinteger(state, -3, LV_FLEX_ALIGN_START)),
        static_cast<lv_flex_align_t>(luaL_optinteger(state, -2, LV_FLEX_ALIGN_START)),
        static_cast<lv_flex_align_t>(luaL_optinteger(state, -1, LV_FLEX_ALIGN_START))
    );

    lua_pop(state, 3);
}

void set_flex_grow(lua_State* state, lv_obj_t* object, int index) {
    lv_obj_set_flex_grow(object, static_cast<uint8_t>(luaL_checkinteger(state, index)));
}

/** Sorted by name so the lookup below can binary search. */
constexpr Property PROPERTIES[] = {
    { "active_tab", set_active_tab },
    { "align", set_align },
    { "arc_bg_end", set_arc_bg_end },
    { "arc_bg_start", set_arc_bg_start },
    { "arc_end", set_arc_end },
    { "arc_rotation", set_arc_rotation },
    { "arc_start", set_arc_start },
    { "axis_range", set_axis_range },
    { "bg_color", set_bg_color },
    { "bg_grad_color", set_bg_grad_color },
    { "bg_grad_dir", set_bg_grad_dir },
    { "bg_opa", set_bg_opa },
    { "border_color", set_border_color },
    { "border_opa", set_border_opa },
    { "border_side", set_border_side },
    { "border_width", set_border_width },
    { "brightness", set_brightness },
    { "chart_type", set_chart_type },
    { "checked", set_checked },
    { "column_widths", set_column_widths },
    { "columns", set_columns },
    { "ctrl_map", set_ctrl_map },
    { "day_names", set_day_names },
    { "digit_format", set_digit_format },
    { "div_lines", set_div_lines },
    { "flex_align", set_flex_align },
    { "flex_flow", set_flex_flow },
    { "flex_grow", set_flex_grow },
    { "h", set_height },
    { "height", set_height },
    { "keyboard_mode", set_keyboard_mode },
    { "label_show", set_label_show },
    { "layout", set_layout },
    { "led_color", set_led_color },
    { "long_mode", set_long_mode },
    { "major_tick_every", set_major_tick_every },
    { "map", set_map },
    { "margin_bottom", set_margin_bottom },
    { "margin_left", set_margin_left },
    { "margin_right", set_margin_right },
    { "margin_top", set_margin_top },
    { "max", set_max },
    { "max_height", set_max_height },
    { "max_width", set_max_width },
    { "min", set_min },
    { "min_height", set_min_height },
    { "min_width", set_min_width },
    { "on", set_led_on },
    { "one_checked", set_one_checked },
    { "one_line", set_one_line },
    { "opa", set_opa },
    { "options", set_options },
    { "outline_color", set_outline_color },
    { "outline_opa", set_outline_opa },
    { "outline_pad", set_outline_pad },
    { "outline_width", set_outline_width },
    { "pad_all", set_pad_all },
    { "pad_bottom", set_pad_bottom },
    { "pad_column", set_pad_column },
    { "pad_left", set_pad_left },
    { "pad_right", set_pad_right },
    { "pad_row", set_pad_row },
    { "pad_top", set_pad_top },
    { "password_mode", set_password_mode },
    { "placeholder", set_placeholder },
    { "point_count", set_point_count },
    { "popovers", set_popovers },
    { "post_draw", set_post_draw },
    { "radius", set_radius },
    { "range", set_range },
    { "rows", set_rows },
    { "scale_angle", set_scale_angle },
    { "scale_mode", set_scale_mode },
    { "scale_rotation", set_scale_rotation },
    { "scroll_dir", set_scroll_dir },
    { "scroll_snap_x", set_scroll_snap_x },
    { "scroll_snap_y", set_scroll_snap_y },
    { "scrollbar_mode", set_scrollbar_mode },
    { "selected_button", set_selected_button },
    { "selected_cell", set_selected_cell },
    { "shadow_color", set_shadow_color },
    { "shadow_ofs_x", set_shadow_ofs_x },
    { "shadow_ofs_y", set_shadow_ofs_y },
    { "shadow_opa", set_shadow_opa },
    { "shadow_spread", set_shadow_spread },
    { "shadow_width", set_shadow_width },
    { "shown_date", set_shown_date },
    { "spin_step", set_spin_step },
    { "src", set_source },
    { "tab_bar_position", set_tab_bar_position },
    { "tab_bar_size", set_tab_bar_size },
    { "text", set_text },
    { "text_align", set_text_align },
    { "text_color", set_text_color },
    { "text_font", set_text_font },
    { "text_letter_space", set_text_letter_space },
    { "text_line_space", set_text_line_space },
    { "text_src", set_text_src },
    { "textarea", set_textarea },
    { "tick_count", set_tick_count },
    { "ticks_on_top", set_ticks_on_top },
    { "today", set_today },
    { "transform_height", set_transform_height },
    { "transform_pivot_x", set_transform_pivot_x },
    { "transform_pivot_y", set_transform_pivot_y },
    { "transform_rotation", set_transform_rotation },
    { "transform_scale_x", set_transform_scale_x },
    { "transform_scale_y", set_transform_scale_y },
    { "transform_width", set_transform_width },
    { "update_mode", set_update_mode },
    { "value", set_value },
    { "w", set_width },
    { "width", set_width },
    { "x", set_x },
    { "y", set_y },
};

constexpr size_t PROPERTY_COUNT = sizeof(PROPERTIES) / sizeof(PROPERTIES[0]);

const Property* find_property(const char* name) {
    size_t low = 0;
    size_t high = PROPERTY_COUNT;

    while (low < high) {
        const size_t middle = (low + high) / 2;
        const int comparison = std::strcmp(name, PROPERTIES[middle].name);

        if (comparison == 0) {
            return &PROPERTIES[middle];
        }
        if (comparison < 0) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }

    return nullptr;
}

} // namespace

int luavgl_apply_properties(lua_State* state, lv_obj_t* object, int table_index) {
    const int absolute_index = lua_absindex(state, table_index);

    lua_pushnil(state);
    while (lua_next(state, absolute_index) != 0) {
        // key at -2, value at -1

        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 1);
            continue; // array parts of the table are not properties
        }

        const char* name = lua_tostring(state, -2);
        const Property* property = find_property(name);

        if (property == nullptr) {
            // Raising is deliberate: silently ignoring a typo like `colour = ...` would
            // leave the script looking correct while doing nothing.
            return luaL_error(state, "unknown property '%s'", name);
        }

        property->set(state, object, lua_absindex(state, -1));

        lua_pop(state, 1); // value; key stays for lua_next
    }

    return 0;
}
