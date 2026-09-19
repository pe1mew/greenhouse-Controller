"""
ventmodel.py -- the control law from drivers/ventModel, loaded into Python.

The closed-loop simulator has to test the law the firmware runs, not a Python
copy of it: simulation.py carries its own port of the stepped law, and a port
drifts. design/ventModelContract.md s.4 makes the library host-compilable by
rule so that one set of sources serves the firmware, the host tests and the
offline tools. This module is that last consumer: the closed loop and
vent_step_replay.py both load the law through it, and entry_temp_c() answers
"where does this step start?" by asking the law instead of recomputing it.

Build
-----
g++ (MinGW-w64 -- the toolchain drivers/ventModel/set_compiler.py finds)
compiles every drivers/ventModel/src/*.cpp together with ventmodel_ffi.cpp
into build/ventmodel.dll. The C++ runtime is linked statically, so nothing
has to be on PATH when Python loads it. The DLL is rebuilt whenever one of
its sources is newer than it. Set VENTMODEL_CXX to use another compiler.

Layout check
------------
The ctypes structures below mirror src/vent_model.h by hand. On load, every
struct size and member offset is compared, BY C NAME, with the compiled
layout (the shim's k_layout table), and any difference raises before a
single step runs: a missing or extra member, a wrong width, or two
same-sized members swapped. (A comparison by position alone let the swap
through; that was found by testing the check against broken mirrors.)

Usage
-----
    from ventmodel import VentModel, VentIn
    law = VentModel("stepped")
    out = law.step(vin)          # vin: a filled VentIn
    out.step, out.win[2].action  # read it before the next step() call

ASCII-only output (Windows console is cp1252 -- see memory/gotcha-log.md).
"""

from __future__ import annotations

import ctypes
import os
import shutil
import subprocess
from ctypes import (POINTER, c_bool, c_char_p, c_int, c_int8, c_int16, c_int32,
                    c_uint8, c_uint16, c_uint32)
from pathlib import Path

HERE      = Path(__file__).resolve().parent
REPO      = HERE.parent.parent
LIB_SRC   = REPO / "drivers" / "ventModel" / "src"
FFI_SRC   = HERE / "ventmodel_ffi.cpp"
BUILD_DIR = HERE / "build"
DLL_PATH  = BUILD_DIR / ("ventmodel.dll" if os.name == "nt" else "libventmodel.so")

# Where drivers/ventModel/set_compiler.py looks for the Code::Blocks MinGW.
_CXX_CANDIDATES = (
    r"C:\Program Files\CodeBlocks\MinGW\bin\g++.exe",
    r"C:\Program Files (x86)\CodeBlocks\MinGW\bin\g++.exe",
    r"C:\CodeBlocks\MinGW\bin\g++.exe",
)

# --------------------------------------------------------------------------
# vent_model.h, mirrored. Keep every struct's member order identical to the
# header: the layout check compares offsets in declaration order.
# --------------------------------------------------------------------------

VENT_MODEL_API = 1
VENT_WINDOWS   = 3
VENT_STEPS_MAX = 3
VENT_STEP_NONE = -1

VENT_CAP_DIGITAL, VENT_CAP_LINEAR = 0, 1

(VENT_WIN_UNKNOWN, VENT_WIN_CLOSED, VENT_WIN_MOVING_OPEN,
 VENT_WIN_OPEN, VENT_WIN_MOVING_CLOSE) = range(5)

VENT_ACT_HOLD, VENT_ACT_CLOSE, VENT_ACT_OPEN, VENT_ACT_TARGET = range(4)

(VENT_RES_NONE, VENT_RES_DONE, VENT_RES_FAIL_TIMEOUT,
 VENT_RES_FAIL_FAULT, VENT_RES_ABORTED) = range(5)


class VentWinIn(ctypes.Structure):
    _fields_ = [
        ("state",           c_int),
        ("cap",             c_int),
        ("pos_x10",         c_int16),
        ("pos_age_ms",      c_uint32),
        ("last_target_x10", c_int16),
        ("last_result",     c_int),
        ("ms_since_move",   c_uint32),
    ]


