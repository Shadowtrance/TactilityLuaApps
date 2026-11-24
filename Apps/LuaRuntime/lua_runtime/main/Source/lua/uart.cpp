// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <tactility/drivers/uart_controller.h>
#include <tactility/error.h>

#include <cstdlib>

extern "C" {
#include <lauxlib.h>
}

namespace {

int push_error(lua_State* state, error_t error) {
    lua_pushnil(state);
    lua_pushstring(state, error_to_string(error));
    return 2;
}

constexpr size_t MAX_BLOCK_SIZE = 64 * 1024;

} // namespace

/** `tactility.uart.is_open(controller)` */
int lua_uart_is_open(lua_State* state) {
    lua_pushboolean(state, uart_controller_is_open(lua_bindings_check_device(state, 1)));
    return 1;
}

/** `tactility.uart.available(controller)` */
int lua_uart_available(lua_State* state) {
    auto* controller = lua_bindings_check_device(state, 1);
    size_t available = 0;
    const error_t error = uart_controller_get_available(controller, &available);
    if (error != ERROR_NONE) {
        return push_error(state, error);
    }
    lua_pushinteger(state, static_cast<lua_Integer>(available));
    return 1;
}

/** `tactility.uart.read_bytes(controller, size, timeout?)` */
int lua_uart_read_bytes(lua_State* state) {
    auto* controller = lua_bindings_check_device(state, 1);
    const auto size = static_cast<size_t>(luaL_checkinteger(state, 2));
    const auto timeout = static_cast<TickType_t>(luaL_optinteger(state, 3, 100));
    if (size == 0 || size > MAX_BLOCK_SIZE) {
        return luaL_error(state, "size must be between 1 and %d", static_cast<int>(MAX_BLOCK_SIZE));
    }

    auto* buffer = static_cast<uint8_t*>(std::malloc(size));
    if (buffer == nullptr) {
        return luaL_error(state, "out of memory");
    }

    const error_t error = uart_controller_read_bytes(controller, buffer, size, timeout);
    if (error != ERROR_NONE) {
        std::free(buffer);
        return push_error(state, error);
    }

    lua_bindings_push_bytes(state, buffer, size);
    std::free(buffer);
    return 1;
}

/** `tactility.uart.write_bytes(controller, data, timeout?)` */
int lua_uart_write_bytes(lua_State* state) {
    auto* controller = lua_bindings_check_device(state, 1);
    size_t size = 0;
    const uint8_t* data = lua_bindings_check_bytes(state, 2, &size);
    const auto timeout = static_cast<TickType_t>(luaL_optinteger(state, 3, 100));

    if (size == 0) {
        lua_pushboolean(state, true);
        return 1;
    }

    const error_t error = uart_controller_write_bytes(controller, data, size, timeout);
    if (error != ERROR_NONE) {
        return push_error(state, error);
    }

    lua_pushboolean(state, true);
    return 1;
}

/** `tactility.uart.read_until(controller, terminator, size, timeout?)` */
int lua_uart_read_until(lua_State* state) {
    auto* controller = lua_bindings_check_device(state, 1);
    const auto terminator = static_cast<uint8_t>(luaL_checkinteger(state, 2));
    const auto size = static_cast<size_t>(luaL_checkinteger(state, 3));
    const auto timeout = static_cast<TickType_t>(luaL_optinteger(state, 4, 100));
    if (size == 0 || size > MAX_BLOCK_SIZE) {
        return luaL_error(state, "size must be between 1 and %d", static_cast<int>(MAX_BLOCK_SIZE));
    }

    auto* buffer = static_cast<uint8_t*>(std::malloc(size));
    if (buffer == nullptr) {
        return luaL_error(state, "out of memory");
    }

    size_t read = 0;
    const error_t error = uart_controller_read_until(
        controller,
        buffer,
        size,
        terminator,
        true,
        &read,
        timeout
    );
    if (error != ERROR_NONE) {
        std::free(buffer);
        return push_error(state, error);
    }

    lua_bindings_push_bytes(state, buffer, read);
    std::free(buffer);
    return 1;
}
