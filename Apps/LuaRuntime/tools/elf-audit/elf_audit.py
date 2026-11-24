"""Check packaged Lua apps against the Xtensa section ELF loader.

The S3 firmware loads `.text`, `.data`, `.rodata`, `.data.rel.ro` and `.bss` into
separate allocations and resolves a `R_XTENSA_RELATIVE` addend with
`esp_elf_map_sym()`. That mapping uses strict containment (`addr <= sym <
addr + size`), so a pointer to *one past the end* of a section maps to 0. C++
emits exactly that pointer for `std::end(array)`, which turned a range-based loop
over a static table into an endless walk (see the 0.1.9 animation fix).

This tool replays the loader's section mapping over every packaged ELF and fails
when a relocation addend would become NULL, or when the relocation site itself
falls outside a loaded section.

Usage (from the repository root):

    py Apps\\LuaRuntime\\tools\\elf-audit\\elf_audit.py
    py Apps\\LuaRuntime\\tools\\elf-audit\\elf_audit.py path\\to\\app.elf
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

EM_XTENSA = 94
EM_RISCV = 243

SHT_NOBITS = 8
SHT_RELA = 4
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4

R_XTENSA_RTLD = 2
R_XTENSA_GLOB_DAT = 3
R_XTENSA_JMP_SLOT = 4
R_XTENSA_RELATIVE = 5
# The loader explicitly supports these; everything else is a hard failure.
SUPPORTED_XTENSA = {R_XTENSA_RTLD, R_XTENSA_GLOB_DAT, R_XTENSA_JMP_SLOT, R_XTENSA_RELATIVE}

LOADED_NAMES = (".text", ".bss", ".data", ".rodata", ".data.rel.ro")


class Section:
    def __init__(self, name: str, type_: int, flags: int, addr: int, offset: int, size: int):
        self.name = name
        self.type = type_
        self.flags = flags
        self.addr = addr
        self.offset = offset
        self.size = size


class Elf:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        if self.data[:4] != b"\x7fELF":
            raise ValueError("not an ELF file")
        if self.data[4] != 1:
            raise ValueError("not ELF32")
        endian = self.data[5]
        if endian not in (1, 2):
            raise ValueError("unknown endianness")
        self.order = "<" if endian == 1 else ">"
        (self.type, self.machine) = struct.unpack_from(self.order + "HH", self.data, 0x10)
        (self.shoff,) = struct.unpack_from(self.order + "I", self.data, 0x20)
        (self.shentsize,) = struct.unpack_from(self.order + "H", self.data, 0x2e)
        (self.shnum,) = struct.unpack_from(self.order + "H", self.data, 0x30)
        (self.shstrndx,) = struct.unpack_from(self.order + "H", self.data, 0x32)
        self.sections = self._read_sections()

    def _string(self, table: Section, offset: int) -> str:
        start = table.offset + offset
        end = self.data.index(b"\x00", start)
        return self.data[start:end].decode("utf-8", "replace")

    def _read_sections(self) -> list[Section]:
        raw = []
        for index in range(self.shnum):
            base = self.shoff + index * self.shentsize
            name, type_, flags, addr, offset, size, _link, _info, _align, _entsize = struct.unpack_from(
                self.order + "IIIIIIIIII", self.data, base
            )
            raw.append((name, type_, flags, addr, offset, size))
        if not raw:
            return []
        name_table = Section("shstrtab", 0, 0, 0, raw[self.shstrndx][4], raw[self.shstrndx][5])
        sections = []
        for name, type_, flags, addr, offset, size in raw:
            sections.append(Section(self._string(name_table, name), type_, flags, addr, offset, size))
        return sections

    def loaded_sections(self) -> dict[str, Section]:
        """Mirrors esp_elf_load_section()'s section selection."""
        loaded: dict[str, Section] = {}
        for section in self.sections:
            if section.type == SHT_NOBITS:
                if (section.flags & (SHF_ALLOC | SHF_WRITE)) == (SHF_ALLOC | SHF_WRITE) and section.name == ".bss":
                    loaded[".bss"] = section
                continue
            if not (section.flags & SHF_ALLOC) or section.type != 1:
                continue
            if section.flags & SHF_EXECINSTR and section.name == ".text":
                loaded[".text"] = section
            elif section.flags & SHF_WRITE and section.name == ".data":
                loaded[".data"] = section
            elif section.name == ".rodata":
                loaded[".rodata"] = section
            elif section.name == ".data.rel.ro":
                loaded[".data.rel.ro"] = section
        return loaded

    def section_at(self, vaddr: int) -> Section | None:
        for section in self.sections:
            if section.addr <= vaddr < section.addr + section.size:
                return section
        return None

    def read_word(self, vaddr: int) -> int | None:
        section = self.section_at(vaddr)
        if section is None:
            return None
        if section.type == SHT_NOBITS:
            return 0  # the loader's data buffer is zeroed for .bss
        file_offset = section.offset + (vaddr - section.addr)
        if file_offset + 4 > len(self.data):
            return None
        return struct.unpack_from(self.order + "I", self.data, file_offset)[0]

    def map_sym(self, sym: int, loaded: dict[str, Section]) -> int | None:
        """Returns the loader's mapped address, or None when it would be 0."""
        if sym == 0:
            return None
        for name in LOADED_NAMES:
            section = loaded.get(name)
            if section is None:
                continue
            if section.addr <= sym < section.addr + section.size:
                return sym - section.addr  # base is irrelevant for the zero check
        return None

    def audit(self) -> list[str]:
        problems: list[str] = []
        loaded = self.loaded_sections()
        if ".text" not in loaded:
            problems.append("no .text section")
            return problems

        for section in self.sections:
            if section.type != SHT_RELA:
                continue
            entries = section.size // 12
            for index in range(entries):
                base = section.offset + index * 12
                offset, info, _addend = struct.unpack_from(self.order + "III", self.data, base)
                rel_type = info & 0xFF
                where = self.section_at(offset)
                if where is None:
                    problems.append(
                        f"{section.name}[{index}]: relocation site 0x{offset:08x} is outside every section"
                    )
                    continue
                if rel_type not in SUPPORTED_XTENSA:
                    problems.append(
                        f"{section.name}[{index}]: unsupported relocation type {rel_type} at 0x{offset:08x}"
                    )
                    continue
                if rel_type != R_XTENSA_RELATIVE:
                    continue
                addend = self.read_word(offset)
                if addend is None:
                    problems.append(
                        f"{section.name}[{index}]: cannot read addend at 0x{offset:08x}"
                    )
                    continue
                if addend != 0 and self.map_sym(addend, loaded) is None:
                    problems.append(
                        f"{section.name}[{index}]: addend 0x{addend:08x} at 0x{offset:08x} "
                        "maps to NULL (one-past-section-end sentinel?)"
                    )
        return problems


