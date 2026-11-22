#!/usr/bin/env python3
"""Generate an SDK coverage report for the Lua runtime.

The SDK does not ship a single list of the symbols a side-loaded app may call.
This tool builds one from two sources:

  * the firmware's `symbols.c` export tables, when the firmware tree is available;
  * the SDK headers, for modules that do not have an export table and for
    mapping symbols back to a public header.

It then classifies each symbol against the Lua runtime:

  generated  - listed as `c = "..."` in bindings/*.toml
  bound      - used by lua_runtime/main/Source/lua or lua_runner/main/Source/luavgl
  raw-only   - only referenced through tactility.raw.call(...) in a script
  unbound    - exported/declared but not reachable from a script yet

Output is written to <app>/catalog (override with --output):
  sdk-symbols.csv        full machine-readable inventory
  sdk-coverage.json      same data, grouped
  sdk-coverage.md        human-readable summary and unbound list

Run with --check to fail when a generated binding's C symbol is absent from the
SDK. That makes SDK drift visible during a build instead of at runtime.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import re
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

APP_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_FIRMWARE = APP_ROOT.parents[2] / "TactilityWork"
DEFAULT_OUTPUT = APP_ROOT / "catalog"

# Modules whose public headers are worth cataloging. Third-party libraries
# (lvgl core, minitar, minmea, FreeRTOS) are reached through these modules.
MODULE_NAMES = [
    "TactilityKernel",
    "lvgl-module",
    "app-module",
    "app-hub-module",
    "audio-decoder-module",
    "binfont-module",
    "bluetooth-module",
    "cjson-module",
    "crypt-module",
    "gps-module",
    "graphics-module",
    "http-module",
    "localisation-module",
    "location-module",
    "lvgl-window-manager-module",
    "notification-module",
    "platform-esp32",
    "service-module",
    "wifi-module",
]

MODULE_TITLES = {
    "TactilityKernel": "Kernel / drivers",
    "lvgl-module": "UI / LVGL",
    "app-module": "App runtime",
    "app-hub-module": "App hub",
    "audio-decoder-module": "Audio",
    "binfont-module": "Fonts",
    "bluetooth-module": "Bluetooth",
    "cjson-module": "JSON",
    "crypt-module": "Cryptography",
    "gps-module": "GPS",
    "graphics-module": "Graphics",
    "http-module": "HTTP",
    "localisation-module": "Localisation",
    "location-module": "Location",
    "lvgl-window-manager-module": "Window manager",
    "notification-module": "Notification",
    "platform-esp32": "Platform",
    "service-module": "Services",
    "wifi-module": "Wi-Fi",
}

STATUS_ORDER = ["generated", "bound", "raw-only", "raw-missing", "unbound", "declared"]


@dataclass
class Symbol:
    module: str
    name: str
    status: str
    header: str
    source: str  # "export" (symbols.c) or "header"
    note: str = ""


def find_sdk() -> Path | None:
    env = os.environ.get("TACTILITY_SDK_PATH")
    if env:
        candidate = Path(env) / "platform-esp32p4" / "TactilitySDK"
        if candidate.is_dir():
            return candidate
        candidate = Path(env) / "TactilitySDK"
        if candidate.is_dir():
            return candidate

    root = Path.home() / ".tactility" / "sdk"
    if not root.is_dir():
        return None

    versions = sorted(
        (p for p in root.iterdir() if p.is_dir()),
        key=lambda p: p.name,
        reverse=True,
    )
    for version in versions:
        for platform in ("platform-esp32p4", "platform-esp32s3"):
            candidate = version / platform / "TactilitySDK"
            if candidate.is_dir():
                return candidate
    return None


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def read_metadata(sdk: Path, firmware: Path | None) -> dict[str, str | bool | None]:
    """Provenance for the report: SDK commit and firmware checkout state."""
    metadata: dict[str, str | bool | None] = {
        "sdk_version": read_text(sdk / "version.txt").strip() or None,
        "idf_version": read_text(sdk / "idf-version.txt").strip() or None,
        "sdk_commit": read_text(sdk.parent / "commit").strip() or None,
        "firmware_commit": None,
        "firmware_dirty": None,
    }

    if firmware is None:
        return metadata

    try:
        commit = subprocess.run(
            ["git", "-C", str(firmware), "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        if commit.returncode == 0:
            metadata["firmware_commit"] = commit.stdout.strip()

        status = subprocess.run(
            ["git", "-C", str(firmware), "status", "--porcelain"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        if status.returncode == 0:
            metadata["firmware_dirty"] = bool(status.stdout.strip())
    except (OSError, subprocess.SubprocessError):
        pass

    return metadata


# ---------------------------------------------------------------------------
# Symbol sources


def parse_symbols_c(path: Path) -> set[str]:
    """Parse DEFINE_MODULE_SYMBOL()/DEFINE_MODULE_SYMBOL_ALIAS() entries."""
    text = read_text(path)
    names: set[str] = set()

    # Comments are not part of an entry; removing them avoids matching examples.
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)

    for match in re.finditer(r"DEFINE_MODULE_SYMBOL\s*\(\s*([A-Za-z_]\w*)\s*\)", text):
        names.add(match.group(1))

    for match in re.finditer(
        r'DEFINE_MODULE_SYMBOL_ALIAS\s*\(\s*"([^"]+)"\s*,\s*([A-Za-z_]\w*)\s*\)',
        text,
    ):
        names.add(match.group(1))

    return names


# A function declaration: a name followed by parentheses, ending in a semicolon.
# The broad pattern is filtered by the prefix checks below to keep typedefs,
# macros and struct declarations out.
DECLARATION_RE = re.compile(
    r"(?m)^[ \t]*(?!#)(?:[A-Za-z_][^;{}\n]*?)\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*;",
)

EXCLUDED_DECLARATION_PREFIXES = (
    "typedef",
    "using ",
    "template",
    "class ",
    "namespace ",
    "static ",
    "inline ",
    "#",
)

EXCLUDED_NAMES = {
    "if",
    "for",
    "while",
    "switch",
    "return",
    "sizeof",
    "alignof",
    "static_assert",
}


def parse_header_declarations(include_root: Path) -> dict[str, str]:
    """Map function name -> header path for every public header under a root."""
    found: dict[str, str] = {}
    if not include_root.is_dir():
        return found

    for header in sorted(include_root.rglob("*.h")):
        text = read_text(header)
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)

        try:
            relative = header.relative_to(include_root)
        except ValueError:
            relative = header.name

        for match in DECLARATION_RE.finditer(text):
            name = match.group(1)
            if name in EXCLUDED_NAMES:
                continue

            line_start = text.rfind("\n", 0, match.start()) + 1
            line = text[line_start : match.end()].lstrip()
            if line.startswith(EXCLUDED_DECLARATION_PREFIXES):
                continue

            # Keep the first header that declares a name.
            found.setdefault(name, str(relative).replace("\\", "/"))

    return found


def module_include_roots(sdk: Path, module: str) -> list[Path]:
    """Include roots that matter for public symbol lookup."""
    if module == "TactilityKernel":
        return [sdk / "Libraries" / "TactilityKernel" / "include"]

    roots = [sdk / "Modules" / module / "include"]

    # The wrapper module re-exports the LVGL core, whose headers live in the
    # LVGL library rather than the module.
    if module == "lvgl-module":
        roots.append(sdk / "Libraries" / "lvgl" / "include" / "include" / "lvgl")

    return roots


def firmware_symbols(firmware: Path, module: str) -> set[str]:
    candidates = []
    if module == "TactilityKernel":
        candidates.append(firmware / "TactilityKernel" / "source" / "symbols.c")
    else:
        # Modules either keep their export table in symbols.c or in module.cpp.
        candidates.append(firmware / "Modules" / module / "source" / "symbols.c")
        candidates.append(firmware / "Modules" / module / "source" / "module.cpp")

    names: set[str] = set()
    for candidate in candidates:
        if candidate.is_file():
            names.update(parse_symbols_c(candidate))
    return names


# ---------------------------------------------------------------------------
# Lua-side references


def generated_symbols(app_root: Path) -> set[str]:
    """C symbols declared by the capability IDL.

    `impl = "custom"` entries name a hand-written Lua C function, not an SDK symbol,
    so they are excluded from the generated set and from the drift check.
    """
    result: set[str] = set()
    for path in (app_root / "bindings").glob("*.toml"):
        text = read_text(path)
        for block in re.split(r"\[\[function\]\]", text)[1:]:
            if re.search(r'^\s*impl\s*=\s*"custom"', block, flags=re.M):
                continue
            match = re.search(r'^\s*c\s*=\s*"([A-Za-z_]\w*)"', block, flags=re.M)
            if match:
                result.add(match.group(1))
    return result


def binding_tokens(app_root: Path) -> set[str]:
    """Identifiers used by the Lua-facing C bindings (not the app front-ends)."""
    tokens: set[str] = set()
    roots = [
        app_root / "lua_runtime" / "main" / "Source" / "lua",
        app_root / "lua_runner" / "main" / "Source" / "luavgl",
    ]

    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*.cpp"):
            tokens.update(re.findall(r"[A-Za-z_]\w*", read_text(path)))
        for path in root.rglob("*.h"):
            tokens.update(re.findall(r"[A-Za-z_]\w*", read_text(path)))

    return tokens


def raw_symbols(app_root: Path) -> set[str]:
    """Symbol names passed to tactility.raw.call() in scripts and docs.

    Negative tests deliberately call a non-existent name; those are not drift.
    """
    result: set[str] = set()
    roots = [
        app_root / "assets",
        app_root / "tests",
    ]
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*.lua"):
            text = read_text(path)
            for name in re.findall(
                r"tactility\.raw\.call\s*\(\s*[\"']([A-Za-z_]\w*)[\"']",
                text,
            ):
                if name.startswith("definitely_"):
                    continue
                result.add(name)
    return result


# ---------------------------------------------------------------------------
# Cataloging


def classify(
    name: str,
    generated: set[str],
    tokens: set[str],
    raw: set[str],
    declared_only: bool,
) -> str:
    if name in generated:
        return "generated"
    if name in tokens:
        return "bound"
    if name in raw:
        return "raw-only"
    return "declared" if declared_only else "unbound"


def collect(sdk: Path, firmware: Path | None, app_root: Path) -> list[Symbol]:
    generated = generated_symbols(app_root)
    tokens = binding_tokens(app_root)
    raw = raw_symbols(app_root)

    symbols: dict[tuple[str, str], Symbol] = {}

    for module in MODULE_NAMES:
        exports = firmware_symbols(firmware, module) if firmware else set()

        header_map: dict[str, str] = {}
        for root in module_include_roots(sdk, module):
            header_map.update(parse_header_declarations(root))

        if exports:
            source = "export"
            names = exports
        else:
            source = "header"
            names = set(header_map)

        for name in names:
            status = classify(name, generated, tokens, raw, declared_only=source == "header")
            header = header_map.get(name, "")
            note = ""
            if source == "header" and not header:
                note = "no public header found"
            key = (module, name)
            symbols[key] = Symbol(
                module=module,
                name=name,
                status=status,
                header=header,
                source=source,
                note=note,
            )

    return sorted(symbols.values(), key=lambda s: (s.module, s.status, s.name))


def add_missing_raw_calls(symbols: list[Symbol], app_root: Path) -> list[Symbol]:
    """Raw call names that are not exported/declared: drift worth surfacing."""
    known = {symbol.name for symbol in symbols}
    for name in sorted(raw_symbols(app_root) - known):
        symbols.append(
            Symbol(
                module="script",
                name=name,
                status="raw-missing",
                header="",
                source="script",
                note="tactility.raw.call() name not found in the SDK catalog",
            )
        )
    return sorted(symbols, key=lambda s: (s.module, s.status, s.name))


def status_counts(symbols: list[Symbol]) -> dict[str, int]:
    counts = {status: 0 for status in STATUS_ORDER}
    for symbol in symbols:
        counts[symbol.status] = counts.get(symbol.status, 0) + 1
    return counts


def write_csv(symbols: list[Symbol], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(
            file,
            fieldnames=["module", "name", "status", "source", "header", "note"],
        )
        writer.writeheader()
        for symbol in symbols:
            writer.writerow(asdict(symbol))


def write_json(
    symbols: list[Symbol],
    path: Path,
    sdk: Path,
    firmware: Path | None,
    metadata: dict[str, str | bool | None],
) -> None:
    payload = {
        "generated_at": dt.datetime.now().isoformat(timespec="seconds"),
        "sdk": str(sdk),
        "firmware": str(firmware) if firmware else None,
        **metadata,
        "symbols": [asdict(symbol) for symbol in symbols],
    }
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def prefix_of(name: str) -> str:
    """A coarse grouping for long symbol lists (e.g. lv_obj_*)."""
    for separator in ("_",):
        if separator in name:
            parts = name.split(separator)
            if len(parts) >= 2:
                return parts[0] + "_" + parts[1].split("_")[0]
    return name


def write_markdown(
    symbols: list[Symbol],
    path: Path,
    sdk: Path,
    firmware: Path | None,
    metadata: dict[str, str | bool | None],
) -> None:
    lines: list[str] = []
    lines.append("# SDK coverage")
    lines.append("")
    lines.append(
        "Generated by `tools/sdk-catalog/catalog.py`. Do not edit by hand: "
        "rerun the tool after an SDK or binding change."
    )
    lines.append("")
    lines.append(f"- SDK: `{sdk}`")
    if metadata.get("sdk_version"):
        lines.append(f"- SDK version: `{metadata['sdk_version']}`")
    if metadata.get("sdk_commit"):
        lines.append(f"- SDK commit: `{metadata['sdk_commit']}`")
    if metadata.get("idf_version"):
        lines.append(f"- ESP-IDF: `{metadata['idf_version']}`")
    if firmware:
        lines.append(f"- Firmware exports: `{firmware}`")
        if metadata.get("firmware_commit"):
            dirty = " (modified)" if metadata.get("firmware_dirty") else ""
            lines.append(f"- Firmware commit: `{metadata['firmware_commit']}`{dirty}")
    else:
        lines.append("- Firmware exports: not found; header declarations only")
    lines.append(f"- Generated: {dt.datetime.now().strftime('%Y-%m-%d %H:%M')}")
    lines.append("")

    if (
        metadata.get("sdk_commit")
        and metadata.get("firmware_commit")
        and not str(metadata["firmware_commit"]).startswith(str(metadata["sdk_commit"])[:7])
    ):
        lines.append(
            "> **Note:** the firmware checkout and the installed SDK come from different "
            "commits. Exports from `symbols.c` reflect the checkout; headers reflect the SDK."
        )
        lines.append("")

    lines.append("## Status")
    lines.append("")
    lines.append(
        "- **generated**: declared in `bindings/*.toml` and emitted by the capability generator."
    )
    lines.append(
        "- **bound**: referenced by a hand-written Lua binding in "
        "`lua_runtime/main/Source/lua` or `lua_runner/main/Source/luavgl`."
    )
    lines.append(
        "- **raw-only**: exported and only reached through `tactility.raw.call(\"name\", ...)`."
    )
    lines.append(
        "- **raw-missing**: used by a script through `raw.call`, but absent from the SDK."
    )
    lines.append(
        "- **unbound**: exported (or publicly declared) but not reachable from Lua yet."
    )
    lines.append(
        "- **declared**: a public header function in a module without an export table; "
        "treat it as a candidate, not a confirmed ABI."
    )
    lines.append("")
    lines.append(
        "The full list, including headers and per-symbol notes, is in "
        "`sdk-symbols.csv`. `sdk-coverage.json` has the same data for tooling."
    )
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append(
        "| Module | Source | Total | Generated | Bound | Raw-only | Raw-missing | Unbound | Declared |"
    )
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")

    def summary_row(module: str, module_symbols: list[Symbol]) -> str:
        counts = status_counts(module_symbols)
        source = "exports" if any(s.source == "export" for s in module_symbols) else "headers"
        if module == "script":
            source = "scripts"
        return (
            "| {title} (`{module}`) | {source} | {total} | {generated} | {bound} | "
            "{raw} | {raw_missing} | {unbound} | {declared} |".format(
                title=MODULE_TITLES.get(module, module),
                module=module,
                source=source,
                total=len(module_symbols),
                generated=counts.get("generated", 0),
                bound=counts.get("bound", 0),
                raw=counts.get("raw-only", 0),
                raw_missing=counts.get("raw-missing", 0),
                unbound=counts.get("unbound", 0),
                declared=counts.get("declared", 0),
            )
        )

    for module in MODULE_NAMES:
        module_symbols = [s for s in symbols if s.module == module]
        if module_symbols:
            lines.append(summary_row(module, module_symbols))

    script_symbols = [s for s in symbols if s.module == "script"]
    if script_symbols:
        lines.append(summary_row("script", script_symbols))

    total_counts = status_counts(symbols)
    lines.append(
        "| **All** | | **{total}** | **{generated}** | **{bound}** | **{raw}** | "
        "**{raw_missing}** | **{unbound}** | **{declared}** |".format(
            total=len(symbols),
            generated=total_counts.get("generated", 0),
            bound=total_counts.get("bound", 0),
            raw=total_counts.get("raw-only", 0),
            raw_missing=total_counts.get("raw-missing", 0),
            unbound=total_counts.get("unbound", 0),
            declared=total_counts.get("declared", 0),
        )
    )
    lines.append("")

    raw_symbols_all = [s for s in symbols if s.status in ("raw-only", "raw-missing")]
    if raw_symbols_all:
        lines.append("## Raw calls")
        lines.append("")
        lines.append("| Module | Symbol | Status | Notes |")
        lines.append("|---|---|---|---|")
        for symbol in raw_symbols_all:
            lines.append(
                f"| `{symbol.module}` | `{symbol.name}` | {symbol.status} | {symbol.note or '—'} |"
            )
        lines.append("")

    lines.append("## Unbound symbols by area")
    lines.append("")
    lines.append(
        "Prefix counts make the long tail navigable; use `sdk-symbols.csv` for the "
        "complete row list and headers."
    )
    lines.append("")

    for module in MODULE_NAMES:
        module_symbols = [s for s in symbols if s.module == module and s.status == "unbound"]
        if not module_symbols:
            continue

        counts: dict[str, int] = {}
        for symbol in module_symbols:
            area = prefix_of(symbol.name)
            counts[area] = counts.get(area, 0) + 1

        lines.append(f"### {MODULE_TITLES.get(module, module)} (`{module}`)")
        lines.append("")
        lines.append(f"{len(module_symbols)} unbound symbol(s).")
        lines.append("")
        lines.append("| Area | Count |")
        lines.append("|---|---:|")
        for area, count in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
            lines.append(f"| `{area}` | {count} |")
        lines.append("")

    declared = [s for s in symbols if s.status == "declared"]
    if declared:
        lines.append("## Declared candidates")
        lines.append("")
        lines.append(
            "These modules have public headers but no firmware export table. "
            "Confirm the export before binding; they are the most likely next capabilities."
        )
        lines.append("")

        for module in MODULE_NAMES:
            module_symbols = [s for s in declared if s.module == module]
            if not module_symbols:
                continue

            lines.append(f"### {MODULE_TITLES.get(module, module)} (`{module}`)")
            lines.append("")
            lines.append("| Symbol | Header |")
            lines.append("|---|---|")
            for symbol in module_symbols:
                lines.append(f"| `{symbol.name}` | `{symbol.header or '—'}` |")
            lines.append("")

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def check_generated(symbols: list[Symbol], app_root: Path) -> bool:
    """Fail when an IDL `c =` symbol is not present in the SDK."""
    known = {symbol.name for symbol in symbols}
    missing: list[tuple[Path, str]] = []

    for path in (app_root / "bindings").glob("*.toml"):
        text = read_text(path)
        for block in re.split(r"\[\[function\]\]", text)[1:]:
            if re.search(r'^\s*impl\s*=\s*"custom"', block, flags=re.M):
                continue
            match = re.search(r'^\s*c\s*=\s*"([A-Za-z_]\w*)"', block, flags=re.M)
            if match and match.group(1) not in known:
                missing.append((path, match.group(1)))

    if not missing:
        print("catalog: every generated binding symbol exists in the SDK")
        return True

    print("catalog: generated binding symbols missing from the SDK:")
    for path, name in missing:
        print(f"  {path.relative_to(app_root)}: {name}")
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, help="TactilitySDK directory")
    parser.add_argument(
        "--firmware",
        type=Path,
        help="Firmware source tree with the module symbols.c files",
    )
    parser.add_argument("--app", type=Path, default=APP_ROOT, help="LuaRuntime app directory")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="Catalog directory")
    parser.add_argument(
        "--check",
        action="store_true",
        help="Only verify that generated bindings resolve; write nothing",
    )
    args = parser.parse_args()

    sdk = args.sdk or find_sdk()
    if sdk is None or not sdk.is_dir():
        print("catalog: cannot find the Tactility SDK; use --sdk", file=sys.stderr)
        return 2

    firmware = args.firmware
    if firmware is None:
        firmware = DEFAULT_FIRMWARE if DEFAULT_FIRMWARE.is_dir() else None
    if firmware is not None and not firmware.is_dir():
        firmware = None

    symbols = collect(sdk, firmware, args.app)
    symbols = add_missing_raw_calls(symbols, args.app)
    if not symbols:
        print("catalog: no symbols found", file=sys.stderr)
        return 2

    metadata = read_metadata(sdk, firmware)

    if (
        metadata.get("sdk_commit")
        and metadata.get("firmware_commit")
        and not str(metadata["firmware_commit"]).startswith(str(metadata["sdk_commit"])[:7])
    ):
        print(
            "catalog: warning: firmware checkout "
            f"{metadata['firmware_commit']} differs from SDK "
            f"{str(metadata['sdk_commit'])[:7]}; exports use the checkout",
            file=sys.stderr,
        )

    if args.check:
        return 0 if check_generated(symbols, args.app) else 1

    output: Path = args.output
    write_csv(symbols, output / "sdk-symbols.csv")
    write_json(symbols, output / "sdk-coverage.json", sdk, firmware, metadata)
    write_markdown(symbols, output / "sdk-coverage.md", sdk, firmware, metadata)

    counts = status_counts(symbols)
    print(
        "catalog: {total} symbols ({generated} generated, {bound} bound, "
        "{raw} raw-only, {unbound} unbound, {declared} declared) -> {output}".format(
            total=len(symbols),
            generated=counts.get("generated", 0),
            bound=counts.get("bound", 0),
            raw=counts.get("raw-only", 0),
            unbound=counts.get("unbound", 0),
            declared=counts.get("declared", 0),
            output=output,
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
