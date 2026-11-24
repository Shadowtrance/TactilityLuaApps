# Lua Runtime as an External Module Package — Plan

Status: proof + proper multi-binary package + Lua-drawn UI verified on hardware;
companion editor, bundled examples, folder picker and New-file flow verified
(2026-11-21)
Date: 2026-11-21
Firmware reference: `C:\Users\teron\espidf-projects\TactilityWork` (`b2ab8cf1` + wt32)
Apps reference: `C:\Users\teron\espidf-projects\TactilityApps`
Old prototype for salvage: `C:\Users\teron\Documents\GitHub\Tactility` branch `lua-poc-wip`, plus `OLD_LUA_STUFF/`

Package: `C:\Users\teron\espidf-projects\TactilityApps\Apps\LuaRuntime`

### Proof status (2026-11-19)

Verified on a Tab5 (ESP32-P4), stock SDK, no Lua in the firmware:

- External module package installs, starts, and registers a launcher app.
- The module registers a memory app through `app_manager_add()`; it draws a
  real window-manager toolbar and label.
- The module carries Lua 5.5 and runs `/sdcard/tactility/lua/hello.lua`.
- The script reached the kernel and printed:
  `devices: 43`, `battery: 98%`, `mounts: /sdcard`, `uptime: 195 s`.
- The package is ~205 KB; the module ELF is ~197 KB.
- `ui.lua` draws a complete card, labels, a button and a click handler entirely
  through the `luavgl` bindings. This was ported to LVGL 9.6 with one API rename
  (`lvgl_get_shared_icon_font` → `lvgl_get_shared_icon_default_font`) and 44
  additional LVGL exports in the firmware.
### Architecture revision (same day)

The first pass put `luavgl` inside the runtime module. That made the module
reference `lv_*` at relocation time, which forced a firmware boot-order change
to load installed modules after `lvgl::start()`.

That change is now **reverted**. The final split:

- `lua_runtime` module — Lua 5.5, `tactility.*` bindings, exported Lua C API.
  No LVGL references, so it loads at boot like Mystify's driver module.
- `lua` app — the script browser/companion.
- `lua_runner` app — hidden; carries `luavgl`, loads after LVGL, and resolves
  `lua_*` from the module at app-load time.

The launcher grid is stable: loose scripts are not launcher apps, and the
module no longer registers per-script apps. Packaged Lua apps (manifest +
`lua/main.lua`) are the future path for launcher entries.

Current firmware exports the generic LVGL, kernel and libc/libm symbols the
external runtime needs, so no firmware patch is required.

Package files of interest:

- `manifest.properties` — v3 package: browser app, hidden runner app, runtime module.
- `lua_runtime/main/Source/main.c` — module entry; no LVGL.
- `lua_runtime/main/Source/symbols.c` — runtime API + full Lua C API exports.
- `lua_runtime/main/Source/lua/` + `Include/lua` + `Private/lua` — runtime and
  `tactility.*` bindings.
- `lua_runtime/components/lua/` — vendored Lua 5.5.
- `lua/main/Source/main.c` — script browser.
- `lua_runner/main/Source/main.c` — hidden script runner (window, console).
  Runs a loose script from its argument, or, when it has none, treats itself as
  a packaged app and runs `<install>/lua/main.lua`.
- `tools/lua-app/make_lua_app.py` — assembles an installable Lua app package
  from a script and the built runner.
- `lua_runner/main/Source/luavgl/` — LVGL 9.6 bindings (widgets, styles,
  events, timers, toolbar).
- `assets/examples/hello.lua`, `assets/examples/clock.lua` — text-result scripts.
- `assets/examples/ui.lua` — a complete Lua-drawn UI.
- `assets/examples/dashboard.lua` — live system dashboard.
- `assets/examples/game.lua` — whack-a-mole with events and a timer.
- `lua/main/Source/main.c` — C companion: themed browser, text editor,
  Save/Run/Back, folder picker and the New-file flow. Bundled examples are
  listed from the package assets.
- `BINDINGS.md` — design for the capability API + generated raw signature
  database, the IDL, and the SDK update workflow. Supersedes the old
  generated-wrapper approach.

Separate, unrelated firmware bug: `tt shell "mkdir ..."` reliably reboots the
Tab5. `ls`, `date`, and file pushes work. Avoid `mkdir` for now; use an
existing directory or `tt push` to a path that already exists.

---

## 1. Goal

Ship a Lua runtime that:

- runs on **stock firmware** from the webflasher — no firmware fork, no firmware patch;
- costs **zero firmware flash**, because the interpreter lives in an installable package on SD;
- gives scripts the **same SDK access as a C app**, because the runtime is an SDK-built ELF module, not a reimplementation;
- lets users install scripts and run them **without compiling anything**;
- is maintainable as **"rebuild the package for each SDK release"**, not "rebase a firmware fork".

Non-goals for v1:

- 4 MB / no-PSRAM devices. Excluded through `requires.ram` / `requires.device.id`.
- Bundling Lua in the firmware image.
- Sandboxing scripts. They have the same trust level as ELF apps.
- Bytecode distribution. Ship source.
- Hybrid ELF + Lua packages.

---

## 2. Why this works on current firmware

These are facts from the current tree, not assumptions:

