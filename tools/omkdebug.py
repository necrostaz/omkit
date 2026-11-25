"""OMKit DEBUG over USB: read the camera's RAM log and state, read files of the card, update OMKit files.

The camera must run OMKit core 1.40 or later with `[debug] enabled = on` in OMKIT.INI (docs/debug.md; restart, shoot, info
and session need a core that has them) and be connected by USB in the MTP mode (choose MTP in the camera's USB dialog).
The tool talks to it through Windows Portable Devices (WPD) - the camera's own driver, no driver change; it sends only the
vendor operations 0x9E00-0x9E02 of core's debug service, and the firmware's own reboot request 0x970A for `restart --ptp`.
Windows only; needs the comtypes package (`pip install comtypes`). The parsing and the session's steps are plain Python and
run anywhere (tests/test_omkdebug.py, tests/test_omkdebug_session.py).

    python tools/omkdebug.py hello                    the service, its flags, the power-on number
    python tools/omkdebug.py tail [--follow]          the RAM log of this power-on (lines and snapshots)
    python tools/omkdebug.py postmortem               the RAM log of the power-on before (after a reset)
    python tools/omkdebug.py banks                    both bank headers
    python tools/omkdebug.py snap [-o FILE]           every task of the three CPUs, now
    python tools/omkdebug.py mem ADDR LEN [-o FILE]   memory, as the white list allows (RAM only)
    python tools/omkdebug.py hist                     the firmware's last 20 PTP operations
    python tools/omkdebug.py level N                  the RAM log's level 0..4 (in RAM only)
    python tools/omkdebug.py mark [TAG]               a mark and a snapshot in the RAM log
    python tools/omkdebug.py ls PATH                  a folder of the card (OMKIT\\..., DCIM\\...)
    python tools/omkdebug.py get PATH [-o FILE]       a file of the card
    python tools/omkdebug.py pictures [-n N] [-o DIR] the last N pictures of DCIM (JPEG and ORF)
    python tools/omkdebug.py put FILE PATH            write a file into the OMKit folder ([debug] write = on)
    python tools/omkdebug.py info                     the build of core (branch@commit), the run, the scenario
    python tools/omkdebug.py shoot status|stop        the test scenario ([shoot] in docs/config.md)
    python tools/omkdebug.py shoot start [STEPS]      ... started: it waits until the camera shoots (cable out)
    python tools/omkdebug.py restart [--ptp] [--wait S]   restart the camera over USB (core's RESTART; --ptp: the
                                                      firmware's own 0x970A; by hand where neither) and wait for it
                                                      to come back with a new run
    python tools/omkdebug.py session [options]        a whole test session: files, scenario, restart, the
                                                      scenario's pictures and logs; --hands-free: with the
                                                      cable in throughout (session --help)

Exit status: 0 done, 1 the camera refused or failed, 2 bad use, 3 no camera.
"""
from __future__ import annotations

import argparse
import binascii
import secrets
import struct
import sys
import time
from pathlib import Path

OP_CMD, OP_GET, OP_PUT = 0x9E00, 0x9E01, 0x9E02
OP_FW_REBOOT = 0x970A                       # the firmware's own reboot request (core's RESTART posts the same event)
HELLO, STATUS, LEVEL, MARK, SNAP, READ, LIST, WBEGIN, WCOMMIT, WABORT, RESTART, SHOOT = (1, 2, 3, 4, 5, 0x10, 0x11, 0x20,
                                                                                   0x22, 0x23, 0x30, 0x31)
SHOOT_STATUS, SHOOT_START, SHOOT_STOP = 0, 1, 2
BANKS, BANK, MEM, HIST, XFER, INFO = 1, 2, 3, 4, 5, 6
PATH, TEXT, WDATA = 1, 2, 0x21
F_RESTART, F_SHOOT, F_INFO, F_HANDS_FREE = 64, 128, 256, 512
SHOOT_STATES = ["idle", "running", "done", "failed", "stopped"]
MAGIC = 0x444B4D4F
RESTART_KEY = 0x21544252
STAGE, XFER_SIZE = 16384, 65536
J_DONE, J_FAILED = 3, 4
FLAGS = {1: "ring", 2: "run-before", 4: "write", 8: "no-card-writes", 16: "usb-ops", 32: "buffers", 64: "restart",
         128: "shoot", 256: "info", 512: "hands-free"}
RESULTS = ["ok", "bad path", "cannot open", "card i/o", "size differs", "CRC differs", "rename failed", "wrong state",
           "timeout", "card", "restart not available", "a step not understood", "a scenario runs already"]
RESPONSES = {0x2001: "OK", 0x2002: "GeneralError", 0x2005: "OperationNotSupported", 0x200F: "AccessDenied (no write key)",
             0x2019: "DeviceBusy", 0x201D: "InvalidParameter"}
LEVELS = "-EWID"
TRIGGERS = {1: "card+", 2: "card-", 3: "window", 4: "camera busy", 5: "camera idle", 6: "stuck", 7: "mark", 8: "boot", 9: "job"}
STATES = {1: "RUN", 2: "RDY", 4: "WAI", 8: "SUS", 12: "WAS", 16: "DMT", 0xFF: "GONE"}
WAITS = ["-", "SLP", "DLY", "SEM", "FLG", "SDTQ", "RDTQ", "MBX", "MTX", "SMBF", "RMBF", "CAL", "ACP", "RDV", "MPF", "MPL", "W15"]


# The PROPVARIANT union's field (comtypes' generated name; a double underscore name is mangled inside a class)
PV_UNION = '__MIDL____MIDL_itf_PortableDeviceApi_0001_00000001'

