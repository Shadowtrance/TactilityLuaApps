// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <tactility/drivers/spi_controller.h>
#include <tactility/error.h>

#include <cstdlib>

extern "C" {
#include <lauxlib.h>
}

namespace {

constexpr size_t MAX_TRANSFER_SIZE = 64 * 1024;

int push_error(lua_State* state, error_t error) {
    lua_pushnil(state);
    lua_pushstring(state, error_to_string(error));
    return 2;
}

struct Request {
    struct Device* controller;
    uint32_t clock_hz;
    uint8_t mode;
    int32_t cs_index;
    bool half_duplex;
    const uint8_t* tx;
    size_t length;
};

/** Adds a temporary peripheral, runs one transfer and removes it again. */
enum class TransferKind { Duplex, Read, Write };

int transfer(lua_State* state, const Request& request, TransferKind kind) {
    if (request.mode > 3) {
        return luaL_error(state, "mode must be 0, 1, 2 or 3");
    }
    if (request.clock_hz == 0) {
        return luaL_error(state, "clock_hz must be positive");
    }
    if (request.length == 0) {
        if (kind == TransferKind::Write) {
            lua_pushboolean(state, true);
        } else {
            lua_pushliteral(state, "");
        }
        return 1;
    }

    uint8_t* rx = nullptr;
    if (kind != TransferKind::Write) {
        rx = static_cast<uint8_t*>(std::malloc(request.length));
        if (rx == nullptr) {
            return luaL_error(state, "out of memory");
        }
    }

    struct SpiDeviceConfig config = {};
    config.clock_speed_hz = request.clock_hz;
    config.mode = request.mode;
    config.cs_index = request.cs_index;
    config.half_duplex = request.half_duplex;

    struct SpiDevice* spi_device = nullptr;
    error_t error = spi_controller_add_device(request.controller, &config, &spi_device);
    if (error != ERROR_NONE) {
        std::free(rx);
        return push_error(state, error);
    }

    error = spi_controller_transmit(
        request.controller,
        spi_device,
        request.tx,
        rx,
        request.length
    );
    const error_t remove_error = spi_controller_remove_device(request.controller, spi_device);
    if (error == ERROR_NONE) {
        error = remove_error;
    }
    if (error != ERROR_NONE) {
        std::free(rx);
        return push_error(state, error);
    }

    if (kind == TransferKind::Write) {
        lua_pushboolean(state, true);
    } else {
        lua_bindings_push_bytes(state, rx, request.length);
    }
    std::free(rx);
    return 1;
}

} // namespace

/** `tactility.spi.transmit(controller, clock_hz, mode, cs_index, tx, half_duplex?)` */
int lua_spi_transmit(lua_State* state) {
    size_t length = 0;
    const uint8_t* tx = lua_bindings_check_bytes(state, 5, &length);
    if (length > MAX_TRANSFER_SIZE) {
        return luaL_error(state, "transfer is larger than %d bytes", static_cast<int>(MAX_TRANSFER_SIZE));
    }
    const Request request = {
        .controller = lua_bindings_check_device(state, 1),
        .clock_hz = static_cast<uint32_t>(luaL_checkinteger(state, 2)),
        .mode = static_cast<uint8_t>(luaL_checkinteger(state, 3)),
        .cs_index = static_cast<int32_t>(luaL_checkinteger(state, 4)),
        .half_duplex = lua_toboolean(state, 6) != 0,
        .tx = tx,
        .length = length,
    };
    return transfer(state, request, TransferKind::Duplex);
}

/** `tactility.spi.read(controller, clock_hz, mode, cs_index, length, half_duplex?)` */
int lua_spi_read(lua_State* state) {
    const auto length = static_cast<size_t>(luaL_checkinteger(state, 5));
    if (length > MAX_TRANSFER_SIZE) {
        return luaL_error(state, "transfer is larger than %d bytes", static_cast<int>(MAX_TRANSFER_SIZE));
    }
    const Request request = {
        .controller = lua_bindings_check_device(state, 1),
        .clock_hz = static_cast<uint32_t>(luaL_checkinteger(state, 2)),
        .mode = static_cast<uint8_t>(luaL_checkinteger(state, 3)),
        .cs_index = static_cast<int32_t>(luaL_checkinteger(state, 4)),
        .half_duplex = lua_toboolean(state, 6) != 0,
        .tx = nullptr,
        .length = length,
    };
    return transfer(state, request, TransferKind::Read);
}

/** `tactility.spi.write(controller, clock_hz, mode, cs_index, tx, half_duplex?)` */
int lua_spi_write(lua_State* state) {
    size_t length = 0;
    const uint8_t* tx = lua_bindings_check_bytes(state, 5, &length);
    if (length > MAX_TRANSFER_SIZE) {
        return luaL_error(state, "transfer is larger than %d bytes", static_cast<int>(MAX_TRANSFER_SIZE));
    }
    const Request request = {
        .controller = lua_bindings_check_device(state, 1),
        .clock_hz = static_cast<uint32_t>(luaL_checkinteger(state, 2)),
        .mode = static_cast<uint8_t>(luaL_checkinteger(state, 3)),
        .cs_index = static_cast<int32_t>(luaL_checkinteger(state, 4)),
        .half_duplex = lua_toboolean(state, 6) != 0,
        .tx = tx,
        .length = length,
    };
    return transfer(state, request, TransferKind::Write);
}
