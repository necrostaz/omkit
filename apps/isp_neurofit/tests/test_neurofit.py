"""ISP neurofit tests on synthetic data only (stdlib unittest). Run from the repository root:

    python -m unittest discover -s OMKit/apps -t OMKit/apps

The look and its estimate: "scans" are a known look applied to other random scenes, and the estimate must find it. The
command line: a whole run (analyze, cassettes, preset, anchors, init, estimate, fit, status, restore) on synthetic
pictures - the RAW reader is replaced by one that makes a scene from the file's bytes, so no camera file is needed. The
skill: its frontmatter, the commands it names, the installer and the wrapper script."""
import contextlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
import warnings
from pathlib import Path
from unittest import mock

import numpy as np
from PIL import Image

from isp_neurofit import cli, install_skill
from isp_neurofit import look as lk
from isp_neurofit import pictures as pic
from isp_neurofit import stats as st
from ispkit import camera, cani, color, ispfile

APPS = Path(__file__).resolve().parents[2]
SKILL = APPS / "isp_neurofit" / "skill" / "isp-neurofit"


def scenes(rng, n, pictures):
    """Random "pictures" as white-balanced RAW: scenes (linear BT.709, grey 0.18) with a spread of light and colour."""
    out = []
    for _ in range(pictures):
        grey = 0.18 * 2.0 ** rng.normal(0, 1.6, (n, 1))
        tint = np.exp(rng.normal(0, 0.45, (n, 3)) * rng.uniform(0, 1, (n, 1)))
        x = np.clip(grey * tint, 0, 2.4)
        out.append(np.clip(camera.raw_of_table(camera.table_of_scene(x)), 0, 1).reshape(-1, 1, 3))
    return out


class FakeRun:
    """Sets without files: what estimate takes from a Run."""

    def __init__(self, raws, scans, anchors=()):
        self._raws = [({"file": f"r{i}", "weight": 1.0}, a) for i, a in enumerate(raws)]
        self._scans = [({"file": f"s{i}", "weight": 1.0}, a) for i, a in enumerate(scans)]
        self._sel = {"anchors": list(anchors)}

    def selection(self):
        return self._sel

    def raws(self, sel=None):
        return self._raws

    def scans(self, sel=None):
        return self._scans


def film():
    """A known "film" look: more contrast, lifted black, warm highlights, saturation 0.8."""
    p = lk.natural_look()
    t = np.array(p["tone_L"])
    p["tone_L"] = np.clip(50 + (t - 50) * 1.25, 4, 97).tolist()
    p["cast_a"], p["cast_b"] = [0, 1, 2, 2, 1], [-3, -1, 2, 5, 4]
    p["sat"] = 0.8
    return p


def dist(run, p):
    s = st.set_stats(*st.pixels(run.scans(), st.scan_lab))
    r, _ = st.raw_stats(run.raws(), st.look_lab(lk.ScanLook(p)), s["Lq"][8])
    return st.distance(r, s)[0]


class Look(unittest.TestCase):
    def test_natural_look(self):
        look = lk.ScanLook(lk.natural_look())
        lab = look.lab(np.array([[0.18] * 3, [0.0] * 3, [2.5] * 3, [0.02] * 3]))
        self.assertAlmostEqual(lab[0, 0], 49.5, delta=0.6)             # the metered grey: the middle
        self.assertLess(lab[1, 0], 1.0)
        self.assertGreater(lab[2, 0], 99.0)                            # the camera's headroom reaches white
        self.assertLess(np.abs(lab[:, 1:]).max(), 0.05)                # a neutral stays neutral
        out = look(np.array([[0.18, 0.18, 0.18], [0.4, 0.1, 0.05]]))
        self.assertTrue((out >= 0).all() and (out <= 1).all())

    def test_lab_roundtrip(self):
        d = np.random.default_rng(0).uniform(0, 1, (500, 3))
        self.assertLess(np.abs(lk.linear_of_lab(lk.lab_of_linear(d)) - d).max(), 1e-6)

    def test_look_is_a_fitter_target(self):
        ct = camera.CameraTarget(lk.ScanLook(film()))
        s = camera.table_of_scene(np.array([[0.18] * 3, [0.5, 0.2, 0.1]]))
        self.assertEqual(ct(s).shape, (2, 3))

    def test_check(self):
        self.assertEqual(lk.check(lk.natural_look()), [])
        p = lk.natural_look()
        p["tone_L"][5] = 99
        self.assertTrue(any("decrease" in x for x in lk.check(p)))
        p = lk.natural_look()
        del p["hue_sat"]
        p["cast_a"] = [0, 0]
        self.assertEqual(len(lk.check(p)), 2)

    def test_pin_grey(self):
        p = lk.natural_look()
        p["tone_L"] = (np.array(p["tone_L"]) - 6).clip(0).tolist()       # a darker curve
        q = lk.pin_grey(p, log=lambda *_: None)
        self.assertAlmostEqual(float(np.interp(0, lk.TONE_EV, q["tone_L"])),
                               float(np.interp(0, lk.TONE_EV, lk.natural_tone())), delta=0.05)


