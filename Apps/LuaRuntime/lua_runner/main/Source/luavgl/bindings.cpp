// SPDX-License-Identifier: Apache-2.0
#include <luavgl/bindings.h>
#include <luavgl/object_private.h>

#include <lua/runtime.h>

#include <lvgl/theme.h>
#include <lvgl/ui_assets.h>
#include <lvgl/widgets/icon_button.h>
#include <lvgl/widgets/statusline.h>
#include <lvgl/widgets/tabs.h>
#include <lvgl/devices/keyboard.h>
#include <lvgl/icons/shared.h>

extern "C" {
#include <lauxlib.h>
}

namespace {

// Widgets ---------------------------------------------------------------------------------

int widget_object(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_obj_create(parent); });
}

int widget_label(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_label_create(parent); });
}

int widget_button(lua_State* state) {
    return luavgl_create_widget(state, [](lv_obj_t* parent) { return lv_button_create(parent); });
}

// Theme -----------------------------------------------------------------------------------

/** The theme's accent colours, as 0xRRGGBB, for scripts that draw their own indicators. */
int lvgl_theme_color_primary(lua_State* state) {
    const lv_color_t color = lv_theme_get_color_primary(lv_screen_active());
    lua_pushinteger(state, (lua_Integer)((color.red << 16) | (color.green << 8) | color.blue));
    return 1;
}

int lvgl_theme_color_secondary(lua_State* state) {
    const lv_color_t color = lv_theme_get_color_secondary(lv_screen_active());
    lua_pushinteger(state, (lua_Integer)((color.red << 16) | (color.green << 8) | color.blue));
    return 1;
}

int lvgl_theme_color_error(lua_State* state) {
    LvglThemeSettings settings;
    lvgl_theme_get_settings(&settings);
    const lv_color_t color = settings.color_error;
    lua_pushinteger(state, (lua_Integer)((color.red << 16) | (color.green << 8) | color.blue));
    return 1;
}

/**
 * The appearance settings that scale the theme: 0 is compact, 100 is regular.
 *
 * A script that draws its own spacing can pass these to `theme_lerp` instead of
 * picking pixel values that ignore the user's density setting.
 */
int lvgl_theme_spacing(lua_State* state) {
    lua_pushinteger(state, lvgl_theme_get_spacing());
    return 1;
}

int lvgl_theme_size(lua_State* state) {
    lua_pushinteger(state, lvgl_theme_get_size());
    return 1;
}

int lvgl_theme_mono(lua_State* state) {
    lua_pushboolean(state, lvgl_theme_is_mono());
    return 1;
}

/** `theme_lerp(compact, regular [, percent])`, defaulting to the theme's spacing. */
int lvgl_theme_lerp_value(lua_State* state) {
    const auto compact = static_cast<int32_t>(luaL_checkinteger(state, 1));
    const auto regular = static_cast<int32_t>(luaL_checkinteger(state, 2));
    const auto percent = static_cast<uint8_t>(luaL_optinteger(state, 3, lvgl_theme_get_spacing()));

    lua_pushinteger(state, lvgl_theme_lerp(compact, regular, percent));
    return 1;
}

/**
 * True when a started, non-restricted hardware keyboard is present.
 *
 * This is the same check the firmware uses to decide whether to offer its
 * on-screen keyboard, so a script can hide its own software keyboard when the
 * user is typing on real keys. A keypad, encoder or trackball is registered as
 * a keyboard device but is marked restricted, so it does not count.
 */
int lvgl_hardware_keyboard_available(lua_State* state) {
    lua_pushboolean(state, lvgl_hardware_keyboard_is_available());
    return 1;
}

// Screen ----------------------------------------------------------------------------------

/** The object new widgets parent to by default. */
int lvgl_root(lua_State* state) {
    luavgl_push_object(state, luavgl_get_root(state));
    return 1;
}

int lvgl_horizontal_resolution(lua_State* state) {
    lua_pushinteger(state, lv_display_get_horizontal_resolution(lv_display_get_default()));
    return 1;
}