def discover() -> list[Path]:
    root = Path(__file__).resolve().parents[2]
    found = sorted(root.glob("*/build/cmake-build-*/tactility.*.app.elf"))
    if not found:
        found = sorted(root.glob("build/package-intermediate/bin/*/*.elf"))
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*", type=Path, help="packaged .app.elf files (default: discover built apps)")
    parser.add_argument("--quiet", action="store_true", help="only print failures and the summary")
    args = parser.parse_args()

    paths = args.paths or discover()
    if not paths:
        print("elf-audit: no packaged ELFs found; build the app first", file=sys.stderr)
        return 1

    failures = 0
    checked = 0
    for path in paths:
        label = f"{path.parent.name}/{path.name}"
        try:
            elf = Elf(path)
        except (OSError, ValueError, struct.error) as error:
            print(f"elf-audit: {label}: {error}", file=sys.stderr)
            failures += 1
            continue
        if elf.machine == EM_RISCV:
            if not args.quiet:
                print(f"{label}: skipped (RISC-V uses the segment loader)")
            continue
        if elf.machine != EM_XTENSA:
            if not args.quiet:
                print(f"{label}: skipped (machine {elf.machine})")
            continue
        problems = elf.audit()
        checked += 1
        if problems:
            failures += 1
            print(f"{label}: {len(problems)} problem(s)")
            for problem in problems:
                print(f"  {problem}")
        elif not args.quiet:
            print(f"{label}: OK")

    print(f"elf-audit: {checked} Xtensa image(s) checked, {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
