"""ISP player: a local web page - drop a picture (the camera's RAW .ORF, JPEG, PNG, TIFF) and an .isp profile, see the
picture through the profile's tables as the OM-5 Mark II colour path model renders them, and what the file carries.

    python OMKit/apps/isp_player/run.py [--port 8790] [--open]

The player fits nothing: the tables come from the file. The blocks the tables run with are the ones the file marks
present (ISPB +2: matrix, saturation, sectors), as the camera applies them; the gamma domain is META "domain", else
linear. The picture, the table signal and the render are ispkit's (picture.py, render.py, model.py; gpu.py on a CUDA GPU).

Local use only: the server listens on 127.0.0.1 and keeps everything in memory (the last few pictures and profiles);
nothing is written to disk and nothing leaves the computer."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import threading
import time
import traceback
import webbrowser
import zlib
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

import ispkit                                    # first: it sets the BLAS threads before numpy loads
import numpy as np

from ispkit import camera, cani, canister, color, editors, gpu, ispfile, model, picture, render

HERE = Path(__file__).resolve().parent
STATIC = HERE / "static"
HOST = "127.0.0.1"
PORT = 8790

LOCK = threading.Lock()
IMAGES: OrderedDict = OrderedDict()      # id -> picture record (picture.load_image)
ISPS: OrderedDict = OrderedDict()        # id -> {"hw", "use", "domain", "info", "art"}
RENDERS: OrderedDict = OrderedDict()     # (image, isp, ev) -> render result
MAX_IMAGES, MAX_ISPS, MAX_RENDERS = 4, 32, 12
MAX_IMAGE_BYTES = 200 * 1024 * 1024      # an upload larger than this is refused (a camera RAW is about 20 MB)
MAX_ISP_BYTES = 1024 * 1024              # an .isp file is at most 64 KiB; larger uploads are not profiles
DRAIN_MAX = 2 * MAX_IMAGE_BYTES          # an upload over a limit is read and dropped up to this size (see do_POST)


def _put(cache: OrderedDict, key, value, limit):
    cache[key] = value
    cache.move_to_end(key)
    while len(cache) > limit:
        cache.popitem(last=False)


# ------------------------------------------------------------------ the picture
def load_image(data: bytes, name: str, space: str = "auto") -> dict:
    rec, info = picture.load_image(data, name, space)
    _put(IMAGES, info["id"], rec, MAX_IMAGES)
    return info


# ------------------------------------------------------------------ the .isp file
def _curve(v, top, n=64):
    """A 256-node table as n points 0..1 (for the page's small plots)"""
    v = np.asarray(v, float) / top
    return [round(float(t), 4) for t in np.interp(np.linspace(0, len(v) - 1, n), np.arange(len(v)), v)]


def summary(ispb: bytes, hw: model.Hardware) -> dict:
    """What the ISP block carries, readable: present blocks, base mode, matrix, k, the curves and the 12 sectors"""
    layout, present, base, _ = struct.unpack_from("<HHHH", ispb)
    shown = editors.show(ispb)                 # as the camera's editors show the preset (hue, colour of each sector)
    names = (("matrix", ispfile.PRESENT_MATRIX), ("gamma", ispfile.PRESENT_GAMMA), ("tone", ispfile.PRESENT_TONE),
             ("saturation", ispfile.PRESENT_SATURATION), ("sectors", ispfile.PRESENT_SECTORS))
    m = np.asarray(hw.matrix_q7, int)
    sectors = []
    for k in range(12):
        sectors.append({"hue": shown["hue"][k], "width": round(int(hw.hw[k]) * 360 / model.TURN, 1), "rgb": shown["rgb"][k],
                        "sat": round(int(hw.a_sat[k]) / 1023, 3), "hue_shift": round(int(hw.a_hue[k]) * 360 / model.TURN, 2),
                        "lum": round(int(hw.a_lum[k]) / 1023, 3), "sat_steps": shown["base"][k],
                        "c": [int(hw.c0[k]), int(hw.cw[k])], "y": [int(hw.y0[k]), int(hw.yw[k])]})
    return {"layout": layout, "present": {n: bool(present & b) for n, b in names}, "present_bits": present,
            "base_mode": ispfile.BASE_MODES.get(base, str(base)), "base_mode_value": base,
            "matrix_q7": m.tolist(), "matrix": (m / 128).round(3).tolist(),
            "matrix_identity": bool((m == np.eye(3, dtype=int) * 128).all()),
            "k": round(hw.k_q / 144, 3), "k_q": int(hw.k_q), "mono": shown["mono"],
            "gamma": [_curve(hw.gamma12[c], 4095) for c in range(3)], "tone": _curve(hw.tone10, 1023),
            "curve": [round(v / 255, 4) for v in shown["curve"]], "sectors": sectors, "sector_mask": shown["mask"],
            "active_sectors": int(sum(1 for s in sectors if s["sat"] or s["hue_shift"] or s["lum"]))}


def fallback_art(name: str) -> tuple[bytes, int]:
    """A generic cassette for a file without (or with a bad) CANI, picked by the profile's name (the header's, else the
    file's)"""
    g = cani.default_generic(name)
    return cani.generic(g, name, cani.guess_iso(name)), g


def load_isp(data: bytes, name: str) -> dict:
    """Parse and check an .isp file; keep its tables when it can be rendered. Errors (the file's checks, the platform, no
    ISP block) stop the render; warnings (values outside what the reference writer writes, a bad CANI, parts not marked
    present) do not."""
    key = hashlib.sha256(data).hexdigest()[:12]
    errors, warnings = [], []
    info = {"id": key, "file": name, "size": len(data), "crc": f"{zlib.crc32(data) & 0xFFFFFFFF:08X}",
            "sha256": hashlib.sha256(data).hexdigest()}
    info.update(ispfile.peek_header(data))
    info["sections"] = ispfile.list_sections(data) if info.get("version") else []
    parsed = None
    try:
        parsed = ispfile.read(data)
    except ValueError as e:
        errors.append(f"the file failed its checks: {e}")
    except struct.error as e:
        errors.append(f"the file is cut short: {e}")
    hw = ispb = None
    meta = {}
    if parsed:
        info.update(name=parsed["name"], platform=parsed["platform"], version=".".join(map(str, parsed["version"])),
                    flags=parsed["flags"])
        meta = ispfile.meta_of(parsed["sections"].get("META", b""))
        if parsed["platform"] != "OM5M2":
            errors.append(f"platform {parsed['platform']}: the model knows only OM5M2")
        ispb = parsed["sections"].get("ISPB")
        if ispb is None:
            errors.append("no ISPB section (the ISP tables)")
        elif len(ispb) != ispfile.ISPB_SIZE:
            errors.append(f"the ISPB section is {len(ispb)} bytes, expected {ispfile.ISPB_SIZE}")
            ispb = None
        if ispb is not None:
            try:
                hw = ispfile.hardware_from_ispb(ispb)
            except (ValueError, struct.error) as e:
                errors.append(f"ISPB: {e}")
        unknown = sorted(set(parsed["sections"]) - {"ISPB", "META", "CANI", "ICON"})
        if unknown:
            warnings.append("unknown sections: " + ", ".join(unknown))
    info["meta"] = meta
    domain = meta.get("domain", "lin")
    if domain not in model.DOMAINS:
        warnings.append(f"gamma domain '{domain}' is not known to the model: linear taken")
        domain = "lin"
    info["domain"] = domain
    use = ()
    if hw is not None:
        present = struct.unpack_from("<H", ispb, 2)[0]
        try:
            # the reference writer's range checks (sector fields, profile values) and the round trip
            again = ispfile.ispb_om5m2(hw, struct.unpack_from("<H", ispb, 4)[0], bool(present & ispfile.PRESENT_MATRIX))
            if again[:2] + again[4:] != ispb[:2] + ispb[4:]:
                warnings.append("the ISPB block is not reproduced byte for byte by the reference writer (reserved "
                                "fields, window widths)")
        except ValueError as e:       # rendered all the same
            warnings.append(f"ISPB outside the reference writer's ranges: {e}")
        for b, label in ((ispfile.PRESENT_GAMMA, "gamma"), (ispfile.PRESENT_TONE, "tone")):
            if not present & b:
                warnings.append(f"{label} not marked present in the file: the camera keeps its stock table, the model "
                                "takes the file's table")
        info["isp"] = summary(ispb, hw)
        use = ispfile.use_of_present(present)
    info["use"] = list(use)
    # the cassette: the file's CANI, else a generic one
    art, art_from = None, "none"
    if parsed and "CANI" in parsed["sections"]:
        try:
            info["cani"] = {**cani.parse(parsed["sections"]["CANI"]), "size": len(parsed["sections"]["CANI"])}
            art, art_from = parsed["sections"]["CANI"], "file"
        except ValueError as e:
            warnings.append(f"CANI failed its checks ({e}): a generic cassette is shown")
    if art is None:
        try:
            art, g = fallback_art(info.get("name") or Path(name).stem)
            art_from = f"generic:{g}"
            info["cani"] = {**cani.parse(art), "size": len(art), "generic": cani.GENERICS[g][1]}
        except ValueError:
            art = None
    info["art"] = art_from
    info["errors"], info["warnings"] = errors, warnings
    info["renderable"] = hw is not None and not errors
    _put(ISPS, key, {"hw": hw if info["renderable"] else None, "use": use, "domain": domain, "info": info, "art": art},
         MAX_ISPS)
    return info


def cassette_png(isp_id: str) -> bytes:
    rec = ISPS.get(isp_id)
    if rec is None or rec["art"] is None:
        raise KeyError("no cassette")
    return canister.render_png(rec["art"])


# ------------------------------------------------------------------ render
def _image(image_id):
    im = IMAGES.get(image_id)
    if im is None:
        raise KeyError("the picture is not on the server (was it restarted?): load it again")
    return im


def _isp(isp_id):
    rec = ISPS.get(isp_id)
    if rec is None:
        raise KeyError("the profile is not on the server (was it restarted?): load it again")
    return rec


def do_render(req: dict) -> dict:
    """The original (the picture without a look: sRGB of the scene) and the picture through the .isp's tables; the look's
    strength - CIEDE2000 between them over the pixels"""
    image, isp = req["image"], req.get("isp")
    ev = round(float(req.get("ev", 0)) * 4) / 4
    key = (image, isp, ev)
    if key in RENDERS:
        RENDERS.move_to_end(key)
        return {**RENDERS[key], "cached": True}
    im = _image(image)
    inv, shape = im["inv"], im["shape"]
    t0 = time.time()
    x, s = render.signal(im, ev)
    plain = render.plain(x)
    out = {"plain": render.png_url(plain, inv, shape), "w": shape[1], "h": shape[0], "ev": ev,
           "version": ispkit.__version__}
    rec = _isp(isp) if isp else None
    if rec is not None and rec["hw"] is not None:
        mm, backend = render.run_tables(s, rec["hw"], rec["domain"], rec["use"])
        de = render.strength(plain, mm)
        out.update(isp=render.png_url(mm, inv, shape), backend=backend, strength=render.stats(de[inv]))
    out["ms"] = round((time.time() - t0) * 1000)
    _put(RENDERS, key, out, MAX_RENDERS)
    return {**out, "cached": False}


def do_autoev(req: dict) -> dict:
    """The EV at which the picture through the profile is as bright as the original (mean CIE L* of a sample of 20000
    pixels; bisection over -3..3 EV to a quarter EV). A camera RAW is metered already: 0."""
    im, rec = _image(req["image"]), _isp(req["isp"])
    if im.get("raw") is not None or rec["hw"] is None:
        return {"ev": 0.0}
    flat = im["lin"][im["inv"]]
    if len(flat) > 20000:
        flat = flat[np.random.default_rng(0).choice(len(flat), 20000, replace=False)]
    want = color.lab(color.srgb_encode(np.clip(flat, 0, 1)))[:, 0].mean()
    u, inv, _ = picture.distinct(flat)
    lo, hi = -3.0, 3.0
    for _ in range(14):
        if round(lo * 4) == round(hi * 4):
            break
        mid = (lo + hi) / 2
        mm, _ = render.run_tables(camera.sensor(camera.table_of_scene(u * 2.0 ** mid)), rec["hw"], rec["domain"],
                                  rec["use"])
        got = color.lab(mm)[inv, 0].mean()
        lo, hi = (mid, hi) if got < want else (lo, mid)
    return {"ev": round((lo + hi) / 2 * 4) / 4}


# ------------------------------------------------------------------ HTTP
class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def send(self, code, body, ctype="application/json; charset=utf-8", extra=None):
        data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        try:
            if u.path in ("/", "/index.html"):
                return self.send(200, (STATIC / "index.html").read_bytes(), "text/html; charset=utf-8")
            if u.path.startswith("/static/"):
                p = (STATIC / u.path[len("/static/"):]).resolve()
                if STATIC in p.parents and p.is_file():
                    ctype = {".js": "text/javascript", ".css": "text/css", ".svg": "image/svg+xml"}.get(p.suffix,
                                                                                                        "application/octet-stream")
                    return self.send(200, p.read_bytes(), ctype + "; charset=utf-8")
            if u.path == "/api/version":
                return self.send(200, {"version": ispkit.__version__, "backend": render.backend_name(),
                                       "threads": render.THREADS, "max_image_bytes": MAX_IMAGE_BYTES,
                                       "max_isp_bytes": MAX_ISP_BYTES})
            if u.path == "/api/cassette":
                with LOCK:
                    return self.send(200, cassette_png(q.get("isp", [""])[0]), "image/png")
        except KeyError as e:
            return self.send(404, {"error": str(e)})
        self.send(404, {"error": "not found"})

    def do_POST(self):
        limit = {"/api/image": MAX_IMAGE_BYTES, "/api/isp": MAX_ISP_BYTES}.get(self.path, 64 * 1024)
        try:
            n = int(self.headers.get("Content-Length", 0))
        except ValueError:
            n = -1
        if not 0 <= n <= limit:
            # the body is read and dropped in 1 MB pieces (constant memory), so the client gets the answer rather than
            # a reset connection; a body over DRAIN_MAX is not read at all
            left = n if 0 < n <= DRAIN_MAX else 0
            while left > 0:
                got = self.rfile.read(min(left, 1 << 20))
                if not got:
                    break
                left -= len(got)
            self.close_connection = True
            return self.send(413, {"error": f"the upload is larger than {limit // (1024 * 1024) or 1} MB"
                                            if n > limit else "no valid Content-Length"})
        body = self.rfile.read(n)
        try:
            with LOCK:
                if self.path == "/api/image":
                    out = load_image(body, unquote(self.headers.get("X-Name", "image")), self.headers.get("X-Space", "auto"))
                elif self.path == "/api/isp":
                    out = load_isp(body, unquote(self.headers.get("X-Name", "profile.isp")))
                elif self.path == "/api/render":
                    out = do_render(json.loads(body))
                elif self.path == "/api/autoev":
                    out = do_autoev(json.loads(body))
                else:
                    return self.send(404, {"error": "not found"})
            self.send(200, out)
        except Exception as e:     # the page shows the message; a trace only for what is not the user's mistake
            if not isinstance(e, (KeyError, ValueError)):
                traceback.print_exc()
            msg = e.args[0] if isinstance(e, KeyError) and e.args and isinstance(e.args[0], str) else f"{type(e).__name__}: {e}"
            self.send(400, {"error": msg})


def serve(port=PORT):
    """The server object (not started), bound to 127.0.0.1: for the tests and main()"""
    return ThreadingHTTPServer((HOST, port), Handler)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="isp_player",
                                 description="ISP player: a picture through an .isp profile, as the OM-5 Mark II colour "
                                             "path model renders it (a local web page)")
    ap.add_argument("--port", type=int, default=PORT, help=f"port on 127.0.0.1 (default {PORT}; 0 - any free port)")
    ap.add_argument("--open", action="store_true", help="open the page in the default browser")
    a = ap.parse_args(argv)
    try:
        srv = serve(a.port)
    except OSError as e:
        print(f"ISP player: cannot listen on {HOST}:{a.port} ({e}). Is it already running? Try --port with another "
              "number.", file=sys.stderr)
        return 1
    if gpu.available():
        threading.Thread(target=gpu.warm, daemon=True).start()
    url = f"http://{HOST}:{srv.server_address[1]}/"
    print(f"ISP player: {url}  (ispkit {ispkit.__version__}, "
          f"{'GPU' if gpu.available() else 'CPU'}, {render.THREADS} render threads; Ctrl+C to stop)", flush=True)
    if a.open:
        webbrowser.open(url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0
