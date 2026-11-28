# Lua Runtime

Run Lua 5.5 scripts on Tactility without compiling a firmware or an app for each script.

The package contains four binaries:

| Binary | Role |
|---|---|
| `lua_runtime` | Kernel module: Lua 5.5, the `tactility.*` bindings, and the exported Lua C API. No LVGL references. |
| `lua` | Companion "Lua" app: lists loose scripts and starts one in the runner. |
| `lua_runner` | Hidden app: owns the window, interprets one script, and hosts the `luavgl` bindings. |
| `lua_apps` | "Lua Apps" app: lists, opens and uninstalls the packaged Lua apps (`tactility.lua.*`). |

The split matters. The module loads at boot like any package module (Mystify's
driver module is the same shape) because it never references `lv_*`. The runner
is an app, so it loads after LVGL and can use `luavgl` without a firmware
boot-order change. It resolves `lua_*` from the module at load time.

[CATALOG.md](CATALOG.md) is the curated coverage checklist and
[catalog/sdk-coverage.md](catalog/sdk-coverage.md) is the generated per-symbol
inventory.

## Scripts

Loose scripts are a workshop, not launcher apps. By default they live in a
`lua` folder at the storage root (`/sdcard/lua` on the Tab5, `/data/lua` when
there is no card). The folder is created on demand by **New** or **Save Copy**.

Open the **Lua** app to list them. Selecting one opens the built-in editor
(the companion is a C app, not a Lua script), with **Save**, **Run**,
**Export** and **Back**. **New** asks for a name and opens a fresh script in
the editor. **Export** asks for an app name, packages the script with the
runner and icons from this package, and installs it on the device — the new
app appears in the launcher immediately. Both views keep the toolbar for the
title and a vertical-dots overflow menu: the browser's **Folder** and **New**,
and the editor's **Save**, **Run**, **Export** and **Back**, live in that menu.
The editor's status line shows the file name and a `Ln X, Col Y` readout that
follows the cursor. Enter keeps the previous line's indentation and adds four
spaces after a block opener (`{`, `(`, `[`, `then`, `do` or `else`). Tab remains
LVGL's focus-navigation key, so indent with spaces.

**Folder** chooses a different script folder (the firmware's file selection
app). The choice is stored next to the app's other settings, in
`<data>/appdata/tactility.luaruntime/runtime.properties` (on the Tab5:
`/sdcard/tactility/appdata/tactility.luaruntime/runtime.properties`).

The package's bundled examples are listed directly from its assets, under
**Examples**, alongside the scripts in the selected folder. They stay
read-only. **Run** executes a bundled example as-is; after an edit, **Run**
(or **Save Copy**) writes the editor's content into the selected folder and
continues from that copy.

- `hello.lua` — the smallest script, a text result (`result = ...`)
- `clock.lua` — live time, date and uptime in a themed card
- `console.lua` — a small Lua console: type a snippet, run it, see what it prints
- `appearance.lua` — the Appearance settings view, exported by the PC GUI designer
- `dashboard.lua` — live battery, Wi-Fi, device, mount and clock cards
- `focus_timer.lua` — a Pomodoro-style 25/5/15 focus timer on a themed arc
- `game_of_life.lua` — Conway's Game of Life on a responsive grid, with
  tap-to-edit and a generation timer
- `gpio.lua` — live GPIO pin states (the C GPIO app, written in Lua)
- `i2c_scan.lua` — scans each bus once, then the chips switch between themed
  address/name grids (the Lua I2C scanner)
- `lights_out.lua` — the Lights Out puzzle on a square, theme-coloured grid
- `memory.lua` — memory match with LVGL symbols and timed flips
- `notification.lua` — posts info/warning/error notifications, lists and dismisses
  them, and shows the live event stream
