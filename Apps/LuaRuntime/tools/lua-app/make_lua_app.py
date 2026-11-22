#!/usr/bin/env python3
"""Build a Tactility .app package that runs a Lua script as a real launcher app.

The package carries the already-built lua_runner ELF (from the LuaRuntime
package) plus the user's script at lua/main.lua. Nothing is compiled here, so a
script can be packaged and installed without an ESP-IDF build.

The runtime package (`Apps/LuaRuntime`) must already be installed on the device:
its module provides the Lua interpreter that lua_runner resolves at load time.

Examples:
    py make_lua_app.py --script demo.lua
    py make_lua_app.py --script demo.lua --name "Lua Dashboard" --id tactility.lua.dashboard
    py make_lua_app.py --script demo.lua --platform esp32p4 --output dist/demo.app
"""

import argparse
import io
import re
import sys
import tarfile
from pathlib import Path

RUNTIME_DIR = Path(__file__).resolve().parents[2]  # Apps/LuaRuntime
REPO_ROOT = RUNTIME_DIR.parents[1]
DEFAULT_RUNNER = RUNTIME_DIR / "build" / "package-intermediate" / "bin" / "{platform}" / "lua_runner.elf"

DEFAULT_SDK = "0.8.0-dev"
DEFAULT_PLATFORM = "esp32p4"


def fail(message):
    print(f"make_lua_app: {message}", file=sys.stderr)
    sys.exit(1)


def slugify(text):
    slug = re.sub(r"[^a-z0-9]+", "-", text.strip().lower()).strip("-")
    return slug or "script"


def titleize(text):
    words = re.split(r"[^A-Za-z0-9]+", text.strip())
    return " ".join(word[:1].upper() + word[1:] for word in words if word) or "Lua Script"


def read_sdk_version():
    try:
        for line in (RUNTIME_DIR / "manifest.properties").read_text(encoding="utf-8").splitlines():
            if line.startswith("target.sdk="):
                return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return DEFAULT_SDK


def read_runtime_api_version():
    header = RUNTIME_DIR / "lua_runtime" / "main" / "Include" / "lua" / "runtime.h"
    try:
        for line in header.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if stripped.startswith("#define LUA_RUNTIME_API_VERSION"):
                return int(stripped.split()[-1])
    except (OSError, ValueError):
        pass
    return 1


def validate_id(app_id):
    if not app_id:
        fail("the app id is empty")
    if len(app_id) > 32:
        fail(f"app id {app_id!r} is longer than 32 characters")
    if not re.fullmatch(r"[a-z0-9][a-z0-9.-]*", app_id):
        fail(f"app id {app_id!r} must be lowercase letters, digits, '.', or '-'")