class Estimate(unittest.TestCase):
    def test_recovers_a_known_look(self):
        rng = np.random.default_rng(3)
        look = lk.ScanLook(film())
        raws = scenes(rng, 4000, 6)
        scans = [look(camera.scene_of_raw(a.reshape(-1, 3))).reshape(-1, 1, 3) for a in scenes(rng, 4000, 6)]
        run = FakeRun(raws, scans)
        start = lk.natural_look()
        d0 = dist(run, start)
        p = start
        for _ in range(3):
            p = cli.estimate(run, p, ["tone", "cast", "sat"], 0.7, log=lambda *_: None)
        d1 = dist(run, p)
        self.assertLess(d1["score"], 0.3 * d0["score"])
        self.assertAlmostEqual(p["sat"], 0.8, delta=0.08)
        self.assertAlmostEqual(p["cast_b"][3], 5.0, delta=1.5)
        self.assertAlmostEqual(p["tone_L"][0], 4.0, delta=2.5)         # the lifted black is found
        t = np.array(p["tone_L"])
        self.assertTrue((np.diff(t) >= 0).all())
        ref = np.array(start["tone_L"])
        slope = np.interp(1, lk.TONE_EV, t) - np.interp(-1, lk.TONE_EV, t)
        slope0 = np.interp(1, lk.TONE_EV, ref) - np.interp(-1, lk.TONE_EV, ref)
        self.assertAlmostEqual(slope / slope0, 1.25, delta=0.12)

    def test_exposure_difference_is_not_a_look(self):
        rng = np.random.default_rng(5)
        nat = lk.ScanLook(lk.natural_look())
        raws = scenes(rng, 4000, 6)
        # the "scans": the same neutral look on scenes a stop brighter - the look must not change
        scans = [nat(camera.scene_of_raw(a.reshape(-1, 3)) * 2.0).reshape(-1, 1, 3) for a in scenes(rng, 4000, 6)]
        p = lk.natural_look()
        for _ in range(3):
            p = cli.estimate(FakeRun(raws, scans), p, ["tone"], 0.7, log=lambda *_: None)
        mid = (lk.TONE_EV > -4) & (lk.TONE_EV < 1.5)
        self.assertLess(np.abs(np.array(p["tone_L"]) - np.array(lk.natural_look()["tone_L"]))[mid].max(), 4.0)

    def test_anchor_guard_and_use(self):
        rng = np.random.default_rng(7)
        raws = scenes(rng, 2000, 2)
        nat = lk.ScanLook(lk.natural_look())
        skin = np.clip(camera.raw_of_table(camera.table_of_scene(np.tile([[0.45, 0.25, 0.17]], (64, 1)))), 0, 1)
        raws.append(skin.reshape(8, 8, 3))
        lab = nat.lab(camera.scene_of_raw(skin))

        def scan_of(dh, gain):
            h = np.radians(np.degrees(np.arctan2(lab[:, 2], lab[:, 1])) + dh)
            c = np.hypot(lab[:, 1], lab[:, 2]) * gain
            out = np.stack([lab[:, 0], c * np.cos(h), c * np.sin(h)], 1)
            return color.srgb_encode(np.clip(lk.linear_of_lab(out), 0, 1)).reshape(8, 8, 3)

        box = [0.0, 0.0, 1.0, 1.0]
        for dh, gain, applied in ((8.0, 0.85, True), (80.0, 1.0, False)):
            scans = [nat(camera.scene_of_raw(a.reshape(-1, 3))).reshape(a.shape) for a in raws[:2]] + [scan_of(dh, gain)]
            run = FakeRun(raws, scans, [{"set": "raw", "file": "r2", "class": "skin", "box": box},
                                        {"set": "scan", "file": "s2", "class": "skin", "box": box}])
            log = []
            p = cli.estimate(run, lk.natural_look(), ["anchors"], 1.0, log=log.append)
            moved = max(np.abs(p["hue_shift"]))
            if applied:
                self.assertAlmostEqual(moved, dh, delta=3.0)
                rep = st.anchor_report(st.anchor_means(run.selection(), run.raws(), run.scans(),
                                                       st.look_lab(lk.ScanLook(p))))
                self.assertLess(rep["skin"]["dE_ab"], 2.0)
            else:
                self.assertEqual(moved, 0.0)
                self.assertTrue(any("NOT applied" in x for x in log))


