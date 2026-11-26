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
- **The S3's dominant cost is symbol relocation, not the window or Lua.** With a
  temporary log on the first line of `lua_runner`'s `main()`, the earlier
  "window" segment split into ~1,400 ms before `main()` and ~20 ms for path
  resolution. `ELF: elf->entry` is logged after the segments are copied but
  *before* `esp_elf_relocate()` runs, so the label was wrong; see the update
  below.
- Both devices have enough PSRAM for the runtime and a typical script; the S3
  has ~7.2 MB external free with the demo open.

## Update 2026-11-25: where the S3 launch time goes

Firmware **ac42cd31** reproduces the same timings (hello: loader ~140 ms,
"window" ~1,420 ms, eval 50–100 ms). Adding a log at the start of
`lua_runner`'s `main()` shows the gap is entirely **before the app runs**:

| Segment | T-Deck Plus |
|---|---:|
| `app_manager: starting` → `ELF: elf->entry` (segments copied) | ~140 ms |
| `ELF: elf->entry` → `main()` (relocation + cache sync) | ~1,400 ms |
| `main()` → path/API resolved | ~20 ms |
| Lua evaluation | 50–100 ms |

`esp_elf.c` logs `elf->entry` after `esp_elf_load_*()` but before its relocation
loop, so almost the whole 1.4 s is relocation. The runner ELF has ~2,200
relocations, 483 of them `R_XTENSA_JMP_SLOT` for external functions. The ELF
loader's resolver calls `module_resolve_symbol_global()`, which linearly scans
every module's symbol table (~2,600 exported symbols across ~30 modules) under
the module-ledger mutex for each name. That turns ~550 lookups into hundreds of
thousands of `strcmp` calls against PSRAM-backed tables; the Tab5's faster
memory and cache do the same work in ~170 ms.

The fix belongs in the kernel: index module symbols (a hash table or a
per-module Bloom filter to skip modules that cannot contain the name) instead of
scanning all of them per lookup. No app-side change can remove the relocations.
This was implemented on 2026-11-26; see the update below.

Raw run durations are in `build/measure_devices.py`'s output; the script is a
local measurement helper and is not packaged.

## Update 2026-11-26: kernel symbol index

Firmware **ac42cd31** plus a local kernel change: while an ELF is relocated, each
module gets a small FNV-1a hash index of its exported names, so a global lookup
probes one bucket per module instead of `strcmp`-ing every symbol. The loader's
existing `module_refs_begin()`/`module_refs_end()` pair brackets the work, so
the indexes live in one arena allocation only for the duration of
`esp_elf_relocate()` and are freed afterwards. Idle systems keep **no** index
memory; the linear scan remains the fallback when the allocation fails or a
module has more than 65,535 symbols.

Three launches of **hello 0.1.2 (code 3)** after a hard reset:

| Segment | Before (2026-11-25) | After |
|---|---:|---:|
| loader (`starting` → `elf->entry`) | 135–142 ms | 143–145 ms |
| `elf->entry` → `LuaRunner: started` | 1,382–1,390 ms | 55–69 ms |
| eval (`started` → `script evaluated`) | 50–100 ms | 48–101 ms |
| **total** | **~1.6 s** | **248–313 ms** |

The first launch is the slow one (69 ms relocation, 101 ms eval) because the
script and its fonts are cold; the next two relocate in 55 ms and evaluate in
48–49 ms. The bulk of the remaining relocation time is the 1,664
`R_XTENSA_RELATIVE` stores and the single cache flush, not name lookup.

The arena is ~15.4 KB for the 2,647 names exported by the firmware build
(`2 * bucket_count + 2 * count` per module), plus allocator overhead. Because it
is only held during relocation, that is a peak rather than a permanent cost;
the earlier resident variant kept it at boot, which is what the 98,647 B vs
115,787 B boot comparison measured. Building it costs ~2 ms per launch (hashing
the names and one allocation).

The kernel test suite covers the indexed path, duplicate names (the first table
entry still wins), nested acquire/release, removal while an index is active, and
resolution with no index at all (`TactilityKernelTests`, 229 cases).

### Runner/runtime integer width

hello initially aborted in the runtime module right after `LuaRunner: started`,
with 103,767 B of heap free, so it was not an allocation failure. The installed
package was older than 0.1.24: a script package embeds its own runner, and
0.1.24 changed `lua_Integer` to 64-bit. Reinstalling hello 0.1.2 (code 3)
fixed it. Any third-party script package built before 0.1.24 needs the same
repackage; the script itself is unchanged.