| Mechanism | Where |
|---|---|
| Packages may contain kernel modules: `module.0.binary=main` | `Buildscripts/TactilitySDK/NewProject/module/manifest.properties` |
| `tt app create` has a `module` project type | `Buildscripts/TactilitySDK/Tools/app.py` |
| Module binaries are loaded at install and on boot, and stay loaded until uninstall | `Modules/app-module/source/install.cpp`, `Modules/app-module/source/manager.cpp` |
| Module `main()` returns `struct Module*`; loader calls it directly | `Modules/app-esp32-module/source/app_esp32_loader_service.cpp` |
| Module symbols are globally resolvable from `module_add()`, even before `start()` | `TactilityKernel/source/module.cpp` |
| External modules resolve the same firmware symbols as any ELF app | `app_esp32_symbol_resolver()` in `Modules/app-esp32-module/source/module.cpp` |
| `app_manager_add` / `app_manager_remove` / `app_manager_add_package` are exported to apps | `Modules/app-module/source/module.cpp` (`SYMBOLS[]`) |
| Memory apps call a function pointer and can use the window manager | `Modules/app-module/source/internal_loader.cpp`, `Buildscripts/TactilitySDK/NewProject/gui/main/Source/main.c` |
| Packages install under `CONFIG_TT_ROOT_PATH` — on this build `/sdcard` | `TactilityKernel/source/paths.cpp`, `sdkconfig` |
| Device gating exists: `requires.ram` (total internal+external MB), `requires.device.id`, `target.platforms` | `Modules/app-module/source/package_compatibility.cpp` |
| A module-only package is valid: zero `app.N.*` entries is accepted | `Modules/app-module/source/package_manifest_parsing_v3.cpp` |

The consequence: **the interpreter does not need to be in firmware.** It can be a module in a normal `.app` package, built against the public SDK, resolving the same symbols C apps resolve.

---

## 3. Architecture

### 3.1 Package layout

```
lua-runtime.app                 (USTAR tar, installed like any app)
  manifest.properties
  bin/<platform>/lua_runtime.elf   (core module: Lua + tactility bindings, no LVGL)
  bin/<platform>/lua.elf           (script browser app)
  bin/<platform>/lua_runner.elf    (hidden runner app: window + luavgl)
```

Manifest (v3):

```properties
manifest.version=0.3
id=tactility.luaruntime
version.name=0.1.0
version.code=1
target.sdk=<sdk version>
target.platforms=all
requires.ram=1M
app.0.id=tactility.luaruntime
app.0.name=Lua
app.0.binary=lua
app.1.id=lua.runner
app.1.name=Lua Script
app.1.binary=lua_runner
app.1.hidden=true
module.0.binary=lua_runtime
```

`requires.ram=1M` is a first cut: it excludes 4 MB/no-PSRAM boards (about 520 KB total RAM) and admits PSRAM boards. Confirm the threshold from measurements in step 5.

### 3.2 Boot / install flow

1. Package is installed with the existing app install path (app hub or `tt app install`).
2. `app_install()` extracts to `<root>/tactility/app/tactility.luaruntime/`.
3. `app_module_loader_load_package()` loads `bin/<platform>/lua_runtime.elf`, calls its `main()`, gets `struct Module*`, constructs/adds/starts it. The module has no LVGL references, so loading before `lvgl::start()` is fine.
4. The module registers no apps. It exports the runtime API and the Lua C API through `symbols.c`.
5. The launcher shows the `Lua` browser app. Selecting a script starts the hidden `lua.runner` app with the script path as `argv[0]`.
6. `lua_runner` loads after LVGL (it is an app), resolves `lua_*` from the module, creates a window with `window_manager_create()`, runs the script, and returns when it receives `APP_EVENT_CLOSE`.

### 3.3 Script app model

Each script is a normal app entry backed by a memory app manifest:

```c
static AppManifest script_manifest = {
    .id = "lua.hello",
    .name = "Hello",
    .category = APP_CATEGORY_USER,
    .location = { APP_LOCATION_MEMORY, (void*)script_main },
    .stack = { .depth = 8192 },
};
```

`script_main(int argc, char** argv)`:

```c
AppInstanceId id = app_scheduler_current_app_id();
WindowId window = window_manager_create(id, create_widgets, script_path);
// event loop until APP_EVENT_CLOSE, same shape as NewProject/gui
window_manager_remove(window);
return 0;
```

`create_widgets(parent, script_path)` is where Lua gets its root object and draws.

This is better than the old `on_show`/`on_hide` model: the app framework owns the window and the close event, so the old LVGL deadlock path around `tt_app_stop()` does not apply.

### 3.4 Reference rules

- `app_manager_add()` stores a **non-owning pointer**. Manifests must be static in the module and outlive registration.
- The module must keep a registry of manifests and app ids so it can `app_manager_remove()` on `stop()`/uninstall.
- `module_stop()` only sees module references, not memory apps. The module must track running script apps and return `ERROR_RESOURCE_BUSY` from `stop()` while any are alive, or the package could unload code under a callback.
- Widgets/timers created by Lua outlive `onHide` in the old model. In the new model, tear down the Lua state before `window_manager_remove()` finishes, and detach callbacks before freeing the state.

---

## 4. Device gating

Constraints to encode in the manifest:

- `requires.ram`: total RAM (internal + external), in whole MB.
- `requires.device.id`: comma-separated device ids, if an explicit allowlist is safer.
- `target.platforms`: `all`, or a list.

Suggested v1 policy:

