# Scripts

Every `*.lua` file in this folder is packaged as its own Tactility app by the
publish workflow. The app id is derived from the file name:
`hello.lua` becomes `tactility.lua.hello`, with the title `Hello`.

Use the comment block at the top of a script for a short explanation; it is not
used as the app description. For a custom name, id, version, description or
icon, package the script by hand:

```powershell
py Apps\LuaRuntime\tools\lua-app\make_lua_app.py --script Scripts\hello.lua `
  --name "Hello" --id tactility.lua.hello --version 1.0.0 --code 100 `
  --description "Says hello" --output build\hello.app
```
