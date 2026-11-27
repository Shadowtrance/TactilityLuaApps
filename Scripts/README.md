# Scripts

Every `*.lua` file in this folder is packaged as its own Tactility app by the
publish workflow. The app id is derived from the file name:
`hello.lua` becomes `tactility.lua.hello`, with the title `Hello`.

A directory with a `main.lua` is packaged too, and can use `require` for modules
that sit next to it. Its directory name becomes the app id: `weather/` becomes
`tactility.lua.weather`.

```text
weather/
  main.lua
  lib/cities.lua
  lib/weather.lua
```

```lua
local weather = require("lib.weather")
```

Use the comment block at the top of a script for a short explanation; it is not
used as the app description.

## Versioning

App Hub compares `version.code`, so a script that changes without a new code is
not offered as an update. Put the version in the leading comment block:

```lua
-- @version 1.1.0
-- @code 2
```

The packaging tool reads those tags (only in the header, before the first line
of code) and falls back to `0.1.0`/`1` when they are absent. `--version` and
`--code` still override them when packaging by hand. Bump **both** whenever the
script changes; `@code` is the part App Hub actually uses.

For a custom name, id, version, description or icon, package the script by hand:

```powershell
py Apps\LuaRuntime\tools\lua-app\make_lua_app.py --script Scripts\hello.lua `
  --name "Hello" --id tactility.lua.hello --version 1.0.0 --code 100 `
  --description "Says hello" --output build\hello.app

py Apps\LuaRuntime\tools\lua-app\make_lua_app.py --directory Scripts\weather `
  --name "Weather" --id tactility.lua.weather --output build\weather.app
```