int lvgl_vertical_resolution(lua_State* state) {
    lua_pushinteger(state, lv_display_get_vertical_resolution(lv_display_get_default()));
    return 1;
}

/** LV_PCT is a macro taking an argument, so it cannot be a plain constant. */
int lvgl_percent(lua_State* state) {
    lua_pushinteger(state, LV_PCT(luaL_checkinteger(state, 1)));
    return 1;
}

/** LV_GRID_FR is a macro taking an argument, used in `obj:grid()`. */
int lvgl_grid_fraction(lua_State* state) {
    lua_pushinteger(state, LV_GRID_FR(luaL_checkinteger(state, 1)));
    return 1;
}

/**
 * The firmware's spinner image as an opaque image-descriptor handle.
 *
 * Image widgets and image buttons accept a path or a descriptor. There is no image
 * decoder for the device's SVG icons turned on, so a script that wants to try an image
 * source needs at least one built-in descriptor to pass.
 */
int lvgl_spinner_image(lua_State* state) {
    lua_pushlightuserdata(state, const_cast<lv_image_dsc_t*>(lvgl_get_spinner_image()));
    return 1;
}

const luaL_Reg lvgl_functions[] = {
    { "Object", widget_object },
    { "Label", widget_label },
    { "Button", widget_button },
    { "root", lvgl_root },
    { "theme_color_primary", lvgl_theme_color_primary },
    { "theme_color_secondary", lvgl_theme_color_secondary },
    { "theme_color_error", lvgl_theme_color_error },
    { "theme_spacing", lvgl_theme_spacing },
    { "theme_size", lvgl_theme_size },
    { "theme_mono", lvgl_theme_mono },
    { "theme_lerp", lvgl_theme_lerp_value },
    { "hardware_keyboard_available", lvgl_hardware_keyboard_available },
    { "HOR_RES", lvgl_horizontal_resolution },
    { "VER_RES", lvgl_vertical_resolution },
    { "PCT", lvgl_percent },
    { "GRID_FR", lvgl_grid_fraction },
    { "spinner_image", lvgl_spinner_image },
    { nullptr, nullptr }
};

// Constants -------------------------------------------------------------------------------

struct Constant {
    const char* name;
    lua_Integer value;
};

/** Adds a named sub-table of integer constants to the table on top of the stack. */
void add_constants(lua_State* state, const char* name, const Constant* constants, size_t count) {
    lua_createtable(state, 0, static_cast<int>(count));

    for (size_t i = 0; i < count; i++) {
        lua_pushinteger(state, constants[i].value);
        lua_setfield(state, -2, constants[i].name);
    }

    lua_setfield(state, -2, name);
}

const Constant ALIGN_CONSTANTS[] = {
    { "DEFAULT", LV_ALIGN_DEFAULT },
    { "TOP_LEFT", LV_ALIGN_TOP_LEFT },
    { "TOP_MID", LV_ALIGN_TOP_MID },
    { "TOP_RIGHT", LV_ALIGN_TOP_RIGHT },
    { "BOTTOM_LEFT", LV_ALIGN_BOTTOM_LEFT },
    { "BOTTOM_MID", LV_ALIGN_BOTTOM_MID },
    { "BOTTOM_RIGHT", LV_ALIGN_BOTTOM_RIGHT },
    { "LEFT_MID", LV_ALIGN_LEFT_MID },
    { "RIGHT_MID", LV_ALIGN_RIGHT_MID },
    { "CENTER", LV_ALIGN_CENTER },
};

const Constant FLEX_FLOW_CONSTANTS[] = {
    { "ROW", LV_FLEX_FLOW_ROW },
    { "COLUMN", LV_FLEX_FLOW_COLUMN },
    { "ROW_WRAP", LV_FLEX_FLOW_ROW_WRAP },
    { "COLUMN_WRAP", LV_FLEX_FLOW_COLUMN_WRAP },
};

