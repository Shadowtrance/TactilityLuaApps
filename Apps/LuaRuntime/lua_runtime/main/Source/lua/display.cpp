// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <tactility/device.h>
#include <tactility/drivers/backlight.h>
#include <tactility/drivers/display.h>
#include <tactility/error.h>

extern "C" {
#include <lauxlib.h>
}

namespace {

/** Reference guard for a device returned by a lookup function. */
struct ScopedDevice {
    Device* device = nullptr;

    explicit ScopedDevice(Device* value) : device(value) {}

    ~ScopedDevice() {
        if (device != nullptr) {
            device_put(device);
        }
    }

    ScopedDevice(const ScopedDevice&) = delete;
    ScopedDevice& operator=(const ScopedDevice&) = delete;
};

int push_reason(lua_State* state, const char* reason) {
    lua_pushnil(state);
    lua_pushstring(state, reason);
    return 2;
}

bool find_display(Device** out_display) {
    return device_get_first_active_by_type(&DISPLAY_TYPE, out_display) == ERROR_NONE;
}

/** On success `out_backlight` gets a reference the caller must put. */
bool find_backlight(Device** out_backlight) {
    Device* display = nullptr;
    if (!find_display(&display)) {
        return false;
    }

    ScopedDevice guard(display);
    return display_get_backlight(display, out_backlight) == ERROR_NONE;
}

int display_is_available(lua_State* state) {
    Device* display = nullptr;
    const bool found = find_display(&display);
    if (found) {
        device_put(display);
    }
    lua_pushboolean(state, found ? 1 : 0);
    return 1;
}

int display_name(lua_State* state) {
    Device* display = nullptr;
    if (!find_display(&display)) {
        return push_reason(state, "no display");
    }

    ScopedDevice guard(display);
    lua_pushstring(state, display->name);
    return 1;
}

/** `display.size() -> width, height` */
int display_size(lua_State* state) {
    Device* display = nullptr;
    if (!find_display(&display)) {
        return push_reason(state, "no display");
    }

    ScopedDevice guard(display);
    lua_pushinteger(state, display_get_resolution_x(display));
    lua_pushinteger(state, display_get_resolution_y(display));
    return 2;
}

/** `display.shape() -> { shape = "rectangle"|"rounded"|"circle", radius = px }` */
int display_shape(lua_State* state) {
    Device* display = nullptr;
    if (!find_display(&display)) {
        return push_reason(state, "no display");
    }

    ScopedDevice guard(display);
    DisplayShape shape = {};
    display_get_shape(display, &shape);

    const char* name = "rectangle";
    if (shape.shape == DISPLAY_SHAPE_ROUNDED) {
        name = "rounded";
    } else if (shape.shape == DISPLAY_SHAPE_CIRCLE) {
        name = "circle";
    }

    lua_createtable(state, 0, 2);
    lua_pushstring(state, name);
    lua_setfield(state, -2, "shape");
    lua_pushinteger(state, shape.corner_radius);
    lua_setfield(state, -2, "radius");
    return 1;
}

int display_has_backlight(lua_State* state) {
    Device* backlight = nullptr;
    const bool found = find_backlight(&backlight);
    if (found) {
        device_put(backlight);
    }
    lua_pushboolean(state, found ? 1 : 0);
    return 1;
}

int display_brightness_range(lua_State* state) {
    Device* backlight = nullptr;
    if (!find_backlight(&backlight)) {
        return push_reason(state, "no backlight");
    }

    ScopedDevice guard(backlight);
    lua_pushinteger(state, backlight_get_min_brightness(backlight));
    lua_pushinteger(state, backlight_get_max_brightness(backlight));
    return 2;
}

/** Raw backlight level, for diagnostics. Most scripts want the percentage. */
int display_brightness(lua_State* state) {
    Device* backlight = nullptr;
    if (!find_backlight(&backlight)) {
        return push_reason(state, "no backlight");
    }

    ScopedDevice guard(backlight);
    uint8_t brightness = 0;
    const error_t error = backlight_get_brightness(backlight, &brightness);
    if (error != ERROR_NONE) {
        return push_reason(state, error_to_string(error));
    }

    lua_pushinteger(state, brightness);
    return 1;
}

int percent_to_brightness(Device* backlight, int percent) {
    const int32_t minimum = backlight_get_min_brightness(backlight);
    const int32_t range = backlight_get_max_brightness(backlight) - minimum;
    const int32_t brightness = minimum + (percent * range + 50) / 100;
    // A brightness of 0 turns the backlight off, so 0% is the dimmest level that is still on.
    return brightness < 1 ? 1 : brightness;
}

int brightness_to_percent(Device* backlight, int brightness) {
    // A backlight with few levels can't map a level back to an exact percentage, so use the
    // same 10% steps as the firmware's Display settings screen.
    for (int percent = 0; percent <= 100; percent += 10) {
        if (percent_to_brightness(backlight, percent) == brightness) {
            return percent;
        }
    }

    const int32_t minimum = backlight_get_min_brightness(backlight);
    const int32_t range = backlight_get_max_brightness(backlight) - minimum;
    int32_t clamped = brightness;
    if (clamped < minimum) {
        clamped = minimum;
    } else if (clamped > minimum + range) {
        clamped = minimum + range;
    }
    return range > 0 ? static_cast<int>(((clamped - minimum) * 100 + range / 2) / range) : 100;
}

/** `display.brightness_percent() -> 0..100` */
int display_brightness_percent(lua_State* state) {
    Device* backlight = nullptr;
    if (!find_backlight(&backlight)) {
        return push_reason(state, "no backlight");
    }

    ScopedDevice guard(backlight);
    uint8_t brightness = 0;
    const error_t error = backlight_get_brightness(backlight, &brightness);
    if (error != ERROR_NONE) {
        return push_reason(state, error_to_string(error));
    }

    lua_pushinteger(state, brightness_to_percent(backlight, brightness));
    return 1;
}

/** `display.set_brightness_percent(percent)` - 0 is the dimmest level that stays on. */
int display_set_brightness_percent(lua_State* state) {
    const auto percent = luaL_checkinteger(state, 1);
    if (percent < 0 || percent > 100) {
        return luaL_error(state, "brightness percent must be between 0 and 100");
    }

    Device* backlight = nullptr;
    if (!find_backlight(&backlight)) {
        return push_reason(state, "no backlight");
    }

    ScopedDevice guard(backlight);
    const error_t error = backlight_set_brightness(
        backlight,
        static_cast<uint8_t>(percent_to_brightness(backlight, static_cast<int>(percent)))
    );
    if (error != ERROR_NONE) {
        return push_reason(state, error_to_string(error));
    }

    lua_pushboolean(state, 1);
    return 1;
}

int display_set_brightness_default(lua_State* state) {
    Device* backlight = nullptr;
    if (!find_backlight(&backlight)) {
        return push_reason(state, "no backlight");
    }

    ScopedDevice guard(backlight);
    const error_t error = backlight_set_brightness_default(backlight);
    if (error != ERROR_NONE) {
        return push_reason(state, error_to_string(error));
    }

    lua_pushboolean(state, 1);
    return 1;
}

const luaL_Reg display_functions[] = {
    { "is_available", display_is_available },
    { "name", display_name },
    { "size", display_size },
    { "shape", display_shape },
    { "has_backlight", display_has_backlight },
    { "brightness", display_brightness },
    { "brightness_range", display_brightness_range },
    { "brightness_percent", display_brightness_percent },
    { "set_brightness_percent", display_set_brightness_percent },
    { "set_brightness_default", display_set_brightness_default },
    { nullptr, nullptr }
};

} // namespace

void lua_bindings_open_display(lua_State* state) {
    luaL_newlib(state, display_functions);
    lua_setfield(state, -2, "display"); // tactility.display
}
