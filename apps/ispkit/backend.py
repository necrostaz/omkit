"""The dense arithmetic of fitting - the Jacobian (points x parameters) and the normal equations - on the GPU (torch,
float32) or on the CPU (numpy, float64). The per-point vectors stay numpy float64 everywhere; only the arrays with a
parameter axis go to the backend. Consumer GPUs run float64 at a small fraction of float32, so the GPU works in float32
and the small P x P system is solved on the CPU in float64.

torch is optional. ISPKIT_GPU=0 in the environment forces the CPU; without torch or CUDA the CPU is used. The render of
the quantised tables (gpu.py) uses the same device in float64."""
from __future__ import annotations

import os

import numpy as np

try:
    import torch
except ImportError:          # torch is optional
    torch = None


class Numpy:
    name = "cpu"

    def V(self, a):
        return np.asarray(a, float)

    def I(self, a):
        return np.asarray(a)

    def zeros(self, shape):
        return np.zeros(shape)

    def einsum(self, eq, *ops):
        return np.einsum(eq, *ops)

    def stack(self, xs, axis):
        return np.stack(xs, axis)

    def cat(self, xs, axis):
        return np.concatenate(xs, axis)

    def normal(self, J, r):
        """J (M, P) and r (M,) -> J^T J, J^T r as float64 numpy."""
        return J.T @ J, J.T @ r


class Torch:
    name = "gpu"

    def __init__(self):
        self.dev = torch.device("cuda")
        torch.backends.cuda.matmul.allow_tf32 = False      # full float32 in the products

    def V(self, a):
        return torch.as_tensor(np.asarray(a, np.float32), device=self.dev)

    def I(self, a):
        return torch.as_tensor(np.asarray(a, np.int64), device=self.dev)

    def zeros(self, shape):
        return torch.zeros(shape, dtype=torch.float32, device=self.dev)

    def einsum(self, eq, *ops):
        return torch.einsum(eq, *[o if torch.is_tensor(o) else self.V(o) for o in ops])

    def stack(self, xs, axis):
        return torch.stack(xs, dim=axis)

    def cat(self, xs, axis):
        return torch.cat(xs, dim=axis)

    def normal(self, J, r):
        h = J.T @ J
        g = J.T @ self.V(r)
        return h.double().cpu().numpy(), g.double().cpu().numpy()


def _choose():
    if os.environ.get("ISPKIT_GPU", "auto") == "0" or torch is None:
        return Numpy()
    try:
        return Torch() if torch.cuda.is_available() else Numpy()
    except Exception:            # a broken CUDA setup: the CPU still works
        return Numpy()


B = _choose()
