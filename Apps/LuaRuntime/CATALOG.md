# Lua Runtime catalog

The curated checklist for the external Lua runtime. Tick items as they are
implemented **and** tested. The generated SDK inventory lives next to it in
[`catalog/sdk-coverage.md`](catalog/sdk-coverage.md); that file is not edited by
hand.

Legend:

- `[x]` implemented and tested on the Tab5
- `[~]` implemented, but limited or not fully exercised
- `[ ]` planned
- `[!]` blocked or a known limitation
- `[-]` deliberately out of scope

Every item should name its C surface or IDL and, where one exists, the demo or
test that proves it.

## 1. Runtime and packaging

| Status | Item | Notes |
|---|---|---|
| [x] | External `lua_runtime` module | Lua 5.5; no firmware fork |
| [x] | Hidden `lua_runner` app | Owns the window, Lua state and `luavgl` |
| [x] | Companion `lua` app | Browser, editor, Run, Export |
| [x] | Script folder `<storage>/lua` | Configurable; persisted in appdata |
| [x] | Package v3 output | Launcher tile, manifest, icons, module |
| [x] | On-device Export | Tar writer + `app_install()` |
| [x] | Runtime API guard | `LUA_RUNTIME_API_VERSION` = 7 |
| [x] | Toolbar close actions | Runner and companion emit `APP_EVENT_CLOSE` |
| [x] | Missing-export failure | Module load fails at install, not at script runtime |
| [x] | Companion runtime-version display | Browser title shows the installed package version |
| [x] | `require` / multi-file scripts | Modules resolve from the entry script's directory; `--directory` packaging |
| [x] | Package asset API (images) | `...` gives the install dir; absolute paths get the `A:` LVGL prefix; PNG decodes through lodepng |
| [ ] | Sandbox / permissions | A script has full ELF-app access |
| [-] | Firmware fork | Not needed with the external-module design |

## 2. Lua core and standard library

| Status | Item | Notes |
|---|---|---|
| [x] | Base, string, table, math, os | Standard Lua 5.5 libraries |
| [x] | `io.open`, `io.read/write` | Works on mounts |
| [x] | `result` fallback | Runner draws it when a script creates no widgets |
| [x] | `print` / `tactility.log` | UART logging |
| [~] | `os.time` / `os.date` | Custom `tactility.time` is richer |
| [~] | `coroutine` | Available; not yet used by an example |
| [!] | `io.tmpfile()` | Needs a writable `/tmp`; not mounted |
| [!] | `os.tmpname()` | Returns a `/tmp/...` name that cannot be opened |
| [x] | `require` search path | Scoped to the entry script's directory; editor view still single-file |
| [ ] | `debug` library policy | Decide whether to expose it |
| [ ] | UTF-8 / locale regression test | `setlocale` export is present |

## 3. UI core (`luavgl`)

