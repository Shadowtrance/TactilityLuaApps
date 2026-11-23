// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include <lua.h>
}

/**
 * Each of these expects the `tactility` table on top of the stack, and adds one
 * sub-table to it, leaving the stack as it found it.
 */

/**
 * Handle accessors, for generated bindings to call.
 *
 * A handle is not a bare pointer - DeviceHandle carries a `released` flag so an explicit
 * release() followed by collection does not double-put the kernel's reference, and these
 * raise when a script uses a handle it already gave up. No type signature can express
 * that, so generation stops here and defers to the code that owns the lifetime rules.
 * See GENERATION.md.
 */
struct Device* lua_bindings_check_device(lua_State* state, int index);
struct GpioDescriptor* lua_bindings_check_gpio_descriptor(lua_State* state, int index);

void lua_bindings_open_log(lua_State* state);
void lua_bindings_open_device(lua_State* state);
void lua_bindings_open_fs(lua_State* state);
void lua_bindings_open_settings(lua_State* state);
void lua_bindings_open_time(lua_State* state);
void lua_bindings_open_power(lua_State* state);
void lua_bindings_open_display(lua_State* state);
void lua_bindings_open_wifi(lua_State* state);
void lua_bindings_open_notification(lua_State* state);

/** Generated from the bindings IDL; adds every capability table to `tactility`. */
void lua_bindings_open_generated(lua_State* state);

/** Generated: extracts the C pointer behind a capability handle, or NULL. */
void* lua_bindings_raw_pointer(lua_State* state, int index);

/** Reads a Lua string as a byte buffer. The pointer is valid while the string is on the stack. */
const uint8_t* lua_bindings_check_bytes(lua_State* state, int index, size_t* size);

/** Pushes a byte buffer as a Lua string. */
void lua_bindings_push_bytes(lua_State* state, const uint8_t* data, size_t size);

/** Adds the low-level `tactility.raw` escape hatch. */
void lua_bindings_open_raw(lua_State* state);