const Constant FLEX_ALIGN_CONSTANTS[] = {
    { "START", LV_FLEX_ALIGN_START },
    { "END", LV_FLEX_ALIGN_END },
    { "CENTER", LV_FLEX_ALIGN_CENTER },
    { "SPACE_EVENLY", LV_FLEX_ALIGN_SPACE_EVENLY },
    { "SPACE_AROUND", LV_FLEX_ALIGN_SPACE_AROUND },
    { "SPACE_BETWEEN", LV_FLEX_ALIGN_SPACE_BETWEEN },
};

/** `layout = lvgl.LAYOUT.*`; `NONE` gives absolute child positioning. */
const Constant LAYOUT_CONSTANTS[] = {
    { "NONE", LV_LAYOUT_NONE },
    { "FLEX", LV_LAYOUT_FLEX },
    { "GRID", LV_LAYOUT_GRID },
};

const Constant LONG_MODE_CONSTANTS[] = {
    { "WRAP", LV_LABEL_LONG_MODE_WRAP },
    { "DOTS", LV_LABEL_LONG_MODE_DOTS },
    { "SCROLL", LV_LABEL_LONG_MODE_SCROLL },
    { "SCROLL_CIRCULAR", LV_LABEL_LONG_MODE_SCROLL_CIRCULAR },
    { "CLIP", LV_LABEL_LONG_MODE_CLIP },
};

const Constant FLAG_CONSTANTS[] = {
    { "HIDDEN", LV_OBJ_FLAG_HIDDEN },
    { "CLICKABLE", LV_OBJ_FLAG_CLICKABLE },
    { "CLICK_FOCUSABLE", LV_OBJ_FLAG_CLICK_FOCUSABLE },
    { "CHECKABLE", LV_OBJ_FLAG_CHECKABLE },
    { "SCROLLABLE", LV_OBJ_FLAG_SCROLLABLE },
    { "SCROLL_ELASTIC", LV_OBJ_FLAG_SCROLL_ELASTIC },
    { "SCROLL_MOMENTUM", LV_OBJ_FLAG_SCROLL_MOMENTUM },
    { "SCROLL_CHAIN", LV_OBJ_FLAG_SCROLL_CHAIN },
    { "SCROLL_ON_FOCUS", LV_OBJ_FLAG_SCROLL_ON_FOCUS },
    { "SCROLL_WITH_ARROW", LV_OBJ_FLAG_SCROLL_WITH_ARROW },
    { "SNAPPABLE", LV_OBJ_FLAG_SNAPPABLE },
    { "PRESS_LOCK", LV_OBJ_FLAG_PRESS_LOCK },
    { "EVENT_BUBBLE", LV_OBJ_FLAG_EVENT_BUBBLE },
    { "GESTURE_BUBBLE", LV_OBJ_FLAG_GESTURE_BUBBLE },
    { "ADV_HITTEST", LV_OBJ_FLAG_ADV_HITTEST },
    { "IGNORE_LAYOUT", LV_OBJ_FLAG_IGNORE_LAYOUT },
    { "FLOATING", LV_OBJ_FLAG_FLOATING },
    { "OVERFLOW_VISIBLE", LV_OBJ_FLAG_OVERFLOW_VISIBLE },
    { "FLEX_IN_NEW_TRACK", LV_OBJ_FLAG_FLEX_IN_NEW_TRACK },
    { "USER_1", LV_OBJ_FLAG_USER_1 },
    { "USER_2", LV_OBJ_FLAG_USER_2 },
    { "USER_3", LV_OBJ_FLAG_USER_3 },
    { "USER_4", LV_OBJ_FLAG_USER_4 },
};

const Constant TEXT_ALIGN_CONSTANTS[] = {
    { "LEFT", LV_TEXT_ALIGN_LEFT },
    { "CENTER", LV_TEXT_ALIGN_CENTER },
    { "RIGHT", LV_TEXT_ALIGN_RIGHT },
    { "AUTO", LV_TEXT_ALIGN_AUTO },
};

const Constant DIR_CONSTANTS[] = {
    { "NONE", LV_DIR_NONE },
    { "LEFT", LV_DIR_LEFT },
    { "RIGHT", LV_DIR_RIGHT },
    { "TOP", LV_DIR_TOP },
    { "BOTTOM", LV_DIR_BOTTOM },
    { "HOR", LV_DIR_HOR },
    { "VER", LV_DIR_VER },
    { "ALL", LV_DIR_ALL },
};

