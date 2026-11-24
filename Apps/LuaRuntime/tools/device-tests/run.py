"""Run the Lua regression scripts on real devices as throwaway packaged apps.

Each script is copied with a small timer appended that reports its final ``result``
through ``tactility.log``. The tool installs the package, starts it, and reads the
device's serial console for that marker. Serial is used instead of the on-device
``log`` command: starting a shell app per test left the development server with a
busy shell and eventually wedged the devices.

Example (from the repository root):

    py Apps\\LuaRuntime\\tools\\device-tests\\run.py ^
      --device "Tab5,192.168.0.245,esp32p4,COM34" ^
      --device "T-Deck Plus,192.168.0.206,esp32s3,COM22"

Requires pyserial, the Lua Runtime installed on each device, and the device's
development server running.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

try:
    import serial
except ImportError:  # pragma: no cover - environment guidance
    raise SystemExit("device-tests: pyserial is required (py -m pip install pyserial)")

ROOT = Path(__file__).resolve().parents[2]
MAKE_APP = ROOT / "tools" / "lua-app" / "make_lua_app.py"
MANIFEST = ROOT / "manifest.properties"
APP_ID = "tactility.lua.sweep"
DEFAULT_SETTLE_SECONDS = 4.5

# Tests that report success synchronously and abort on failure. When the harness
# timer's marker is missing from the serial capture, the runner's
# "script evaluated (N-byte result)" line is a safe fallback.
MARKERLESS_TESTS = {
    "adc",
    "icons",
    "spi",
    "uart",
    "bindings",
    "display",
    "gpio",
    "i2c",
    "line",
    "notification",
    "object_api",
    "raw",
    "system",
    "tabs_statusline",
    "teardown",
}

HARNESS = """

-- device-tests harness: report the final result after the screen settles
__sweep_result_timer = lvgl.Timer{ period = 1800, callback = function()
    __sweep_result_timer:delete()
    tactility.log.info("SWEEPRESULT", tostring(result))
end }
"""


@dataclass(frozen=True)
class Device:
    name: str
    host: str
    platform: str
    port: str


class SerialCapture:
    """Reads a device's console in the background without resetting it."""

    def __init__(self, port: str, baud: int):
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = baud
        self.serial.timeout = 0.2
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.buffer = bytearray()
        self.lock = threading.Lock()
        self.closed = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self) -> None:
        while not self.closed.is_set():
            try:
                data = self.serial.read(4096)
            except Exception:
                break
            if data:
                with self.lock:
                    self.buffer += data
                    if len(self.buffer) > 512 * 1024:
                        del self.buffer[: len(self.buffer) - 256 * 1024]

    def clear(self) -> None:
        with self.lock:
            self.buffer.clear()

    def text(self) -> str:
        with self.lock:
            return self.buffer.decode("utf-8", "replace")

    def close(self) -> None:
        self.closed.set()
        self.thread.join(timeout=1)
        try:
            self.serial.close()
        except Exception:
            pass


def sdk_version() -> str:
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        if line.startswith("target.sdk="):
            return line.split("=", 1)[1].strip()
    raise SystemExit(f"device-tests: no target.sdk in {MANIFEST}")


def resolve_tools(explicit: str | None) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    environment = os.environ.get("TACTILITY_SDK_TOOLS")
    if environment:
        candidates.append(Path(environment))
    candidates.append(Path.home() / ".tactility" / "sdk" / sdk_version() / "tools")
    for candidate in candidates:
        if (candidate / "shell.py").is_file():
            return candidate
    searched = "\n  ".join(str(candidate) for candidate in candidates)
    raise SystemExit(
        "device-tests: could not find the SDK tools (shell.py). Searched:\n  "
        + searched
        + "\nPass --tools or set TACTILITY_SDK_TOOLS."
    )


def parse_device(text: str) -> Device:
    parts = [part.strip() for part in text.split(",")]
    if len(parts) != 4 or not all(parts):
        raise argparse.ArgumentTypeError(
            "expected NAME,HOST,PLATFORM,SERIAL (for example Tab5,192.168.0.245,esp32p4,COM34)"
        )
    name, host, platform, port = parts
    return Device(name, host, platform, port)


def package(device: Device, test: Path, output_dir: Path) -> Path:
    script = output_dir / f"{test.stem}.lua"
    script.write_text(test.read_text(encoding="utf-8") + HARNESS, encoding="utf-8")
    output = output_dir / f"{test.stem}-{device.platform}.app"
    command = [
        sys.executable,
        str(MAKE_APP),
        "--script",
        str(script),
        "--name",
        "Sweep",
        "--id",
        APP_ID,
        "--platform",
        device.platform,
        "--output",
        str(output),
    ]
    if test.stem == "assets":
        command += ["--assets", str(ROOT / "tests" / "assets")]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError((result.stdout + result.stderr).strip())
    return output


def retry(operation, attempts: int = 5, delay: float = 2.0):
    for attempt in range(1, attempts + 1):
        try:
            return operation()
        except BaseException:  # noqa: BLE001 - shell helpers exit via SystemExit
            if attempt == attempts:
                raise
            time.sleep(delay)
    raise AssertionError("unreachable")


