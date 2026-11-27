#!/usr/bin/env python3
"""Check the build kind of OMKit images: a release image has nothing of the debug build.

    python3 tools/buildcheck.py [--kind release|debug] [--map core.map] CORE.BIN [NAME.OMM ...]

core and the modules are built in two kinds (docs/build.md, "Release and debug builds"). The code of the debug kind -
the debug service over USB, the task dump, the probes of the camera's state, the test scenario, the log's debug level -
is compiled only with OMK_DEBUG, and this tool checks the result:

  CORE.BIN   names its kind in the text of the log's first line ("OMKit core 1.45 release"). A release image holds
             none of the texts and none of the hardware register addresses listed below; with --map (the linker's map
             of that build) also none of the debug kind's functions and object files.
  NAME.OMM   a debug build of a module holds the text of its debug marker line (OMK_LOG_D in its module_init); a
             release build does not.

Without --kind the kind found in CORE.BIN is taken (the modules must be of the same kind). Prints one line per file and
exits 1 when a check fails.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

KIND_RE = re.compile(rb"OMKit core (\d+\.\d+) (release|debug) ")
MODULE_MARK = b"a debug build of the module"

# Texts of log lines, settings and files that only the debug kind has
DEBUG_TEXTS = [
    b"power probe", b"viewfinder", b"taskdump", b"[debug]", b"debug:", b"RAM log", b"OMKDBG", b"SHOOT.RUN", b"SHOOT.END",
    b"shoot:", b"hands_free", b"hands-free", b"power_rescue", b"rescue", b"mask_test", b"chroma_test", b"census",
    b"dial trace", b"emu:", b"emu_restart", b"stuck", b"watchdog", b"NMI record", b"core_events", b"fw_cache_inv",
    b"boot: config", b"boot: installing", b"stack: %u of %u bytes used", b"subprocessor clock", b"schedule", b"crumb", b"bench", b"heartbeat",
    b"worker_test", b"bisect",
]
# Hardware registers only the debug kind reads or writes (little-endian words in the code): the video compositor block
# (the screen or viewfinder word of the probe line), the subprocessor clock, the reset cause
DEBUG_WORDS = {0x9C000000: "video compositor registers", 0x9C000178: "video compositor register 0x178",
               0x98E00000: "subprocessor clock registers", 0x8FF000B0: "reset cause register"}
# Functions of the debug kind (the linker's map): by prefix and by name
DEBUG_PREFIXES = ("dbg_", "taskdump_", "shoot_", "hf_", "dring_", "drv_usb_", "drv_restart_", "drv_emu_", "bis_",
                  "rescue_", "optics_bench", "optics_schedule", "crumb_")
DEBUG_NAMES = {
    "drv_power_probe", "drv_ui_probe", "drv_task_read", "drv_obj_read", "drv_mutex_read", "drv_keep_area", "drv_ptp_add",
    "drv_ptp_call", "drv_ptp_entry", "drv_ptp_send", "drv_ptp_recv", "drv_ptp_recv_len", "drv_ptp_history",
    "drv_mem_readable", "drv_rename", "drv_shutter", "drv_reset", "drv_fw_reset", "drv_ok_key", "drv_key_api",
    "drv_app_event", "drv_rtc", "drv_crash_read", "drv_look_trace", "drv_prof_lv_read", "drv_isp_census",
    "drv_look_choose", "drv_look_find", "drv_ev_other_cpu", "drv_edp_gaps", "drv_grain_route_state", "drv_exif_count",
    "log_tee", "log_card", "log_checkpoint", "log_room", "store_card", "core_card_write", "core_card_ok", "core_run",
    "power_cancels", "power_hook_count", "cfg_once", "cfg_once_drop", "cfg_once_str", "omk_ms_add",
}
DEBUG_OBJECTS = ("taskdump.o", "dring.o", "debug.o", "shoot.o", "handsfree.o")


def map_symbols(text: str) -> set[str]:
    """The names the linker's map places at an address (lines "   0x6345a010   _name")"""
    return {m.group(1).lstrip("_") for m in re.finditer(r"^\s+0x[0-9a-f]+\s+(_?[A-Za-z]\w*)$", text, re.M)}


def check_core(data: bytes, kind: str | None, map_text: str | None) -> tuple[str | None, list[str]]:
    faults = []
    kinds = {m.group(2).decode() for m in KIND_RE.finditer(data)}
    if len(kinds) != 1:
        return None, [f"the image names {len(kinds)} build kinds (the text 'OMKit core N.NN release|debug')"]
    found = kinds.pop()
    if kind and found != kind:
        faults.append(f"the image is a {found} build, {kind} expected")
    symbols = map_symbols(map_text) if map_text is not None else None
    if found == "release":
        for t in DEBUG_TEXTS:
            if t in data:
                faults.append(f"text of the debug build: {t.decode()!r}")
        for word, name in DEBUG_WORDS.items():
            if struct.pack("<I", word) in data:
                faults.append(f"the word {word:#010x} ({name}) is in the image")
        if symbols is not None:
            bad = sorted(s for s in symbols if s in DEBUG_NAMES or s.startswith(DEBUG_PREFIXES))
            if bad:
                faults.append("functions of the debug build: " + ", ".join(bad))
            objs = [o for o in DEBUG_OBJECTS if re.search(r"[/\\]" + re.escape(o) + r"\b", map_text)]
            if objs:
                faults.append("object files of the debug build: " + ", ".join(objs))
    elif symbols is not None and "dbg_service" not in symbols:
        faults.append("a debug build without the debug service (dbg_service is not in the map)")
    return found, faults


def check_module(data: bytes, kind: str) -> list[str]:
    has = MODULE_MARK in data
    if kind == "release" and has:
        return ["the module is a debug build (its debug marker line is in the file)"]
    if kind == "debug" and not has:
        return ["the module is not a debug build (no debug marker line in the file)"]
    return []


def main() -> int:
    ap = argparse.ArgumentParser(description="Check the build kind of CORE.BIN and of modules")
    ap.add_argument("--kind", choices=("release", "debug"))
    ap.add_argument("--map", help="the linker's map of the core build (core.map)")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()
    kind = args.kind
    failed = False
    files = sorted(args.files, key=lambda f: not f.upper().endswith(".BIN"))   # core first: it names the kind
    for name in files:
        data = Path(name).read_bytes()
        if name.upper().endswith(".BIN"):
            map_text = Path(args.map).read_text(encoding="utf-8", errors="replace") if args.map else None
            found, faults = check_core(data, kind, map_text)
            kind = kind or found
            what = f"{found or '?'} build, {len(data)} bytes" + ("" if args.map else " (no map: texts and words only)")
        else:
            if not kind:
                print(f"  FAIL  {name}: no --kind and no CORE.BIN to take it from")
                failed = True
                continue
            faults = check_module(data, kind)
            what = f"{kind} build, {len(data)} bytes"
        if faults:
            failed = True
            print(f"  FAIL  {name}: {what}")
            for f in faults:
                print(f"        {f}")
        else:
            print(f"  ok    {name}: {what}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
