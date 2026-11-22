#!/usr/bin/env python3
"""Tap, type and press keys on a running Tactility device.

Uses the development server's remote session (the same one `tt remote` uses),
so it works headlessly: no window is opened on this computer.

Examples:
    py device_ui.py tap 1200,157
    py device_ui.py tap 1200,157 wait 1 tap 600,300
    py device_ui.py type "print('hi')" key enter
    py device_ui.py tap 600,300 wait 0.5 shot out.png

Coordinates are screen pixels as seen in `tt screenshot` (the UI orientation).
Use --native to pass the display's raw pixels instead.
"""

import argparse
import datetime
import json
import os
import sys
import time


def find_sdk_tools():
    """Locate the SDK's Python tools, which provide shell.py and _remote_viewer.py."""
    configured = os.environ.get("TACTILITY_SDK_TOOLS")
    if configured and os.path.isfile(os.path.join(configured, "shell.py")):
        return configured

    home = os.path.join(os.path.expanduser("~"), ".tactility")
    sdk_root = os.path.join(home, "sdk")

    # Prefer the version that tt itself last used
    try:
        with open(os.path.join(home, "tt.json"), "r", encoding="utf-8") as file:
            versions = list(json.load(file).get("sdk", {}))
    except (OSError, ValueError):
        versions = []

    for version in versions:
        candidate = os.path.join(sdk_root, version, "tools")
        if os.path.isfile(os.path.join(candidate, "shell.py")):
            return candidate

    import glob

    matches = glob.glob(os.path.join(sdk_root, "*", "tools", "shell.py"))
    if not matches:
        raise SystemExit(
            "device_ui: no Tactility SDK tools found. Set TACTILITY_SDK_TOOLS to the folder that contains shell.py."
        )
    newest = max(matches, key=lambda path: os.path.getmtime(path))
    return os.path.dirname(newest)


sys.path.insert(0, find_sdk_tools())

import shell  # noqa: E402  (needs the SDK tools on sys.path)
import _remote_viewer as remote_viewer  # noqa: E402


DEFAULT_HOST = os.environ.get("TACTILITY_HOST", "192.168.0.245")
DEFAULT_PORT = int(os.environ.get("TACTILITY_PORT", "6667"))

KEY_CODEPOINTS = {
    name.lower(): codepoint
    for name, codepoint in remote_viewer.CODEPOINTS.items()
}
KEY_CODEPOINTS.update({
    "enter": 10,
    "return": 10,
    "space": 32,
    "esc": 0x1B,
    "backspace": 8,
    "del": 0x7F,
    "delete": 0x7F,
    "tab": 9,
})


def fail(message):
    print(f"device_ui: {message}", file=sys.stderr)
    sys.exit(1)


def parse_point(text):
    try:
        x, y = text.replace("x", ",").split(",")[:2]
        return int(x.strip()), int(y.strip())
    except (TypeError, ValueError):
        fail(f"expected X,Y but got {text!r}")


def parse_key(text):
    if text.lower() in KEY_CODEPOINTS:
        return KEY_CODEPOINTS[text.lower()]
    try:
        return int(text, 0)
    except ValueError:
        pass
    if len(text) == 1:
        return ord(text)
    fail(f"unknown key {text!r} (try a character, a codepoint like 0x0A, or a name like enter/escape/tab)")


def build_steps(tokens):
    """Turns `tap 1,2 wait 1 key enter` into a list of (kind, value) tuples."""
    steps = []
    index = 0
    while index < len(tokens):
        command = tokens[index]
        if command == "wait":
            if index + 1 >= len(tokens):
                fail("wait needs a number of seconds")
            steps.append(("wait", float(tokens[index + 1])))
            index += 2
        elif command in ("tap", "press", "release"):
            if index + 1 >= len(tokens):
                fail(f"{command} needs X,Y")
            steps.append((command, parse_point(tokens[index + 1])))
            index += 2
        elif command == "swipe":
            if index + 2 >= len(tokens):
                fail("swipe needs a start and an end point: swipe X1,Y1 X2,Y2")
            steps.append(("swipe", (parse_point(tokens[index + 1]), parse_point(tokens[index + 2]))))
            index += 3
        elif command == "type":
            if index + 1 >= len(tokens):
                fail("type needs text")
            steps.append(("type", tokens[index + 1]))
            index += 2
        elif command == "key":
            if index + 1 >= len(tokens):
                fail("key needs a key name or character")
            steps.append(("key", parse_key(tokens[index + 1])))
            index += 2
        else:
            fail(f"unknown step {command!r} (use tap, press, release, type, key or wait)")
    return steps


def to_native(x, y, width, height, orientation):
    """Converts a point in the UI (as shown in a screenshot) to the display's pixels."""
    ui_rotation = orientation & remote_viewer.ORIENTATION_ROTATION_MASK
    software_rotation = orientation & remote_viewer.ORIENTATION_SOFTWARE
    turns = (ui_rotation if software_rotation else 0) & 3
    view_width, view_height = (height, width) if turns & 1 else (width, height)
    native_x, native_y, _, _ = remote_viewer.undo_quarter_turns(x, y, view_width, view_height, ui_rotation)
    return native_x, native_y


