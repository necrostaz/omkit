"""ispkit unit tests (stdlib unittest). Run from the repository root:

    python -m unittest discover -s OMKit/apps -t OMKit/apps
"""
import io
import struct
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

from ispkit import camera, cani, canister, editors, gpu, ispfile, model, picture, render

PRESETS = Path(__file__).resolve().parents[3] / "resources" / "modules" / "filmlook" / "presets"


def _presets():
    return sorted(PRESETS.glob("*.ISP"))


def _neutral_tables():
    """A plain table set: linear gamma, a gentle tone, no sectors"""
    st = model.State(gamma=np.tile(np.linspace(0, 1, model.KG) ** 0.6, (3, 1)), tone=np.linspace(0, 1, model.KT))
    return model.quantise(st)


class IspFile(unittest.TestCase):
    def test_round_trip(self):
        hw = _neutral_tables()
        hw.a_sat[3] = 120
        hw.a_hue[7] = -455
        data = ispfile.write("Round Trip 100", "OM5M2", ispfile.ispb_om5m2(hw), meta={"domain": "lin"},
                             extra=[(b"CANI", cani.generic(1, "Round Trip 100", "100"))])
        f = ispfile.read(data)
        self.assertEqual((f["name"], f["platform"], f["version"]), ("Round Trip 100", "OM5M2", (1, 0)))
        self.assertEqual(sorted(f["sections"]), ["CANI", "ISPB", "META"])
        back = ispfile.hardware_from_ispb(f["sections"]["ISPB"])
        for k in ("matrix_q7", "gamma12", "tone10", "a_sat", "a_hue", "a_lum", "h0", "hw", "c0", "cw", "y0", "yw", "prof"):
            self.assertTrue(np.array_equal(getattr(back, k), getattr(hw, k)), k)
        self.assertEqual(ispfile.meta_of(f["sections"]["META"]), {"domain": "lin"})
        self.assertEqual(ispfile.peek_header(data)["name"], "Round Trip 100")
        self.assertTrue(all(s["ok"] for s in ispfile.list_sections(data)))

    def test_damage_is_found(self):
        data = bytearray(ispfile.write("X", "OM5M2", ispfile.ispb_om5m2(_neutral_tables())))
        data[200] ^= 0xFF
        with self.assertRaises(ValueError):
            ispfile.read(bytes(data))
        self.assertFalse(ispfile.list_sections(bytes(data))[0]["ok"])
        with self.assertRaises(ValueError):
            ispfile.read(b"not an isp file" * 8)

    def test_use_of_present(self):
        self.assertEqual(ispfile.use_of_present(0x1F), ("matrix", "sat", "sectors"))
        self.assertEqual(ispfile.use_of_present(ispfile.PRESENT_GAMMA | ispfile.PRESENT_TONE), ())

    @unittest.skipUnless(_presets(), "no bundled presets")
    def test_bundled_presets(self):
        for p in _presets():
            f = ispfile.read(p.read_bytes())
            b = f["sections"]["ISPB"]
            self.assertEqual(len(b), ispfile.ISPB_SIZE, p.name)
            hw = ispfile.hardware_from_ispb(b)
            present = struct.unpack_from("<H", b, 2)[0]
            again = ispfile.ispb_om5m2(hw, struct.unpack_from("<H", b, 4)[0], bool(present & ispfile.PRESENT_MATRIX))
            self.assertEqual(again[:2] + again[4:], b[:2] + b[4:], p.name)
            info = cani.parse(f["sections"]["CANI"])
            self.assertEqual(info["kind"], cani.KIND_DRAW)
            self.assertEqual(len(editors.show(b)["curve"]), 64)


class Cassettes(unittest.TestCase):
    def test_generics(self):
        self.assertEqual(len(cani.GENERICS), len(canister.GENERICS))
        for i in range(len(cani.GENERICS)):
            blob = cani.generic(i, "Some Film 400", "400")
            info = cani.parse(blob)
            self.assertEqual((info["name"], info["iso"]), ("Some Film 400", "400"))
            self.assertTrue(canister.limits(blob)["ok"])
        png = canister.render_png(cani.generic(0, "Some Film 400", "400"))
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")

    def test_generic_choice(self):
        self.assertEqual(cani.GENERICS[cani.default_generic("Ilford HP5 400")][0], "bw")
        self.assertEqual(cani.GENERICS[cani.default_generic("Something Else")][0], "filmlook")
        self.assertEqual(cani.guess_iso("Vision3 500T"), "500T")

    def test_refuses_kind_3(self):
        blob = bytearray(cani.generic(0))
        blob[2] = cani.KIND_GENERIC
        with self.assertRaises(ValueError):
            cani.parse(bytes(blob))


