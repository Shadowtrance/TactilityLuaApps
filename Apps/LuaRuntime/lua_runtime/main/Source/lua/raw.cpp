// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <tactility/module.h>

extern "C" {
#include <lauxlib.h>
}

/*
 * Low-level escape hatch for exported SDK symbols that the capability IDL has
 * not covered yet.
 *
 * Arguments are marshalled as pointer-sized integers: integers, booleans,
 * strings and handles (Device, generated handles). The return value is pushed
 * as an integer, so an `error_t` result is the raw code. This is deliberately
 * untyped: the same trust model as an ELF app, and it can crash the device.
 * The typed signature database can wrap this later without changing the ABI.
 */

namespace {

uintptr_t to_argument(lua_State* state, int index) {
    switch (lua_type(state, index)) {
        case LUA_TNIL:
        case LUA_TNONE:
            return 0;
        case LUA_TBOOLEAN:
            return lua_toboolean(state, index) != 0 ? 1 : 0;
        case LUA_TNUMBER:
            return static_cast<uintptr_t>(lua_tointeger(state, index));
        case LUA_TSTRING:
            return reinterpret_cast<uintptr_t>(lua_tostring(state, index));
        case LUA_TLIGHTUSERDATA:
            return reinterpret_cast<uintptr_t>(lua_touserdata(state, index));
        case LUA_TUSERDATA: {
            void* pointer = lua_bindings_raw_pointer(state, index);
            if (pointer == nullptr) {
                luaL_error(state, "unsupported userdata argument");
            }
            return reinterpret_cast<uintptr_t>(pointer);
        }
        default:
            return static_cast<uintptr_t>(luaL_error(state, "unsupported argument type"));
    }
}

uintptr_t invoke(uintptr_t address, const uintptr_t* arguments, int count) {
    switch (count) {
        case 0:
            return reinterpret_cast<uintptr_t (*)()>(address)();
        case 1:
            return reinterpret_cast<uintptr_t (*)(uintptr_t)>(address)(arguments[0]);
        case 2:
            return reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(address)(arguments[0], arguments[1]);
        case 3:
            return reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t)>(address)(
                arguments[0], arguments[1], arguments[2]
            );
        case 4:
            return reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(address)(
                arguments[0], arguments[1], arguments[2], arguments[3]
            );
        case 5:
            return reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(address)(
                arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]
            );
        case 6:
            return reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(address)(
                arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5]
            );
        default:
            return 0;
    }
}

int raw_call(lua_State* state) {
    const char* name = luaL_checkstring(state, 1);

    uintptr_t address = 0;
    if (!module_resolve_symbol_global(name, &address) || address == 0) {
        lua_pushnil(state);
        lua_pushfstring(state, "unknown symbol: %s", name);
        return 2;
    }

    const int count = lua_gettop(state) - 1;
    if (count > 6) {
        return luaL_error(state, "tactility.raw.call accepts at most 6 arguments");
    }

    uintptr_t arguments[6] = { 0 };
    for (int i = 0; i < count; i++) {
        arguments[i] = to_argument(state, i + 2);
    }

    lua_pushinteger(state, static_cast<lua_Integer>(invoke(address, arguments, count)));
    return 1;
}

} // namespace

void lua_bindings_open_raw(lua_State* state) {
    lua_newtable(state);

    lua_pushcfunction(state, raw_call);
    lua_setfield(state, -2, "call");

    lua_pushinteger(state, 1);
    lua_setfield(state, -2, "api");

    lua_setfield(state, -2, "raw");
}
