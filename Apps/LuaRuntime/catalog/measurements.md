# Device measurements

Runtime **0.1.13** (code 14), SDK **0.8.0-dev**, firmware **a0439804**
(`Simulator sound & display dimming, window manager dialogs, UI improvements and more`).

Measured on 2026-11-24 with the development server running. App timings come from
the device's own `I (ms)` log timestamps:

- **loader**: `app_manager: starting <app>` → `ELF: elf->entry`
- **window**: `ELF: elf->entry` → `LuaRunner: started`
- **eval**: `LuaRunner: started` → `LuaRunner: script evaluated`
- **total**: starting → evaluated

Boot is wall-clock time from an esptool hard reset to the first successful
development-server command (`free`), polling every 0.5 s. It includes Wi-Fi
association and the service start-up, so it varies with the access point.

## Boot and memory

| Device | Boot to dev server | Heap free (boot) | External free (boot) |
|---|---:|---:|---:|
| Tab5 (ESP32-P4, 32 MB PSRAM) | 40.0 s | 163,827 B | 27,739,204 B |
| T-Deck Plus (ESP32-S3, 8 MB PSRAM) | 35.6 s | 115,787 B | 7,861,888 B |

After running hello and the Tabview/Tileview demo, while the last app was still
open:

| Device | Heap free | External free | Heap used | External used |
|---|---:|---:|---:|---:|
| Tab5 | 115,143 B | 26,856,196 B | ~47.5 KB | ~862 KB |
| T-Deck Plus | 67,019 B | 7,208,456 B | ~47.6 KB | ~638 KB |

The internal-heap cost is essentially the same on both; the external figure
differs because the two heaps have different allocator overhead and PSRAM
carve-outs.

## App start

`hello` is one label; `tabsdemo` is a Tabview + Tileview with a label and a
button. Three runs for hello, two for tabsdemo, in milliseconds.

| Device | App | Loader | Window | Eval | Total |
|---|---|---:|---:|---:|---:|
| Tab5 | hello | 30 | 176 | 28 | 234 |
| Tab5 | tabsdemo | 31 | 176 | 48 | 254 |
| T-Deck Plus | hello | 139 | 1,392 | 48 | 1,579 |
| T-Deck Plus | tabsdemo | 136 | 1,384 | 128 | 1,648 |

## Observations

- Script evaluation itself is fast on both: tens of milliseconds for a small
  script, ~50–150 ms for the tab/tile demo.
- The S3 ELF load is ~4.5× slower than the P4 (140 ms vs 30 ms), consistent with
  PSRAM text relocation on the smaller part.
- **The S3's dominant cost is the window/app start (~1.4 s), not the loader or
  Lua.** The P4 does the same step in ~175 ms. That is the first thing to
  profile if S3 app launches need to feel faster; it is before the Lua runtime
  is allocated and before the script runs.
- Both devices have enough PSRAM for the runtime and a typical script; the S3
  has ~7.2 MB external free with the demo open.

Raw run durations are in `build/measure_devices.py`'s output; the script is a
local measurement helper and is not packaged.
