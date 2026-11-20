"""Build description of the OMKit loader on top of stock E148 1.1 (platform om5m2, firmware 1.1: fw1100).

    python -X utf8 -m OMKit.src.stub.platform.om5m2.fw1100.gen     -> spec.json next to this file

spec.json is the input of the patch builder: the hook body stub.s at BODY, its trampoline at TRAMP, the hook at SITE.
gen() takes another site and name for a variant of the same loader (the same body at another hook site).
"""
import json
from pathlib import Path

SITE = 0x601B1362                 # FastEndCb: on the camera it runs 1.19 s after power-on
TRAMP, BODY = 0x605AF4C0, 0x605AF510         # tail of section 2, "low" risk; code does not write here: writing here on the camera causes a crash
DONE = 0x615EE7C0                 # tail of section 9: earlier test builds wrote here on the camera; it is 0 again at every power-on
BUF, ENTRY = 0xA345A000, 0x6345A010          # start of the resource gap with a margin; the entry address is cached
STACK_LEN = 0x800                 # from the kernel pool, like BUSMES_MON; core switches to its own stack right away

SYMBOLS = {
    "omk_done": DONE, "omk_sections": 0xA26E2698,
    "f_keyup": 0x60130FB8, "f_acre": 0x60051D5C, "f_sleep": 0x60051A2B, "f_ext": 0x6003167E,
    "f_fopen": 0x606AA3BD, "f_fread": 0x606AA54E, "f_fclose": 0x606AA3C6,
    "OMK_MAGIC": 0x434B4D4F, "OMK_ABI": 1, "OMK_MAX": 0x300000, "OMK_BUF": BUF, "OMK_ENTRY": ENTRY,
    "OMK_STACK_LEN": STACK_LEN, "OMK_PRIO": 66, "OMK_TRIES": 10, "OMK_DELAY": 1000,
    "OMK_SEC_TRIES": 300, "OMK_SEC_DELAY": 100,
}


def gen(site: int = SITE, name: str = "omkit_stub") -> dict:
    return {
        "model": "E148", "name": name,
        "comment": "OMKit loader v2: stock E148 1.1 + hook. A changed build needs the maintainers' review before it goes to a camera.",
        "symbols": {k: hex(v) for k, v in SYMBOLS.items()},
        "blobs": [{"name": "omk_hook", "at": hex(BODY), "asm": "stub.s"},
                  {"name": "omk_data", "at": hex(DONE), "bytes": "00000000"}],
        "hooks": [{"name": "omk_site", "site": hex(site), "tramp": hex(TRAMP), "body": "omk_hook", "frame": 16}],
    }


def write(path: Path, spec: dict) -> None:
    path.write_text(json.dumps(spec, ensure_ascii=False, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(path)


def main() -> int:
    write(Path(__file__).with_name("spec.json"), gen())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