def is_pass(result: str) -> bool:
    lowered = result.lower()
    if "fail" in lowered or "crash" in lowered:
        return False
    return "pass" in lowered or "ok" in lowered or "armed" in lowered or "survived" in lowered


def run_device(device: Device, tests: list[Path], shell, options) -> int:
    print(f"\n== {device.name} ({device.platform})", flush=True)
    try:
        capture = SerialCapture(device.port, options.baud)
    except Exception as error:
        print(f"cannot open {device.port}: {error}", flush=True)
        return len(tests)

    failures = 0
    try:
        if not wait_for_device(device.host, shell, options.connect_timeout):
            print("device did not become reachable", flush=True)
            return len(tests)

        for test in tests:
            package_path = package(device, test, options.output)
            capture.clear()
            started = time.monotonic()
            try:
                retry(lambda: shell.install(device.host, 6667, str(package_path)))
                retry(lambda: shell.run(device.host, 6667, APP_ID))
                time.sleep(options.settle)
                text = capture.text()
            except BaseException as error:  # noqa: BLE001
                failures += 1
                print(f"{test.stem:22} ERROR {error!r}", flush=True)
                continue

            elapsed = time.monotonic() - started
            log_path = options.output / f"{device.name.replace(' ', '-').lower()}-{test.stem}.log"
            log_path.write_text(text, encoding="utf-8", errors="replace")

            matches = re.findall(r"SWEEPRESULT: (.*)", text)
            result = matches[-1].strip() if matches else None
            if result is None and test.stem in MARKERLESS_TESTS:
                evaluated = re.search(r"script evaluated: .*\((\d+)-byte result", text)
                failed = "failed to evaluate" in text or "LuaRunner: on_show failed" in text
                if evaluated and int(evaluated.group(1)) > 0 and not failed:
                    result = f"PASS (evaluated {evaluated.group(1)}-byte result; marker missing)"

            crashed = "Guru Meditation" in text or "Backtrace:" in text

            if test.stem == "teardown" and result:
                # Teardown only arms itself; closing the app is the actual test.
                try:
                    shell.app_request(device.host, 6667, shell.FRAME_APP_UNINSTALL, APP_ID)
                    time.sleep(3)
                    code, _ = shell.run_command(device.host, 6667, "free", timeout=8)
                    result = "teardown survived close" if code == 0 else "teardown crashed"
                except BaseException as error:  # noqa: BLE001
                    result = f"teardown crashed ({error!r})"

            if result is None:
                failures += 1
                print(f"{test.stem:22} FAIL  no result ({elapsed:.1f}s)", flush=True)
                if crashed:
                    print("    crash marker on serial", flush=True)
                for line in text.splitlines():
                    if "LuaRunner:" in line or "assert" in line.lower():
                        print(f"    {line.strip()}", flush=True)
            elif is_pass(result):
                print(f"{test.stem:22} PASS  {result} ({elapsed:.1f}s)", flush=True)
            else:
                failures += 1
                print(f"{test.stem:22} FAIL  {result} ({elapsed:.1f}s)", flush=True)

        if not options.keep:
            try:
                shell.app_request(device.host, 6667, shell.FRAME_APP_UNINSTALL, APP_ID)
            except BaseException as error:  # noqa: BLE001
                print(f"cleanup failed: {error!r}", flush=True)
    finally:
        capture.close()
    return failures


def wait_for_device(host: str, shell, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            code, _ = shell.run_command(host, 6667, "free", timeout=5)
            if code == 0:
                return True
        except BaseException:  # noqa: BLE001
            pass
        time.sleep(3)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--device",
        action="append",
        type=parse_device,
        default=[],
        metavar="NAME,HOST,PLATFORM,SERIAL",
        help="device to test; repeat for more (required)",
    )
    parser.add_argument("--only", help="run one test by stem")
    parser.add_argument("--list", action="store_true", help="list the tests and exit")
    parser.add_argument("--tools", help="SDK tools directory (contains shell.py)")
    parser.add_argument("--keep", action="store_true", help="do not uninstall the sweep app")
    parser.add_argument("--settle", type=float, default=DEFAULT_SETTLE_SECONDS, help="seconds to collect serial after starting a test")
    parser.add_argument("--baud", type=int, default=115200, help="serial baud rate")
    parser.add_argument("--connect-timeout", type=float, default=120.0, help="seconds to wait for the dev server")
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "device-tests", help="package/log output directory")
    options = parser.parse_args()

    tests = sorted((ROOT / "tests").glob("*.lua"))
    if options.only:
        tests = [test for test in tests if test.stem == options.only]
    if options.list:
        for test in tests:
            print(test.stem)
        return 0
    if not tests:
        print("device-tests: no tests found", file=sys.stderr)
        return 1
    if not options.device:
        parser.error("at least one --device is required (or use --list)")

    options.output.mkdir(parents=True, exist_ok=True)
    tools = resolve_tools(options.tools)
    sys.path.insert(0, str(tools))
    import shell  # noqa: E402

    failures = 0
    for device in options.device:
        failures += run_device(device, tests, shell, options)

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