class Sets(unittest.TestCase):
    def test_collect_sources(self):
        """Several folders and files; subfolders only when recursive; equal names told apart by their folder"""
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            for rel in ("a/1.jpg", "a/2.JPG", "a/note.txt", "b/1.jpg", "b/sub/3.tif", "c/x/1.jpg"):
                (t / rel).parent.mkdir(parents=True, exist_ok=True)
                (t / rel).write_bytes(b"")
            flat = pic.collect([t / "a", t / "b", t / "a" / "2.JPG"], pic.SCAN_EXT)
            self.assertEqual([k for k, _ in flat], ["a/1.jpg", "2.JPG", "b/1.jpg"])
            deep = dict(pic.collect([t], pic.SCAN_EXT, recursive=True))
            self.assertEqual(sorted(deep), ["2.JPG", "3.tif", "a/1.jpg", "b/1.jpg", "x/1.jpg"])
            self.assertEqual(deep["3.tif"], (t / "b/sub/3.tif").resolve())
            with self.assertRaises(pic.SetError):
                pic.collect([t / "missing"], pic.SCAN_EXT)

    def test_workspace(self):
        with self.assertRaises(cli.Failure):
            cli.workspace(str(cli.OMKIT / "runs"))
        with self.assertRaises(cli.Failure):
            cli.workspace(str(APPS))
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(cli.workspace(t), Path(t).resolve())
            with mock.patch.dict(os.environ, {cli.ENV_WORKSPACE: t}):
                self.assertEqual(cli.workspace(None), Path(t).resolve())
            for bad in ("../x", "a/b", "", "-x", "a" * 65):
                with self.assertRaises(cli.Failure):
                    cli.Run(bad, Path(t))


# ------------------------------------------------------------------ a whole run on synthetic pictures
def fake_orf(path: Path, seed: int, model=b"OM-5MarkII"):
    """A stand-in for an ORF: a model string the way the camera's EXIF holds it, and a seed for the scene."""
    path.write_bytes(b"IIRO\x08\x00\x00\x00" + b"\x00" * 64 + model + b"\x00" + bytes([seed]) + b"\x00" * 64)


def fake_raw_of_orf(data: bytes):
    """The RAW of a fake ORF: a scene with sky, foliage, skin and grey areas, its light set by the seed."""
    seed = data[8 + 64 + data[72:].index(b"\x00") + 1]
    rng = np.random.default_rng(seed)
    h, w = 160, 240
    y, x = np.mgrid[0:h, 0:w] / np.array([h, w])[:, None, None]
    scene = np.empty((h, w, 3))
    scene[:] = [0.2, 0.32, 0.6]                                   # sky
    scene[y > 0.4] = [0.08, 0.14, 0.05]                           # foliage
    scene[(y > 0.55) & (x > 0.6)] = [0.45, 0.28, 0.2]             # skin
    scene[(y > 0.8) & (x < 0.4)] = [0.18, 0.18, 0.18]             # grey
    scene *= 2.0 ** rng.normal(0, 0.5) * (1 + 0.15 * np.sin(9 * x + 5 * y + seed))[..., None]
    return np.clip(camera.raw_of_table(camera.table_of_scene(scene.reshape(-1, 3))), 0, 1).reshape(h, w, 3)


def fake_scan(path: Path, seed: int, look):
    raw = fake_raw_of_orf(b"IIRO\x08\x00\x00\x00" + b"\x00" * 64 + b"X\x00" + bytes([seed + 100]))
    rgb = look(camera.scene_of_raw(raw.reshape(-1, 3))).reshape(raw.shape)
    Image.fromarray((rgb * 255 + 0.5).astype(np.uint8)).save(path, quality=93)


