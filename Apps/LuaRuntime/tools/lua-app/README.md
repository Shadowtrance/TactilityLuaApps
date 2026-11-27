# lua-app

Package a Lua script as an installable Tactility app, so it gets a real
launcher tile, name, icon, version and uninstall — without compiling anything.

The package contains the already-built `lua_runner` ELF from
`Apps/LuaRuntime` plus the script at `lua/main.lua` (and, with `--directory`, the
modules it requires). It therefore needs the **Lua Runtime package** installed on
the device, because that is where the Lua interpreter module comes from.

## Usage

```powershell
# Build the runner once (the script packages reuse its ELF)
tt app build --path Apps\LuaRuntime -a esp32p4

# Package a script (id and name are derived from the file name)
py Apps\LuaRuntime\tools\lua-app\make_lua_app.py --script my_script.lua

# Or a multi-file script directory (with main.lua and modules)
py Apps\LuaRuntime\tools\lua-app\make_lua_app.py --directory my_app --name "My App"

# Or set them explicitly
py Apps\LuaRuntime\tools\lua-app\make_lua_app.py --script dashboard.lua --name "Lua Dashboard" --id tactility.lua.dashboard

# Install it
tt app install tactility.lua.dashboard.app --host 192.168.0.245
```

The app appears in the launcher immediately after install and can be removed
through the usual app management.

## Package layout

```
manifest.properties         package/app metadata
icon.svg, icon_mono.svg     launcher icons (defaults to the Lua icons)
lua/main.lua                the script (or the entry of a --directory package)
lua/...                     modules copied from --directory, preserving paths
bin/<platform>/lua_runner.elf
assets/...                  optional, with --assets DIR
```

The script receives its install directory as the chunk's vararg, so assets
resolve the same from the package:

```lua
local app_dir = ...
local file = io.open(app_dir .. "/assets/data.txt", "r")
```

## Options

| Option | Meaning |
| --- | --- |
| `--script FILE` | package one Lua script (copied to `lua/main.lua`) |
| `--directory DIR` | package a directory with `main.lua`, preserving subdirectories |
| `--name NAME` | launcher name (default: title-cased file name) |
| `--id ID` | package and app id (default: `tactility.lua.<file-name>`) |
| `--version`, `--code` | app version (default `0.1.0`, `1`) |
| `--description` | launcher description |
| `--platform` | target platform, repeatable (default `esp32p4`) |
| `--runner` | alternate `lua_runner.elf` |
| `--icon`, `--mono-icon` | launcher icons |
| `--assets DIR` | copy a folder to `assets/` |
| `--ram` | `requires.ram` (default `1M`) |
| `--requires-api` | minimum Lua runtime API written as `lua.runtime.api` (default from the runtime header) |
| `--output` | output file (default `<id>.app`) |

## Caveats

- The runner ELF is copied into every package. When the Lua Runtime is updated
  with a runner change, re-run this tool (no rebuild of the script is needed)
  to ship the new runner.
- The Lua runtime package must be installed and its module started; the script
  app alone cannot interpret Lua.
- The app id is limited to 32 characters and must be lowercase letters, digits,
  `.` and `-`.