- `requires.ram=1M` (or `2M` after measuring).
- Keep a tested-device list in the catalog, even if the manifest is broader.
- Do not add a `requires.psram` field for v1; that is a firmware change. `requires.ram` is a good enough proxy because no-PSRAM ESP32/S3 boards are ~0.5 MB total.

Note: a 4 MB flash board with PSRAM passes `requires.ram`. That is fine if it actually runs; measure it.

---

## 5. Minimal proof path

Each step has a go/no-go. Do not build the full runtime before step 2 is proven.

### Step 0 — Baseline (30 min)

- Pick one PSRAM target, e.g. `esp32s3`.
- Confirm `tt app build` and `tt app install` work for a normal app on stock firmware.
- Record firmware version, SDK version, free heap/PSRAM.

**Done when:** a normal app installs and runs from the current firmware.

---

### Step 1 — Hello module package (half a day) — done

**Status: done.** `tt app create` fell back to the GUI template when its prompts
were piped, so the proof copied `Buildscripts/TactilitySDK/NewProject/module`
directly. The package installed and the log showed `module: start luaruntime`.
This step was done as a single-binary proof; the final package is the
multi-binary `Apps/LuaRuntime` layout.

The initial proof used the SDK's module template directly. The final package is
multi-binary, so the build is:

```bash
cd Apps/LuaRuntime
tt app build -a esp32p4
tt app install --host <device-ip>
```

The template already does:

```properties
manifest.version=0.3
module.0.binary=main
```

and its `main.c` returns a `Module*` whose `start()` prints `Module started`.

**Done when:**
- install succeeds;
- device log shows `Module started`;
- after reboot, it starts again;
- uninstall removes it.

**If it fails:** check the binary landed at `bin/<platform>/main.elf`, and that `target.sdk` is not newer than firmware.

**Why this matters:** it proves the entire "external runtime, no firmware patch" premise with almost no code.

---

### Step 2 — Register one memory app from the module (1–2 days) — done

**Status: done.** `app_manager_add()` works from the module's `start()`, the app
appears in the manager, `shell.run(host, port, "lua.proof")` starts it, and the
window manager draws it. Register → launch → render → close all passed.

One crash was caused by the proof harness itself: an auto-launch thread called
`app_manager_stop()` on a possibly stale instance after another app had taken
the foreground, and `stop()` removed the manifest while the app could still be
running. The harness was removed; `stop()` now stops a tracked instance before
removing the manifest.

Add to the module:

1. A static `AppManifest` (id `lua.proof`, name `Lua Proof`).
2. `app_manager_add(&manifest)` in `start()`, `app_manager_remove()` in `stop()`.
3. A trampoline `script_main()` that:
   - reads `app_scheduler_current_app_id()`;
   - calls `window_manager_create(id, create_widgets, NULL)`;
   - draws one label;
   - waits for `APP_EVENT_CLOSE` and returns.

Build, install, reboot.

**Done when:**
- `Lua Proof` appears in the launcher;
- opening it shows the label;
- closing returns to the launcher cleanly;
- relaunch works repeatedly;
- uninstalling the package removes the launcher entry.

**Unknowns this resolves:**
- Does the launcher refresh after `app_manager_add()` at runtime, or only after reboot/relaunch? If only after reboot, that is acceptable for v1 but must be documented.
- Does a memory app get a usable `app_scheduler_current_app_id()` and window?
- Does `app_manager_add()` at module `start()` run at a safe point in boot?

**Fallback if dynamic registration is too fragile:** ship one host app (`Lua`) with a script browser/editor, like the old `LuaEditor`. That still meets "no firmware compile, no 4 MB tax"; only the per-script launcher entries are lost.

---

### Step 3 — Embed Lua and run a script (2–4 days) — core done

**Status: done.** Lua 5.5 and the ported `tactility.*` bindings live in the
runtime module (`ELF_COMPONENTS "lua"`). The `luavgl` bindings live in the
hidden runner app, which loads after LVGL. A script on SD reads
devices/power/fs/time; scripts with no widgets get their `result` drawn by C.

The external-module constraint surfaced six libc imports the firmware did not
export: `tmpfile`, `tmpnam`, `ungetc`, `setlocale`, `strtof`, `log2f`
(`tmpfile` was found first; the rest came from `check-app-symbols.py`). They are
now exported generically from the firmware's `c-symbols` module, so the runtime
module uses the real implementations instead of local fallbacks and no local
patch is needed. Verified on-device: `io.read("*n")` pushes back correctly,
`os.setlocale()` returns `"C"`, `math.log(8, 2)` works, and `os.tmpname()`
returns a name. `io.tmpfile()` still depends on a writable `/tmp`, which this
filesystem does not mount; that is a system limitation rather than a missing
symbol.

Future libc gaps follow the same rule: add a generic export upstream when more
than one app needs it; otherwise keep a module-local shim.

Two API drifts from the old prototype also had to be fixed: the kernel
`file_mutex` API is gone (plain POSIX I/O for now), and `service_paths_*` is not
reliably active, so settings live under `paths_get_data_path()/lua`.

- Vendor `Libraries/lua` from the old branch (self-contained, Lua 5.5).
- Link it statically into the module.
- In `create_widgets(parent, path)`, allocate a `lua_State`, open the bindings, run the script with `parent` as the LVGL root.
- Start with `tactility.log`, `tactility.fs`, `tactility.time`, `tactility.device`.
- Script location for the proof: `/sdcard/tactility/lua/main.lua`.

