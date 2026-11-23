// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

#include <lvgl/widgets/badge.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/chip.h>
#include <lvgl/widgets/icon_button.h>
#include <lvgl/widgets/list.h>
#include <lvgl/widgets/page_indicator.h>
#include <lvgl/widgets/sliderbox.h>
#include <lvgl/widgets/spinner.h>
#include <lvgl/widgets/statusline.h>
#include <lvgl/widgets/step_indicator.h>
#include <lvgl/widgets/tabs.h>
#include <lvgl/widgets/text_button.h>

#include <cstdlib>
#include <cstring>
#include <initializer_list>

extern "C" {
#include <lauxlib.h>
}

/**
 * The core widget set beyond Object/Label/Button.
 *
 * Constructors follow `luavgl_create_widget`, so every one accepts an optional parent and
 * an optional property table in either order. Widget-specific properties live in
 * properties.cpp alongside the shared ones, dispatched on the widget's class - LVGL's
 * setters do not check, so writing a slider's value through a label would corrupt memory
 * rather than fail.
 *
 * Getters are methods rather than properties: `slider:value()` reads better than a
 * `get{}` counterpart to `set{}`, and it keeps the property table write-only, which is
 * what makes a single flat table workable.
 */

namespace {

/** Pushes a shallow copy of `table_index` without the named string keys. */
void push_properties_without(lua_State* state, int table_index, std::initializer_list<const char*> excluded);

// Constructors ----------------------------------------------------------------------------

int widget_slider(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_slider_create(parent); });
}

int widget_switch(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_switch_create(parent); });
}

int widget_checkbox(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_checkbox_create(parent); });
}

int widget_bar(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_bar_create(parent); });
}

int widget_dropdown(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_dropdown_create(parent); });
}

int widget_roller(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_roller_create(parent); });
}

int widget_textarea(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_textarea_create(parent); });
}

int widget_image(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_image_create(parent); });
}

/**
 * Tactility's spinner, not LVGL's.
 *
 * `LV_USE_SPINNER` is off in the ESP32 sdkconfig - widget availability is per-target, and
 * the simulator's lv_conf.h is not the firmware's - so lv_spinner_create() does not exist
 * there. `lvgl-module` provides a portable one and already exports it.
 */
int widget_spinner(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lvgl_spinner_create(parent); });
}

// Batch 2 widgets -------------------------------------------------------------------------

/**
 * Applies `on` before the other LED properties.
 *
 * `lv_led_on()` resets the brightness to maximum, so a `brightness` applied first would
 * be overwritten. Turning the LED on first and then adjusting the brightness gives the
 * result the property table reads like, whatever order Lua iterates it in.
 */
int widget_led(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);

    auto* object = lv_led_create(args.parent);
    luavgl_push_object(state, object);

    if (args.properties_index != 0) {
        lua_getfield(state, args.properties_index, "on");
        if (!lua_isnil(state, -1)) {
            lua_createtable(state, 0, 1);
            lua_pushvalue(state, -2);
            lua_setfield(state, -2, "on");
            luavgl_apply_properties(state, object, lua_gettop(state));
            lua_pop(state, 1);
        }
        lua_pop(state, 1); // on value

        push_properties_without(state, args.properties_index, { "on" });
        luavgl_apply_properties(state, object, lua_gettop(state));
        lua_pop(state, 1);
    }

    return 1;
}

/**
 * Applies `rows` and `columns` before the rest of the table's properties.
 *
 * LVGL sizes its cell storage from the counts; a `column_widths` or `selected_cell`
 * applied first would be clipped. Lua table iteration order is not defined, so the order
 * cannot be left to the dispatcher.
 */
int widget_table(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);

    auto* object = lv_table_create(args.parent);
    luavgl_push_object(state, object);

    if (args.properties_index != 0) {
        lua_createtable(state, 0, 2);
        const int counts_index = lua_gettop(state);

        for (const char* name : { "rows", "columns" }) {
            lua_getfield(state, args.properties_index, name);
            if (lua_isnil(state, -1)) {
                lua_pop(state, 1);
            } else {
                lua_setfield(state, counts_index, name);
            }
        }

        luavgl_apply_properties(state, object, counts_index);
        lua_pop(state, 1);

        push_properties_without(state, args.properties_index, { "rows", "columns" });
        luavgl_apply_properties(state, object, lua_gettop(state));
        lua_pop(state, 1);
    }

    return 1;
}

int widget_scale(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_scale_create(parent); });
}

int widget_imagebutton(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_imagebutton_create(parent); });
}

int widget_chart(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_chart_create(parent); });
}

int widget_calendar(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_calendar_create(parent); });
}

int widget_keyboard(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_keyboard_create(parent); });
}

/**
 * Applies `map` before the rest of the matrix's properties.
 *
 * `ctrl_map`, `selected_button` and `one_checked` all refer to buttons that only exist
 * once the map is set. Lua table iteration order is not defined, so the constructor sets
 * the map first and then applies everything else.
 */
int widget_buttonmatrix(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);

    auto* object = lv_buttonmatrix_create(args.parent);
    luavgl_push_object(state, object);

    if (args.properties_index != 0) {
        lua_getfield(state, args.properties_index, "map");
        if (!lua_isnil(state, -1)) {
            lua_createtable(state, 0, 1);
            lua_pushvalue(state, -2);
            lua_setfield(state, -2, "map");
            luavgl_apply_properties(state, object, lua_gettop(state));
            lua_pop(state, 1);
        }
        lua_pop(state, 1); // map value

        push_properties_without(state, args.properties_index, { "map" });
        luavgl_apply_properties(state, object, lua_gettop(state));
        lua_pop(state, 1);
    }

    return 1;
}

// Composite widgets -----------------------------------------------------------------------

int widget_tabview(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_tabview_create(parent); });
}

int widget_tileview(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_tileview_create(parent); });
}

// Framework widgets -----------------------------------------------------------------------

/**
 * Tags a framework widget in its user data.
 *
 * The card, chip, slider box and indicator classes live inside the firmware and are not
 * exported as `lv_obj_class_t`s, so `lv_obj_check_type` cannot identify them from the
 * runtime. Nothing in these bindings uses an object's user data, which leaves it free.
 */
char CHIP_TAG;
char SLIDERBOX_TAG;
char LIST_TAG;
char PAGE_INDICATOR_TAG;
char STEP_INDICATOR_TAG;
char ARC_TAG;
char SPINBOX_TAG;
char TABS_TAG;
char STATUSLINE_TAG;
char LINE_TAG;

/** Raises unless the widget carries `tag`, then returns it. */
lv_obj_t* check_tagged_widget(lua_State* state, int index, const void* tag, const char* name) {
    auto* object = luavgl_check_object(state, index);

    if (lv_obj_get_user_data(object) != tag) {
        luaL_error(state, "%s expected", name);
    }

    return object;
}

/** Pushes a shallow copy of `table_index` without the named string keys. */
void push_properties_without(lua_State* state, int table_index, std::initializer_list<const char*> excluded) {
    const int absolute_index = lua_absindex(state, table_index);

    lua_createtable(state, 0, 8);
    const int target_index = lua_gettop(state);

    lua_pushnil(state);
    while (lua_next(state, absolute_index) != 0) {
        bool skip = false;

        if (lua_type(state, -2) == LUA_TSTRING) {
            const char* key = lua_tostring(state, -2);

            for (const char* name : excluded) {
                if (std::strcmp(key, name) == 0) {
                    skip = true;
                    break;
                }
            }
        }

        if (!skip) {
            lua_pushvalue(state, -2);
            lua_pushvalue(state, -2);
            lua_settable(state, target_index);
        }

        lua_pop(state, 1); // value; key stays for lua_next
    }
}

/**
 * Leaves the handle for a hand-built framework widget on the stack and applies its
 * property table, minus any keys the constructor consumed itself.
 */
int finish_widget(
    lua_State* state,
    lv_obj_t* object,
    LuavglConstructorArgs args,
    std::initializer_list<const char*> consumed = {}
) {
    luavgl_push_object(state, object);

    if (args.properties_index != 0) {
        if (consumed.size() == 0) {
            luavgl_apply_properties(state, object, args.properties_index);
        } else {
            push_properties_without(state, args.properties_index, consumed);
            luavgl_apply_properties(state, object, lua_gettop(state));
            lua_pop(state, 1);
        }
    }

    return 1;
}

