# Shared Lua runner — plan

Status: proposal, not implemented. Written 2026-11-28 for the evening session.

## Goal

Install **one `lua_runner` binary per platform** with the LuaRuntime package and let
script packages use it instead of embedding their own copy.

Today every packaged Lua app contains `bin/<platform>/lua_runner.{elf,so}`. The
runner is ~120–165 kB depending on the build. That costs package size, install
time and — more importantly — ABI drift: a script built with an older runner can
load against a newer `lua_runtime` module and fail in confusing ways (`tactility.http`
missing, `abort()` in the runner, etc.). Those failures happened twice today.

## What the framework allows today

- A package manifest's `app.N.binary` is a **filename**, not a path:
  `app_package_manifest_is_valid_binary_name()` accepts only alphanumerics, `.`,
  `_` and `-`.
- The loaders resolve `<app install dir>/bin/<target>/<binary>.{elf,so}`.
- The app instance is the script app, so `lua_runner` already resolves the script
  as `<script app>/lua/main.lua` from `...`. A shared ELF would keep working
  without a script argument, because the app id/context still belongs to the
  script package.
- The app framework can pass start arguments (`AppStartContext.arguments`), but
  package manifests have no way to specify them, and the launcher starts by app id.

## Options

### 1. Shared binary path in the manifest (smallest)

Allow `app.N.binary` to contain a relative path that stays under
`/sdcard/tactility/app`, for example:

```properties
app.0.binary=../../tactility.luaruntime/bin/esp32s3/lua_runner
```

The package still installs only `lua/`, `assets/`, icons and the manifest. The
runner is loaded from the runtime package; the app id is still the script app, so
the script path resolution is unchanged.

Work:

- **Firmware** (`app-module`): extend the binary-name validation/parse to a
  `binary.path` (or allow `/` and `..` with normalization) and make the loader
  resolve it relative to the app install dir; reject paths that escape
  `/sdcard/tactility/app`.
- **Packaging** (`make_lua_app.py`): stop adding the runner; write the shared
  path for the target platform; keep `requires.api` so an old runtime is
  detected before launch.
- **Runtime**: no change. The runtime package already installs the runner and
  the module.
- **Versioning**: the script package depends on the installed runtime's API
  version. Show a clear error when the runtime is missing or too old.

Pros: small change, no new process/task model, script packages shrink to a few
hundred kB. Cons: still loads the runner ELF per app launch; peak RAM is the same.

### 2. Resident runner service module + thin app (best memory model)

A module owns the interpreter (and possibly the window) for the lifetime of the
system. Script apps become thin manifests that ask the service to open a script.

Work:

- A `lua_runner` **service module** that loads after LVGL, owns a Lua state and a
  root widget, and exposes `run(script_path, app_instance)`.
- A thin **runner app** (or the script app itself) that hosts the window and
  delegates the script to the service. The service must stay resident after the
  app closes.
- App-framework support for passing the script path/app id to the service and
  for the service to attach a window to the app's root.
- Error isolation (a script error must not take down the service), one script at
  a time, and cleanup of script-created widgets/descriptors.

Pros: one ELF load, one interpreter, lowest RAM, no per-package runner, no ABI
drift. Cons: largest design; the current `lua_runtime` module deliberately avoids
LVGL so it can load at boot, and a UI service adds lifecycle/architecture work.

### 3. Singleton runner app + request queue (framework workaround)

Keep a single runner app instance and pass it a script path through a settings
key or a small request service. A script package still needs a binary to launch,
unless the app framework grows an "alias/launcher" app type. Not simpler than 1.

## Recommendation

Start with **Option 1** tonight. It removes the per-package runner and ABI drift
with a bounded firmware change, and it can be tested on the simulator plus one
device quickly. Keep Option 2 as the follow-up if we want the runner to stay
resident and save the per-launch load.

## Test plan

Simulator:

1. Install the runtime package.
2. Install a script package **without** `bin/` and with the shared path.
3. Start it from the launcher and from the development shell.
4. Verify images (`.bin` via PSRAM), HTTP GET, settings, close/reopen.
5. Remove/rename the runtime package and verify a clear "runtime missing" error.

Devices:

1. T-Deck and Tab5: package size before/after, `free` before/after launch.
2. Install a runtime update only; an existing script package must keep running
   (ABI compatibility check).
3. Reboot, start the script, close it, start it again (runner unload/reload).
4. Run the device suite and the Weather example.

Firmware change is required only for Option 1's manifest/loader path. Everything
else stays in the LuaRuntime package.
