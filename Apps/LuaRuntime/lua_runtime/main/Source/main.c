#include <tactility/module.h>

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
 * Lua's standard library also needs a handful of libc functions (tmpfile,
 * tmpnam, ungetc, setlocale, strtof, log2f) that the firmware now exports from
 * its c-symbols module, so they are the real implementations rather than local
 * fallbacks. See `firmware-c-symbols-exports.patch`.
 */

/** Defined in symbols.c: the runtime API plus the whole Lua C API. */
extern const struct ModuleSymbol lua_module_symbols[];

// No start/stop: the interpreter holds no global state of its own, so there is
// nothing to bring up. Each runtime is independent and created on demand.
static struct Module module = {
    .name = "luaruntime",
    .start = NULL,
    .stop = NULL,
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
    return &module;
}
