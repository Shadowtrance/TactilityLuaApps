// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <cJSON.h>

#include <cstring>

extern "C" {
#include <lauxlib.h>
}

/**
 * `tactility.json`
 *
 * Lua tables in, Lua tables out, on top of the cJSON module. Objects become
 * string-keyed tables and arrays become 1-based sequences. JSON `null` is a
 * sentinel (`tactility.json.null`) rather than nil, so array slots and object
 * fields keep their position and existence.
 *
 * Limits are deliberate: a device should reject a hostile or accidentally huge
 * payload with an error, not exhaust PSRAM inside cJSON. The depth limit also
 * bounds the recursive Lua stack use.
 */

namespace {

constexpr size_t MAX_INPUT_BYTES = 512 * 1024;
constexpr size_t MAX_OUTPUT_BYTES = 512 * 1024;
constexpr int MAX_DEPTH = 64;

/** A unique light userdata value that means JSON null. Also exposed as json.null. */
char NULL_SENTINEL = 0;

int push_failure(lua_State* state, const char* message) {
    lua_pushnil(state);
    lua_pushstring(state, message);
    return 2;
}

void push_null(lua_State* state) {
    lua_pushlightuserdata(state, &NULL_SENTINEL);
}

bool is_null(lua_State* state, int index) {
    return lua_type(state, index) == LUA_TLIGHTUSERDATA &&
           lua_touserdata(state, index) == &NULL_SENTINEL;
}

/** Recursively pushes one parsed cJSON node. Returns false on depth overflow. */
bool push_node(lua_State* state, const cJSON* node, int depth) {
    if (depth > MAX_DEPTH) {
        return false;
    }

    if (node == nullptr || cJSON_IsNull(node)) {
        push_null(state);
        return true;
    }

    if (cJSON_IsBool(node)) {
        lua_pushboolean(state, cJSON_IsTrue(node) ? 1 : 0);
        return true;
    }

    if (cJSON_IsNumber(node)) {
        const double value = node->valuedouble;
        if (value >= -9007199254740992.0 && value <= 9007199254740992.0 &&
            value == static_cast<double>(static_cast<long long>(value))) {
            lua_pushinteger(state, static_cast<lua_Integer>(value));
        } else {
            lua_pushnumber(state, static_cast<lua_Number>(value));
        }
        return true;
    }

    if (cJSON_IsString(node)) {
        lua_pushstring(state, node->valuestring != nullptr ? node->valuestring : "");
        return true;
    }

    if (cJSON_IsArray(node)) {
        const int count = cJSON_GetArraySize(node);
        lua_createtable(state, count, 0);
        int index = 1;
        for (const cJSON* child = node->child; child != nullptr; child = child->next) {
            if (!push_node(state, child, depth + 1)) {
                lua_pop(state, 1);
                return false;
            }
            lua_seti(state, -2, index++);
        }
        return true;
    }

    if (cJSON_IsObject(node)) {
        lua_createtable(state, 0, cJSON_GetArraySize(node));
        for (const cJSON* child = node->child; child != nullptr; child = child->next) {
            if (child->string == nullptr) {
                continue;
            }
            if (!push_node(state, child, depth + 1)) {
                lua_pop(state, 1);
                return false;
            }
            lua_setfield(state, -2, child->string);
        }
        return true;
    }

    // Raw/unknown node types: expose the raw text rather than dropping it.
    if (node->valuestring != nullptr) {
        lua_pushstring(state, node->valuestring);
    } else {
        push_null(state);
    }
    return true;
}

/**
 * `tactility.json.decode(text) -> table | nil, reason`
 *
 * Parses JSON into Lua tables. Arrays are 1-based and JSON null is
 * `tactility.json.null`, so array slots keep their position.
 */
int json_decode(lua_State* state) {
    size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    if (length > MAX_INPUT_BYTES) {
        return push_failure(state, "JSON input is too large");
    }

    cJSON* root = cJSON_ParseWithLength(text, length);
    if (root == nullptr) {
        const char* error = cJSON_GetErrorPtr();
        if (error != nullptr && error >= text && static_cast<size_t>(error - text) <= length) {
            lua_pushnil(state);
            lua_pushfstring(state, "invalid JSON near byte %d", static_cast<int>(error - text));
            return 2;
        }
        return push_failure(state, "invalid JSON");
    }

    if (!lua_checkstack(state, MAX_DEPTH + 8)) {
        cJSON_Delete(root);
        return push_failure(state, "JSON is nested too deeply");
    }

    if (!push_node(state, root, 0)) {
        cJSON_Delete(root);
        return push_failure(state, "JSON is nested too deeply");
    }

    cJSON_Delete(root);
    return 1;
}

bool is_array_table(lua_State* state, int index, lua_Integer* out_count) {
    const int table_index = lua_absindex(state, index);
    const lua_Integer length = luaL_len(state, table_index);
    if (length > 0) {
        for (lua_Integer i = 1; i <= length; i++) {
            lua_geti(state, table_index, i);
            const bool missing = lua_isnil(state, -1);
            lua_pop(state, 1);
            if (missing) {
                return false;
            }
        }
    }

    lua_pushnil(state);
    while (lua_next(state, table_index) != 0) {
        lua_pop(state, 1); // value
        if (lua_type(state, -1) != LUA_TNUMBER) {
            lua_pop(state, 1); // key
            return false;
        }
        const lua_Number key = lua_tonumber(state, -1);
        if (key != static_cast<lua_Number>(static_cast<lua_Integer>(key)) ||
            key < 1 || key > static_cast<lua_Number>(length)) {
            lua_pop(state, 1);
            return false;
        }
        // Keep the key: lua_next needs it to find the following entry.
    }

    *out_count = length;
    return true;
}

cJSON* create_node(lua_State* state, int index, int depth, const char** error);

cJSON* create_table(lua_State* state, int index, int depth, const char** error) {
    const int table_index = lua_absindex(state, index);
    lua_Integer count = 0;
    if (is_array_table(state, table_index, &count)) {
        cJSON* array = cJSON_CreateArray();
        if (array == nullptr) {
            *error = "out of memory";
            return nullptr;
        }
        for (lua_Integer i = 1; i <= count; i++) {
            lua_geti(state, table_index, i);
            cJSON* child = create_node(state, -1, depth + 1, error);
            lua_pop(state, 1);
            if (child == nullptr) {
                cJSON_Delete(array);
                return nullptr;
            }
            cJSON_AddItemToArray(array, child);
        }
        return array;
    }

    cJSON* object = cJSON_CreateObject();
    if (object == nullptr) {
        *error = "out of memory";
        return nullptr;
    }

    lua_pushnil(state);
    while (lua_next(state, table_index) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            cJSON_Delete(object);
            *error = "object keys must be strings";
            return nullptr;
        }
        const char* key = lua_tostring(state, -2);
        cJSON* child = create_node(state, -1, depth + 1, error);
        if (child == nullptr) {
            lua_pop(state, 2);
            cJSON_Delete(object);
            return nullptr;
        }
        cJSON_AddItemToObject(object, key, child);
        lua_pop(state, 1); // value, keep key for lua_next
    }
    return object;
}

