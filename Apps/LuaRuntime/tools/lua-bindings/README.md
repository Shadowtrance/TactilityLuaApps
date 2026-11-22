# lua-bindings

Generate the mechanical Lua glue for a capability API from
`Apps/LuaRuntime/bindings/*.toml`.

The generator emits the parts that are the same for every function: argument
reading, calling the exported SDK function, `error_t` → `nil, reason`, handle
userdata (acquire/release/`__gc`), the `luaL_Reg` tables and constants. The
output is written to the module build directory; it is never edited by hand.

The module's CMake project runs the generator before the component is
configured, so a normal `tt app build` is enough. Changing an IDL file triggers
a regeneration.

## IDL

```toml
[module]
name = "gpio"
api = 1
includes = ["tactility/drivers/gpio_controller.h", "tactility/error.h"]

[types.Pin]
kind = "handle"
c_type = "struct GpioDescriptor*"
release = "gpio_descriptor_release"

[types.GpioOwner]
kind = "enum"
c_type = "enum GpioOwnerType"

[constants]
OUTPUT = 4

[[function]]
name = "acquire"
signature = "acquire(controller: Device, pin: u8, flags: u16, owner?: GpioOwner = 2) -> Pin | nil, string"
c = "gpio_descriptor_acquire"
null_error = "could not acquire the GPIO pin"

[[function]]
name = "set_level"
signature = "Pin:set_level(high: bool) -> true | nil, string"
c = "gpio_descriptor_set_level"

[[function]]
name = "get_level"
signature = "Pin:get_level() -> bool | nil, string"
c = "gpio_descriptor_get_level"
```

For an SDK signature where the out parameter is not last (for example before a
trailing timeout), list the C call order explicitly. The `out` token becomes the
generated result variable:

```toml
[[function]]
name = "read8"
signature = "read8(controller: Device, address: u8, reg: u8, timeout?: u32 = 100) -> u8 | nil, string"
c = "i2c_controller_register8_get"
c_args = ["controller", "address", "reg", "out", "timeout"]
```

Signature types: `bool`, `u8`, `u16`, `u32`, `i32`, `i64`, `u64`, `f64`,
`string`, `Device`, and declared handles/enums. Optional arguments carry a
default (`owner?: GpioOwner = 2`).

A function with `impl = "custom"` is not generated: the generated registration
references the named C function (`c = "lua_gpio_info"`), which lives in a small
hand-written file. Use that for table building, callbacks, buffers and API
shaping the IDL cannot express. The generator declares custom functions
**outside** its anonymous namespace, so define them at global scope in the
hand-written file.

For byte buffers, `lua_bindings_check_bytes()` and `lua_bindings_push_bytes()`
in `bindings_private.h` wrap Lua's binary-safe strings. `bindings/i2c.toml`'s
`read`/`write`/`write_read` are the worked example.

## Generated code

- `bindings_generated.cpp` in the module build directory;
- `bindings_generated.h` (declaration of `lua_bindings_open_generated`).

The generated file includes the IDL's `includes` and `bindings_private.h`.
Handles get a userdata struct, `check_*`/`push_*`, an idempotent `release` and a
`__gc` that releases an un-released handle.

## Running the generator by hand

```powershell
py Apps\LuaRuntime\tools\lua-bindings\generate_bindings.py `
  --idl-dir Apps\LuaRuntime\bindings `
  --output-dir Apps\LuaRuntime\lua_runtime\build\generated
```

The build passes the SDK Python interpreter, so `tt app build` needs no
separate setup.
