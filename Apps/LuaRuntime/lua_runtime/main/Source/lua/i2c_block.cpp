// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <tactility/drivers/i2c_controller.h>
#include <tactility/error.h>

#include <vector>

extern "C" {
#include <lauxlib.h>
}

/**
 * I2C block transfers.
 *
 * Register access covers sensors, but displays, EEPROMs and audio codecs transfer byte
 * blocks. The generator has no buffer type yet, so these three functions are hand-written
 * and registered through the IDL's `impl = "custom"` hook. They share the same handle,
 * address and timeout conventions as the generated register functions.
 */

namespace {

constexpr size_t MAX_BLOCK_SIZE = 4096;

TickType_t get_timeout(lua_State* state, int index) {
    return static_cast<TickType_t>(luaL_optinteger(state, index, 100));
}

uint8_t get_address(lua_State* state, int index) {
    const lua_Integer address = luaL_checkinteger(state, index);
    if (address < 0 || address > 0x7F) {
        luaL_error(state, "address must be a 7-bit I2C address");
    }
    return static_cast<uint8_t>(address);
}

size_t get_size(lua_State* state, int index) {
    const lua_Integer size = luaL_checkinteger(state, index);
    if (size <= 0 || static_cast<size_t>(size) > MAX_BLOCK_SIZE) {
        luaL_error(state, "size must be between 1 and %d", static_cast<int>(MAX_BLOCK_SIZE));
    }
    return static_cast<size_t>(size);
}

void push_error(lua_State* state, error_t error) {
    lua_pushnil(state);
    lua_pushstring(state, error_to_string(error));
}

} // namespace

/** `tactility.i2c.read(controller, address, size [, timeout]) -> string` */
int lua_i2c_read(lua_State* state) {
    auto* device = lua_bindings_check_device(state, 1);
    const uint8_t address = get_address(state, 2);
    const size_t size = get_size(state, 3);
    const TickType_t timeout = get_timeout(state, 4);

    std::vector<uint8_t> data(size);
    const error_t error = i2c_controller_read(device, address, data.data(), size, timeout);
    if (error != ERROR_NONE) {
        push_error(state, error);
        return 2;
    }

    lua_bindings_push_bytes(state, data.data(), size);
    return 1;
}

/** `tactility.i2c.write(controller, address, data [, timeout]) -> true` */
int lua_i2c_write(lua_State* state) {
    auto* device = lua_bindings_check_device(state, 1);
    const uint8_t address = get_address(state, 2);

    size_t size = 0;
    const uint8_t* data = lua_bindings_check_bytes(state, 3, &size);
    if (size == 0 || size > MAX_BLOCK_SIZE) {
        return luaL_error(state, "data must be between 1 and %d bytes", static_cast<int>(MAX_BLOCK_SIZE));
    }

    const TickType_t timeout = get_timeout(state, 4);
    const error_t error = i2c_controller_write(device, address, data, size, timeout);
    if (error != ERROR_NONE) {
        push_error(state, error);
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

/**
 * `tactility.i2c.write_read(controller, address, write_data, read_size [, timeout]) -> string`
 *
 * The register index and the returned value are one transfer, which is what most devices
 * expect.
 */
int lua_i2c_write_read(lua_State* state) {
    auto* device = lua_bindings_check_device(state, 1);
    const uint8_t address = get_address(state, 2);

    size_t write_size = 0;
    const uint8_t* write_data = lua_bindings_check_bytes(state, 3, &write_size);
    if (write_size == 0 || write_size > MAX_BLOCK_SIZE) {
        return luaL_error(state, "write_data must be between 1 and %d bytes", static_cast<int>(MAX_BLOCK_SIZE));
    }

    const size_t read_size = get_size(state, 4);
    const TickType_t timeout = get_timeout(state, 5);

    std::vector<uint8_t> read_data(read_size);
    const error_t error = i2c_controller_write_read(
        device,
        address,
        write_data,
        write_size,
        read_data.data(),
        read_size,
        timeout
    );
    if (error != ERROR_NONE) {
        push_error(state, error);
        return 2;
    }

    lua_bindings_push_bytes(state, read_data.data(), read_size);
    return 1;
}
