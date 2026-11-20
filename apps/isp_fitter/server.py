"""ISP fitter: a local web page - drop a look (a 3D LUT in a .cube file) and a picture, fit the OM-5 Mark II colour tables
to the look, compare the look with its copy through the camera's colour path, set up the cassette the camera shows and
download the profile as an .isp file for the FILMLOOK module.

    python OMKit/apps/isp_fitter/run.py [--port 8791] [--open] [--prior FILE.npz] [--cache DIR | --no-cache]

The fit belongs to the look, not to the picture (as the camera's tables do): it runs on a training set of colours
(ispkit/train.py) and the picture only shows the result. A fit runs on a worker thread, one at a time; the page polls
its progress. Fits are kept in memory and in a cache folder (one small JSON file per look, options, fitter version,
prior and backend; default: the user's cache folder, see default_cache()), so a look fitted once opens at once.

Local use only: the server listens on 127.0.0.1. Pictures and looks are kept in memory only; the cache holds the fitted
tables and the accuracy numbers, nothing of the pictures."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import sys
import threading
import time
import traceback
import uuid
import webbrowser
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

import ispkit                                    # first: it sets the BLAS threads before numpy loads
import numpy as np

from ispkit import backend, camera, cani, canister, color, cube, gpu, ispfile, model, picture, prior, render, train

HERE = Path(__file__).resolve().parent
STATIC = HERE / "static"
HOST = "127.0.0.1"
PORT = 8791

MAX_IMAGE_BYTES = 200 * 1024 * 1024      # a picture upload larger than this is refused (a camera RAW is about 20 MB)
MAX_CUBE_BYTES = 64 * 1024 * 1024        # a .cube of 129^3 rows is about 50 MB of text
DRAIN_MAX = 2 * MAX_IMAGE_BYTES          # an upload over a limit is read and dropped up to this size (see do_POST)
MAX_IMAGES, MAX_LOOKS, MAX_FITS, MAX_VIEWS, MAX_JOBS = 4, 16, 32, 6, 64
NAME_BYTES = 31                          # the profile's name in the .isp header: at most 31 bytes of UTF-8

LOCK = threading.Lock()                  # the state below; a fit runs outside it, on WORKER
IMAGES: OrderedDict = OrderedDict()      # id -> picture record (picture.load_image)
LOOKS: OrderedDict = OrderedDict()       # id -> {"text", "name", "sha", "enc_in", "enc_out", "n", "title"}
FITS: OrderedDict = OrderedDict()        # fit key -> {"hw", "use", "look", "enc", "public"}
VIEWS: OrderedDict = OrderedDict()       # (look, enc, image, ev) -> the look's frame (look_view)
JOBS: OrderedDict = OrderedDict()        # job id -> progress and result of a fit
WORKER = ThreadPoolExecutor(1)           # fits run one at a time
CONFIG = {"prior": None, "cache": None}  # set by main() / configure()


def _put(cache: OrderedDict, key, value, limit):
    cache[key] = value
    cache.move_to_end(key)
    while len(cache) > limit:
        cache.popitem(last=False)


def default_cache() -> Path:
    """The fit cache's folder: %LOCALAPPDATA%\\OMKit\\isp_fitter on Windows, ~/Library/Caches/OMKit/isp_fitter on macOS,
    $XDG_CACHE_HOME (else ~/.cache)/OMKit/isp_fitter elsewhere."""
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData" / "Local")
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Caches"
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache")
    return base / "OMKit" / "isp_fitter"


def configure(prior_file=None, cache=None):
    """The prior to fit with (None: the bundled one) and the cache folder (None: no cache on disk)."""
    CONFIG["prior"] = prior.load(prior_file)
    CONFIG["cache"] = Path(cache) if cache else None
    with LOCK:
        FITS.clear()
        VIEWS.clear()


def _prior():
    if CONFIG["prior"] is None:
        CONFIG["prior"] = prior.load()
    return CONFIG["prior"]


# ------------------------------------------------------------------ the picture
def load_image(data: bytes, name: str, space: str = "auto") -> dict:
    rec, info = picture.load_image(data, name, space)
    _put(IMAGES, info["id"], rec, MAX_IMAGES)
    return info


def _image(image_id):
    im = IMAGES.get(image_id)
    if im is None:
        raise KeyError("the picture is not on the server (was it restarted?): load it again")
    return im


# ------------------------------------------------------------------ the look
def encodings() -> dict:
    return {"enc_in": {k: v[0] for k, v in cube.CUBE_IN.items()}, "enc_out": {k: v[0] for k, v in cube.CUBE_OUT.items()}}


def load_look(data: bytes, name: str) -> dict:
    """A .cube file: parsed and checked (cube.CubeTarget); the encodings guessed from its text."""
    if not name.lower().endswith(".cube"):
        raise ValueError("a look is a .cube file (a 3D LUT)")
    text = data.decode("utf-8", errors="replace")
    t = cube.CubeTarget(text=text, name=Path(name).stem)
    sha = hashlib.sha256(data).hexdigest()
    key = sha[:12]
    _put(LOOKS, key, {"text": text, "name": Path(name).stem, "sha": sha, "enc_in": t.enc_in, "enc_out": t.enc_out,
                      "n": t.n, "title": t.title}, MAX_LOOKS)
    return {"id": key, "name": Path(name).stem, "file": name, "title": t.title, "size": t.n, "enc_in": t.enc_in,
            "enc_out": t.enc_out, "options": encodings()}


def _look(look_id):
    lk = LOOKS.get(look_id)
    if lk is None:
        raise KeyError("the look is not on the server (was it restarted?): load the .cube again")
    return lk


_TARGETS: OrderedDict = OrderedDict()


def target_of(look_id, enc_in, enc_out) -> cube.CubeTarget:
    """The look with these encodings (kept: its camera target is built once)."""
    lk = _look(look_id)
    k = (look_id, enc_in, enc_out)
    if k not in _TARGETS:
        _put(_TARGETS, k, cube.CubeTarget(text=lk["text"], enc_in=enc_in, enc_out=enc_out, name=lk["name"]), MAX_LOOKS)
    return _TARGETS[k]


# ------------------------------------------------------------------ the fit
def recipes() -> dict:
    return {"default": train.DEFAULT, "matrix": train.MATRIX,
            "recipes": [{"id": k, "label": v[0], "desc": v[1]} for k, v in train.RECIPES.items()]}


def _options(req: dict) -> dict:
    lk = _look(req["look"])
    variant = req.get("variant") or train.DEFAULT
    if variant not in train.RECIPES:
        raise ValueError(f"unknown recipe {variant!r}")
    enc_in, enc_out = req.get("enc_in") or lk["enc_in"], req.get("enc_out") or lk["enc_out"]
    if enc_in not in cube.CUBE_IN or enc_out not in cube.CUBE_OUT:
        raise ValueError("unknown .cube encoding")
    return {"look": req["look"], "variant": variant, "matrix": bool(req.get("matrix", train.MATRIX)),
            "quick": bool(req.get("quick", False)), "enc_in": enc_in, "enc_out": enc_out}


def fit_key(opt: dict) -> str:
    """A fit's key: the look's content, the options, the fitter's version, the prior and the dense backend (the GPU's
    float32 normal equations give slightly other tables than the CPU's float64)."""
    lk = _look(opt["look"])
    parts = [lk["sha"], opt["enc_in"], opt["enc_out"], opt["variant"], opt["matrix"], opt["quick"], train.version(),
             _prior().digest, backend.B.name]
    return hashlib.sha256(json.dumps(parts).encode()).hexdigest()[:16]


