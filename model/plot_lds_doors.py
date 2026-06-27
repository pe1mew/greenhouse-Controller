"""Plot door open/closed state for LDS01 sensors from the Wenumseveld MySQL database."""

import sys
import mysql.connector
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from datetime import datetime

DB = dict(host="192.168.20.232", user="wenumseveld", password="wenumseveld",
          database="wenumseveld", connect_timeout=15)

SENSORS = ["lds01-5", "lds01-6"]
OUT = "model/plot_lds_doors.png"


def fetch(cur, sensor):
    cur.execute("""
        SELECT dateTime, doorStatus, doorOpenTimes
        FROM wenumseveld
        WHERE sensor = %s
        ORDER BY dateTime
    """, (sensor,))
    rows = cur.fetchall()
    times  = [r[0] for r in rows]
    status = [r[1] for r in rows]
    opens  = [r[2] for r in rows]
    return times, status, opens


cn  = mysql.connector.connect(**DB)
cur = cn.cursor()

fig, axes = plt.subplots(len(SENSORS), 1, figsize=(13, 5), sharex=True)
fig.suptitle("Wenumseveld — LDS01 door sensors", fontsize=13, fontweight="bold")

for ax, sensor in zip(axes, SENSORS):
    times, status, opens = fetch(cur, sensor)

    # Extend the step trace to "now" so the last state is visible
    if times:
        times  = times  + [datetime(2026, 6, 27, 14, 0)]
        status = status + [status[-1]]
        opens  = opens  + [opens[-1]]

    # Step fill: red = open (1), green = closed (0)
    for i in range(len(times) - 1):
        color = "#d62728" if status[i] == 1 else "#2ca02c"
        ax.fill_between([times[i], times[i+1]], [0, 0], [1, 1],
                        step="post", color=color, alpha=0.75)

    ax.step(times, status, where="post", color="#333333", linewidth=0.8)

    # Cumulative open count on right axis
    ax2 = ax.twinx()
    ax2.step(times, opens, where="post", color="#1f77b4",
             linewidth=1.0, linestyle="--", alpha=0.7)
    ax2.set_ylabel("cumulative opens", color="#1f77b4", fontsize=8)
    ax2.tick_params(axis="y", labelcolor="#1f77b4", labelsize=7)
    ax2.set_ylim(bottom=0)

    total_open  = sum(1 for s in status[:-1] if s == 1)
    total_close = sum(1 for s in status[:-1] if s == 0)
    n_open_events = opens[-1] if opens else 0

    ax.set_ylim(-0.15, 1.35)
    ax.set_yticks([0, 1])
    ax.set_yticklabels(["closed", "open"], fontsize=9)
    ax.set_ylabel(sensor, fontsize=9, fontweight="bold")
    ax.set_title(
        f"{sensor}  |  {len(times)-1} uplinks  |  "
        f"{total_open} open / {total_close} closed uplinks  |  "
        f"{n_open_events} cumulative open events",
        fontsize=8, loc="left", pad=3,
    )
    ax.grid(axis="x", linestyle=":", alpha=0.4)
    ax.spines[["top", "right"]].set_visible(False)

cur.close(); cn.close()

# X-axis formatting
locator = mdates.AutoDateLocator()
formatter = mdates.ConciseDateFormatter(locator)
axes[-1].xaxis.set_major_locator(locator)
axes[-1].xaxis.set_major_formatter(formatter)
fig.autofmt_xdate(rotation=0, ha="center")

# Legend patches
from matplotlib.patches import Patch
legend_elements = [
    Patch(facecolor="#d62728", alpha=0.75, label="open"),
    Patch(facecolor="#2ca02c", alpha=0.75, label="closed"),
]
fig.legend(handles=legend_elements, loc="upper right", fontsize=8, framealpha=0.7)

plt.tight_layout(rect=[0, 0, 1, 0.95])
plt.savefig(OUT, dpi=150, bbox_inches="tight")
print(f"Saved {OUT}")