class CommandLine(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        t = Path(cls.tmp.name)
        cls.ws = t / "ws"
        (t / "raw").mkdir()
        (t / "scans").mkdir()
        for i in range(8):
            fake_orf(t / "raw" / f"P{i:07d}.ORF", i)
        fake_orf(t / "raw" / "OTHER.ORF", 9, model=b"E-M10MarkIV")
        look = lk.ScanLook(film())
        for i in range(8):
            fake_scan(t / "scans" / f"scan{i:02d}.jpg", i, look)
        cls.raw, cls.scans = t / "raw", t / "scans"
        cls.patch = mock.patch.object(camera, "raw_of_orf", fake_raw_of_orf)
        cls.patch.start()

    @classmethod
    def tearDownClass(cls):
        cls.patch.stop()
        cls.tmp.cleanup()

    def nf(self, *args, code=0):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out), warnings.catch_warnings():
            warnings.simplefilter("ignore")
            rc = cli.main([args[0], "--workspace", str(self.ws), *args[1:]] if args[0] != "check-raw" else list(args))
        self.assertEqual(rc, code, out.getvalue())
        return out.getvalue()

    def test_run(self):
        out = self.nf("analyze", "--run", "film", "--raw", str(self.raw), "--scans", str(self.scans))
        self.assertIn("NOT OM-5MarkII (1): OTHER.ORF", out)
        run = self.ws / "film"
        an = json.loads((run / "analysis.json").read_text(encoding="utf-8"))
        self.assertEqual((len(an["raw"]), len(an["scans"])), (9, 8))
        self.assertTrue(an["sets"]["noise_floor"])
        self.assertTrue((run / "contact_raw_01.jpg").exists() and (run / "contact_scan_01.jpg").exists())
        # the choices: the other camera out, anchors in both sets
        sel = json.loads((run / "selection.json").read_text(encoding="utf-8"))
        for e in sel["raw"]:
            e["use"] = e["file"] != "OTHER.ORF"
        sel["anchors"] = [{"set": "raw", "file": "P0000001.ORF", "class": "neutral", "box": [0.05, 0.85, 0.35, 0.98]},
                          {"set": "scan", "file": "scan01.jpg", "class": "neutral", "box": [0.05, 0.85, 0.35, 0.98]},
                          {"set": "raw", "file": "P0000002.ORF", "class": "skin", "box": [0.65, 0.6, 0.95, 0.75]},
                          {"set": "scan", "file": "scan02.jpg", "class": "skin", "box": [0.65, 0.6, 0.95, 0.75]},
                          {"set": "scan", "file": "nope.jpg", "class": "sky", "box": [0, 0, 1, 0.3]}]
        (run / "selection.json").write_text(json.dumps(sel), encoding="utf-8")
        out = self.nf("analyze", "--run", "film")                  # the same sets, the choices kept
        sel2 = json.loads((run / "selection.json").read_text(encoding="utf-8"))
        self.assertEqual([e["use"] for e in sel2["raw"]], [e["use"] for e in sel["raw"]])
        self.assertEqual(len(sel2["anchors"]), 5)
        out = self.nf("anchors", "--run", "film")
        self.assertIn("not used: scan 'nope.jpg'", out)
        self.assertTrue((run / "anchors.jpg").exists())
        # the preset: a bundled preset's file name is refused, a cassette is drawn
        self.nf("preset", "--run", "film", "--title", "Fujifilm C200", "--file", "C200", code=2)
        self.nf("cassettes", "--run", "film", "--label", "TEST 400", "--ids", "yellow,p_portra,1")
        self.assertTrue((run / "cassettes.png").stat().st_size > 1000)
        self.nf("preset", "--run", "film", "--title", "Test Film 400", "--file", "TESTF400", "--cassette", "yellow",
                "--label", "TEST 400", "--brand", "ACME", "--proc", "C-41")
        ps = json.loads((run / "preset.json").read_text(encoding="utf-8"))
        self.assertEqual((ps["file"], ps["cassette"], ps["iso"]), ("TESTF400", "yellow", "400"))
        # the look
        self.nf("estimate", "--run", "film", code=2)                # no look.json yet
        self.nf("init", "--run", "film")
        self.nf("init", "--run", "film", code=2)                    # exists
        n0 = lk.natural_look()
        self.nf("estimate", "--run", "film", "--parts", "tone,cast,sat", "--rounds", "2")
        p = json.loads((run / "look.json").read_text(encoding="utf-8"))
        self.assertNotEqual(p["tone_L"], n0["tone_L"])
        self.assertNotEqual((p["cast_b"], p["sat"]), (n0["cast_b"], n0["sat"]))
        self.nf("estimate", "--run", "film", "--parts", "bogus", code=2)
        out = self.nf("fit", "--run", "film", "--quick", "--note", "first", "--out", str(self.ws / "out" / "T.ISP"))
        self.assertIn("iter 1:", out)
        it = run / "iter_01"
        parsed = ispfile.read((it / "TESTF400.ISP").read_bytes())
        self.assertEqual(parsed["name"], "Test Film 400")
        self.assertEqual(cani.parse(parsed["sections"]["CANI"])["name"], "TEST 400")
        meta = ispfile.meta_of(parsed["sections"]["META"])
        self.assertEqual((meta["variant"], meta["quick"], meta["art"]), ("best", "1", "generic:1"))
        self.assertEqual((self.ws / "out" / "T.ISP").read_bytes(), (it / "TESTF400.ISP").read_bytes())
        m = json.loads((it / "metrics.json").read_text(encoding="utf-8"))
        for k in ("isp_vs_scans", "tables_vs_look_dE2000", "anchors", "noise_floor", "compared_at_ev", "mono"):
            self.assertIn(k, m)
        self.assertLess(m["tables_vs_look_dE2000"]["mean"], 4)
        for f in ("board_01.jpg", "stages.jpg", "hardware.json", "look.json"):
            self.assertTrue((it / f).exists(), f)
        # a broken hand edit is refused; restore brings the iteration's look back
        bad = dict(p, tone_L=p["tone_L"][:5])
        (run / "look.json").write_text(json.dumps(bad), encoding="utf-8")
        self.nf("fit", "--run", "film", "--quick", code=2)
        self.nf("restore", "--run", "film", "--iter", "1")
        self.assertEqual(json.loads((run / "look.json").read_text(encoding="utf-8"))["tone_L"], p["tone_L"])
        out = self.nf("status", "--run", "film")
        self.assertIn("first", out)
        self.nf("pin-grey", "--run", "film")
        # a second run may not take the first run's file name
        self.nf("preset", "--run", "other", "--title", "Another", "--file", "TESTF400", code=2)
        self.nf("status", "--run", "nothing", code=2)

    def test_check_raw(self):
        out = self.nf("check-raw", "--dir", str(self.raw))
        self.assertIn("9 RAW files, 8 of OM-5MarkII, 1 of another camera", out)

    def test_raw_every(self):
        out = self.nf("analyze", "--run", "every", "--raw", str(self.raw), "--scans", str(self.scans), "--raw-every", "3")
        self.assertIn("every 3th of 9", out)
        sel = json.loads((self.ws / "every" / "selection.json").read_text(encoding="utf-8"))
        self.assertEqual(len(sel["raw"]), 3)


