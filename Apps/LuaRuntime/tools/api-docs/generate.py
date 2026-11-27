#!/usr/bin/env python3
"""Generate the searchable Lua API reference.

The reference is assembled from the binding sources, so it cannot drift from the
code:

  * ``bindings/*.toml`` for generated capabilities (names, signatures, docs,
    types and constants);
  * the ``luaL_Reg`` tables in the hand-written bindings for function and method
    names, with the doc comment above each C function;
  * the LVGL constant tables and the generated shared-icon list.

Output (in ``catalog/api`` by default):

  * ``index.html`` - self-contained page, no network access required;
  * ``api.json`` - the same data for tooling and tests;
  * ``README.md`` - how to regenerate and what is included.

Run from the app directory:

    py tools/api-docs/generate.py

``--check`` renders in memory and exits non-zero when the committed files are
stale, which is the CI-friendly mode.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

APP_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = APP_ROOT / "catalog" / "api"

RUNTIME_SOURCES = APP_ROOT / "lua_runtime" / "main" / "Source" / "lua"
RUNNER_SOURCES = APP_ROOT / "lua_runner" / "main" / "Source" / "luavgl"
BINDINGS_DIR = APP_ROOT / "bindings"

# Display order and prose for the hand-written modules. Generated modules add
# their own summary from the TOML, and anything missing here still shows up.
MODULE_INFO: dict[str, tuple[str, str]] = {
    "tactility.app": ("App manager", "List, launch, stop, install and uninstall apps."),
    "tactility.device": ("Devices", "Browse the kernel devicetree."),
    "tactility.display": ("Display", "Panel resolution, shape and backlight."),
    "tactility.fs": (
        "Filesystem",
        "Whole-file reads and writes, directory listing and the file/folder picker.",
    ),
    "tactility.log": ("Logging", "Write to the device log."),
    "tactility.notification": (
        "Notifications",
        "Post and dismiss notifications, and subscribe to the event stream.",
    ),
    "tactility.power": ("Power", "Battery state and charging control."),
    "tactility.settings": ("Settings", "Key/value storage owned by the running app."),
    "tactility.system": ("System", "Memory, storage and task snapshots."),
    "tactility.time": ("Time", "Uptime, wall clock and sleeping."),
    "tactility.wifi": ("Wi-Fi", "Radio and station state, and network scans."),
    "tactility.raw": ("Raw escape hatch", "Call exported SDK symbols without a typed binding."),
    "tactility": ("Runtime", "Runtime-wide helpers."),
    "tactility.json": ("JSON", "Encode and decode JSON with the cJSON module."),
    "tactility.http": ("HTTP", "Asynchronous HTTP requests on a worker task."),
    "tactility.ui": ("App UI", "Access to the runner's own toolbar."),
    "lua": ("Script loading", "The module system for multi-file scripts."),
    "tactility.gpio": ("GPIO", "Pins through the kernel GPIO controllers."),
    "tactility.i2c": ("I2C", "Register and block transfers through the I2C controllers."),
    "tactility.adc": ("ADC", "Raw conversions from configured ADC channels."),
    "tactility.uart": ("UART", "Byte and block transfers on UART controllers."),
    "tactility.spi": ("SPI", "One-shot transfers and persistent SPI device handles."),
    "lvgl": ("LVGL", "Widgets, styles, events, animations, timers and theme values."),
}

# luaL_Reg table -> (owner, separator, kind). Owners with a type suffix are
# methods on the handle that the module returns.
TABLE_TARGETS: dict[str, tuple[str, str, str]] = {
    "fs_functions": ("tactility.fs", ".", "function"),
    "picker_functions": ("tactility.fs", ".", "function"),
    "app_functions": ("tactility.app", ".", "function"),
    "device_functions": ("tactility.device", ".", "function"),
    "device_methods": ("tactility.Device", ":", "method"),
    "display_functions": ("tactility.display", ".", "function"),
    "json_functions": ("tactility.json", ".", "function"),
    "http_functions": ("tactility.http", ".", "function"),
    "http_methods": ("tactility.http.Request", ":", "method"),
    "log_functions": ("tactility.log", ".", "function"),
    "notification_functions": ("tactility.notification", ".", "function"),
    "power_functions": ("tactility.power", ".", "function"),
    "settings_functions": ("tactility.settings", ".", "function"),
    "system_functions": ("tactility.system", ".", "function"),
    "time_functions": ("tactility.time", ".", "function"),
    "wifi_functions": ("tactility.wifi", ".", "function"),
    "handle_methods": ("tactility.spi.SpiDevice", ":", "method"),
    "lvgl_functions": ("lvgl", ".", "function"),
    "widget_constructors": ("lvgl", ".", "constructor"),
    "object_methods": ("lvgl.Object", ":", "method"),
    "widget_methods": ("lvgl.Widget", ":", "method"),
    "anim_methods": ("lvgl.Animation", ":", "method"),
    "event_methods": ("lvgl.Event", ":", "method"),
    "group_methods": ("lvgl.Group", ":", "method"),
    "style_methods": ("lvgl.Style", ":", "method"),
    "object_style_methods": ("lvgl.Object", ":", "method"),
    "timer_methods": ("lvgl.Timer", ":", "method"),
    "toolbar_functions": ("lvgl.toolbar", ".", "function"),
    "subscription_methods": ("tactility.notification.Subscription", ":", "method"),
}

GROUP_BY_OWNER: dict[str, str] = {
    "tactility.Device": "Device methods",
    "tactility.spi.SpiDevice": "SpiDevice methods",
    "tactility.notification.Subscription": "Subscriptions",
    "tactility.http.Request": "Request methods",
    "lvgl.Object": "Object methods",
    "lvgl.Widget": "Widget methods",
    "lvgl.Animation": "Animation methods",
    "lvgl.Event": "Event methods",
    "lvgl.Group": "Group methods",
    "lvgl.Style": "Style methods",
    "lvgl.Timer": "Timer methods",
    "lvgl.toolbar": "toolbar",
    "lvgl": "Functions",
}

# Entries the registrations cannot describe on their own.
MANUAL_ENTRIES = [
    {
        "id": "tactility.heap_used",
        "name": "heap_used",
        "owner": "tactility",
        "kind": "function",
        "signature": "tactility.heap_used() -> integer",
        "description": "Bytes the script's runtime has allocated through the kernel allocator.",
        "source": "lua_runtime/main/Source/lua/bindings.cpp",
        "line": 22,
        "group": "Runtime",
    },
    {
        "id": "tactility.raw.call",
        "name": "call",
        "owner": "tactility.raw",
        "kind": "function",
        "signature": "tactility.raw.call(name, ...) -> integer",
        "description": (
            "Resolves an exported SDK symbol and calls it with up to six pointer-sized "
            "arguments (integers, booleans, strings and handles). Untyped and unsafe: the "
            "return value is the raw function result."
        ),
        "source": "lua_runtime/main/Source/lua/raw.cpp",
        "line": 77,
        "group": "Escape hatch",
    },
    {
        "id": "tactility.raw.api",
        "name": "api",
        "owner": "tactility.raw",
        "kind": "constant",
        "signature": "tactility.raw.api",
        "value": "1",
        "description": "Version of the raw-call argument marshalling.",
        "source": "lua_runtime/main/Source/lua/raw.cpp",
        "line": 109,
        "group": "Escape hatch",
    },
    {
        "id": "tactility.notification.subscribe",
        "name": "subscribe",
        "owner": "tactility.notification",
        "kind": "function",
        "signature": "tactility.notification.subscribe(callback) -> Subscription",
        "description": (
            "Receives posted/dismissed/cleared/updated events on the LVGL task. "
            "Keep the returned Subscription alive; call cancel() to stop."
        ),
        "source": "lua_runner/main/Source/luavgl/notification_events.cpp",
        "line": 253,
        "group": "Functions",
    },
    {
        "id": "require",
        "name": "require",
        "owner": "lua",
        "kind": "function",
        "signature": "require(name) -> module",
        "description": (
            "Loads <entry dir>/<name>.lua or <entry dir>/<name>/init.lua, where the "
            "entry dir is lua/ inside a package. Dots map to directories, modules are "
            "cached, cycles raise an error, and C modules are disabled."
        ),
        "source": "lua_runtime/main/Source/lua/runtime.cpp",
        "line": 193,
        "group": "Modules",
    },
    {
        "id": "tactility.json.null",
        "name": "null",
        "owner": "tactility.json",
        "kind": "constant",
        "signature": "tactility.json.null",
        "description": (
            "The decoded JSON null value. Kept distinct from Lua nil so object fields "
            "and array slots do not disappear; encode() writes it back as null."
        ),
        "source": "lua_runtime/main/Source/lua/json.cpp",
        "line": 0,
        "group": "Values",
    },
    {
        "id": "tactility.ui.toolbar",
        "name": "toolbar",
        "owner": "tactility.ui",
        "kind": "function",
        "signature": "tactility.ui.toolbar() -> lvgl.Toolbar | nil",
        "description": (
            "Returns the runner's own toolbar, so scripts can add refresh or settings "
            "actions with lvgl.toolbar.add_text_action()/add_icon_action()."
        ),
        "source": "lua_runner/main/Source/luavgl/bindings.cpp",
        "line": 0,
        "group": "Toolbar",
    },
]

# Owners with a type suffix are methods on the handle that the module returns.
OWNER_MODULES: dict[str, str] = {
    "tactility.Device": "tactility.device",
    "tactility.spi.SpiDevice": "tactility.spi",
    "tactility.notification.Subscription": "tactility.notification",
    "tactility.http.Request": "tactility.http",
    "lvgl.Object": "lvgl",
    "lvgl.Widget": "lvgl",
    "lvgl.Animation": "lvgl",
    "lvgl.Event": "lvgl",
    "lvgl.Group": "lvgl",
    "lvgl.Style": "lvgl",
    "lvgl.Timer": "lvgl",
    "lvgl.toolbar": "lvgl",
}

DOC_COMMENT_RE = re.compile(
    r"(?:/\*\*(?P<doc>(?:(?!\*/).)*)\*/\s*)?(?:static\s+)?int\s+(?P<name>\w+)\s*\(\s*lua_State\s*\*",
    re.S,
)
REG_TABLE_RE = re.compile(r"const\s+luaL_Reg\s+(\w+)\[\]\s*=\s*\{(.*?)\};", re.S)
REG_ENTRY_RE = re.compile(r'\{\s*"([^"]+)"\s*,\s*(\w+)\s*\}')
CONST_ARRAY_RE = re.compile(r"const\s+Constant\s+(\w+)\[\]\s*=\s*\{(.*?)\};", re.S)
CONST_ENTRY_RE = re.compile(r'\{\s*"([^"]+)"\s*,\s*[^}]+\}')
ADD_CONSTANTS_RE = re.compile(r'add_constants\(state,\s*"([^"]+)",\s*(\w+)')
INCLUDE_ENTRY_RE = re.compile(r'\{\s*"([^"]+)"\s*,\s*[^}]+\}')


@dataclass
class Entry:
    id: str
    name: str
    owner: str
    kind: str
    signature: str
    group: str
    description: str = ""
    value: str | None = None
    source: str = ""
    line: int = 0

    def as_dict(self) -> dict:
        data = {
            "id": self.id,
            "name": self.name,
            "owner": self.owner,
            "kind": self.kind,
            "signature": self.signature,
            "group": self.group,
            "description": self.description,
            "source": self.source,
            "line": self.line,
        }
        if self.value is not None:
            data["value"] = self.value
        return data


@dataclass
class Module:
    name: str
    title: str
    summary: str
    entries: list[Entry] = field(default_factory=list)

    def as_dict(self) -> dict:
        return {
            "name": self.name,
            "title": self.title,
            "summary": self.summary,
            "entries": [entry.as_dict() for entry in self.entries],
        }


def relative(path: Path) -> str:
    try:
        return path.relative_to(APP_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def clean_comment(text: str) -> str:
    lines = []
    for line in text.splitlines():
        line = re.sub(r"^\s*\*\s?", "", line).rstrip()
        lines.append(line)
    return "\n".join(lines).strip()


def split_signature(text: str) -> tuple[str | None, str]:
    """Uses a leading backticked call as the signature, not prose references."""
    if not text.startswith("`"):
        return None, text
    match = re.match(r"`([^`\n]*\([^`\n]*\)[^`\n]*)`", text)
    if match is None:
        return None, text
    signature = match.group(1).strip()
    description = text[match.end() :].strip().lstrip("-:—").strip()
    if description and description[0].islower():
        description = description[0].upper() + description[1:]
    return signature, description


def normalize_signature(signature: str, owner: str) -> str:
    """Turns `display.size()` into `tactility.display.size()`."""
    tail = owner.rsplit(".", 1)[-1]
    if signature.startswith(tail + ".") or signature.startswith(tail + ":"):
        separator = signature[len(tail)]
        return owner + separator + signature[len(tail) + 1 :]
    return signature


def collect_definitions() -> dict[str, tuple[str, int, str]]:
    """C binding function -> (source, line, doc comment, possibly empty)."""
    definitions: dict[str, tuple[str, int, str]] = {}
    for root in (RUNTIME_SOURCES, RUNNER_SOURCES):
        for path in sorted(root.glob("*.cpp")):
            text = path.read_text(encoding="utf-8", errors="replace")
            for match in DOC_COMMENT_RE.finditer(text):
                name = match.group("name")
                line = text.count("\n", 0, match.start("name")) + 1
                comment = clean_comment(match.group("doc")) if match.group("doc") else ""
                # First definition wins; names are unique across the bindings.
                definitions.setdefault(name, (relative(path), line, comment))
    return definitions


def module_for(modules: dict[str, Module], name: str, title: str, summary: str) -> Module:
    module = modules.get(name)
    if module is None:
        module = Module(name, title, summary)
        modules[name] = module
    elif summary and not module.summary:
        module.summary = summary
    return module


def module_name_for(owner: str) -> str:
    if owner in OWNER_MODULES:
        return OWNER_MODULES[owner]
    return owner


def collect_registrations(modules: dict[str, Module], definitions: dict[str, tuple[str, int, str]]) -> None:
    sources = []
    for root in (RUNTIME_SOURCES, RUNNER_SOURCES):
        for path in sorted(root.glob("*.cpp")):
            sources.append((path, path.read_text(encoding="utf-8", errors="replace")))

    for path, text in sources:
        for table in REG_TABLE_RE.finditer(text):
            table_name = table.group(1)
            target = TABLE_TARGETS.get(table_name)
            if target is None:
                continue
            owner, separator, kind = target
            module_name = module_name_for(owner)
            title, summary = MODULE_INFO.get(module_name, (module_name, ""))
            module = module_for(modules, module_name, title, summary)
            group = GROUP_BY_OWNER.get(owner, "Functions")

            for entry_match in REG_ENTRY_RE.finditer(table.group(2)):
                name, c_function = entry_match.group(1), entry_match.group(2)
                definition = definitions.get(c_function)
                signature = f"{owner}{separator}{name}(...)"
                description = ""
                source = relative(path)
                line = text.count("\n", 0, table.start(2) + entry_match.start()) + 1
                extracted = None
                entry_group = group

                if definition is not None:
                    source, line, comment = definition
                    extracted, description = split_signature(comment)
                    if extracted:
                        signature = normalize_signature(extracted, owner)
                    elif comment:
                        description = comment

                if kind == "constructor" or (kind == "function" and name[:1].isupper()):
                    entry_group = "Constructors"
                    if extracted is None:
                        signature = f"{owner}{separator}{name}{{...}}"

                module.entries.append(
                    Entry(
                        id=f"{owner}{separator}{name}",
                        name=name,
                        owner=owner,
                        kind=kind,
                        signature=signature,
                        group=entry_group,
                        description=description,
                        source=source,
                        line=line,
                    )
                )


def collect_bindings(modules: dict[str, Module]) -> None:
    for path in sorted(BINDINGS_DIR.glob("*.toml")):
        data = tomllib.loads(path.read_text(encoding="utf-8"))
        module_data = data.get("module", {})
        name = "tactility." + module_data.get("name", path.stem)
        title, summary = MODULE_INFO.get(name, (path.stem, ""))
        summary = module_data.get("summary", summary)
        module = module_for(modules, name, title, summary)
        source = relative(path)
        types = data.get("types", {})

        for type_name, type_data in types.items():
            module.entries.append(
                Entry(
                    id=f"{name}.{type_name}",
                    name=type_name,
                    owner=name,
                    kind="type",
                    signature=f"{name}.{type_name}",
                    group="Types",
                    description=type_data.get("summary", ""),
                    source=source,
                    line=0,
                )
            )

        for function in data.get("function", []):
            function_name = function.get("name", "")
            signature = function.get("signature", f"{function_name}(...)")
            owner, separator = name, "."
            receiver = re.match(r"([A-Z]\w*)\s*:", signature)
            if receiver is not None and receiver.group(1) in types:
                owner, separator = f"{name}.{receiver.group(1)}", ":"
                group = f"{receiver.group(1)} methods"
            else:
                group = "Functions"
            module.entries.append(
                Entry(
                    id=f"{owner}{separator}{function_name}",
                    name=function_name,
                    owner=owner,
                    kind="function",
                    signature=signature,
                    group=group,
                    description=function.get("doc", ""),
                    source=source,
                    line=0,
                )
            )

        for constant_name, value in data.get("constants", {}).items():
            module.entries.append(
                Entry(
                    id=f"{name}.{constant_name}",
                    name=constant_name,
                    owner=name,
                    kind="constant",
                    signature=f"{name}.{constant_name}",
                    group="Constants",
                    value=str(value),
                    source=source,
                    line=0,
                )
            )


def collect_lvgl_constants(modules: dict[str, Module]) -> None:
    module = modules.get("lvgl")
    if module is None:
        title, summary = MODULE_INFO["lvgl"]
        module = module_for(modules, "lvgl", title, summary)

    bindings_path = RUNNER_SOURCES / "bindings.cpp"
    text = bindings_path.read_text(encoding="utf-8", errors="replace")
    source = relative(bindings_path)

    arrays: dict[str, list[str]] = {}
    for array in CONST_ARRAY_RE.finditer(text):
        arrays[array.group(1)] = CONST_ENTRY_RE.findall(array.group(2))

    seen: set[str] = set()
    for registration in ADD_CONSTANTS_RE.finditer(text):
        group, array_name = registration.group(1), registration.group(2)
        for name in arrays.get(array_name, []):
            entry_id = f"lvgl.{group}.{name}"
            if entry_id in seen:
                continue
            seen.add(entry_id)
            module.entries.append(
                Entry(
                    id=entry_id,
                    name=name,
                    owner=f"lvgl.{group}",
                    kind="constant",
                    signature=entry_id,
                    group=group,
                    source=source,
                )
            )

    icons_path = RUNNER_SOURCES / "icons_shared.inc"
    if icons_path.exists():
        for name in INCLUDE_ENTRY_RE.findall(icons_path.read_text(encoding="utf-8", errors="replace")):
            entry_id = f"lvgl.ICON.{name}"
            module.entries.append(
                Entry(
                    id=entry_id,
                    name=name,
                    owner="lvgl.ICON",
                    kind="constant",
                    signature=entry_id,
                    group="ICON",
                    source=relative(icons_path),
                )
            )

    symbol_block = re.search(
        r"const struct \{ const char\* name; const char\* symbol; \} symbols\[\] = \{(.*?)\};",
        text,
        re.S,
    )
    if symbol_block is not None:
        for name in CONST_ENTRY_RE.findall(symbol_block.group(1)):
            entry_id = f"lvgl.SYMBOL.{name}"
            module.entries.append(
                Entry(
                    id=entry_id,
                    name=name,
                    owner="lvgl.SYMBOL",
                    kind="constant",
                    signature=entry_id,
                    group="SYMBOL",
                    source=source,
                )
            )

    for name, value, description in (
        ("SIZE_CONTENT", "LV_SIZE_CONTENT", "Let a widget size itself to its content."),
        ("GRID_CONTENT", "LV_GRID_CONTENT", "Let a grid track size itself to its content."),
        ("TABLE_CELL_NONE", "LV_TABLE_CELL_NONE", "No table cell."),
        ("BUTTONMATRIX_BUTTON_NONE", "LV_BUTTONMATRIX_BUTTON_NONE", "No button-matrix button."),
    ):
        module.entries.append(
            Entry(
                id=f"lvgl.{name}",
                name=name,
                owner="lvgl",
                kind="constant",
                signature=f"lvgl.{name}",
                group="Values",
                value=value,
                description=description,
                source=source,
            )
        )


def build() -> dict:
    modules: dict[str, Module] = {}
    definitions = collect_definitions()

    collect_bindings(modules)
    collect_registrations(modules, definitions)
    collect_lvgl_constants(modules)

    for manual in MANUAL_ENTRIES:
        module_name = module_name_for(manual["owner"])
        title, summary = MODULE_INFO.get(module_name, (module_name, ""))
        module = module_for(modules, module_name, title, summary)
        module.entries.append(
            Entry(
                id=manual["id"],
                name=manual["name"],
                owner=manual["owner"],
                kind=manual["kind"],
                signature=manual["signature"],
                group=manual["group"],
                description=manual["description"],
                value=manual.get("value"),
                source=manual["source"],
                line=manual["line"],
            )
        )

    order = {name: index for index, name in enumerate(MODULE_INFO)}
    ordered = sorted(
        modules.values(),
        key=lambda module: (order.get(module.name, len(order)), module.name),
    )
    # Drop empty modules so a stale metadata entry cannot create a blank card.
    ordered = [module for module in ordered if module.entries]

    entry_count = sum(len(module.entries) for module in ordered)
    return {
        "title": "Tactility Lua API",
        "entry_count": entry_count,
        "modules": [module.as_dict() for module in ordered],
    }


HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__TITLE__</title>
<style>
  :root {
    color-scheme: light dark;
    --bg: #f6f7f9;
    --panel: #ffffff;
    --ink: #1d2330;
    --muted: #5b6473;
    --line: #e2e6ec;
    --accent: #16705a;
    --accent-soft: #e3f3ee;
    --code: #eef1f5;
    --chip: #ffffff;
    --shadow: 0 1px 2px rgba(16, 24, 40, 0.05), 0 8px 24px rgba(16, 24, 40, 0.06);
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #10141a;
      --panel: #161c24;
      --ink: #e6eaf0;
      --muted: #9aa4b2;
      --line: #28313d;
      --accent: #5fd3b0;
      --accent-soft: #14332b;
      --code: #1c2530;
      --chip: #161c24;
      --shadow: 0 1px 2px rgba(0, 0, 0, 0.4), 0 12px 32px rgba(0, 0, 0, 0.25);
    }
  }
  * { box-sizing: border-box; }
  html { scroll-behavior: smooth; }
  body {
    margin: 0;
    background: var(--bg);
    color: var(--ink);
    font: 15px/1.55 ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  }
  a { color: var(--accent); text-decoration: none; }
  a:hover { text-decoration: underline; }
  code, pre, .signature {
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, "Liberation Mono", monospace;
  }
  header {
    position: sticky;
    top: 0;
    z-index: 2;
    background: color-mix(in srgb, var(--bg) 88%, transparent);
    backdrop-filter: blur(12px);
    border-bottom: 1px solid var(--line);
  }
  .bar {
    max-width: 1120px;
    margin: 0 auto;
    padding: 18px 24px 14px;
    display: flex;
    align-items: center;
    gap: 18px;
    flex-wrap: wrap;
  }
  .brand { display: flex; align-items: center; gap: 12px; margin-right: auto; }
  .mark {
    width: 38px; height: 38px; border-radius: 11px;
    background: linear-gradient(145deg, #1b8a6f, #10243d);
    color: #eafff8; display: grid; place-items: center;
    font: 700 19px/1 ui-monospace, monospace;
  }
  h1 { font-size: 19px; margin: 0; letter-spacing: -0.01em; }
  .sub { color: var(--muted); font-size: 12.5px; }
  .search {
    position: relative;
    flex: 1 1 340px;
    max-width: 520px;
  }
  .search input {
    width: 100%;
    padding: 11px 14px 11px 38px;
    border: 1px solid var(--line);
    border-radius: 10px;
    background: var(--panel);
    color: var(--ink);
    font: inherit;
    box-shadow: var(--shadow);
    outline: none;
  }
  .search input:focus { border-color: var(--accent); }
  .search svg {
    position: absolute; left: 12px; top: 50%; transform: translateY(-50%);
    width: 16px; height: 16px; color: var(--muted);
  }
  .search kbd {
    position: absolute; right: 10px; top: 50%; transform: translateY(-50%);
    border: 1px solid var(--line); border-radius: 5px; padding: 1px 6px;
    color: var(--muted); background: var(--bg); font: 12px/1.5 inherit;
  }
  main { max-width: 1120px; margin: 0 auto; padding: 18px 24px 80px; }
  .meta { display: flex; gap: 14px; align-items: baseline; flex-wrap: wrap; color: var(--muted); font-size: 13px; }
  .meta strong { color: var(--ink); }
  .chips { display: flex; gap: 8px; flex-wrap: wrap; margin: 14px 0 22px; }
  .chip {
    border: 1px solid var(--line);
    background: var(--chip);
    color: var(--muted);
    border-radius: 999px;
    padding: 5px 12px;
    font: 12.5px/1.4 inherit;
    cursor: pointer;
  }
  .chip[aria-pressed="true"] {
    border-color: var(--accent);
    background: var(--accent-soft);
    color: var(--accent);
    font-weight: 600;
  }
  .module {
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 14px;
    box-shadow: var(--shadow);
    margin-bottom: 22px;
    overflow: hidden;
  }
  .module-head {
    padding: 18px 20px 14px;
    border-bottom: 1px solid var(--line);
  }
  .module-head h2 { margin: 0 0 3px; font-size: 17px; letter-spacing: -0.01em; }
  .module-head p { margin: 0; color: var(--muted); font-size: 13.5px; }
  .module-name { color: var(--accent); font: 12.5px/1.5 ui-monospace, monospace; }
  .group { padding: 6px 20px 10px; }
  .group h3 {
    margin: 16px 0 6px;
    font-size: 11.5px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: var(--muted);
  }
  .entry {
    padding: 10px 0;
    border-top: 1px solid color-mix(in srgb, var(--line) 65%, transparent);
    scroll-margin-top: 92px;
  }
  .entry:first-of-type { border-top: 0; }
  .entry:target { background: var(--accent-soft); border-radius: 8px; padding-left: 8px; padding-right: 8px; }
  .signature {
    display: inline;
    font-size: 13.5px;
    color: var(--ink);
    background: var(--code);
    border-radius: 6px;
    padding: 2px 6px;
    overflow-wrap: anywhere;
  }
  .constant .signature { color: var(--accent); }
  .entry p { margin: 5px 0 0; color: var(--muted); font-size: 13.5px; white-space: pre-line; }
  .entry p code {
    background: var(--code);
    border-radius: 4px;
    padding: 0 3px;
    font-size: 12.5px;
  }
  .where { margin-top: 5px; font-size: 11.5px; color: var(--muted); }
  .where a { color: inherit; text-decoration: underline; text-decoration-color: var(--line); }
  .value { font-size: 11.5px; color: var(--muted); margin-left: 6px; }
  .empty { padding: 60px 20px; text-align: center; color: var(--muted); display: none; }
  .hidden { display: none !important; }
  footer { max-width: 1120px; margin: 0 auto; padding: 0 24px 40px; color: var(--muted); font-size: 12px; }
  @media (max-width: 640px) {
    .bar { padding: 14px 16px 12px; gap: 12px; }
    main { padding: 14px 16px 60px; }
    .module-head, .group { padding-left: 14px; padding-right: 14px; }
    .search kbd { display: none; }
  }
</style>
</head>
<body>
<header>
  <div class="bar">
    <div class="brand">
      <div class="mark">L</div>
      <div>
        <h1>Tactility Lua API</h1>
        <div class="sub">Generated from the bindings &middot; __COUNT__ entries</div>
      </div>
    </div>
    <label class="search">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><circle cx="11" cy="11" r="7"></circle><path d="m20 20-3.5-3.5"></path></svg>
      <input id="q" type="search" placeholder="Search functions, methods and constants" autocomplete="off" spellcheck="false" aria-label="Search the API">
      <kbd>/</kbd>
    </label>
  </div>
</header>
<main>
  <div class="meta">
    <span><strong id="shown">__COUNT__</strong> shown</span>
    <span>Generated from the binding sources</span>
    <span><a href="https://github.com/Shadowtrance/TactilityLuaApps/blob/main/Apps/LuaRuntime/README.md">README</a></span>
    <span><a href="https://github.com/Shadowtrance/TactilityLuaApps/blob/main/Apps/LuaRuntime/CATALOG.md">Coverage</a></span>
  </div>
  <div class="chips" id="chips"></div>
  <div id="list"></div>
  <div class="empty" id="empty">Nothing matches that search.</div>
</main>
<footer>
  Signatures and descriptions come from the binding sources and <code>bindings/*.toml</code>.
  Entries without a description are listed by name only. Run
  <code>py tools/api-docs/generate.py</code> to regenerate.
</footer>
<script id="api-data" type="application/json">__DATA__</script>
<script>
(() => {
  const data = JSON.parse(document.getElementById("api-data").textContent);
  const list = document.getElementById("list");
  const chips = document.getElementById("chips");
  const query = document.getElementById("q");
  const shown = document.getElementById("shown");
  const empty = document.getElementById("empty");

  const state = { text: "", module: "*" };
  const moduleNames = data.modules.map(module => module.name);

  const chip = (label, value) => {
    const button = document.createElement("button");
    button.className = "chip";
    button.type = "button";
    button.textContent = label;
    button.dataset.value = value;
    button.setAttribute("aria-pressed", String(state.module === value));
    button.addEventListener("click", () => {
      state.module = value;
      chips.querySelectorAll(".chip").forEach(other => {
        other.setAttribute("aria-pressed", String(other.dataset.value === value));
      });
      render();
    });
    return button;
  };

  chips.append(chip("All", "*"));
  for (const name of moduleNames) chips.append(chip(name, name));

  const haystack = entry => (
    entry.id + " " + entry.signature + " " + entry.description + " " + entry.group
  ).toLowerCase();

  const appendText = (parent, text) => {
    const parts = text.split("`");
    parts.forEach((part, index) => {
      if (!part) return;
      if (index % 2 === 1) {
        const code = document.createElement("code");
        code.textContent = part;
        parent.append(code);
      } else {
        parent.append(document.createTextNode(part));
      }
    });
  };

  function render() {
    const terms = state.text.toLowerCase().split(/\\s+/).filter(Boolean);
    let total = 0;
    list.textContent = "";

    for (const module of data.modules) {
      if (state.module !== "*" && module.name !== state.module) continue;

      const entries = module.entries.filter(entry => {
        const text = haystack(entry);
        return terms.every(term => text.includes(term));
      });
      if (entries.length === 0) continue;

      const section = document.createElement("section");
      section.className = "module";

      const head = document.createElement("div");
      head.className = "module-head";
      const title = document.createElement("h2");
      title.textContent = module.title;
      const name = document.createElement("div");
      name.className = "module-name";
      name.textContent = module.name;
      const summary = document.createElement("p");
      summary.textContent = module.summary;
      head.append(title, name, summary);
      section.append(head);

      let groupName = "";
      let group = null;
      for (const entry of entries) {
        if (entry.group !== groupName) {
          groupName = entry.group;
          group = document.createElement("div");
          group.className = "group";
          const heading = document.createElement("h3");
          heading.textContent = groupName;
          group.append(heading);
          section.append(group);
        }

        const row = document.createElement("div");
        row.className = "entry " + entry.kind;
        row.id = entry.id;

        const anchor = document.createElement("a");
        anchor.href = "#" + encodeURIComponent(entry.id);
        anchor.className = "signature";
        anchor.textContent = entry.signature;
        row.append(anchor);

        if (entry.value) {
          const value = document.createElement("span");
          value.className = "value";
          value.textContent = entry.value;
          row.append(value);
        }

        if (entry.description) {
          const description = document.createElement("p");
          appendText(description, entry.description);
          row.append(description);
        }

        if (entry.source) {
          const where = document.createElement("div");
          where.className = "where";
          const link = document.createElement("a");
          link.href = "https://github.com/Shadowtrance/TactilityLuaApps/blob/main/Apps/LuaRuntime/"
            + entry.source + (entry.line ? "#L" + entry.line : "");
          link.target = "_blank";
          link.rel = "noreferrer";
          link.textContent = entry.source + (entry.line ? ":" + entry.line : "");
          where.append(link);
          row.append(where);
        }

        group.append(row);
        total++;
      }

      list.append(section);
    }

    shown.textContent = String(total);
    empty.style.display = total === 0 ? "block" : "none";
  }

  const focusFromHash = () => {
    if (!location.hash) return;
    let id;
    try { id = decodeURIComponent(location.hash.slice(1)); } catch { return; }
    const target = document.getElementById(id);
    if (!target) return;
    const module = data.modules.find(candidate => candidate.entries.some(entry => entry.id === id));
    if (module && state.module !== "*" && state.module !== module.name) {
      state.module = module.name;
      chips.querySelectorAll(".chip").forEach(other => {
        other.setAttribute("aria-pressed", String(other.dataset.value === module.name));
      });
      render();
    }
    requestAnimationFrame(() => target.scrollIntoView({ block: "center" }));
  };

  query.addEventListener("input", () => {
    state.text = query.value.trim();
    render();
  });

  document.addEventListener("keydown", event => {
    if (event.key === "/" && document.activeElement !== query) {
      event.preventDefault();
      query.focus();
      query.select();
    } else if (event.key === "Escape" && document.activeElement === query) {
      query.value = "";
      state.text = "";
      query.blur();
      render();
    }
  });

  window.addEventListener("hashchange", focusFromHash);

  render();
  focusFromHash();
})();
</script>
</body>
</html>
"""