int widget_card(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);
    return finish_widget(state, lvgl_card_create(args.parent), args);
}

/** Tagged because the line class is not exported; `line:set_points` checks the tag. */
int widget_line(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);
    auto* object = lv_line_create(args.parent);
    lv_obj_set_user_data(object, &LINE_TAG);
    return finish_widget(state, object, args);
}

/** Material 3 tabs: the bar only; the app swaps the pane itself on VALUE_CHANGED. */
int widget_tabs(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);

    auto* tabs = lvgl_tabs_create(args.parent);
    lv_obj_set_user_data(tabs, &TABS_TAG);

    if (args.properties_index != 0) {
        lua_getfield(state, args.properties_index, "mode");
        if (lua_isinteger(state, -1)) {
            lvgl_tabs_set_mode(tabs, static_cast<LvglTabsMode>(lua_tointeger(state, -1)));
        }
        lua_pop(state, 1);
    }

    return finish_widget(state, tabs, args, { "mode" });
}

/** One line of small status text with a theme-coloured dot. */
int widget_statusline(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);

    auto* statusline = lvgl_statusline_create(args.parent);
    lv_obj_set_user_data(statusline, &STATUSLINE_TAG);

    if (args.properties_index != 0) {
        lua_getfield(state, args.properties_index, "text");
        if (!lua_isnil(state, -1)) {
            lvgl_statusline_set_text(statusline, luaL_checkstring(state, -1));
        }
        lua_pop(state, 1);

        lua_getfield(state, args.properties_index, "level");
        if (lua_isinteger(state, -1)) {
            lvgl_statusline_set_level(
                statusline,
                static_cast<LvglStatuslineLevel>(lua_tointeger(state, -1))
            );
        }
        lua_pop(state, 1);
    }

    return finish_widget(state, statusline, args, { "text", "level" });
}

int widget_arc(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);
    auto* object = lv_arc_create(args.parent);
    lv_obj_set_user_data(object, &ARC_TAG);
    return finish_widget(state, object, args);
}

int widget_spinbox(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);
    auto* object = lv_spinbox_create(args.parent);
    lv_obj_set_user_data(object, &SPINBOX_TAG);
    return finish_widget(state, object, args);
}

int widget_badge(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);
    return finish_widget(state, lvgl_badge_create(args.parent), args);
}

int widget_text_button(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);
    return finish_widget(state, lvgl_text_button_create(args.parent), args);
}

int widget_icon_button(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);

    auto variant = LVGL_ICON_BUTTON_STANDARD;
    if (args.properties_index != 0) {
        lua_getfield(state, args.properties_index, "variant");
        if (lua_isinteger(state, -1)) {
            variant = static_cast<LvglIconButtonVariant>(lua_tointeger(state, -1));
        }
        lua_pop(state, 1);
    }

    auto* object = lvgl_icon_button_create_variant(args.parent, variant);
    return finish_widget(state, object, args, { "variant" });
}

/**
 * A chip plus the label its theme expects. The class does not create one, because C apps
 * add their own with `lv_label_create()`.
 */
int widget_chip(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);

    auto* chip = lvgl_chip_create(args.parent);
    lv_obj_set_user_data(chip, &CHIP_TAG);

    auto* label = lv_label_create(chip);
    lv_obj_center(label);

    return finish_widget(state, chip, args);
}

/**
 * `lvgl.SliderBox(parent?, min, max, step?, value?)`
 *
 * The range and step are baked into the slider box when it is created, so they are
 * positional arguments rather than properties.
 */
int widget_sliderbox(lua_State* state) {
    lv_obj_t* parent = nullptr;
    int index = 1;

    if (luaL_testudata(state, 1, LUAVGL_OBJECT_METATABLE) != nullptr) {
        parent = luavgl_check_object(state, 1);
        index = 2;
    }

    if (parent == nullptr) {
        parent = luavgl_get_root(state);
    }

    const auto min = static_cast<int32_t>(luaL_checkinteger(state, index));
    const auto max = static_cast<int32_t>(luaL_checkinteger(state, index + 1));
    const auto step = static_cast<int32_t>(luaL_optinteger(state, index + 2, 1));
    const auto value = static_cast<int32_t>(luaL_optinteger(state, index + 3, min));

    auto* object = lvgl_sliderbox_create(parent, min, max, step, value);
    lv_obj_set_user_data(object, &SLIDERBOX_TAG);

    luavgl_push_object(state, object);
    return 1;
}

int widget_list(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);

    auto* list = lvgl_list_create(args.parent);
    lv_obj_set_user_data(list, &LIST_TAG);

    return finish_widget(state, list, args);
}

int widget_page_indicator(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);

    auto* object = lvgl_page_indicator_create(args.parent);
    lv_obj_set_user_data(object, &PAGE_INDICATOR_TAG);

    return finish_widget(state, object, args);
}

int widget_step_indicator(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);

    auto* object = lvgl_step_indicator_create(args.parent);
    lv_obj_set_user_data(object, &STEP_INDICATOR_TAG);

    return finish_widget(state, object, args);
}

// Framework widget methods ----------------------------------------------------------------

/** `list:add_text(text)` - a full-width section header. Returns the new label. */
int list_add_text(lua_State* state) {
    auto* list = check_tagged_widget(state, 1, &LIST_TAG, "list");
    auto* label = lvgl_list_add_text(list, luaL_optstring(state, 2, nullptr));

    luavgl_push_object(state, label);
    return 1;
}

/**
 * `list:add_button(text, icon?)` - a full-width button. Returns the new button.
 *
 * The Lua order puts the text first because it is the required argument. `icon` is an
 * `lvgl.SYMBOL` value; its string is pinned because LVGL keeps the pointer.
 */
int list_add_button(lua_State* state) {
    auto* list = check_tagged_widget(state, 1, &LIST_TAG, "list");
    const char* text = luaL_optstring(state, 2, nullptr);

    const void* icon = nullptr;
    if (!lua_isnoneornil(state, 3)) {
        icon = luaL_checkstring(state, 3);
        luavgl_pin_string(state, 3);
    }

    auto* button = lvgl_list_add_button(list, icon, text);

    luavgl_push_object(state, button);
    return 1;
}

int list_hide_last_divider(lua_State* state) {
    auto* list = check_tagged_widget(state, 1, &LIST_TAG, "list");
    lvgl_list_hide_last_divider(list);

    lua_settop(state, 1);
    return 1;
}

int page_indicator_set_page_count(lua_State* state) {
    auto* object = check_tagged_widget(state, 1, &PAGE_INDICATOR_TAG, "page indicator");
    lvgl_page_indicator_set_page_count(object, static_cast<uint32_t>(luaL_checkinteger(state, 2)));

    lua_settop(state, 1);
    return 1;
}

int page_indicator_set_page(lua_State* state) {
    auto* object = check_tagged_widget(state, 1, &PAGE_INDICATOR_TAG, "page indicator");
    lvgl_page_indicator_set_page(object, static_cast<uint32_t>(luaL_checkinteger(state, 2)));

    lua_settop(state, 1);
    return 1;
}

int page_indicator_set_vertical(lua_State* state) {
    auto* object = check_tagged_widget(state, 1, &PAGE_INDICATOR_TAG, "page indicator");
    luaL_checktype(state, 2, LUA_TBOOLEAN);
    lvgl_page_indicator_set_vertical(object, lua_toboolean(state, 2));

    lua_settop(state, 1);
    return 1;
}

int step_indicator_set_step_count(lua_State* state) {
    auto* object = check_tagged_widget(state, 1, &STEP_INDICATOR_TAG, "step indicator");
    lvgl_step_indicator_set_step_count(object, static_cast<uint32_t>(luaL_checkinteger(state, 2)));

    lua_settop(state, 1);
    return 1;
}

int step_indicator_set_finished_count(lua_State* state) {
    auto* object = check_tagged_widget(state, 1, &STEP_INDICATOR_TAG, "step indicator");
    lvgl_step_indicator_set_finished_count(object, static_cast<uint32_t>(luaL_checkinteger(state, 2)));

    lua_settop(state, 1);
    return 1;
}