const Constant SCROLLBAR_MODE_CONSTANTS[] = {
    { "OFF", LV_SCROLLBAR_MODE_OFF },
    { "ON", LV_SCROLLBAR_MODE_ON },
    { "ACTIVE", LV_SCROLLBAR_MODE_ACTIVE },
    { "AUTO", LV_SCROLLBAR_MODE_AUTO },
};

const Constant SCROLL_SNAP_CONSTANTS[] = {
    { "NONE", LV_SCROLL_SNAP_NONE },
    { "START", LV_SCROLL_SNAP_START },
    { "CENTER", LV_SCROLL_SNAP_CENTER },
    { "END", LV_SCROLL_SNAP_END },
};

const Constant BORDER_SIDE_CONSTANTS[] = {
    { "NONE", LV_BORDER_SIDE_NONE },
    { "TOP", LV_BORDER_SIDE_TOP },
    { "BOTTOM", LV_BORDER_SIDE_BOTTOM },
    { "LEFT", LV_BORDER_SIDE_LEFT },
    { "RIGHT", LV_BORDER_SIDE_RIGHT },
    { "FULL", LV_BORDER_SIDE_FULL },
};

const Constant GRAD_DIR_CONSTANTS[] = {
    { "NONE", LV_GRAD_DIR_NONE },
    { "VER", LV_GRAD_DIR_VER },
    { "HOR", LV_GRAD_DIR_HOR },
};

const Constant OPACITY_CONSTANTS[] = {
    { "TRANSP", LV_OPA_TRANSP },
    { "COVER", LV_OPA_COVER },
    { "50", LV_OPA_50 },
};

/**
 * Selectors for `widget:add_style(style, selector)`.
 *
 * A selector is a part OR'd with a state - `lvgl.PART.KNOB | lvgl.STATE.PRESSED` styles a
 * slider's knob only while it is held. Without these a script can only style the main part
 * in its default state, which leaves most of LVGL's styling unreachable.
 */
const Constant PART_CONSTANTS[] = {
    { "MAIN", LV_PART_MAIN },
    { "SCROLLBAR", LV_PART_SCROLLBAR },
    { "INDICATOR", LV_PART_INDICATOR },
    { "KNOB", LV_PART_KNOB },
    { "SELECTED", LV_PART_SELECTED },
    { "ITEMS", LV_PART_ITEMS },
    { "CURSOR", LV_PART_CURSOR },
    { "ANY", LV_PART_ANY },
};

const Constant STATE_CONSTANTS[] = {
    { "DEFAULT", LV_STATE_DEFAULT },
    { "ALT", LV_STATE_ALT },
    { "CHECKED", LV_STATE_CHECKED },
    { "FOCUSED", LV_STATE_FOCUSED },
    { "FOCUS_KEY", LV_STATE_FOCUS_KEY },
    { "EDITED", LV_STATE_EDITED },
    { "HOVERED", LV_STATE_HOVERED },
    { "PRESSED", LV_STATE_PRESSED },
    { "SCROLLED", LV_STATE_SCROLLED },
    { "DISABLED", LV_STATE_DISABLED },
    { "USER_1", LV_STATE_USER_1 },
    { "USER_2", LV_STATE_USER_2 },
    { "USER_3", LV_STATE_USER_3 },
    { "USER_4", LV_STATE_USER_4 },
    { "ANY", LV_STATE_ANY },
};

/** Looks for `lvgl.IconButton{ variant = ... }`. */
const Constant ICON_BUTTON_CONSTANTS[] = {
    { "STANDARD", LVGL_ICON_BUTTON_STANDARD },
    { "FILLED", LVGL_ICON_BUTTON_FILLED },
    { "TONAL", LVGL_ICON_BUTTON_TONAL },
};

