# Release 2.5.1

**Date:** 2026-09-12
**Built on:** 2.5.0
**Fixes:** [gh#57](https://github.com/pe1mew/greenhouse-Controller/issues/57) part 2 — the
poll-interval range now matches the requirement it always claimed to

Patch: a bounds correction plus three documentation fixes. No payload change, no new NVS
key, no new behaviour.

## The conflict was not a stand-off

gh#57 recorded `poll_interval` as a decision to be made: FR-S03 and FR-CF07 are both
"Must" and both say **15–120 s**, while `cfg_limits.h` said **30–300**, so either amend
the requirements or change the code. Reading further showed there was nothing to decide.
**The code already implemented the requirement at the only place that sets the cadence.**

`sensor_poll.cpp` defines `SP_POLL_MIN_S = 15` and `SP_POLL_MAX_S = 120`, and clamps to
that range on every loop pass, four lines above the `vTaskDelay` that sets the interval:

```c
int32_t poll_s = dm_get_poll_interval_s();
if (poll_s < SP_POLL_MIN_S) poll_s = SP_POLL_MIN_S;
if (poll_s > SP_POLL_MAX_S) poll_s = SP_POLL_MAX_S;
vTaskDelay(pdMS_TO_TICKS((uint32_t)poll_s * 1000u));
```

Six places state the range. Only one said 30–300:

| source | range |
|---|---|
| FR-S03, FR-CF07, both **Must** | 15–120 |
| TSDS, five separate places, citing FR-CF07 | 15–120 |
| `sensor_poll.cpp` `SP_POLL_MIN_S`/`MAX_S` — **the actual poller** | 15–120 |
| `sensor_poll.cpp` file header comment | 15–120 |
| `beheerderHandleiding.md` §12.4 advice lines | 15–30 / 60–120 |
| `cfg_limits.h`, and the clamp, limits API, GUI slider and tooltip derived from it | **30–300** |

Git decides which one drifted. `cfg_limits.h` was created whole in `v1.16.25`
(2026-05-07, 69 insertions, new file), and `SP_POLL_MIN_S = 15` is already present in
that commit's parent. The file whose header calls it "the single source of truth for all
integer config parameter bounds" is the newest statement of this bound, and it diverged
from both the requirement and the already-shipped poller. Its recorded rationale, *"below
30 s provides no benefit for greenhouse dynamics"*, was written in the same commit that
introduced the divergence. It is an opinion in a comment, not a measurement.

## What the divergence cost

The 300 s ceiling was not harmless slack. Two things went wrong whenever a value above
120 was stored:

- **A silent cap.** The unit polled at 120 s while `/api/config` reported 300, the GUI
  showed 300, and the audit log recorded 300. FR-LG09 ties the log snapshot cadence to
  the poll interval, so the log's own row spacing contradicted the configuration it sat
  next to.
- **Averaging silently disappeared.** T5 sizes the sliding-average window from the *raw*
  shadow value, not the clamped one. At a stored 300 the temperature window became
  `(6 * 60) / 300 = 1` sample by integer division, where the real 120 s cadence called
  for 3. A 6-minute average degenerated to the raw reading, which is exactly the noise
  rejection FR-S06 asks for. **This is filed separately — see "Known, filed separately"
  below.**

## On the averaging argumentation

The coupling is `window samples = avg_win_minutes * 60 / poll_interval_s`, clamped to
1–360. The recorded rationale for 30 s is about the **default**, not the floor:
`DEF_POLL_INTERVAL_S = 30` is annotated *"doubles smoothing-buffer depth at same
time-window"*, meaning 30 s was chosen over 60 s to get 12 samples in a 6-minute window
instead of 6. That argues for the default and says nothing against a 15 s floor.

The buffer costs nothing either way: `SP_AVG_DEPTH` is a fixed 360 slots per channel,
allocated regardless. A 30-minute window at 15 s needs 120 slots, a third of what exists.
360 slots is enough for a 60-minute window at a 10 s poll, which is *wider* than either
the FRS or `cfg_limits.h` ever allowed — more evidence the buffer was sized to the
requirement.

## Changes

- `CFG_MIN_POLL_S` 30 becomes **15**; `CFG_MAX_POLL_S` 300 becomes **120**. Both now
  carry a comment naming FR-S03/FR-CF07 and the matching `sensor_poll.cpp` constant.
- **GUI tooltip, wrong twice over.** It read "Range: 30–300 s. Default: 60 s. Takes
  effect after reboot." The default has been 30 s, and **no reboot is needed** — T5 calls
  `dm_get_poll_interval_s()` at the top of every loop pass. Now reads "Range: 15–120 s.
  Default: 30 s. Takes effect on the next poll cycle — no reboot needed."
- **`beheerderHandleiding.md` §12.4** repeated both errors: a 30–300 s table row and a
  "**Reboot vereist** na wijziging" note. Both corrected, and the correction says what
  the old text claimed so an operator who remembers it is not left guessing.
- **`beheerderHandleiding.md` §12.6** listed seven event types and omitted `SENSOR_HR`
  and `SUN` (both since rc.1.4.0) and `PIN_AUTH` (2.5.0). Now complete, with `SENSOR`
  marked as the pre-rc.1.4.0 form that `logparser.py` still reads for old archives.
- `webUiMock/mock_server.py` follows the new bounds.

## Verification

**Built clean** (`pio run -e lolin_s3`, `-Werror`), 66.0 % flash.

**Static pre-flight.** The limits literal expands to valid JSON: 40 keys, 851 bytes, no
malformed ranges, and the mock matches the firmware key-for-key with nothing differing.

**OTA to FDA4 confirmed on both partitions** — `fw_ver = 2.5.1` *and*
`asset_version = 2.5.1` read back after the reboot.

**On FDA4, 2.5.1.** The fail-first baseline is 2.5.0, which this unit ran until minutes
earlier: a POST of 15 was clamped *up* to 30, and a POST of 300 was stored verbatim.

| POST | stored | note |
|---|---|---|
| 15 | 15 | the FRS floor, **unreachable before 2.5.1** |
| 10 | 15 | below the floor |
| 121 | 120 | above the ceiling |
| 300 | 120 | **was stored verbatim before 2.5.1** |
| 120 | 120 | the ceiling, exact |
| 45 | 45 | in range, verbatim — the clamp is not flattening |

`/api/config/limits` returns `poll_interval: [15, 120]` and still 40 keys.

**A 15 s interval is genuinely live, and needs no reboot.** FR-LG09 ties the log snapshot
cadence to the poll interval, so the `SENSOR_HR` row spacing on the SD card is the
observable. At a stored 15 the last eight gaps read:

```
15, 15, 16, 15, 15, 16, 15, 15     (seconds between SENSOR_HR ch0 rows)
```

with uptime climbing from 375 s to 587 s across the measurement, so no reboot occurred.
Restoring 30 returned the cadence to `30, 31, 30, 30, 31, 30`. `eg1` stayed 0 throughout
and every value the test moved was restored and re-read.

**One measurement caveat, recorded because it produced a false failure first.** T5 reads
the interval at the *top* of its loop and only then sleeps, so a change takes effect after
the in-flight sleep drains — worst case one whole **old** interval. The first attempt
measured 100 s after setting 15 while a 120 s sleep from the clamp tests was still in
flight, and read `[..., 31, 76, 31, 15]`. The firmware was right; the wait was too short.

## Known, filed separately

T5 sizes the averaging window from the **raw** stored `poll_interval` (`poll_s_cfg`,
`sensor_poll.cpp:418`) rather than the clamped `poll_s` (`:404`). Narrowing the config
bounds makes the two identical across the whole legal range, so no new unit can reach the
bad state. But `nvs_load_system()` reads the key with `nvs_cfg_get_i32_or_default()` and
**does not clamp on load**, so a unit still holding a legacy value outside 15–120 keeps it
across a boot and would compute its averaging depth from a cadence it does not use. Filed
as its own issue rather than folded in here, so that fix carries its own hardware
evidence. FDA4 holds 30; 5C88 cannot be read from here, and both its factory default and
the manual say 30.

## Not changed

The averaging-window range is untouched. FR-CF17 and FR-S07 say **1–60 minutes** and
`cfg_limits.h` says **1–30**; the TSDS says 1–60 in two places. That is the same
divergence shape in the same coupled pair, and it is recorded in the issue above rather
than decided here.

## Upgrade notes

No NVS migration, no partition change.

**For anyone driving `POST /api/config`:** `poll_interval` now clamps to 15–120 instead of
30–300, and `GET /api/config/limits` reports the new pair. A caller that was setting a
value above 120 was already being polled at 120; it will now see the clamped value
reported back honestly.

**No soak yet on this build at time of writing.** Nothing is promoted to `mainstream`;
5C88 stays on 2.3.1 pending the wider defect review.