// Layout methods --------------------------------------------------------------------------

/** Frees the descriptor block a grid object saved on its creation. */
void free_grid_tracks(lv_event_t* event) {
    lv_free(lv_event_get_user_data(event));
}

/** Copies a Lua array of track sizes into `out`, closed with LV_GRID_TEMPLATE_LAST. */
void read_tracks(lua_State* state, int index, int32_t* out, lua_Integer count) {
    for (lua_Integer i = 1; i <= count; i++) {
        lua_rawgeti(state, index, i);
        out[i - 1] = static_cast<int32_t>(luaL_checkinteger(state, -1));
        lua_pop(state, 1);
    }
    out[count] = LV_GRID_TEMPLATE_LAST;
}

/**
 * `obj:grid(columns [, rows])` - makes the object a grid.
 *
 * Both arrays are copied into one heap block because LVGL keeps the pointers, not a
 * copy; an LV_EVENT_DELETE callback frees the block with the widget. Omitting `rows`
 * (or `columns`) creates a sub-grid on that axis, matching `lv_obj_set_grid_dsc_array`.
 */
int object_grid(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);

    const bool has_columns = !lua_isnoneornil(state, 2);
    const bool has_rows = !lua_isnoneornil(state, 3);
    if (!has_columns && !has_rows) {
        return luaL_error(state, "grid needs at least one track array");
    }

    lua_Integer column_count = 0;
    lua_Integer row_count = 0;

    if (has_columns) {
        luaL_checktype(state, 2, LUA_TTABLE);
        column_count = luaL_len(state, 2);
    }
    if (has_rows) {
        luaL_checktype(state, 3, LUA_TTABLE);
        row_count = luaL_len(state, 3);
    }

    const lua_Integer total = (has_columns ? column_count + 1 : 0) + (has_rows ? row_count + 1 : 0);
    auto* tracks = static_cast<int32_t*>(lv_malloc(static_cast<size_t>(total) * sizeof(int32_t)));
    if (tracks == nullptr) {
        return luaL_error(state, "out of memory for grid tracks");
    }

    int32_t* columns = nullptr;
    int32_t* rows = nullptr;

    if (has_columns) {
        columns = tracks;
        read_tracks(state, 2, columns, column_count);
    }
    if (has_rows) {
        rows = tracks + (has_columns ? column_count + 1 : 0);
        read_tracks(state, 3, rows, row_count);
    }

    lv_obj_set_grid_dsc_array(object, columns, rows);

    // LVGL keeps the pointers, not a copy. The block is intentionally not freed:
    // an LV_EVENT_DELETE callback would run from this app's ELF when the window
    // manager tears the widget tree down, which can happen after the app exits.
    // Grid allocations are small (a few dozen bytes) and bounded by the app's
    // lifetime. Reusing a single card for repeated scans avoids growth.
    (void)free_grid_tracks;

    lua_settop(state, 1);
    return 1;
}

/** `obj:grid_align(column_align, row_align)` - how fixed tracks are packed. */
int object_grid_align(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);

    lv_obj_set_grid_align(
        object,
        static_cast<lv_grid_align_t>(luaL_checkinteger(state, 2)),
        static_cast<lv_grid_align_t>(luaL_checkinteger(state, 3))
    );

    lua_settop(state, 1);
    return 1;
}

/** `child:grid_cell(column_align, col, col_span, row_align, row, row_span)` */
int object_grid_cell(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);

    const auto column_align = static_cast<lv_grid_align_t>(luaL_checkinteger(state, 2));
    const auto column = static_cast<int32_t>(luaL_checkinteger(state, 3));
    const auto column_span = static_cast<int32_t>(luaL_checkinteger(state, 4));
    const auto row_align = static_cast<lv_grid_align_t>(luaL_checkinteger(state, 5));
    const auto row = static_cast<int32_t>(luaL_checkinteger(state, 6));
    const auto row_span = static_cast<int32_t>(luaL_checkinteger(state, 7));

    lv_obj_set_grid_cell(object, column_align, column, column_span, row_align, row, row_span);

    lua_settop(state, 1);
    return 1;
}

// Value accessors -------------------------------------------------------------------------


/**
 * `widget:value()` - the current value of a slider, bar, dropdown or roller.
 *
 * One method rather than one per widget: a script reading "whatever this control is set
 * to" should not have to know which class it holds.
 */
int object_value(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);

    if (luavgl_is_sliderbox(object)) {
        lua_pushinteger(state, lvgl_sliderbox_get_value(object));
    } else if (lv_obj_check_type(object, &lv_slider_class)) {
        lua_pushinteger(state, lv_slider_get_value(object));
    } else if (lv_obj_check_type(object, &lv_bar_class)) {
        lua_pushinteger(state, lv_bar_get_value(object));
    } else if (luavgl_is_arc(object)) {
        lua_pushinteger(state, lv_arc_get_value(object));
    } else if (luavgl_is_spinbox(object)) {
        lua_pushinteger(state, lv_spinbox_get_value(object));
    } else if (lv_obj_check_type(object, &lv_dropdown_class)) {
        lua_pushinteger(state, lv_dropdown_get_selected(object) + 1); // 1-based, Lua style
    } else if (lv_obj_check_type(object, &lv_roller_class)) {
        lua_pushinteger(state, lv_roller_get_selected(object) + 1);
    } else {
        return luaL_error(state, "this widget has no value");
    }

    return 1;
}

/** `widget:text()` - the text of a label, checkbox, textarea, or a dropdown's selection. */
int object_text(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);

    if (luavgl_is_chip(object)) {
        auto* label = lv_obj_get_child(object, 0);
        lua_pushstring(state, label != nullptr ? lv_label_get_text(label) : "");
    } else if (lv_obj_check_type(object, &lv_label_class)) {
        lua_pushstring(state, lv_label_get_text(object));
    } else if (lv_obj_check_type(object, &lv_checkbox_class)) {
        lua_pushstring(state, lv_checkbox_get_text(object));
    } else if (lv_obj_check_type(object, &lv_textarea_class)) {
        lua_pushstring(state, lv_textarea_get_text(object));
    } else if (lv_obj_check_type(object, &lv_dropdown_class)) {
        char buffer[128];
        lv_dropdown_get_selected_str(object, buffer, sizeof(buffer));
        lua_pushstring(state, buffer);
    } else if (lv_obj_check_type(object, &lv_roller_class)) {
        char buffer[128];
        lv_roller_get_selected_str(object, buffer, sizeof(buffer));
        lua_pushstring(state, buffer);
    } else {
        return luaL_error(state, "this widget has no text");
    }

    return 1;
}

/**
 * `widget:is_checked()` - state of a switch or checkbox.
 *
 * Both use LV_STATE_CHECKED rather than a value, so this reads the state flag.
 */
int object_is_checked(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);

    if (!lv_obj_check_type(object, &lv_switch_class) &&
        !lv_obj_check_type(object, &lv_checkbox_class) &&
        !luavgl_is_chip(object)) {
        return luaL_error(state, "this widget has no checked state");
    }

    lua_pushboolean(state, lv_obj_has_state(object, LV_STATE_CHECKED));
    return 1;
}

/** `widget:set_checked(bool)` - for switches and checkboxes. Returns the widget. */
int object_set_checked(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    luaL_checktype(state, 2, LUA_TBOOLEAN);

    if (!lv_obj_check_type(object, &lv_switch_class) &&
        !lv_obj_check_type(object, &lv_checkbox_class) &&
        !luavgl_is_chip(object)) {
        return luaL_error(state, "this widget has no checked state");
    }

    if (lua_toboolean(state, 2)) {
        lv_obj_add_state(object, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(object, LV_STATE_CHECKED);
    }

    lua_settop(state, 1);
    return 1;
}

// Text area editing -----------------------------------------------------------------------

/** Raises unless the widget is a text area, for the editing methods below. */
lv_obj_t* check_textarea(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);

    if (!lv_obj_check_type(object, &lv_textarea_class)) {
        luaL_error(state, "this widget is not a text area");
    }

    return object;
}

