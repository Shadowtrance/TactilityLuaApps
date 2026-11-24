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
constexpr auto* HANDLE_METATABLE = "tactility.SpiDevice";

int push_error(lua_State* state, error_t error) {
    lua_pushnil(state);
    lua_pushstring(state, error_to_string(error));
    return 2;
}

enum class TransferKind { Duplex, Read, Write };

struct Request {
    struct Device* controller;
    uint32_t clock_hz;
    uint8_t mode;
    int32_t cs_index;
    bool half_duplex;
    const uint8_t* tx;
    size_t length;
};

/** A persistent peripheral: one SpiDevice reused by every transfer. */
struct SpiHandle {
    struct Device* controller;
    struct SpiDevice* device;
};

int push_empty(lua_State* state, TransferKind kind) {
    if (kind == TransferKind::Write) {
        lua_pushboolean(state, true);
    } else {
        lua_pushliteral(state, "");
    }
    return 1;
}

/** Runs one transfer on an already-added device. */
int run_transfer(
    lua_State* state,
    struct Device* controller,
    struct SpiDevice* device,
    TransferKind kind,
    const uint8_t* tx,
    size_t length
) {
    if (length == 0) {
        return push_empty(state, kind);
    }

    uint8_t* rx = nullptr;
    if (kind != TransferKind::Write) {
        rx = static_cast<uint8_t*>(std::malloc(length));
        if (rx == nullptr) {
            return luaL_error(state, "out of memory");
        }
    }

    const error_t error = spi_controller_transmit(controller, device, tx, rx, length);
    if (error != ERROR_NONE) {
        std::free(rx);
        return push_error(state, error);
    }

    if (kind == TransferKind::Write) {
        lua_pushboolean(state, true);
    } else {
        lua_bindings_push_bytes(state, rx, length);
    }
    std::free(rx);
    return 1;
}

void validate_request(lua_State* state, const Request& request) {
    if (request.mode > 3) {
        luaL_error(state, "mode must be 0, 1, 2 or 3");
    }
    if (request.clock_hz == 0) {
        luaL_error(state, "clock_hz must be positive");
    }
}