/** Looks for `lvgl.Tabs{ mode = ... }` and `tabs:set_mode(...)`. */
const Constant TABS_MODE_CONSTANTS[] = {
    { "FIXED", LVGL_TABS_MODE_FIXED },
    { "SCROLLABLE", LVGL_TABS_MODE_SCROLLABLE },
    { "WRAP", LVGL_TABS_MODE_WRAP },
};

/** Looks for `lvgl.Statusline{ level = ... }` and `statusline:set_level(...)`. */
const Constant STATUSLINE_LEVEL_CONSTANTS[] = {
    { "OK", LVGL_STATUSLINE_LEVEL_OK },
    { "WARNING", LVGL_STATUSLINE_LEVEL_WARNING },
    { "ERROR", LVGL_STATUSLINE_LEVEL_ERROR },
};

/** Aligns a child inside its grid cell, for `child:grid_cell(...)`. */
const Constant GRID_ALIGN_CONSTANTS[] = {
    { "START", LV_GRID_ALIGN_START },
    { "CENTER", LV_GRID_ALIGN_CENTER },
    { "END", LV_GRID_ALIGN_END },
    { "STRETCH", LV_GRID_ALIGN_STRETCH },
    { "SPACE_EVENLY", LV_GRID_ALIGN_SPACE_EVENLY },
    { "SPACE_AROUND", LV_GRID_ALIGN_SPACE_AROUND },
    { "SPACE_BETWEEN", LV_GRID_ALIGN_SPACE_BETWEEN },
};

const Constant TABLE_CTRL_CONSTANTS[] = {
    { "NONE", LV_TABLE_CELL_CTRL_NONE },
    { "MERGE_RIGHT", LV_TABLE_CELL_CTRL_MERGE_RIGHT },
    { "TEXT_CROP", LV_TABLE_CELL_CTRL_TEXT_CROP },
    { "CUSTOM_1", LV_TABLE_CELL_CTRL_CUSTOM_1 },
    { "CUSTOM_2", LV_TABLE_CELL_CTRL_CUSTOM_2 },
    { "CUSTOM_3", LV_TABLE_CELL_CTRL_CUSTOM_3 },
    { "CUSTOM_4", LV_TABLE_CELL_CTRL_CUSTOM_4 },
};

const Constant SCALE_MODE_CONSTANTS[] = {
    { "HORIZONTAL_TOP", LV_SCALE_MODE_HORIZONTAL_TOP },
    { "HORIZONTAL_BOTTOM", LV_SCALE_MODE_HORIZONTAL_BOTTOM },
    { "VERTICAL_LEFT", LV_SCALE_MODE_VERTICAL_LEFT },
    { "VERTICAL_RIGHT", LV_SCALE_MODE_VERTICAL_RIGHT },
    { "ROUND_INNER", LV_SCALE_MODE_ROUND_INNER },
    { "ROUND_OUTER", LV_SCALE_MODE_ROUND_OUTER },
};

const Constant IMAGEBUTTON_STATE_CONSTANTS[] = {
    { "RELEASED", LV_IMAGEBUTTON_STATE_RELEASED },
    { "PRESSED", LV_IMAGEBUTTON_STATE_PRESSED },
    { "DISABLED", LV_IMAGEBUTTON_STATE_DISABLED },
    { "CHECKED_RELEASED", LV_IMAGEBUTTON_STATE_CHECKED_RELEASED },
    { "CHECKED_PRESSED", LV_IMAGEBUTTON_STATE_CHECKED_PRESSED },
    { "CHECKED_DISABLED", LV_IMAGEBUTTON_STATE_CHECKED_DISABLED },
};

const Constant CHART_TYPE_CONSTANTS[] = {
    { "NONE", LV_CHART_TYPE_NONE },
    { "LINE", LV_CHART_TYPE_LINE },
    { "CURVE", LV_CHART_TYPE_CURVE },
    { "BAR", LV_CHART_TYPE_BAR },
    { "STACKED", LV_CHART_TYPE_STACKED },
    { "SCATTER", LV_CHART_TYPE_SCATTER },
};

