"""ispkit fitting tests: the .cube look, the photo prior, the training set and a fit (stdlib unittest). Run from the
repository root:

    python -m unittest discover -s OMKit/apps -t OMKit/apps
"""
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

from ispkit import camera, color, cube, ispfile, model, prior, train


def cube_text(fn, n=9, head=""):
    """A .cube of size n with the output fn(rgb) at its nodes (red fastest)."""
    v = np.linspace(0, 1, n)
    b, g, r = np.meshgrid(v, v, v, indexing="ij")
    out = np.clip(fn(np.stack([r.ravel(), g.ravel(), b.ravel()], 1)), 0, 1)
    return head + f"LUT_3D_SIZE {n}\n" + "".join(f"{a:.6f} {c:.6f} {d:.6f}\n" for a, c, d in out)


def warm(rgb):
    y = rgb @ np.array([0.2126, 0.7152, 0.0722])
    s = y[:, None] + (rgb - y[:, None]) * 1.2
    return (0.5 + (s - 0.5) * 1.1) * np.array([1.03, 1.0, 0.94])


class Cube(unittest.TestCase):
    def test_identity(self):
        """An identity LUT with sRGB in and out renders the scene as plain sRGB"""
        t = cube.CubeTarget(text=cube_text(lambda x: x, 17))
        self.assertEqual((t.enc_in, t.enc_out, t.n), ("srgb", "srgb", 17))
        x = np.random.default_rng(0).uniform(0, 1, (500, 3))
        self.assertLess(np.abs(t(x) - color.srgb_encode(x)).max(), 1e-6)

    def test_guess_and_domain(self):
        t = cube.CubeTarget(text=cube_text(lambda x: x, 2, '# Cineon log input, Rec709 output\n'))
        self.assertEqual((t.enc_in, t.enc_out), ("cineon", "bt1886"))
        t = cube.CubeTarget(text=cube_text(lambda x: x, 2, 'TITLE "x"\n# ACEScct\n'))
        self.assertEqual((t.enc_in, t.enc_out, t.title), ("acescct", "srgb", "x"))
        t = cube.CubeTarget(text=cube_text(lambda x: x, 2, "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"), enc_in="linear")
        self.assertAlmostEqual(float(t.apply(np.array([[1.0, 1.0, 1.0]]))[0, 0]), 0.5)

    def test_refused(self):
        for text in ("LUT_1D_SIZE 2\n0 0 0\n1 1 1\n", "LUT_3D_SIZE 3\n0 0 0\n", "hello\n",
                     cube_text(lambda x: x, 2, "DOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n")):
            with self.assertRaises(ValueError):
                cube.CubeTarget(text=text)
        with self.assertRaises(ValueError):
            cube.CubeTarget(text=cube_text(lambda x: x, 2), enc_in="slog3")


class Prior(unittest.TestCase):
    def test_bundled(self):
        pr = prior.load()
        for b in prior.BINS:
            x, w = pr.cells(b)
            self.assertEqual((x.shape[1], x.dtype, w.dtype), (3, np.float64, np.float64))
            self.assertAlmostEqual(float(w.sum()), 1.0, places=9)
            self.assertTrue(np.all(x >= 0) and np.all(x <= 1))
        self.assertGreater(len(pr.cells(24)[0]), len(pr.cells(16)[0]))

    def test_build_save_load(self):
        rng = np.random.default_rng(3)
        with tempfile.TemporaryDirectory() as tmp:
            for i in range(3):
                a = (rng.uniform(0, 1, (60, 90, 3)) * 255).astype(np.uint8)
                Image.fromarray(a).save(Path(tmp) / f"p{i}.png")
            Path(tmp, "notes.txt").write_text("not a picture")
            paths = prior.pictures_in(tmp)
            self.assertEqual(len(paths), 3)
            imgs = [prior.load_picture(p) for p in paths]
            pr = prior.build(imgs, description="test")
            out = prior.save(pr, Path(tmp) / "mine")
            self.assertEqual(out.suffix, ".npz")
            back = prior.load(out)
            for b in prior.BINS:
                self.assertTrue(np.array_equal(back.cells(b)[0], pr.cells(b)[0]))
                self.assertTrue(np.array_equal(back.cells(b)[1], pr.cells(b)[1]))
            self.assertEqual((back.info["pictures"], back.info["pixels"]), (3, 3 * 60 * 90))
            self.assertEqual(back.digest, pr.digest)
            # one cell per distinct bin: the mean linear colour of a single-colour picture is that colour
            one = prior.bin_pixels([np.full((10, 3), 128, np.uint8)], [1.0], 24)
            self.assertEqual(len(one[0]), 1)
            self.assertAlmostEqual(float(one[0][0, 0]), float(color.srgb_decode(128 / 255)))
            Path(tmp, "bad.npz").write_bytes(b"not a zip")
            with self.assertRaises(ValueError):
                prior.load(Path(tmp) / "bad.npz")


class Fit(unittest.TestCase):
    def test_training_set(self):
        x, w = train.training_set(quick=True)
        x0, w0 = train.training_set(quick=True, prior=False)
        n = len(prior.load().cells(16)[0])
        self.assertEqual(len(x), len(x0) + n)
        self.assertAlmostEqual(float(w[len(x0):].sum()), train.PRIOR_WEIGHT, places=6)

    def test_quick_fit(self):
        """A quick fit of a warm look: the copy is close on ColorChecker, the tables write an .isp block"""
        t = cube.CubeTarget(text=cube_text(warm), name="warm")
        info = {}
        hw = train.fit_look(t, "best", quick=True, info=info, log=lambda s: None)
        self.assertEqual(info["mono"], "")
        self.assertEqual(info["stages"], train.plan("best"))
        acc = train.accuracy(t, hw, train.use_of("best"))
        self.assertLess(float(acc["colorchecker"].mean()), 2.0)
        self.assertEqual(len(ispfile.ispb_om5m2(hw)), ispfile.ISPB_SIZE)
        back = train.hardware_of_dict(train.hardware_dict(hw))
        self.assertEqual(ispfile.ispb_om5m2(back), ispfile.ispb_om5m2(hw))

    def test_mono(self):
        t = cube.CubeTarget(text=cube_text(lambda x: np.repeat((x @ np.array([0.3, 0.55, 0.15]))[:, None], 3, 1)))
        info = {}
        hw = train.fit_look(t, "full", quick=True, info=info, log=lambda s: None)
        self.assertEqual((info["mono"], info["stages"]), ("neutral", 1))
        self.assertEqual(hw.k_q, 0)

    def test_camera_target(self):
        """The metered grey of the camera reaches the look as the scene's 0.18"""
        t = cube.CubeTarget(text=cube_text(lambda x: x, 33), enc_in="linear")
        ct = train.camera_target(t)
        got = ct(camera.table_of_scene(np.full((1, 3), 0.18)))
        self.assertLess(abs(float(got[0, 1]) - 0.18), 2e-3)


if __name__ == "__main__":
    unittest.main()
