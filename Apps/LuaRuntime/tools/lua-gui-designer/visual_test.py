#!/usr/bin/env python3
"""Headless visual regression for the designer preview.

The preview is drawn by `render.render_image()`, the same function the Tk canvas
shows, so it can be rendered without a window. This script renders fixed
projects at device profiles and compares them against PNG goldens. It needs only
Pillow: no Tk, no Custom2kinter, no display.

    python visual_test.py              # compare against golden/
    python visual_test.py --update     # rewrite golden/ after an intended change
    python visual_test.py --diff-dir out   # save amplified difference images

Comparison is tolerant of harmless noise: a pixel counts as changed only when a
channel differs by more than `--tolerance` (default 2), and a case fails when
more than `--max-changed` (default 0.02%) of pixels changed or the mean
per-channel difference exceeds `--max-mean` (default 0.05). Rendering is
deterministic because the fonts are bundled and CI pins Pillow, so the
thresholds are tight enough to catch a moved or recoloured widget.
"""

from __future__ import annotations

import argparse
import datetime
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

from demo import demo_project
from model import Project
from profiles import find_profile
from render import MARGIN, render_image
from theme import MaterialTheme, ThemeSettings

HERE = Path(__file__).resolve().parent
GOLDEN_DIR = HERE / "golden"

# The status bar draws the time; pin it so a golden does not go stale at the
# next minute.
CLOCK = datetime.datetime(2026, 1, 1, 9, 41)


@dataclass(frozen=True)
class Case:
    name: str
    source: Path | None  # project file, or None for the demo
    profile: str | None = None  # override the project's profile
    dark: bool | None = None
    mono: bool = False


CASES: tuple[Case, ...] = (
    # The 1:1 Appearance Settings clone: 71 widgets, mostly flex/grid content
    # sizing, at the profile it was measured against and in portrait.
    Case("appearance-tab5-dark", HERE / "appearance.luadesign.json"),
    Case("appearance-tab5-portrait", HERE / "appearance.luadesign.json", profile="tab5-portrait"),
    Case("appearance-tab5-mono", HERE / "appearance.luadesign.json", mono=True),
    # The demo covers the widget set the clone does not: chips, sliders,
    # switches, the bar and the light palette.
    Case("demo-tab5-dark", None),
    Case("demo-tab5-light", None, dark=False),
    Case("demo-tab5-mono", None, mono=True),
)


def build_project(case: Case) -> tuple[Project, str]:
    if case.source is None:
        profile_id = case.profile or "tab5-landscape"
        profile = find_profile(profile_id)
        theme: ThemeSettings = profile.default_theme()
        if case.dark is not None:
            theme.dark = case.dark
        theme.mono = case.mono
        project = demo_project(theme)
        project.profile = profile.id
        project.theme = theme
        return project, profile.id

    project = Project.load(case.source)
    profile_id = case.profile or project.profile
    project.profile = profile_id
    if case.dark is not None:
        project.theme.dark = case.dark
    project.theme.mono = case.mono
    return project, profile_id


def render_case(case: Case) -> Image.Image:
    project, profile_id = build_project(case)
    profile = find_profile(profile_id)
    theme = MaterialTheme(project.theme, profile.dpi)
    image = render_image(
        project,
        profile,
        theme,
        zoom=1.0,
        selected_id=None,
        selected_ids=None,
        show_grid=False,
        scroll_y=0,
        now=CLOCK,
    )
    if image is None:
        raise RuntimeError(f"{case.name}: the preview has no size")
    # Drop the canvas margin so the golden is exactly one device screen.
    return image.crop((MARGIN, MARGIN, MARGIN + profile.width, MARGIN + profile.height))


def compare(actual: Image.Image, golden: Image.Image, tolerance: int):
    if actual.size != golden.size:
        return None, None, None
    actual = actual.convert("RGB")
    golden = golden.convert("RGB")
    diff = ImageChops.difference(actual, golden)
    changed_mask = None
    for band in diff.split():
        band = band.point(lambda value: 255 if value > tolerance else 0)
        changed_mask = band if changed_mask is None else ImageChops.lighter(changed_mask, band)
    changed = changed_mask.histogram()[255]
    total = actual.width * actual.height
    mean = sum(ImageStat.Stat(diff).mean) / 3
    return changed / total, mean, diff


def amplify(diff: Image.Image) -> Image.Image:
    return diff.point(lambda value: min(255, value * 8))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--update", action="store_true", help="Write the goldens and exit")
    parser.add_argument("--golden-dir", type=Path, default=GOLDEN_DIR, help="Golden image folder")
    parser.add_argument("--diff-dir", type=Path, help="Write amplified difference images on failure")
    parser.add_argument("--only", help="Only run cases whose name contains this text")
    parser.add_argument("--list", action="store_true", help="List case names and exit")
    parser.add_argument("--tolerance", type=int, default=2, help="Per-channel difference that still passes")
    parser.add_argument(
        "--max-changed",
        type=float,
        default=0.0002,
        help="Fraction of pixels allowed to exceed the tolerance",
    )
    parser.add_argument("--max-mean", type=float, default=0.05, help="Maximum mean channel difference")
    args = parser.parse_args()

    cases = [case for case in CASES if not args.only or args.only in case.name]
    if not cases:
        parser.error(f"no case matches {args.only!r}")
    if args.list:
        for case in cases:
            print(case.name)
        return 0

    args.golden_dir.mkdir(parents=True, exist_ok=True)
    if args.diff_dir is not None:
        args.diff_dir.mkdir(parents=True, exist_ok=True)

    failures = 0
    for case in cases:
        image = render_case(case)
        path = args.golden_dir / f"{case.name}.png"
        if args.update:
            image.save(path)
            print(f"{case.name}: wrote {path}")
            continue
        if not path.exists():
            print(f"{case.name}: FAIL missing {path} (run with --update)")
            failures += 1
            continue

        golden = Image.open(path)
        if golden.size != image.size:
            print(f"{case.name}: FAIL size {image.size} != golden {golden.size}")
            failures += 1
            continue

        changed, mean, diff = compare(image, golden, args.tolerance)
        passed = changed <= args.max_changed and mean <= args.max_mean
        print(
            f"{case.name}: {'PASS' if passed else 'FAIL'} "
            f"({changed * 100:.3f}% pixels, mean {mean:.3f})"
        )
        if not passed:
            failures += 1
            if args.diff_dir is not None and diff is not None:
                amplify(diff).save(args.diff_dir / f"{case.name}.diff.png")
                image.save(args.diff_dir / f"{case.name}.png")

    if args.update:
        return 0
    if failures:
        print(f"{failures} visual failure(s)")
        return 1
    print(f"{len(cases)} visual case(s) passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
