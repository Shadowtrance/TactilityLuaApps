# Lua bindings: design and IDL

Status: partially implemented (2026-11-21)

- The capability generator exists (`tools/lua-bindings`) and runs during the
  runtime module build.
- `tactility.gpio` is generated from `bindings/gpio.toml` and verified on the
  Tab5: 55 pins, constants, handle lifetime and the invalid-pin error path.
- The Lua GPIO app (`assets/examples/gpio.lua`, packaged as `Lua GPIO`) matches
  the C app's centred 10-column block using only the capability API.
- `tactility.i2c` is generated from `bindings/i2c.toml`: probe plus 8/16-bit
  register read/write/set/reset. It uses the new `c_args` hook so the out
  parameter can sit before the timeout in the C call. Verified against the
  Tab5's internal bus (3 controllers, 8 devices on `i2c0`).
- I2C block transfers are hand-written and registered through the IDL's
  `impl = "custom"` hook: `i2c.read`, `i2c.write` and `i2c.write_read`. Lua
  strings are the binary-safe input buffer; a read returns a new string.
  `tests/i2c.lua` exercises a block read on `0x10` and the failure paths.
- `tactility.adc` is generated from `bindings/adc.toml`:
  `read_raw(controller, channel, timeout?)` returns the raw conversion or
  `nil, reason`. `tests/adc.lua` covers discovery and the invalid channel path.
- `tactility.uart` is generated from `bindings/uart.toml` for open/close,
  byte reads/writes and flush. `is_open`, `available` and the block transfers
  are hand-written because the C API returns plain values or takes buffers.
  `tests/uart.lua` introspects a controller and only touches the error paths
  when it is closed, so it cannot steal bytes from a GPS/radio service.
- `tactility.spi` is generated for lock/try_lock/unlock and hand-written for
  one-shot `transmit`/`read`/`write`: each call adds a temporary `SpiDevice` for
  the clock/mode/CS, transfers and removes it. `tests/spi.lua` uses an invalid CS
  so it never selects a display or SD card.
- `device.address()` exposes the devicetree address of a device, so the Lua I2C
  scanner can map a controller's children to the address rows it found. The
  scanner now mirrors the internal one: bus chips plus a grid with device names.
- `tactility.notification` is hand-written: `send{...}`, `list`, `get`,
  `dismiss`, `dismiss_all`, `dismiss_non_sticky` and `count`, with `SEVERITY`
  and `SOURCE` constants. The runner adds `subscribe(callback)` and `EVENT`:
  a 200 ms LVGL timer drains the module's event queue, so the callback runs on
  the LVGL task with the right state, and the subscription is pinned until
  cancelled or the runtime closes. Verified with `tests/notification.lua`,
  `tests/notification_events.lua` and `assets/examples/notification.lua`.
  Actions and Bundle data are still deliberately not exposed; an action would
  need a Lua-callback lifetime design because a notification can outlive the
  script's state, and Bundle enumeration is a kernel API that does not exist yet.
- `tactility.raw.call(name, ...)` is implemented as the low-level escape hatch:
  it resolves any exported symbol at runtime and calls it with pointer-sized
  arguments (integers, booleans, strings, handles). The **typed signature
  database** described below is still the next step; the untyped call proves
  symbol lookup, the trampoline and handle extraction.

This document describes how Lua scripts get access to Tactility and the SDK
without repeating the old "one generated wrapper per C function" experiment.

## 1. Why the old approach did not work

The old in-firmware POC generated a Lua wrapper for every C function it could
describe. That creates:

- thousands of wrappers that all change when the SDK moves;
- per-function decisions about ownership, out-parameters, callbacks and
  structs that a C header does not describe;
- a Lua API shaped like the C SDK instead of shaped like something a script
  should use;
- a large review surface for very little product value.

The useful part was the *idea of describing functions mechanically*. The
mistake was generating code for each one.

## 2. Three tiers

### Tier 0 — capability API (hand-written, the default)

