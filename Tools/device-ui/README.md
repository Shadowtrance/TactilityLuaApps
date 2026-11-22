# device-ui

Headless tap/type/key control for a running Tactility device, built on the
development server's remote session (the same protocol as `tt remote`). Useful
for driving an app from a terminal or an agent while iterating on UI.

## Usage

Run it with Python 3. It finds the SDK tools automatically (or set
`TACTILITY_SDK_TOOLS`), and talks to `192.168.0.245` by default. Override with
`--host`, or the `TACTILITY_HOST` / `TACTILITY_PORT` environment variables.

```powershell
# One tap: use coordinates from a `tt screenshot` (UI pixels)
py Tools\device-ui\device_ui.py tap 1200,157

# Several steps in one session
py Tools\device-ui\device_ui.py tap 1200,157 wait 0.8 type "demo" tap 544,664

# Text and special keys
py Tools\device-ui\device_ui.py type "result = 'hi'" key enter

# Screenshot (a separate, short connection)
py Tools\device-ui\device_ui.py shot out.png
```

Steps:

| Step | Meaning |
| --- | --- |
| `tap X,Y` | press and release at a point |
| `press X,Y` | put a finger down (combine with `release` for long presses or drags) |
| `release` | lift all fingers |
| `swipe X1,Y1 X2,Y2` | drag from one point to another (scrolling, sliders) |
| `type TEXT` | type text through the keyboard input (use `\n` for Enter) |
| `key NAME` | one key: `enter`, `escape`, `tab`, `backspace`, `delete`, arrows, `home`, `end`, a single character, or a codepoint like `0x0A` |
| `wait SECONDS` | pause |
| `shot PATH` | take a screenshot and pull it (must be the only step) |

Options: `--native` (coordinates are raw display pixels instead of screenshot
pixels), `--tap-duration`, `--step-delay`, `--key-delay`, `--settle-delay`,
`--verbose`.

## Notes

- Coordinates are the pixels of a `tt screenshot`; the tool converts them to the
  display's native orientation using the orientation reported by the device.
- The device creates its virtual input devices when the session starts. The
  tool waits one second (`--settle-delay`) before the first step; without that,
  the first touch can be dropped.
- After launching an app, give the window a moment (`wait 2`) before tapping.
- The remote session's touch and keyboard devices only exist while the tool is
  connected; they are removed when it exits.
- On some panels the last few display pixels near a bezel do not report touches.
  In UI space that is usually the left screen edge, so a control drawn there
  (such as the window-close X) may ignore synthetic taps even though a finger
  works. Tap a little inward, or close the app another way.
- If a device has a hardware keyboard attached, no on-screen keyboard appears.
  `type` still works through the remote keyboard input.
