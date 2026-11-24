# Device tests

Runs the scripts in `Apps/LuaRuntime/tests` on real devices and reports PASS/FAIL
from the console.

```powershell
py Apps\LuaRuntime\tools\device-tests\run.py ^
  --device "Tab5,192.168.0.245,esp32p4,COM34" ^
  --device "T-Deck Plus,192.168.0.206,esp32s3,COM22"
```

Each `--device` is `NAME,HOST,PLATFORM,SERIAL`. Repeat it for more devices. The
platform must match a runner built in `Apps/LuaRuntime/build/package-intermediate`.

Useful options:

```powershell
--only widget_batch2      # one test
--list                    # list the test names
--settle 6                # seconds of serial to collect per test
--keep                    # leave the throwaway app installed
--output build\device-tests
--tools <sdk>\tools       # override SDK discovery (or set TACTILITY_SDK_TOOLS)
```

## How it works

- Copies each test, appends a 1.8 s `lvgl.Timer` that logs `SWEEPRESULT <result>`
  through `tactility.log`, and packages it as a throwaway app
  (`tactility.lua.sweep`) for the device's platform.
- Installs it over the development server, starts it, and watches the serial
  console. Serial is used instead of the on-device `log` command: one shell app
  per test left the development server busy and wedged the devices.
- Uninstalls the app when the run ends (unless `--keep`).

`teardown.lua` is special-cased: the tool closes the app and checks that the
device is still reachable, because that script only arms the teardown.

A few synchronous tests occasionally lose the timer marker in the serial
capture. For those, the runner's `script evaluated (N-byte result)` line is used
as a fallback; a failed evaluation logs `failed to evaluate` and still counts as
a failure.

## Requirements

- the Lua Runtime installed on the device
- the device's development server running on port 6667
- `pyserial`
- the SDK tools for the runtime's `target.sdk` (by default under
  `~/.tactility/sdk/<version>/tools`)

The tool opens the serial port with DTR/RTS low so it does not reset the device.