/**
 * `ta:cursor()` / `ta:set_cursor(position)` - the caret, as a 1-based character offset.
 *
 * 1-based to match the rest of Lua (and this binding's dropdown/roller selections), so a
 * script can index the text with the same number it gets back.
 */
int textarea_cursor(lua_State* state) {
    lua_pushinteger(state, lv_textarea_get_cursor_pos(check_textarea(state)) + 1);
    return 1;
}

int textarea_set_cursor(lua_State* state) {
    auto* object = check_textarea(state);
    const auto position = static_cast<int32_t>(luaL_checkinteger(state, 2));

    lv_textarea_set_cursor_pos(object, position - 1);

    lua_settop(state, 1);
    return 1;
}

/** `ta:insert(text)` - inserts at the cursor, as typing would. Returns the widget. */
int textarea_insert(lua_State* state) {
    auto* object = check_textarea(state);
    lv_textarea_add_text(object, luaL_checkstring(state, 2));

    lua_settop(state, 1);
    return 1;
}

/** `ta:delete_char()` - removes the character before the cursor. Returns the widget. */
int textarea_delete_char(lua_State* state) {
    auto* object = check_textarea(state);
    lv_textarea_delete_char(object);

    lua_settop(state, 1);
    return 1;
}

// Batch 2 accessors -----------------------------------------------------------------------

/** Raises unless the handle is a `cls`, then returns it. */
lv_obj_t* check_widget_of(lua_State* state, int index, const lv_obj_class_t* cls, const char* name) {
    auto* object = luavgl_check_object(state, index);

    if (!lv_obj_check_type(object, cls)) {
        luaL_error(state, "%s expected", name);
    }

    return object;
}

// LED -------------------------------------------------------------------------------------

int led_brightness(lua_State* state) {
    lua_pushinteger(state, lv_led_get_brightness(check_widget_of(state, 1, &lv_led_class, "LED")));
    return 1;
}

int led_on(lua_State* state) {
    lv_led_on(check_widget_of(state, 1, &lv_led_class, "LED"));

    lua_settop(state, 1);
    return 1;
}

int led_off(lua_State* state) {
    lv_led_off(check_widget_of(state, 1, &lv_led_class, "LED"));

    lua_settop(state, 1);
    return 1;
}

int led_toggle(lua_State* state) {
    lv_led_toggle(check_widget_of(state, 1, &lv_led_class, "LED"));

    lua_settop(state, 1);
    return 1;
}

// Line ------------------------------------------------------------------------------------
//
// LVGL keeps the points array by pointer rather than copying it, so the binding owns a
// heap array. The array lives in a side list keyed by the line object because the object's
// user data holds the line tag.

struct LinePoints {
    lv_obj_t* line;
    lv_point_precise_t* points;
    LinePoints* next;
};

LinePoints* line_points_list = nullptr;

LinePoints* find_line_points(lv_obj_t* line) {
    for (auto* entry = line_points_list; entry != nullptr; entry = entry->next) {
        if (entry->line == line) {
            return entry;
        }
    }
    return nullptr;
}

void remove_line_points(lv_obj_t* line) {
    LinePoints** link = &line_points_list;
    while (*link != nullptr) {
        LinePoints* entry = *link;
        if (entry->line == line) {
            *link = entry->next;
            free(entry->points);
            free(entry);
            return;
        }
        link = &entry->next;
    }
}

void line_points_deleted(lv_event_t* event) {
    remove_line_points(lv_event_get_target_obj(event));
}

/** `line:set_points{ x1, y1, x2, y2, ... }` - coordinates relative to the line object. */
int line_set_points(lua_State* state) {
    auto* line = check_tagged_widget(state, 1, &LINE_TAG, "line");
    luaL_checktype(state, 2, LUA_TTABLE);

    const size_t value_count = luaL_len(state, 2);
    if (value_count < 4 || value_count % 2 != 0) {
        return luaL_error(state, "points must be an even list of at least 4 numbers");
    }

    const size_t point_count = value_count / 2;
    auto* points = static_cast<lv_point_precise_t*>(
        malloc(sizeof(lv_point_precise_t) * point_count)
    );
    if (points == nullptr) {
        return luaL_error(state, "out of memory for line points");
    }

    for (size_t index = 0; index < point_count; index++) {
        lua_geti(state, 2, static_cast<lua_Integer>(index * 2 + 1));
        const lua_Number x = luaL_checknumber(state, -1);
        lua_pop(state, 1);
        lua_geti(state, 2, static_cast<lua_Integer>(index * 2 + 2));
        const lua_Number y = luaL_checknumber(state, -1);
        lua_pop(state, 1);
        points[index].x = static_cast<lv_value_precise_t>(x);
        points[index].y = static_cast<lv_value_precise_t>(y);
    }

    LinePoints* entry = find_line_points(line);
    if (entry != nullptr) {
        free(entry->points);
        entry->points = points;
    } else {
        entry = static_cast<LinePoints*>(malloc(sizeof(LinePoints)));
        if (entry == nullptr) {
            free(points);
            return luaL_error(state, "out of memory for line points");
        }
        entry->line = line;
        entry->points = points;
        entry->next = line_points_list;
        line_points_list = entry;
        lv_obj_add_event_cb(line, line_points_deleted, LV_EVENT_DELETE, nullptr);
    }

    lv_line_set_points(line, points, static_cast<uint32_t>(point_count));

    lua_settop(state, 1);
    return 1;
}

// Table -----------------------------------------------------------------------------------

/** 1-based, like `child()`, the dropdowns and the text-area cursor. */
void check_table_cell(lua_State* state, lv_obj_t* table, lua_Integer row, lua_Integer column) {
    if (row < 1 || column < 1 ||
        static_cast<uint32_t>(row) > lv_table_get_row_count(table) ||
        static_cast<uint32_t>(column) > lv_table_get_column_count(table)) {
        luaL_error(state, "table cell out of bounds");
    }
}

int table_dimensions(lua_State* state) {
    auto* table = check_widget_of(state, 1, &lv_table_class, "table");

    lua_pushinteger(state, lv_table_get_row_count(table));
    lua_pushinteger(state, lv_table_get_column_count(table));
    return 2;
}

int table_set_cell(lua_State* state) {
    auto* table = check_widget_of(state, 1, &lv_table_class, "table");
    const auto row = luaL_checkinteger(state, 2);
    const auto column = luaL_checkinteger(state, 3);
    check_table_cell(state, table, row, column);

    lv_table_set_cell_value(
        table,
        static_cast<uint32_t>(row - 1),
        static_cast<uint32_t>(column - 1),
        luaL_checkstring(state, 4)
    );

    lua_settop(state, 1);
    return 1;
}

int table_cell(lua_State* state) {
    auto* table = check_widget_of(state, 1, &lv_table_class, "table");
    const auto row = luaL_checkinteger(state, 2);
    const auto column = luaL_checkinteger(state, 3);
    check_table_cell(state, table, row, column);

    lua_pushstring(
        state,
        lv_table_get_cell_value(
            table,
            static_cast<uint32_t>(row - 1),
            static_cast<uint32_t>(column - 1)
        )
    );
    return 1;
}

int table_set_cell_ctrl(lua_State* state) {
    auto* table = check_widget_of(state, 1, &lv_table_class, "table");
    const auto row = luaL_checkinteger(state, 2);
    const auto column = luaL_checkinteger(state, 3);
    check_table_cell(state, table, row, column);

    lv_table_set_cell_ctrl(
        table,
        static_cast<uint32_t>(row - 1),
        static_cast<uint32_t>(column - 1),
        static_cast<lv_table_cell_ctrl_t>(luaL_checkinteger(state, 4))
    );

    lua_settop(state, 1);
    return 1;
}

int table_clear_cell_ctrl(lua_State* state) {
    auto* table = check_widget_of(state, 1, &lv_table_class, "table");
    const auto row = luaL_checkinteger(state, 2);
    const auto column = luaL_checkinteger(state, 3);
    check_table_cell(state, table, row, column);

    lv_table_clear_cell_ctrl(
        table,
        static_cast<uint32_t>(row - 1),
        static_cast<uint32_t>(column - 1),
        static_cast<lv_table_cell_ctrl_t>(luaL_checkinteger(state, 4))
    );

    lua_settop(state, 1);
    return 1;
}

