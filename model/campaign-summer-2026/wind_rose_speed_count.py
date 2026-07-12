"""Polar plot: mean wind speed per direction bin, colored by sample count.

Data: calibration_input_2026-06-04_2026-07-12.csv, wind-valid era only
(>= 2026-06-19 12:00, vane commissioning). 30 s cadence rows from the unit's
own anemometer. One radial axis (mean speed, m/s); the number of samples per
22.5-degree bin is encoded as a sequential ramp + labels on dominant bins.

Output: wind_rose_speed_count.png (same directory).
"""
import csv
import math
from datetime import datetime
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import LinearSegmentedColormap, Normalize
from matplotlib.cm import ScalarMappable

HERE   = Path(__file__).parent
CAL_IN = HERE / "calibration_input_2026-06-04_2026-07-12.csv"
WIND_VALID_FROM = datetime(2026, 6, 19, 12, 0, 0)

N_BINS = 16
BIN_W  = 360.0 / N_BINS

speeds = [[] for _ in range(N_BINS)]
with open(CAL_IN) as f:
    for r in csv.DictReader(f):
        try:
            ts = datetime.strptime(r["timestamp"], "%Y-%m-%dT%H:%M:%S")
            if ts < WIND_VALID_FROM:
                continue
            v = float(r["wind_ms"]); d = float(r["wind_dir_deg"]) % 360.0
        except (ValueError, KeyError):
            continue
        speeds[int(((d + BIN_W / 2) % 360.0) // BIN_W)].append(v)  # bins centred on N, NNE, ...

counts = np.array([len(s) for s in speeds])
means  = np.array([np.mean(s) if s else 0.0 for s in speeds])
total  = counts.sum()

INK, MUTED, GRID = "#1a1a1a", "#666666", "#d9d9d9"
# sequential blue ramp (reference palette steps 200..650)
cmap = LinearSegmentedColormap.from_list(
    "seqblue", ["#9ec5f4", "#6da7ec", "#3987e5", "#256abf", "#184f95", "#104281"])
norm = Normalize(vmin=0, vmax=counts.max())

fig = plt.figure(figsize=(8.0, 8.8), facecolor="white")
ax = fig.add_subplot(111, projection="polar")
ax.set_theta_zero_location("N")
ax.set_theta_direction(-1)

R_MAX = math.ceil(means.max() * 1.25 * 2) / 2
ax.set_rlim(0, R_MAX)

# context: M3 windward / leeward sector boundaries (visual continuity with m3_ach_polar)
for b in (-45, 45, 135, 225):
    ax.plot([math.radians(b)] * 2, [0, R_MAX], color=GRID, lw=0.8, ls="--", zorder=1)
ax.text(math.radians(0), R_MAX * 0.94, "windward for M3 (N wall)",
        ha="center", va="center", fontsize=8, color=MUTED, style="italic")
ax.text(math.radians(180), R_MAX * 0.94, "leeward",
        ha="center", va="center", fontsize=8, color=MUTED, style="italic")

# bars: radius = mean speed, fill = sample count (sequential)
theta = np.radians(np.arange(N_BINS) * BIN_W)
width = math.radians(BIN_W) * 0.93          # ~2px surface gap between bars
bars = ax.bar(theta, means, width=width, bottom=0.0,
              color=[cmap(norm(c)) for c in counts],
              edgecolor="white", linewidth=1.2, zorder=3)

# direct count labels on the dominant bins only (top 4 by count),
# offset radially outward along each bar's own direction
for i in np.argsort(counts)[-4:]:
    dx, dy = 16 * math.sin(theta[i]), 16 * math.cos(theta[i])
    ax.annotate(f"n={counts[i]:,}".replace(",", " "),
                (theta[i], means[i]), textcoords="offset points",
                xytext=(dx, dy), ha="center", fontsize=8, color=INK, zorder=6)

ax.set_thetagrids(range(0, 360, 45), labels=["N", "NE", "E", "SE", "S", "SW", "W", "NW"])
ax.tick_params(axis="x", labelsize=10, colors=INK, pad=2)
rticks = np.arange(0.5, R_MAX + 0.01, 0.5)
ax.set_rgrids(rticks, angle=112, fontsize=8, color=MUTED,
              labels=[f"{t:g}" for t in rticks])
ax.text(math.radians(112), R_MAX * 1.12, "mean wind [m/s]", fontsize=8,
        color=MUTED, ha="center")
ax.grid(color=GRID, lw=0.7)
ax.spines["polar"].set_color(GRID)

cbar = fig.colorbar(ScalarMappable(norm=norm, cmap=cmap), ax=ax,
                    orientation="horizontal", fraction=0.045, pad=0.07, aspect=32)
cbar.set_label(f"samples per bin (30 s cadence; total {total:,})".replace(",", " "),
               fontsize=8.5, color=MUTED)
cbar.ax.tick_params(labelsize=8, colors=MUTED)
cbar.outline.set_color(GRID)

fig.suptitle("Wind at unit 5C88 — mean speed and sample count per direction",
             fontsize=13, fontweight="bold", color=INK, y=0.98)
ax.set_title("22.5° bins centred on the compass points; wind-valid era "
             "(≥ 2026-06-19 12:00 → 2026-07-12)",
             fontsize=9, color=MUTED, pad=24)

fig.savefig(HERE / "wind_rose_speed_count.png", dpi=150, bbox_inches="tight",
            facecolor="white")
print(f"saved wind_rose_speed_count.png  (bins: " +
      ", ".join(f"{int(b*BIN_W)}°:{c}" for b, c in enumerate(counts)) + ")")