**Done when:**
- editing `main.lua` and relaunching the app changes the UI;
- no reboot, no compile, no reinstall;
- script errors show on screen or in the log with a traceback.

**Measure now:**
- module ELF size;
- runtime heap after open (`lua_runtime_get_memory_used()`);
- total PSRAM/internal heap before/after.

---

### Step 4 — One launcher app per script (2–4 days) — done

**Status: done.** The module scans `/sdcard/tactility/lua/*.lua` at start and
registers one memory app per file. Two scripts (`hello.lua`, `clock.lua`) produce
`lua.hello` and `lua.clock`; both start from `shell.run()` and draw their own
result. Install/replace stops the running script, then reloads and rescans.

Design notes:
- Bounded static table (`MAX_SCRIPT_APPS = 8`) because `app_manager_add()` keeps
  non-owning manifest pointers.
- One trampoline per slot, because the app framework does not expose the
  manifest id from an instance id. Each manifest points at its own slot.
- Ids are `lua.<sanitized stem>`; invalid stems are skipped, not renamed.
- `stop()` stops the tracked foreground instance before removing manifests.

- Scan `/sdcard/tactility/lua/*.lua` in `start()` (or on demand).
- For each script, fill a static manifest entry (bounded, e.g. 16 scripts) and call `app_manager_add()`.
- Ids: `lua.<filename>` with a validated charset.
- Remove manifests on `stop()`/uninstall.
- Optional: `app_manager_add_package()` with a synthetic package so scripts are grouped and can be enumerated.

**Done when:**
- dropping `foo.lua` adds a `foo` app after rescan/reboot;
- deleting it removes the app;
- uninstalling the runtime package removes all script apps;
- no duplicate-id failures across reboots.

**Unknowns:** max scripts, id length (`APP_MANIFEST_ID_LENGTH` is 32), and whether the launcher listens for package add/remove events.

---

### Step 5 — Gate and measure (1 day)

Test on:

- one PSRAM board (the main target);
- one 4 MB/no-PSRAM board (expected to be excluded).

Record:

- package install size on SD;
- module load time;
- runtime heap and peak;
- app open/close time;
- behavior when the runtime package is updated while a script is open.

Set `requires.ram` and `requires.device.id` from the numbers.

**Done when:** the manifest admits the devices that work and rejects the ones that don't, with a clear install-time message.

---

## 6. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Launcher doesn't refresh on runtime registration | Accept reboot/relaunch for v1, or call the app hub/launcher refresh; fallback to a host app |
| `app_manager_add` keeps a raw pointer | Static manifests in the module; remove before stop; never reallocate |
| Package uninstalled while a script runs | Track live script apps; return `ERROR_RESOURCE_BUSY` from `module_stop()` |
| Module unloaded under callbacks | Tear down Lua/LVGL state before stop; block stop while apps are alive |
| Missing firmware symbols (module fails to load) | Add a package-time `nm -u` check against the SDK's exported symbol lists; fix generic gaps upstream |
| RAM on small devices | `requires.ram` gate; measure in step 5 |
| SDK ABI drift | Rebuild the package per SDK release in CI; `target.sdk` blocks mismatches at install |
| LVGL drift | Keep the Lua-visible widget/property surface small; port `luavgl-module` deliberately |
| Scripts break across runtime versions | Version the Lua API; keep the surface small; document breaking changes |
| Security | Same trust model as ELF apps; no sandbox; review scripts like apps |

---

## 7. What to salvage from the old WIP

Ported (2026-11-21):

- `Libraries/lua/` — vendored Lua 5.5 in `lua_runtime/components/lua`.
- `Modules/lua-module/source/runtime.cpp` — allocator, tracebacks, eval/eval_file.
- Kernel bindings `log`, `device`, `fs`, `settings`, `time`, `power`, `wifi`,
  including the `nil, reason` and handle patterns.
- `Modules/luavgl-module/` — widgets, `set{}` property dispatcher, styles,
  events, timers and the toolbar, ported to LVGL 9.6 in `lua_runner`.
- The Lua C API export idea (`generate-symbols.py`) — implemented as the
  runtime module's export table, so side-loaded apps share one interpreter.
- Regression coverage: the old C++ `luavgl` suite is replaced by on-device Lua
  scripts (`tests/`). The text-area teardown path is covered explicitly.

Ported through the generator:

- **GPIO**: `bindings/gpio.toml` + `Apps/LuaRuntime/tools/lua-bindings` produce `tactility.gpio`
  at build time (count, acquire, release, level, flags, pin number). The old
  `gpio_generated.cpp` was the reference for the descriptor API.
- The `bindings_private.h` GPIO declaration is now implemented by that glue
  (`lua_bindings_check_device` is shared; the handle lives in generated code).

Still to port:

- The rest of `Tests/lua-module` (fs, settings, time, power/wifi, runtime).
- The raw signature database/extractor for the long tail; see §8.
- Interrupt/callback GPIO APIs (need a callback lifetime policy).

Drop / rewrite:

- `Tactility/Source/app/LuaApp.cpp`, `AppManifest`/`AppInstance` changes,
  `Tactility.cpp` module start, `release-sdk.py` changes. Those are tied to the
  old in-firmware design.
- `OLD_LUA_STUFF/tactility_lua.py` packaging. Current `tt app` handles module
  packages.
- The old `on_show`/`on_hide` app glue. The new model is `main()` +
  `window_manager_create()`.

---

## 8. Binding strategy

