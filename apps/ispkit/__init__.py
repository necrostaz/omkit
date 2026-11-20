"""ispkit: the shared library of the OMKit picture apps - a numerical model of the OM-5 Mark II colour path and the files
it reads and writes.

    color       transfer functions, primaries, CIELAB and CIEDE2000
    camera      the camera contract: what signal the replaced tables get and what scene it stands for; RAW (.ORF) loading
    model       the colour path model: matrix, gamma, YCbCr with the saturation factor, tone, the 12 sectors; the quantised
                tables (Hardware) and their render (run_hardware); the analytic Jacobian for fitting
    backend     the dense arithmetic backend: torch on a CUDA GPU when present, else numpy
    gpu         run_hardware on the GPU (torch, float64), checked against model.run_hardware per table set
    ispfile     the .isp profile file (OMKit/docs/formats/isp.md): reader, writer, the OM5M2 ISP block
    cani        the CANI cassette picture section: checks, generic cassette choice
    canister    the cassette generator (generic pictures with a name and ISO drawn in), the camera-like renderer and
                sheets of cassettes to choose from
    editors     what the camera's FILMLOOK editors show for a preset (sector hues and colours, the tone curve)
    picture     loading a picture: the camera's RAW, or a developed picture through its colour space; the camera model
                an ORF names
    render      the render of a picture through a table set: per distinct colour, on a small thread pool, GPU or CPU
    cube        a look given as a 3D LUT in a .cube file, with its input and output encodings
    fit         the Levenberg-Marquardt fit of the model to a look (CIEDE2000 cost, analytic Jacobian)
    train       the fitting recipe: the training set, the recipes and their stages, black-and-white looks, the result
    prior       the photo colour prior of the training set: loading (data/prior.npz, bundled) and building one

Required: numpy, Pillow; rawpy for the camera's RAW files. Optional: torch with CUDA (the render on the GPU - the same
8-bit result as on the CPU, see gpu.py; the fit's dense arithmetic in float32, see backend.py)."""
from __future__ import annotations

import os
import sys

__version__ = "1.0"

# One BLAS thread, set before numpy loads its OpenBLAS (an explicit setting is kept). The render runs numpy on several
# threads of its own (render.rows), and OpenBLAS 0.3.30 (numpy 2.3) with threads of its own crashes when two threads
# multiply at once (an access violation; 0.3.34 of numpy 2.5 does not). render.THREADS checks this flag.
BLAS_FRESH = "numpy" not in sys.modules
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
