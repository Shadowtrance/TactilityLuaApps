// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

extern "C" {
#include <lauxlib.h>
}

/**
 * Byte buffers shared by the capabilities that transfer more than one value.
 *
 * Lua strings are binary safe, so they are the natural input buffer. Output buffers
 * cannot be Lua strings (they are immutable), so a binding allocates, fills, and pushes
 * the result as a new string.
 */

const uint8_t* lua_bindings_check_bytes(lua_State* state, int index, size_t* size) {
    return reinterpret_cast<const uint8_t*>(luaL_checklstring(state, index, size));
}

void lua_bindings_push_bytes(lua_State* state, const uint8_t* data, size_t size) {
    lua_pushlstring(state, reinterpret_cast<const char*>(data), size);
}
