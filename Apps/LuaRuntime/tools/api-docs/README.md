# api-docs

Generates the searchable Lua API reference published at
<https://shadowtrance.github.io/TactilityLuaApps/api/>.

The entries come from the binding sources, not a second list:

- `bindings/*.toml` for generated capabilities (signatures, docs, types,
  constants);
- the `luaL_Reg` tables in `lua_runtime/main/Source/lua` and
  `lua_runner/main/Source/luavgl`, plus the doc comment above each C function;
- the LVGL constant tables, `icons_shared.inc` and the `SYMBOL` table.

Adding a doc comment above a binding function and rerunning the generator is
enough to document it. Comments that start with a backticked signature are
parsed as both signature and description; other comments become the description
only.

## Output

Written to `Apps/LuaRuntime/catalog/api`:

| File | Purpose |
|---|---|
| `index.html` | Self-contained page (inline JSON, no network requests) |
| `api.json` | The same entries for tooling |
| `README.md` | What is included and how to regenerate |

## Run

From `Apps/LuaRuntime`:

```powershell
py tools\api-docs\generate.py
py tools\api-docs\generate.py --check
```

`--check` renders in memory and exits non-zero when the committed files differ,
which is what CI runs. The output has no timestamp, so it is deterministic.

The Pages job copies `catalog/api` to `/api/` after building the app catalog.