class VentIn(ctypes.Structure):
    _fields_ = [
        ("now_ms",           c_uint32),
        ("unix_time",        c_uint32),
        ("daytime",          c_bool),
        ("t_c10",            c_int16),
        ("t_avg_c10",        c_int16),
        ("t_avg_c",          c_int16),
        ("rh_pct",           c_uint8),
        ("rh_avg_pct",       c_uint8),
        ("wind_ms10",        c_uint16),
        ("wind_avg_ms10",    c_uint16),
        ("wind_dir_deg",     c_uint16),
        ("wind_dir_avg_deg", c_uint16),
        ("wind_dir_var_deg", c_uint16),
        ("t_valid",          c_bool),
        ("rh_valid",         c_bool),
        ("wind_valid",       c_bool),
        ("t_max_c10",        c_int16),
        ("rh_max_pct",       c_uint8),
        ("rh_min_pct",       c_uint8),
        ("hyst_t_c",         c_uint8),
        ("hyst_rh_pct",      c_uint8),
        ("cr_priority",      c_uint8),
        ("rh_ctrl_en",       c_bool),
        ("m3_deadzone_x10",  c_uint16),
        ("m3_min_move_ms",   c_uint16),
        ("win",              VentWinIn * VENT_WINDOWS),
    ]


class VentWinOut(ctypes.Structure):
    _fields_ = [
        ("action",     c_int),
        ("target_x10", c_int16),
    ]


class VentOut(ctypes.Structure):
    _fields_ = [
        ("win",           VentWinOut * VENT_WINDOWS),
        ("reason",        c_uint8),
        ("step",          c_int8),
        ("step_t",        c_int8),
        ("step_rh",       c_int8),
        ("demand_t_x10",  c_int16),
        ("demand_rh_x10", c_int16),
    ]


class VentState(ctypes.Structure):
    _fields_ = [("v", c_int32 * 8)]


def _mirror_layout():
    """{C name: size or offset} for the ctypes mirror, keyed like k_layout."""
    out = {}
    for cname, struct in (("vent_win_in_t", VentWinIn), ("vent_in_t", VentIn),
                          ("vent_win_out_t", VentWinOut), ("vent_out_t", VentOut)):
        out[cname] = ctypes.sizeof(struct)
        for name, _ in struct._fields_:
            out["%s.%s" % (cname, name)] = getattr(struct, name).offset
    out["vent_state_t"] = ctypes.sizeof(VentState)
    return out


# --------------------------------------------------------------------------
# Build
# --------------------------------------------------------------------------

def _find_cxx():
    env = os.environ.get("VENTMODEL_CXX")
    if env:
        return env
    for cand in _CXX_CANDIDATES:
        if Path(cand).is_file():
            return cand
    found = shutil.which("g++")
    if found:
        return found
    raise RuntimeError("no C++ compiler found: install the Code::Blocks MinGW that "
                       "drivers/ventModel already needs, or set VENTMODEL_CXX")


def _sources():
    return sorted(LIB_SRC.glob("*.cpp")) + [FFI_SRC]


def build_dll(out, sources, deps=(), include_dirs=(), force=False, cxx_std="c++17"):
    """Compile sources into a shared library at out, when any of them changed.

    C++ and C sources both go through g++ (a .c file compiles as C++; the
    kernels here are written to be valid as either). Shared with plant2.py.
    """
    out = Path(out)
    watch = list(sources) + list(deps)
    if not force and out.is_file():
        built = out.stat().st_mtime
        if all(Path(p).stat().st_mtime <= built for p in watch):
            return out

    out.parent.mkdir(exist_ok=True)
    cxx = _find_cxx()
    cmd = [cxx, "-std=%s" % cxx_std, "-O2", "-Wall", "-Wextra", "-shared"]
    if os.name == "nt":
        cmd += ["-static", "-static-libgcc", "-static-libstdc++"]
    else:
        cmd += ["-fPIC"]
    for inc in include_dirs:
        cmd += ["-I", str(inc)]
    cmd += [str(p) for p in sources] + ["-o", str(out)]

    # MinGW's driver finds cc1plus, as and ld through its own directory.
    env = dict(os.environ)
    env["PATH"] = str(Path(cxx).parent) + os.pathsep + env.get("PATH", "")
    res = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if res.returncode != 0:
        raise RuntimeError("building %s failed:\n  %s\n%s"
                           % (out.name, " ".join(cmd), res.stderr))
    return out


def build(force=False):
    """Compile the library and the shim into DLL_PATH when a source changed."""
    return build_dll(DLL_PATH, _sources(), deps=sorted(LIB_SRC.glob("*.h")),
                     include_dirs=[LIB_SRC], force=force)


# --------------------------------------------------------------------------
# Load
# --------------------------------------------------------------------------

