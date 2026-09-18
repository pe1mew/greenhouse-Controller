"""
plant2.py -- the two-node plant: parameters, the C kernel, and a step-wise wrapper.

The equations are in plant2_kernel.c's header. This module
  * builds the kernel (build/plant2.dll, via ventmodel.build_dll) and binds it;
  * names the parameter vector (PARAMS) and holds the fit bounds (BOUNDS);
  * runs a whole Dataset in one call (run(), what the fit uses);
  * wraps it as Plant2, advanced one step at a time by the closed loop.

There is ONE implementation of the equations -- the C loop. The closed loop
calls it with n = 1 and carries the state in Python, so the plant it
simulates is exactly the plant that was fitted.

What the kernel returns as T and AH is what the SENSOR reads: the air node
through the sensor stage (tau_s1, tau_s2; NS-10). With both at 0 -- every
artifact fitted before the stage existed -- that is the air node itself.

Volume: V is a unit convention here, not a free parameter. Heat flows are
in W/K and do not depend on it; the ach figures and E/V do. It is fixed at
the adopted artifact's 2 400 m3 so the two models' ach numbers compare.
"""

from __future__ import annotations

import ctypes
import json
from pathlib import Path

import numpy as np

from plant import CP_AIR, RHO_AIR, rh_from_ah  # noqa: F401  (re-exported for refit.py)
from ventmodel import BUILD_DIR, HERE, build_dll

KERNEL_SRC = HERE / "plant2_kernel.c"
KERNEL_DLL = BUILD_DIR / "plant2.dll"

# Mirrors the enum in plant2_kernel.c.
PARAMS = ["Ca_MJ", "Cs_MJ", "Gas", "Gso", "UA0", "ach_m1", "ach_m2", "ach_m3",
          "ach_door", "ka", "ks", "V", "ach_inf", "e0", "e1", "m3_ww", "m3_dir0",
          "tau_s1", "tau_s2"]
N_STATE = 7   # Ta, Ts, AH, then the sensor stage: T1, T2, AH1, AH2
V_FIXED = 2400.0

# Fit bounds: wide, and physical where physics gives a limit. ka/ks in W per
# outdoor lux: the physical incident gain is of order 2-4 W/lux for a floor of
# ~400-500 m2 (about 120 lux per W/m2 of sunlight), so 10 is generous.
BOUNDS = {
    "Ca_MJ":    (0.2, 30.0),
    "Cs_MJ":    (1.0, 2000.0),
    "Gas":      (10.0, 60000.0),
    "Gso":      (0.0, 20000.0),
    "UA0":      (10.0, 20000.0),
    "ach_m1":   (0.0, 40.0),
    "ach_m3":   (0.0, 60.0),
    "ach_door": (0.0, 40.0),
    "ka":       (0.0, 10.0),
    "ks":       (0.0, 10.0),
    "ach_inf":  (0.0, 10.0),
    "e0":       (0.0, 0.05),
    "e1":       (0.0, 2e-6),
    "m3_ww":    (0.0, 60.0),
    "tau_s1":   (0.0, 900.0),
    "tau_s2":   (0.0, 900.0),
}

_D = ctypes.POINTER(ctypes.c_double)
_B = ctypes.POINTER(ctypes.c_ubyte)
_LIB = None


def lib():
    global _LIB
    if _LIB is None:
        path = build_dll(KERNEL_DLL, [KERNEL_SRC])
        dll = ctypes.CDLL(str(path))
        dll.p2_param_count.restype = ctypes.c_int
        dll.p2_run.restype = ctypes.c_int
        dll.p2_run.argtypes = [ctypes.c_int, _D] + [_D] * 9 + [_B, _D, _D, _D, _D, _D, _D]
        if dll.p2_param_count() != len(PARAMS):
            raise RuntimeError("plant2_kernel.c has %d parameters, plant2.py names %d"
                               % (dll.p2_param_count(), len(PARAMS)))
        _LIB = dll
    return _LIB


def _ptr(a):
    return a.ctypes.data_as(_D) if a is not None else None


def vector(params):
    """A full kernel parameter vector from a {name: value} dict."""
    v = np.zeros(len(PARAMS))
    for i, name in enumerate(PARAMS):
        v[i] = float(params.get(name, 0.0))
    v[PARAMS.index("V")] = V_FIXED
    if "ach_m2" not in params:
        v[PARAMS.index("ach_m2")] = v[PARAMS.index("ach_m1")]   # prior: M1 = M2
    return v


def cover_ua(params):
    """UA0 minus its air-exchange part: what conducts through the cover, W/K."""
    return params["UA0"] - params["ach_inf"] * V_FIXED * RHO_AIR * CP_AIR / 3600.0