class Refused(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------
# Formats (src/core/include/omk_dring.h, omk_debug.h)
HEAD_WORDS = ("magic", "version", "boot", "size", "tail", "head", "seq", "lost", "run0", "run1", "beat", "flags",
              "cause", "snap_ms", "crc", "spare")


def parse_header(b: bytes) -> dict:
    w = struct.unpack_from("<16I", b)
    h = dict(zip(HEAD_WORDS, w))
    h["run"] = struct.pack("<II", h["run0"], h["run1"]).rstrip(b"\0").decode("ascii", "replace")
    fixed = struct.pack("<6I", *w[0:4], h["run0"], h["run1"])
    h["valid"] = (h["magic"] == MAGIC and h["version"] == 1 and h["boot"] != 0 and h["size"] and
                  (h["head"] - h["tail"]) % 2**32 <= h["size"] and binascii.crc32(fixed) == h["crc"])
    h["ended"] = bool(h["flags"] & 1)
    return h


def parse_banks(b: bytes) -> dict:
    out = {"banks": [parse_header(b[0:64]), parse_header(b[64:128])]}
    out["area"], out["area_size"], out["cur"], out["prev"] = struct.unpack_from("<4I", b, 128)
    return out


def parse_records(data: bytes, start: int = 0):
    """Records {u8 0xA5, u8 type, u16 len, u32 seq, u32 ms} + data padded to 4, from data[0:] at ring position start.
    Stops at the first record that is not whole (a torn end). Yields (pos, type, seq, ms, payload)"""
    at, want = 0, None
    while at + 12 <= len(data):
        sync, typ, n, seq, ms = struct.unpack_from("<BBHII", data, at)
        if sync != 0xA5 or at + 12 + n > len(data) or (want is not None and seq != want):
            return
        yield start + at, typ, seq, ms, data[at + 12:at + 12 + n]
        want = seq + 1
        at += 12 + ((n + 3) & ~3)


def parse_map(p: bytes) -> dict:
    names = {}
    for i in range(0, len(p) - 23, 24):
        tid, cpu = struct.unpack_from("<HH", p, i)
        name = p[i + 4:i + 24].split(b"\0")[0].decode("ascii", "replace")
        if tid:
            names[(cpu, tid)] = name
    return names


def parse_snapshot(p: bytes) -> dict:
    trig, n, ui, app, keys, same, card, ms = struct.unpack_from("<HBBIIIII", p)
    tasks = []
    for i in range(n):
        tid, wobj, state, wait, pri, flags, tmo, r0, r1 = struct.unpack_from("<HHBBBBIII", p, 24 + 20 * i)
        tasks.append({"id": tid, "wobj": wobj, "state": state, "wait": wait, "pri": pri, "flags": flags, "tmo": tmo,
                      "r0": r0, "r1": r1})
    return {"trigger": TRIGGERS.get(trig, str(trig)), "app": app, "keys": keys, "ui": ui, "same": same,
            "camera_busy": bool(card & 1), "core_writing": bool(card & 2), "ms": ms, "tasks": tasks}


def parse_task_dump(b: bytes) -> dict:
    magic, ver, nt, no, ms, app, keys, run01, run2 = struct.unpack_from("<9I", b)
    if magic != 0x534B4D4F:
        raise ValueError("not a task dump")
    tasks, at = [], 36
    for _ in range(nt):
        f = struct.unpack_from("<6H4B8I6I", b, at)
        tasks.append({"id": f[0], "state": f[1], "wait": f[2], "wobj": f[3], "pri": f[4], "bpri": f[5], "cpu": f[6],
                      "wup": f[7], "sus": f[8], "flags": f[9], "tmo": f[10], "wptn": f[11], "wmode": f[12], "entry": f[13],
                      "stk": f[14], "stksz": f[15], "sp": f[16], "never": f[17], "ret": [r for r in f[18:24] if r],
                      "name": b[at + 72:at + 92].split(b"\0")[0].decode("ascii", "replace")})
        at += 92
    objs = []
    for _ in range(no):
        oid, kind, owner, atr, a, bb = struct.unpack_from("<4H2I", b, at)
        objs.append({"id": oid, "kind": kind, "owner": owner, "atr": atr, "a": a, "b": bb})
        at += 16
    return {"ms": ms, "app": app, "keys": keys, "running": [run01 & 0xFFFF, run01 >> 16, run2 & 0xFFFF], "tasks": tasks,
            "objects": objs}


def parse_list(b: bytes) -> list[dict]:
    out = []
    for i in range(0, len(b) - 63, 64):
        attr, size, made, changed = struct.unpack_from("<4I", b, i)
        out.append({"name": b[i + 16:i + 64].split(b"\0")[0].decode("ascii", "replace"), "dir": bool(attr & 0x10),
                    "size": size, "made": dos_time(made), "changed": dos_time(changed)})
    return out


def dos_time(w: int) -> str:
    """FAT date << 16 | time -> 'YYYY-MM-DD hh:mm:ss'"""
    d, t = w >> 16, w & 0xFFFF
    return f"{1980 + (d >> 9):04d}-{d >> 5 & 15:02d}-{d & 31:02d} {t >> 11:02d}:{t >> 5 & 63:02d}:{(t & 31) * 2:02d}"


def wait_name(bit: int) -> str:
    return WAITS[bit] if bit < len(WAITS) else f"W{bit}"


def format_record(typ: int, seq: int, ms: int, p: bytes, names: dict) -> str:
    if typ == 1:                            # a log line: its level byte, then the line as the log has it
        return p[1:].decode("ascii", "replace")
    if typ == 3:
        return f"{ms:8d} M {p.decode('ascii', 'replace')}"
    if typ == 4:
        names.update(parse_map(p))
        return f"{ms:8d} T task map: " + ", ".join(f"{n} {t:#06x}" for (_, t), n in sorted(parse_map(p).items()))
    if typ == 2:
        s = parse_snapshot(p)
        by_id = {t: n for (_, t), n in names.items()}
        parts = []
        for t in s["tasks"]:
            st = STATES.get(t["state"], str(t["state"]))
            w = f" {wait_name(t['wait'])} {t['wobj']:04x}" if t["state"] & 4 else ""
            parts.append(f"{by_id.get(t['id'], hex(t['id']))} {st}{w} p{t['pri']} r {t['r0']:08x}")
        flags = ("cam " if s["camera_busy"] else "") + ("core-writing " if s["core_writing"] else "")
        same = f" (+{s['same']} same)" if s["same"] else ""
        return f"{ms:8d} S {s['trigger']}{same}: app {s['app']:x} {flags}| " + "; ".join(parts)
    if typ == 5:
        return f"{ms:8d} B {p.hex()}"
    return f"{ms:8d} ? type {typ} {p.hex()}"


# ---------------------------------------------------------------------------------------------------------------
# WPD transport (Windows): MTP extension commands, as OM Workspace uses them
class Camera:
    def __init__(self, name_part: str = "OM"):
        import ctypes
        import comtypes
        import comtypes.client
        from comtypes import GUID

        comtypes.CoInitialize()
        comtypes.client.GetModule("portabledeviceapi.dll")
        comtypes.client.GetModule("portabledevicetypes.dll")
        from comtypes.gen import PortableDeviceApiLib as api
        from comtypes.gen import PortableDeviceTypesLib as types

        self.ct, self.cc, self.api, self.types = ctypes, comtypes.client, api, types
        self.MTP_EXT = GUID("{4D545058-1A2E-4106-A357-771E0819FC56}")
        self.COMMON = GUID("{F0422A9C-5DC8-4440-B5BD-5DF28835658A}")
        mgr = self.cc.CreateObject(api.PortableDeviceManager, interface=api.IPortableDeviceManager)
        get_devices = mgr._IPortableDeviceManager__com_GetDevices
        get_name = mgr._IPortableDeviceManager__com_GetDeviceFriendlyName
        n = ctypes.c_ulong(0)
        get_devices(None, ctypes.byref(n))
        ids = (ctypes.c_wchar_p * n.value)()
        if n.value:
            get_devices(ctypes.cast(ids, ctypes.POINTER(ctypes.c_wchar_p)), ctypes.byref(n))
        found = []
        for dev_id in ids[:n.value]:
            ln = ctypes.c_ulong(0)
            get_name(dev_id, None, ctypes.byref(ln))
            buf = ctypes.create_unicode_buffer(ln.value)
            get_name(dev_id, ctypes.cast(buf, ctypes.POINTER(ctypes.c_ushort)), ctypes.byref(ln))
            found.append((dev_id, buf.value))
        match = [(i, f) for i, f in found if name_part.lower() in f.lower()]
        if len(match) != 1:
            raise LookupError(f"need exactly one device with '{name_part}' in its name, found: {[f for _, f in found]}")
        self.dev_id, self.name = match[0]
        client = self._values()
        client.SetStringValue(self._key(GUID("{204D9F0C-2292-4080-9F42-40664E70F859}"), 2), "omkdebug")
        self.dev = self.cc.CreateObject(api.PortableDevice, interface=api.IPortableDevice)
        self.dev.Open(self.dev_id, client)

    def close(self):
        if self.dev is not None:
            self.dev.Close()
            self.dev = None

    def _key(self, fmtid, pid):
        k = self.api._tagpropertykey()
        k.fmtid, k.pid = fmtid, pid
        return k

    def _values(self):
        return self.cc.CreateObject(self.types.PortableDeviceValues, interface=self.api.IPortableDeviceValues)

    def _ulongs(self, coll) -> list[int]:
        out, n = [], self.ct.c_ulong(0)
        coll._IPortableDevicePropVariantCollection__com_GetCount(self.ct.byref(n))
        for i in range(n.value):
            pv = self.api.tag_inner_PROPVARIANT()
            coll._IPortableDevicePropVariantCollection__com_GetAt(i, self.ct.byref(pv))
            out.append(getattr(pv, PV_UNION).ulVal)
        return out

    def _command(self, cmd: int, opcode=None, params=(), extra=None):
        p = self._values()
        p.SetGuidValue(self._key(self.COMMON, 1001), self.MTP_EXT)
        p.SetUnsignedIntegerValue(self._key(self.COMMON, 1002), cmd)
        if opcode is not None:
            p.SetUnsignedIntegerValue(self._key(self.MTP_EXT, 1001), opcode)
            coll = self.cc.CreateObject(self.types.PortableDevicePropVariantCollection,
                                        interface=self.api.IPortableDevicePropVariantCollection)
            for v in params:
                pv = self.api.tag_inner_PROPVARIANT()
                pv.vt = 19                  # VT_UI4
                getattr(pv, PV_UNION).ulVal = v & 0xFFFFFFFF
                coll.Add(pv)
            p.SetIPortableDevicePropVariantCollectionValue(self._key(self.MTP_EXT, 1002), coll)
        for f in extra or ():
            f(p)
        r = self.dev.SendCommand(0, p)
        hr = r.GetErrorValue(self._key(self.COMMON, 1003))
        if hr != 0:
            raise Refused(f"WPD error {hr & 0xFFFFFFFF:#x} (operation {opcode if opcode is None else hex(opcode)})")
        return r

    def _resp(self, r):
        code = r.GetUnsignedIntegerValue(self._key(self.MTP_EXT, 1003))
        return code, self._ulongs(r.GetIPortableDevicePropVariantCollectionValue(self._key(self.MTP_EXT, 1004)))

    def cmd(self, *params):
        return self._resp(self._command(12, OP_CMD, params))

    def op(self, opcode: int, *params):
        """Any operation without data (the firmware's own, e.g. 0x970A)"""
        return self._resp(self._command(12, opcode, params))

    def get(self, *params):
        r = self._command(13, OP_GET, params)
        ctx = r.GetStringValue(self._key(self.MTP_EXT, 1006))
        total = r.GetUnsignedLargeIntegerValue(self._key(self.MTP_EXT, 1007))
        data = bytearray()
        while len(data) < total:
            want = min(total - len(data), 0x40000)

            def setup(p, want=want):
                p.SetStringValue(self._key(self.MTP_EXT, 1006), ctx)
                p.SetUnsignedLargeIntegerValue(self._key(self.MTP_EXT, 1008), want)
                buf = (self.ct.c_ubyte * want)()
                p.SetBufferValue(self._key(self.MTP_EXT, 1012), buf, want)
            rr = self._command(15, extra=[setup])
            got = rr.GetUnsignedLargeIntegerValue(self._key(self.MTP_EXT, 1009))
            ptr, size = rr.GetBufferValue(self._key(self.MTP_EXT, 1012))
            data += self.ct.string_at(ptr, min(got, size))
            self.ct.windll.ole32.CoTaskMemFree(ptr)
            if got == 0:
                break
        code, rp = self._resp(self._command(17, extra=[lambda p: p.SetStringValue(self._key(self.MTP_EXT, 1006), ctx)]))
        return code, rp, bytes(data)

    def put(self, data: bytes, *params):
        def total(p):
            p.SetUnsignedLargeIntegerValue(self._key(self.MTP_EXT, 1007), len(data))
        r = self._command(14, OP_PUT, params, extra=[total])
        ctx = r.GetStringValue(self._key(self.MTP_EXT, 1006))

        def chunk(p):
            p.SetStringValue(self._key(self.MTP_EXT, 1006), ctx)
            p.SetUnsignedLargeIntegerValue(self._key(self.MTP_EXT, 1010), len(data))
            buf = (self.ct.c_ubyte * len(data)).from_buffer_copy(data)
            p.SetBufferValue(self._key(self.MTP_EXT, 1012), buf, len(data))
        self._command(16, extra=[chunk])
        return self._resp(self._command(17, extra=[lambda p: p.SetStringValue(self._key(self.MTP_EXT, 1006), ctx)]))


# ---------------------------------------------------------------------------------------------------------------
def ok(code: int, what: str):
    if code != 0x2001:
        raise Refused(f"{what}: {code:#06x} {RESPONSES.get(code, '?')}")


def job(cam, *params, timeout: float = 30.0) -> list[int]:
    """A job of the core task: started, then STATUS until it is done"""
    code, _ = cam.cmd(*params)
    ok(code, f"job {params[0]:#x}")
    t0 = time.time()
    while True:
        code, st = cam.cmd(STATUS)
        ok(code, "STATUS")
        state = st[0] >> 8 & 0xFF
        if state in (J_DONE, J_FAILED):
            if state == J_FAILED:
                r = st[1]
                raise Refused(f"job {params[0]:#x} failed: {RESULTS[r] if r < len(RESULTS) else r}")
            return st
        if time.time() - t0 > timeout:
            raise Refused(f"job {params[0]:#x}: no answer in {timeout} s")
        time.sleep(0.02)


def set_path(cam, path: str):
    p = path.replace("/", "\\").strip("\\").upper().encode("ascii")
    code, _ = cam.put(p, PATH)
    ok(code, "PATH")


def fetch(cam, n: int) -> bytes:
    out = bytearray()
    while len(out) < n:
        piece = min(n - len(out), XFER_SIZE)
        code, _, data = cam.get(XFER, len(out), piece)
        ok(code, "XFER")
        out += data
    return bytes(out)


def hello(cam) -> list[int]:
    code, p = cam.cmd(HELLO)
    ok(code, "HELLO")
    if not p or p[0] != MAGIC:
        raise Refused("not the OMKit debug service")
    return p


def read_bank(cam, which: int, banks: dict, start=None) -> tuple[bytes, int, int]:
    """The bank's records from start (default: its tail) to its head; pieces of STAGE, overwritten ones skipped"""
    h = banks["banks"][banks["cur"] if which == 0 else banks["prev"]]
    pos, data, head = (h["tail"] if start is None else start), bytearray(), h["head"]
    first = pos
    while (head - pos) % 2**32 and (head - pos) % 2**32 <= h["size"]:
        n = min((head - pos) % 2**32, STAGE)
        n += n & 1
        code, rp, piece = cam.get(BANK, which, pos, n)
        ok(code, "BANK")
        tail, head = rp[1], rp[2]
        if (tail - first) % 2**32 > (pos - first) % 2**32 and (pos - first) % 2**32 < 2**31:
            data, pos, first = bytearray(), tail, tail      # overwritten while read: from the new tail
            continue
        data += piece
        pos = (pos + len(piece)) % 2**32
    return bytes(data), first, pos


def show_records(data: bytes, start: int, names: dict):
    last = start
    for pos, typ, seq, ms, p in parse_records(data, start):
        print(format_record(typ, seq, ms, p, names))
        last = pos + 12 + ((len(p) + 3) & ~3)
    return last


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("hello")
    sub.add_parser("info")
    sub.add_parser("banks")
    t = sub.add_parser("tail")
    t.add_argument("--follow", action="store_true")
    t.add_argument("--interval", type=float, default=0.5)
    sub.add_parser("postmortem")
    s = sub.add_parser("snap")
    s.add_argument("-o")
    m = sub.add_parser("mem")
    m.add_argument("addr")
    m.add_argument("length")
    m.add_argument("-o")
    sub.add_parser("hist")
    lv = sub.add_parser("level")
    lv.add_argument("n", type=int)
    mk = sub.add_parser("mark")
    mk.add_argument("tag", nargs="?", default="0")
    ls = sub.add_parser("ls")
    ls.add_argument("path")
    g = sub.add_parser("get")
    g.add_argument("path")
    g.add_argument("-o")
    pc = sub.add_parser("pictures")
    pc.add_argument("-n", type=int, default=1)
    pc.add_argument("-o", default=".")
    pt = sub.add_parser("put")
    pt.add_argument("file")
    pt.add_argument("path")
    sh = sub.add_parser("shoot")
    sh.add_argument("action", choices=["status", "start", "stop"])
    sh.add_argument("steps", nargs="?", default="", help="the steps (as [shoot] steps =); none: those of OMKIT.INI")
    rs = sub.add_parser("restart")
    rs.add_argument("--wait", type=float, default=180.0)
    rs.add_argument("--ptp", action="store_true", help="the firmware's own operation 0x970A, without core's checks")
    se = sub.add_parser("session", description=SESSION_HELP, formatter_class=argparse.RawDescriptionHelpFormatter)
    se.add_argument("--put", action="append", default=[], metavar="FILE[=CARDPATH]",
                    help="a file for the OMKit folder (default place: OMKIT\\NAME, a .OMM in OMKIT\\MODULES); repeatable")
    se.add_argument("--scenario", metavar="STEPS", help="the test scenario's steps, as [shoot] steps = (\"keep\": one "
                    "step without a change of preset)")
    se.add_argument("--shots", type=int, help="pictures per step (default: OMKIT.INI's [shoot] shots)")
    se.add_argument("--start-delay", type=int, metavar="MS", help="ms after the camera shoots before the first step")
    se.add_argument("--restart", action="store_true", help="restart the camera after the files (new CORE.BIN, modules "
                    "and OMKIT.INI run); the scenario then runs once from OMKIT\\SHOOT.RUN")
    se.add_argument("--wait", action="store_true", help="wait for the scenario to end (the cable must go out meanwhile)")
    se.add_argument("--hands-free", action="store_true", help="the scenario runs with the cable in, as charge only: the "
                    "camera restarts into it, restarts again at its end and comes back in the MTP mode (its USB mode set "
                    "to MTP); implies --restart and --wait")
    se.add_argument("--pull-log", action="store_true", help="the RAM log and OMKIT\\LOG\\CORE.LOG into the output folder")
    se.add_argument("--pull-pictures", type=int, default=0, metavar="N", help="the last N pictures into the output folder")
    se.add_argument("-o", "--out", default="session", help="the output folder (default: session)")
    se.add_argument("--connect-timeout", type=float, default=120.0, metavar="S")
    se.add_argument("--restart-timeout", type=float, default=180.0, metavar="S")
    se.add_argument("--scenario-timeout", type=float, default=1800.0, metavar="S")
    se.add_argument("--usb-off", metavar="CMD", help="a command that cuts the camera's USB port (a hub with port power "
                    "switching): the scenario then runs without hands")
    se.add_argument("--usb-on", metavar="CMD", help="the command that powers the port again")
    se.add_argument("--offline", type=float, metavar="S", help="how long the port stays off (default: estimated from "
                    "the scenario)")
    a = ap.parse_args(argv)
    if a.cmd == "session":
        return session(a, Camera)
    try:
        cam = Camera()
    except (ImportError, OSError, LookupError) as e:
        print("no camera:", e, file=sys.stderr)
        return 3
    try:
        return run(cam, a)
    except Refused as e:
        print("refused:", e, file=sys.stderr)
        return 1
    finally:
        cam.close()


def list_dir(cam, path: str) -> list[dict]:
    set_path(cam, path)
    out, first = [], 0
    while True:
        st = job(cam, LIST, first)
        out += parse_list(fetch(cam, st[2]))
        if not st[4] >> 31:
            return out
        first = st[4] & 0x7FFFFFFF


def read_file(cam, path: str) -> bytes:
    set_path(cam, path)
    data = bytearray()
    while True:
        st = job(cam, READ, len(data), XFER_SIZE)
        piece = fetch(cam, st[2])
        if binascii.crc32(piece) != st[3]:
            raise Refused(f"{path}: the piece at {len(data)} arrived damaged")
        data += piece
        if st[2] < XFER_SIZE:
            return bytes(data)


def put_file(cam, data: bytes, target: str) -> list[int]:
    """A file of the OMKit folder: written to a temporary file, checked by its CRC-32 read back, then renamed"""
    target = target.replace("/", "\\").strip("\\").upper()
    if not target.startswith("OMKIT\\"):
        raise Refused("put: only files of the OMKit folder (OMKIT\\...)")
    set_path(cam, target)
    job(cam, WBEGIN, len(data), binascii.crc32(data))
    try:
        for at in range(0, len(data), XFER_SIZE):
            code, _ = cam.put(data[at:at + XFER_SIZE], WDATA, at)
            ok(code, f"WDATA at {at}")
            job_wait(cam)
        return job(cam, WCOMMIT, timeout=120.0)
    except Refused:
        try:
            cam.cmd(WABORT)
        except Refused:
            pass
        raise


def parse_info(text: str) -> dict:
    """GET INFO: key=value lines"""
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def info(cam, p: list[int] | None = None) -> dict:
    """The build and the run: GET INFO, or what HELLO tells (a core without INFO: no build, the power-on as the run)"""
    p = p or hello(cam)
    if p[3] & F_INFO:
        code, _, data = cam.get(INFO)
        ok(code, "INFO")
        d = parse_info(data.decode("ascii", "replace"))
    else:
        d = {"core": f"{p[2] >> 16}.{p[2] & 0xFFFF:02d}", "build": "?", "run": f"power-on {p[4]}"}
    d["flags"], d["power-on"] = p[3], str(p[4])
    return d


def shoot_status(cam) -> dict:
    code, p = cam.cmd(SHOOT, SHOOT_STATUS)
    ok(code, "SHOOT status")
    st = p[0] & 0xFF
    return {"state": SHOOT_STATES[st] if st < len(SHOOT_STATES) else str(st), "waiting": bool(p[0] >> 8 & 1),
            "total": p[1], "taken": p[2], "failed": p[3], "step": p[4] & 0xFFFF, "steps": p[4] >> 16}


def shoot_text(s: dict) -> str:
    w = ", waiting for the camera to shoot" if s["waiting"] else ""
    step = f", step {s['step']} of {s['steps']}" if s["step"] else ""
    return f"{s['state']}{w}: {s['taken']} of {s['total']} pictures made, {s['failed']} not seen made{step}"


def shoot_start(cam, steps: str):
    code, _ = cam.put(steps.encode("ascii"), TEXT)
    ok(code, "TEXT")
    job(cam, SHOOT, SHOOT_START)


def build_of(data: bytes) -> str | None:
    """The build name a CORE.BIN carries (the text of its INFO: build=branch@commit)"""
    i = data.find(b"\nbuild=")
    if i < 0:
        return None
    end = data.find(b"\n", i + 7)
    return data[i + 7:end].decode("ascii", "replace") if end > 0 else None


def run(cam, a) -> int:
    p = hello(cam)
    flags = [n for b, n in FLAGS.items() if p[3] & b]
    if a.cmd == "hello":
        d = info(cam, p)
        print(f"{cam.name}: OMKit debug protocol {p[1]}, core {p[2] >> 16}.{p[2] & 0xFFFF:02d} ({d['build']}), "
              f"run {d['run']}, power-on {p[4]}, flags {' '.join(flags)}")
        return 0
    if a.cmd == "info":
        for k, v in info(cam, p).items():
            print(f"{k}: {v:#x}" if k == "flags" else f"{k}: {v}")
        return 0
    if a.cmd in ("banks", "tail", "postmortem"):
        code, _, raw = cam.get(BANKS)
        ok(code, "BANKS")
        banks = parse_banks(raw)
        if a.cmd == "banks":
            print(f"area {banks['area']:#010x}, {banks['area_size']} bytes; this power-on: bank {banks['cur']}, "
                  f"the one before: {banks['prev'] if banks['prev'] != 0xFF else '-'}")
            for i, h in enumerate(banks["banks"]):
                print(f"bank {i}: {'valid' if h['valid'] else 'not valid'}, power-on {h['boot']} run {h['run']}, "
                      f"{(h['head'] - h['tail']) % 2**32} bytes, records up to {h['seq']}, lost {h['lost']}, "
                      f"heartbeat {h['beat']} ms, {'ended with a power-off' if h['ended'] else 'did not end'}")
            return 0
        which = 1 if a.cmd == "postmortem" else 0
        if which and banks["prev"] == 0xFF:
            raise Refused("no RAM log of a power-on before (a power-off or the battery cleared the memory)")
        names: dict = {}
        data, start, end = read_bank(cam, which, banks)
        pos = show_records(data, start, names)
        while a.cmd == "tail" and a.follow:
            time.sleep(a.interval)
            code, _, raw = cam.get(BANKS)
            ok(code, "BANKS")
            data, start, end = read_bank(cam, 0, parse_banks(raw), pos)
            pos = show_records(data, start, names)
        return 0
    if a.cmd == "snap":
        st = job(cam, SNAP)
        blob = fetch(cam, st[2])
        if a.o:
            Path(a.o).write_bytes(blob)
        d = parse_task_dump(blob)
        print(f"{len(d['tasks'])} tasks at {d['ms']} ms, app {d['app']:x}, running {d['running']}")
        for t in d["tasks"]:
            w = f" {WAITS[(t['wait'] & -t['wait']).bit_length()]} {t['wobj']:04x}" if t["state"] & 4 and t["wait"] else ""
            print(f"c{t['cpu']} {t['id']:04x} {t['name']:<19} {STATES.get(t['state'], t['state'])}{w} p{t['pri']}/{t['bpri']} "
                  f"r {' '.join(f'{r:08x}' for r in t['ret'])}")
        for o in d["objects"]:
            print(f"obj {o['kind']:#x} {o['id']:04x} owner {o['owner']:04x} {o['a']} {o['b']}")
        return 0
    if a.cmd == "mem":
        addr, n = int(a.addr, 0), int(a.length, 0)
        out = bytearray()
        while len(out) < n:
            piece = min(n - len(out), STAGE)
            code, _, data = cam.get(MEM, addr + len(out), piece)
            ok(code, "MEM")
            out += data
        if a.o:
            Path(a.o).write_bytes(out)
        else:
            for i in range(0, len(out), 16):
                print(f"{addr + i:08x}  {out[i:i + 16].hex(' ')}")
        return 0
    if a.cmd == "hist":
        code, _, data = cam.get(HIST)
        ok(code, "HIST")
        if not data:
            print("the camera keeps no PTP history now (its pointer at 0xa2644f98 is 0)")
            return 0
        idx = struct.unpack_from("<I", data)[0] if len(data) >= 4 else 0
        print(f"index {idx}")
        for i in range(8, len(data) - 23, 24):
            c, resp, *ps = struct.unpack_from("<HH5I", data, i)
            if c:
                print(f"{(i - 8) // 24:2d} op {c:#06x} resp {resp:#06x} " + " ".join(f"{x:#x}" for x in ps))
        return 0
    if a.cmd == "level":
        code, p = cam.cmd(LEVEL, a.n)
        ok(code, "LEVEL")
        print(f"RAM log level {a.n} (was {p[0]})")
        return 0
    if a.cmd == "mark":
        code, _ = cam.cmd(MARK, int(a.tag, 0))
        ok(code, "MARK")
        return 0
    if a.cmd == "ls":
        for e in list_dir(cam, a.path):
            print(f"{'<dir>' if e['dir'] else e['size']:>10}  {e['changed']}  {e['name']}")
        return 0
    if a.cmd == "get":
        data = read_file(cam, a.path)
        out = Path(a.o or a.path.replace("\\", "/").split("/")[-1])
        out.write_bytes(data)
        print(f"{out}: {len(data)} bytes")
        return 0
    if a.cmd == "pictures":
        for out, n in pull_pictures(cam, a.n, Path(a.o)):
            print(f"{out}: {n} bytes")
        return 0
    if a.cmd == "put":
        target = a.path.replace("/", "\\").strip("\\").upper()
        if not target.startswith("OMKIT\\"):
            print("put: only files of the OMKit folder (OMKIT\\...)", file=sys.stderr)
            return 2
        st = put_file(cam, Path(a.file).read_bytes(), target)
        print(f"{target}: {st[2]} bytes written and checked, CRC-32 {st[3]:08x}")
        return 0
    if a.cmd == "shoot":
        if not p[3] & F_SHOOT:
            raise Refused("this core has no test scenario over USB")
        if a.action == "start":
            shoot_start(cam, a.steps)
            print("started: it waits until the camera shoots - take the USB cable out (or choose charge only in the "
                  "camera's USB dialog); plug it back in when it is done")
        elif a.action == "stop":
            job(cam, SHOOT, SHOOT_STOP)
        print(shoot_text(shoot_status(cam)))
        return 0
    if a.cmd == "restart":
        ses = Session(Camera, print)
        ses.cam = cam
        done = ses.restart(info(cam, p), a.wait, ptp=a.ptp)
        cam.dev = None                      # closed by the session, or gone with the restart
        ses.drop()
        return 0 if done else 1
    return 2


DCF_SUFFIXES = ("OMSYS", "OLYMP")          # the DCF folder names of OM SYSTEM and Olympus cameras: NNNOMSYS, NNNOLYMP
DCF_GAP = 5                                 # numbers in a row without pictures that end the search for folders


def dcim_folders(cam) -> list[str]:
    """The DCF folders of DCIM, oldest first. The listing of core (the firmware's findfirst with the mask *.*) leaves out
    names without a dot - the folders (camera, card155) - so DCIM lists empty: then the folders are looked for by name,
    NNN + OMSYS or OLYMP from 100 up, until DCF_GAP numbers in a row have no files (an empty folder looks the same)"""
    found = sorted(e["name"] for e in list_dir(cam, "DCIM") if e["dir"] and e["name"][:1].isdigit())
    if found:
        return found
    gap, n = 0, 100
    while n <= 999 and gap < DCF_GAP:
        hit = False
        for suffix in DCF_SUFFIXES:
            if list_dir(cam, f"DCIM\\{n}{suffix}"):
                found.append(f"{n}{suffix}")
                hit = True
        gap = 0 if hit else gap + 1
        n += 1
    return found


def pull_pictures(cam, n: int, folder: Path) -> list[tuple[Path, int]]:
    """The last n pictures of DCIM (JPEG and ORF of each): by folder, then by the time written, then by name. Fewer than
    n there - Refused (a session must not end well without them)"""
    pics: list[tuple[str, str, str]] = []           # (folder, changed, name)
    stems: list[tuple[str, str]] = []
    for f in reversed(dcim_folders(cam)):
        here = [(f, e["changed"], e["name"]) for e in list_dir(cam, "DCIM\\" + f)
                if not e["dir"] and e["name"].upper().endswith((".JPG", ".ORF"))]
        pics = sorted(here) + pics
        last: dict = {}                             # a picture (folder, stem): its latest file
        for x in pics:
            k = (x[0], Path(x[2]).stem)
            last[k] = max(last.get(k, x), x)
        stems = sorted(last, key=last.get)
        if len(stems) >= n:
            break
    if len(stems) < n:
        raise Refused(f"pictures: {len(stems)} found in DCIM, {n} asked for")
    want = set(stems[-n:]) if n > 0 else set()
    out = []
    folder.mkdir(parents=True, exist_ok=True)
    for f, _, name in pics:
        if (f, Path(name).stem) in want:
            data = read_file(cam, f"DCIM\\{f}\\{name}")
            path = folder / name
            path.write_bytes(data)
            out.append((path, len(data)))
    return out


def job_wait(cam, timeout: float = 30.0):
    t0 = time.time()
    while True:
        code, st = cam.cmd(STATUS)
        ok(code, "STATUS")
        state = st[0] >> 8 & 0xFF
        if state == J_FAILED:
            raise Refused(f"write failed: {RESULTS[st[1]] if st[1] < len(RESULTS) else st[1]}")
        if state == J_DONE:
            return st
        if time.time() - t0 > timeout:
            raise Refused("no answer")
        time.sleep(0.02)


# ---------------------------------------------------------------------------------------------------------------
# A test session
SESSION_HELP = """A whole test session, step by step, each with its checks and time limit:

  1. connect: wait for the camera in the MTP mode (choose MTP in its USB dialog), HELLO and INFO;
  2. --put: the files of the OMKit folder, each checked by its CRC-32 read back from the card;
  3. --scenario: with --restart it goes to OMKIT\\SHOOT.RUN and runs once at the next start; without, it is started
     now over USB. Either way it waits until the camera shoots: in the MTP mode it does not;
  4. --restart: the camera restarts (over USB where core can, else by hand: the tool says what to do) and comes back
     with a new run; when a CORE.BIN was put, its build must be the one that runs now;
  5. --wait: the scenario runs once the cable is out (by hand, or --usb-off / --usb-on with a hub that switches its
     ports' power); the tool waits for the camera to come back and for the scenario's end;
  6. --pull-log, --pull-pictures N: the RAM logs of this power-on and of the one before, CORE.LOG and the last N
     pictures into the output folder.

With --hands-free, steps 3 to 5 need no hands: the scenario goes to SHOOT.RUN with hands_free = on; the camera restarts,
keeps the cable as charge only (no data link, so it shoots), runs the scenario, writes its result to OMKIT\\SHOOT.END,
restarts once more and comes back in the MTP mode. The tool waits up to --restart-timeout + --scenario-timeout for it,
then tells the result and why. Set the camera's USB mode to MTP for it (no dialog to answer).

The camera comes back in the MTP mode by itself only when its USB mode is set to MTP; otherwise choose MTP in its USB
dialog each time it asks. Exit status: 0 done, 1 a step failed, 2 bad use, 3 no camera.

  python tools/omkdebug.py session --put build/CORE.BIN --put OMKIT.INI --scenario "look:Kodak Portra 400, stock"
      --restart --wait --pull-log --pull-pictures 2 -o run1
  python tools/omkdebug.py session --scenario keep --shots 3 --hands-free --pull-log --pull-pictures 3 -o run2
"""


class StepFailed(Exception):
    pass


RESULT_FILE = "OMKIT\\SHOOT.END"          # core's result of a scenario without hands (docs/config.md, 10.2)
HANDS_FREE_ARM_WAIT = 120000                # ms: as charge only the camera shoots within seconds of its start
MUTE_S = 15.0                               # s: on USB without the debug service this long - core does not run (MTP
                                            # comes up within a second or two after core's operations at a full start)


def default_target(spec: str) -> tuple[Path, str]:
    """FILE[=CARDPATH]: the card path, by default OMKIT\\NAME (a module: OMKIT\\MODULES\\NAME)"""
    if "=" in spec:
        f, t = spec.split("=", 1)
        return Path(f), t.replace("/", "\\").strip("\\").upper()
    f = Path(spec)
    name = f.name.upper()
    return f, ("OMKIT\\MODULES\\" if name.endswith(".OMM") else "OMKIT\\") + name


def shoot_run_text(steps: str, shots: int | None, start_delay: int | None, session_id: str | None = None) -> bytes:
    """OMKIT\\SHOOT.RUN: [shoot] keys over those of OMKIT.INI, for one start; with session_id - a run without hands
    (the cable in as charge only, a restart at the end, the result in OMKIT\\SHOOT.END under that name)"""
    lines = ["; one test scenario, written by omkdebug.py session; core removes this file before it runs", "[shoot]",
             f"steps = {steps}"]
    if shots is not None:
        lines.append(f"shots = {shots}")
    if start_delay is not None:
        lines.append(f"start_delay = {start_delay}")
    if session_id is not None:
        lines += ["hands_free = on", f"session = {session_id}", f"arm_wait = {HANDS_FREE_ARM_WAIT}"]
    return ("\r\n".join(lines) + "\r\n").encode("ascii")


def parse_result(data: bytes) -> dict:
    """OMKIT\\SHOOT.END: key=value lines, padded with spaces to a fixed length"""
    return {k.strip(): v.strip() for k, v in parse_info(data.decode("ascii", "replace")).items()}


def result_text(r: dict) -> str:
    return (f"{r.get('result', '?')}: {r.get('taken', '?')} of {r.get('total', '?')} pictures made, "
            f"{r.get('failed', '?')} not seen made" + (f" ({r['why']})" if r.get("why") else ""))


def scenario_seconds(steps: str, shots: int | None) -> float:
    """A generous guess of a scenario's length with the defaults of [shoot]: how long the USB port stays off"""
    n = max(1, len([x for x in steps.split(",") if x.strip()])) * (shots or 1)
    return 60.0 + n * 30.0


class Session:
    """The steps of a session against camera objects made by connect() (Camera, or a test's fake)"""

    def __init__(self, connect, say, clock=time.monotonic, sleep=time.sleep, shell=None, session_id=None):
        self.connect, self.say, self.clock, self.sleep = connect, say, clock, sleep
        self.shell = shell or (lambda cmd: __import__("subprocess").run(cmd, shell=True).returncode)
        self.cam = None
        self.after: dict = {}
        self.session_id = session_id or secrets.token_hex(4).upper()
        self.mute = False

    def drop(self):
        if self.cam is not None:
            try:
                self.cam.close()
            except Exception:               # the device may be gone already
                pass
        self.cam = None

    def try_connect(self):
        """A camera with the debug service, or None; self.mute: the camera is on USB but the service does not answer"""
        self.mute = False
        try:
            cam = self.connect()
        except Exception:                   # no device, another device, or a COM error of one going away
            return None
        try:
            hello(cam)
            return cam
        except Exception:
            self.mute = True
            try:
                cam.close()
            except Exception:
                pass
            return None

    def wait_camera(self, timeout: float, what: str, hint: str | None = None, other_than: str | None = None) -> dict:
        """Until a camera answers (with other_than: in another run than that one); its INFO. A camera on USB whose debug
        service does not answer for MUTE_S in a row: core does not run - the camera came on from charging by the cable
        without a full start (camera, card163) - and the tool says how to start it"""
        t0, told, mute_since, mute_told = self.clock(), False, None, False
        self.drop()
        while self.clock() - t0 < timeout:
            cam = self.try_connect()
            if cam is not None:
                try:
                    d = info(cam)
                except Exception:
                    d = None
                if d is not None and (other_than is None or d["run"] != other_than):
                    self.cam = cam
                    return d
                cam.close()
            if not self.mute:
                mute_since = None
            elif mute_since is None:
                mute_since = self.clock()
            elif not mute_told and self.clock() - mute_since >= MUTE_S:
                self.say(f"{what}: the camera is on USB but OMKit does not answer - core does not run (the camera came on "
                         "from charging by the cable, without a full start?): switch it off with the lever and on again, "
                         "choose MTP if it asks")
                mute_told = True
            if hint and not told and self.clock() - t0 > 10 and mute_since is None:   # no camera at all
                self.say(hint)
                told = True
            self.sleep(2.0)
        raise StepFailed(f"{what}: no camera in {timeout:.0f} s" + (" (one was on USB, but OMKit did not answer: switch "
                                                                   "it off with the lever and on again)" if mute_told
                                                                   else ""))

    def wait_gone(self, timeout: float) -> bool:
        """Until the camera no longer answers"""
        t0 = self.clock()
        while self.clock() - t0 < timeout:
            try:
                hello(self.cam)
            except Exception:
                self.drop()
                return True
            self.sleep(1.0)
        return False

    def restart(self, before: dict, timeout: float, ptp: bool = False) -> bool:
        """core's RESTART (the firmware's reboot request after core's checks), the firmware's 0x970A, or by hand"""
        run_before = before["run"]
        hint = ("restart: no camera yet - if it shows its USB dialog, choose MTP; if it stays off, switch it on with "
                "the lever")
        if ptp:
            code, _ = self.cam.op(OP_FW_REBOOT)
            ok(code, "0x970A")
            self.say("restart: the firmware's reboot request (0x970A) accepted; waiting for the camera to come back")
        elif before["flags"] & F_RESTART:
            st = job(self.cam, RESTART, RESTART_KEY, timeout=30.0)
            self.say(f"restart: accepted by core, the request follows in {st[4]} ms; waiting for the camera to come back")
        else:
            self.say("restart: this core cannot restart the camera - switch it off with the lever, take the cable and "
                     "the battery out (with the cable in, the lever only charges: no new start), put them back, switch "
                     "it on and choose MTP in its USB dialog")
            hint = None
        if not self.wait_gone(30.0 if hint else timeout):
            self.say("restart: the camera stayed on USB - the request was not taken (see the RAM log: tail)")
            return False
        try:
            d = self.wait_camera(timeout, "restart", hint, other_than=run_before)
        except StepFailed as e:
            self.say(str(e))
            return False
        self.say(f"restart: back, run {d['run']} (was {run_before}), core {d['core']} ({d['build']})")
        self.after = d
        return True

    def hands_free(self, a, before: dict) -> dict:
        """core's RESTART; the camera leaves USB, starts again and runs the scenario of SHOOT.RUN with the cable in as
        charge only (off the computer), restarts at its end and comes back in the MTP mode (its USB mode set to MTP).
        Its result: OMKIT\\SHOOT.END of this session"""
        run_a = before["run"]
        st = job(self.cam, RESTART, RESTART_KEY, timeout=30.0)
        self.say(f"hands-free: restart accepted by core, the request follows in {st[4]} ms")
        if not self.wait_gone(30.0):
            raise StepFailed("hands-free: the camera stayed on USB - the restart request was not taken (see the RAM "
                             "log: tail)")
        limit = a.restart_timeout + a.scenario_timeout
        self.say(f"hands-free: the camera starts again and runs the scenario with the cable in, charging only, off the "
                 f"computer; at its end it restarts once more and comes back in the MTP mode; waiting up to {limit:.0f} s")
        try:
            d = self.wait_camera(limit, "hands-free", other_than=run_a)
        except StepFailed:
            raise StepFailed(f"hands-free: no camera in {limit:.0f} s - it hangs, stays off or stays in charge only: take "
                             "the battery out and put it back, switch the camera on (choose MTP if it asks), then pull the "
                             "log - the run of the scenario is the one before (postmortem, CORE.LOG)") from None
        self.after = d
        self.say(f"hands-free: back, run {d['run']} (was {run_a}), core {d['core']} ({d['build']})")
        try:
            res = parse_result(read_file(self.cam, RESULT_FILE))
        except Refused:
            res = {}
        if res.get("session") == self.session_id:
            return res
        s = shoot_status(self.cam)
        if s["state"] == "running":
            try:
                job(self.cam, SHOOT, SHOOT_STOP)
            except Refused:
                pass
            raise StepFailed("hands-free: the camera came up connected to the computer with the scenario waiting - USB "
                             "was not held as charge only (see the RAM log); the scenario is stopped")
        raise StepFailed(f"hands-free: no result of this session in {RESULT_FILE} (the scenario here: {s['state']}; a "
                         "core without hands-free?)")

    def wait_scenario(self, a, run: str) -> dict:
        s = shoot_status(self.cam)
        self.say("scenario: " + shoot_text(s))
        if s["state"] == "running" and s["waiting"]:
            if a.usb_off:
                off = a.offline or scenario_seconds(a.scenario or "keep", a.shots)
                self.say(f"scenario: the USB port off for {off:.0f} s")
                self.drop()
                if self.shell(a.usb_off):
                    raise StepFailed("scenario: the --usb-off command failed")
                self.sleep(off)
                if self.shell(a.usb_on):
                    raise StepFailed("scenario: the --usb-on command failed")
            else:
                self.say("scenario: take the USB cable out now - the camera shoots only without it (or choose charge "
                         "only in its USB dialog); plug it back in once it has made the pictures")
                if not self.wait_gone(600.0):
                    raise StepFailed("scenario: the cable was not taken out in 10 minutes")
        t0 = self.clock()
        while True:
            if self.cam is None:
                d = self.wait_camera(a.scenario_timeout, "scenario",
                                     "scenario: waiting for the camera (plug the cable in; choose MTP if it asks)")
                if d["run"] != run:
                    raise StepFailed(f"scenario: the camera came back with another run ({d['run']}, was {run}): "
                                     "it restarted, the scenario is lost")
            try:
                s = shoot_status(self.cam)
            except Exception:
                self.drop()
                continue
            if s["state"] != "running":
                self.say("scenario: " + shoot_text(s))
                return s
            if self.clock() - t0 > a.scenario_timeout:
                raise StepFailed("scenario: not finished in time: " + shoot_text(s))
            self.sleep(2.0)

    def pull_log(self, out: Path):
        """The RAM logs of this power-on (ramlog.txt) and of the one before (ramlog_prev.txt), and CORE.LOG"""
        code, _, raw = self.cam.get(BANKS)
        ok(code, "BANKS")
        banks = parse_banks(raw)
        counts = []
        for which, name in ((0, "ramlog"), (1, "ramlog_prev")):
            if which and banks["prev"] == 0xFF:
                counts.append("no RAM log of a power-on before")
                continue
            h = banks["banks"][banks["cur"] if which == 0 else banks["prev"]]
            data, start, _ = read_bank(self.cam, which, banks)
            names: dict = {}
            lines = [f"# power-on {h['boot']} run {h['run']}, "
                     f"{'ended with a power-off' if h['ended'] else 'did NOT end with a power-off'}"]
            lines += [format_record(typ, seq, ms, p, names) for _, typ, seq, ms, p in parse_records(data, start)]
            (out / f"{name}.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
            counts.append(f"{name}.txt {len(lines) - 1} records (run {h['run']})")
        log = read_file(self.cam, "OMKIT\\LOG\\CORE.LOG")
        (out / "CORE.LOG").write_bytes(log)
        self.say(f"log: {', '.join(counts)}, CORE.LOG {len(log)} bytes")

    def run(self, a) -> int:
        out = Path(a.out)
        done: list[str] = []

        def step(name):
            done.append(name)
            self.say(f"--- {name}")

        hands = getattr(a, "hands_free", False)
        failed = None                       # hands-free: the scenario's failure, told after the logs are pulled
        try:
            step("connect")
            before = self.wait_camera(a.connect_timeout, "connect", "connect: no camera yet - connect it and choose "
                                      "MTP in its USB dialog")
            self.say(f"connect: core {before['core']} ({before['build']}), run {before['run']}")
            if (a.put or (a.scenario is not None and a.restart)) and not before["flags"] & 4:
                raise StepFailed("connect: [debug] write = on is needed to write files")
            if a.scenario is not None and not a.restart and not before["flags"] & F_SHOOT:
                raise StepFailed("connect: this core has no test scenario over USB")
            if hands and not before["flags"] & F_RESTART:
                raise StepFailed("connect: this core cannot restart the camera over USB")
            if hands and not before["flags"] & F_HANDS_FREE and not any(default_target(x)[1] == "OMKIT\\CORE.BIN"
                                                                       for x in a.put):
                raise StepFailed("connect: this core cannot run a scenario without hands (put a CORE.BIN that can)")
            expect = None
            for spec in a.put:
                f, target = default_target(spec)
                step(f"put {f} -> {target}")
                data = f.read_bytes()
                st = put_file(self.cam, data, target)
                if st[2] != len(data) or st[3] != binascii.crc32(data):
                    raise StepFailed(f"put: {target} reported {st[2]} bytes, CRC-32 {st[3]:08x}")
                self.say(f"put: {target}: {len(data)} bytes, CRC-32 {st[3]:08x} checked on the card")
                if target == "OMKIT\\CORE.BIN":
                    expect = build_of(data)
            run = before["run"]
            if a.scenario is not None:
                step("scenario")
                if a.restart:
                    put_file(self.cam, shoot_run_text(a.scenario, a.shots, a.start_delay,
                                                      self.session_id if hands else None), "OMKIT\\SHOOT.RUN")
                    self.say("scenario: OMKIT\\SHOOT.RUN written, it runs once at the next start" +
                             (f" without hands (session {self.session_id})" if hands else ""))
                else:
                    if a.shots is not None or a.start_delay is not None:
                        self.say("scenario: --shots and --start-delay need --restart (SHOOT.RUN); OMKIT.INI's are used")
                    shoot_start(self.cam, a.scenario)
                    self.say("scenario: started")
            if hands:
                step("hands-free run")
                res = self.hands_free(a, before)
                self.say("scenario: " + result_text(res) + f", run {res.get('run', '?')}")
                if res.get("run") == self.after["run"]:
                    self.say("hands-free: the camera did not restart at the end (core kept it on: see its log)")
                if expect and res.get("build") != expect:
                    raise StepFailed(f"hands-free: the scenario ran on {res.get('build')}, the CORE.BIN put is {expect}")
                if res.get("result") != "done":
                    failed = "scenario: " + result_text(res)
            elif a.restart:
                step("restart")
                if not self.restart(before, a.restart_timeout):
                    raise StepFailed("restart: the camera did not come back")
                run = self.after["run"]
                if expect and self.after["build"] != expect:
                    raise StepFailed(f"restart: the camera runs {self.after['build']}, the CORE.BIN put is {expect}")
            if a.scenario is not None and a.wait and not hands:
                step("wait for the scenario")
                s = self.wait_scenario(a, run)
                if s["state"] != "done":
                    raise StepFailed("scenario: " + shoot_text(s))
            if a.pull_log or a.pull_pictures:
                out.mkdir(parents=True, exist_ok=True)
            if a.pull_log:
                step("pull the log")
                self.pull_log(out)
            if a.pull_pictures:
                step("pull the pictures")
                for path, n in pull_pictures(self.cam, a.pull_pictures, out):
                    self.say(f"pictures: {path}: {n} bytes")
            if failed:
                done.append("hands-free run")
                raise StepFailed(failed)
            self.say("session: done (" + ", ".join(done) + ")")
            return 0
        except (StepFailed, Refused, OSError) as e:
            self.say(f"session: FAILED at '{done[-1] if done else '?'}': {e}")
            return 1
        finally:
            self.drop()


def session(a, connect) -> int:
    if a.usb_off and not a.usb_on:
        print("session: --usb-off needs --usb-on", file=sys.stderr)
        return 2
    if a.hands_free:
        if a.scenario is None or a.usb_off:
            print("session: --hands-free needs --scenario and goes without --usb-off", file=sys.stderr)
            return 2
        a.restart = a.wait = True
    if not (a.put or a.scenario is not None or a.restart or a.pull_log or a.pull_pictures):
        print("session: nothing to do (session --help)", file=sys.stderr)
        return 2
    if connect is Camera:
        try:
            __import__("comtypes")
        except ImportError as e:
            print("no camera:", e, file=sys.stderr)
            return 3
    return Session(connect, lambda m: print(m, flush=True)).run(a)


if __name__ == "__main__":
    sys.exit(main())
