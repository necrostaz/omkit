"""The camera contract (OM-5 Mark II, firmware 1.1): what signal the replaced tables get and what scene it stands for, in
one place (OMKit/docs/platform/om5m2/color-pipeline.md).

The tables (gamma R/G/B first) get the camera's linear signal after its own matrix S+80, which core does not replace:

    table signal = S+80 @ (white-balanced RAW, 1 = the sensor's white) / FULL_SCALE

- the white-balanced RAW is clipped at the sensor's white (1) in every channel: blown highlights stay neutral (measured
  on the camera: a shot with thousands of blown blocks has a* 0.0, b* 0.0 in its JPEG; without the clip the model gave a
  colour cast of dE 8.5);
- the look's matrix F does not come after S+80: core writes one matrix round(F * S+80) (model.run_hardware), so the
  table signal is not clipped before F - only the gamma's input is (sensor() gives what the sensor can deliver);
- S+80 is one fixed matrix for still jobs in Natural, whatever the white balance (core's log on the camera);
- FULL_SCALE: the gamma's last node is 1.06 of the RAW white (camera experiment);
- GREY: a correctly metered middle grey lands at 0.075 of the RAW white (stock Natural renders it L* 50: five stock
  shots at ISO 200-400, 0.0742-0.0758);
- PRIMARIES: S+80's output is not BT.709. P = S+80 @ M_DNG^-1 maps linear BT.709 to S+80's output (M_DNG: the camera's
  DNG colour matrix, rows normalised to keep white) - an estimate until a ColorChecker calibration of our own.

A scene is linear BT.709 with middle grey at 0.18; table_of_scene() puts it where a correctly metered shot would.
CameraTarget runs a look defined on the scene on the table signal (what a fitter copies)."""
from __future__ import annotations

import numpy as np

from . import color

S80 = np.array([[183, -40, -15], [-25, 180, -27], [1, -32, 159]]) / 128.0
FULL_SCALE = 1.06
GREY = 0.075
# The camera's colour matrix as raw readers report it (rgb_xyz_matrix x 10000; the same in every ORF of the camera)
_CAM_XYZ = np.array([[11896, -5110, -1076], [-3181, 11378, 2048], [-519, 1224, 5166]]) / 10000.0


def _dng_709():
    cam_rgb = _CAM_XYZ @ color.M709
    cam_rgb /= cam_rgb.sum(axis=1, keepdims=True)
    return np.linalg.inv(cam_rgb)


M_DNG = _dng_709()                          # white-balanced camera RGB -> linear BT.709
P = S80 @ np.linalg.inv(M_DNG)              # linear BT.709 -> S+80's output
P_INV = np.linalg.inv(P)
S80_INV = np.linalg.inv(S80)
EXPOSURE_EV = float(np.log2(0.18 / GREY))


def raw_of_orf(data: bytes):
    """An ORF of the camera -> its white-balanced RAW (h, w, 3), 1 = the sensor's white, at half size (a 2x2 Bayer
    block a pixel), turned as the camera held it; the white balance is the shot's own (as the camera applies it before
    S+80), every channel then clipped at the sensor's white, as the camera does. Needs rawpy."""
    import io as _io

    import rawpy
    with rawpy.imread(_io.BytesIO(data)) as r:
        raw = r.raw_image_visible.astype(np.float64)
        cols = r.raw_colors_visible
        bl = np.array(r.black_level_per_channel, float)
        wl = float(r.white_level)
        wb = np.array(r.camera_whitebalance[:3], float)
        flip = r.sizes.flip
    wb = wb / wb[1]
    h, w = raw.shape[0] // 2 * 2, raw.shape[1] // 2 * 2
    out = np.zeros((h // 2, w // 2, 3))
    cnt = np.zeros(3)
    for dy in (0, 1):
        for dx in (0, 1):
            c = int(cols[dy, dx])
            ch = 1 if c in (1, 3) else (0 if c == 0 else 2)
            out[..., ch] += (raw[dy:h:2, dx:w:2] - bl[c]) / (wl - bl[c])
            cnt[ch] += 1
    out = np.minimum(np.clip(out / cnt, 0, None) * wb, 1.0)
    # the reader's flip: 3 - 180 degrees, 5 - 90 counterclockwise, 6 - 90 clockwise
    return np.rot90(out, {3: 2, 5: 1, 6: 3}.get(flip, 0)).copy()


def scene_of_raw(raw):
    """White-balanced RAW -> the scene it stands for (linear BT.709, the metered grey 0.18; the camera's headroom above 1
    kept): table_of_scene of it gives back the table signal."""
    return signal709_of_table(table_of_raw(raw)) * (0.18 / GREY)


def table_of_raw(raw):
    """White-balanced RAW (1 = the sensor's white) -> the table signal."""
    return np.asarray(raw, float) @ S80.T / FULL_SCALE


def raw_of_table(s):
    """The table signal -> the white-balanced RAW that gives it."""
    return (np.asarray(s, float) * FULL_SCALE) @ S80_INV.T


def sensor(s):
    """The table signal limited to what the sensor delivers: the white-balanced RAW in 0..1."""
    return table_of_raw(np.clip(raw_of_table(s), 0, 1))


def table_of_scene(x709):
    """Scene (linear BT.709, grey 0.18) -> the table signal the camera would give for it, correctly metered."""
    return (np.asarray(x709, float) * (GREY / 0.18)) @ P.T / FULL_SCALE


def signal709_of_table(s):
    """The table signal -> the camera's linear signal in BT.709 primaries (1 = the sensor's white)."""
    return (np.asarray(s, float) * FULL_SCALE) @ P_INV.T


class CameraTarget:
    """A look (a function of the scene: linear BT.709, grey 0.18 -> sRGB-encoded display RGB, such as cube.CubeTarget)
    as the camera should render it, on the table signal: the signal is brought to BT.709 primaries (P^-1) and scaled by
    0.18 / GREY, so the camera's metered grey is the look's grey. A look clips its input at the scene's 1 (a .cube does),
    so the camera's highlights above it (up to the sensor's white, 2.4 times the scene's 1) render as the look's white."""

    def __init__(self, look):
        self.look = look
        self.name = look.name
        self.desc = getattr(look, "desc", look.name)

    def _input(self, s):
        return signal709_of_table(s) * (0.18 / GREY)

    def __call__(self, s):
        return self.look(self._input(np.asarray(s, float).reshape(-1, 3)))