| Status | Item | Notes |
|---|---|---|
| [x] | `Object`, `Label`, `Button` | Base widgets |
| [x] | `Slider`, `Switch`, `Checkbox`, `Bar` | Core LVGL controls |
| [x] | `Arc`, `Spinbox` | Core LVGL controls |
| [x] | `Dropdown`, `Roller`, `TextArea` | Text/selection controls |
| [x] | `Line`, `Image`, `Spinner` | Tactility spinner, because LVGL spinner is off |
| [x] | `Card`, `Chip`, `SliderBox` | Framework widgets |
| [x] | `List`, `Badge` | Framework widgets |
| [x] | `PageIndicator`, `StepIndicator` | Framework widgets |
| [x] | `IconButton`, `TextButton` | Icon-button variants exposed |
| [x] | `LED`, `Table`, `Scale`, `ImageButton` | Batch 2 display widgets |
| [x] | `Chart`, `Calendar` | Batch 2 display widgets |
| [x] | `Keyboard`, `ButtonMatrix` | Batch 2 input widgets |
| [x] | `Tabview`, `Tileview` | Composite widgets with page/tile helpers |
| [ ] | `TileGrid`, `PathBar`, `Files` | New firmware widgets; Lua bindings and designer support planned |
| [x] | Widget exclusion policy | Only 9.6 widgets that are enabled and not deprecated |
| [x] | `set{}` property dispatcher | 115 properties; see table below |
| [x] | Object states/flags/children | `add_state`, `remove_state`, `has_state`, `child_count`, `child` |
| [x] | `value()`, `text()`, `is_checked()`, `set_checked()` | Class-checked accessors |
| [x] | Text-area editing | `cursor`, `set_cursor`, `insert`, `delete_char` |
| [x] | Styles | `lvgl.Style`, `add_style`, `remove_style`, part/state selectors |
| [x] | Events | `on()`, `off()`, deferred dispatch |
| [x] | Timers | `lvgl.Timer`, pause/resume/delete |
| [x] | Input groups | `lvgl.Group`, add/remove, focus next/prev, freeze, editing, wrap, default, attach/detach keypads and encoders |
| [x] | Numeric animations | `lvgl.Anim`, `ANIM_PATH`, `ANIM_INFINITE`; C frame callbacks, no Lua frame callbacks |
| [x] | Toolbar | Title, navigation action, script text/icon actions on the runner toolbar, spinner |
| [x] | Flex layout | `flex_flow`, `flex_align`, `flex_grow`, padding/gap |
| [x] | Grid layout | `grid()`, `grid_cell()`, `GRID_CONTENT`, `GRID_FR` |
| [x] | Layout control | `layout = lvgl.LAYOUT.NONE/FLEX/GRID`; `NONE` enables absolute children |
| [x] | Theme tokens | Primary/secondary/error, spacing/size/mono/lerp |
| [x] | Constants | Align, flex, long mode, flags, opa, parts, states, symbols, table/scale/chart/keyboard/matrix |
| [x] | Scroll snapping and scrollbar API | `scroll_snap_x/y`, `scrollbar_mode`, `scroll_to/by/x` |
| [x] | Transform, shadow, outline, margin | Core style coverage complete |
| [x] | `min_width` / `max_width` | `min_height`/`max_height` too |
| [x] | `Menu`, `Win` | Excluded: deprecated in LVGL 9.6 |
| [x] | `Msgbox` | Excluded: intentionally disabled in the firmware sdkconfig |
| [ ] | `QR` | LVGL widget not bound |
| [ ] | Colorwheel | Removed in LVGL 9.6; nothing to bind |
| [x] | Image from package assets | PNG files resolve from `... .. "/assets/..."`; SVG icons are not decoded on device |
| [!] | Lua animation callbacks | Numeric properties animate from C; no per-frame Lua `exec`/completion callbacks yet |
| [!] | Input-device group assignment | Groups and focus work; assigning a specific encoder/keyboard to a group stays firmware-side |

### Property coverage

| Status | Properties |
|---|---|
| [x] | `align`, `bg_color`, `bg_opa`, `border_color`, `border_width`, `checked` |
| [x] | `flex_align`, `flex_flow`, `flex_grow`, `w`, `h`, `width`, `height`, `x`, `y` |
| [x] | `long_mode`, `min`, `max`, `one_line`, `opa`, `options` |
| [x] | `layout` (`NONE`/`FLEX`/`GRID`) |
| [x] | `pad_all`, `pad_top/bottom/left/right`, `pad_column`, `pad_row`, `password_mode`, `placeholder` |
| [x] | `margin_top/bottom/left/right`, `min_width/height`, `max_width/height` |
| [x] | `outline_width/color/opa/pad`, `shadow_width/color`, `border_opa/side` |
| [x] | `text_align`, `text_letter_space`, `text_line_space` |
| [x] | `transform_width/height`, `transform_scale_x/y`, `transform_rotation`, `transform_pivot_x/y` |
| [x] | `scroll_dir`, `scrollbar_mode`, `scroll_snap_x/y` |
| [x] | `shadow_ofs_x/y`, `shadow_opa`, `shadow_spread`, `bg_grad_color/dir` |
| [x] | `arc_start/end`, `arc_bg_start/end`, `arc_rotation`, `range`, `spin_step`, `digit_format` |
| [x] | `radius`, `src`, `text`, `text_color`, `text_font`, `value` |
| [x] | `brightness`, `led_color`, `on` (LED) |
| [x] | `rows`, `columns`, `column_widths`, `selected_cell` (table) |
| [x] | `tick_count`, `major_tick_every`, `label_show`, `scale_angle`, `scale_rotation`, `scale_mode`, `text_src`, `post_draw`, `ticks_on_top` (scale) |
| [x] | `chart_type`, `point_count`, `update_mode`, `div_lines`, `axis_range` (chart) |
| [x] | `today`, `shown_date`, `day_names` (calendar) |
| [x] | `textarea`, `keyboard_mode`, `popovers`, `set_textarea` (keyboard) |
| [x] | `map`, `ctrl_map`, `one_checked`, `selected_button` (button matrix) |
| [x] | `active_tab`, `tab_bar_position`, `tab_bar_size` (tabview) |