def _stats(de):
    return render.stats(de)


def _cache_file(key):
    return CONFIG["cache"] / "fits" / f"{key}.json" if CONFIG["cache"] else None


def _from_cache(key):
    p = _cache_file(key)
    if p is None:
        return None
    try:
        kept = json.loads(p.read_text(encoding="utf-8"))
        hw = train.hardware_of_dict(kept["hardware"])
        ispfile.ispb_om5m2(hw)                 # the writer's range checks: a damaged file is fitted again
        return hw, kept["public"]
    except (OSError, ValueError, KeyError, TypeError):
        return None


def _to_cache(key, hw, public):
    p = _cache_file(key)
    if p is None:
        return
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_name(f"{p.stem}.{os.getpid()}.{threading.get_ident()}.tmp")
        tmp.write_text(json.dumps({"format": 1, "public": public, "hardware": train.hardware_dict(hw)}), encoding="utf-8")
        os.replace(tmp, p)
    except OSError:
        traceback.print_exc()                  # the fit still works, it is only not kept


def _store(key, opt, hw, public):
    _put(FITS, key, {"hw": hw, "use": train.use_of(opt["variant"], opt["matrix"]), "look": opt["look"],
                     "enc": (opt["enc_in"], opt["enc_out"]), "public": public}, MAX_FITS)


