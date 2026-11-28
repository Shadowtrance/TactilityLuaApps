#!/usr/bin/env python3
"""Convert the Weather source PNGs to LVGL RGB565A8 .bin images.

The Lua runtime can load a .bin into a Lua string (PSRAM-preferring) and draw it
straight from memory. The firmware has no decoded-image cache, so this avoids a
PNG decode on every scroll frame.

Usage (from the repository root):

    python Scripts/weather-tools/make_bins.py

Source images live in Scripts/weather-tools/source and are not packaged; the
generated assets go to Scripts/weather/assets.
"""

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "source"
ASSETS = ROOT.parent / "weather" / "assets"

MAGIC = 0x19
COLOR_FORMAT_RGB565A8 = 0x14
HEADER_SIZE = 12

# Only the sizes the app asks for: hero 96/64, forecast 32/26, metrics 20 and
# digits 64/40. The source folder keeps every size for future variants.
USED_SIZES = {
    "weather": {"20", "26", "32", "64", "96"},
    "temp": {"40", "64"},
}


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def convert(png_path: Path, output_path: Path) -> None:
    image = Image.open(png_path).convert("RGBA")
    width, height = image.size
    pixels = image.load()

    color = bytearray(width * height * 2)
    alpha = bytearray(width * height)
    index = 0
    alpha_index = 0
    for y in range(height):
        for x in range(width):
            r, g, b, a = pixels[x, y]
            value = rgb565(r, g, b)
            color[index] = value & 0xFF
            color[index + 1] = value >> 8
            index += 2
            alpha[alpha_index] = a
            alpha_index += 1

    header = bytearray(HEADER_SIZE)
    header[0] = MAGIC
    header[1] = COLOR_FORMAT_RGB565A8
    header[4:6] = width.to_bytes(2, "little")
    header[6:8] = height.to_bytes(2, "little")
    header[8:10] = (width * 2).to_bytes(2, "little")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(header + color + alpha)


def main() -> None:
    converted = 0
    for png_path in sorted(SOURCE.rglob("*.png")):
        relative = png_path.relative_to(SOURCE)
        group = relative.parts[0]
        size = png_path.stem.rsplit("-", 1)[-1]
        if group in USED_SIZES and size not in USED_SIZES[group]:
            continue
        convert(png_path, ASSETS / relative.with_suffix(".bin"))
        converted += 1
    print("wrote", converted, "images to", ASSETS)


if __name__ == "__main__":
    main()
