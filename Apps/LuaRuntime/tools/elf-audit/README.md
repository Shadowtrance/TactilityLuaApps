# ELF audit

Checks packaged Lua apps against the Xtensa section ELF loader used on the S3.

The S3 firmware loads `.text`, `.data`, `.rodata`, `.data.rel.ro` and `.bss` into
separate allocations and resolves a `R_XTENSA_RELATIVE` addend with
`esp_elf_map_sym()`. That mapping uses strict containment (`addr <= sym < addr +
size`), so a pointer to *one past the end* of a section becomes `0`. C++ emits
exactly that pointer for `std::end(array)`, which is how the animation bindings
once turned a range-based loop into an endless walk.

This tool replays the loader's section mapping over the built `.app.elf` files and
fails when a relocation addend would become `NULL`, or when a relocation site
falls outside every loaded section.

```powershell
py Apps\LuaRuntime\tools\elf-audit\elf_audit.py
py Apps\LuaRuntime\tools\elf-audit\elf_audit.py path\to\app.elf
```

With no paths it discovers `Apps/LuaRuntime/*/build/cmake-build-*/*.app.elf`.
RISC-V (P4) images are skipped because they use the segment loader instead.

The publish workflow runs it after building the app, so a future one-past-the-end
sentinel fails CI instead of crashing a device.

The optional firmware patch `Apps/LuaRuntime/firmware-elf-loader-end-pointer.patch`
fixes the loader itself. This audit is independent of it: it catches the app-side
pattern on firmware that still has the strict mapping.
