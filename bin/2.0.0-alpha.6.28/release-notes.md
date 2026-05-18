# 2.0.0-alpha.6.28 — IP geolocation + timezone sync (Phase 6.14.X step 1)

## What landed

Restored the **IP-geolocation + timezone sync** that the minimal T10 deferred in alpha.6.14. The 1.20.3 implementation fetched lat/lon and IANA timezone from `http://ip-api.com/json` once per boot after WiFi+NTP came up, then pushed lat/lon to T4 via Q4 and applied the POSIX TZ string via `setenv`/`tzset`. Same behaviour now lives in `task_network_manager` using esp_http_client.

User report from alpha.6.27 acceptance:
> "coordinate as presented in web gui system tam shall be updated with the location lookup on IP that does not seem the case. coordinate is 52.0, 5.0 (default)"

The default 52.0, 5.0 is what `cfg_defaults.h` writes when NVS is fresh — central-Netherlands grid intersection, not a real geocoded location. With the geo sync restored every boot now resolves the public IP and updates the coordinates with real precision.

## Architecture

| Concern | Choice |
|---|---|
| Provider | `http://ip-api.com/json?fields=status,lat,lon,timezone` — same as 1.20.3 (free tier, HTTP only, ~99 % uptime, no API key) |
| Trigger | First T10 main-loop tick where `client_connected && ntp_synced` |
| Retry | `s_geo_done` latches on full parse success; subsequent ticks retry until success (covers DNS hiccups, rate-limit transients) |
| Storage | lat/lon: Q4 → T4 → NVS `system/lat_deg`, `lat_frac`, `lon_deg`, `lon_frac` (recalculates sunrise/sunset). TZ: direct `nvs_cfg_set_str(NVS_NS_SYSTEM, "tz_str", …)` + immediate `setenv`/`tzset` |
| IANA → POSIX | New header `firmware/src/network_manager/tz_table.h` — ~90 entries covering Europe (full incl. CET/CEST/WET/EET), Americas (US/Canada/Mexico/SA), Asia (Middle East/South/SE/East), Australia, Pacific, Africa (major). Ported verbatim from the 1.20.3 archive. |

The HTTP-not-HTTPS choice matches 1.20.3: ip-api.com's free tier doesn't support TLS, and the data is non-sensitive (worst case of a man-in-the-middle is sunrise calc drifts by a few minutes — operationally minor).

## What changed

- **`firmware/src/network_manager/network_manager.cpp`** — added ~140 lines:
  - `geo_resp_t` + `geo_http_event_cb` for body accumulation
  - `float_to_deg_frac` — float → integer deg + millidegree fraction (matches NVS schema)
  - `parse_geo_response` — strstr-based JSON extractor (verbatim port from 1.20.3)
  - `post_q4` — Q4 send helper (uses `strncpy` + explicit NUL per the alpha.6.18 format-truncation trap)
  - `do_geo_sync` — esp_http_client GET against ip-api.com, parses, writes NVS, applies TZ
  - `s_geo_done` latch + dual call sites in `task_network_manager` (initial sync after Q5 post + retry on every main-loop tick if still pending)
  - New includes: `esp_http_client.h`, `nvs_config.h`, `tz_table.h`, `<stdlib.h>` (for atof)
- **`firmware/src/network_manager/tz_table.h`** — new file, ~140 lines, IANA → POSIX TZ table.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.28`.

## Acceptance — hardware verified on 192.168.20.160

Pre-alpha.6.28 (with NVS defaults from a fresh unit or a wiped namespace):
```
lat_deg=52  lat_frac=0   lon_deg=5  lon_frac=0   →  52.000 N, 5.000 E
```

After alpha.6.28 boot + geo sync:
```
lat_deg=52  lat_frac=218  lon_deg=5  lon_frac=939  →  52.218 N, 5.939 E
tz_str = "CET-1CEST,M3.5.0,M10.5.0/3"
```

The 218 milli-degree fraction puts the unit ~24 km north of the 52.000 grid line; 939 milli-degrees east of the 5.000 line puts it ~65 km east. That's central-east Netherlands, consistent with the public IP's actual physical location. Coordinates persist across reboots (NVS) so a network outage between syncs doesn't lose the geocoded location.

## Build delta vs alpha.6.27

| Metric | alpha.6.27 | alpha.6.28 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 307 408 B | 1 318 976 B | +11 568 B |
| RAM static | ~60 256 B | (~similar) | unchanged |

+11.6 KB flash. Roughly: TZ table ~6 KB, do_geo_sync + helpers ~1 KB, plus any new lwIP/HTTP-over-TCP linkage pulled in by esp_http_client's HTTP_TRANSPORT_OVER_TCP path (status_post had only used HTTP_TRANSPORT_OVER_SSL until now).

bin sha256: `C03F761512223009…`

## Paired asset bundle

`bin/build_release.ps1` rebuild produced `web-assets-2.0.0-alpha.6.28.zip` (STORE-only, 91 414 B) — uploaded via `POST /api/ota/assets`. Post-reboot `/api/status` reports `fw_ver == asset_version == 2.0.0-alpha.6.28`; dashboard MISMATCH badge cleared.

## Deferred from Phase 6.14.X — still to land

The geo/timezone sync lands here. Three Phase-6.14.X items remain:
- **AP fallback** — soft-AP captive portal when STA can't connect (currently relying on esp_wifi's auto-reconnect)
- **Exponential backoff** — explicit 2 → 4 → 8 → 16 → 32 → 60 s state machine (currently fixed-cadence)
- **Periodic 24 h NTP resync** — DS1307 RTC has been precise enough so far; revisit if Phase 7 soak shows drift

Each is operationally bounded; we can ship 2.0.0-rc.1 without them and add them in a 2.0.1 patch if the soak surfaces a need.
