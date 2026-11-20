"""The OMKIT.INI of the card bundle: core's own sample with the bundled modules turned on.

    python3 OMKit/tools/cardini.py write DEST NAME[:VERSION]...
    python3 OMKit/tools/cardini.py check FILE NAME[:VERSION]...

`core` writes a sample OMKIT.INI when the card has none (the C string cfg_template in src/core/cfg.c); that sample
loads no module. The card bundle (`make -C OMKit card`, and with it a release) carries an OMKIT.INI so that the bundled
modules run from the first start. This tool makes it from the sample itself, read out of cfg.c, so the two cannot drift:
every line of the sample is kept as it is (its comments explain the keys), `enabled = on` in [modules], and after the
sample's [modules] lines a comment and one `NAME = on` line per module, in the order given (the load order). The
arguments are the words of RELEASE_MODULES in the Makefile (`creatprf:1.5`); the version part is ignored.

The result is checked by the rules core parses the file with (src/core/cfg.c): at most CFG_MAX bytes and CFG_ENTRIES
`key = value` lines, no line core would not understand, the sample's line ends (CR LF), and every module on. `check`
compares an existing file with what `write` would write.

Standard library only.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

CFG_C = Path(__file__).resolve().parent.parent / "src" / "core" / "cfg.c"
MODULE_NAME = re.compile(r"^[A-Z0-9_]{1,8}$")    # core: 1-8 characters A-Z 0-9 _, an 8.3 file name, not CORE
MAX_MODULES = 16                                 # core: at most 16 modules
NOTE = "; ON: the bundled modules of this OMKit card (the example above is only a comment; without this file, core writes its sample, which loads none)"
_ESCAPES = {"\\\\": "\\", '\\"': '"', "\\'": "'", "\\r": "\r", "\\n": "\n", "\\t": "\t"}


class IniError(ValueError):
    pass


def _define(src: str, name: str) -> int:
    m = re.search(rf"^#define\s+{name}\s+(\d+)\s*$", src, re.M)
    if not m:
        raise IniError(f"{name} not found in {CFG_C.name}")
    return int(m.group(1))


def _c_string(body: str) -> str:
    """Concatenated C string literals, whitespace between them only, simple escapes only."""
    out, pos = [], 0
    for m in re.finditer(r'"((?:[^"\\\n]|\\.)*)"', body):
        if body[pos:m.start()].strip():
            raise IniError(f"cfg_template: unexpected text {body[pos:m.start()].strip()[:20]!r}")
        for piece in re.split(r"(\\.)", m.group(1)):
            if piece.startswith("\\"):
                if piece not in _ESCAPES:
                    raise IniError(f"cfg_template: escape {piece!r} not handled")
                out.append(_ESCAPES[piece])
            else:
                out.append(piece)
        pos = m.end()
    if body[pos:].strip() or not out:
        raise IniError("cfg_template: not a plain sequence of string literals")
    return "".join(out)


def limits(cfg_c: Path = CFG_C) -> tuple[int, int]:
    """(CFG_MAX, CFG_ENTRIES) of core."""
    src = cfg_c.read_text("utf-8")
    return _define(src, "CFG_MAX"), _define(src, "CFG_ENTRIES")


def sample(cfg_c: Path = CFG_C) -> str:
    """The text core writes when the card has no OMKIT.INI."""
    m = re.search(r"static\s+const\s+char\s+cfg_template\[\]\s*=(.*?);\s*$", cfg_c.read_text("utf-8"), re.S | re.M)
    if not m:
        raise IniError(f"cfg_template not found in {cfg_c}")
    return _c_string(m.group(1))


def module_names(words: list[str]) -> list[str]:
    names = [w.partition(":")[0].upper() for w in words]
    for n in names:
        if not MODULE_NAME.match(n) or n == "CORE":
            raise IniError(f"module name {n!r}: 1-8 characters A-Z 0-9 _, not CORE")
    if len(set(names)) != len(names) or not 0 < len(names) <= MAX_MODULES:
        raise IniError(f"modules {names}: 1..{MAX_MODULES} different names expected")
    return names


def parse(text: str) -> tuple[list[tuple[str, str, str]], list[int]]:
    """core's parser (cfg.c parse): ([(section, key, value)], numbers of the lines it would not understand)."""
    if text.startswith("﻿"):
        text = text[1:]
    entries, bad, section = [], [], ""
    for n, line in enumerate(text.split("\n"), 1):
        line = line.strip(" \t\r")
        if not line or line[0] in ";#":
            continue
        if line[0] == "[":                          # no "]": a bad line, even with an "=" in it
            if "]" in line:
                section = line[1:line.index("]")].strip(" \t\r")
            else:
                bad.append(n)
            continue
        key, eq, value = line.partition("=")
        if eq and key:
            entries.append((section, key.strip(" \t\r"), value.strip(" \t\r")))
        else:
            bad.append(n)
    return entries, bad