const Constant CHART_AXIS_CONSTANTS[] = {
    { "PRIMARY_Y", LV_CHART_AXIS_PRIMARY_Y },
    { "SECONDARY_Y", LV_CHART_AXIS_SECONDARY_Y },
    { "PRIMARY_X", LV_CHART_AXIS_PRIMARY_X },
    { "SECONDARY_X", LV_CHART_AXIS_SECONDARY_X },
};

const Constant CHART_UPDATE_CONSTANTS[] = {
    { "SHIFT", LV_CHART_UPDATE_MODE_SHIFT },
    { "CIRCULAR", LV_CHART_UPDATE_MODE_CIRCULAR },
};

const Constant KEYBOARD_MODE_CONSTANTS[] = {
    { "TEXT_LOWER", LV_KEYBOARD_MODE_TEXT_LOWER },
    { "TEXT_UPPER", LV_KEYBOARD_MODE_TEXT_UPPER },
    { "SPECIAL", LV_KEYBOARD_MODE_SPECIAL },
    { "NUMBER", LV_KEYBOARD_MODE_NUMBER },
    { "USER_1", LV_KEYBOARD_MODE_USER_1 },
    { "USER_2", LV_KEYBOARD_MODE_USER_2 },
    { "USER_3", LV_KEYBOARD_MODE_USER_3 },
    { "USER_4", LV_KEYBOARD_MODE_USER_4 },
};

const Constant BUTTONMATRIX_CTRL_CONSTANTS[] = {
    { "NONE", LV_BUTTONMATRIX_CTRL_NONE },
    { "WIDTH_1", LV_BUTTONMATRIX_CTRL_WIDTH_1 },
    { "WIDTH_2", LV_BUTTONMATRIX_CTRL_WIDTH_2 },
    { "WIDTH_3", LV_BUTTONMATRIX_CTRL_WIDTH_3 },
    { "WIDTH_4", LV_BUTTONMATRIX_CTRL_WIDTH_4 },
    { "WIDTH_5", LV_BUTTONMATRIX_CTRL_WIDTH_5 },
    { "WIDTH_6", LV_BUTTONMATRIX_CTRL_WIDTH_6 },
    { "WIDTH_7", LV_BUTTONMATRIX_CTRL_WIDTH_7 },
    { "WIDTH_8", LV_BUTTONMATRIX_CTRL_WIDTH_8 },
    { "WIDTH_9", LV_BUTTONMATRIX_CTRL_WIDTH_9 },
    { "WIDTH_10", LV_BUTTONMATRIX_CTRL_WIDTH_10 },
    { "WIDTH_11", LV_BUTTONMATRIX_CTRL_WIDTH_11 },
    { "WIDTH_12", LV_BUTTONMATRIX_CTRL_WIDTH_12 },
    { "WIDTH_13", LV_BUTTONMATRIX_CTRL_WIDTH_13 },
    { "WIDTH_14", LV_BUTTONMATRIX_CTRL_WIDTH_14 },
    { "WIDTH_15", LV_BUTTONMATRIX_CTRL_WIDTH_15 },
    { "HIDDEN", LV_BUTTONMATRIX_CTRL_HIDDEN },
    { "NO_REPEAT", LV_BUTTONMATRIX_CTRL_NO_REPEAT },
    { "DISABLED", LV_BUTTONMATRIX_CTRL_DISABLED },
    { "CHECKABLE", LV_BUTTONMATRIX_CTRL_CHECKABLE },
    { "CHECKED", LV_BUTTONMATRIX_CTRL_CHECKED },
    { "CLICK_TRIG", LV_BUTTONMATRIX_CTRL_CLICK_TRIG },
    { "POPOVER", LV_BUTTONMATRIX_CTRL_POPOVER },
    { "RECOLOR", LV_BUTTONMATRIX_CTRL_RECOLOR },
    { "RESERVED_1", LV_BUTTONMATRIX_CTRL_RESERVED_1 },
    { "RESERVED_2", LV_BUTTONMATRIX_CTRL_RESERVED_2 },
    { "CUSTOM_1", LV_BUTTONMATRIX_CTRL_CUSTOM_1 },
    { "CUSTOM_2", LV_BUTTONMATRIX_CTRL_CUSTOM_2 },
};

} // namespace