class VentLib:
    """The loaded DLL, its layout verified against the mirror."""

    def __init__(self, path=None):
        path = Path(path) if path else build()
        self.path = path
        dll = ctypes.CDLL(str(path))
        dll.vm_api.restype = c_int
        dll.vm_count.restype = c_int
        dll.vm_name.argtypes = [c_int]
        dll.vm_name.restype = c_char_p
        dll.vm_version.argtypes = [c_int]
        dll.vm_version.restype = c_int
        dll.vm_reset.argtypes = [c_int, POINTER(VentState)]
        dll.vm_reset.restype = c_int
        dll.vm_step.argtypes = [c_int, POINTER(VentIn), POINTER(VentState), POINTER(VentOut)]
        dll.vm_step.restype = c_int
        dll.vm_layout_count.restype = c_int
        dll.vm_layout_name.argtypes = [c_int]
        dll.vm_layout_name.restype = c_char_p
        dll.vm_layout_value.argtypes = [c_int]
        dll.vm_layout_value.restype = c_int
        self.dll = dll
        self._verify()

    def _verify(self):
        api = self.dll.vm_api()
        if api != VENT_MODEL_API:
            raise RuntimeError("vent_model.h is API %d, this mirror is API %d"
                               % (api, VENT_MODEL_API))
        compiled = {self.dll.vm_layout_name(i).decode("ascii"): self.dll.vm_layout_value(i)
                    for i in range(self.dll.vm_layout_count())}
        mirror = _mirror_layout()
        wrong = sorted(k for k in set(compiled) | set(mirror)
                       if compiled.get(k) != mirror.get(k))
        if wrong:
            raise RuntimeError(
                "the ctypes mirror in ventmodel.py does not match vent_model.h as "
                "compiled -- update the mirror before running anything.\n" +
                "\n".join("  %-34s compiled %-5s mirror %s" % (k, compiled.get(k, "-"),
                                                               mirror.get(k, "-"))
                          for k in wrong))

    def models(self):
        """{name: (id, version)} for every law the library provides."""
        out = {}
        for i in range(self.dll.vm_count()):
            out[self.dll.vm_name(i).decode("ascii")] = (i, self.dll.vm_version(i))
        return out


_LIB = None


def load():
    """The shared, built-and-verified library (built once per process)."""
    global _LIB
    if _LIB is None:
        _LIB = VentLib()
    return _LIB


class VentModel:
    """One law from the library, called the way T6 will call it.

    step() returns the model's output buffer, which the next step() call
    overwrites -- copy what you need before calling again.
    """

    def __init__(self, name="stepped", lib=None):
        self.lib = lib or load()
        models = self.lib.models()
        if name not in models:
            raise KeyError("no model %r in drivers/ventModel (have: %s)"
                           % (name, ", ".join(sorted(models))))
        self.id, self.version = models[name]
        self.name = name
        self.state = VentState()
        self.out = VentOut()
        self.reset()

    def reset(self):
        if self.lib.dll.vm_reset(self.id, ctypes.byref(self.state)) != 0:
            raise RuntimeError("vm_reset(%d) failed" % self.id)

    def step(self, vin):
        rc = self.lib.dll.vm_step(self.id, ctypes.byref(vin),
                                  ctypes.byref(self.state), ctypes.byref(self.out))
        if rc != 0:
            raise RuntimeError("vm_step(%d) failed" % self.id)
        return self.out


def entry_temp_c(step, t_max_c, hyst_t_c, name="stepped"):
    """The lowest whole-degree average, at or above t_max_c, at which the law's
    temperature demand reaches `step`: from rest (a fresh law), by day, with
    humidity off.

    Asked of the law, so no tool restates its arithmetic. For the stepped law
    and step VENT_STEPS_MAX this is where M3 opens: 31 degC at 5C88's
    t_max_day 28 and hyst_t 5.
    """
    if not 0 <= hyst_t_c <= 255:
        raise ValueError("hyst_t %r does not fit vent_in_t.hyst_t_c" % (hyst_t_c,))
    law = VentModel(name)
    v = VentIn()
    v.daytime = True
    v.t_valid = True
    v.t_max_c10 = t_max_c * 10
    v.hyst_t_c = hyst_t_c
    for t in range(t_max_c, t_max_c + 3 * 256):    # past any step hyst_t_c can set
        law.reset()
        v.t_avg_c = t
        if law.step(v).step_t >= step:
            return t
    raise ValueError("the %s law never demands step %d at t_max %d degC, hyst_t %d"
                     % (name, step, t_max_c, hyst_t_c))


if __name__ == "__main__":
    lib = VentLib(build(force=True))
    print("built and verified: %s" % lib.path)
    for name, (mid, ver) in sorted(lib.models().items()):
        print("  model %d: %s v%d" % (mid, name, ver))