- `simon.lua` — Simon: dim/bright themed pads, a sequence timer, growing rounds
  and a best score (the highlight/timing borrows from TamaTac's pattern game)
- `system_info.lua` — Memory / Storage / Tasks / About tabs with usage bars,
  statusline and live display, heap, every mounted storage volume (SD card,
  internal data, ...) and task readings

Scripts draw into the content area below a toolbar the runner provides, so
every script has a working close button. `on_create` / `on_show` / `on_hide` /
`on_destroy` are optional.

## Packages vs loose files

Loose files are for editing and experimenting. A script that should look like a
real app — icon, name, version, uninstall — is packaged with
`Apps/LuaRuntime/tools/lua-app/make_lua_app.py`:

```powershell
py Apps\LuaRuntime\tools\lua-app\make_lua_app.py --script my_app.lua --name "My App"
tt app install tactility.lua.my-app.app --host 192.168.0.245
```

The package carries the already-built `lua_runner` ELF plus `lua/main.lua` and a
normal package manifest, so it gets a launcher tile without a firmware change.
When the runner has no script argument it treats itself as a packaged app,
resolves its own install directory and runs `lua/main.lua` from there. The chunk
receives the install directory as `...`, so assets resolve from the package.
Pass `--assets <dir>` to copy files into `assets/`; image `src` values may use the
resulting absolute path, which the bindings prefix with LVGL's `A:` drive. PNG
files decode through the firmware's lodepng decoder; SVG icons do not decode on
device.

The same package can be created without a PC: **Export** in the editor asks for
an app name and builds/installs the tar on the device.

The Lua Runtime package must be installed: its module supplies the interpreter.
When the runner changes, re-run the packaging tool to refresh the copy inside
each script package (the script itself does not need rebuilding).

## Multi-file scripts

A directory with `main.lua` at its root can split code into modules:

```text
my_app/
  main.lua
  lib/api.lua
  lib/ui.lua
```

```lua
local api = require("lib.api")   -- my_app/lib/api.lua
local ui = require("lib.ui")
```

Package the whole directory:

```powershell
py Apps\LuaRuntime\tools\lua-app\make_lua_app.py --directory my_app --name "My App"
```

`require` searches only the directory that holds the entry script (`.../lua`
inside a package). Names are relative to it: dots become directory separators
(`require("lib.api")` → `lib/api.lua`), and `.../lib/api/init.lua` works too.
Modules are cached, cycles are reported as errors, and C modules are disabled.
The editor still edits one file at a time; this is the runtime side, and a
multi-file browser is the next editor step.

`Scripts/weather` is a complete packaged example: `main.lua` plus two modules,
Open-Meteo over `tactility.http`, `tactility.json` for the payload, Material
tabs for the current conditions and the forecast, bundled weather icons and
temperature digits, a 24-hour temperature chart, a 5-day forecast, a place
search (Open-Meteo geocoder) with custom coordinates and a name,
Celsius/Fahrenheit, a refresh interval and saved settings.

`tactility.settings` keeps each app's values in that app's user-data directory:
`<data>/appdata/<app id>/<namespace>.properties`. A packaged app therefore owns
its file, the file survives reinstalling or updating the app, and the same key in
two apps cannot collide. The value is the id from the app's manifest, so Weather
stores under `tactility.lua.weather`.

The runner and the runtime must agree on Lua's number layout: a script package
embeds its own runner, and the Lua C API passes `lua_Integer` values by width.
0.1.24 moved integers to 64-bit (floats stay 32-bit), so a package built against
an earlier runtime aborts inside the runtime module when its script is
evaluated. Repackage it with 0.1.24; the script and assets do not change. Hello
0.1.2 is rebuilt for it.

## Lua Apps

The **Lua Apps** app (id `tactility.luaapps`, in this package) collects the
packaged Lua apps in one list, with their icon, name, id and version. Tapping a
row opens the app; the bin icon uninstalls it after a confirmation. It only
lists ids that start with `tactility.lua.` (the packaging convention), so the
runtime's own entries are not included, and it excludes itself. The launcher
still shows the apps as usual.

It is a C app rather than a Lua script on purpose: it reads the app manager's
package and manifest registry directly, loads the icons from each install
directory, and uses the firmware's uninstall API. Starting a packaged app and
uninstalling are handed to the app's own task; doing either on the LVGL task
trips ESP-IDF's flash-cache stack assertion for flash-loaded apps.

## Publishing a custom repository

The runtime is not part of the official app catalog. A custom repository can
host the runtime package and any packaged Lua apps, using the firmware's
**Software → App repositories** page: add the repository's base URL and the App
Hub lists its contents next to the official catalog.

Build a release folder and publish it with
[TactilityAppPublisher](https://github.com/TactilityProject/TactilityAppPublisher#readme)
(clone it next to this repository for the local commands):

```powershell
# 1. Build the runtime package (module + lua, lua_runner and lua_apps)
.\tt.ps1 app build --path Apps\LuaRuntime -a esp32p4

# 2. Package the scripts that should appear in the repository
py Apps\LuaRuntime\tools\lua-app\make_lua_app.py --script my_app.lua --name "My App" `
   --id tactility.lua.myapp --output build\lua-myapp.app

# 3. Put the .app files in one folder and generate the catalog
py ..\TactilityAppPublisher\apppublisher.py release build\lua-repo

# 4. Host it: GitHub Pages or an S3-compatible bucket
py ..\TactilityAppPublisher\apppublisher.py publish pages build\lua-repo `
   --sdk 0.8.0 --out build\lua-repo-site
```

The device requests `<base>/<sdk>/...`, where `<sdk>` drops the `-dev` postfix
(`0.8.0` for `0.8.0-dev`), so use that value for `--sdk` and add the base URL of
the published folder (`.../apps`) as the repository.

All packages in one repository must target the same SDK. The generated Lua app
manifests now carry `description.short` (at most 32 characters), which the
publisher requires; it defaults to `Lua app: <name>`.

## Bindings

The Lua surface is split into a capability API (`tactility.gpio`,
`tactility.fs`, …) and a raw escape hatch for exported SDK symbols. Capability
glue is generated from `bindings/*.toml` at build time, so adding a function is
an IDL entry, not a wrapper. GPIO, I2C, ADC, UART and SPI are generated tables
(UART, I2C and SPI add hand-written block transfers); the Lua GPIO
app uses GPIO. I2C also has hand-written block transfers (`read`, `write`,
`write_read`) because the IDL has no buffer type yet. `tactility.raw.call(name, ...)` resolves an exported symbol at
runtime and calls it with pointer-sized arguments; the typed signature database
is the next step. Notification send/list/dismiss are hand-written and do not
expose actions or Bundle data yet; the runner adds `subscribe(callback)` for the
posted/dismissed/cleared/updated event stream, delivered on the LVGL task. See
[BINDINGS.md](BINDINGS.md) for the design and
[tools/lua-bindings](tools/lua-bindings/README.md) for the generator. The
searchable reference is generated from these bindings:
<https://shadowtrance.github.io/TactilityLuaApps/api/> (rebuild with
`py tools\api-docs\generate.py`).

`tactility.display` finds the active panel and drives its backlight:

```lua
local width, height = tactility.display.size()
local shape = tactility.display.shape()       -- { shape = "rounded", radius = 32 }
local percent = tactility.display.brightness_percent()
tactility.display.set_brightness_percent(60)
```

`brightness_percent()` uses the same 10%-step mapping as the Display settings
screen; 0% is the dimmest level that keeps the backlight on. `has_backlight()`
is false on a panel without one, and the brightness calls return `nil, reason`.

`tactility.fs` reads and writes whole files and lists directories. Picking a
path opens the firmware's file selection dialog, and reports back through a
callback:

```lua
tactility.fs.pick_file({ extensions = { "txt", "lua" } }, function(path, reason)
    if path then
        tactility.log.info("picked", path)
    end
end)

tactility.fs.pick_folder(function(folder) end)
tactility.fs.pick_files({ allow_new = true }, function(paths) end)
```

`pick_file` and `pick_folder` pass one path; `pick_files` passes an array.
`allow_new` permits a name that does not exist yet (save-as). The callback runs on
the app's own task with the LVGL lock held, so it can create widgets. Cancelling
(with `cancel()` or the dialog's close button) calls back with `nil, "cancelled"`.
The call returns a handle with `cancel()` and `is_active()`; the picker is
asynchronous because the callback must not run before the script has finished, and
blocking would freeze the UI.

`tactility.http.request(options, callback)` fetches a URL without blocking the UI:

```lua
tactility.http.request({
    url = "https://api.open-meteo.com/v1/forecast?latitude=52.52&longitude=13.41&current=temperature_2m",
    headers = { Accept = "application/json" },
    timeout_ms = 20000,
    max_bytes = 32768,
}, function(response, reason)
    if response then
        local data = tactility.json.decode(response.body)
        tactility.log.info("weather", data.current.temperature_2m .. " C")
    end
end)
```

The request runs on its own task; the callback runs from the app loop with the
LVGL lock held, so it may update widgets. `response` is
`{ status, body, length, content_type }`; a received 4xx/5xx is still a response,
while transport failures pass `nil, reason`. Up to four requests may be active,
request bodies are capped at 64 KB and responses at `max_bytes` (1 MB hard limit).
A GET without a body uses the firmware's cross-platform client with certificate
verification and up to five redirects, so it also works in the simulator. Other
methods (and requests with a body) stay on the ESP-IDF client and are ESP-only.
`options.certificate` supplies a PEM certificate for either path.

`tactility.json.decode(text)` and `tactility.json.encode(value)` map JSON to Lua
tables and back. Arrays are 1-based, objects are string-keyed, and JSON `null` is
`tactility.json.null` so array slots do not collapse. Input and output are capped
at 512 KB and nesting at 64 levels; failures return `nil, reason`.

`tactility.http.download(options, callback)` streams a GET to a file with the
firmware's cross-platform client. It supports headers, a custom certificate,
redirects, progress through `options.progress(received, total)`, and cancellation;
it works in the simulator too. The callback receives `{ status, length, path }` or
`nil, reason`.

The runner's own toolbar is available to scripts, so actions belong in the real
toolbar instead of a second bar in the content area:

```lua
local toolbar = tactility.ui.toolbar()
lvgl.toolbar.add_icon_action(toolbar, lvgl.ICON.REFRESH, refresh)
lvgl.toolbar.add_icon_action(toolbar, lvgl.ICON.SETTINGS, open_settings)
```

`add_text_action` and `add_icon_action` return the action button; callbacks are
detached when the script closes. `keyboard:set_textarea(field)` binds an on-screen
keyboard to a text field, which the Weather settings view uses for custom
coordinates.

`lvgl.TileGrid` is the firmware's launcher-style paged grid: pass `items` with
`id`, `name` and an optional `icon`/`highlighted`/`checked`, and optionally
`on_clicked`, `on_long_pressed` and `on_page_changed`. `set_items()`, `set_style()`,
`set_icon_color()`, `next_page()`, `previous_page()`, `page()`, `find_tile()` and
`tiles()` cover the rest.

`lvgl.PathBar(toolbar, { path = ..., on_pressed = ..., on_rebuilt = ... })` puts
the firmware's path segments in the runner toolbar. `set_path()`,
`focus_parent_segment()` and `parent_segment()` update and inspect it; the
callback receives the pressed path and whether a key triggered it.

`tactility.audio.status()` reports the audio stream device, whether output is
supported and the shared volume/mute/enabled state. `tactility.audio.play_file`
plays a 16/24/32-bit PCM WAV on a worker task and returns a handle with `stop`,
`pause`, `resume`, `is_active`, `position`, `duration`, `volume` and `error`.
Playback needs an output codec; devices without one report a skip in the test.

`tactility.ui.alert`, `tactility.ui.input` and `tactility.ui.select` show the
firmware's own modal dialogs, and `tactility.fs.pick_file`/`pick_files`/`pick_folder`
show its native file picker. Both return a handle with `dismiss()`/`cancel()` and
`is_active()`; results arrive through the runner's app loop with the LVGL lock held,
so the callback can update widgets. None of these starts another app, so they also
work in the simulator.

`tactility.system.memory()` returns `{ internal = usage, external = usage }`
with `free`, `total`, `used` and `percent` values. External is all zeroes on a
board without PSRAM. `tactility.system.storage(path?)` finds the mounted file
system that contains `path` (default `/sdcard`) and returns the same fields
from its usage query. It returns `nil, reason` when no mount contains the path
or the file system does not report usage.
`tactility.system.tasks()` returns a snapshot array of `{ name, state,
priority, base_priority, stack_free, runtime, number }`.

`tactility.adc.read_raw(controller, channel, timeout?)` returns the raw
conversion result for a channel the board's devicetree already configures. Find
the controller with `tactility.device` (`type() == "adc-controller"`); an
unconfigured channel returns `nil, reason`.

```lua
local raw = tactility.adc.read_raw(controller, 0, 50)
```

`tactility.uart` opens and closes a UART controller, reads and writes single
bytes or blocks, reports available bytes, flushes input and reads or applies
its baud rate, data bits, parity and stop bits. Only open a controller the
script owns: a GPS or radio service may already hold one.

```lua
local config = tactility.uart.config(controller) -- { baud_rate, data_bits, parity, stop_bits }
tactility.uart.set_config(controller, { baud_rate = 115200, parity = "none" })
tactility.uart.open(controller)
tactility.uart.write_bytes(controller, "AT\r\n")
local reply = tactility.uart.read_until(controller, 10, 64, 500)
tactility.uart.close(controller)
```

`set_config` requires a closed controller and starts from the current values,
so a script can change only the baud rate. `parity` is `"none"`, `"even"` or
`"odd"`; `stop_bits` is `1`, `1.5` or `2`. A controller without a stored
configuration (a console UART, for example) makes `config()` return
`nil, reason`; pass all four fields to configure it.

`tactility.spi` performs one-shot transfers: each call adds a temporary
peripheral for the given clock/mode/chip-select, transfers and removes it. Use
only a chip select that the script owns; a display or SD driver may already hold
one. `transmit` sends and receives the same length, `read` leaves the transmit
line idle, and `write` discards the response. `lock`/`try_lock`/`unlock` take
the controller's bus mutex.

For several transfers to one peripheral, `open` returns a persistent handle:

```lua
local device = tactility.spi.open(controller, 1000000, 0, 0)
device:write("\x9F")           -- JEDEC id command
local id = device:read(3)
device:close()
```

The handle methods use the same `transmit`/`read`/`write` contract and close
the peripheral when the handle is collected, so an explicit `close` is optional.

`tactility.app` lists every registered app (`list()` returns `{ id, name,
category, hidden }`), reports whether one is running (`is_running(id)`), returns
the topmost app id (`current()`) and launches one (`start(id, args?)`, which
returns the new instance id). `stop(instance_id_or_id)` closes a running app and
refuses the app that runs the script; `install(path)` and `uninstall(package_id)`
manage `.app` packages. Starting an app returns as soon as it is scheduled; its
window opens like any launcher start.

### Widgets

`lvgl` covers LVGL's core widgets plus Tactility's framework widgets:
`Object`, `Label`, `Button`, `Slider`, `Bar`, `Arc`, `Spinbox`, `Switch`,
`Checkbox`, `Dropdown`, `Roller`, `TextArea`, `Line`, `Image`, `Spinner`,
`LED`, `Table`, `Scale`, `ImageButton`, `Chart`, `Calendar`, `Keyboard`,
`ButtonMatrix`, `Tabview`, `Tileview`, `Tabs`, `Statusline`, `Card`, `Chip`,
`SliderBox`, `List`, `Badge`, `PageIndicator`, `StepIndicator`, `IconButton`
and `TextButton`. They are themed by the firmware, so scripts stay consistent
with the device's colours and density.

The binding deliberately follows LVGL 9.6: widgets that were removed
(`Colorwheel`), deprecated (`Menu`, `Win`) or disabled in the firmware
(`Msgbox`) are not exposed. `add_flag`/`remove_flag`/`has_flag` remain the Lua
interface but call LVGL's dedicated `lv_obj_set_<flag>()`/`lv_obj_is_<flag>()`
functions, not the deprecated generic flag API.

The object API includes states, flags, child lookup, `size()`/`position()`,
scrollbar direction and visibility, and a broad `set{}` surface: min/max size,
margins and edge padding, outline, shadow width/colour, text alignment and
spacing, transforms, percentages, flex and grid.

`lvgl.Group` gives an encoder or keyboard a focus order:

```lua
local group = lvgl.Group()
group:add(first); group:add(second)
group:focus(first)
group:next()
group:set_default() -- let input devices use this group
group:attach_input() -- take over keypads/encoders; previous groups are restored on close
```

The theme's keyboard focus ring is the secondary colour; enabling group editing
switches to the primary edit ring. `group:focus()` sets `LV_STATE_FOCUSED` only,
so add `lvgl.STATE.FOCUS_KEY` when a programmatically focused widget should show
the keyboard ring.

`lvgl.Anim` animates one numeric property from C, so the interpreter is not in
the frame loop:

```lua
local move = lvgl.Anim(box, {
    property = "x", from = 0, to = 200, time = 300,
    path = lvgl.ANIM_PATH.EASE_IN_OUT,
})
move:start()
```

Properties are `x`, `y`, `w`/`width`, `h`/`height`, `opa`, `bg_opa`, `radius`,
`border_width`, `text_opa`, `transform_rotation`, `transform_scale_x/y`,
`transform_width/height`, `bg_color`, `text_color`, `border_color` and `value`
(slider, bar, arc, spinbox or `SliderBox`). Deleting the target stops its
animations, and closing the app stops anything still running.

```lua
local card = lvgl.Card(screen, { w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT })
local chip = lvgl.Chip(card, { text = "Selected", checked = true })
local slider = lvgl.SliderBox(card, 0, 100, 5, 40)

local list = lvgl.List(card, { w = lvgl.PCT(100) })
list:add_text("Section")
list:add_button("Open", lvgl.SYMBOL.RIGHT)

slider:set{ value = 60 }
chip:set_checked(false)
```

`Chip` keeps its text on an internal label, so `text`, `checked` and
`set_checked` work as they do for a checkbox. `SliderBox` takes its range and
step as constructor arguments (`parent?, min, max, step?, value?`) because they
cannot change afterwards; `value` is read with `value()` and written with
`set{ value = ... }`, clamped to the range.

`Tabs` is the Material 3 tab bar. The app swaps the pane itself when
`VALUE_CHANGED` fires:

```lua
local tabs = lvgl.Tabs(root, { mode = lvgl.TABS_MODE.WRAP })
tabs:add_tab("One")
tabs:add_tab_with_icon(lvgl.ICON.MEMORY, "Memory")
tabs:set_selected(2)          -- 1-based
local selected = tabs:selected()
tabs:set_tab_badge(2, true)
```

`lvgl.ICON` is the firmware's shared Material Symbols set (87 names such as
`INFO`, `MEMORY` and `HARD_DISK`). It is separate from `lvgl.SYMBOL`, which is
LVGL's own icon font. Icons are ordinary glyph strings: set the caption and the
icon font together, or the glyph renders as a missing character. List rows take
the icon as their second argument:

```lua
lvgl.Label(root, { text = lvgl.ICON.MEMORY, text_font = "icon" })
local button = lvgl.IconButton(root)
lvgl.Label(button, {
    text = lvgl.ICON.SAVE, text_font = "icon", align = lvgl.ALIGN.CENTER,
})
list:add_button("Open", lvgl.ICON.FOLDER)
```

`Statusline` is one line of status text with a coloured level dot:

```lua
local line = lvgl.Statusline(root, {
    text = "Mesh is running",
    level = lvgl.STATUSLINE_LEVEL.OK,
})
line:set_text("No internet")
line:set_level(lvgl.STATUSLINE_LEVEL.WARNING)
```

`Line` takes an even list of x/y pairs with `set_points`; the binding owns the
array LVGL keeps by pointer:

```lua
local spark = lvgl.Line(root, { w = 200, h = 80 })
spark:set_points{ 0, 70, 40, 30, 80, 55, 120, 10, 160, 40, 200, 20 }
```

Grids use LVGL's descriptors without exposing raw arrays:

```lua
local card = lvgl.Card(screen, { w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT })
card:grid(
    { lvgl.GRID_CONTENT, lvgl.GRID_FR(1) },
    { lvgl.GRID_CONTENT, lvgl.GRID_CONTENT }
)

local title = lvgl.Label(card, { text = "10 devices found" })
title:grid_cell(lvgl.GRID_ALIGN.START, 0, 2, lvgl.GRID_ALIGN.CENTER, 0, 1)

local address = lvgl.Label(card, { text = "0x10" })
address:grid_cell(lvgl.GRID_ALIGN.START, 0, 1, lvgl.GRID_ALIGN.START, 1, 1)
```

Apply a grid while the widget is being built. In this LVGL build, re-applying
`grid()` to an already laid-out widget (for example from a timer or event
handler) corrupts teardown, so build one card per layout and switch cards with
`add_flag(lvgl.FLAG.HIDDEN)` / `remove_flag` instead. The scanner does this.

`layout = lvgl.LAYOUT.NONE` disables the parent's flex/grid layout so children
use their `x`/`y` coordinates. This is what the PC GUI designer exports:

```lua
local canvas = lvgl.Object(lvgl.root(), {
    w = lvgl.PCT(100),
    h = lvgl.PCT(100),
    layout = lvgl.LAYOUT.NONE,
    pad_all = 0,
})
```

The runner's content area is an ordinary object on the app's display, so every
widget inherits the active Material theme, including light/dark, mono, density
and the user's colour choices. The framework widgets use their themed classes;
the examples avoid hardcoded surface colours. A script that draws its own
spacing can follow the appearance settings:

```lua
local gap = lvgl.theme_lerp(4, 8)          -- uses theme_spacing() as percent
lvgl.theme_color_primary()
lvgl.theme_color_secondary()
lvgl.theme_color_error()
lvgl.theme_spacing()                        -- 0 (compact) to 100 (regular)
lvgl.theme_size()                           -- control size/radius scale
lvgl.theme_mono()
lvgl.hardware_keyboard_available()          -- a started hardware keyboard counts
```

`hardware_keyboard_available()` uses the same check as the firmware's
on-screen keyboard: a keypad, encoder or trackball registers as a keyboard
device too, but is marked restricted and does not count. A script that shows
its own software keyboard can skip it when this returns true.

`theme_lerp(compact, regular, percent?)` is the same interpolation the theme
uses, so a custom layout tracks the density slider instead of picking pixels.

## Runtime API version

Packaged apps declare `lua.runtime.api=N` in their manifest. The runner reads
`_LUA_RUNTIME_API` from the interpreter module and, when the module is older
than the package asks for, shows an update message instead of failing with an
unresolved symbol or a broken screen. `LUA_RUNTIME_API_VERSION` in
`lua_runtime/main/Include/lua/runtime.h` is the single value to bump when the
runtime gains something a packaged app depends on.

## Tests

`tests/` contains on-device regression scripts:

- `luavgl_smoke.lua` — objects, text areas (create/clean/recreate/delete),
  widget values, styles, events and a timer. It reports `PASS (N checks)`.
- `teardown.lua` — recreates the retired editor's text-area cleanup and deletes a
  wrapped widget before closing, which used to fault during `luavgl` teardown.
- `object_api.lua` — core object/style properties, states, flags, child access,
  scrolling, Arc and Spinbox.
- `widget_batch2.lua` — LED, Table, Scale, ImageButton, Chart, Calendar,
  Keyboard and ButtonMatrix.
- `widget_batch3.lua` — Tabview and Tileview.
- `tabs_statusline.lua` — Material 3 Tabs, Statusline and the shared `lvgl.ICON` table.
- `display.lua` — display size, shape, backlight range and percentage control.
- `system.lua` — internal/external heap usage and FAT storage statistics.
- `line.lua` — line points, replacement and the owned point array.
- `assets.lua` — the install-directory argument and a packaged PNG in `Image`
  and `ImageButton`.
- `group.lua` — input groups, focus next/prev/freeze, wrap, editing and default.
- `animation.lua` — x/value/colour animations, stop and target deletion.
- `gpio.lua` — GPIO controller discovery, count and the invalid-pin error path.
- `i2c.lua` — controller discovery, address probing and the missing-device path.
- `adc.lua` — ADC controller discovery, raw channel read and the invalid-channel path.
- `uart.lua` — UART discovery, `is_open`/`available` and the closed error paths.
- `spi.lua` — SPI discovery, constants, invalid CS and try_lock/unlock.
- `icons.lua` — shared icon names, including the icons added in the latest firmware.
- `grid.lua` — grid tracks and cell placement on a themed card.
- `notification.lua` — send, get, list, dismiss and missing-id handling.
- `notification_events.lua` — subscribe, posted/dismissed/cleared order and cancel.
- `bindings.lua` — fs, time, device, settings, power and Wi-Fi smoke test.
- `settings.lua` — multi-key round trip, replacement, `keys`/`has`/`remove`/`clear`,
  and repeated writes after the file already exists (the FAT rename case).
- `dialogs.lua` — native alert/input/select option validation and dismissing an alert.
- `download.lua` — HTTPS download to a file and the result shape.
- `tile_grid.lua` — launcher grid items, styles, paging and lookup.
- `path_bar.lua` — toolbar path segments, path updates and parent lookup.
- `audio.lua` — WAV playback through the audio stream device (skips without output).
- `raw.lua` — `tactility.raw.call` symbol lookup and missing-symbol error.

Run the whole suite on connected devices (serial + development server):

```powershell
py Apps\LuaRuntime\tools\device-tests\run.py ^
  --device "Tab5,192.168.0.245,esp32p4,COM34" ^
  --device "T-Deck Plus,192.168.0.206,esp32s3,COM22"
```

See [tools/device-tests](tools/device-tests/README.md) for the options. A single
script can still be run from the Lua app, or packaged for a headless run:

```powershell
py Apps\LuaRuntime\tools\lua-app\make_lua_app.py --script tests\luavgl_smoke.lua --name "Lua Smoke Test"
tt app install tactility.lua.luavgl-smoke.app --host 192.168.0.245
```

The publish workflow also runs [tools/elf-audit](tools/elf-audit/README.md), which
fails when a packaged Xtensa image contains a relocation that the S3 section
loader would map to `NULL`.

## GUI designer

[tools/lua-gui-designer](tools/lua-gui-designer/README.md) is a PC
(Custom2kinter) designer for `luavgl` scripts. It previews a project with the
firmware's Material theme and device profiles, then exports a Lua script with
empty event handlers. The project JSON is the source of truth; the Lua preview
has syntax highlighting and the event editor has auto-indent.

```powershell
py Apps\LuaRuntime\tools\lua-gui-designer\designer.py
py Apps\LuaRuntime\tools\lua-gui-designer\designer.py --self-test
py Apps\LuaRuntime\tools\lua-gui-designer\visual_test.py
```

## Catalog

[CATALOG.md](CATALOG.md) is the curated checklist: runtime, UI surface,
capabilities, app clones and known limitations, with demo/test columns.
[catalog/sdk-coverage.md](catalog/sdk-coverage.md) is generated from the SDK and
the firmware's export tables by `tools/sdk-catalog/catalog.py`; it classifies
every exported/declared symbol as generated, bound, raw-only or unbound.
[catalog/measurements.md](catalog/measurements.md) has boot, memory and
app-start timings for the Tab5 and the T-Deck Plus.
[catalog/notification-actions.md](catalog/notification-actions.md) is the design
for notification data and tap actions without storing Lua callbacks in C.

```powershell
py tools\sdk-catalog\catalog.py          # regenerate catalog/
py tools\sdk-catalog\catalog.py --check  # fail if an IDL symbol is missing
```

## Symbol exports

The runner resolves LVGL and Lua symbols from the loaded modules at ELF load
time. A binding that calls a function the module does not export makes the app
fail to start, even though it links. Audit the built runner:

```powershell
py tools\symbol-audit\symbol_audit.py --check
py tools\symbol-audit\symbol_audit.py --fix
```

`--fix` appends missing entries to the owning `symbols.c`. Module changes need a
Tactility firmware rebuild before they are active on the device.

## Firmware requirement

Current Tactility firmware exports the kernel, LVGL and libc/libm symbols the
external runtime needs, so **no firmware patch is required**. The runtime only
uses public kernel APIs; it does not call `esp_*` functions, which keeps it
portable to firmware built for another platform.

Earlier firmware needed hand-written export patches. Those lived in this app as
`firmware-*.patch`; the upstream exports made them obsolete and they were
removed. If a later firmware lacks an export, add it generically upstream (or
recreate a temporary patch here) rather than working around it in a binding.

## Stopping the runtime

The runtime is an external module. **Settings > Kernel** can stop it: new
scripts then fail with “Failed to allocate Lua runtime”, while a script that is
already running keeps its interpreter. Stop is refused (`resource busy`) while a
runtime is alive, so a running script is never torn down underneath the app.
Starting the module enables new runtimes again. The module's code stays mapped,
as it does for every kernel module; only the Lua API boundary is gated.

## Simulator

The same package also builds for **linux-x86_64** and runs in FirmwareSim on a
Linux host (WSL2 with WSLg works). The simulator's POSIX loader needs one
simulator-only change: a module's handle must be promoted to `RTLD_GLOBAL` so
apps loaded later can resolve its symbols. Apply
`firmware-app-posix-module-rtld-global.patch` to the firmware; device firmware
needs no patch. See [catalog/simulator.md](catalog/simulator.md) for the build,
run and install steps.

`firmware-elf-loader-end-pointer.patch` was also removed. It mapped a pointer
to one-past-the-end of a loaded section to the end of its copy instead of
`NULL`; the runtime no longer uses that pattern. `tools/elf-audit/elf_audit.py`
still guards the S3 loader in CI by rejecting the resulting `NULL` relocation.

The earlier boot-order change is **not** needed with this split: the module has
no LVGL references, and the runner is an app. The firmware boot path is back to
upstream's.

## Build

From this directory:

```bash
tt app build -a esp32p4
tt app install --host <device-ip>
```

This is a multi-binary v3 package: `get_binary_dirs()` builds `lua/`,
`lua_runner/` and `lua_runtime/` as independent CMake projects.

## Limits

- `io.tmpfile()` needs a writable `/tmp`, which the device filesystem does not
  mount; `io.open` works normally. `os.tmpname()` returns a `/tmp/...` name from
  newlib, but opening that path fails for the same reason.
- `requires.ram=1M` keeps the package off devices without PSRAM. Targets are
  `esp32s3` (T-Deck Plus), `esp32p4` (Tab5), `esp32s31` (esp-mosaico) and
  `linux-x86_64` (simulator).
- LVGL 9.6. The `luavgl` port needed one font API rename; LVGL symbols now come
  from the firmware's generic exports.
- There is no sandbox. A script has the same access as an ELF app.
