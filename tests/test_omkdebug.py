"""tools/omkdebug.py: the parsing of what the camera's debug service sends (the formats of src/core/include/omk_dring.h
and omk_debug.h), without a camera."""
import binascii
import struct
import sys
import unittest
from pathlib import Path

OMKIT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(OMKIT / "tools"))
import omkdebug  # noqa: E402


def header(boot, tail, head, seq, run=b"RUN00001", flags=0, size=65536):
    r0, r1 = struct.unpack("<II", run.ljust(8, b"\0"))
    w = [omkdebug.MAGIC, 1, boot, size, tail, head, seq, 0, r0, r1, 1234, flags, 0, 0, 0, 0]
    w[14] = binascii.crc32(struct.pack("<6I", *w[0:4], r0, r1))
    return struct.pack("<16I", *w)


def record(typ, seq, ms, payload):
    b = struct.pack("<BBHII", 0xA5, typ, len(payload), seq, ms) + payload
    return b + bytes(-len(b) % 4)


class Formats(unittest.TestCase):
    def test_banks(self):
        raw = header(2, 0, 100, 5) + header(1, 0, 0, 0, flags=1) + struct.pack("<4I", 0xA375FF00, 0x20080, 0, 1)
        b = omkdebug.parse_banks(raw)
        self.assertTrue(b["banks"][0]["valid"])
        self.assertEqual(b["banks"][0]["run"], "RUN00001")
        self.assertTrue(b["banks"][1]["ended"])
        self.assertEqual((b["cur"], b["prev"], b["area"]), (0, 1, 0xA375FF00))
        bad = bytearray(raw)
        bad[12] ^= 4                        # the size: the fixed part's CRC no longer matches
        self.assertFalse(omkdebug.parse_banks(bytes(bad))["banks"][0]["valid"])

    def test_records_and_torn_end(self):
        data = record(1, 7, 10, b"\x03     10 I hello") + record(3, 8, 11, b"card+ C:\OMKIT\LOG\CORE.LOG")
        torn = record(1, 9, 12, b"\x03 never finished")[:-6]
        recs = list(omkdebug.parse_records(data + torn, 4096))
        self.assertEqual([r[2] for r in recs], [7, 8])
        self.assertEqual(recs[1][0], 4096 + len(record(1, 7, 10, b"\x03     10 I hello")))
        self.assertEqual(omkdebug.format_record(*recs[0][1:], {}), "     10 I hello")
        gap = data + record(1, 11, 13, b"\x03 out of order")
        self.assertEqual(len(list(omkdebug.parse_records(gap))), 2)

    def test_snapshot_with_names(self):
        names = {}
        m = struct.pack("<HH", 0x51, 0) + b"FILECLBK".ljust(20, b"\0") + struct.pack("<HH", 0x9F, 0) + b"OMK_LOAD".ljust(20, b"\0")
        self.assertIn("FILECLBK 0x0051", omkdebug.format_record(4, 1, 5, m, names))
        snap = struct.pack("<HBBIIIII", 2, 2, 0, 0x5101, 0x14, 3, 1, 900)
        snap += struct.pack("<HHBBBBIII", 0x51, 0x30, 4, 6, 55, 0, 0, 0x606b9100, 0)
        snap += struct.pack("<HHBBBBIII", 0x9F, 0, 1, 0, 66, 0, 0, 0, 0)
        s = omkdebug.parse_snapshot(snap)
        self.assertEqual((s["trigger"], s["app"], s["same"], s["camera_busy"]), ("card-", 0x5101, 3, True))
        line = omkdebug.format_record(2, 2, 900, snap, names)
        self.assertIn("FILECLBK WAI RDTQ 0030 p55 r 606b9100", line)
        self.assertIn("OMK_LOAD RUN p66", line)

    def test_task_dump_and_list(self):
        head = struct.pack("<9I", 0x534B4D4F, 1, 1, 1, 5000, 0x5100, 0, 0x9F | 0x1023 << 16, 0x2005)
        task = struct.pack("<6H4B8I6I", 0x51, 4, 0x20, 0x30, 55, 55, 0, 0, 0, 2, 0, 0, 0, 0x606b0000, 0x61000000, 0x800,
                           0x61000700, 0, 0x606b9100, 0, 0, 0, 0, 0) + b"FILECLBK".ljust(20, b"\0")
        obj = struct.pack("<4H2I", 0x30, 0x20, 0, 0, 0, 0)
        d = omkdebug.parse_task_dump(head + task + obj)
        self.assertEqual(d["running"], [0x9F, 0x1023, 0x2005])
        self.assertEqual((d["tasks"][0]["name"], d["tasks"][0]["ret"]), ("FILECLBK", [0x606B9100]))
        self.assertEqual(d["objects"][0]["kind"], 0x20)
        lst = struct.pack("<4I", 0x10, 0, 1, 2) + b"100OLYMP".ljust(48, b"\0") + struct.pack("<4I", 0x20, 12345, 1, 2)
        lst += b"P1010001.ORF".ljust(48, b"\0")
        lst = lst[:76] + struct.pack("<I", 0x5D787263) + lst[80:]   # the second entry changed: as the camera wrote CORE.LOG
        e = omkdebug.parse_list(lst)
        self.assertEqual([(x["name"], x["dir"], x["size"]) for x in e], [("100OLYMP", True, 0), ("P1010001.ORF", False, 12345)])
        self.assertEqual(e[1]["changed"], "2026-11-24 14:19:06")


if __name__ == "__main__":
    unittest.main()