`tactility.fs`, `tactility.gpio`, `tactility.i2c`, … These are designed for Lua:
handles, `nil, reason`, constants, short names. They are not a C mirror.

```lua
local pin = tactility.gpio.acquire(5, tactility.gpio.OUTPUT)
if not pin then return end
pin:set_level(true)
pin:release()
```

This is what scripts should use. Adding a capability is a deliberate API
decision, not "the header changed".

The glue is **generated from the IDL wherever the contract is mechanical**. The
driver, kernel and SDK functions are the existing exported C; nothing is
reimplemented. A capability function is a thin `lua_State*` function that reads
arguments, calls the SDK function, maps `error_t` to `nil, reason`, and manages
the handle. The generator emits it:

```c
// build/generated/gpio_api.c — generated, do not edit
static int lua_gpio_set_level(lua_State* state) {
    auto* pin = check_pin(state, 1);
    const bool high = lua_toboolean(state, 2);

    const error_t error = gpio_descriptor_set_level(pin->descriptor, high);
    if (error != ERROR_NONE) {
        lua_pushnil(state);
        lua_pushstring(state, error_to_string(error));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}
```

The generator also emits the `luaL_Reg` table, the handle userdata
struct/check/push/`__gc` helpers, the constants, the LuaLS stubs and the docs.
The repo contains the IDL and the *custom* C, not the generated output.

### Generated vs custom

| Concern | Default |
| --- | --- |
| argument and return marshalling, optional/default args | generated |
| `error_t` → `nil, reason` | generated |
| handle userdata, acquire/release/`__gc`, method dispatch | generated from the type descriptor |
| `luaL_Reg` tables, stubs, docs | generated |
| constants/enums listed in the IDL | generated |
| table building (`device.list`, `fs.mounts`, scan results) | custom |
| callbacks and event subscriptions | custom |
| buffers and transfers whose length/ownership is not in the C type | custom, marked `buffer` in the IDL |
| API shaping that deliberately differs from the C signature | custom, marked `impl = "custom"` |

A custom function is still small and explicit: the IDL names it, a hand-written
C file implements it, and the generator wires it into the same table. There is
no per-function hand-editing of the generated file.

Functions that are pure 1:1 mechanical calls do not need a Tier 0 wrapper at
all: the raw dispatcher already covers them. Curate when the Lua contract needs
ownership, defaults or shaping — and even then, the glue is generated.

### Tier 1 — raw dispatcher (generated data, the long tail)

For SDK functions that have not been curated, the runtime ships a **signature
database** and one generic dispatcher:

```lua
local descriptor = tactility.raw.call("gpio_descriptor_acquire_pin_spec", device, 5, flags)
local ok, reason = tactility.raw.call("gpio_descriptor_set_level", descriptor, true)
```

No per-function C. A build step extracts signatures from the SDK headers,
filters them by the symbols the firmware actually exports, and emits a compact
table. The dispatcher marshals Lua values according to that table.

Unsupported in Tier 1: callbacks, structs by value, varargs, arrays. Those stay
in Tier 0 where their semantics can be expressed.

### Tier 2 — optional binding modules (later)

Niche subsystems (HTTP, audio, camera) can be separate packages that register
their own `tactility.*` table into each new Lua state. The core runtime stays
small and each subsystem can be updated independently. Not required for the
first milestone.

## 3. API conventions

- Everything lives under the `tactility` global. No new globals.
- Fallible calls return `value` (`true` for void success) or `nil, reason`.
  `reason` is a string, usually from `error_to_string(error)`.
- Never use `false` to mean failure; `false` is a legitimate boolean value.
- Handles are userdata with methods. `release()` is explicit and idempotent in
  effect; using a released handle raises a Lua error. `__gc` releases an
  un-released handle.
- Constants are integers under the subsystem table, e.g. `tactility.gpio.OUTPUT`.
- Names are verbs and nouns, not C spellings: `acquire`, `release`, `set_level`,
  `get_level`, `transfer`.