int table_has_cell_ctrl(lua_State* state) {
    auto* table = check_widget_of(state, 1, &lv_table_class, "table");
    const auto row = luaL_checkinteger(state, 2);
    const auto column = luaL_checkinteger(state, 3);
    check_table_cell(state, table, row, column);

    lua_pushboolean(
        state,
        lv_table_has_cell_ctrl(
            table,
            static_cast<uint32_t>(row - 1),
            static_cast<uint32_t>(column - 1),
            static_cast<lv_table_cell_ctrl_t>(luaL_checkinteger(state, 4))
        )
    );
    return 1;
}

int table_select_cell(lua_State* state) {
    auto* table = check_widget_of(state, 1, &lv_table_class, "table");
    const auto row = luaL_checkinteger(state, 2);
    const auto column = luaL_checkinteger(state, 3);
    check_table_cell(state, table, row, column);

    lv_table_set_selected_cell(
        table,
        static_cast<uint16_t>(row - 1),
        static_cast<uint16_t>(column - 1)
    );

    lua_settop(state, 1);
    return 1;
}

/** Returns the selected cell as two 1-based numbers, or nothing when none is selected. */
int table_selected_cell(lua_State* state) {
    auto* table = check_widget_of(state, 1, &lv_table_class, "table");

    uint32_t row = 0;
    uint32_t column = 0;
    lv_table_get_selected_cell(table, &row, &column);

    if (row == LV_TABLE_CELL_NONE || column == LV_TABLE_CELL_NONE) {
        return 0;
    }

    lua_pushinteger(state, static_cast<lua_Integer>(row) + 1);
    lua_pushinteger(state, static_cast<lua_Integer>(column) + 1);
    return 2;
}

// Scale -----------------------------------------------------------------------------------

lv_scale_section_t* check_scale_section(lua_State* state, int index) {
    if (lua_type(state, index) != LUA_TLIGHTUSERDATA) {
        luaL_error(state, "scale section expected");
    }

    return static_cast<lv_scale_section_t*>(lua_touserdata(state, index));
}

/** `scale:add_section(min, max)` - a coloured range. Returns its handle. */
int scale_add_section(lua_State* state) {
    auto* scale = check_widget_of(state, 1, &lv_scale_class, "scale");
    const auto min = static_cast<int32_t>(luaL_checkinteger(state, 2));
    const auto max = static_cast<int32_t>(luaL_checkinteger(state, 3));

    auto* section = lv_scale_add_section(scale);
    lv_scale_set_section_range(scale, section, min, max);

    lua_pushlightuserdata(state, section);
    return 1;
}

int scale_set_section_range(lua_State* state) {
    auto* scale = check_widget_of(state, 1, &lv_scale_class, "scale");
    auto* section = check_scale_section(state, 2);

    lv_scale_set_section_range(
        scale,
        section,
        static_cast<int32_t>(luaL_checkinteger(state, 3)),
        static_cast<int32_t>(luaL_checkinteger(state, 4))
    );

    lua_settop(state, 1);
    return 1;
}

/** `scale:set_section_style(section, part, style)`; part is `lvgl.PART.MAIN/INDICATOR/ITEMS`. */
int scale_set_section_style(lua_State* state) {
    auto* scale = check_widget_of(state, 1, &lv_scale_class, "scale");
    auto* section = check_scale_section(state, 2);
    const auto part = static_cast<lv_part_t>(luaL_checkinteger(state, 3));
    auto* style = luavgl_check_style(state, 4);

    switch (part) {
        case LV_PART_MAIN:
            lv_scale_set_section_style_main(scale, section, style);
            break;
        case LV_PART_INDICATOR:
            lv_scale_set_section_style_indicator(scale, section, style);
            break;
        case LV_PART_ITEMS:
            lv_scale_set_section_style_items(scale, section, style);
            break;
        default:
            return luaL_error(state, "section style part must be MAIN, INDICATOR or ITEMS");
    }

    lua_settop(state, 1);
    return 1;
}

// Chart -----------------------------------------------------------------------------------

lv_chart_series_t* check_chart_series(lua_State* state, int index) {
    if (lua_type(state, index) != LUA_TLIGHTUSERDATA) {
        luaL_error(state, "chart series expected");
    }

    return static_cast<lv_chart_series_t*>(lua_touserdata(state, index));
}

/** `chart:add_series(color [, axis])` - returns a handle to pass to the chart methods. */
int chart_add_series(lua_State* state) {
    auto* chart = check_widget_of(state, 1, &lv_chart_class, "chart");
    const lv_color_t color = lv_color_hex(static_cast<uint32_t>(luaL_checkinteger(state, 2)));
    const auto axis = static_cast<lv_chart_axis_t>(luaL_optinteger(state, 3, LV_CHART_AXIS_PRIMARY_Y));

    lua_pushlightuserdata(state, lv_chart_add_series(chart, color, axis));
    return 1;
}

int chart_remove_series(lua_State* state) {
    auto* chart = check_widget_of(state, 1, &lv_chart_class, "chart");

    lv_chart_remove_series(chart, check_chart_series(state, 2));

    lua_settop(state, 1);
    return 1;
}

int chart_set_values(lua_State* state) {
    auto* chart = check_widget_of(state, 1, &lv_chart_class, "chart");
    auto* series = check_chart_series(state, 2);
    luaL_checktype(state, 3, LUA_TTABLE);

    const lua_Integer count = luaL_len(state, 3);
    if (count <= 0) {
        lua_settop(state, 1);
        return 1;
    }

    int32_t stack_values[64];
    int32_t* values = stack_values;
    if (count > static_cast<lua_Integer>(sizeof(stack_values) / sizeof(stack_values[0]))) {
        values = static_cast<int32_t*>(lv_malloc(static_cast<size_t>(count) * sizeof(int32_t)));
        if (values == nullptr) {
            return luaL_error(state, "out of memory for chart values");
        }
    }

    for (lua_Integer i = 1; i <= count; i++) {
        lua_rawgeti(state, 3, i);
        values[i - 1] = static_cast<int32_t>(luaL_checkinteger(state, -1));
        lua_pop(state, 1);
    }

    lv_chart_set_series_values(chart, series, values, static_cast<size_t>(count));

    if (values != stack_values) {
        lv_free(values);
    }

    lua_settop(state, 1);
    return 1;
}

int chart_next_value(lua_State* state) {
    auto* chart = check_widget_of(state, 1, &lv_chart_class, "chart");

    lv_chart_set_next_value(
        chart,
        check_chart_series(state, 2),
        static_cast<int32_t>(luaL_checkinteger(state, 3))
    );

    lua_settop(state, 1);
    return 1;
}

int chart_set_all_values(lua_State* state) {
    auto* chart = check_widget_of(state, 1, &lv_chart_class, "chart");

    lv_chart_set_all_values(
        chart,
        check_chart_series(state, 2),
        static_cast<int32_t>(luaL_checkinteger(state, 3))
    );

    lua_settop(state, 1);
    return 1;
}

int chart_set_series_color(lua_State* state) {
    auto* chart = check_widget_of(state, 1, &lv_chart_class, "chart");
    auto* series = check_chart_series(state, 2);
    const lv_color_t color = lv_color_hex(static_cast<uint32_t>(luaL_checkinteger(state, 3)));

    lv_chart_set_series_color(chart, series, color);

    lua_settop(state, 1);
    return 1;
}

int chart_hide_series(lua_State* state) {
    auto* chart = check_widget_of(state, 1, &lv_chart_class, "chart");
    auto* series = check_chart_series(state, 2);
    luaL_checktype(state, 3, LUA_TBOOLEAN);

    lv_chart_hide_series(chart, series, lua_toboolean(state, 3) != 0);

    lua_settop(state, 1);
    return 1;
}

int chart_refresh(lua_State* state) {
    lv_chart_refresh(check_widget_of(state, 1, &lv_chart_class, "chart"));

    lua_settop(state, 1);
    return 1;
}

// Calendar --------------------------------------------------------------------------------

