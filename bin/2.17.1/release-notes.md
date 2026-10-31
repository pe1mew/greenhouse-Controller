# Release 2.17.1

**Date:** 2026-10-30
**Built on:** 2.17.0
**Changes:** the follow-ups from the 2.3.1 vs 2.17.0 release comparison: T17's stack, the GUI's commissioning poll, the ROTA high-water mark, the Q3-overflow log row, and an idle abort on a unit with no sensor
([gh#97](https://github.com/pe1mew/greenhouse-Controller/issues/97) to [gh#103](https://github.com/pe1mew/greenhouse-Controller/issues/103))

**Patch. Bug fixes only:** no setting, key or payload change. The log gains one `LOG_SYSTEM` subtype, 33, in place of an encoding that collided with the others.

> **Soaked on 2344 (bench build of the same code): PASS. Not published.** **The ROTA change has not run:** it needs a publish, so it needs the operator's go.

## What changed

- **T17's stack goes from 4096 to 5120 B** (gh#97). A full characterisation run on 2344 left 724 B free, under T1's 1 KB warning line. It costs about 1 KB of internal heap.
- **T1's stack sweep covers T16 and T17** (gh#97). It is serial-only, as it always was; the TSDS said otherwise and is corrected. Bench builds also report it on `GET /api/diag/windowpos?stacks`, because reading it otherwise needs USB.
- **The GUI's commissioning poll runs only for an admin on the Motors tab of a unit with a sensor** (gh#98). It ran every second for anyone on any tab, each call renewed the session, and an anonymous call crossing a login could leave the page looking logged out. `showTab()` polls once on opening the tab.
- **The ROTA high-water mark is written by T13 at the bank switch** (gh#99), through `ota_assets_end_ex(seq)`, and no longer when T13 is spawned. A T13 failure used to make the unit refuse that release for good; now T16 retries it. A crash under 30 s still rolls back with the mark set, so a bad release is not re-offered. Row 24/0 now means "handed to T13"; rows 16 and 17 say how it ended.
- **Q3 overflow is `LOG_SYSTEM` 33, count in `value_b`** (gh#100). The count used to go in `value_a`, where it read as whichever subtype it equalled. `log/logparser.py` also decodes −1 and −2 correctly and tells T10's two 19 rows (DHCP renew, STA reassociate) from a legacy heartbeat. `bin/sd_soak.py` fails on 33.
- **A unit with no sensor no longer shows a warning about Linear** (gh#101): the Motors line is information, as FR-WPF05 asks. **`ctrl_mode_m3` stays 1 by default**, as FR-WPF07 requires; the comment and the FRS said a default could not reach a unit by OTA, which is wrong for a key the unit never had.
- **An idle abort is refused on a unit with no sensor** (gh#102). It put two timeouts on the bus for a device that is not there. The abort of a run still going is kept.
- **Documentation drift** (gh#103), including the lost `[2.15.0]` changelog heading. The same pass corrected the release comparison's two wrong recommendations.

### Who reads and writes what

Found with grep on 2026-10-30.

| Item | Enumerated |
|---|---|
| `ota_assets_end()` / `_ex()` callers | T16 `ota_client.cpp` (`_ex`, the seq); T11 `web_server.cpp` (a push, plain, no mark) |
| `fw_hiwater` | Written only by T13 now; read only by T16 `rota_handle_update()` |
| `LOG_SYSTEM` 33 | Emitter T9 `event_logger.cpp`, the only one. Consumers `log/logparser.py`, `bin/sd_soak.py`. Codes 34–99 stay free |
| `commPoll()` callers | The gated 1 s timer; `showTab()`; `applyWposFitted`, `commAct`, `charStart` (direct, unguarded on purpose) |
| The not-fitted gate on `/api/diag/commission` | Firmware route and the mock's copy of it |

## Verification

| Check | Result |
|---|---|
| Release image | 1 435 152 B, sha256 `987097f24d239f98…`, version `2.17.1`. No bench routes. Assets `web-assets-2.17.1.zip`, 174 833 B, sha256 `eb51707d9ab21e1c…` |
| Bench image on 2344 | `c911d5def866f20e…`. `fw_ver` and `asset_version` 2.17.1-bench |
| **Stacks, full characterisation on 2344** (bench, before the T17 change) | 111 starts, 57 min, band 32 mm. Unused: **T17 724 of 4096**, T7 1072 of 3072, T6 3124 of 6144, T2 5876 of 8192, T8 4920 of 8192, T16 11564 of 16384. No reboot |
| T17 at 5120 B | 2136 B free after the 12 h soak, **1752 B free after a full characterisation** (123 starts, 63 min). Others after that run: T7 1072 of 3072, T6 2888 of 6144, T2 5908, T8 4888, T16 11516 |
| **Soak on 2344, bench build** | 12.25 h from 2026-10-30 16:33: 74 judged strokes, every counter 0, no reboot, no coredump, largest T9 gap 31 s, no SYSTEM -1 or 33 row. 12 of 12 LCD sessions. 7 of 12 stroke sessions ran; 5 skipped, too cold to open. 14 decisions conform, no hunt, config identical. Heap at 12 h: 64 KB free, 31 KB largest |
| GUI on the mock | Anonymous 0 polls in 3.5 s; admin on another tab 0; admin on Motors with a sensor about 1 a second; no sensor, polling off. The not-fitted line renders as information. No script errors beyond the expected anonymous 401s |
| gh#102 on 2344 | `at_wpos_fitted.py unfitted`: all checks PASS, including "an idle abort is refused". An abort on a fitted unit still works (mock). The rig's settings restored afterwards, gate ok. **No fail-first run:** that would have needed the old image |
| Parser fixture | Eight rows decode as intended: −1, −2, 33, 19/0, 19/1, an old-form overflow of 40, a legacy heartbeat, and a WEB coredump 19 |
| `bin/check_cfg_desc.py` | 52 keys, 41 published, all agree |
| Bus at rest on 2344 | 3 min: addr 1, 40 and 44 all 100 % |
| Q3 overflow forced on 2344 (gh#100) | Bench build `7800c769`, 64 rows into the 32-row queue by `GET /api/diag/windowpos?q3flood`. Row `33,31` on the card, 33 rows kept, so all 64 accounted for. The parser reads it, `sd_soak.py`'s check fails on it. The route is bench-only |
| GUI poll on 2344, not logged in (gh#98) | 0 commissioning calls in 50 s, 0 in 8 s on Motors |
| GUI poll on 2344, logged in as admin (gh#98) | On 2.17.2-bench, 2026-10-31, the same poll code: 8 calls, all 200. One on each opening of Motors, none on another tab, about one a second on Motors. Timings read off the browser's waterfall |

## Upgrading

- **Nothing to do.** No setting, key or NVS record changed.
- **On a unit with a sensor nothing visible changes**, apart from the commissioning card refreshing only on the Motors tab.

## Known limitations

- **Not published.** The soak ran on the bench build; the release image has only been booted.
- **Two brief sensor faults in the soak, on the rig's EMULATED slaves.** Two failed polls in a row on addr 1 (23:33) and on addr 44 (02:33, which also raised wind override), each cleared in the same second. The three soaks before it had no such run. Addr 40, the real encoder, read 0 failures, and the bus code has only comment changes since 2.17.0, so the emulator is the likely source. That is not proven.
- **The ROTA high-water change has not run on hardware.** Its fail-first offers a release with a DEFLATE zip, which needs a publish, and a crash-looping image for the regression. Until then it rests on reading the code. It takes effect only for updates applied *from* a fixed image.
- **An old overflow row of 1–33 stays read as that subtype.** It cannot be told apart.
- **The −1 and −2 rows still rarely reach the card:** T9 has marked it gone before they are drained (gh#100).
- **A cookie's `Max-Age` is set only at login**, so a browser drops it at login + `session_timeout_min`, even for an active operator (gh#106, not reproduced, not fixed).
