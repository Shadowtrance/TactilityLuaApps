# Lua app icon

The launcher icons are generated with `tt icon-studio` from the official Lua
logo:

- `Lua-Logo.svg` — the original download.
- `Lua-Logo-square.svg` — the original plus a full-bleed `#0EA5E9` tile, so
  icon-studio keeps the logo's composition.
- `Lua-Logo-mono.svg` — a single even-odd path for the moon, its cutout and the
  `Lua` wordmark. The wordmark and cutout become transparent holes in the mono
  icon instead of black-on-black.

Regenerate the colour icon with:

```powershell
tt icon-studio --svg tools\Lua-Logo-square.svg `
    --out <temp> --bg "#0EA5E9" --glyph keep --no-gui
Copy-Item <temp>\icon.svg icon.svg
```

Regenerate the mono icon with:

```powershell
tt icon-studio --svg tools\Lua-Logo-mono.svg --out <temp-mono> --no-gui
Copy-Item <temp-mono>\icon_mono.svg icon_mono.svg
```

`icon.svg` is the colour launcher icon; `icon_mono.svg` is the themed mask.
