"""Tests of the module builder tools/mkmod.py. Run by `make -C OMKit test`, or from OMKit/:

    python3 -m unittest discover -s tests

OMKIT_TEST_MODULES names a folder with HELLO.OMM and hello.elf built by src/modules/Makefile (the make target builds
them when the MN10300 toolchain is there); without it the tests of a real module are skipped. Standard library only.
"""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

OMKIT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(OMKIT / "tools"))
import mkmod  # noqa: E402

MODULES = Path(os.environ["OMKIT_TEST_MODULES"]) if os.environ.get("OMKIT_TEST_MODULES") else None
HELLO_RES = OMKIT / "src" / "modules" / "hello" / "res"


def built(name: str) -> Path:
    return MODULES / name if MODULES else Path("/nonexistent") / name


class HelloModuleTest(unittest.TestCase):
    def setUp(self):
        if not built("HELLO.OMM").is_file() or not built("hello.elf").is_file():
            self.skipTest("no HELLO.OMM and hello.elf (OMKIT_TEST_MODULES; needs the MN10300 toolchain)")

    def test_output(self):
        """HELLO.OMM: header, CRC, relocations point into the image, the entry is in the image, the sizes add up."""
        data = built("HELLO.OMM").read_bytes()
        h = mkmod.parse(data)
        self.assertEqual((h["magic"], h["format"], h["name"], h["version"]), (mkmod.MAGIC, 1, "HELLO", 1 << 16))
        self.assertTrue(h["crc_ok"])
        self.assertEqual(h["api"], mkmod.api_version())
        self.assertLess(h["entry"], h["image"])
        offs = struct.unpack_from(f"<{h['relocs']}I", data, 64 + h["image"])
        self.assertEqual(list(offs), sorted(set(offs)))
        for o in offs:
            value = struct.unpack_from("<I", data, 64 + o)[0]
            self.assertLess(value, h["image"] + h["bss"], hex(o))
        self.assertEqual(len(data), 64 + h["image"] + 4 * h["relocs"] + 16 * h["res_count"] + h["res_bytes"] + h["res_disk"])
        self.assertEqual(h["res_disk"], 5000)

    def test_res_over(self):
        """--res-over: a resource of the same type and id replaces the module's own, a new one is added; both are listed."""
        elf = built("hello.elf").read_bytes()
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "1_1.txt").write_bytes(b"replaced")
            (Path(d) / "2_5.bin").write_bytes(b"added")
            notes: list = []
            data = mkmod.build(elf, "HELLO", (1, 0), HELLO_RES, mkmod.api_version(), Path(d), None, notes)
            own = mkmod.build(elf, "HELLO", (1, 0), HELLO_RES, mkmod.api_version())
        self.assertIn(b"replaced", data)
        self.assertIn(b"added", data)
        self.assertNotIn(b"Text resource of HELLO", data)
        self.assertIn(b"Text resource of HELLO", own)
        self.assertEqual(len(notes), 2, notes)
        self.assertTrue(any("1/1" in n and "replaces" in n for n in notes), notes)


class StringsTest(unittest.TestCase):
    def test_strings_table(self):
        """Strings: printable ASCII only, unique names, identical texts stored once, ids in line order."""
        blob, names = mkmod.compile_strings("# c\n; c\nA = Hi\nB = There\nC = Hi\nD =\n")
        self.assertEqual(names, ["A", "B", "C", "D"])
        count, zero, *offs = struct.unpack_from("<HH4H", blob)
        self.assertEqual((count, zero), (4, 0))
        self.assertEqual(offs[0], offs[2])
        self.assertEqual(blob[offs[1]:offs[1] + 6], b"There\0")
        self.assertEqual(blob[offs[3]], 0)
        self.assertEqual(len(blob), 12 + 3 + 6 + 1)
        self.assertEqual(blob[-1], 0)
        for bad in ("A = caf\xe9","A = tab\there", "a = lower name", "A = 1\nA = 2", "just text"):
            with self.assertRaises(mkmod.ModError, msg=bad):
                mkmod.compile_strings(bad)
        empty, none = mkmod.compile_strings("")
        self.assertEqual((empty, none), (b"\0\0\0\0\0", []))
        self.assertIn("#define STR_B 1", mkmod.strings_header(names))
        self.assertIn("#define STR_COUNT 4", mkmod.strings_header(names))


if __name__ == "__main__":
    unittest.main()
