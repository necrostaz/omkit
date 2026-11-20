"""ISP player server tests on real HTTP (stdlib unittest). Run from the repository root:

    python -m unittest discover -s OMKit/apps -t OMKit/apps
"""
import io
import json
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image

from isp_player import server

PRESET = Path(__file__).resolve().parents[3] / "resources" / "modules" / "filmlook" / "presets" / "PORTRA400.ISP"


def _jpeg(w=64, h=48):
    y, x = np.mgrid[0:h, 0:w]
    a = np.stack([x / w, y / h, 0.5 + 0.4 * np.sin(x / 7.0)], -1)
    buf = io.BytesIO()
    Image.fromarray((a * 255).astype(np.uint8)).save(buf, "JPEG", quality=92)
    return buf.getvalue()


class Server(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = server.serve(0)
        cls.base = f"http://127.0.0.1:{cls.srv.server_address[1]}"
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()

    def get(self, path):
        try:
            with urllib.request.urlopen(self.base + path, timeout=60) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def post(self, path, body, name=None):
        if name is None:
            req = urllib.request.Request(self.base + path, json.dumps(body).encode(), {"Content-Type": "application/json"})
        else:
            req = urllib.request.Request(self.base + path, body, {"X-Name": name})
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_page(self):
        code, body = self.get("/")
        self.assertEqual(code, 200)
        self.assertIn(b"ISP Player", body)
        self.assertEqual(self.get("/static/player.js")[0], 200)
        self.assertEqual(self.get("/nope")[0], 404)

    @unittest.skipUnless(PRESET.exists(), "no bundled preset")
    def test_render(self):
        code, isp = self.post("/api/isp", PRESET.read_bytes(), PRESET.name)
        self.assertEqual((code, isp["renderable"], isp["errors"]), (200, True, []))
        self.assertEqual(self.get(f"/api/cassette?isp={isp['id']}")[0], 200)
        code, img = self.post("/api/image", _jpeg(), "pic.jpg")
        self.assertEqual((code, img["w"], img["h"]), (200, 64, 48))
        code, r = self.post("/api/render", {"image": img["id"], "isp": isp["id"], "ev": 0})
        self.assertEqual(code, 200)
        self.assertTrue(r["isp"].startswith("data:image/png;base64,"))
        code, r = self.post("/api/autoev", {"image": img["id"], "isp": isp["id"]})
        self.assertEqual(code, 200)

    def test_errors(self):
        code, r = self.post("/api/isp", b"junk" * 40, "junk.isp")
        self.assertEqual((code, r["renderable"]), (200, False))
        code, r = self.post("/api/render", {"image": "none"})
        self.assertEqual(code, 400)


if __name__ == "__main__":
    unittest.main()