def run_fit(opt: dict, job: dict | None = None):
    """Fit a look (the worker's task): the tables, the accuracy on ColorChecker and the RGB cube. job (a dict) gets the
    progress: the stage and the iteration of the Levenberg-Marquardt loop (fit.fit logs them)."""
    with LOCK:                               # the look's records are shared with the request threads
        t = target_of(opt["look"], opt["enc_in"], opt["enc_out"])
        pr = _prior()
    info = job if job is not None else {}

    def log(line):
        s = line.strip()
        if s.startswith("iter "):
            it = int(s.split()[1].rstrip(":"))
            if it == 0:
                info["stage"] = info.get("stage", 0) + 1
            info["iter"] = it + 1

    hw = train.fit_look(t, opt["variant"], train.DOMAIN, opt["quick"], log, opt["matrix"], pr, info)
    use = train.use_of(opt["variant"], opt["matrix"])
    acc = train.accuracy(t, hw, use)
    public = {"look": t.name, "desc": t.desc, "variant": opt["variant"],
              "recipe": train.RECIPES[opt["variant"]][0], "matrix": opt["matrix"], "quick": opt["quick"],
              "enc_in": opt["enc_in"], "enc_out": opt["enc_out"], "mono": info.get("mono", ""),
              "colorchecker": _stats(acc["colorchecker"]), "cube": _stats(acc["cube"]),
              "prior": "bundled" if pr.source == str(prior.DEFAULT) else Path(pr.source).name}
    return hw, public


def _job_task(job_id, key, opt):
    job = JOBS[job_id]
    try:
        hw, public = run_fit(opt, job)
        public = {**public, "fit": key}
        _to_cache(key, hw, public)
        with LOCK:
            _store(key, opt, hw, public)
        job.update(state="done", result={**public, "cached": False})
    except Exception as e:      # noqa: BLE001 - the page shows the message
        traceback.print_exc()
        job.update(state="error", error=f"{type(e).__name__}: {e}")


def start_fit(req: dict) -> dict:
    """A fit for these options: done at once from memory or the cache folder, else a job on the worker (the page polls
    /api/job). The same fit asked again while it runs joins the running job."""
    opt = _options(req)
    key = fit_key(opt)
    name = _look(opt["look"])["name"]       # a kept fit shows the look's current file name
    if key in FITS:
        FITS.move_to_end(key)
        return {"state": "done", "result": {**FITS[key]["public"], "look": name, "cached": True}}
    kept = _from_cache(key)
    if kept is not None:
        hw, public = kept
        public = {**public, "fit": key, "look": name}
        _store(key, opt, hw, public)
        return {"state": "done", "result": {**public, "cached": True}}
    for jid, j in JOBS.items():
        if j["key"] == key and j["state"] == "running":
            return {"state": "running", "job": jid}
    jid = uuid.uuid4().hex[:12]
    _put(JOBS, jid, {"key": key, "state": "running", "stage": 0, "stages": train.plan(opt["variant"], opt["matrix"]),
                     "iter": 0, "iters": 12 if opt["quick"] else 40, "started": time.time()}, MAX_JOBS)
    WORKER.submit(_job_task, jid, key, opt)
    return {"state": "running", "job": jid}