def main():
    parser = argparse.ArgumentParser(
        prog="make_lua_app",
        description="package a Lua script as an installable Tactility app",
    )
    parser.add_argument("--script", required=True, help="the Lua script to run (copied to lua/main.lua)")
    parser.add_argument("--name", help="app name (default: derived from the script file name)")
    parser.add_argument("--id", help="app and package id (default: tactility.lua.<script name>)")
    parser.add_argument("--version", default="0.1.0", help="version name (default 0.1.0)")
    parser.add_argument("--code", type=int, default=1, help="version code (default 1)")
    parser.add_argument("--description", help="launcher description")
    parser.add_argument(
        "--short-description",
        help="one-line description for app catalogs (max 32 characters, default: 'Lua app: <name>')",
    )
    parser.add_argument("--platform", action="append", help=f"target platform, repeatable (default {DEFAULT_PLATFORM})")
    parser.add_argument("--runner", help="lua_runner ELF to ship (default: the built LuaRuntime runner)")
    parser.add_argument("--icon", help="colour icon SVG (default: the LuaRuntime icon)")
    parser.add_argument("--mono-icon", help="monochrome icon SVG (default: the LuaRuntime icon)")
    parser.add_argument("--assets", help="directory copied to the package's assets/ folder")
    parser.add_argument("--ram", default="1M", help="requires.ram value (default 1M)")
    parser.add_argument(
        "--requires-api",
        type=int,
        help="minimum Lua runtime API (default: LUA_RUNTIME_API_VERSION from the runtime header)",
    )
    parser.add_argument("--output", help="output .app file (default: <id>.app)")
    args = parser.parse_args()

    script = Path(args.script).expanduser()
    if not script.is_file():
        fail(f"{script}: no such file")

    stem = script.stem
    name = args.name or titleize(stem)
    app_id = args.id or f"tactility.lua.{slugify(stem)}"
    validate_id(app_id)

    platforms = args.platform or [DEFAULT_PLATFORM]
    output = Path(args.output).expanduser() if args.output else Path(f"{app_id}.app")
    sdk = read_sdk_version()

    runners = {}
    for platform in platforms:
        runner = Path(args.runner).expanduser() if args.runner else Path(
            str(DEFAULT_RUNNER).format(platform=platform)
        )
        if not runner.is_file():
            fail(
                f"{runner}: no lua_runner for {platform}. "
                "Build LuaRuntime first (tt app build --path Apps/LuaRuntime) or pass --runner."
            )
        runners[platform] = runner

    icon = Path(args.icon).expanduser() if args.icon else RUNTIME_DIR / "icon.svg"
    mono_icon = Path(args.mono_icon).expanduser() if args.mono_icon else RUNTIME_DIR / "icon_mono.svg"
    for path, label in ((icon, "colour icon"), (mono_icon, "mono icon")):
        if not path.is_file():
            fail(f"{path}: no such {label}")

    description = args.description or f"Run {name} with the Lua runtime."
    # App Hub catalogs (TactilityAppPublisher) require description.short <= 32 chars.
    short_description = args.short_description or f"Lua app: {name}"
    if len(short_description) > 32:
        if args.short_description:
            fail("--short-description is longer than 32 characters")
        short_description = short_description[:29] + "..."
    runtime_api = args.requires_api if args.requires_api is not None else read_runtime_api_version()

    icon_data = icon.read_bytes()
    mono_icon_data = mono_icon.read_bytes()
    script_data = script.read_bytes()

    assets = Path(args.assets).expanduser() if args.assets else None
    if assets is not None and not assets.is_dir():
        fail(f"{assets}: not a directory")

    output.parent.mkdir(parents=True, exist_ok=True)

    manifest = "\n".join([
        "manifest.version=0.3",
        f"target.sdk={sdk}",
        f"target.platforms={','.join(platforms)}",
        f"id={app_id}",
        f"version.name={args.version}",
        f"version.code={args.code}",
        f"name={name}",
        f"description.short={short_description}",
        f"description={description}",
        f"app.0.id={app_id}",
        f"app.0.name={name}",
        "app.0.binary=lua_runner",
        f"lua.runtime.api={runtime_api}",
        f"requires.ram={args.ram}",
        "",
    ]).encode("utf-8")

    with tarfile.open(output, "w") as package:
        def add_bytes(name, data, mode=0o644):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = mode
            info.mtime = int(script.stat().st_mtime)
            package.addfile(info, io.BytesIO(data))

        add_bytes("manifest.properties", manifest)
        add_bytes("icon.svg", icon_data)
        add_bytes("icon_mono.svg", mono_icon_data)
        add_bytes("lua/main.lua", script_data)

        for platform, runner in runners.items():
            binary = runner.read_bytes()
            add_bytes(f"bin/{platform}/lua_runner.elf", binary, mode=0o755)

        if assets is not None:
            for path in sorted(assets.rglob("*")):
                if path.is_file():
                    relative = path.relative_to(assets).as_posix()
                    add_bytes(f"assets/{relative}", path.read_bytes())

    print(f"{output} ({output.stat().st_size} bytes)")
    print(f"app id: {app_id}")
    print(f"install: tt app install --host <device> {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
