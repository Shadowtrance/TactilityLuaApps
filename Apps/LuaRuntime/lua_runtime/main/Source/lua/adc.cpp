// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <tactility/drivers/adc_controller.h>
#include <tactility/error.h>

extern "C" {
#include <lauxlib.h>
}

namespace {

int push_error(lua_State* state, error_t error) {
    lua_pushnil(state);
    lua_pushstring(state, error_to_string(error));
    return 2;
}

} // namespace

/**
 * `tactility.adc.read_raw(controller, channel, timeout?)`
 *
 * Custom rather than generated: the kernel takes `int*`, while the IDL's `i32`
 * maps to `int32_t*`, which is a different type on Xtensa.
 */
int lua_adc_read_raw(lua_State* state) {
    auto* controller = lua_bindings_check_device(state, 1);
    const auto channel = static_cast<uint8_t>(luaL_checkinteger(state, 2));
    const auto timeout = static_cast<TickType_t>(luaL_optinteger(state, 3, 100));

    int raw = 0;
    const error_t error = adc_controller_read_raw(controller, channel, &raw, timeout);
    if (error != ERROR_NONE) {
        return push_error(state, error);
    }

    lua_pushinteger(state, static_cast<lua_Integer>(raw));
    return 1;
}