def job_status(job_id) -> dict:
    j = JOBS.get(job_id)
    if j is None:
        raise KeyError("no such fit job (was the server restarted?): fit again")
    out = {"state": j["state"], "stage": j.get("stage", 0), "stages": j.get("stages", 1), "iter": j.get("iter", 0),
           "iters": j.get("iters", 40), "seconds": round(time.time() - j["started"], 1), "mono": j.get("mono", "")}
    if j["state"] == "done":
        out["result"] = j["result"]
    elif j["state"] == "error":
        out["error"] = j["error"]
    return out


def _fit(fit_id):
    f = FITS.get(fit_id)
    if f is None:
        raise KeyError("the fit is not on the server (was it restarted?): fit again")
    return f


# ------------------------------------------------------------------ render
def heat(de, scale=10.0):
    """The error map's colours: black 0, red 5, yellow 10 and above (CIEDE2000)."""
    t = np.clip(de / scale, 0, 1)
    return np.stack([np.clip(t * 2, 0, 1), np.clip(t * 2 - 1, 0, 1), np.zeros_like(t)], axis=-1)


def look_view(f, image_id, ev, x, s):
    """The look and the original at this exposure: they do not depend on the recipe, so they are kept per (look,
    encodings, picture, EV) for the last few."""
    key = (f["look"], f["enc"], image_id, ev)
    if key in VIEWS:
        VIEWS.move_to_end(key)
        return VIEWS[key]
    im = IMAGES[image_id]
    inv, shape = im["inv"], im["shape"]
    ct = train.camera_target(target_of(f["look"], *f["enc"]))
    tt = render.rows(ct, s)
    lab_tt = render.rows(color.lab, tt)
    plain = render.plain(x)
    de_look = render.rows(color.de2000, render.rows(color.lab, plain), lab_tt)
    v = {"lab": lab_tt, "plain": render.png_url(plain, inv, shape), "look": render.png_url(tt, inv, shape),
         "strength": render.stats(de_look[inv])}
    _put(VIEWS, key, v, MAX_VIEWS)
    return v


def do_render(req: dict) -> dict:
    """The original, the look, the look's copy through the fitted tables and the map of their difference (CIEDE2000),
    with the numbers: the copy's error on this frame and the look's strength (look against the original)."""
    f = _fit(req["fit"])
    im = _image(req["image"])
    inv, shape = im["inv"], im["shape"]
    ev = round(float(req.get("ev", 0)) * 4) / 4
    x, s = render.signal(im, ev)
    view = look_view(f, req["image"], ev, x, s)
    mm, used = render.run_tables(s, f["hw"], train.DOMAIN, f["use"])
    de_u = render.rows(color.de2000, view["lab"], render.rows(color.lab, mm))
    return {"plain": view["plain"], "look": view["look"], "isp": render.png_url(mm, inv, shape),
            "heat": render.png_url(heat(de_u), inv, shape), "image_de": render.stats(de_u[inv]),
            "look_strength": view["strength"], "w": shape[1], "h": shape[0], "ev": ev, "backend": used}


def do_autoev(req: dict) -> dict:
    """The EV at which the look is as bright as the picture itself (mean CIE L* of a sample of 20000 pixels; bisection
    over -3..3 EV to a quarter EV): a developed picture already has a tone curve, and a look adds its own. A camera RAW
    is metered already: 0."""
    f = _fit(req["fit"])
    im = _image(req["image"])
    if im.get("raw") is not None:
        return {"ev": 0.0}
    flat = im["lin"][im["inv"]]
    if len(flat) > 20000:
        flat = flat[np.random.default_rng(0).choice(len(flat), 20000, replace=False)]
    want = color.lab(color.srgb_encode(np.clip(flat, 0, 1)))[:, 0].mean()
    u, inv, _ = picture.distinct(flat)
    ct = train.camera_target(target_of(f["look"], *f["enc"]))
    lo, hi = -3.0, 3.0
    for _ in range(14):
        if round(lo * 4) == round(hi * 4):
            break
        mid = (lo + hi) / 2
        got = color.lab(render.rows(ct, camera.table_of_scene(u * 2.0 ** mid)))[inv, 0].mean()
        lo, hi = (mid, hi) if got < want else (lo, mid)
    return {"ev": round((lo + hi) / 2 * 4) / 4}