Two tiers, so "parity" does not mean "hand-write 386 wrappers":

### Tier 1 — generated typed trampoline (coverage)

- Parse the SDK headers with libclang at package build time.
- Emit one descriptor row per function: `{ name, signature, address }`.
- One dispatcher handles scalars, `error_t`, out-parameters, handle types, and the `nil, reason` convention.
- Expose `tactility.raw.call("fn_name", ...)` for anything not curated.

This gives broad coverage with data, not generated C per function. Signatures that cannot be expressed (callbacks, structs by value, ownership) are marked unsupported and stay hand-written.

### Tier 2 — curated ergonomic layer (daily use)

- Hand-write the 10% that matters: device discovery, handles, fs, settings, time, power, wifi, gpio, i2c/uart/pwm, and the LVGL widget tables.
- Keep names and conventions stable; document them.

The old `generate-bindings.py` proves the idea. The upgrade is:
1. signatures extracted from headers instead of hand-annotated;
2. a descriptor table + one dispatcher instead of per-function wrappers;
3. generated at package build time, so an SDK bump is a rebuild, not a port.

**Status:** the current bindings are still hand-written. The next maintainability
step is to bring `generate-bindings.py` and its `.signatures` files into this
package for the driver-style APIs (GPIO first), keeping curated tables for
handles and lifetime rules. That makes an SDK signature change a regeneration
instead of a wrapper rewrite, which is the problem the old in-firmware version
had.

---

## 9. Milestones after the proof

- **M1 — Runtime host**: module package, script folder, log/fs/time/device bindings, error UI.
- **M2 — Binding pipeline**: libclang extraction, descriptor trampoline, `tactility.raw`, curated core.
- **M3 — UI**: port `luavgl-module` to the window manager model; core widgets, styles, events, timers.
- **M4 — Launcher integration**: per-script apps, add/remove, package grouping, refresh.
- **M5 — Release engineering**: per-platform CI, SDK-versioned builds, install/update flow, catalog metadata.
- **M6 — Distribution (optional)**: script packages with metadata/icons vs plain `.lua` files.

---

## 10. Decisions to make

1. **Source home**: decided — `Apps/LuaRuntime/` in this repo. The package is an app artifact; the app repo is the natural home.
2. **License**: TactilityApps defaults to GPLv3; the old modules are Apache-2.0; Lua and luavgl are MIT. Pick deliberately before publishing.
3. **Script distribution**: folder on SD (simplest) vs `.app` packages per script (launcher metadata, icons, versions).
4. **Target devices for v1**: one PSRAM board, then a small allowlist.
5. **Host app vs dynamic apps**: try dynamic first (step 2); keep the host app as fallback.
6. **Rebuild policy**: rebuild on every SDK release, or pin to a known-good SDK and update on demand.

---

## 11. Open questions

- Does the launcher refresh after `app_manager_add()` at runtime? (Step 2)
- Is `app_manager_add()` safe during `module_start()` on the boot scan thread? (Step 2)
- Does `app_manager_install_path_scan()` ever remove dynamically registered apps? It tracks scanned packages only, but verify. (Step 4)
- How much PSRAM does the module ELF consume after relocation on the target? (Step 3/5)
- Does the ELF loader load module code to PSRAM on this board (`CONFIG_ELF_LOADER_LOAD_PSRAM`)? (Step 5)
- Are all SDK functions the runtime needs actually exported by firmware? Run the `nm -u` check. (Step 3)
- What is the real max script count before launcher/manager limits bite? (Step 4)

---

## 12. Immediate checklist

- [x] Run step 0: confirm normal app build/install on the chosen device.
- [x] Run step 1: module package builds, installs, starts (`module: start luaruntime`).
- [x] Run step 2: register one memory app; launch, render, close all pass.
- [x] Decide: dynamic apps vs host app — dynamic registration works.
- [x] Run step 3 core: Lua 5.5 runs on-device and reaches kernel bindings.
- [x] Flash the firmware with the `registerInstalledApps()` boot-order fix and
      verify `lua.hello` survives a reboot without reinstall.
- [x] Run step 4: scan the scripts folder and register one app per script.
- [x] Port `luavgl-module` so scripts draw their own widgets.
- [x] Split `luavgl` out of the module into a hidden `lua_runner` app, so the
      module loads at boot like Mystify's driver module and the boot-order
      firmware change is unnecessary.
- [x] Add a companion "Lua" app that lists loose scripts and starts the runner.
- [x] Replace the fragile Lua editor with a C companion editor: themed browser,
      text area, Save/Run/Back. The old `LuaEditor` script is retired; its
      lifetime bug in `luavgl` teardown was the crash source.
- [x] Add a folder picker to the companion: `tactility.fileselection` with
      `--folder`, a stream result, and `script_dir` persisted in
      `runtime.properties` in the app's user data directory.
- [x] Correct the paths: settings live in
      `<data>/appdata/tactility.luaruntime/`; scripts default to
      `<storage root>/lua` (`/sdcard/lua`, or `/data/lua`), not `<data>/lua`.
- [x] Add packaged Lua apps: a package carries the built `lua_runner` ELF plus
      `lua/main.lua`, so a real app gets a launcher tile, icon, version and
      uninstall. The runner resolves its own install directory when started
      without a script argument, and passes it to the chunk as `...`.
      `Apps/LuaRuntime/tools/lua-app/make_lua_app.py` assembles and installs these packages
      without compiling; verified with a packaged Dashboard and the launcher
      tile.