## 4. Capabilities (`tactility.*`)

| Status | Capability | Binding | Demo / test |
|---|---|---|---|
| [x] | `device` | hand-written | `tests/bindings.lua`, I2C scanner |
| [x] | `fs` | hand-written | `tests/bindings.lua` |
| [x] | `time` | hand-written | `tests/bindings.lua`, dashboard |
| [x] | `settings` | hand-written | `tests/bindings.lua` |
| [x] | `power` | hand-written | `tests/bindings.lua`, dashboard |
| [x] | `wifi` (read-only) | hand-written | `tests/bindings.lua`, dashboard |
| [x] | `log` | hand-written | `tests/bindings.lua` |
| [x] | `heap_used` | hand-written | dashboard |
| [x] | `raw.call` | hand-written | `tests/raw.lua` |
| [x] | `gpio` | generated (`gpio.toml`) | `tests/gpio.lua`, `Lua GPIO` |
| [x] | `i2c` | generated (`i2c.toml`) + custom | `tests/i2c.lua`, `Lua I2C Scan` |
| [x] | `notification` | hand-written | `tests/notification.lua`, `Lua Notifications` |
| [x] | `uart` | generated (`uart.toml`) + custom | `tests/uart.lua` |
| [ ] | `pwm` | — | |
| [x] | `adc` | generated (`adc.toml`) | `tests/adc.lua` |
| [x] | `spi` | generated (`spi.toml`) + custom | `tests/spi.lua` |
| [ ] | `i2s` / audio | — | Player/recorder clones |
| [x] | `display` / touch / backlight | hand-written; panel geometry, shape and brightness percentage | `tests/display.lua` |
| [ ] | `camera` | — | |
| [ ] | `gps` | — | GPS viewer clone |
| [ ] | `bluetooth` | — | |
| [ ] | `location` | — | Maps clone |
| [x] | `http` | Async worker task, ESP-IDF client + cert bundle, `request` handle | `tests/http.lua`, Weather app |
| [ ] | `crypt` | — | |
| [x] | `cjson` | `tactility.json.decode/encode`, null sentinel, 512 KB/64 depth | `tests/json.lua` |
| [ ] | `binfont` | — | Custom fonts |
| [ ] | `graphics` (PPA/SVG) | — | |
| [ ] | `service` | — | Battery/status services |
| [ ] | `bundle` / properties files | — | |
| [x] | `app` | hand-written | `tests/app.lua` |
| [x] | App lifecycle | list/start/stop/is_running/current/install/uninstall from Lua; stop refuses the running script, uninstall refuses the runtime |
| [x] | Dialogs / input dialog | `tactility.ui.alert/input/select`, native window-manager dialogs | `tests/dialogs.lua`; interactive flows manual |
| [x] | File picker | `tactility.fs.pick_file/pick_files/pick_folder` with a callback, served by the native file dialog | `tests/fs.lua` option errors, `tests/picker.lua` cancel/release; selection flow manual |
| [x] | Notification events | Runner timer + `notification_subscribe` | `subscribe(callback)`, `EVENT`, `tests/notification_events.lua` |
| [!] | Notification actions / Bundle data | — | Actions need Lua-callback lifetime design; Bundle needs a kernel key-enumeration API |
| [ ] | Clipboard | — | |
| [x] | Buffer marshalling (I2C) | hand-written helpers + `impl="custom"` | `tests/i2c.lua` read/write/write_read |
| [ ] | Buffer marshalling (UART/ADC/SPI) | reuse `lua_bindings_check_bytes`/`push_bytes` | Next capabilities |
| [x] | Persistent SPI device handles | `tactility.spi.open()` + methods | `tests/spi.lua` |
| [ ] | Typed raw signature database | — | Turn `raw.call` into a typed dispatcher |
| [ ] | Error mapping | — | `ERROR_UNDEFINED` → meaningful reasons |
| [-] | `power_supply_power_off()` | — | Deliberately not bound |
| [-] | Wi-Fi connect / disconnect | — | Credential policy first |

