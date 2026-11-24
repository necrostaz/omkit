"""OMKit DEBUG over USB: read the camera's RAM log and state, read files of the card, update OMKit files.

The camera must run OMKit core 1.40 or later with `[debug] enabled = on` in OMKIT.INI (docs/debug.md) and be connected
by USB in the MTP mode (choose MTP in the camera's USB dialog). The tool talks to it through Windows Portable Devices
(WPD) - the camera's own driver, no driver change; it sends only the vendor operations 0x9E00-0x9E02 of core's debug
service. Windows only; needs the comtypes package (`pip install comtypes`). The parsing functions are plain Python and
run anywhere (tests/test_omkdebug.py).

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
    python tools/omkdebug.py restart [--wait S]       the restart: not available yet - asks for a manual one and
                                                      waits for the camera to come back

Exit status: 0 done, 1 the camera refused or failed, 2 bad use, 3 no camera.
"""
from __future__ import annotations

import argparse
import binascii
import struct
import sys
import time
from pathlib import Path

OP_CMD, OP_GET, OP_PUT = 0x9E00, 0x9E01, 0x9E02
HELLO, STATUS, LEVEL, MARK, SNAP, READ, LIST, WBEGIN, WCOMMIT, WABORT, RESTART = 1, 2, 3, 4, 5, 0x10, 0x11, 0x20, 0x22, 0x23, 0x30
BANKS, BANK, MEM, HIST, XFER = 1, 2, 3, 4, 5
PATH, WDATA = 1, 0x21
MAGIC = 0x444B4D4F
RESTART_KEY = 0x21544252
STAGE, XFER_SIZE = 16384, 65536
J_DONE, J_FAILED = 3, 4
FLAGS = {1: "ring", 2: "run-before", 4: "write", 8: "no-card-writes", 16: "usb-ops", 32: "buffers", 64: "restart"}
RESULTS = ["ok", "bad path", "cannot open", "card i/o", "size differs", "CRC differs", "rename failed", "wrong state",
           "timeout", "card", "restart not available"]
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
    rs = sub.add_parser("restart")
    rs.add_argument("--wait", type=float, default=120.0)
    a = ap.parse_args(argv)
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


def run(cam, a) -> int:
    p = hello(cam)
    flags = [n for b, n in FLAGS.items() if p[3] & b]
    if a.cmd == "hello":
        print(f"{cam.name}: OMKit debug protocol {p[1]}, core {p[2] >> 16}.{p[2] & 0xFFFF:02d}, power-on {p[4]}, "
              f"flags {' '.join(flags)}")
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
        folders = sorted(e["name"] for e in list_dir(cam, "DCIM") if e["dir"] and e["name"][:1].isdigit())
        files = []
        for f in reversed(folders):
            names = sorted(e["name"] for e in list_dir(cam, "DCIM\\" + f) if not e["dir"])
            files = [f"DCIM\\{f}\\{n}" for n in names if n.upper().endswith((".JPG", ".ORF"))] + files
            stems = {Path(x).stem for x in files}
            if len(stems) >= a.n:
                break
        stems = sorted({Path(x.replace("\\", "/")).stem for x in files})[-a.n:]
        for x in files:
            if Path(x.replace("\\", "/")).stem in stems:
                data = read_file(cam, x)
                out = Path(a.o) / x.split("\\")[-1]
                out.write_bytes(data)
                print(f"{out}: {len(data)} bytes")
        return 0
    if a.cmd == "put":
        data = Path(a.file).read_bytes()
        target = a.path.replace("/", "\\").strip("\\").upper()
        if not target.startswith("OMKIT\\"):
            print("put: only files of the OMKit folder (OMKIT\\...)", file=sys.stderr)
            return 2
        set_path(cam, target)
        job(cam, WBEGIN, len(data), binascii.crc32(data))
        try:
            for at in range(0, len(data), XFER_SIZE):
                piece = data[at:at + XFER_SIZE]
                code, _ = cam.put(piece, WDATA, at)
                ok(code, f"WDATA at {at}")
                job_wait(cam)
            st = job(cam, WCOMMIT, timeout=120.0)
        except Refused:
            try:
                cam.cmd(WABORT)
            except Refused:
                pass
            raise
        print(f"{target}: {st[2]} bytes written and checked, CRC-32 {st[3]:08x}")
        return 0
    if a.cmd == "restart":
        print("restart over USB is not available in this core: switch the camera off and on with its lever "
              "(waiting for it to come back)", file=sys.stderr)
        cam.close()
        t0 = time.time()
        while time.time() - t0 < a.wait:
            time.sleep(2)
            try:
                again = Camera()
            except (OSError, LookupError):
                continue
            try:
                p2 = hello(again)
                if p2[4] != p[4]:
                    print(f"back: power-on {p2[4]}")
                    cam.dev = again.dev
                    return 0
            except Refused:
                pass
            again.close()
        print("the camera did not come back", file=sys.stderr)
        cam.dev = None
        return 1
    return 2


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


if __name__ == "__main__":
    sys.exit(main())
