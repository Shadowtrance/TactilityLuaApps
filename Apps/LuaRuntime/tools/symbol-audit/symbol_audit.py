#!/usr/bin/env python3
"""Audit the Lua runner's unresolved symbols against module exports.

The runner is a side-loaded ELF. At load time it resolves `lv_*` from the
`lvgl-module` and `lua_*` from the `lua_runtime` module. A symbol that is
present at link time but absent from a module's export table makes the app fail
to start, so every new binding needs a matching `DEFINE_MODULE_SYMBOL`.

This tool reads the built runner's dynamic undefined symbols with `nm`, parses
the module `symbols.c` tables and reports the difference. `--fix` appends the
missing entries to the correct table.

Examples:
    py Apps\\LuaRuntime\\tools\\symbol-audit\\symbol_audit.py --check
    py Apps\\LuaRuntime\\tools\\symbol-audit\\symbol_audit.py --fix
    py Apps\\LuaRuntime\\tools\\symbol-audit\\symbol_audit.py --elf path\\to\\lua_runner.elf
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

RUNTIME_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ELF = RUNTIME_ROOT / "build" / "package-intermediate" / "bin" / "esp32p4" / "lua_runner.elf"
DEFAULT_RUNTIME_SYMBOLS = RUNTIME_ROOT / "lua_runtime" / "main" / "Source" / "symbols.c"
SYMBOL_RE = re.compile(r"DEFINE_MODULE_SYMBOL\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")
NM_RE = re.compile(r"\bU\s+([A-Za-z_][A-Za-z0-9_]*)\s*$")
DEFAULT_PREFIXES = ("lv_", "lvgl_", "lua_", "luaL_", "luaopen_")


def find_lvgl_symbols() -> Path | None:
    for parent in Path(__file__).resolve().parents:
        candidate = parent / "TactilityWork" / "Modules" / "lvgl-module" / "source" / "symbols.c"
        if candidate.exists():
            return candidate
    return None


def find_nm() -> Path | None:
    tools_path = os.environ.get("IDF_TOOLS_PATH")
    roots = [Path(tools_path)] if tools_path else []
    roots.extend([Path("C:/Espressif/tools"), Path.home() / ".espressif" / "tools"])
    candidates: list[Path] = []
    for root in roots:
        candidates.extend(root.glob("riscv32-esp-elf-*/riscv32-esp-elf/bin/riscv32-esp-elf-nm.exe"))
        candidates.extend(root.glob("riscv32-esp-elf/esp-*/riscv32-esp-elf/bin/riscv32-esp-elf-nm.exe"))
    if not candidates:
        return None

    def version_key(path: Path) -> tuple:
        match = re.search(r"esp-([0-9.]+)_", str(path))
        return tuple(int(part) for part in match.group(1).split(".")) if match else ()

    return max(candidates, key=version_key)


def undefined_symbols(nm: Path, elf: Path) -> list[str]:
    result = subprocess.run(
        [str(nm), "-D", "-u", str(elf)],
        capture_output=True,
        text=True,
        check=False,
    )
    output = (result.stdout or "") + (result.stderr or "")
    if "no symbols" in output.lower():
        result = subprocess.run(
            [str(nm), "-u", str(elf)],
            capture_output=True,
            text=True,
            check=False,
        )
        output = (result.stdout or "") + (result.stderr or "")

    names: set[str] = set()
    for line in output.splitlines():
        match = NM_RE.search(line)
        if match:
            names.add(match.group(1))
    return sorted(names)


def exported_symbols(paths: list[Path]) -> tuple[set[str], dict[str, Path]]:
    exported: set[str] = set()
    owners: dict[str, Path] = {}
    for path in paths:
        text = path.read_text(encoding="utf-8")
        for name in SYMBOL_RE.findall(text):
            exported.add(name)
            owners.setdefault(name, path)
    return exported, owners


def owner_for(name: str, paths: list[Path]) -> Path | None:
    if name.startswith(("lv_", "lvgl_")):
        for path in paths:
            if "lvgl-module" in str(path):
                return path
    if name.startswith(("lua_", "luaL_", "luaopen_")):
        for path in paths:
            if "lua_runtime" in str(path):
                return path
    return None


def append_symbols(path: Path, names: list[str]) -> None:
    text = path.read_text(encoding="utf-8")
    marker = "    // Added by Apps/LuaRuntime/tools/symbol-audit\n"
    if marker not in text:
        insertion = marker + "".join(f"    DEFINE_MODULE_SYMBOL({name}),\n" for name in names)
        if "// GENERATED END" in text:
            text = text.replace("// GENERATED END", insertion + "// GENERATED END", 1)
        elif "MODULE_SYMBOL_TERMINATOR" in text:
            text = text.replace(
                "    MODULE_SYMBOL_TERMINATOR",
                insertion + "    MODULE_SYMBOL_TERMINATOR",
                1,
            )
        else:
            raise SystemExit(f"{path}: no insertion point found")
    else:
        # Add to the existing block, just before the next terminator.
        for name in names:
            entry = f"    DEFINE_MODULE_SYMBOL({name}),\n"
            if entry in text:
                continue
            if "// GENERATED END" in text:
                text = text.replace("// GENERATED END", entry + "// GENERATED END", 1)
            else:
                text = text.replace("    MODULE_SYMBOL_TERMINATOR", entry + "    MODULE_SYMBOL_TERMINATOR", 1)
    path.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, default=DEFAULT_ELF)
    parser.add_argument(
        "--symbols",
        type=Path,
        action="append",
        help="symbols.c file to read (repeatable; defaults to lua_runtime + lvgl-module)",
    )
    parser.add_argument("--nm", type=Path, help="path to riscv32-esp-elf-nm")
    parser.add_argument("--prefix", action="append", help="symbol prefix to audit (repeatable)")
    parser.add_argument("--fix", action="store_true", help="append missing symbols to the owning table")
    parser.add_argument("--check", action="store_true", help="exit 1 when symbols are missing")
    parser.add_argument("--json", action="store_true", help="print JSON")
    args = parser.parse_args()

    if not args.elf.exists():
        raise SystemExit(
            f"{args.elf}: missing. Build the LuaRuntime app first, or pass --elf."
        )

    symbol_files = list(args.symbols or [])
    if not symbol_files:
        if DEFAULT_RUNTIME_SYMBOLS.exists():
            symbol_files.append(DEFAULT_RUNTIME_SYMBOLS)
        lvgl_file = find_lvgl_symbols()
        if lvgl_file is not None:
            symbol_files.append(lvgl_file)
    for path in symbol_files:
        if not path.exists():
            raise SystemExit(f"{path}: no such symbols file")

    nm = args.nm or find_nm()
    if nm is None or not nm.exists():
        raise SystemExit("riscv32-esp-elf-nm not found; pass --nm")

    prefixes = tuple(args.prefix or DEFAULT_PREFIXES)
    undefined = [name for name in undefined_symbols(nm, args.elf) if name.startswith(prefixes)]
    exported, _owners = exported_symbols(symbol_files)
    missing = sorted(set(undefined) - exported)

    if missing and args.fix:
        grouped: dict[Path, list[str]] = {}
        unresolved: list[str] = []
        for name in missing:
            path = owner_for(name, symbol_files)
            if path is None:
                unresolved.append(name)
            else:
                grouped.setdefault(path, []).append(name)
        for path, names in grouped.items():
            append_symbols(path, names)
            print(f"symbol audit: added {len(names)} symbol(s) to {path}")
        missing = unresolved
        if missing:
            print("symbol audit: no owning table for: " + ", ".join(missing))

    report = {
        "elf": str(args.elf),
        "symbols": [str(path) for path in symbol_files],
        "undefined": len(undefined),
        "exported": len(exported),
        "missing": missing,
    }
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(
            f"symbol audit: {len(undefined)} runtime symbols, "
            f"{len(exported)} module exports, {len(missing)} missing"
        )
        for name in missing:
            print(f"  MISSING {name}")
        if not missing:
            print("symbol audit: every runtime symbol is exported")

    if missing and (args.check or not args.fix):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