def make(words: list[str], cfg_c: Path = CFG_C) -> bytes:
    """The card's OMKIT.INI for the modules `words` (NAME or name:version)."""
    names = module_names(words)
    text = sample(cfg_c)
    eol = "\r\n" if "\r\n" in text else "\n"
    if text.count("\r") != text.count("\r\n") or (eol == "\r\n" and text.count("\n") != text.count("\r\n")):
        raise IniError("cfg_template: mixed line ends")
    if not text.endswith(eol):
        raise IniError("cfg_template: the last line has no line end")
    lines = text[:-len(eol)].split(eol)
    heads = [i for i, s in enumerate(lines) if re.match(r"^\s*\[", s)]
    mods = [i for i in heads if lines[i].strip(" \t").lower() == "[modules]"]
    if len(mods) != 1:
        raise IniError("cfg_template: expected one [modules] section")
    start = mods[0]
    end = next((i for i in heads if i > start), len(lines))
    while end > start + 1 and not lines[end - 1].strip():       # module lines go before the blank lines that end it
        end -= 1
    body = lines[start + 1:end]
    keys = [s.partition("=")[0].strip().lower() for s in body if s.strip() and s.strip()[0] not in ";#"]
    if [k for k in keys if k != "enabled"]:
        raise IniError(f"cfg_template: [modules] already has keys {keys}; update this tool")
    body = [re.sub(r"=.*$", "= on", s) if s.strip() and s.strip()[0] not in ";#" else s for s in body]
    if not keys:
        body.append("enabled = on")
    body += [NOTE] + [f"{n} = on" for n in names]
    out = eol.join(lines[:start + 1] + body + lines[end:]) + eol
    check_text(out, names, cfg_c)
    return out.encode("ascii")


def check_text(text: str, names: list[str], cfg_c: Path = CFG_C) -> None:
    """core would read every line of text, and every module of names is on."""
    cfg_max, cfg_entries = limits(cfg_c)
    data = text.encode("ascii")
    if len(data) > cfg_max:
        raise IniError(f"{len(data)} bytes: core reads at most {cfg_max}")
    entries, bad = parse(text)
    if bad:
        raise IniError(f"lines core would not understand: {bad}")
    if len(entries) > cfg_entries:
        raise IniError(f"{len(entries)} entries: core keeps at most {cfg_entries}")
    last = {}
    for sec, key, value in entries:                      # the last value wins
        last[(sec.lower(), key.lower())] = value.lower()
    if last.get(("modules", "enabled"), "on") != "on":
        raise IniError("[modules] enabled is not on")
    for n in names:
        if last.get(("modules", n.lower())) != "on":
            raise IniError(f"[modules] {n} is not on")


def main(argv: list[str]) -> int:
    if len(argv) < 3 or argv[0] not in ("write", "check"):
        print("usage: cardini.py write DEST NAME[:VERSION]... | cardini.py check FILE NAME[:VERSION]...",
              file=sys.stderr)
        return 2
    try:
        want = make(argv[2:])
    except IniError as e:
        print(f"cardini: {e}", file=sys.stderr)
        return 1
    path = Path(argv[1])
    if argv[0] == "write":
        path.write_bytes(want)
        print(f"cardini: {path}: {len(want)} bytes, modules on: {', '.join(module_names(argv[2:]))}")
        return 0
    if not path.is_file() or path.read_bytes() != want:
        print(f"cardini: {path} differs from core's sample with the modules {', '.join(module_names(argv[2:]))} on",
              file=sys.stderr)
        return 1
    print(f"cardini: {path} up to date")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