def anchors(ds, horizon_s):
    """restart codes for the kernel: 1 at data gaps, 2 every horizon_s (0 = none)."""
    code = ds.restart.astype(np.uint8)
    if horizon_s > 0:
        last = ds.t_s[0]
        for i in range(len(ds)):
            if code[i] == 1 or ds.t_s[i] - last >= horizon_s:
                if code[i] == 0:
                    code[i] = 2
                last = ds.t_s[i]
    return code


class Prepared:
    """A Dataset's kernel inputs as contiguous arrays, built once (the fit's hot path)."""

    def __init__(self, ds, doors=None, horizon_s=0):
        f = lambda a: np.ascontiguousarray(a, dtype=np.float64)  # noqa: E731
        self.n = len(ds)
        self.dt, self.To, self.AHo, self.lux = f(ds.dt), f(ds.T_out), f(ds.AH_out), f(ds.lux)
        self.o = [f(ds.o[:, k]) for k in range(3)]
        self.doors = f(ds.door1 + ds.door2 if doors is None else doors)
        # No valid direction before the vane's commissioning: NaN, which the
        # kernel's cos > 0 test reads as "no windward term".
        self.wdir = f(np.where(ds.wind_valid, ds.wind_dir, np.nan))
        self.restart = np.ascontiguousarray(anchors(ds, horizon_s))
        self.Tm, self.AHm = f(ds.T_in), f(ds.AH_in)
        self.Ta, self.Ts, self.AH = np.empty(self.n), np.empty(self.n), np.empty(self.n)
        self.state = np.zeros(N_STATE)
        self.ptrs = [_ptr(x) for x in (self.dt, self.To, self.AHo, self.lux, self.o[0], self.o[1],
                                       self.o[2], self.doors, self.wdir)]
        self.tail = [self.restart.ctypes.data_as(_B), _ptr(self.Tm), _ptr(self.AHm),
                     _ptr(self.state), _ptr(self.Ta), _ptr(self.Ts), _ptr(self.AH)]


def run(params, ds, doors=None, prep=None):
    """Run the plant over a Dataset. Returns (Ta, Ts, AH) arrays.

    Restarts from the measurement wherever ds.restart is set. Pass a
    Prepared to skip the array preparation; the returned arrays are then
    its buffers, overwritten by the next run with it.
    """
    prep = prep or Prepared(ds, doors)
    p = vector(params) if isinstance(params, dict) else params
    prep.state[:] = (prep.Tm[0], prep.Tm[0], prep.AHm[0],
                     prep.Tm[0], prep.Tm[0], prep.AHm[0], prep.AHm[0])
    rc = lib().p2_run(prep.n, _ptr(p), *prep.ptrs, *prep.tail)
    if rc != 0:
        raise ValueError("p2_run rejected the parameters")
    return prep.Ta, prep.Ts, prep.AH


class Plant2:
    """The same kernel, one step at a time, for the closed loop."""

    def __init__(self, params):
        self.params = dict(params)
        self.p = vector(self.params)
        self.state = np.zeros(N_STATE)
        self._buf = {k: np.zeros(1) for k in ("dt", "To", "AHo", "lux", "o1", "o2", "o3",
                                               "doors", "wdir", "Ta", "Ts", "AH")}

    @classmethod
    def from_json(cls, path):
        with open(path) as fh:
            return cls(json.load(fh)["params"])

    def reset(self, T0, AH0, Ts0=None):
        """From a measurement: the air node and the sensor stage both take it."""
        self.state[:] = (T0, T0 if Ts0 is None else Ts0, AH0, T0, T0, AH0, AH0)

    def step(self, openness, doors, wind_dir, T_out, AH_out, lux, dt=30.0):
        b = self._buf
        b["dt"][0], b["To"][0], b["AHo"][0], b["lux"][0] = dt, T_out, AH_out, lux
        b["o1"][0], b["o2"][0], b["o3"][0] = openness
        b["doors"][0], b["wdir"][0] = doors, wind_dir
        rc = lib().p2_run(1, _ptr(self.p), _ptr(b["dt"]), _ptr(b["To"]), _ptr(b["AHo"]),
                          _ptr(b["lux"]), _ptr(b["o1"]), _ptr(b["o2"]), _ptr(b["o3"]),
                          _ptr(b["doors"]), _ptr(b["wdir"]), None, None, None,
                          _ptr(self.state), _ptr(b["Ta"]), _ptr(b["Ts"]), _ptr(b["AH"]))
        if rc != 0:
            raise ValueError("p2_run rejected the parameters")
        T = float(self.state[4])                        # what the sensor reads
        return T, float(rh_from_ah(self.state[6], T))

    @property
    def T_air(self):
        return float(self.state[0])

    @property
    def T_structure(self):
        return float(self.state[1])