cJSON* create_node(lua_State* state, int index, int depth, const char** error) {
    if (depth > MAX_DEPTH) {
        *error = "value is nested too deeply";
        return nullptr;
    }

    switch (lua_type(state, index)) {
        case LUA_TNIL:
            return cJSON_CreateNull();
        case LUA_TBOOLEAN:
            return lua_toboolean(state, index) ? cJSON_CreateTrue() : cJSON_CreateFalse();
        case LUA_TNUMBER:
            return cJSON_CreateNumber(static_cast<double>(lua_tonumber(state, index)));
        case LUA_TSTRING: {
            size_t length = 0;
            const char* text = lua_tolstring(state, index, &length);
            if (std::memchr(text, '\0', length) != nullptr) {
                *error = "strings with NUL bytes cannot be encoded as JSON";
                return nullptr;
            }
            return cJSON_CreateString(text);
        }
        case LUA_TLIGHTUSERDATA:
            if (is_null(state, index)) {
                return cJSON_CreateNull();
            }
            break;
        case LUA_TTABLE:
            return create_table(state, index, depth, error);
        default:
            break;
    }

    *error = "only nil, booleans, numbers, strings, json.null and tables can be encoded";
    return nullptr;
}

/**
 * `tactility.json.encode(value) -> string | nil, reason`
 *
 * Serializes Lua tables. A table with only consecutive integer keys becomes a
 * JSON array, any other table becomes an object, and `tactility.json.null`
 * becomes null.
 */
int json_encode(lua_State* state) {
    if (!lua_checkstack(state, MAX_DEPTH + 8)) {
        return push_failure(state, "not enough stack to encode JSON");
    }

    const char* error = "unsupported value";
    cJSON* root = create_node(state, 1, 0, &error);
    if (root == nullptr) {
        return push_failure(state, error);
    }

    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) {
        return push_failure(state, "out of memory");
    }

    const size_t length = std::strlen(text);
    if (length > MAX_OUTPUT_BYTES) {
        cJSON_free(text);
        return push_failure(state, "JSON output is too large");
    }

    lua_pushlstring(state, text, length);
    cJSON_free(text);
    return 1;
}

const luaL_Reg json_functions[] = {
    { "decode", json_decode },
    { "encode", json_encode },
    { nullptr, nullptr }
};

} // namespace

void lua_bindings_open_json(lua_State* state) {
    luaL_newlib(state, json_functions);
    push_null(state);
    lua_setfield(state, -2, "null");
    lua_setfield(state, -2, "json");
}