void push_calendar_date(lua_State* state, const lv_calendar_date_t* date) {
    lua_createtable(state, 0, 3);
    lua_pushinteger(state, static_cast<lua_Integer>(date->year));
    lua_setfield(state, -2, "year");
    lua_pushinteger(state, static_cast<lua_Integer>(date->month));
    lua_setfield(state, -2, "month");
    lua_pushinteger(state, static_cast<lua_Integer>(date->day));
    lua_setfield(state, -2, "day");
}

int calendar_today(lua_State* state) {
    auto* calendar = check_widget_of(state, 1, &lv_calendar_class, "calendar");

    push_calendar_date(state, lv_calendar_get_today_date(calendar));
    return 1;
}

int calendar_shown(lua_State* state) {
    auto* calendar = check_widget_of(state, 1, &lv_calendar_class, "calendar");

    push_calendar_date(state, lv_calendar_get_showed_date(calendar));
    return 1;
}

/** The clicked day, or nil. Set `on(VALUE_CHANGED)` and read it from the handler. */
int calendar_pressed(lua_State* state) {
    auto* calendar = check_widget_of(state, 1, &lv_calendar_class, "calendar");

    lv_calendar_date_t date;
    if (lv_calendar_get_pressed_date(calendar, &date) != LV_RESULT_OK) {
        lua_pushnil(state);
        return 1;
    }

    push_calendar_date(state, &date);
    return 1;
}

// Keyboard --------------------------------------------------------------------------------

int keyboard_textarea(lua_State* state) {
    auto* keyboard = check_widget_of(state, 1, &lv_keyboard_class, "keyboard");
    auto* textarea = lv_keyboard_get_textarea(keyboard);

    if (textarea == nullptr) {
        lua_pushnil(state);
    } else {
        luavgl_push_object(state, textarea);
    }

    return 1;
}

int keyboard_mode(lua_State* state) {
    lua_pushinteger(state, lv_keyboard_get_mode(check_widget_of(state, 1, &lv_keyboard_class, "keyboard")));
    return 1;
}

int keyboard_popovers(lua_State* state) {
    lua_pushboolean(state, lv_keyboard_get_popovers(check_widget_of(state, 1, &lv_keyboard_class, "keyboard")));
    return 1;
}

// Image button ----------------------------------------------------------------------------

/**
 * Reads an optional image source.
 *
 * A string is pinned because LVGL keeps the pointer. A light userdata value is a built-in
 * image descriptor, as returned by `lvgl.spinner_image()`, and needs no pinning.
 */
const void* read_image_source(lua_State* state, int index) {
    if (lua_isnoneornil(state, index)) {
        return nullptr;
    }

    if (lua_type(state, index) == LUA_TLIGHTUSERDATA) {
        return lua_touserdata(state, index);
    }

    return luavgl_lvgl_path(state, index);
}

/** `ib:set_image_src(state, left, mid, right)`; left/right may be nil. */
int imagebutton_set_image_src(lua_State* state) {
    auto* button = check_widget_of(state, 1, &lv_imagebutton_class, "image button");
    const auto image_state = static_cast<lv_imagebutton_state_t>(luaL_checkinteger(state, 2));

    lv_imagebutton_set_src(
        button,
        image_state,
        read_image_source(state, 3),
        read_image_source(state, 4),
        read_image_source(state, 5)
    );

    lua_settop(state, 1);
    return 1;
}

int imagebutton_set_image_state(lua_State* state) {
    auto* button = check_widget_of(state, 1, &lv_imagebutton_class, "image button");

    lv_imagebutton_set_state(button, static_cast<lv_imagebutton_state_t>(luaL_checkinteger(state, 2)));

    lua_settop(state, 1);
    return 1;
}

// Button matrix ---------------------------------------------------------------------------

int buttonmatrix_button_text(lua_State* state) {
    auto* matrix = check_widget_of(state, 1, &lv_buttonmatrix_class, "button matrix");
    const auto index = luaL_checkinteger(state, 2);
    if (index < 1) {
        return luaL_error(state, "button index is 1-based");
    }

    const char* text = lv_buttonmatrix_get_button_text(matrix, static_cast<uint32_t>(index - 1));
    if (text == nullptr) {
        lua_pushnil(state);
    } else {
        lua_pushstring(state, text);
    }

    return 1;
}

/** Returns the selected button as a 1-based number, or nil. */
int buttonmatrix_selected_button(lua_State* state) {
    auto* matrix = check_widget_of(state, 1, &lv_buttonmatrix_class, "button matrix");
    const uint32_t selected = lv_buttonmatrix_get_selected_button(matrix);

    if (selected == LV_BUTTONMATRIX_BUTTON_NONE) {
        lua_pushnil(state);
    } else {
        lua_pushinteger(state, static_cast<lua_Integer>(selected) + 1);
    }

    return 1;
}

/** `matrix:select_button(index)`; nil or 0 clears the selection. */
int buttonmatrix_select_button(lua_State* state) {
    auto* matrix = check_widget_of(state, 1, &lv_buttonmatrix_class, "button matrix");

    uint32_t index = LV_BUTTONMATRIX_BUTTON_NONE;
    if (!lua_isnoneornil(state, 2)) {
        const auto value = luaL_checkinteger(state, 2);
        index = value <= 0 ? LV_BUTTONMATRIX_BUTTON_NONE : static_cast<uint32_t>(value - 1);
    }

    lv_buttonmatrix_set_selected_button(matrix, index);

    lua_settop(state, 1);
    return 1;
}

int buttonmatrix_set_button_ctrl(lua_State* state) {
    auto* matrix = check_widget_of(state, 1, &lv_buttonmatrix_class, "button matrix");
    const auto index = luaL_checkinteger(state, 2);
    if (index < 1) {
        return luaL_error(state, "button index is 1-based");
    }

    lv_buttonmatrix_set_button_ctrl(
        matrix,
        static_cast<uint32_t>(index - 1),
        static_cast<lv_buttonmatrix_ctrl_t>(luaL_checkinteger(state, 3))
    );

    lua_settop(state, 1);
    return 1;
}

int buttonmatrix_clear_button_ctrl(lua_State* state) {
    auto* matrix = check_widget_of(state, 1, &lv_buttonmatrix_class, "button matrix");
    const auto index = luaL_checkinteger(state, 2);
    if (index < 1) {
        return luaL_error(state, "button index is 1-based");
    }

    lv_buttonmatrix_clear_button_ctrl(
        matrix,
        static_cast<uint32_t>(index - 1),
        static_cast<lv_buttonmatrix_ctrl_t>(luaL_checkinteger(state, 3))
    );

    lua_settop(state, 1);
    return 1;
}

int buttonmatrix_has_button_ctrl(lua_State* state) {
    auto* matrix = check_widget_of(state, 1, &lv_buttonmatrix_class, "button matrix");
    const auto index = luaL_checkinteger(state, 2);
    if (index < 1) {
        return luaL_error(state, "button index is 1-based");
    }

    lua_pushboolean(
        state,
        lv_buttonmatrix_has_button_ctrl(
            matrix,
            static_cast<uint32_t>(index - 1),
            static_cast<lv_buttonmatrix_ctrl_t>(luaL_checkinteger(state, 3))
        )
    );
    return 1;
}

int buttonmatrix_set_button_width(lua_State* state) {
    auto* matrix = check_widget_of(state, 1, &lv_buttonmatrix_class, "button matrix");
    const auto index = luaL_checkinteger(state, 2);
    if (index < 1) {
        return luaL_error(state, "button index is 1-based");
    }

    lv_buttonmatrix_set_button_width(
        matrix,
        static_cast<uint32_t>(index - 1),
        static_cast<uint32_t>(luaL_checkinteger(state, 3))
    );

    lua_settop(state, 1);
    return 1;
}

// Tabview ---------------------------------------------------------------------------------

/** `tabview:add_tab(title)` - adds a tab and returns its page. */
int tabview_add_tab(lua_State* state) {
    auto* view = check_widget_of(state, 1, &lv_tabview_class, "tabview");

    luavgl_push_object(state, lv_tabview_add_tab(view, luaL_checkstring(state, 2)));
    return 1;
}

