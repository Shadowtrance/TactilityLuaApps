# Tactility Lua apps

A custom [Tactility](https://tactility.one) app repository for the Lua runtime and
packaged Lua apps. GitHub Actions builds the runtime, turns every script in
`Scripts/` into an app, generates the [App Hub](https://tactility.one) catalog
files with [TactilityAppPublisher](https://github.com/TactilityProject/TactilityAppPublisher)
and deploys it to GitHub Pages.

## Repository layout

| Path | Purpose |
|---|---|
| `Apps/LuaRuntime` | The runtime package: `lua_runtime` module, the **Lua** companion, the hidden script runner and the **Lua Apps** manager. |
| `Scripts/*.lua` | Packaged as one app each, with the id `tactility.lua.<file name>`. |
| `site/index.html` | The landing page for the Pages site. `apps/<sdk>/` is added next to it on deploy. |
| `.github/workflows/publish.yml` | Build, bundle and deploy. |

## Use it on a device

1. Push to `main` and set **Settings → Pages → Source** to **GitHub Actions**.
2. The site is `https://<user>.github.io/<repo>/`.
3. On the device, open **Software → App repositories** and add
   `https://<user>.github.io/<repo>/apps`.
4. Open **App Hub**: the runtime and the Lua apps are listed next to the
   official catalog.
5. Install **Lua Runtime** first. The packaged Lua apps need the runtime's
   module, and they embed the runner ELF from the build.

## Add a Lua app

Drop a script in `Scripts/` and push. The workflow packages it with the default
name, id and icon of the LuaRuntime package (the id comes from the file name).
For custom metadata, package it by hand:

```powershell
py Apps/LuaRuntime/tools/lua-app/make_lua_app.py --script Scripts/my_app.lua `
  --name "My App" --id tactility.lua.myapp --version 1.0.0 --code 100 `
  --description "What it does" --output build/my_app.app
```

The generated manifest includes `description.short` (required by the
publisher) and `target.sdk` from the runtime package. Keep every package on the
same SDK version; they share one catalog folder.

## Local build and publish

Requires the Tactility SDK and ESP-IDF, like the official apps repository.

```powershell
# Runtime package
.\tt.ps1 app build --path Apps\LuaRuntime -a esp32p4

# One script package
py Apps\LuaRuntime\tools\lua-app\make_lua_app.py --script Scripts\hello.lua --output build\hello.app

# Catalog + site (needs a TactilityAppPublisher checkout next to this repo)
py ..\TactilityAppPublisher\apppublisher.py release build\release
py ..\TactilityAppPublisher\apppublisher.py publish pages build\release --sdk 0.8.0 --out site
```

The SDK folder on the site drops the `-dev` postfix: packages that declare
`target.sdk=0.8.0-dev` are published under `apps/0.8.0/`.

Pull requests build and bundle without deploying, so a broken package is caught
before it reaches the site. Changed apps must bump `version.code` and
`version.name`, which the publisher checks.

## The Lua Apps manager

The runtime package includes a **Lua Apps** app (`tactility.luaapps`). It lists
every installed package whose id starts with `tactility.lua.`, opens them and
uninstalls them after a confirmation, so a device with many Lua apps stays
manageable.
