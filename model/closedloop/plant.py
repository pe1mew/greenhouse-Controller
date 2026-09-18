"""
plant.py -- the calibrated greenhouse plant, advanced one 30 s step at a time.

The equations and the discrete update are those of
calibrate_plant_dynamic.simulate(), which fitted the adopted artifact
(campaign-summer-2026/plant_calibrated_constrained_summer2026_freem3.json):

    ach   = ach_inf + ach_m1*o1 + ach_m2*o2 + ach_m3*c(o3)          [/h]
    UA    = ach/3600 * V * rho * cp                                  [W/K]
    T_eq  = T_out + k_solar * lux / UA
    T[n]  = a_T  * T_eq  + (1 - a_T)  * T[n-1]      a_T  = DT * UA / C_eff
    AH_eq = AH_out + transp / (ach/3600 * V)
    AH[n] = a_AH * AH_eq + (1 - a_AH) * AH[n-1]     a_AH = DT * ach/3600
    RH[n] = rh_from_ah(AH[n], T[n])

o_k is window k's openness, 0..1, and c() maps M3's openness to the fraction
of its fully open effect (identity by default -- nobody has measured a
part-open M3 yet, so for linear control c() is an assumption to vary, not a
fact). The calibrator counted a window as open whenever its logged state was
not CLOSED, both moving states included, and applied the state LOGGED at
sample n to the step into n. T2 establishes that state at n-1, so a decision
at n-1 acts on the step into n; step() keeps that order.

Carried over from the calibrator on purpose
-------------------------------------------
* hold_on_change. calibrate_plant_dynamic.piecewise_lag() starts each
  window-state segment's filter at the previous output, which holds T and AH
  unchanged on the first sample of every segment (and on the very first
  sample). True reproduces the calibrator to the last bit -- the plant gate
  uses it to prove these are the calibrator's equations. The closed loop
  leaves it off: it is an artefact of the fit's bookkeeping, one skipped
  30 s update per window change, not physics.
* The volume. V is the artifact's own volume_m3, 2 400 m3, whatever the
  documents say (they say 2 900). The ach values only mean something
  against the volume they were fitted with.
* The psychrometrics, imported from the calibrator itself.

What the parameters are not
---------------------------
They are EFFECTIVE, not physical (thermalProfileCampaign.md s.9.5). The
fitted solar gain peaks at about 4.5 kW and the conductance is of the same
reduced order; only their ratio, and the time constant C_eff/UA, are tuned
to the data. There is no cover conduction, no latent heat, no structure
node. Do not read ach as a ventilation rate you could measure.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

MODEL_DIR = Path(__file__).resolve().parent.parent
if str(MODEL_DIR) not in sys.path:
    sys.path.insert(0, str(MODEL_DIR))

from calibrate_plant_dynamic import (  # noqa: E402  (path set above)
    CP_AIR, DT_S, RHO_AIR, ah_from_rh, rh_from_ah,
)

ADOPTED = MODEL_DIR / "campaign-summer-2026" / "plant_calibrated_constrained_summer2026_freem3.json"


class Plant:
    """One well-mixed air node for T and absolute humidity."""

    def __init__(self, params, dt_s=DT_S, hold_on_change=False, m3_curve=None):
        self.V        = float(params["volume_m3"])
        self.k_solar  = float(params["k_solar_w_per_lux"])
        self.c_eff_J  = float(params["c_eff_mj_per_c"]) * 1e6
        self.transp   = float(params["transpiration_kg_s"])
        self.ach_inf  = float(params["ach_inf"])
        self.ach_win  = (float(params["ach_m1"]), float(params["ach_m2"]),
                         float(params["ach_m3"]))
        self.dt       = float(dt_s)
        self.hold_on_change = hold_on_change
        self.m3_curve = m3_curve if m3_curve is not None else (lambda o: o)
        self.T = self.AH = None
        self._mask = None

    @classmethod
    def from_json(cls, path=ADOPTED, **kw):
        with open(path) as fh:
            return cls(json.load(fh), **kw)

    def reset(self, T0, AH0):
        """Start from a known state: T in degC, AH in kg/m3."""
        self.T = float(T0)
        self.AH = float(AH0)
        self._mask = None

    def ach(self, openness):
        """Total air changes per hour for the given window openness (o1, o2, o3)."""
        o1, o2, o3 = openness
        return (self.ach_inf + self.ach_win[0] * o1 + self.ach_win[1] * o2
                + self.ach_win[2] * self.m3_curve(o3))

    def step(self, openness, T_out, RH_out, lux):
        """Advance one DT with the windows as they are now. Returns (T, RH)."""
        mask = tuple(o > 0.0 for o in openness)
        if self.hold_on_change and mask != self._mask:
            self._mask = mask
            return self.T, self.rh()
        self._mask = mask

        ach_s = self.ach(openness) / 3600.0
        ua    = ach_s * self.V * RHO_AIR * CP_AIR
        a_t   = self.dt * ua / self.c_eff_J
        a_ah  = self.dt * ach_s
        if not (0.0 < a_t < 1.0 and 0.0 < a_ah < 1.0):
            raise ValueError("30 s update unstable at ach=%.2f /h (a_T=%.3f, a_AH=%.3f)"
                             % (ach_s * 3600.0, a_t, a_ah))

        T_eq  = T_out + self.k_solar * lux / ua
        AH_eq = float(ah_from_rh(RH_out, T_out)) + self.transp / (ach_s * self.V)
        self.T  = a_t * T_eq + (1.0 - a_t) * self.T
        self.AH = max(a_ah * AH_eq + (1.0 - a_ah) * self.AH, 0.0)
        return self.T, self.rh()

    def rh(self):
        return float(rh_from_ah(self.AH, self.T))