extern "C" error_t luavgl_bindings_open(struct LuaRuntime* runtime, lv_obj_t* root) {
    auto* state = static_cast<lua_State*>(lua_runtime_get_state(runtime));

    luavgl_open_object(state);
    luavgl_set_root(state, root);

    luaL_newlib(state, lvgl_functions);
    luavgl_open_widgets(state); // the rest of the widget constructors

    add_constants(state, "ALIGN", ALIGN_CONSTANTS, sizeof(ALIGN_CONSTANTS) / sizeof(Constant));
    add_constants(state, "TEXT_ALIGN", TEXT_ALIGN_CONSTANTS, sizeof(TEXT_ALIGN_CONSTANTS) / sizeof(Constant));
    add_constants(state, "DIR", DIR_CONSTANTS, sizeof(DIR_CONSTANTS) / sizeof(Constant));
    add_constants(state, "SCROLLBAR", SCROLLBAR_MODE_CONSTANTS, sizeof(SCROLLBAR_MODE_CONSTANTS) / sizeof(Constant));
    add_constants(state, "SCROLL_SNAP", SCROLL_SNAP_CONSTANTS, sizeof(SCROLL_SNAP_CONSTANTS) / sizeof(Constant));
    add_constants(state, "BORDER_SIDE", BORDER_SIDE_CONSTANTS, sizeof(BORDER_SIDE_CONSTANTS) / sizeof(Constant));
    add_constants(state, "GRAD_DIR", GRAD_DIR_CONSTANTS, sizeof(GRAD_DIR_CONSTANTS) / sizeof(Constant));
    add_constants(state, "FLEX_FLOW", FLEX_FLOW_CONSTANTS, sizeof(FLEX_FLOW_CONSTANTS) / sizeof(Constant));
    add_constants(state, "FLEX_ALIGN", FLEX_ALIGN_CONSTANTS, sizeof(FLEX_ALIGN_CONSTANTS) / sizeof(Constant));
    add_constants(state, "LAYOUT", LAYOUT_CONSTANTS, sizeof(LAYOUT_CONSTANTS) / sizeof(Constant));
    add_constants(state, "LONG", LONG_MODE_CONSTANTS, sizeof(LONG_MODE_CONSTANTS) / sizeof(Constant));
    add_constants(state, "FLAG", FLAG_CONSTANTS, sizeof(FLAG_CONSTANTS) / sizeof(Constant));
    add_constants(state, "OPA", OPACITY_CONSTANTS, sizeof(OPACITY_CONSTANTS) / sizeof(Constant));
    add_constants(state, "PART", PART_CONSTANTS, sizeof(PART_CONSTANTS) / sizeof(Constant));
    add_constants(state, "STATE", STATE_CONSTANTS, sizeof(STATE_CONSTANTS) / sizeof(Constant));
    add_constants(state, "ICON_BUTTON", ICON_BUTTON_CONSTANTS, sizeof(ICON_BUTTON_CONSTANTS) / sizeof(Constant));
    add_constants(state, "TABS_MODE", TABS_MODE_CONSTANTS, sizeof(TABS_MODE_CONSTANTS) / sizeof(Constant));
    add_constants(state, "STATUSLINE_LEVEL", STATUSLINE_LEVEL_CONSTANTS, sizeof(STATUSLINE_LEVEL_CONSTANTS) / sizeof(Constant));
    add_constants(state, "GRID_ALIGN", GRID_ALIGN_CONSTANTS, sizeof(GRID_ALIGN_CONSTANTS) / sizeof(Constant));
    add_constants(state, "TABLE_CTRL", TABLE_CTRL_CONSTANTS, sizeof(TABLE_CTRL_CONSTANTS) / sizeof(Constant));
    add_constants(state, "SCALE_MODE", SCALE_MODE_CONSTANTS, sizeof(SCALE_MODE_CONSTANTS) / sizeof(Constant));
    add_constants(state, "IMAGEBUTTON", IMAGEBUTTON_STATE_CONSTANTS, sizeof(IMAGEBUTTON_STATE_CONSTANTS) / sizeof(Constant));
    add_constants(state, "CHART_TYPE", CHART_TYPE_CONSTANTS, sizeof(CHART_TYPE_CONSTANTS) / sizeof(Constant));
    add_constants(state, "CHART_AXIS", CHART_AXIS_CONSTANTS, sizeof(CHART_AXIS_CONSTANTS) / sizeof(Constant));
    add_constants(state, "CHART_UPDATE", CHART_UPDATE_CONSTANTS, sizeof(CHART_UPDATE_CONSTANTS) / sizeof(Constant));
    add_constants(state, "KEYBOARD_MODE", KEYBOARD_MODE_CONSTANTS, sizeof(KEYBOARD_MODE_CONSTANTS) / sizeof(Constant));
    add_constants(state, "BUTTONMATRIX_CTRL", BUTTONMATRIX_CTRL_CONSTANTS, sizeof(BUTTONMATRIX_CTRL_CONSTANTS) / sizeof(Constant));

    // Sizing helpers, which are macros in C and so need explicit exposure
    lua_pushinteger(state, LV_SIZE_CONTENT);
    lua_setfield(state, -2, "SIZE_CONTENT");

    // Grid track helper; LV_GRID_FR() is the function form below.
    lua_pushinteger(state, LV_GRID_CONTENT);
    lua_setfield(state, -2, "GRID_CONTENT");

    // Sentinel values for table cells and button matrices.
    lua_pushinteger(state, LV_TABLE_CELL_NONE);
    lua_setfield(state, -2, "TABLE_CELL_NONE");
    lua_pushinteger(state, LV_BUTTONMATRIX_BUTTON_NONE);
    lua_setfield(state, -2, "BUTTONMATRIX_BUTTON_NONE");

    // LVGL's built-in icons. These are UTF-8 strings in the symbol font, not image paths,
    // so a script needs them by name - passing an arbitrary string where an icon is
    // expected renders nothing at all.
    lua_newtable(state);
    const struct { const char* name; const char* symbol; } symbols[] = {
        { "CLOSE", LV_SYMBOL_CLOSE },
        { "OK", LV_SYMBOL_OK },
        { "LEFT", LV_SYMBOL_LEFT },
        { "RIGHT", LV_SYMBOL_RIGHT },
        { "SETTINGS", LV_SYMBOL_SETTINGS },
        { "REFRESH", LV_SYMBOL_REFRESH },
        { "SAVE", LV_SYMBOL_SAVE },
        { "TRASH", LV_SYMBOL_TRASH },
        { "HOME", LV_SYMBOL_HOME },
        { "WIFI", LV_SYMBOL_WIFI },
        { "BATTERY_FULL", LV_SYMBOL_BATTERY_FULL },
    };
    for (const auto& entry : symbols) {
        lua_pushstring(state, entry.symbol);
        lua_setfield(state, -2, entry.name);
    }
    lua_setfield(state, -2, "SYMBOL");

    // The firmware's shared Material Symbols set (the same glyphs C apps use for tools,
    // tabs and status lines). Separate from `SYMBOL`, which is LVGL's own icon font.
    static const struct { const char* name; const char* symbol; } shared_icons[] = {
#include "icons_shared.inc"
    };
    lua_newtable(state);
    for (const auto& entry : shared_icons) {
        lua_pushstring(state, entry.symbol);
        lua_setfield(state, -2, entry.name);
    }
    lua_setfield(state, -2, "ICON");

    luavgl_open_toolbar(state);
    luavgl_open_events(state);
    luavgl_open_timer(state);
    luavgl_open_notification(state);
    luavgl_open_picker(state);
    luavgl_open_group(state);
    luavgl_open_anim(state);
    luavgl_open_style(state);

    lua_setglobal(state, "lvgl");

    return ERROR_NONE;
}

extern "C" void luavgl_bindings_close(struct LuaRuntime* runtime) {
    luavgl_detach_all(static_cast<lua_State*>(lua_runtime_get_state(runtime)));
}