# ------------------------------------------------------------------ the cassette and the .isp
def cassettes(look_name: str) -> dict:
    """The generic cassettes (canister.GENERICS) with their own brand and process lines, and the guesses for a look's
    name: the picture (cani.default_generic) and the ISO (cani.guess_iso)."""
    gens = [{"id": i, "key": k, "label": t, "brand": canister.GENERICS[i].get("brand", ""),
             "proc": canister.GENERICS[i].get("proc", "")} for i, (k, t) in enumerate(cani.GENERICS)]
    return {"generics": gens, "default": cani.default_generic(look_name), "iso": cani.guess_iso(look_name),
            "full_font": canister.full_font_chars(),
            "limits": {"iso": canister.HEAD_ISO, "name": canister.HEAD_NAME, "brand": canister.HEAD_BRAND,
                       "profile": NAME_BYTES}}


def cassette(req: dict):
    """(section or None, the page's report): the generic picture with the user's texts, checked (canister.make_generic)."""
    blob, rep, errors, warnings = canister.make_generic(req.get("gen", 0), req.get("name", ""), req.get("iso", ""),
                                                        req.get("brand", ""), req.get("proc", ""))
    return blob, {**rep, "errors": errors, "warnings": warnings}


def do_cassette(req: dict) -> dict:
    blob, rep = cassette(req)
    if blob is not None:
        rep["png"] = "data:image/png;base64," + base64.b64encode(canister.render_png(blob)).decode()
    return rep


def check_name(name: str):
    """The profile's name: (errors, warnings)."""
    errors, warnings = [], []
    if not name.strip():
        errors.append("the profile's name is empty")
    n = len(name.encode("utf-8"))
    if n > NAME_BYTES:
        errors.append(f"the profile's name is {n} bytes in UTF-8, at most {NAME_BYTES}")
    if any(not 32 <= ord(c) < 127 for c in name):
        warnings.append("the profile's name has characters outside ASCII: the camera's font may not show them")
    return errors, warnings


def make_isp(req: dict) -> tuple[bytes, str]:
    """The .isp file of a fit: the tables (ISPB), META (the fitter's version, the recipe, the matrix option, the gamma
    domain, the cassette's picture) and the cassette (CANI). Returns (bytes, file name)."""
    f = _fit(req["fit"])
    name = str(req.get("name", ""))
    errors, _ = check_name(name)
    blob, rep = cassette(req.get("cassette") or {})
    errors += rep["errors"]
    if errors:
        raise ValueError("; ".join(errors))
    p = f["public"]
    meta = {"fitter": f"isp_fitter {train.version()}", "variant": p["variant"], "matrix": p["matrix"],
            "domain": train.DOMAIN, "art": f"generic:{rep['index']}"}
    data = ispfile.write(name, "OM5M2", ispfile.ispb_om5m2(f["hw"]), meta=meta, extra=[(b"CANI", blob)])
    return data, file_name(name)


