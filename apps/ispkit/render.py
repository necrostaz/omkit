"""The render of a picture (picture.py's record) through a table set, as the camera would make the frame.

The picture becomes the camera's table signal (camera.py): a RAW straight from its sensor values (clipped at the sensor's
white after the white balance, as the camera does), any other picture as a metered scene within what the sensor
delivers. The tables run on it with model.run_hardware - on the GPU when torch with CUDA is there and its copy of the model
renders these tables as model.py does (gpu.py), else on the CPU in chunks of CHUNK rows on THREADS threads.

Everything works per distinct colour of the picture; inv (the record's) gives the pixels. Chunks of this size stay in the
cache: one thread on 64k chunks runs the tables 2.4 times faster than on the whole picture; above 4 threads the GIL eats
the gain. The result does not depend on the chunks or the threads: every function here is per colour."""
from __future__ import annotations

import base64
import io
import os
from concurrent.futures import ThreadPoolExecutor

import numpy as np                                # ispkit/__init__ has set the BLAS threads before this
from PIL import Image

from . import BLAS_FRESH, camera, color, gpu, model

CHUNK = 1 << 16


def _threads():
    """The render's threads: ISPKIT_THREADS, else up to 4 - when the BLAS is safe on several threads (ispkit/__init__)."""
    if "ISPKIT_THREADS" in os.environ:
        return max(1, int(os.environ["ISPKIT_THREADS"]))
    safe = BLAS_FRESH and os.environ.get("OPENBLAS_NUM_THREADS") == "1"
    if not safe:
        try:
            ver = np.show_config(mode="dicts")["Build Dependencies"]["blas"]["version"]
            safe = tuple(int(v) for v in ver.split(".")[:3]) >= (0, 3, 34)
        except Exception:        # an unknown build: no threads
            safe = False
    return min(4, os.cpu_count() or 1) if safe else 1


THREADS = _threads()
POOL = ThreadPoolExecutor(THREADS) if THREADS > 1 else None


def rows(fn, *arrays, parallel=True):
    """fn of arrays with rows in step (a per-colour function: row i of the result depends on row i of the inputs only)
    in chunks of CHUNK rows, on the pool when parallel - the result is the same as fn(*arrays)."""
    n = len(arrays[0])
    if n <= CHUNK:
        return fn(*arrays)
    parts = [tuple(a[i:i + CHUNK] for a in arrays) for i in range(0, n, CHUNK)]
    if POOL is None or not parallel:
        return np.concatenate([fn(*p) for p in parts])
    return np.concatenate(list(POOL.map(lambda p: fn(*p), parts)))


def png_bytes(rgb, inv=None, shape=None) -> bytes:
    """PNG of display RGB in 0..1: (h, w, 3), or per distinct colour (U, 3) with the pixels' index inv (quantised to 8
    bit first: the same image, U instead of h * w roundings)."""
    q = (np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8)
    if inv is not None:
        q = q[inv].reshape(shape)
    buf = io.BytesIO()
    Image.fromarray(q).save(buf, "PNG", compress_level=1)
    return buf.getvalue()


def png_url(rgb, inv=None, shape=None) -> str:
    """png_bytes as a data URL."""
    return "data:image/png;base64," + base64.b64encode(png_bytes(rgb, inv, shape)).decode()


def stats(de):
    return {"mean": round(float(de.mean()), 2), "p95": round(float(np.percentile(de, 95)), 2), "max": round(float(de.max()), 2)}


def signal(rec: dict, ev: float):
    """The picture at this exposure: the linear scene x and the table signal s, per distinct colour (a RAW straight from
    its sensor values clipped at the sensor's white; a developed picture as a metered scene)."""
    gain = 2.0 ** ev
    x = np.maximum(rec["lin"] * gain, 0)          # a RAW keeps its headroom above the scene's 1
    if rec.get("raw") is not None:
        s = camera.table_of_raw(np.minimum(rec["raw"] * gain, 1.0))
    else:
        s = camera.sensor(camera.table_of_scene(x))
    return x, s


def backend_name() -> str:
    return "gpu" if gpu.available() else "cpu"


def run_tables(s, hw: model.Hardware, domain, use, use_gpu=True):
    """The tables on the table signal s: the GPU copy when it is there and checked, else model.run_hardware in row chunks
    on the pool. Returns (display RGB with 8-bit values, "gpu" or "cpu")."""
    mm = gpu.run_hardware(s, hw, domain, use) if use_gpu and gpu.available() else None
    if mm is not None:
        return mm, "gpu"
    return rows(lambda a: model.run_hardware(a, hw, domain, use), s), "cpu"


def plain(x):
    """The picture without a look: sRGB of the scene (the camera's frame without tables, as a reference)."""
    return color.srgb_encode(np.clip(x, 0, 1))


def strength(plain_rgb, look_rgb):
    """CIEDE2000 between two renders per distinct colour."""
    return rows(color.de2000, rows(color.lab, plain_rgb), rows(color.lab, look_rgb))