- A table carries an `api` integer (`tactility.gpio.api`). The package-level
  `LUA_RUNTIME_API_VERSION` covers the core runtime; a future per-subsystem
  version can be added without a new global.

## 4. Capability IDL (hand-authored contract)

One TOML file per subsystem under `bindings/`. TOML is readable, allows
comments, and Python 3.11+ parses it with `tomllib`, so the build needs no new
dependency.

```toml
# bindings/gpio.toml
[module]
name = "gpio"
summary = "GPIO pins through the kernel GPIO controller"
api = 1

[types.Pin]
kind = "handle"
c_type = "struct GpioDescriptor*"
acquire = "gpio_descriptor_acquire_pin_spec"
release = "gpio_descriptor_release"
summary = "An acquired GPIO descriptor"

[[function]]
name = "acquire"
signature = "acquire(pin: u32, flags?: u32) -> Pin | nil, reason"
doc = "Acquires a pin. Release the handle when done."
c = "gpio_descriptor_acquire_pin_spec"
# impl = "generated" is the default

[[function]]
name = "set_level"
signature = "Pin:set_level(high: bool) -> true | nil, reason"
doc = "Drives the pin high or low."
c = "gpio_descriptor_set_level"

[[function]]
name = "info"
signature = "tactility.gpio.info() -> table | nil, reason"
doc = "Controller and pin counts, shaped as a Lua table."
impl = "custom"          # a small hand-written C function
c = "lua_gpio_info"
```

The signature string is a small language:

```
name(arg: type, optional?: type) -> result | nil, reason
Pin:method(arg: type) -> result | nil, reason
```

Types: `bool`, `i32`, `u32`, `i64`, `u64`, `f64`, `string`, `buffer`, plus
handle names (`Pin`, `Device`, …). An optional argument may have a default
(`flags?: u32 = 0`).

Generated from the IDL:

- the `lua_State*` glue for every `generated` function;
- handle userdata structs, check/push/`__gc`/release helpers;
- `luaL_Reg` tables and the module registration;
- constants/enums listed in the module;
- LuaLS stubs, Markdown reference and the test checklist.

Still hand-written:

- functions marked `impl = "custom"` (table building, callbacks, buffers,
  deliberate API shaping), each a small, named C function;
- behavioural tests.

`c =` names the exported C function the glue calls. It is also used for
compile-time signature checking: the generated code takes the function's typed
pointer, so a changed SDK signature fails the build instead of failing at
runtime.

## 5. Raw signature database (generated)

### Sources

1. SDK headers, parsed with libclang.
2. The exported symbol list of the firmware/module (only what a side-loaded
   module can actually resolve).

The intersection is the candidate set. The generator reports functions it
skips and why (callback, struct by value, varargs, unsupported type).

### Format

A generated C file with a data table and typed address checks:

```c
// generated, do not edit
static const uint8_t SIG_set_level[] = { RAW_HANDLE, RAW_BOOL };

static error_t (*const check_set_level)(struct GpioDescriptor*, bool) =
    &gpio_descriptor_set_level;

const LuaRawFunction LUA_RAW_FUNCTIONS[] = {
    { "gpio_descriptor_set_level", RAW_ERROR, 2, SIG_set_level,
      (void*)check_set_level },
    ...
};
```

The typed `check_*` assignment makes a changed SDK signature a compile error.
The table is data: no wrapper body, no per-function Lua code.

### Dispatcher rules

- Lua types: integer → fixed-width integer with range check; number → `f32`/
  `f64`; boolean → `bool`; string → `const char*` valid for the call; light
  userdata or a Tier 0 handle → pointer.
- Out scalars: allocate a temporary, pass its address, return the value.
- `error_t` returns: `ERROR_NONE` → result(s), otherwise `nil, reason`.
- Pointer results are light userdata. Ownership is the script's problem; Tier 0
  exists so it usually does not have to be.
- Explicitly unsupported: callbacks, structs by value, arrays, varargs. The
  generator omits them; the dispatcher rejects them if a name is not in the
  table.
