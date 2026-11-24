// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <tactility/drivers/uart_controller.h>
#include <tactility/error.h>

#include <cstdlib>
#include <cstring>

extern "C" {
#include <lauxlib.h>
}

namespace {

int push_error(lua_State* state, error_t error) {
    lua_pushnil(state);
    lua_pushstring(state, error_to_string(error));
    return 2;
}

/** get_config() reports ERROR_RESOURCE when the driver has no stored config. */
int push_config_error(lua_State* state, error_t error) {
    if (error == ERROR_RESOURCE) {
        lua_pushnil(state);
        lua_pushliteral(state, "the controller has no configuration yet");
        return 2;
    }
    return push_error(state, error);
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

const char* parity_name(enum UartParity parity) {
    switch (parity) {
        case UART_CONTROLLER_PARITY_EVEN: return "even";
        case UART_CONTROLLER_PARITY_ODD: return "odd";
        default: return "none";
    }
}

bool parse_parity(const char* name, enum UartParity* out) {
    if (strcmp(name, "none") == 0) {
        *out = UART_CONTROLLER_PARITY_DISABLE;
    } else if (strcmp(name, "even") == 0) {
        *out = UART_CONTROLLER_PARITY_EVEN;
    } else if (strcmp(name, "odd") == 0) {
        *out = UART_CONTROLLER_PARITY_ODD;
    } else {
        return false;
    }
    return true;
}

/** The enum values are offsets from 5 bits and 1 stop bit. */
int data_bits_value(enum UartDataBits bits) {
    return 5 + static_cast<int>(bits);
}

lua_Number stop_bits_value(enum UartStopBits bits) {
    switch (bits) {
        case UART_CONTROLLER_STOP_BITS_1_5: return 1.5;
        case UART_CONTROLLER_STOP_BITS_2: return 2.0;
        default: return 1.0;
    }
}

/** `tactility.uart.config(controller)` */
int lua_uart_config(lua_State* state) {
    auto* controller = lua_bindings_check_device(state, 1);
    struct UartConfig config = {};
    const error_t error = uart_controller_get_config(controller, &config);
    if (error != ERROR_NONE) {
        return push_config_error(state, error);
    }

    lua_createtable(state, 0, 4);

    lua_pushinteger(state, static_cast<lua_Integer>(config.baud_rate));
    lua_setfield(state, -2, "baud_rate");

    lua_pushinteger(state, data_bits_value(config.data_bits));
    lua_setfield(state, -2, "data_bits");

    lua_pushstring(state, parity_name(config.parity));
    lua_setfield(state, -2, "parity");

    lua_pushnumber(state, stop_bits_value(config.stop_bits));
    lua_setfield(state, -2, "stop_bits");

    return 1;
}

/** `tactility.uart.set_config(controller, { baud_rate?, data_bits?, parity?, stop_bits? })` */
int lua_uart_set_config(lua_State* state) {
    auto* controller = lua_bindings_check_device(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);

    // A field the script leaves out is taken from the controller's current
    // configuration. When the controller has none yet, the script has to pass
    // all four fields (see the get_config() fallback below).
    struct UartConfig config = {};
    bool has_baud_rate = false;
    bool has_data_bits = false;
    bool has_parity = false;
    bool has_stop_bits = false;

    lua_getfield(state, 2, "baud_rate");
    if (!lua_isnil(state, -1)) {
        const lua_Integer baud_rate = luaL_checkinteger(state, -1);
        if (baud_rate <= 0) {
            return luaL_error(state, "baud_rate must be positive");
        }
        config.baud_rate = static_cast<uint32_t>(baud_rate);
        has_baud_rate = true;
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "data_bits");
    if (!lua_isnil(state, -1)) {
        const lua_Integer data_bits = luaL_checkinteger(state, -1);
        if (data_bits < 5 || data_bits > 8) {
            return luaL_error(state, "data_bits must be 5, 6, 7 or 8");
        }
        config.data_bits = static_cast<enum UartDataBits>(data_bits - 5);
        has_data_bits = true;
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "parity");
    if (!lua_isnil(state, -1)) {
        if (!parse_parity(luaL_checkstring(state, -1), &config.parity)) {
            return luaL_error(state, "parity must be \"none\", \"even\" or \"odd\"");
        }
        has_parity = true;
    }
    lua_pop(state, 1);

    lua_getfield(state, 2, "stop_bits");
    if (!lua_isnil(state, -1)) {
        const lua_Number stop_bits = luaL_checknumber(state, -1);
        if (stop_bits == 1.0) {
            config.stop_bits = UART_CONTROLLER_STOP_BITS_1;
        } else if (stop_bits == 1.5) {
            config.stop_bits = UART_CONTROLLER_STOP_BITS_1_5;
        } else if (stop_bits == 2.0) {
            config.stop_bits = UART_CONTROLLER_STOP_BITS_2;
        } else {
            return luaL_error(state, "stop_bits must be 1, 1.5 or 2");
        }
        has_stop_bits = true;
    }
    lua_pop(state, 1);

    if (!has_baud_rate || !has_data_bits || !has_parity || !has_stop_bits) {
        struct UartConfig current = {};
        const error_t error = uart_controller_get_config(controller, &current);
        if (error != ERROR_NONE) {
            return push_config_error(state, error);
        }
        if (!has_baud_rate) config.baud_rate = current.baud_rate;
        if (!has_data_bits) config.data_bits = current.data_bits;
        if (!has_parity) config.parity = current.parity;
        if (!has_stop_bits) config.stop_bits = current.stop_bits;
    }

    const error_t error = uart_controller_set_config(controller, &config);
    if (error != ERROR_NONE) {
        return push_error(state, error);
    }

    lua_pushboolean(state, true);
    return 1;
}
