# 2.1.3 — release notes

**Date built:** 2026-07-08
**Built on top of:** 2.1.2 (gh#36 — SD log listing truncation)
**Closes:** **gh#37** — system clock follows a failing DS1307 over NTP
**Scope:** 1 function changed in `firmware/src/data_manager/data_manager.cpp`; new `LOG_SYSTEM value_a=21` audit code; `log/logparser.py` updated to decode it.

---

## The problem this addresses

T4's main loop called `read_rtc_and_seed_clock()` every ~60 s, which unconditionally did `settimeofday()` from the DS1307 RTC. The chip therefore outranked SNTP: every hourly SNTP correction was overwritten within a minute by whatever the RTC said.

**Observed on 2344 (2026-07-08):** the unit's DS1307 was already losing ~40 s/hour on Jul 7, froze overnight (multiple SD rows stamped exactly `00:50:52` — halted oscillator), restarted ~05:27 permanently 4 h 35 m behind, and dragged the NTP-correct system clock back every 60 s. Symptoms: unit clock 09:31 at real 14:06 with `ntp_synced=true`; hourly ±16 500 s see-saw in the SD log; sunrise/sunset window shifted 4.6 h; corrupted log timestamps. The TN4 handler rewrites the DS1307 after every SNTP sync, but the writes did not hold on the failing chip.

**Operational consequence beyond cosmetics:** daylight-gated control behaviour ran on a clock 4.6 h wrong, and the planned internet-pull OTA (remoteOTAstudy.md stage 0, TLS certificate date checking) requires trustworthy wall time — this bug class had to be fixed first.

---

## What changed

### `firmware/src/data_manager/data_manager.cpp` — trust inversion in `read_rtc_and_seed_clock()`

- **NTP not yet synced** (`nm_is_sntp_synced()` false — boot window, no-internet operation): unchanged — the DS1307 seeds the system clock; it is the best available source.
- **NTP synced:** the system clock is authoritative and the DS1307 is **never applied**. If `|DS1307 − system| > 10 s` (`RTC_DIVERGENCE_WARN_S`), T4 emits `LOG_SYSTEM value_a=21` with `value_b` = divergence in seconds (clamped int16), rate-limited to ~1 row/hour (`RTC_DIVERGENCE_LOG_CALLS`), plus an `ESP_LOGW`. A persistent stream of these rows in the SD log = replace the RTC/battery.
- The MX4 shadow (`current_unix_ts`) and sun-time recompute now always follow the authoritative clock.

### `firmware/src/event_logger/event_logger.h`

`value_a=21` added to the LOG_SYSTEM table.

### `log/logparser.py`

Decodes `value_a=21`: `RTC divergence: DS1307 is +16504 s vs NTP-synced clock (DS1307 ignored — check RTC/battery)`.

---

## What this does NOT change

| Subsystem | Reason |
|---|---|
| Boot-time RTC seed | Unchanged — DS1307 still seeds before SNTP completes |
| TN4 (write NTP time → DS1307 after sync) | Unchanged — the chip is still corrected after every sync |
| `dm_set_manual_time()` | Unchanged — manual time set still writes both clocks |
| No-internet units | Unchanged behaviour — without SNTP the latch stays false and the DS1307 keeps seeding |
| SD log format | New value_a code only; row structure identical |

---

## Build artefacts

| File | Size | SHA-256 |
|---|---:|---|
| `greenhouse-controller-2.1.3.bin` | 1 360 528 B | `92ab41161fd700a0acaad9162f6c308cc0371bc2693c74376407cc3195f0e545` |
| `web-assets-2.1.3.zip`            |   108 073 B | `e42a9cb87599ce72efccc3b67399d8c9e2d33327f5be03d352a23e04d5f86786` |
| `bootloader-2.1.3.bin`            |    22 528 B | `ae4b50f04fdf8d9f837c96b20b1b7e7537af3e7cb8d7baf6fe735ebfc9d11367` |
| `partitions-2.1.3.bin`            |     3 072 B | `18fbe59ac37567be8897bc7f5266aec2ba2df85934a3b8fb9b229d8e59e7e74d` (unchanged) |
| `firmware-2.1.3.elf`              |  12 796 KB  | (for coredump decoding) |
| `firmware-2.1.3.map`              |  10 762 KB  | (for coredump decoding) |

---

## Verifiable post-OTA

1. `GET /api/status` → both `fw_ver` AND `asset_version` read `2.1.3`. ✓ (confirmed on 2344)
2. On 2344 (DS1307 currently 4 h 35 m behind): `time_iso` matches real time within seconds after the first SNTP sync and **stays** correct across RTC poll cycles. ✓ (was 4 h 35 m wrong on 2.1.2 with `ntp_synced=true`)
3. `value_a=21` rows appear in the SD log (~1/h) while the sick DS1307 remains diverged — visible in the next daily log upload. *(Not yet observed at release time — the active SD file is not downloadable; check the next uploaded log.)*

---

## Open issues after this release

- **Hardware:** replace/check the DS1307 backup battery on 2344 — the chip froze and restarted 4 h 35 m behind on 2026-07-08; until fixed, hourly value_a=21 rows are expected (and harmless).
- **gh#7** — bug: serial-port WDT freeze
- **gh#27** — T15 heap-drop sampling timing
- **gh#32** — SD handling on LCD/keypad GUI
- **gh#35** — independent wind averaging window (`avg_win_wind`)