- [x] Add runtime API metadata: packaged apps declare `lua.runtime.api=N`; the
      module exposes `_LUA_RUNTIME_API`, and the runner shows an update message
      when the module is too old. Verified with a package requiring API 99.
- [x] On-device export: the editor's **Export** button writes a tar package with
      the built runner, icons, manifest and the current script, then calls
      `app_install()`. No firmware change and no PC.
- [x] Add on-device `luavgl` regression scripts (`tests/luavgl_smoke.lua`,
      `tests/teardown.lua`). The object lifetime code that caused the old
      teardown fault (delete callbacks outliving the Lua state) is already
      hardened; the text-area clean/close sequence now completes without a
      panic.
- [x] Add richer examples: `dashboard.lua` and `game.lua`, bundled in the package.
- [x] List bundled examples directly from the package assets, next to the
      selected folder's scripts. Examples are read-only; Save/Run writes a copy
      into the selected folder first.
- [x] Add a **New** file flow: `tactility.inputdialog` asks for a name, the
      companion creates `<script_dir>/<name>.lua` and opens the editor.
- [ ] Add a second target (esp32s3 / esp32s31) to `target.platforms`.
- [ ] Pinned for later, after everything else: a POSIX/simulator build of the
      runtime and bindings, so scripts can be developed on a desktop.
- [x] Upstream the export additions for LVGL, kernel and libc/libm; the
      temporary `firmware-*.patch` files were removed. Recreate a patch only if
      a later firmware drops an export.
- [x] Add the capability generator (`Apps/LuaRuntime/tools/lua-bindings`) and generate the
      `tactility.gpio` glue from `bindings/gpio.toml` at build time. It covers
      count/acquire/release/level/flags/pin number, with handle and enum
      plumbing. Verified on the Tab5.
- [x] Add the low-level `tactility.raw.call(name, ...)` escape hatch: it
      resolves exported symbols at runtime (`module_resolve_symbol_global`) and
      calls them with pointer-sized arguments, including capability handles.
      Verified with `lv_tick_get`.
- [x] Add the `tactility.i2c` capability from `bindings/i2c.toml`: probe and
      8/16-bit register access. The generator now supports `c_args` so an out
      parameter can be placed before a trailing timeout. Verified on the Tab5
      (3 controllers, 8 devices on `i2c0`).
- [ ] Investigate the GPIO app's 31/32 (internal I2C) activity difference. The
      Lua timer is 100 ms and does run; sampling those pins at 10 ms still
      returns steady `true` through `gpio_controller_get_level`, while the C
      app shows activity. Compare the C app's controller/timer and the level
      semantics for I2C-owned pins.
- [x] Add buffer marshalling helpers (`lua_bindings_check_bytes`/`push_bytes`)
      and I2C block transfers through the IDL `impl = "custom"` hook. Verified
      with `tests/i2c.lua` (block read on `0x10`, failure paths for read/write/
      write_read).
- [ ] Add buffer-based bindings for UART, ADC and SPI.
- [x] Add Tactility's framework widgets: `Card`, `Chip`, `SliderBox`, `List`,
      `Badge`, `PageIndicator`, `StepIndicator`, `IconButton` and `TextButton`,
      with chip text/checked state and slider-box value handling. Verified with
      `assets/examples/widgets.lua`.
- [x] Add grid layout support (`obj:grid(columns [, rows])`,
      `child:grid_cell(...)`, `lvgl.GRID_CONTENT`, `lvgl.GRID_FR()`,
      `lvgl.GRID_ALIGN`) with descriptor ownership tied to the widget lifetime.
      Verified with `tests/grid.lua`.
- [x] Expand the core object/style API: min/max sizing, edge margins and
      padding, outline, shadow width/colour, text alignment/spacing, transforms,
      scroll direction/scrollbar mode, states, flags, child access and
      foreground order. Verified with `tests/object_api.lua`.
- [x] Add `Arc` and `Spinbox` widgets, including arc angles/rotation and
      spinbox range/step/digit format.
- [x] Re-added the remaining style setters after the firmware rebuild and
      verified them in `tests/object_api.lua`: shadow offsets/opacity/spread,
      gradients, scroll snapping, transform height, `scroll_to/by/x` and
      `move_background`.
- [x] Add widget batch 2: `LED`, `Table`, `Scale`, `ImageButton`, `Chart`,
      `Calendar`, `Keyboard` and `ButtonMatrix`. Table counts are applied before
      widths/selection and the LED's `on` before `brightness`, because Lua table
      iteration order is undefined. `lvgl.spinner_image()` supplies a built-in
      image descriptor for `ImageButton` until package assets exist. Verified
      with `tests/widget_batch2.lua` (26 checks). Colorwheel was removed in
      LVGL 9.6, so there is nothing to bind.
- [x] Add widget batch 3: `Tabview` and `Tileview`, including page/tile
      creation and tab navigation. Verified with `tests/widget_batch3.lua`.
- [x] Adopt an explicit widget exclusion policy: bind only what exists in LVGL
      9.6, is enabled in the firmware, and is not deprecated. Colorwheel was
      removed upstream; `Menu` and `Win` are deprecated; `Msgbox` is
      intentionally disabled in `sdkconfig-default.properties`.
