#!/usr/bin/env python3
"""Build a Tactility .app package that runs a Lua script as a real launcher app.

The package carries the already-built lua_runner ELF (from the LuaRuntime
package) plus the user's script at lua/main.lua. Nothing is compiled here, so a
script can be packaged and installed without an ESP-IDF build.

The runtime package (`Apps/LuaRuntime`) must already be installed on the device:
its module provides the Lua interpreter that lua_runner resolves at load time.

Examples:
    py make_lua_app.py --script demo.lua
    py make_lua_app.py --directory demo/ --name "Lua Dashboard" --id tactility.lua.dashboard
    py make_lua_app.py --script demo.lua --platform esp32p4 --output dist/demo.app

A `--directory` package expects `main.lua` at its root and copies the whole tree
to `lua/`, so modules can use `require` with the same relative names.

Without --version/--code, the leading `-- @version x.y.z` and `-- @code n`
comments in the script are used, defaulting to 0.1.0/1. Explicit arguments win.
"""

import argparse
import io
import re
import sys
import tarfile
from pathlib import Path

RUNTIME_DIR = Path(__file__).resolve().parents[2]  # Apps/LuaRuntime
REPO_ROOT = RUNTIME_DIR.parents[1]


def default_runner(platform: str) -> Path:
    """The built runner for a platform: .elf on ESP, .so on POSIX."""
    suffix = "so" if platform.startswith("linux") else "elf"
    return RUNTIME_DIR / "build" / "package-intermediate" / "bin" / platform / f"lua_runner.{suffix}"

DEFAULT_SDK = "0.8.0-dev"
DEFAULT_PLATFORM = "esp32p4"
DEFAULT_VERSION = "0.1.0"
DEFAULT_CODE = 1

# Only the comment header is read, so a `-- @version` deeper in a script cannot
# silently change the package version.
METADATA_LINE_LIMIT = 30


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


def read_script_metadata(script):
    """
    Reads the optional `-- @version x.y.z` and `-- @code n` tags from the
    comment header at the top of a script.

    A script in Scripts/ is published by the workflow without arguments, so the
    tags are what let an updated script reach App Hub as a new version. Parsing
    stops at the first non-comment line.
    """
    metadata = {}

    try:
        lines = script.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        fail(f"{script}: {error}")

    for line in lines[:METADATA_LINE_LIMIT]:
        stripped = line.strip()
        if not stripped:
            continue
        if not stripped.startswith("--"):
            break

        match = re.fullmatch(r"--\s*@version\s+(\S+)", stripped)
        if match:
            metadata["version"] = match.group(1)
            continue

        match = re.fullmatch(r"--\s*@code\s+(\d+)", stripped)
        if match:
            metadata["code"] = int(match.group(1))

    return metadata


def main():
    parser = argparse.ArgumentParser(
        prog="make_lua_app",
        description="package a Lua script or script directory as an installable Tactility app",
    )
    parser.add_argument("--script", help="a single Lua script (copied to lua/main.lua)")
    parser.add_argument(
        "--directory",
        help="a script directory with main.lua and modules (copied to lua/, preserving subdirectories)",
    )
    parser.add_argument("--name", help="app name (default: derived from the script file name)")
    parser.add_argument("--id", help="app and package id (default: tactility.lua.<script name>)")
    parser.add_argument(
        "--version",
        help=f"version name (default: @version in the script, or {DEFAULT_VERSION})",
    )
    parser.add_argument(
        "--code",
        type=int,
        help=f"version code (default: @code in the script, or {DEFAULT_CODE})",
    )
    parser.add_argument("--description", help="launcher description")
    parser.add_argument(
        "--short-description",
        help="one-line description for app catalogs (max 32 characters, default: 'Lua app: <name>')",
    )
    parser.add_argument("--platform", action="append", help=f"target platform, repeatable (default {DEFAULT_PLATFORM})")
    parser.add_argument("--runner", help="lua_runner binary to ship (default: the built LuaRuntime runner for each platform)")
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

    if bool(args.script) == bool(args.directory):
        parser.error("pass exactly one of --script or --directory")

    lua_entries = []
    if args.script:
        script = Path(args.script).expanduser()
        if not script.is_file():
            fail(f"{script}: no such file")
        stem = script.stem
        lua_entries.append(("main.lua", script))
    else:
        directory = Path(args.directory).expanduser()
        if not directory.is_dir():
            fail(f"{directory}: no such directory")
        script = directory / "main.lua"
        if not script.is_file():
            fail(f"{script}: a script directory must contain main.lua")
        stem = directory.name
        for path in sorted(directory.rglob("*")):
            if path.is_file():
                lua_entries.append((path.relative_to(directory).as_posix(), path))

    name = args.name or titleize(stem)
    app_id = args.id or f"tactility.lua.{slugify(stem)}"
    validate_id(app_id)

    metadata = read_script_metadata(script)
    version = args.version if args.version is not None else metadata.get("version", DEFAULT_VERSION)
    code = args.code if args.code is not None else metadata.get("code", DEFAULT_CODE)
    if code < 1:
        fail(f"version code {code} must be positive")

    platforms = args.platform or [DEFAULT_PLATFORM]
    output = Path(args.output).expanduser() if args.output else Path(f"{app_id}.app")
    sdk = read_sdk_version()

    runners = {}
    for platform in platforms:
        runner = Path(args.runner).expanduser() if args.runner else default_runner(platform)
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

    assets = Path(args.assets).expanduser() if args.assets else None
    if assets is not None and not assets.is_dir():
        fail(f"{assets}: not a directory")

    output.parent.mkdir(parents=True, exist_ok=True)

    manifest = "\n".join([
        "manifest.version=0.3",
        f"target.sdk={sdk}",
        f"target.platforms={','.join(platforms)}",
        f"id={app_id}",
        f"version.name={version}",
        f"version.code={code}",
        f"name={name}",
        f"description.short={short_description}",
        f"description={description}",
        f"app.0.id={app_id}",
        f"app.0.name={name}",
        "app.0.binary=lua_runner",
        # Module loading nests the Lua parser inside require(), and the scheduler's
        # default stack is too small for that on ESP targets. ESP-IDF counts bytes.
        "app.0.stack.depth=32768",
        f"lua.runtime.api={runtime_api}",
        f"requires.ram={args.ram}",
        "",
    ]).encode("utf-8")

    with tarfile.open(output, "w") as package:
        def add_bytes(name, data, mode=0o644, mtime=None):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = mode
            info.mtime = int(mtime if mtime is not None else script.stat().st_mtime)
            package.addfile(info, io.BytesIO(data))

        add_bytes("manifest.properties", manifest)
        add_bytes("icon.svg", icon_data)
        add_bytes("icon_mono.svg", mono_icon_data)

        for relative, path in lua_entries:
            add_bytes(f"lua/{relative}", path.read_bytes(), mtime=path.stat().st_mtime)

        for platform, runner in runners.items():
            binary = runner.read_bytes()
            add_bytes(f"bin/{platform}/{runner.name}", binary, mode=0o755)

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