### Current `device` surface

`list`, `find`, `find_by_compatible`, `name`, `address`, `type`, `is_ready`,
`is_compatible`, `parent`, `children`, `child_count`, `release`.

### Current `fs` surface

`read`, `write`, `append`, `exists`, `is_file`, `is_directory`, `size`, `list`,
`make_directory`, `remove`, `mounts`, `user_data_path`, `pick_file`,
`pick_files`, `pick_folder`.

### Current `notification` surface

`send{ message, title?, severity?, sticky?, source?, service? }`, `list`, `get`,
`dismiss`, `dismiss_all`, `dismiss_non_sticky`, `count`, `SEVERITY`, `SOURCE`,
and `subscribe(callback)` with `EVENT` returning a handle with `cancel()` and
`is_active()`. Each event is `{ id, type, timestamp, order, bumped }`, where
`type` is `EVENT.POSTED`, `EVENT.DISMISSED`, `EVENT.CLEARED` or `EVENT.UPDATED`.

The runner delivers events from a 200 ms LVGL timer, so the callback runs on the
LVGL task; the handle is pinned until cancelled or the runtime closes. Events
posted before subscribing or after cancelling are not kept.

Notification actions and Bundle data are not exposed yet. Actions need a
Lua-callback lifetime design because a notification can outlive the script's
state, and the kernel Bundle has no key-enumeration API, so a Lua table cannot
be read back from it unaided.

## 5. App clones and demos

| Status | App | Needs | Notes |
|---|---|---|---|
| [x] | GPIO | `gpio` + UI | Matches the C app's 10-column block |
| [x] | I2C Scan | `i2c` + `device` + grid | Matches chips, counts and names |
| [x] | Dashboard | power/Wi-Fi/time/device | Theme-native cards |
| [x] | Widgets gallery | UI | Doubles as a widget regression |
| [x] | Notifications | `notification` | Send/inspect/dismiss from Lua |
| [x] | Designer demo | UI + `layout` | Generated by the PC designer |
| [x] | UI | UI | Theme-native cards and button |
| [x] | Game | UI + timer | Whack-a-mole |
| [x] | Clock / Hello | time / text | Text-result examples |
| [ ] | Calculator | UI only | Good next clone |
| [ ] | Files | `fs` + UI | Good next clone |
| [ ] | Settings / Appearance | `settings` + UI | Good next clone |
| [ ] | Todo | `fs`/`settings` + UI | Good next clone |
| [ ] | Snake / 2048 / Breakout | UI + timer | Games exercise events |
| [ ] | GPS viewer | `gps` + UI | Needs GPS capability |
| [ ] | IMU viewer | `device`/sensor API | Needs sensor capability |
| [ ] | Music player | audio + `fs` | Needs audio stream |
| [x] | Weather | `Scripts/weather`: `require`, HTTP, JSON, toolbar actions, geocoding/custom location, units/interval | Packaged example; 4 days on short screens |
| [ ] | App hub / installs | `http` + app lifecycle | Bigger scope |
| [-] | Camera / Doom | camera / heavy port | Not a Lua demo target |

## 6. Companion and editor