class Skill(unittest.TestCase):
    def frontmatter(self, text):
        m = re.match(r"---\n(.*?)\n---\n", text, re.S)
        self.assertIsNotNone(m)
        out = {}
        for line in m.group(1).splitlines():
            k, _, v = line.partition(":")
            out[k.strip()] = v.strip()
        return out

    def test_frontmatter_and_commands(self):
        text = (SKILL / "SKILL.md").read_text(encoding="utf-8")
        fm = self.frontmatter(text)
        # Agent Skills: name and description required; license and the other optional fields of the format allowed
        self.assertLessEqual({"name", "description", "license"}, set(fm))
        self.assertLessEqual(set(fm), {"name", "description", "license", "compatibility", "metadata", "allowed-tools"})
        self.assertEqual(fm["name"], SKILL.name)
        self.assertRegex(fm["name"], r"^[a-z0-9]+(-[a-z0-9]+)*$")
        self.assertTrue(0 < len(fm["description"]) <= 1024)
        known = set(cli.parser()._subparsers._group_actions[0].choices)
        texts = [text] + [p.read_text(encoding="utf-8") for p in (SKILL / "references").glob("*.md")]
        used = {c for t in texts for c in re.findall(r"python scripts/neurofit\.py ([a-z-]+)", t)}
        self.assertTrue(used and used <= known, used - known)
        for link in re.findall(r"\]\((references/[^)]+)\)", text):
            self.assertTrue((SKILL / link).is_file(), link)

    def test_install_and_wrapper(self):
        with tempfile.TemporaryDirectory() as t:
            dest = install_skill.install(Path(t) / "skills")
            self.assertEqual(dest.name, "isp-neurofit")
            cfg = json.loads((dest / "scripts" / "config.json").read_text(encoding="utf-8"))
            self.assertEqual(Path(cfg["apps"]), APPS)
            with self.assertRaises(SystemExit):
                install_skill.install(Path(t) / "skills")             # exists
            install_skill.install(Path(t) / "skills", force=True)
            env = {k: v for k, v in os.environ.items() if k not in ("OMKIT_APPS", "PYTHONPATH")}
            out = subprocess.run([sys.executable, str(dest / "scripts" / "neurofit.py"), "init", "--help"], cwd=t,
                                 env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("--workspace", out.stdout)


if __name__ == "__main__":
    unittest.main()
