# campaign-fall-2026 — 5C88's SD logs from the end of the summer campaign

| Field | Value |
|---|---|
| Unit | **5C88** (unit_id 23688), the production controller at Herenboeren Willemshoeve |
| Data | 3 SD logs, 2026-09-17 22:17 → 2026-09-23 07:30 local, 15 421 samples at 30 s, 25 MODE rows, no reboot |
| Plots | `plot_2026-09-17.png` … `plot_2026-09-23.png`, `plot_summary.txt` |
| Regenerate | `python model/campaign-summer-2026/plot_daily.py --dir model/campaign-fall-2026` |

## Why this directory exists

The summer campaign is closed: **2026-06-04 → 2026-09-17, 296 678 samples**, the figure every campaign document quotes (`campaignResults_summer2026.md`, `thermalProfileCampaign.md` §9.12, `closedloop/README.md`).

**Its directory is a glob, not a date range.** `closedloop/logdata.py` globs `campaign-summer-2026/*.log` and `closedloop/settings.py` reads every SETPT row it finds there, so a new log dropped in extends the campaign silently. With these three files still in it, `dataset.build()` returned **312 099 samples to 2026-09-23** instead of 296 678 to 09-17 — over a period the campaign has no outdoor data for. Moving them here restores it; the `gate-control` row for "All, Jun 4 – Sep 17" means Jun 4 – Sep 17 again.

## Where the boundary is

At the **summer campaign's last log**, so no file is split. The logs are continuous: each one starts where the previous ended, and `2026-09-17_201800.log` (summer's last) ends at 2026-09-17 22:17:42, which is where `2026-09-19_152025.log` starts.

The file **name** is the rotation time in UTC; the **rows** are the controller's local time, about two hours later. So a file's data ends around its own name and starts at the previous file's end.

**This is not the astronomical boundary.** The September equinox falls on 2026-09-23, so by that measure only the last file would belong here, and only its last few hours. The campaign boundary keeps whole files and matches where the summer analysis actually stops.

## What is here, and what is not

- **The logs and `config.json`.** The config is copied from the summer campaign: no SETPT row appears anywhere in these three files, so nothing was reconfigured in this window, and the plots' setpoint lines are the same ones the summer plots used.
- **No LoRa exports.** The campaign's outdoor T/RH/lux (`lht65_20`), door (`lds01_5`/`-6`) and indoor cross-check series all end 2026-09-17. **The closed-loop simulator cannot run on this period until they are fetched** (`model/fetch_lora_data.py`), because `dataset.py` joins the SD samples to them.
- **`plot_2026-09-17.png` is a part day:** 1 h 43 min, the tail after the summer campaign's last log ends. The rest of 2026-09-17 belongs to the summer campaign.

## What the week shows

Daytime maxima 29.0–30.8 °C on 09-18 to 09-22, nights down to 11.4 °C, humidity hitting 91–94 % on the cool nights at either end. M3 moved once, on 09-21: open at 12:59, shut again at 13:27. M1 drove 14 times and M2 10. There was no wind override and no reboot. `plot_summary.txt` has the per-day numbers.