- [x] Fix the close-after-delete crash. `luavgl_detach_all()` used the
      deprecated `lv_obj_is_valid()` alias, which is
      `lv_obj_is_in_widget_tree()` in 9.6 and dereferences freed memory. The
      runtime now keeps a live address/generation set and removes entries from
      the delete callback, so detach never validates a stale pointer. The flag
      methods also call the dedicated `lv_obj_set_<flag>()` / `lv_obj_is_<flag>()`
      API instead of the deprecated `lv_obj_add_flag()` family.
- [x] Add package image assets: `...` is the install directory, absolute paths
      are rewritten to LVGL's `A:` stdio drive, and PNG files decode through
      lodepng. Verified with `tests/assets.lua` and a packaged asset. SVG icons
      are still not decoded on device, and audio is not exposed.
- [x] Add input groups (`lvgl.Group`): add/remove, focus next/prev, freeze,
      editing, wrap, object enumeration and the default group. Verified with
      `tests/group.lua` (18 checks), including attaching keypad/encoder devices
      and restoring their previous groups on close.
- [x] Add numeric animations (`lvgl.Anim`): x/y, size, opacity, radius/border,
      transforms, colours and value, with `ANIM_PATH` and `ANIM_INFINITE`. The
      frame callback is C, deleted targets stop their animations, and detach
      stops anything still running. Verified with `tests/animation.lua`
      (14 checks). Lua frame/completion callbacks are still open.
- [x] Use the official Lua logo for the launcher icons: colour and mono are
      generated with `tt icon-studio` and documented in
      `tools/README-icons.md`.
- [x] Return to the designer's grid editing: editable column/row tracks
      (`1fr`, `content`/`auto`, pixels), track alignment, per-child cell
      row/column/span/alignment, automatic cell placement and row growth, grid
      layout resolution in the shared layout pass and Lua export. Verified by
      `--self-test`, `--ui-test`, and a generated grid app on the Tab5.
- [x] Make the examples theme-native: use `Card`/`List` instead of hardcoded
      surfaces, expose `theme_color_error`, `theme_spacing`, `theme_size`,
      `theme_mono` and `theme_lerp`, and let the runner's content padding come
      from the theme's container style so it follows the density setting.
- [x] Add `device.address()` for devicetree buses, used by the I2C scanner.
- [ ] Add the typed raw signature database (device/gpio first) on top of the
      low-level call, generated from the SDK headers and export list.
- [x] Add the Lua GPIO app (`assets/examples/gpio.lua`, packaged as `Lua GPIO`)
      matching the C app's centred pin block. Fixed the empty-square artefact
      by matching the C app's `pad_all = 0` and change-only repaint.
- [x] Add the Lua I2C scanner (`assets/examples/i2c_scan.lua`, packaged as
      `Lua I2C Scan`), matching the internal scanner: bus chips, a themed grid
      card with addresses and devicetree device names, `device.address()` and
      the C app's 3-probe filter. It scans each ready bus once while the window
      is built, then the chips show/hide the matching card; re-applying a grid
      after layout corrupts teardown in this LVGL build. Verified i2c0 (10
      devices), i2c2 (1 device), grove0_i2c (0), and that closing works from
      both the packaged app and the companion's Run command.
- [x] Wire the runner and companion toolbar close actions to
      `app_event_emit_close()`. The window manager's default toolbar action
      only closes old-model apps, so both apps' X buttons did nothing before.
- [x] Add the SDK catalog: `CATALOG.md` (curated checklist) and
      `tools/sdk-catalog/catalog.py`, which combines the firmware export tables
      with the SDK headers and writes `catalog/sdk-coverage.md`,
      `sdk-symbols.csv` and `sdk-coverage.json`. The generator reads both
      `symbols.c` and `module.cpp` export tables, so the new notification module
      is classified as exports.
- [x] Add `tools/symbol-audit/symbol_audit.py`: reads the built runner's
      dynamic undefined symbols with `nm` and compares them with the
      `lvgl-module` and `lua_runtime` export tables. `--fix` appends missing
      entries, and the current audit is clean. The module needs a firmware
      rebuild for new exports to take effect.
- [x] Add `tactility.notification`: send/list/get/dismiss/count plus severity
      and source constants. Verified with `tests/notification.lua` and
      `Lua Notifications`. Actions, Bundle data and events are not exposed yet.
- [ ] Add notification actions/data/events once a Lua-callback dispatch design
      and a Bundle binding exist.
- [x] Add the PC Lua GUI designer (`tools/lua-gui-designer`): simulator
      profiles, Material and material-mono tokens, appearance spacing/size/font,
      theme mode, surface override and system-bars controls, firmware-derived
      chrome geometry (split and side), widget palette, drag/resize,
      tree context menu and drag/reparent, editable Code tab, persistent
      designer settings (including maximized state), percentage width/height,
      flex and grid containers, project JSON, Lua export, syntax highlighting
      and auto-indent. Verified by `--self-test`, the `--ui-test` control sweep,
      screenshots, and running the exported `designer_demo.lua` on the Tab5.
- [ ] Add on-device editor conveniences: auto-indent, line numbers and bracket
      matching. Syntax highlighting on device needs a custom text renderer.
- [x] Fix designer selection shifting: geometry `<FocusOut>` handlers now carry their
      owning widget id instead of applying the old panel's X/Y/W/H to the newly
      selected widget. Added a small drag threshold and made content-sized boxes
      follow their children in the preview, with disabled W/H fields.
