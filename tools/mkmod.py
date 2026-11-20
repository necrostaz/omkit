"""Build an OMKit module file .OMM from an ELF linked at address 0 with --emit-relocs (OMKit/src/modules/Makefile).

    python3 mkmod.py build hello.elf HELLO.OMM --name HELLO --version 1.0 [--res hello/res] [--res-disk DIR] [--api 3]
                     [--res-over DIR]
    python3 mkmod.py header hello/res/strings.txt res_strings.h      # STR_NAME string ids for the module; empty without a file

Format: OMKit/src/core/include/omk_mod.h. Relocations: R_MN10300_32 words in loadable sections, except pairs with
R_MN10300_SYM_DIFF (address difference) and references to absolute symbols. Relative jumps and calls are left alone.
Undefined symbols and absolute references 8, 16 or 24 bits wide are errors: a module is not linked against the firmware.

Resources: files res/<type>_<id>.<anything> are kept in memory, res/disk/<type>_<id>.* are read from the card on demand.
--res-disk DIR adds the files of DIR as on-demand resources of the module (as res/disk; e.g. the pictures committed in
OMKit/resources/modules/<mod>/icons); a type and id defined twice is an error.
--res-over DIR (as res: DIR/<type>_<id>.* in memory, DIR/disk/ on demand) replaces the module's own resources of the same
type and id: for private builds with data that is not part of OMKit; the output lists what it adds or replaces.
Type 1..32767, id 0..65535, decimal. Strings: res/strings.txt with "NAME = text", English only, printable ASCII
(0x20-0x7E), comments # and ;. Ids follow line order, identical texts are stored once. The table is resource 0x8000/0.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

MAGIC = 0x4D4B4D4F                      # 'OMKM'
FORMAT = 1
HEAD = 64
EM_MN10300 = 89
SHT_SYMTAB, SHT_RELA, SHT_NOBITS = 2, 4, 8
SHF_ALLOC = 2
SHN_UNDEF, SHN_ABS = 0, 0xFFF1
R_NONE, R_32, R_16, R_8, R_PCREL32, R_PCREL16, R_PCREL8, R_VTINHERIT, R_VTENTRY, R_24 = range(10)
R_SYM_DIFF, R_ALIGN = 33, 34
RES_STRINGS = 0x8000
RES_ONDEMAND = 1
MAX_STRINGS_BYTES = 0x10000
RELATIVE = {R_NONE, R_PCREL32, R_PCREL16, R_PCREL8, R_VTINHERIT, R_VTENTRY, R_ALIGN}


class ModError(Exception):
    pass


class Elf:
    def __init__(self, data: bytes):
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            raise ModError("ELF32 little-endian required")
        (_, machine, _, entry, _, shoff, _, _, _, _, shentsize, shnum, shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
        if machine != EM_MN10300:
            raise ModError(f"machine {machine}, MN10300 required")
        self.data = data
        self.sections = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from("<10I", data, shoff + i * shentsize)
            self.sections.append(dict(name=name, type=typ, flags=flags, addr=addr, off=off, size=size, link=link, info=info))
        names = self.sections[shstrndx]
        for s in self.sections:
            s["name"] = self.cstr(names["off"] + s["name"])

    def cstr(self, off: int) -> str:
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("ascii", "replace")

    def symbols(self) -> list[dict]:
        out = []
        for s in self.sections:
            if s["type"] != SHT_SYMTAB:
                continue
            strtab = self.sections[s["link"]]
            for k in range(s["size"] // 16):
                name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", self.data, s["off"] + 16 * k)
                out.append(dict(name=self.cstr(strtab["off"] + name), value=value, shndx=shndx, bind=info >> 4))
        return out


def api_version() -> int:
    text = (Path(__file__).resolve().parents[1] / "src" / "sdk" / "include" / "omk_api.h").read_text(encoding="utf-8")
    return int(re.search(r"#define OMK_API_VERSION (\d+)", text).group(1))


def compile_strings(text: str, source: str = "strings.txt") -> tuple[bytes, list[str]]:
    """String table {u16 count, u16 0, u16 offsets[count]} and zero-terminated strings; names in order"""
    names, texts = [], []
    for no, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line[0] in "#;":
            continue
        name, eq, value = line.partition("=")
        name, value = name.strip(), value.strip()
        if not eq or not re.fullmatch(r"[A-Z][A-Z0-9_]*", name):
            raise ModError(f"{source}:{no}: expected NAME = text, name of A-Z 0-9 _")
        if name in names:
            raise ModError(f"{source}:{no}: {name} is already defined")
        bad = [c for c in value if not 0x20 <= ord(c) <= 0x7E]
        if bad:
            raise ModError(f"{source}:{no}: English only, printable ASCII; found {bad[0]!r}")
        names.append(name)
        texts.append(value)
    head = 4 + 2 * len(texts)
    pool, offsets, seen = bytearray(), [], {}
    for t in texts:
        if t not in seen:
            seen[t] = head + len(pool)
            pool += t.encode("ascii") + b"\0"
        offsets.append(seen[t])
    blob = struct.pack(f"<HH{len(offsets)}H", len(offsets), 0, *offsets) + bytes(pool)
    if not texts:
        blob += b"\0"                       # the last byte of the table is zero
    if len(blob) > MAX_STRINGS_BYTES:
        raise ModError(f"{source}: strings take {len(blob)} bytes, the limit is {MAX_STRINGS_BYTES}")
    return blob, names


def strings_header(names: list[str]) -> str:
    lines = ["/* Module string ids: generated by tools/mkmod.py from res/strings.txt, do not edit */", "#ifndef RES_STRINGS_H",
             "#define RES_STRINGS_H", ""]
    lines += [f"#define STR_{n} {i}" for i, n in enumerate(names)]
    lines += [f"#define STR_COUNT {len(names)}", "", "#endif", ""]
    return "\n".join(lines)


def build(elf_bytes: bytes, name: str, version: tuple[int, int], res_dir: Path | None, api: int,
          res_over: Path | None = None, res_disk: Path | None = None, notes: list[str] | None = None) -> bytes:
    if not re.fullmatch(r"[A-Z0-9_]{1,8}", name) or name == "CORE":
        raise ModError("name: 1-8 characters A-Z 0-9 _, not CORE")
    elf = Elf(elf_bytes)
    alloc = [s for s in elf.sections if s["flags"] & SHF_ALLOC and s["size"]]
    loaded = [s for s in alloc if s["type"] != SHT_NOBITS]
    bss = [s for s in alloc if s["type"] == SHT_NOBITS]
    if not loaded or min(s["addr"] for s in loaded) != 0:
        raise ModError("the image must start at address 0 (module.ld)")
    image_end = max(s["addr"] + s["size"] for s in loaded)
    if bss and min(s["addr"] for s in bss) < image_end:
        raise ModError("bss must follow the image")
    image = bytearray(image_end)
    for s in loaded:
        image[s["addr"]:s["addr"] + s["size"]] = elf.data[s["off"]:s["off"] + s["size"]]
    end = max([image_end] + [s["addr"] + s["size"] for s in bss])
    image += bytes((-len(image)) % 4)
    bss_size = end - image_end
    bss_size += (-(len(image) + bss_size)) % 4

    syms = elf.symbols()
    undefined = sorted({s["name"] for s in syms if s["shndx"] == SHN_UNDEF and s["name"]})
    if undefined:
        raise ModError("undefined symbols: " + ", ".join(undefined))
    entry = [s for s in syms if s["name"] == "_module_init" and s["shndx"] not in (SHN_UNDEF, SHN_ABS)]
    if not entry:
        raise ModError("no module_init (symbol _module_init)")

    relocs = set()
    for rs in elf.sections:
        if rs["type"] != SHT_RELA:
            continue
        target = elf.sections[rs["info"]]
        if not target["flags"] & SHF_ALLOC:
            continue
        symtab = elf.sections[rs["link"]]
        rows = [struct.unpack_from("<IIi", elf.data, rs["off"] + 12 * k) for k in range(rs["size"] // 12)]
        diff_at = {off for off, info, _ in rows if info & 0xFF == R_SYM_DIFF}
        for off, info, _ in rows:
            typ, symi = info & 0xFF, info >> 8
            sym = struct.unpack_from("<IIIBBH", elf.data, symtab["off"] + 16 * symi)
            absolute = sym[5] == SHN_ABS
            if typ in RELATIVE or typ == R_SYM_DIFF:
                continue
            if typ == R_32:
                if off in diff_at or absolute:
                    continue
                if target["type"] == SHT_NOBITS or off + 4 > image_end:
                    raise ModError(f"relocation outside the image: {off:#x}")
                relocs.add(off)
            elif typ in (R_16, R_8, R_24) and absolute:
                continue
            else:
                raise ModError(f"relocation of type {typ} at {off:#x} ({target['name']}) is not supported")

    dir_rows, blob, disk = [], bytearray(), bytearray()

    def add(typ: int, rid: int, data: bytes, ondemand: bool) -> None:
        area = disk if ondemand else blob
        dir_rows.append(struct.pack("<HHHHII", typ, rid, 0, RES_ONDEMAND if ondemand else 0, len(area), len(data)))
        area.extend(data + bytes((-len(data)) % 4))

    found = {}                              # (type, id) -> (data, on demand); a res-over folder replaces the module's own
    own = [(res_dir, False), (res_dir / "disk" if res_dir else None, True), (res_disk, True)]
    over = [(res_over, False), (res_over / "disk" if res_over else None, True)]
    for group in (own, over):
        seen = set()
        for folder, ondemand in group:
            if not folder or not folder.is_dir():
                continue
            for f in sorted(folder.iterdir()):
                m = re.fullmatch(r"(\d+)_(\d+)(?:\..*)?", f.name)
                if not f.is_file() or not m:
                    continue
                typ, rid = int(m.group(1)), int(m.group(2))
                if not 1 <= typ <= 0x7FFF or rid > 0xFFFF:
                    raise ModError(f"{f.name}: type 1..32767, id 0..65535")
                if (typ, rid) in seen:
                    raise ModError(f"{f.name}: resource {typ}/{rid} is already defined")
                seen.add((typ, rid))
                if group is over and notes is not None:
                    notes.append(f"{typ}/{rid} from {f}" + (" replaces the module's own" if (typ, rid) in found else ""))
                found[(typ, rid)] = (f.read_bytes(), ondemand)
    for (typ, rid), (data, ondemand) in sorted(found.items()):
        add(typ, rid, data, ondemand)
    strings = res_dir / "strings.txt" if res_dir else None
    if strings and strings.is_file():
        add(RES_STRINGS, 0, compile_strings(strings.read_text(encoding="ascii", errors="replace"), str(strings))[0], False)

    rel = b"".join(struct.pack("<I", o) for o in sorted(relocs))
    body = bytes(image) + rel + b"".join(dir_rows) + bytes(blob) + bytes(disk)
    ver = version[0] << 16 | version[1]
    head = struct.pack("<IHH8s10I8x", MAGIC, FORMAT, api, name.encode("ascii"), ver, len(image), bss_size,
                       entry[0]["value"], len(relocs), len(dir_rows), len(blob), 0, 0, len(disk))
    assert len(head) == HEAD
    crc = zlib.crc32(head + body) & 0xFFFFFFFF
    head = head[:48] + struct.pack("<I", crc) + head[52:]
    return head + body


def parse(data: bytes) -> dict:
    """Parse an .OMM header: for tests and inspection"""
    (magic, fmt, api, name, ver, image, bss, entry, relocs, res_count, res_bytes, flags, crc, res_disk) = struct.unpack_from(
        "<IHH8s10I", data)
    return dict(magic=magic, format=fmt, api=api, name=name.rstrip(b"\0").decode(), version=ver, image=image, bss=bss,
                entry=entry, relocs=relocs, res_count=res_count, res_bytes=res_bytes, res_disk=res_disk, flags=flags, crc=crc,
                crc_ok=zlib.crc32(data[:48] + b"\0\0\0\0" + data[52:]) & 0xFFFFFFFF == crc)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("elf")
    b.add_argument("out")
    b.add_argument("--name", required=True)
    b.add_argument("--version", default="1.0")
    b.add_argument("--res")
    b.add_argument("--res-disk", help="a folder of on-demand resources added to the module's own (as res/disk)")
    b.add_argument("--res-over", help="resources that replace the module's own ones of the same type and id")
    b.add_argument("--api", type=int)
    h = sub.add_parser("header")
    h.add_argument("strings", help="res/strings.txt; an empty string means no file")
    h.add_argument("out")
    a = ap.parse_args()
    try:
        if a.cmd == "header":
            names = []
            if a.strings and Path(a.strings).is_file():
                names = compile_strings(Path(a.strings).read_text(encoding="ascii", errors="replace"), a.strings)[1]
            Path(a.out).parent.mkdir(parents=True, exist_ok=True)
            Path(a.out).write_text(strings_header(names), encoding="utf-8", newline="\n")
            return 0
        major, minor = (int(x) for x in a.version.split("."))
        notes: list[str] = []
        data = build(Path(a.elf).read_bytes(), a.name, (major, minor), Path(a.res) if a.res else None, a.api or api_version(),
                     Path(a.res_over) if a.res_over else None, Path(a.res_disk) if a.res_disk else None, notes)
    except ModError as e:
        print(f"mkmod: {e}", file=sys.stderr)
        return 1
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_bytes(data)
    for n in notes:
        print(f"{a.out}: res-over: {n}")
    hd = parse(data)
    print(f"{a.out}: {len(data)} bytes, image {hd['image']}, bss {hd['bss']}, relocs {hd['relocs']}, "
          f"resources {hd['res_count']}: in memory {hd['res_bytes']} bytes, on demand {hd['res_disk']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
