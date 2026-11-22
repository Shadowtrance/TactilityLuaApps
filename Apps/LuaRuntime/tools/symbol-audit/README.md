# Symbol export audit

`lua_runner` is a side-loaded ELF, so its `lv_*` and `lua_*` references are
resolved at load time from the `lvgl-module` and `lua_runtime` modules. If a
symbol is missing from a module's `DEFINE_MODULE_SYMBOL` table, the app fails to
start even though it linked successfully.

This tool compares the built runner's dynamic undefined symbols with the module
export tables:

```powershell
py Apps\LuaRuntime\tools\symbol-audit\symbol_audit.py --check
```

It finds the RISC-V `nm`, the runner ELF and the two `symbols.c` files
automatically. Use `--elf`, `--nm`, `--symbols` or `--prefix` to override them.

When symbols are missing, `--fix` appends them to the right `symbols.c`:

```powershell
py Apps\LuaRuntime\tools\symbol-audit\symbol_audit.py --fix
```

Five important details:

1. `--fix` edits **TactilityWork**, not just the LuaRuntime app.
2. The firmware must be rebuilt after the module export table changes.
3. Run the audit **after** building the runner, not before.
4. The audit ignores unrelated undefined symbols such as libc and app-manager
   symbols; it only checks `lv_`, `lvgl_`, `lua_`, `luaL_` and `luaopen_`.
5. A clean audit is required before adding a new binding that uses LVGL APIs.