class Model(unittest.TestCase):
    def test_grey_stays_grey(self):
        hw = _neutral_tables()
        s = camera.table_of_scene(np.linspace(0, 1, 50)[:, None] * np.ones(3) * 0.18 * 4)
        out = model.run_hardware(s, hw, "lin", ())
        self.assertTrue(np.all((out >= 0) & (out <= 1)))
        self.assertTrue(np.allclose(out * 255, np.round(out * 255)))            # 8-bit output
        self.assertLessEqual(np.abs(out - out.mean(axis=1, keepdims=True)).max() * 255, 1.0 + 1e-9)
        self.assertTrue(np.all(np.diff(out[:, 1]) >= 0))                         # monotone in the light

    def test_sector_hue_shift_moves_hue(self):
        hw = _neutral_tables()
        hw.prof[:, model.PROFILE_ORDER.index("c_hue")] = model.PROF_ONE           # the hue shift at full strength
        s = camera.table_of_scene(np.array([[1.0, 0.05, 0.05]]))
        base = model.run_hardware(s, hw, "lin", ("sectors",))
        hw.a_hue[:] = 2731                                                         # +30 degrees in every sector
        moved = model.run_hardware(s, hw, "lin", ("sectors",))
        self.assertGreater(np.abs(moved - base).max(), 0.02)

    @unittest.skipUnless(gpu.available(), "no torch with CUDA")
    def test_gpu_copy(self):
        hw = _neutral_tables()
        hw.a_sat[2], hw.a_hue[5], hw.a_lum[8] = 200, 1500, 50
        rng = np.random.default_rng(1)
        s = rng.uniform(-0.05, 1.1, (20000, 3))
        cpu = model.run_hardware(s, hw, "lin", ("matrix", "sat", "sectors"))
        dev = gpu.run_hardware(s, hw, "lin", ("matrix", "sat", "sectors"))
        self.assertIsNotNone(dev)
        self.assertLessEqual(np.abs(cpu - dev).max() * 255, 1.0 + 1e-9)


class Pictures(unittest.TestCase):
    def test_load_and_render(self):
        y, x = np.mgrid[0:60, 0:90]
        a = np.stack([x / 90, y / 60, 0.5 + 0.4 * np.sin(x / 5.0)], -1)
        buf = io.BytesIO()
        Image.fromarray((a * 255).astype(np.uint8)).save(buf, "PNG")
        rec, info = picture.load_image(buf.getvalue(), "pic.png")
        self.assertEqual((info["w"], info["h"], info["space"]), (90, 60, "srgb"))
        self.assertEqual(len(rec["inv"]), 90 * 60)
        x_, s = render.signal(rec, 0.0)
        mm, backend = render.run_tables(s, _neutral_tables(), "lin", ())
        self.assertEqual(mm.shape, rec["lin"].shape)
        png = render.png_bytes(mm, rec["inv"], rec["shape"])
        self.assertEqual(Image.open(io.BytesIO(png)).size, (90, 60))
        with self.assertRaises(ValueError):
            picture.load_image(buf.getvalue(), "pic.png", "no-such-space")

    def test_distinct(self):
        a = np.array([[1, 2, 3], [0, 0, 0], [1, 2, 3]], float)
        u, inv, first = picture.distinct(a)
        self.assertEqual(len(u), 2)
        self.assertTrue(np.array_equal(u[inv], a))

    def test_rows_is_per_row(self):
        a = np.random.default_rng(0).uniform(0, 1, (render.CHUNK * 2 + 17, 3))
        self.assertTrue(np.array_equal(render.rows(lambda v: v * 2 + 1, a), a * 2 + 1))


if __name__ == "__main__":
    unittest.main()