- `tactility.raw.api` and `tactility.raw.sdk` identify the database.

The raw layer has the same trust level as an ELF app: it can crash the device.
It is not a sandbox, and it is not the recommended API.

## 6. Build and update workflow

1. **SDK bump** → regenerate the raw database and the capability glue from the
   new headers and symbol list; rebuild the runtime module. The typed address
   checks catch curated bindings that drifted. Both generators run per target
   platform, because the exported set differs between them.
2. **Add a capability** → add the TOML entry and a test. Write C only for a
   function explicitly marked `impl = "custom"` (table shaping, callbacks,
   buffers). Bump `LUA_RUNTIME_API_VERSION` if existing scripts could notice.
3. **Ship** → `target.sdk` gates install; `lua.runtime.api` gates execution.
4. **Verify** → run the on-device tests; the generator prints how many exported
   symbols are covered, skipped and why.

Both the raw database and the capability glue are generated, never
hand-edited. The generated files live in the build directory. The repository
contains the capability IDL and the few custom C functions; the IDL is the
reviewed source of truth.

This is the difference from the old POC:

- it generated wrappers for *all headers*, so the output was huge and the
  surface accidental;
- this generates glue only for a **small curated IDL**, and only for the
  functions whose contract the IDL can express. The rest is either a custom
  function or the raw dispatcher.

The generated output is deterministic and commented with its IDL source, so a
diff is reviewable.

## 7. Testing

- One Lua test per subsystem with assertions, run on-device (the existing
  `tests/` directory).
- A raw smoke test that calls a harmless exported function and checks the
  result.
- The existing `luavgl` and teardown tests.
- A coverage report from the generator: exported symbols → included/skipped.
- Later: the same tests under a POSIX/simulator build, which is pinned for
  after the device work.

## 8. What this deliberately does not do

- Mirror every C function in Lua.
- Generate a wrapper body per function.
- Bind callbacks, structs by value, varargs or arrays automatically.
- Promise a sandbox. Scripts are apps.

## 9. Decisions to confirm

1. **IDL format**: TOML with a compact signature string (recommended) vs a
   Lua-table DSL or a custom line format.
2. **Raw database scope**: start with the subsystems we care about (device, fs,
   gpio, i2c, uart) and grow (recommended), vs generate the whole exported SDK
   immediately.
3. **Raw database shipping**: inside the runtime module while it is small
   (recommended for the first milestone), vs an optional bindings package.
4. **Handle lifetime**: explicit `release()` plus `__gc` release, use-after-
   release raises (recommended), vs no automatic release.
5. **Capability C**: generate the glue by default from the IDL; keep only
   explicitly marked `impl = "custom"` functions hand-written (recommended).
   The SDK implementation itself is never rewritten either way.
6. **Raw availability**: available by default but documented as unstable, vs
   behind a package/flag.

## 10. First milestone

GPIO, because it is the concrete gap from the old port and it is small.

Done:

1. The capability generator (`Apps/LuaRuntime/tools/lua-bindings`) with the TOML parser,
   signature/argument generation, handle plumbing and registration. It runs
   from `lua_runtime`'s CMake project, so `tt app build` regenerates the glue.
2. `bindings/gpio.toml` defining `count`, `acquire`, `set_level`, `get_level`,
   `set_flags`, `get_flags`, `has_flags` and `pin_number`, with `Pin` as a
   handle and `GpioOwner` as an enum. No hand-written GPIO glue.
3. Verified on the Tab5: 55 pins, constants, `api` field, invalid-pin
   `nil, reason`, and handle release.
4. Firmware export added for `gpio_descriptor_has_flags` (the one GPIO function
   the kernel export table was missing).

Next:

5. Raw database generator for `device` and `gpio` headers + dispatcher.
6. Tests: raw call on the same functions, plus the existing teardown/smoke
   suite.
7. Decide whether the raw database stays in the core or moves to an optional
   package.
