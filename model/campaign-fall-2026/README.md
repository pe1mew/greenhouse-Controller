# campaign-fall-2026 — 5C88's SD logs from the end of the summer campaign

| Field | Value |
|---|---|
| Unit | **5C88** (unit_id 23688), the production controller at Herenboeren Willemshoeve |
| Data | 8 SD logs, 2026-09-17 22:17 → 2026-10-02 06:22 local, 41 058 samples at 30 s, 174 MODE rows, one reboot (a power-on reset, 2026-09-28 15:10: `powerOutage28-9.md`) |
| Plots | `plot_2026-09-17.png` … `plot_2026-10-02.png`, `plot_summary.txt` |
| Regenerate | `python model/campaign-summer-2026/plot_daily.py --dir model/campaign-fall-2026` |

## Why this directory exists

The summer campaign is closed: **2026-06-04 → 2026-09-17, 296 678 samples**, the figure every campaign document quotes (`campaignResults_summer2026.md`, `thermalProfileCampaign.md` §9.12, `closedloop/README.md`).

**Its directory is a glob, not a date range.** `closedloop/logdata.py` globs `campaign-summer-2026/*.log` and `closedloop/settings.py` reads every SETPT row it finds there, so a new log dropped in extends the campaign silently. With these three files still in it, `dataset.build()` returned **312 099 samples to 2026-09-23** instead of 296 678 to 09-17 — over a period the campaign has no outdoor data for. Moving them here restores it; the `gate-control` row for "All, Jun 4 – Sep 17" means Jun 4 – Sep 17 again.

## Where the boundary is

At the **summer campaign's last log**, so no file is split. The logs are continuous: each one starts where the previous ended, and `2026-09-17_201800.log` (summer's last) ends at 2026-09-17 22:17:42, which is where `2026-09-19_152025.log` starts.

The file **name** is the rotation time in UTC; the **rows** are the controller's local time, about two hours later. So a file's data ends around its own name and starts at the previous file's end.

**This is not the astronomical boundary.** The September equinox falls on 2026-09-23, so by that measure only the last file would belong here, and only its last few hours. The campaign boundary keeps whole files and matches where the summer analysis actually stops.

## What is here, and what is not

- **The logs.** The first three end at 2026-09-23 07:30. Five more continue them to 2026-10-02 06:22, with `powerOutage28-9.md`, the analysis of the 2026-09-28 reboot.
- **`config.json` is the summer campaign's, with one correction: `rh_min_day` is 60, not 50.** 5C88's `rh_min_day` went from 50 to 60 on 2026-06-11 at 10:47:02 (web, `campaign-summer-2026/2026-06-11_142416.log:22541`); the summer copy predates that, and no later row changes it back. Until 2026-10-04 every plot here drew it at 50. Only the keys the plots draw (`t_*`, `rh_*`, `v_max`) were checked against the logs. The rest of the file is the summer snapshot as it was (its `ap_ssid` is 2344's).
- **One setpoint change is NOT drawn.** On 2026-09-26 at 12:05:09 `rh_max_day` went from 75 to 70 (web, `2026-09-26_192910.log:19214`). `plot_daily.py` draws `config.json`'s values as flat lines, so from that moment on the plots show `rh_max_day` at 75 while 5C88 used 70. The only other `SETPT` rows are the four geolocation rows (param 21) that the 2026-09-28 boot wrote.
- **No LoRa exports.** The campaign's outdoor T/RH/lux (`lht65_20`), door (`lds01_5`/`-6`) and indoor cross-check series all end 2026-09-17. **The closed-loop simulator cannot run on this period until they are fetched** (`model/fetch_lora_data.py`), because `dataset.py` joins the SD samples to them.
- **`plot_2026-09-17.png` is a part day:** 1 h 43 min, the tail after the summer campaign's last log ends. The rest of 2026-09-17 belongs to the summer campaign.
- **`plot_2026-10-02.png` is a part day too:** the last log ends at 06:22.

## What the week shows

Daytime maxima 29.0–30.8 °C on 09-18 to 09-22, nights down to 11.4 °C, humidity hitting 91–94 % on the cool nights at either end. M3 moved once, on 09-21: open at 12:59, shut again at 13:27. M1 drove 14 times and M2 10. There was no wind override and no reboot. `plot_summary.txt` has the per-day numbers.

## What the next nine days show (2026-09-23 07:30 → 2026-10-02 06:22)

- **Warm days, cold nights.** Daytime maxima 29.6–31.9 °C every day from 09-23 to 10-01, nights down to 7.5 °C (09-25), humidity at 94 % every night.
- **Far more switching.** 5–27 MODE rows a day, against 2–10 the week before. M1 opened 13 times, M2 52 and M3 18 (6 of them on 09-29, 7 on 09-30), against 7, 5 and 1 the week before.
- **A power cut on 2026-09-28.** 5C88 lost power for about 2¼ minutes and booted at 15:10 (a power-on reset); the boot calibration closed all three windows, and a T/RH sensor fault followed at 15:39–15:40. `plot_2026-09-28.png` has 2 859 samples instead of 2 864. `powerOutage28-9.md` has the analysis.
- **No wind override.** The setpoint change of 09-26 is above.