int tabview_set_tab_text(lua_State* state) {
    auto* view = check_widget_of(state, 1, &lv_tabview_class, "tabview");
    const auto index = luaL_checkinteger(state, 2);
    if (index < 1) {
        return luaL_error(state, "tab index is 1-based");
    }

    lv_tabview_set_tab_text(view, static_cast<uint32_t>(index - 1), luaL_checkstring(state, 3));

    lua_settop(state, 1);
    return 1;
}

/** The active tab as a 1-based number, or nil when the tabview has no tabs. */
int tabview_active(lua_State* state) {
    auto* view = check_widget_of(state, 1, &lv_tabview_class, "tabview");

    if (lv_tabview_get_tab_count(view) == 0) {
        lua_pushnil(state);
    } else {
        lua_pushinteger(state, static_cast<lua_Integer>(lv_tabview_get_tab_active(view)) + 1);
    }

    return 1;
}

/** `tabview:set_active(index [, animated])`; 1-based, like every other Lua index here. */
int tabview_set_active(lua_State* state) {
    auto* view = check_widget_of(state, 1, &lv_tabview_class, "tabview");
    const auto index = luaL_checkinteger(state, 2);
    if (index < 1) {
        return luaL_error(state, "tab index is 1-based");
    }

    const auto anim = lua_toboolean(state, 3) ? LV_ANIM_ON : LV_ANIM_OFF;
    lv_tabview_set_active(view, static_cast<uint32_t>(index - 1), anim);

    lua_settop(state, 1);
    return 1;
}

int tabview_tab_count(lua_State* state) {
    lua_pushinteger(
        state,
        lv_tabview_get_tab_count(check_widget_of(state, 1, &lv_tabview_class, "tabview"))
    );
    return 1;
}

int tabview_tab_button(lua_State* state) {
    auto* view = check_widget_of(state, 1, &lv_tabview_class, "tabview");
    const auto index = luaL_checkinteger(state, 2);
    if (index < 1) {
        return luaL_error(state, "tab index is 1-based");
    }

    auto* button = lv_tabview_get_tab_button(view, static_cast<int32_t>(index - 1));
    if (button == nullptr) {
        lua_pushnil(state);
    } else {
        luavgl_push_object(state, button);
    }

    return 1;
}

int tabview_content(lua_State* state) {
    auto* view = check_widget_of(state, 1, &lv_tabview_class, "tabview");
    luavgl_push_object(state, lv_tabview_get_content(view));
    return 1;
}

int tabview_tab_bar(lua_State* state) {
    auto* view = check_widget_of(state, 1, &lv_tabview_class, "tabview");
    luavgl_push_object(state, lv_tabview_get_tab_bar(view));
    return 1;
}

// Tabs (Material 3) ------------------------------------------------------------------------

/** 1-based tab index, like every other Lua index in this binding. */
uint32_t check_tab_index(lua_State* state, int index) {
    const auto value = luaL_checkinteger(state, index);
    if (value < 1) {
        luaL_error(state, "tab index is 1-based");
    }
    return static_cast<uint32_t>(value - 1);
}

/** `tabs:add_tab(text)` - returns the tab object. */
int tabs_add_tab(lua_State* state) {
    auto* tabs = check_tagged_widget(state, 1, &TABS_TAG, "tabs");
    luavgl_push_object(state, lvgl_tabs_add_tab(tabs, luaL_checkstring(state, 2)));
    return 1;
}

/** `tabs:add_tab_with_icon(icon, text)` - `icon` is a `lvgl.ICON.*` glyph. */
int tabs_add_tab_with_icon(lua_State* state) {
    auto* tabs = check_tagged_widget(state, 1, &TABS_TAG, "tabs");
    luavgl_push_object(state, lvgl_tabs_add_tab_with_icon(
        tabs,
        luaL_checkstring(state, 2),
        luaL_checkstring(state, 3)
    ));
    return 1;
}

int tabs_set_tab_text(lua_State* state) {
    auto* tabs = check_tagged_widget(state, 1, &TABS_TAG, "tabs");
    lvgl_tabs_set_tab_text(tabs, check_tab_index(state, 2), luaL_checkstring(state, 3));
    lua_settop(state, 1);
    return 1;
}

int tabs_set_tab_badge(lua_State* state) {
    auto* tabs = check_tagged_widget(state, 1, &TABS_TAG, "tabs");
    lvgl_tabs_set_tab_badge(tabs, check_tab_index(state, 2), lua_toboolean(state, 3) != 0);
    lua_settop(state, 1);
    return 1;
}

int tabs_set_tab_hidden(lua_State* state) {
    auto* tabs = check_tagged_widget(state, 1, &TABS_TAG, "tabs");
    lvgl_tabs_set_tab_hidden(tabs, check_tab_index(state, 2), lua_toboolean(state, 3) != 0);
    lua_settop(state, 1);
    return 1;
}

int tabs_set_mode(lua_State* state) {
    auto* tabs = check_tagged_widget(state, 1, &TABS_TAG, "tabs");
    const auto mode = luaL_checkinteger(state, 2);

    if (mode < LVGL_TABS_MODE_FIXED || mode > LVGL_TABS_MODE_WRAP) {
        return luaL_error(state, "mode must be lvgl.TABS_MODE.*");
    }

    lvgl_tabs_set_mode(tabs, static_cast<LvglTabsMode>(mode));
    lua_settop(state, 1);
    return 1;
}

int tabs_set_selected(lua_State* state) {
    auto* tabs = check_tagged_widget(state, 1, &TABS_TAG, "tabs");
    lvgl_tabs_set_selected(tabs, check_tab_index(state, 2));
    lua_settop(state, 1);
    return 1;
}

/** The selected tab as a 1-based number, or nil when there are no tabs. */
int tabs_selected(lua_State* state) {
    auto* tabs = check_tagged_widget(state, 1, &TABS_TAG, "tabs");

    if (lv_obj_get_child_count(tabs) == 0) {
        lua_pushnil(state);
    } else {
        lua_pushinteger(state, static_cast<lua_Integer>(lvgl_tabs_get_selected(tabs)) + 1);
    }

    return 1;
}

/* `add_tab` and `set_tab_text` exist on both Tabview and Tabs. Dispatch on the tag so a
 * script can use the natural name for either widget. */
int add_tab_dispatch(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    if (lv_obj_get_user_data(object) == &TABS_TAG) {
        return tabs_add_tab(state);
    }
    return tabview_add_tab(state);
}

int set_tab_text_dispatch(lua_State* state) {
    auto* object = luavgl_check_object(state, 1);
    if (lv_obj_get_user_data(object) == &TABS_TAG) {
        return tabs_set_tab_text(state);
    }
    return tabview_set_tab_text(state);
}

// Statusline -------------------------------------------------------------------------------

int statusline_set_text(lua_State* state) {
    auto* line = check_tagged_widget(state, 1, &STATUSLINE_TAG, "statusline");
    lvgl_statusline_set_text(line, luaL_checkstring(state, 2));
    lua_settop(state, 1);
    return 1;
}

int statusline_set_level(lua_State* state) {
    auto* line = check_tagged_widget(state, 1, &STATUSLINE_TAG, "statusline");
    const auto level = luaL_checkinteger(state, 2);

    if (level < LVGL_STATUSLINE_LEVEL_OK || level > LVGL_STATUSLINE_LEVEL_ERROR) {
        return luaL_error(state, "level must be lvgl.STATUSLINE_LEVEL.*");
    }

    lvgl_statusline_set_level(line, static_cast<LvglStatuslineLevel>(level));
    lua_settop(state, 1);
    return 1;
}

// Tileview --------------------------------------------------------------------------------

/** `tileview:add_tile(column, row, dir)` - 0-based coordinates, like `grid_cell()`. */
int tileview_add_tile(lua_State* state) {
    auto* view = check_widget_of(state, 1, &lv_tileview_class, "tileview");

    luavgl_push_object(
        state,
        lv_tileview_add_tile(
            view,
            static_cast<uint8_t>(luaL_checkinteger(state, 2)),
            static_cast<uint8_t>(luaL_checkinteger(state, 3)),
            static_cast<lv_dir_t>(luaL_checkinteger(state, 4))
        )
    );
    return 1;
}