- [x] Fix content-sized flex cards collapsing to zero height: a child's `flex_grow`
      inside a flex parent that is content-sized on the main axis is circular.
      It is now hidden in the inspector and omitted from export, and the model
      ignores it too. Reproduced with the appearance-settings clone and verified
      on the Tab5.
- [x] Clone the Appearance Settings main view 1:1 as a designer project and Lua
      app (`tools/lua-gui-designer/appearance.luadesign.json`,
      `appearance.lua`, generated by `make_appearance_clone.py`). Verified on the
      Tab5 against device screenshots: geometry, chips, swatches, bars, fonts and
      the scrolling page all match.
- [x] Match the firmware layout model in the preview: theme focus-ring margins
      for chips/buttons/switches in flex layouts, LVGL's grow sizing (grow items
      take the space others leave and ignore their own width), `Spinbox`,
      `text_align`, LVGL symbol rendering for step buttons, a clipped scrolling
      preview (mouse wheel), and no out-of-bounds warning for a content-sized
      page that scrolls on the device.
- [x] `--open <project>` loads a designer project from the command line; the
      appearance clone opens with it.
- [x] Make scrolling-page exports scroll in the same object as a C app: the
      canvas and the page are content-sized and lose `SCROLLABLE`, so the
      runner's padded content object owns the gesture and the scrollbar sits on
      the window edge. Verified pixel-for-pixel against the Appearance app
      (6 px bar, same colour, same thumb height and position).
- [x] Add **Lua Apps** (`tactility.luaapps`, binary `lua_apps`) to the runtime
      package: a native manager that lists packaged `tactility.lua.*` apps with
      their icons, opens them, and uninstalls them after a confirmation. The
      start and uninstall work runs on the app's task; running the ELF loader on
      the LVGL task trips ESP-IDF's flash-cache stack assertion.
- [x] Fix profile changes squashing content in the designer: content-sized
      labels, chips, buttons and switches are now measured from their text with
      the firmware font (instead of reusing stored boxes), and scaling skips
      content-sized axes. Switching profiles (and back) reflows instead of
      truncating "Spa…" and shrinking chips.
- [x] Make the designer standalone: the firmware's Adwaita Sans and Material
      Symbols subsets are bundled under `tools/lua-gui-designer/fonts/` with
      their licences, so only the apps repository and the SDK are needed. A
      local Tactility checkout is still preferred when present; the env vars
      still override. Verified by running the self-test and the clone generator
      from a temp copy with no checkout nearby.
- [x] Fix profile switches not applying the spacing/size theme: the appearance
      clone stored the card pad as pad+border (24) instead of the theme token
      (23), so theme-derived paddings never matched; scaling touched `pad_all`
      but not `pad_row`/`pad_column`; and an in-place theme edit compared the new
      tokens with themselves. Container pad/gap values now follow the theme on
      profile/spacing/size/mono/Defaults changes, and **Scale** only scales
      geometry. Verified with Tab5 → T-Deck → Tab5 and the spacing slider.
- [x] Add a dedicated Lua apps repository scaffold at
      `../TactilityLuaApps`: the runtime package, `Scripts/*.lua` packaging, a
      GitHub Actions workflow that builds/bundles/publishes to Pages with
      TactilityAppPublisher, and a landing page with the repository URL and the
      catalog list.
- [ ] Add canvas drag-reparenting, multi-select/alignment, an optional
      bottom-docked code pane, asset/icon picking and weighted undo/redo.
- [ ] Add `catalog.py --check` to CI and diff the generated inventory on SDK
      updates.
- [x] Add a cross-binding smoke test (`tests/bindings.lua`) for fs, time,
      device, settings, power and wifi.
- [ ] Port the remaining old `lua-module` tests in more depth (runtime,
      error paths, settings persistence across launches).
- [ ] Write the step-5 measurement table before promising device support.
- [ ] Avoid `tt shell "mkdir"` until its reboot bug is fixed.

---

## 13. Reference index

Firmware (`TactilityWork`):

- `Buildscripts/TactilitySDK/NewProject/module/manifest.properties`
- `Buildscripts/TactilitySDK/NewProject/module/main/Source/main.c`
- `Buildscripts/TactilitySDK/NewProject/gui/main/Source/main.c`
- `Buildscripts/TactilitySDK/Tools/app.py`
- `Modules/app-module/include/app/install.h`
- `Modules/app-module/include/app/loader.h`
- `Modules/app-module/include/app/manager.h`
- `Modules/app-module/source/install.cpp`
- `Modules/app-module/source/manager.cpp`
- `Modules/app-module/source/module_loader.cpp`
- `Modules/app-module/source/package_manifest_parsing_v3.cpp`
- `Modules/app-module/source/package_compatibility.cpp`
- `Modules/app-esp32-module/source/app_esp32_loader_service.cpp`
- `Modules/app-esp32-module/source/module.cpp`
- `TactilityKernel/include/tactility/module.h`
- `TactilityKernel/source/module.cpp`
- `TactilityKernel/source/paths.cpp`

Old prototype (salvage only):

- `Libraries/lua/`
- `Modules/lua-module/`
- `Modules/luavgl-module/`
- `Tests/lua-module/`
- `Tests/luavgl-module/`
- `Modules/lua-module/GENERATION.md`
- `Modules/lua-module/generate-bindings.py`
- `OLD_LUA_STUFF/LuaHello/`, `OLD_LUA_STUFF/LuaEditor/`, `OLD_LUA_STUFF/LuaSdkTest/`
