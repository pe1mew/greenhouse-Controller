"""Polar/compass plot: fitted M3-only ach_m3 vs wind direction (NS-9 step 1).

Data: the seven M3-only segments >= 15 min in the wind-valid era
(>= 2026-06-19 12:00), per-segment fits from ns9_direction_stratified.py
(thermalProfileCampaign.md section 9.11). Compass convention: N up, clockwise.

Output: m3_ach_polar.png (same directory).
"""
import math
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# (dir deg, ach_m3 /h [clipped at 0], wind m/s, group, value label,
#  speed-label offset (pts), ha)
POINTS = [
    (325, 0.0,  0.8, "night",  None,   (-4, 16),   "right"),  # Jun 25 05:19
    (277, 0.0,  0.4, "night",  None,   (-14, 16),  "right"),  # Jul 1 03:31
    (261, 0.0,  1.5, "ns6",    None,   (-14, -22), "right"),  # Jul 4 09:52
    (268, 0.0,  2.4, "ns6",    None,   (-14, -4),  "right"),  # Jul 4 11:54
    ( 65, 4.08, 1.6, "forced", "4.1",  (10, -11),  "left"),   # Jul 11 10:06
    ( 26, 10.18,1.6, "forced", "10.2", (10, -11),  "left"),   # Jul 11 11:14
    ( 21, 2.62, 2.2, "forced", "2.6",  (10, -11),  "left"),   # Jul 11 12:18
]

# categorical slots, fixed order by chronological first appearance
GROUPS = {
    "night":  dict(color="#2a78d6", marker="o", label="Night segments (Jun 25, Jul 1)"),
    "ns6":    dict(color="#1baf7a", marker="s", label="NS-6 test Jul 4 — W wind"),
    "forced": dict(color="#eda100", marker="^", label="Forced test Jul 11 — N/NE wind"),
}
INK, MUTED, GRID = "#1a1a1a", "#666666", "#d9d9d9"

fig = plt.figure(figsize=(8.0, 8.6), facecolor="white")
ax = fig.add_subplot(111, projection="polar")
ax.set_theta_zero_location("N")
ax.set_theta_direction(-1)

R_MAX = 11.0
ax.set_rlim(0, R_MAX)
ax.set_rorigin(-2.4)                       # keep r=0 points visibly off-centre

# context sectors (annotation, not data): windward 315-45, leeward 135-225
th_ww = np.radians(np.linspace(-45, 45, 60))
ax.fill_between(th_ww, 0, R_MAX, color="#000000", alpha=0.055, zorder=0)
th_lee = np.radians(np.linspace(135, 225, 60))
ax.fill_between(th_lee, 0, R_MAX, color="#000000", alpha=0.02, zorder=0)
for b in (-45, 45, 135, 225):
    ax.plot([math.radians(b)] * 2, [0, R_MAX], color=GRID, lw=0.8, ls="--", zorder=1)
ax.text(math.radians(0), R_MAX * 0.86, "windward\nfor M3 (N wall)",
        ha="center", va="center", fontsize=8.5, color=MUTED, style="italic")
ax.text(math.radians(180), R_MAX * 0.80, "leeward",
        ha="center", va="center", fontsize=8.5, color=MUTED, style="italic")

# grid + compass
ax.set_thetagrids(range(0, 360, 45), labels=["N", "NE", "E", "SE", "S", "SW", "W", "NW"])
ax.tick_params(axis="x", labelsize=10, colors=INK, pad=2)
ax.set_rgrids([0, 2, 4, 6, 8, 10], angle=100, fontsize=8, color=MUTED)
ax.text(math.radians(100), 11.8, "ach$_{M3}$ [/h]", fontsize=8, color=MUTED,
        ha="center")
ax.grid(color=GRID, lw=0.7)
ax.spines["polar"].set_color(GRID)

# marks
for deg, ach, v, grp, vlab, off, ha in POINTS:
    g = GROUPS[grp]
    th = math.radians(deg)
    ax.scatter(th, ach, s=120, marker=g["marker"], color=g["color"],
               edgecolor="white", linewidth=1.6, zorder=5)
    # wind-speed label beside each mark (per-point offset avoids collisions
    # among the near-zero W-sector cluster)
    ax.annotate(f"{v} m/s", (th, ach), textcoords="offset points",
                xytext=off, fontsize=7.5, color=MUTED, zorder=6, ha=ha)
    if vlab:  # direct value labels on the non-zero (windward) points
        ax.annotate(f"{vlab} /h", (th, ach), textcoords="offset points",
                    xytext=(9, 5), fontsize=9, color=INK, fontweight="bold",
                    zorder=6)

handles = [plt.Line2D([], [], color=g["color"], marker=g["marker"], ls="",
                      markersize=9, markeredgecolor="white", label=g["label"])
           for g in GROUPS.values()]
ax.legend(handles=handles, loc="lower left", bbox_to_anchor=(-0.12, -0.145),
          fontsize=8.5, frameon=False)

fig.suptitle("M3 (north-wall window) ventilation vs wind direction",
             fontsize=13, fontweight="bold", color=INK, y=0.975)
ax.set_title("Per-segment fitted ach$_{M3}$, M3-only segments ≥ 15 min, "
             "wind-valid era (≥ Jun 19 12:00) — NS-9 step 1",
             fontsize=9, color=MUTED, pad=26)
fig.text(0.5, 0.012,
         "W-sector wind: ach$_{M3}$ ≈ 0 at every observed speed. N/NE-sector: 2.6–10.2 /h at 1.6–2.2 m/s.\n"
         "Windward values are order-of-magnitude (single-node fit underestimates during flush; doors unfiltered on forced tests).",
         ha="center", fontsize=7.5, color=MUTED)

fig.savefig(__file__.replace(".py", ".png"), dpi=150, bbox_inches="tight",
            facecolor="white")
print("saved m3_ach_polar.png")