int tileview_set_tile(lua_State* state) {
    auto* view = check_widget_of(state, 1, &lv_tileview_class, "tileview");
    auto* tile = luavgl_check_object(state, 2);
    const auto anim = lua_toboolean(state, 3) ? LV_ANIM_ON : LV_ANIM_OFF;

    lv_tileview_set_tile(view, tile, anim);

    lua_settop(state, 1);
    return 1;
}

int tileview_goto_tile(lua_State* state) {
    auto* view = check_widget_of(state, 1, &lv_tileview_class, "tileview");
    const auto anim = lua_toboolean(state, 4) ? LV_ANIM_ON : LV_ANIM_OFF;

    lv_tileview_set_tile_by_index(
        view,
        static_cast<uint32_t>(luaL_checkinteger(state, 2)),
        static_cast<uint32_t>(luaL_checkinteger(state, 3)),
        anim
    );

    lua_settop(state, 1);
    return 1;
}

int tileview_active_tile(lua_State* state) {
    auto* view = check_widget_of(state, 1, &lv_tileview_class, "tileview");
    auto* tile = lv_tileview_get_tile_active(view);

    if (tile == nullptr) {
        lua_pushnil(state);
    } else {
        luavgl_push_object(state, tile);
    }

    return 1;
}

const luaL_Reg widget_methods[] = {
    { "value", object_value },
    { "text", object_text },
    { "is_checked", object_is_checked },
    { "set_checked", object_set_checked },
    { "cursor", textarea_cursor },
    { "set_cursor", textarea_set_cursor },
    { "insert", textarea_insert },
    { "delete_char", textarea_delete_char },
    { "brightness", led_brightness },
    { "led_on", led_on },
    { "led_off", led_off },
    { "led_toggle", led_toggle },
    { "dimensions", table_dimensions },
    { "set_cell", table_set_cell },
    { "cell", table_cell },
    { "set_cell_ctrl", table_set_cell_ctrl },
    { "clear_cell_ctrl", table_clear_cell_ctrl },
    { "has_cell_ctrl", table_has_cell_ctrl },
    { "select_cell", table_select_cell },
    { "selected_cell", table_selected_cell },
    { "add_section", scale_add_section },
    { "set_section_range", scale_set_section_range },
    { "set_section_style", scale_set_section_style },
    { "add_series", chart_add_series },
    { "remove_series", chart_remove_series },
    { "set_values", chart_set_values },
    { "next_value", chart_next_value },
    { "set_all_values", chart_set_all_values },
    { "set_series_color", chart_set_series_color },
    { "hide_series", chart_hide_series },
    { "refresh", chart_refresh },
    { "today", calendar_today },
    { "shown", calendar_shown },
    { "pressed", calendar_pressed },
    { "textarea", keyboard_textarea },
    { "keyboard_mode", keyboard_mode },
    { "keyboard_popovers", keyboard_popovers },
    { "set_image_src", imagebutton_set_image_src },
    { "set_image_state", imagebutton_set_image_state },
    { "button_text", buttonmatrix_button_text },
    { "selected_button", buttonmatrix_selected_button },
    { "select_button", buttonmatrix_select_button },
    { "set_button_ctrl", buttonmatrix_set_button_ctrl },
    { "clear_button_ctrl", buttonmatrix_clear_button_ctrl },
    { "has_button_ctrl", buttonmatrix_has_button_ctrl },
    { "set_button_width", buttonmatrix_set_button_width },
    { "add_tab", add_tab_dispatch },
    { "set_tab_text", set_tab_text_dispatch },
    { "active", tabview_active },
    { "set_active", tabview_set_active },
    { "tab_count", tabview_tab_count },
    { "tab_button", tabview_tab_button },
    { "content", tabview_content },
    { "tab_bar", tabview_tab_bar },
    { "add_tab_with_icon", tabs_add_tab_with_icon },
    { "set_tab_badge", tabs_set_tab_badge },
    { "set_tab_hidden", tabs_set_tab_hidden },
    { "set_mode", tabs_set_mode },
    { "set_selected", tabs_set_selected },
    { "selected", tabs_selected },
    { "set_text", statusline_set_text },
    { "set_level", statusline_set_level },
    { "set_points", line_set_points },
    { "add_tile", tileview_add_tile },
    { "set_tile", tileview_set_tile },
    { "goto_tile", tileview_goto_tile },
    { "active_tile", tileview_active_tile },
    { "add_text", list_add_text },
    { "grid", object_grid },
    { "grid_align", object_grid_align },
    { "grid_cell", object_grid_cell },
    { "add_button", list_add_button },
    { "hide_last_divider", list_hide_last_divider },
    { "set_page_count", page_indicator_set_page_count },
    { "set_page", page_indicator_set_page },
    { "set_vertical", page_indicator_set_vertical },
    { "set_step_count", step_indicator_set_step_count },
    { "set_finished_count", step_indicator_set_finished_count },
    { nullptr, nullptr }
};

const luaL_Reg widget_constructors[] = {
    { "Slider", widget_slider },
    { "Switch", widget_switch },
    { "Checkbox", widget_checkbox },
    { "Bar", widget_bar },
    { "Arc", widget_arc },
    { "Spinbox", widget_spinbox },
    { "Dropdown", widget_dropdown },
    { "Roller", widget_roller },
    { "TextArea", widget_textarea },
    { "Line", widget_line },
    { "Image", widget_image },
    { "Spinner", widget_spinner },
    { "LED", widget_led },
    { "Table", widget_table },
    { "Scale", widget_scale },
    { "ImageButton", widget_imagebutton },
    { "Chart", widget_chart },
    { "Calendar", widget_calendar },
    { "Keyboard", widget_keyboard },
    { "ButtonMatrix", widget_buttonmatrix },
    { "Tabview", widget_tabview },
    { "Tileview", widget_tileview },
    { "Tabs", widget_tabs },
    { "Statusline", widget_statusline },
    { "Card", widget_card },
    { "Badge", widget_badge },
    { "Chip", widget_chip },
    { "SliderBox", widget_sliderbox },
    { "List", widget_list },
    { "PageIndicator", widget_page_indicator },
    { "StepIndicator", widget_step_indicator },
    { "IconButton", widget_icon_button },
    { "TextButton", widget_text_button },
    { nullptr, nullptr }
};

} // namespace

bool luavgl_is_chip(lv_obj_t* object) {
    return lv_obj_get_user_data(object) == &CHIP_TAG;
}

bool luavgl_is_sliderbox(lv_obj_t* object) {
    return lv_obj_get_user_data(object) == &SLIDERBOX_TAG;
}

bool luavgl_is_arc(lv_obj_t* object) {
    return lv_obj_get_user_data(object) == &ARC_TAG;
}

bool luavgl_is_spinbox(lv_obj_t* object) {
    return lv_obj_get_user_data(object) == &SPINBOX_TAG;
}

void luavgl_pin_string(lua_State* state, int index) {
    const int absolute_index = lua_absindex(state, index);

    lua_pushliteral(state, "luavgl.pinned_strings");
    if (lua_rawget(state, LUA_REGISTRYINDEX) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushliteral(state, "luavgl.pinned_strings");
        lua_pushvalue(state, -2);
        lua_rawset(state, LUA_REGISTRYINDEX);
    }

    lua_pushvalue(state, absolute_index);
    lua_pushboolean(state, 1);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

const char* luavgl_lvgl_path(lua_State* state, int index) {
    const char* path = luaL_checkstring(state, index);

    // Symbols and any path that already carries a drive letter pass through unchanged.
    if (path[0] != '/') {
        luavgl_pin_string(state, index);
        return path;
    }

    lua_pushfstring(state, "A:%s", path);
    luavgl_pin_string(state, -1);

    const char* normalized = lua_tostring(state, -1);
    lua_pop(state, 1);
    return normalized;
}

void luavgl_add_widget_methods(lua_State* state) {
    luaL_setfuncs(state, widget_methods, 0);
}

void luavgl_open_widgets(lua_State* state) {
    luaL_setfuncs(state, widget_constructors, 0);
}
