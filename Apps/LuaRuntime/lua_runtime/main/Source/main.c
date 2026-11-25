#include <lua/module_state.h>

#include <tactility/concurrent/mutex.h>
#include <tactility/error.h>
#include <tactility/module.h>

#include <stddef.h>

/*
 * External Lua runtime module.
 *
 * This module is only the interpreter and the kernel bindings. It deliberately
 * references no LVGL symbol, so it loads at boot like any other package module
 * (Mystify's driver module is the same shape). The UI lives in the `lua_runner`
 * app, which loads after LVGL and resolves `lua_*` from this module.
 *
 * The module exports the runtime API and the whole Lua C API through
 * symbols.c, so side-loaded apps bind to this interpreter instead of carrying
 * their own copy.
 *
 * The interpreter itself has no global state, but the module now gates new
 * runtimes: stopping it makes lua_runtime_alloc() fail until it is started
 * again. Stop is refused while a runtime is alive, so a running script is never
 * torn down underneath the app. This is the closest equivalent of a service
 * module's stop for a code-only module.
 */

/** Defined in symbols.c: the runtime API plus the whole Lua C API. */
extern const struct ModuleSymbol lua_module_symbols[];

static struct Mutex state_mutex;
static bool state_ready = false;
static bool enabled = false;
static size_t active_runtimes = 0;

/** The module loader calls main() on one task before any app can allocate. */
static void ensure_state(void) {
    if (!state_ready) {
        mutex_construct(&state_mutex);
        state_ready = true;
    }
}

bool lua_module_acquire_runtime(void) {
    if (!state_ready) {
        return false;
    }

    mutex_lock(&state_mutex);
    const bool acquired = enabled;
    if (acquired) {
        active_runtimes++;
    }
    mutex_unlock(&state_mutex);
    return acquired;
}

void lua_module_release_runtime(void) {
    if (!state_ready) {
        return;
    }

    mutex_lock(&state_mutex);
    if (active_runtimes > 0) {
        active_runtimes--;
    }
    mutex_unlock(&state_mutex);
}

static error_t lua_module_start(void) {
    ensure_state();
    mutex_lock(&state_mutex);
    enabled = true;
    mutex_unlock(&state_mutex);
    return ERROR_NONE;
}

static error_t lua_module_stop(void) {
    ensure_state();
    mutex_lock(&state_mutex);
    if (active_runtimes > 0) {
        mutex_unlock(&state_mutex);
        return ERROR_RESOURCE_BUSY;
    }
    enabled = false;
    mutex_unlock(&state_mutex);
    return ERROR_NONE;
}

static struct Module module = {
    .name = "luaruntime",
    .start = lua_module_start,
    .stop = lua_module_stop,
    .drivers = NULL,
    .symbols = lua_module_symbols,
    .internal = NULL,
};

// The entry point of a module returns its struct Module instead of an exit code.
// The asm label keeps the symbol name of main while allowing another return type.
// The build renames main to app_main for the ELF loader, which is why the label goes through a macro.
#define ENTRY_NAME_INNER(name) #name
#define ENTRY_NAME(name) ENTRY_NAME_INNER(name)
struct Module* module_main(int argc, char* argv[]) __asm__(ENTRY_NAME(main));

struct Module* module_main(int argc, char* argv[]) {
    ensure_state();
    return &module;
}
