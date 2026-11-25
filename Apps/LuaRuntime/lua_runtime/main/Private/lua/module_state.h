// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reserves a runtime slot, or returns false when the module is stopped.
 *
 * The caller must pair a successful call with lua_module_release_runtime().
 */
bool lua_module_acquire_runtime(void);

/** Releases a slot reserved by lua_module_acquire_runtime(). */
void lua_module_release_runtime(void);

#ifdef __cplusplus
}
#endif