def run_steps(host, port, steps, options):
    connection = shell.open_session(host, port, "remote")
    frame_type, payload = connection.read_frame(10)
    if frame_type == shell.FRAME_FAIL:
        fail(payload.decode("utf-8", errors="replace"))
    state = remote_viewer.parse_state(payload)
    if state is None:
        fail("the device did not describe its display")

    width, height, color_format, swap_xy, mirror_x, mirror_y, invert, on, orientation = state
    if options.verbose:
        print(
            f"display {width}x{height}, color format {color_format}, "
            f"orientation {orientation}, swap {swap_xy}, mirror {mirror_x}/{mirror_y}"
        )

    session = remote_viewer.RemoteSession(connection)
    session.start()
    session.set_inputs(remote_viewer.INPUT_KEYBOARD | remote_viewer.INPUT_TOUCH)
    # The device creates its virtual keyboard/touch devices in response to the
    # enable frame; input sent before that finishes is lost.
    time.sleep(options.settle_delay)

    def touch(point):
        native = point if options.native else to_native(*point, width, height, orientation)
        if options.verbose:
            print(f"tap {point} -> {native}")
        session.send_touch([native])

    try:
        for kind, value in steps:
            if kind == "wait":
                time.sleep(value)
            elif kind == "tap":
                touch(value)
                time.sleep(options.tap_duration)
                session.send_touch([])
            elif kind == "press":
                touch(value)
            elif kind == "release":
                session.send_touch([])
            elif kind == "swipe":
                start, end = value
                start_native = start if options.native else to_native(*start, width, height, orientation)
                end_native = end if options.native else to_native(*end, width, height, orientation)
                steps_count = options.swipe_steps
                for step in range(steps_count + 1):
                    fraction = step / steps_count
                    point = (
                        round(start_native[0] + (end_native[0] - start_native[0]) * fraction),
                        round(start_native[1] + (end_native[1] - start_native[1]) * fraction),
                    )
                    session.send_touch([point])
                    time.sleep(options.swipe_duration / steps_count)
                session.send_touch([])
            elif kind == "type":
                for character in value:
                    codepoint = 10 if character == "\n" else ord(character)
                    session.send_key(codepoint, True)
                    time.sleep(options.key_delay)
                    session.send_key(codepoint, False)
                    time.sleep(options.key_delay)
            elif kind == "key":
                session.send_key(value, True)
                time.sleep(options.key_delay)
                session.send_key(value, False)
            time.sleep(options.step_delay)
    finally:
        session.send_touch([])
        session.close()
        time.sleep(0.2)


def take_screenshot(host, port, path):
    exit_code, output = shell.run_command(host, port, "screenshot", 30)
    if exit_code != 0:
        fail(output.strip() or "the device failed to take a screenshot")
    remote_path = output.strip().splitlines()[-1]

    try:
        shell.run_pull(argparse.Namespace(
            host=host, port=port, remote_path=remote_path, local_path=path
        ))
    finally:
        shell.run_command(host, port, f"rm {remote_path}")


def main():
    parser = argparse.ArgumentParser(
        prog="device_ui",
        description="tap, type and press keys on a Tactility device over the development server",
        epilog=(
            "steps: tap X,Y | press X,Y | release | swipe X1,Y1 X2,Y2 | type TEXT | key NAME | wait SECONDS. "
            "Add 'shot PATH' as the first (and only) step to take a screenshot."
        ),
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"device address (default {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"development server port (default {DEFAULT_PORT})")
    parser.add_argument("--native", action="store_true", help="coordinates are display pixels, not UI/screenshot pixels")
    parser.add_argument("--tap-duration", type=float, default=0.15, help="seconds a tap stays pressed (default 0.15)")
    parser.add_argument("--step-delay", type=float, default=0.4, help="seconds between steps (default 0.4)")
    parser.add_argument("--settle-delay", type=float, default=1.0, help="seconds after enabling input before the first step (default 1.0)")
    parser.add_argument("--swipe-duration", type=float, default=0.4, help="seconds a swipe takes (default 0.4)")
    parser.add_argument("--swipe-steps", type=int, default=12, help="samples sent during a swipe (default 12)")
    parser.add_argument("--key-delay", type=float, default=0.06, help="seconds between key press and release (default 0.06)")
    parser.add_argument("--verbose", action="store_true", help="print display state and converted coordinates")
    parser.add_argument("steps", nargs=argparse.REMAINDER, help="steps to run")
    args = parser.parse_args()

    if not args.steps:
        parser.print_help()
        return 1

    if args.steps[0] == "shot":
        path = args.steps[1] if len(args.steps) > 1 else (
            datetime.datetime.now().strftime("screenshot-%Y%m%d-%H%M%S.png")
        )
        take_screenshot(args.host, args.port, path)
        return 0

    run_steps(args.host, args.port, build_steps(args.steps), args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