| Status | Item | Notes |
|---|---|---|
| [x] | Themed browser | Examples + script folder |
| [x] | Editor | Save, Run, Export, Back |
| [x] | New file | Input dialog |
| [x] | Folder picker | Persists the script folder |
| [x] | Save Copy | Bundled examples are read-only |
| [x] | On-device Export | Tar + install |
| [x] | Toolbar close | Emits the app close event |
| [x] | PC GUI designer | `tools/lua-gui-designer`; simulator profiles, Material/mono theme, system bars, tree drag/context, editable code, absolute + percentage + flex + grid layout, multi-select align/distribute, Lua export |
| [x] | POSIX / simulator package | `linux-x86_64` .so package; `catalog/simulator.md` |
| [x] | Designer settings persistence | Profile/theme/zoom/grid/snap/tab/window saved next to the tool |
| [x] | Lua highlighting / auto-indent | PC designer Code tab and event editor |
| [x] | On-device auto-indent / cursor position | `Ln X, Col Y` follows the cursor; Tab stays focus navigation |
| [x] | Responsive icon buttons | Both views use the dots overflow menu; the toolbar keeps the title |
| [x] | Large-script editor load | Direct label set (no cursor-to-end scan) and a no-kerning editor font; 25 KB opens in ~1.8 s instead of ~9.8 s on the T-Deck |
| [x] | 64-bit Lua integers | Device matches the simulator; storage totals no longer truncate |
| [x] | Storage tab shows all mounts | `tactility.fs.mounts()`; `/sdcard`, `/data`, ... |
| [x] | Module stop/start | Gates new runtimes; refuses stop while a script runs |
| [ ] | On-device syntax highlighting | Deferred; needs custom drawing |
| [ ] | Rename / delete | Browser currently lists only |
| [x] | Search / filter | Toolbar search toggles an inline App Hub-style row; live, case-insensitive filtering |
| [ ] | Syntax highlighting / line numbers | Editor is plain text |
| [x] | Runtime version display | Browser title shows the installed package version; the runner explains an API mismatch |
| [ ] | Multi-file project view | Runtime `require` is in; browsing/creating module files is not |

## 7. Tests

| Status | Test | Covers |
|---|---|---|
| [x] | `tests/luavgl_smoke.lua` | Objects, text areas, styles, events, timers |
| [x] | `tests/teardown.lua` | Widget cleanup and explicit delete before close |
| [x] | `tests/gpio.lua` | GPIO discovery, count, invalid pin |
| [x] | `tests/i2c.lua` | Controllers, probes, child addresses, block read/write/write_read |
| [x] | `tests/adc.lua` | ADC controller discovery, raw read and invalid channel |
| [x] | `tests/uart.lua` | UART discovery, is_open/available, config and closed error paths |
| [x] | `tests/spi.lua` | SPI discovery, constants, invalid CS, try_lock/unlock and a persistent handle |
| [x] | `tests/icons.lua` | Shared icon table, including the latest firmware additions |
| [x] | `tests/bindings.lua` | fs, time, device, settings, power, Wi-Fi |
| [x] | `tests/settings.lua` | Multi-key round trip, replacement, keys/has/remove/clear and repeated writes |
| [x] | `tests/dialogs.lua` | Native alert/input/select option validation and dismissing an alert |
| [x] | `tests/raw.lua` | Symbol lookup and missing symbol |
| [x] | `tests/grid.lua` | Grid tracks and cell placement |
| [x] | `tests/notification.lua` | Send, get, list, dismiss and missing id |
| [x] | `tests/notification_events.lua` | Subscribe, posted/dismissed/cleared order and cancel |
| [x] | `tests/picker.lua` | Async picker registration, `cancel()` and the `nil, "cancelled"` callback |
| [x] | `tests/require.lua` | Flat, nested, dotted and cached modules, missing module and cycle |
| [x] | `tests/json.lua` | Decode/encode, arrays, null, mixed keys, NUL, depth limit |
| [x] | `tests/http.lua` | HTTPS GET to Open-Meteo plus JSON decoding |
| [x] | `tests/app.lua` | App list (hidden apps included), running state, current app, start/stop error paths and the current-app stop guard |
| [x] | `tests/object_api.lua` | Core sizing/margins/outline/shadow/text/transforms/states/flags/children, Arc and Spinbox |
| [x] | `tests/widget_batch2.lua` | LED, Table, Scale, ImageButton, Chart, Calendar, Keyboard, ButtonMatrix |
| [x] | `tests/widget_batch3.lua` | Tabview, Tileview |
| [x] | `tests/assets.lua` | Install-directory argument, PNG asset in `Image` and `ImageButton` |
| [x] | `tests/group.lua` | Input group count/order, focus next/prev/freeze, wrap/edit/default |
| [x] | `tests/animation.lua` | x/value/colour animations, stop, path constants, target deletion |
| [x] | Designer self-test | `designer.py --self-test` (chrome geometry, mono, model, flex/grid layout, JSON, export, tree moves) |
| [x] | Designer UI test | `designer.py --ui-test` drives every control, tab, widget, property and tree drag |
| [x] | Screenshot UI regression | `visual_test.py`; headless `render_image()` goldens for Tab5 landscape/portrait, split/side bars, dark/light/mono |
| [x] | `catalog.py --check` in CI | IDL drift against the SDK |
| [ ] | Close-path test | Open/close every packaged demo |
| [ ] | Soak / memory test | Long-running scripts |

