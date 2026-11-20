"""ISP fitter server tests on real HTTP (stdlib unittest). Run from the repository root:

    python -m unittest discover -s OMKit/apps -t OMKit/apps
"""
import io
import json
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image

from isp_fitter import build_prior, server
from ispkit import cani, canister, ispfile, prior


def cube_bytes(n=9):
    """A warm look as a .cube of size n, sRGB in and out."""
    v = np.linspace(0, 1, n)
    b, g, r = np.meshgrid(v, v, v, indexing="ij")
    rgb = np.stack([r.ravel(), g.ravel(), b.ravel()], 1)
    y = rgb @ np.array([0.2126, 0.7152, 0.0722])
    out = np.clip((y[:, None] + (rgb - y[:, None]) * 1.2) * np.array([1.03, 1.0, 0.94]), 0, 1)
    return (f"TITLE \"Warm 200\"\nLUT_3D_SIZE {n}\n" + "".join(f"{a:.6f} {c:.6f} {d:.6f}\n" for a, c, d in out)).encode()


def jpeg(w=64, h=48):
    y, x = np.mgrid[0:h, 0:w]
    a = np.stack([x / w, y / h, 0.5 + 0.4 * np.sin(x / 7.0)], -1)
    buf = io.BytesIO()
    Image.fromarray((a * 255).astype(np.uint8)).save(buf, "JPEG", quality=92)
    return buf.getvalue()


class Server(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cache = tempfile.TemporaryDirectory()
        server.configure(None, cls.cache.name)
        cls.srv = server.serve(0)
        cls.base = f"http://127.0.0.1:{cls.srv.server_address[1]}"
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()
        server.configure(None, None)
        cls.cache.cleanup()

    def req(self, path, body=None, name=None, raw=False):
        if body is None:
            r = urllib.request.Request(self.base + path)
        elif name is not None:
            r = urllib.request.Request(self.base + path, body, {"X-Name": name})
        else:
            r = urllib.request.Request(self.base + path, json.dumps(body).encode(), {"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(r, timeout=300) as f:
                data = f.read()
                return f.status, data if raw else json.loads(data)
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_page(self):
        code, body = self.req("/", raw=True)
        self.assertEqual(code, 200)
        self.assertIn(b"fitter.js", body)
        self.assertEqual(self.req("/static/../server.py")[0], 404)
        code, v = self.req("/api/version")
        self.assertEqual((code, v["prior"]), (200, "bundled"))
        self.assertEqual(self.srv.server_address[0], "127.0.0.1")

    def test_flow(self):
        code, look = self.req("/api/look", cube_bytes(), "Warm 200.cube")
        self.assertEqual((code, look["enc_in"], look["title"]), (200, "srgb", "Warm 200"))
        code, r = self.req("/api/fit", {"look": look["id"], "variant": "curves+sat", "quick": True})
        self.assertEqual(code, 200)
        while r["state"] == "running":
            job = r.get("job", getattr(self, "job", None))
            self.job = job
            time.sleep(0.2)
            code, r = self.req("/api/job?id=" + job)
        self.assertEqual(r["state"], "done", r)
        fit = r["result"]
        self.assertEqual((fit["variant"], fit["quick"], fit["cached"]), ("curves+sat", True, False))
        code, img = self.req("/api/image", jpeg(), "pic.jpg")
        code, rr = self.req("/api/render", {"fit": fit["fit"], "image": img["id"], "ev": 0})
        self.assertEqual(code, 200)
        self.assertTrue(rr["isp"].startswith("data:image/png"))
        code, c = self.req("/api/cassettes?look=Warm%20200")
        self.assertEqual(c["iso"], "200")
        cas = {"gen": c["default"], "name": "WARM", "iso": "200", "brand": "", "proc": ""}
        code, data = self.req("/api/isp", {"fit": fit["fit"], "name": "Warm 200", "cassette": cas}, raw=True)
        self.assertEqual(code, 200)
        p = ispfile.read(data)
        self.assertEqual(p["name"], "Warm 200")
        self.assertEqual(p["sections"]["CANI"], canister.generic(c["default"], "WARM", "200"))
        self.assertEqual(cani.parse(p["sections"]["CANI"])["iso"], "200")
        self.assertEqual(len(list(Path(self.cache.name, "fits").glob("*.json"))), 1)

    def test_errors(self):
        code, r = self.req("/api/look", b"LUT_1D_SIZE 2\n0 0 0\n1 1 1\n", "x.cube")
        self.assertEqual(code, 400)
        code, r = self.req("/api/look", cube_bytes(), "x.txt")
        self.assertEqual(code, 400)
        code, r = self.req("/api/cassette", {"gen": 99})
        self.assertTrue(r["errors"])
        self.assertEqual(server.check_name("x" * 32)[0][0][:22], "the profile's name is ")
        self.assertEqual(server.file_name("Kodak Portra 400 / test"), "Kodak_Portra_400___test.isp")


class BuildPrior(unittest.TestCase):
    def test_cli(self):
        rng = np.random.default_rng(5)
        with tempfile.TemporaryDirectory() as tmp:
            pics = Path(tmp, "pics")
            pics.mkdir()
            for i in range(2):
                Image.fromarray((rng.uniform(0, 1, (300, 400, 3)) * 255).astype(np.uint8)).save(pics / f"{i}.jpg")
            out = Path(tmp, "mine.npz")
            import contextlib
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(build_prior.main([str(pics), str(out)]), 0)
            pr = prior.load(out)
            self.assertEqual((pr.info["pictures"], pr.info["pixels"]), (2, 2 * 240 * 180))


if __name__ == "__main__":
    unittest.main()