README_TEMPLATE = """# Lua API reference

Generated by `tools/api-docs/generate.py`. Do not edit `index.html` or
`api.json` by hand.

| File | Purpose |
|---|---|
| `index.html` | Self-contained searchable reference (no network access needed) |
| `api.json` | The same entries for tooling and tests |

The generator reads the binding sources, not a separate list:

- `bindings/*.toml` for generated capability functions, signatures, docs,
  types and constants;
- the `luaL_Reg` tables in `lua_runtime/main/Source/lua` and
  `lua_runner/main/Source/luavgl`, with the doc comment above each C function;
- the LVGL constant tables plus `icons_shared.inc` and the `SYMBOL` table.

Entries without a description only have a name; adding a doc comment above the
binding function and running the generator fills the description in.

## Regenerate

From `Apps/LuaRuntime`:

```powershell
py tools\\api-docs\\generate.py
```

`--check` renders in memory and fails when the committed files are stale:

```powershell
py tools\\api-docs\\generate.py --check
```

The page is copied to `/api/` during the Pages deployment.
"""


def render(data: dict) -> tuple[str, str]:
    payload = json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")
    html = (
        HTML_TEMPLATE.replace("__TITLE__", data["title"])
        .replace("__COUNT__", str(data["entry_count"]))
        .replace("__DATA__", payload)
    )
    return html, json.dumps(data, ensure_ascii=False, indent=2) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true", help="fail when the output is stale")
    arguments = parser.parse_args()

    data = build()
    html, api_json = render(data)
    files = {
        arguments.output / "index.html": html,
        arguments.output / "api.json": api_json,
        arguments.output / "README.md": README_TEMPLATE,
    }

    if arguments.check:
        stale = []
        for path, content in files.items():
            existing = path.read_text(encoding="utf-8") if path.exists() else None
            if existing != content:
                stale.append(path)
        if stale:
            for path in stale:
                print(f"stale: {path}", file=sys.stderr)
            print("run: py tools/api-docs/generate.py", file=sys.stderr)
            return 1
        print(f"api-docs: up to date ({data['entry_count']} entries)")
        return 0

    arguments.output.mkdir(parents=True, exist_ok=True)
    for path, content in files.items():
        path.write_text(content, encoding="utf-8", newline="\n")
        print(f"wrote {path}")
    print(f"api-docs: {data['entry_count']} entries in {len(data['modules'])} modules")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