## 8. Known limitations

| Status | Limitation | Workaround / next step |
|---|---|---|
| [!] | Re-applying `grid()` after layout corrupts teardown | Build one card per layout; toggle `HIDDEN` |
| [!] | Runtime module cannot be replaced while an app uses it | Close the app (or reboot) before install |
| [!] | Script package embeds its runner | Repackage after a Lua ABI change; 0.1.24 moved integers to 64-bit |
| [!] | No sandbox | A script has ELF-app access |
| [!] | No writable `/tmp` | `io.tmpfile`/`os.tmpname` unusable |
| [!] | `requires.ram=1M` | PSRAM required |
| [!] | LVGL 9.6 drift | Keep the export/widget surface deliberate; removed/deprecated/disabled widgets stay excluded |
| [!] | `lv_obj_is_valid()` is deprecated | It now walks the parent chain and faults on freed memory; use the runtime live set |
| [!] | `raw.call` is untyped | Typed signature DB planned |
| [x] | Notification events | `tactility.notification.subscribe` on the LVGL task |
| [!] | Notification actions/data | Design: [catalog/notification-actions.md](catalog/notification-actions.md) |
| [!] | I2C scanner scans all buses at startup (~3 s) | Pre-built cards keep switching/teardown stable |
| [!] | I2C scanner does not persist the selected bus | Add a small settings entry |

## 9. Catalog and CI

| Status | Item | Notes |
|---|---|---|
| [x] | `tools/sdk-catalog/catalog.py` | Parses firmware exports (`symbols.c` + `module.cpp`) and SDK headers |
| [x] | `tools/symbol-audit/symbol_audit.py` | Compares runner undefined symbols with module export tables; `--fix` |
| [x] | Generated coverage report | `catalog/sdk-coverage.md` |
| [x] | Machine-readable inventory | `catalog/sdk-symbols.csv`, `.json` |
| [x] | `tools/api-docs/generate.py` | Searchable reference from the bindings, doc comments and constants; `--check` runs in CI |
| [x] | Generated API reference | `catalog/api/index.html` + `api.json`, published at `/api/` |
| [ ] | CI `--check` | Fail when an IDL symbol disappears or a runner symbol is unexported |
| [ ] | SDK-bump diff | Highlight new/unbound exports |

## Regenerating the SDK report

```powershell
py Apps\LuaRuntime\tools\sdk-catalog\catalog.py
py Apps\LuaRuntime\tools\sdk-catalog\catalog.py --check
```

The generator uses the firmware tree for exact export tables and falls back to
SDK headers for modules without one. See
[`catalog/README.md`](catalog/README.md).

## Regenerating the API reference

```powershell
py Apps\LuaRuntime\tools\api-docs\generate.py
py Apps\LuaRuntime\tools\api-docs\generate.py --check
```

The page is assembled from `bindings/*.toml` and the `luaL_Reg` tables in the
runtime and runner, so it cannot list a function that the code does not have.
Doc comments above the C functions become descriptions. The Pages workflow
copies it to `/api/`.
