# 2.0.0-a.6.32 — T1 full instrumentation (first alpha of the maturation plan)

First alpha of the maturation plan `design/maturationPlan_alpha6.32-6.35.md`. Restores the four T1 features that were deferred when alpha.6.22 first shipped T1 minimal.

Also marks the **switch in the version-naming convention**: `alpha` → `a`. From this tag onwards every pre-release uses the shorter prefix (`2.0.0-a.6.32`, `2.0.0-a.6.33`, …, `2.0.0-rc.1`). Historical alphas in the changelog keep their original full form for traceability.

## What landed

| Feature | Implementation summary | Cost |
|---|---|---|
| **NeoPixel** | `espressif/led_strip@^2.5.3` managed component (declared in new `firmware/src/idf_component.yml`). Driver pulled into `firmware/managed_components/espressif__led_strip/`. T1 calls `led_strip_set_pixel` + `led_strip_refresh` every 500 ms tick |
| **EG1 priority colour** | Aligned with `ui_display.cpp::status_colour_for_bits` so LED and LCD backlight never disagree. MOTOR_ALARM/WIND_OVERRIDE → RED, SENSOR_FAULT_T/_W → AMBER, CALIBRATING → BLUE, else GREEN |
| **Day/night dimming** | `cfg.led_day_brt` / `cfg.led_nite_brt` / `cfg.led_nite_from` / `cfg.led_nite_to` from T4's cfg_shadow. Brightness applied at R/G/B component level (`scaled = (raw * dim) >> 8`) — explicitly NOT via `led_strip_set_brightness` (re-scaling the internal pixel buffer per call would degrade values over many day↔night transitions) |
| **60 s heap rows** | `LOG_SYSTEM value_a=7` (free internal kB), `=8` (free PSRAM kB), `=12` (largest internal contiguous block kB) posted to Q3 → T9 → SD CSV at `tick % 120 == 0` |
| **30 s-offset heap-integrity** | `heap_caps_check_integrity_all(panic=false)` at `tick % 120 == 60`. Failure logs `value_a=9, value_b=0`. Non-panicking — T15 supervisor (when re-enabled) will handle escalation |
| **10-min stack-HWM** | `uxTaskGetStackHighWaterMark()` sweep at `tick % 1200 == 0` over T1..T15 handles. Serial-only (too noisy for SD); LOGW if hwm < 1024 B |

## What changed

- **`firmware/src/watchdog/watchdog.cpp`** — ~210 lines replacing the minimal body. New module-level state `s_strip` (led_strip handle). Tick-modulo dispatch table for heap rows / integrity / stack-HWM. Pulls in `data_manager.h` (for `dm_cfg_snapshot`), `event_logger.h` (for `log_post`), `types/app_types.h` (for EG1 + task_tN handles), `pin_config.h` (for PIN_RGB_LED).
- **`firmware/src/watchdog/watchdog.h`** — internal-only update to the design comments (function signature unchanged).
- **`firmware/src/main.cpp`** — T1 stack bump 4096 → 6144 bytes for the heap walk headroom.
- **`firmware/src/CMakeLists.txt`** — added `espressif__led_strip` to REQUIRES.
- **`firmware/src/idf_component.yml`** — new file declaring `espressif/led_strip: "^2.5.3"`.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-a.6.32` (new naming convention).

## led_strip 2.5.3 API note for future migrators

The 2.5.x and 3.x lines of `espressif/led_strip` use **different** config-struct field names:

| Version | Pixel-format field |
|---|---|
| 2.5.x | `led_pixel_format` (enum `LED_PIXEL_FORMAT_GRB`) |
| 3.x | `color_component_format` (enum `LED_STRIP_COLOR_COMPONENT_FMT_GRB`) |

We pin 2.5.3 (^2.5.3) in `idf_component.yml`. If a future bump to 3.x is wanted, watchdog.cpp's `strip_cfg` literal needs the field rename.

The first build attempt of this alpha hit this mismatch — I'd written the 3.x form against 2.5.3's headers. The IDF component manager's caret-range fetched 2.5.3 cleanly; only the field name differed. Build error was unambiguous: `'led_strip_config_t' has no non-static data member named 'color_component_format'`.

## Acceptance — hardware verified on 192.168.20.160

```
fw_ver        : 2.0.0-a.6.32
asset_version : 2.0.0-a.6.32     (paired bundle uploaded, mismatch cleared)
uptime_s      : 11               (post-OTA-reboot)
ota_status    : state=idle, accepted=true   (T1 ota_mark_healthy still works)
```

Heap rows at uptime=120 (first `% 120 == 0` tick after the minute-mark) on the *previous* boot, captured before the asset OTA forced a reboot:

```
20:51:10 SYSTEM SYS 0 0 7,103    ← free internal = 103 KB
20:51:10 SYSTEM SYS 0 0 8,8153   ← free PSRAM    = 8153 KB (≈ 8 MB)
20:51:10 SYSTEM SYS 0 0 12,31    ← largest block = 31 KB
```

No `value_a=9` (heap corruption) events — `heap_caps_check_integrity_all` passed at `tick % 120 == 60`.

**Pre-existing observation (not caused by this alpha):** largest-block sits at 31 KB. That's below the 50 KB threshold proposed as the gh#23 acceptance criterion for a.6.35. Pre-existing pattern in the firmware (no a.6.32 changes touch the heap-allocation path). Will be watched closely once a.6.35 lands the canonical-JSON + log-upload work that stresses the mbedTLS heap.

**Stack-HWM sweep** runs every 10 min — not visible in 80 s of post-flash uptime. Will appear on the next bench probe with uptime > 600 s.

**NeoPixel visual** — requires physical inspection. Expected: brief amber during boot calibration (first ~30 s), steady green afterwards. Day brightness 200/255 ≈ 78 % during 06:00–22:00 local, night brightness 20/255 ≈ 8 % during 22:00–06:00.

## Build delta vs a.6.31

| Metric | a.6.31 | a.6.32 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 321 312 B | **1 343 120 B** | +21 808 B |
| RAM static | ~60 256 B | 60 488 B | +232 B |

bin sha256: `C4268BE05B51CF0D…`

**+21 KB flash** — much larger than my plan estimate of +6 KB. Breakdown:
- led_strip managed component: **~12 KB** (vs my plan's 3 KB estimate — I was too optimistic; the component pulls in the RMT TX driver bytecode)
- New T1 body (~210 lines of C++ with heap walks + log_post + dm_cfg_snapshot): **~4 KB**
- Stack bump from 4096 → 6144 bytes: pre-allocated FreeRTOS task stack ≈ 2 KB more (RAM not flash)
- Misc linkage (esp_heap_caps + new includes): **~5 KB**

Final flash usage: **64.0 %** of the 2 MB OTA bank. Comfortable.

Updated total-flash budget projection for the maturation plan: a.6.35 will land at **~1.36 MB** (vs my +13.6 KB original estimate, now likely +35 KB once led_strip overhead is counted). Still well under the 1.50 MB hard cap. Migration plan's 1.30 MB target needs explicit re-baselining — flagging in `maturationPlan_alpha6.32-6.35.md` for the next document revision.

## Carried forward

- gh#23 mbedTLS mitigations + T15 supervisor — still out of scope per the maturation plan
- Phase 7 14-day soak — still deferred to post-a.6.35
- The 1.30 MB flash-budget line in the migration plan needs re-baselining

## Next

**a.6.33** — T10 24h NTP resync + LOG_SYSTEM event_a=3 (AP) / event_a=4 (geo). Per the plan, this is the smallest of the four maturation alphas (~+1.2 KB flash, low risk).