def file_name(name: str) -> str:
    """A download name from the profile's name: ASCII letters, digits, - and _, at most 35 characters, then .isp (the
    camera lists names of at most 39 characters with the extension)."""
    stem = "".join(c if c.isascii() and (c.isalnum() or c in "-_") else "_" for c in name).strip("_")[:35]
    return (stem or "profile") + ".isp"


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
                pr = _prior()
                return self.send(200, {"version": ispkit.__version__, "backend": "gpu" if backend.B.name == "gpu" else "cpu",
                                       "render": render.backend_name(), "threads": render.THREADS,
                                       "prior": "bundled" if pr.source == str(prior.DEFAULT) else Path(pr.source).name,
                                       "max_image_bytes": MAX_IMAGE_BYTES, "max_cube_bytes": MAX_CUBE_BYTES})
            if u.path == "/api/recipes":
                return self.send(200, recipes())
            if u.path == "/api/cassettes":
                return self.send(200, cassettes(q.get("look", [""])[0]))
            if u.path == "/api/job":
                with LOCK:
                    return self.send(200, job_status(q.get("id", [""])[0]))
        except KeyError as e:
            return self.send(404, {"error": e.args[0] if e.args else "not found"})
        self.send(404, {"error": "not found"})

    def do_POST(self):
        limit = {"/api/image": MAX_IMAGE_BYTES, "/api/look": MAX_CUBE_BYTES}.get(self.path, 64 * 1024)
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
            if self.path == "/api/isp":
                with LOCK:
                    data, fname = make_isp(json.loads(body))
                return self.send(200, data, "application/octet-stream",
                                 extra={"Content-Disposition": f'attachment; filename="{fname}"'})
            with LOCK:
                if self.path == "/api/image":
                    out = load_image(body, unquote(self.headers.get("X-Name", "image")), self.headers.get("X-Space", "auto"))
                elif self.path == "/api/look":
                    out = load_look(body, unquote(self.headers.get("X-Name", "look.cube")))
                elif self.path == "/api/fit":
                    out = start_fit(json.loads(body))
                elif self.path == "/api/render":
                    out = do_render(json.loads(body))
                elif self.path == "/api/autoev":
                    out = do_autoev(json.loads(body))
                elif self.path == "/api/cassette":
                    out = do_cassette(json.loads(body))
                else:
                    return self.send(404, {"error": "not found"})
            self.send(200, out)
        except Exception as e:     # the page shows the message; a trace only for what is not the user's mistake
            if not isinstance(e, (KeyError, ValueError)):
                traceback.print_exc()
            msg = e.args[0] if isinstance(e, KeyError) and e.args and isinstance(e.args[0], str) else f"{type(e).__name__}: {e}"
            if isinstance(e, ValueError):
                msg = str(e)
            self.send(400, {"error": msg})


def serve(port=PORT):
    """The server object (not started), bound to 127.0.0.1: for the tests and main()"""
    return ThreadingHTTPServer((HOST, port), Handler)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="isp_fitter",
                                 description="ISP fitter: fit the OM-5 Mark II colour tables to a .cube look and save an "
                                             ".isp profile (a local web page)")
    ap.add_argument("--port", type=int, default=PORT, help=f"port on 127.0.0.1 (default {PORT}; 0 - any free port)")
    ap.add_argument("--open", action="store_true", help="open the page in the default browser")
    ap.add_argument("--prior", metavar="FILE.npz", help="the photo colour prior to fit with (build_prior.py makes one); "
                                                        "default: the bundled one")
    ap.add_argument("--cache", metavar="DIR", help=f"the folder fits are kept in (default: {default_cache()})")
    ap.add_argument("--no-cache", action="store_true", help="keep fits in memory only")
    a = ap.parse_args(argv)
    try:
        configure(a.prior, None if a.no_cache else (a.cache or default_cache()))
    except ValueError as e:
        print(f"ISP fitter: {e}", file=sys.stderr)
        return 1
    try:
        srv = serve(a.port)
    except OSError as e:
        print(f"ISP fitter: cannot listen on {HOST}:{a.port} ({e}). Is it already running? Try --port with another "
              "number.", file=sys.stderr)
        return 1
    if gpu.available():
        threading.Thread(target=gpu.warm, daemon=True).start()
    url = f"http://{HOST}:{srv.server_address[1]}/"
    pr = _prior()
    print(f"ISP fitter: {url}  (ispkit {ispkit.__version__}, fits on the {'GPU' if backend.B.name == 'gpu' else 'CPU'}, "
          f"prior {'bundled' if pr.source == str(prior.DEFAULT) else pr.source}, cache "
          f"{CONFIG['cache'] or 'off'}; Ctrl+C to stop)", flush=True)
    if a.open:
        webbrowser.open(url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0