/** Adds a temporary peripheral, runs one transfer and removes it again. */
int transfer(lua_State* state, const Request& request, TransferKind kind) {
    validate_request(state, request);
    if (request.length == 0) {
        return push_empty(state, kind);
    }

    struct SpiDeviceConfig config = {};
    config.clock_speed_hz = request.clock_hz;
    config.mode = request.mode;
    config.cs_index = request.cs_index;
    config.half_duplex = request.half_duplex;

    struct SpiDevice* spi_device = nullptr;
    error_t error = spi_controller_add_device(request.controller, &config, &spi_device);
    if (error != ERROR_NONE) {
        return push_error(state, error);
    }

    uint8_t* rx = nullptr;
    if (kind != TransferKind::Write) {
        rx = static_cast<uint8_t*>(std::malloc(request.length));
        if (rx == nullptr) {
            spi_controller_remove_device(request.controller, spi_device);
            return luaL_error(state, "out of memory");
        }
    }

    error = spi_controller_transmit(request.controller, spi_device, request.tx, rx, request.length);
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

SpiHandle* check_handle(lua_State* state, int index) {
    auto* handle = static_cast<SpiHandle*>(luaL_checkudata(state, index, HANDLE_METATABLE));
    if (handle->device == nullptr) {
        luaL_error(state, "spi device is closed");
    }
    return handle;
}

int handle_transmit(lua_State* state) {
    auto* handle = check_handle(state, 1);
    size_t length = 0;
    const uint8_t* tx = lua_bindings_check_bytes(state, 2, &length);
    if (length > MAX_TRANSFER_SIZE) {
        return luaL_error(state, "transfer is larger than %d bytes", static_cast<int>(MAX_TRANSFER_SIZE));
    }
    return run_transfer(state, handle->controller, handle->device, TransferKind::Duplex, tx, length);
}

int handle_read(lua_State* state) {
    auto* handle = check_handle(state, 1);
    const auto length = static_cast<size_t>(luaL_checkinteger(state, 2));
    if (length > MAX_TRANSFER_SIZE) {
        return luaL_error(state, "transfer is larger than %d bytes", static_cast<int>(MAX_TRANSFER_SIZE));
    }
    return run_transfer(state, handle->controller, handle->device, TransferKind::Read, nullptr, length);
}

int handle_write(lua_State* state) {
    auto* handle = check_handle(state, 1);
    size_t length = 0;
    const uint8_t* tx = lua_bindings_check_bytes(state, 2, &length);
    if (length > MAX_TRANSFER_SIZE) {
        return luaL_error(state, "transfer is larger than %d bytes", static_cast<int>(MAX_TRANSFER_SIZE));
    }
    return run_transfer(state, handle->controller, handle->device, TransferKind::Write, tx, length);
}

int handle_close(lua_State* state) {
    auto* handle = static_cast<SpiHandle*>(luaL_checkudata(state, 1, HANDLE_METATABLE));
    if (handle->device == nullptr) {
        lua_pushboolean(state, true);
        return 1;
    }

    const error_t error = spi_controller_remove_device(handle->controller, handle->device);
    if (error != ERROR_NONE) {
        return push_error(state, error);
    }

    handle->device = nullptr;
    lua_pushboolean(state, true);
    return 1;
}

int handle_is_open(lua_State* state) {
    auto* handle = static_cast<SpiHandle*>(luaL_checkudata(state, 1, HANDLE_METATABLE));
    lua_pushboolean(state, handle->device != nullptr);
    return 1;
}

int handle_gc(lua_State* state) {
    auto* handle = static_cast<SpiHandle*>(luaL_checkudata(state, 1, HANDLE_METATABLE));
    if (handle->device != nullptr) {
        spi_controller_remove_device(handle->controller, handle->device);
        handle->device = nullptr;
    }
    return 0;
}

const luaL_Reg handle_methods[] = {
    { "transmit", handle_transmit },
    { "read", handle_read },
    { "write", handle_write },
    { "close", handle_close },
    { "is_open", handle_is_open },
    { nullptr, nullptr }
};

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

/** `tactility.spi.open(controller, clock_hz, mode, cs_index, half_duplex?)` */
int lua_spi_open(lua_State* state) {
    struct Device* controller = lua_bindings_check_device(state, 1);
    const auto clock_hz = static_cast<uint32_t>(luaL_checkinteger(state, 2));
    const auto mode = static_cast<uint8_t>(luaL_checkinteger(state, 3));
    const auto cs_index = static_cast<int32_t>(luaL_checkinteger(state, 4));
    const bool half_duplex = lua_toboolean(state, 5) != 0;

    if (mode > 3) {
        return luaL_error(state, "mode must be 0, 1, 2 or 3");
    }
    if (clock_hz == 0) {
        return luaL_error(state, "clock_hz must be positive");
    }

    // Created before add_device(), so an add failure cannot leak the SpiDevice.
    auto* handle = static_cast<SpiHandle*>(lua_newuserdatauv(state, sizeof(SpiHandle), 0));
    handle->controller = controller;
    handle->device = nullptr;
    luaL_setmetatable(state, HANDLE_METATABLE);

    struct SpiDeviceConfig config = {};
    config.clock_speed_hz = clock_hz;
    config.mode = mode;
    config.cs_index = cs_index;
    config.half_duplex = half_duplex;

    struct SpiDevice* device = nullptr;
    const error_t error = spi_controller_add_device(controller, &config, &device);
    if (error != ERROR_NONE) {
        return push_error(state, error);
    }

    handle->device = device;
    return 1;
}

void lua_bindings_open_spi_handle(lua_State* state) {
    luaL_newmetatable(state, HANDLE_METATABLE);

    luaL_newlib(state, handle_methods);
    lua_setfield(state, -2, "__index");

    lua_pushcfunction(state, handle_gc);
    lua_setfield(state, -2, "__gc");

    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");

    lua_pop(state, 1);
}
