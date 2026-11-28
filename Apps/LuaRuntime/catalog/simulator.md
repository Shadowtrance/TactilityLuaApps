# Running the Lua runtime in the simulator

Status: the package builds for **linux-x86_64** and was verified end-to-end in a
FirmwareSim build on WSL2. The companion opened, the editor rendered, and the
Clock example ran with the live clock. The same `.app` works on ESP and Linux;
the installer picks the binaries for the current platform.

## What the package contains

`bin/linux-x86_64/` has the same three apps and one module as the ESP build, as
shared objects:

- `lua.so` — the companion/browser
- `lua_runner.so` — the hidden script runner
- `lua_apps.so` — the Lua Apps manager
- `lua_runtime.so` — the Lua interpreter and `tactility.*` bindings module

The POSIX app loader `dlopen()`s the module and calls its `main()`, which returns
the `Module*`, exactly like the ESP ELF loader does. Apps are resolved by the
manifest's `app.N.binary`, so they keep their names (`lua_runner.so`) instead of
the loader's `app.so` fallback.

## Firmware requirement

The POSIX loader keeps app handles `RTLD_LOCAL` so apps cannot collide. The
runner needs the runtime module's `lua_*` symbols at `dlopen()` time, so a module
must be promoted to global scope when it is loaded. `api_get_module()` is only
called by the module loader, which makes it the right place:

```cpp
void* global_handle = dlopen(runtime->path.c_str(), RTLD_NOW | RTLD_GLOBAL);
if (global_handle != nullptr) {
    dlclose(global_handle);
}
```

Apply `firmware-app-posix-module-rtld-global.patch` to the firmware and rebuild
the simulator. Without it the runner loads, but its `lua_*` symbols stay
undefined and the app fails to start. Device firmware is unaffected; this patch
is simulator-only and belongs upstream.

## Build the package

The manifest lists `linux-x86_64` in `target.platforms`, so CI builds all three
platforms. On Linux (or WSL2):

```bash
python3 -c "import tt; tt.prepare_sdk('0.8.0-dev', ['linux-x86_64'])"
python3 tt.py app build -p Apps/LuaRuntime -a linux-x86_64
```

On Windows, pass `-a esp32s3` or `-a esp32p4` for a device build; the tool does
not cross-build Linux there.

## Build and run FirmwareSim (WSL2)

The executable target in the current firmware tree is `Tactility` (the old
`FirmwareSim` name is gone):

```bash
cd TactilityWork
cmake -S ./ -B buildsim
cmake --build buildsim --target Tactility -j"$(nproc)"
cd Data
export DISPLAY=:0
export WAYLAND_DISPLAY=wayland-0
export XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir
../buildsim/Tactility/Tactility
```

WSLg exposes the simulator window on the Windows desktop. On a plain Linux host
the same commands work with the local X11/Wayland display.

The Development app starts the TLS development server on port 6667 during
setup. The first `tt` connection asks to verify the device fingerprint; answer
`y`. The simulator's files live in `TactilityWork/Data/data`.

## Install and run

```bash
python3 tt.py app install Apps/LuaRuntime/build/tactility.luaruntime.app --host localhost
python3 tt.py shell --host localhost free       # connectivity check
tt screenshot --host localhost out.png
```

Start **Lua** from the launcher (or use the development shell/app-start), then
open an example. The simulator's script folder is `/mnt/lua` by default.

## Network

The firmware's `http-module` has POSIX implementations of the streaming client
and download API, so `tactility.http.download` works in the simulator (including
HTTPS and redirects). `tactility.http.request` uses the same client for GET
requests without a body, so the Weather example fetches a live forecast in the
simulator. Other methods, and requests with a body, still use the ESP-IDF client
and return an error in the simulator.

If the same checkout was previously built for `linux-x86_64` on Windows, remove
the CMake cache before building in WSL. CMake rejects a cache generated for a
different source path:

```bash
find Apps/LuaRuntime -type d -name 'cmake-build-linux-x86_64' -prune -exec rm -rf {} +
```

## Limits

- `make_lua_app.py` packages the Linux runner only when the runtime build
  produced `bin/linux-x86_64/lua_runner.so`. CI builds it and the published
  script packages include Linux; a local ESP-only build stays ESP-only.
- Simulator-only modules loaded before the runtime keep `RTLD_LOCAL`; the
  promotion happens when a module is registered, which is enough for apps
  started afterwards.
