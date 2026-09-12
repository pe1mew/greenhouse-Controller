# Release 2.6.0

**Date:** 2026-09-12
**Built on:** 2.5.1
**Fixes:** [gh#54](https://github.com/pe1mew/greenhouse-Controller/issues/54) and
[gh#59](https://github.com/pe1mew/greenhouse-Controller/issues/59) — the audit log stops
lying, and starts recording

**Minor, not patch.** A new `log_param_id_t` value and six new `LOG_SYSTEM` subtypes are
both payload-shape changes. Every consumer learns them in this same changeset.

## gh#54 — every STANDBY transition was read as a ventilation decision

`LOG_MODE_CHANGE` has two unrelated emitters. Both carried `param_id = 0`, so all three
consumers decoded the second as the first:

```
raw     2026-09-12T10:40:39,MODE,WEB,0,0,1,0      <- STANDBY entered via /api/mode
parser  10:40:39  [MODE]  Web UI   Vent step -> 1 (M1 open)  [T-demand: all closed  RH-demand: all closed]
```

Neither happened. The "T-demand / RH-demand" pair is **fabricated**: it is `value_b = 0`
unpacked as two signed bytes, and the STANDBY emitter reserves `value_b` as zero. The
`RELAY` rows four seconds later said the opposite, so the log contradicted itself on
adjacent lines.

**Fix.** Emitter B now sets **`LOG_PARAM_MODE_STANDBY = 47`**. That separates the two
unambiguously and independently of `initiator` and `channel` — which matters, because
2.4.6 briefly made a SYSTEM/channel-0 STANDBY row byte-for-byte identical to a genuine
step-0 vent row, so a parser branching on initiator alone could not have worked.

Three consumers updated:

- `logparser.py` renders it as a STANDBY transition with the surface.
- `plot_daily.py` skips it, instead of plotting a phantom step change.
- `vent_step_replay.py` skips it. This was the one that mattered most: `CLAUDE.md` points
  at that tool for *"judging whether climate control is misbehaving"*, and it refuses to
  project unless it first reproduces ≥ 90 % of the logged T-demands. A STANDBY row
  injected a fabricated demand-free step into that gate, so it could fail a healthy
  configuration or shift a projection.

**Logs written before 2.6.0 cannot be separated** and will keep mis-rendering. That is
stated in `logparser.md` so a reader meeting an old log knows why.

## gh#59 — seven events that changed behaviour and left no trace

Each reached at most the serial console, which on a unit in a greenhouse is not
observability.

| `value_a` | event | payload |
|---|---|---|
| 25 | `is_daytime` flipped | `value_b` 0 = night, 1 = day |
| 26 | factory reset executed | `value_b` = IO0 level 1/2/3 |
| 27 | Q1 command discarded, motor alarm | hi byte action, lo byte source; `channel` = requested |
| 28 | DS1307 unreadable | `value_b` = `rtc_status_t` 1/2/3, rate-limited ~1/h |
| 29 | T6 move deferred on dwell | seconds remaining, **sign = direction**; `channel` = motor |
| 30 | Q4 write rejected, unknown key | `initiator` = the producer that tried |

Plus the seventh, found while verifying gh#58: **a successful web login wrote no audit row
at all.** `LOG_SESSION` came only from `ui_display.cpp`, so the log recorded who got in at
the panel and nothing about who got in over the network. Once 2.5.0 began logging *failed*
attempts from both surfaces that became actively misleading, because a run of
`PIN_AUTH`/WEB failures followed by silence could not be told from one followed by a
successful break-in. `session_open()` and `session_close()` now emit `LOG_SESSION` with
`LOG_BY_WEB`, reusing the LCD's exact encoding so the parser needed no new branch.

Three implementation decisions worth knowing:

- **The factory-reset row is written synchronously.** Level 3 calls `esp_restart()` a few
  lines later, which would cut T9 off before it drained Q3 — so the single
  highest-value row in the issue would have been the one most reliably lost.
  `event_logger_post_sync()` existed for exactly this and had **zero callers**; it now
  takes an `initiator` and `channel` so the row reads as the operator action it is.
- **Subtype 28 is rate-limited** on the same ~1/h budget as the existing divergence row.
  gh#59's volume check called these all edge events, but a dead DS1307 fails *every* poll.
- **Subtype 29 encodes direction in the sign** of `value_b` rather than spending a second
  subtype. Dwell remaining is always positive when deferring and capped at 1500 s, so the
  sign bit is free.

### A correction to gh#59's own premise

The issue said *"Next free `LOG_SYSTEM` `value_a` subtypes are 22 and upward (the
documented table in `event_logger.h` runs −1 and 0–21)"*. **22, 23 and 24 have been ROTA
check / download / apply since firmware 2.2.0.** They were never added to that table, and
they do not show up in the obvious grep because they go through a helper —
`audit_row(22, sub)` rather than `ev.value_a = 22`. `logparser.py` had decoded them all
along, so the parser was ahead of the firmware's own documentation.

Implementing the issue verbatim would have collided all three. The occupied set was
re-derived from the emitters and the parser, the new subtypes start at **25**, and the
table now documents 22–30 with a note saying the emitters are authoritative and the
comment is not.

## Verification

**Built clean** (`pio run -e lolin_s3`, `-Werror`), 66.0 % flash, 19.2 % RAM. The
`-Wmissing-field-initializers` warnings in the URI table are pre-existing.

**OTA to FDA4 confirmed on both partitions** — `fw_ver = 2.6.0` *and*
`asset_version = 2.6.0` read back after the reboot.

**Parser, before touching the unit.** A synthetic CSV exercised every new encoding plus
two no-regression controls (a genuine vent-step row, and a ROTA subtype-22 row). All
decode; the first run failed with `name 'ch' is not defined`, because the new subtypes 27
and 29 read a channel that `_decode_system()` never parsed. Fixed and re-run.

**The two model tools, differentially.** Rather than eyeball the skip, the same six-hour
synthetic log was analysed twice — once plain, once with 16 STANDBY rows added.
`vent_step_replay.py` produced **byte-identical output** and reported **18 MODE decisions
in both**, not 34. That count is the fail-first evidence inline: without the skip the
second run would have seen 34.

### On hardware, 12/15 — and the three failures were the harness

Three of the seven gh#59 events can be provoked over the network. Those are verified:

- **Subtype 25** appears twice, at `13:07:47` and `13:08:09` — once per OTA reboot, each
  on the `-1` sentinel — and its `value_b` agrees with `/api/status`'s `is_daytime`.
- **Web `SESSION` rows**: a farmer login and logout produced exactly two new rows,
  `value_a = 1` then `0`, initiator `WEB`.
- **gh#54**: `POST /api/mode` standby then automatic produced

  ```
  2026-09-12T13:09:36,MODE,WEB,0,47,1,0
  2026-09-12T13:09:40,MODE,WEB,0,47,0,0
  ```

  both `param = 47`, `value_a` 1 then 0, initiator `WEB` and not `SYSTEM`. T6's own rows
  in the same file still carry `param = 0`.

The three failures were all the harness, not the firmware, and are recorded in the gotcha
log as one pattern:

- `mode == AUTOMATIC` and `eg1 == 0` were checked 12 s after leaving STANDBY, while the
  `CMD_RECALIBRATE` sweep that leaving STANDBY *triggers* was still running with
  `EG1_BIT_CALIBRATING` set. Both settled clean on a proper wait.
- `ntp_synced` was false. The clock was correct, seeded from the DS1307, and the existing
  subtype-2 row records `value_b = 0`, a **timeout**, on both boots. Not a regression:
  nothing in this release touches SNTP, and the retry path is already designed for it
  (`NTP_RETRY_INTERVAL_S` is 300 s while never-synced, added in rc.1.5.6). FDA4 simply
  could not reach `pool.ntp.org` at that moment. Noted on gh#55 as evidence for the
  held time-source research, since during such an outage the DS1307 is authoritative and
  the gh#37 divergence protection is inactive by construction.

### Four events are NOT verified on hardware

Stated rather than assumed:

| subtype | why not |
|---|---|
| 26 factory reset | needs the IO0 button at the panel |
| 27 command discarded | needs a real RRK-3 alarm signal on GPIO42 |
| 28 RTC unreadable | needs the DS1307 to actually fail (gh#55) |
| 29 dwell deferral | needs T6 to want a move while a dwell timer runs — opportunistic, watch the soak |

**Warning before anyone tests subtype 26 on the M3 rig:** an IO0 reset wipes `travel_m3`
back to the production 171 and the boot calibration then drives M3 for 176 s on a 13 s
rig. Disconnect M3 first, or use level 1 only.

Subtype 30 is **unreachable by design** and that is the point: `/api/config` returns 400
before Q4, and every key the LCD and T10 send is known. It is defence in depth for a
future producer, in the same spirit as the 2.4.7 classifier.

## Upgrade notes

No NVS migration, no partition change, no config-key change.

**For anything that reads the audit log:** `MODE` rows now carry `param = 47` for STANDBY
transitions, and `SYSTEM` gains subtypes 25–30. `logparser.py`, `plot_daily.py` and
`vent_step_replay.py` handle all of it from this release. An older parser will show the
new subtypes as unknown rather than fail, but will still mis-render pre-2.6.0 STANDBY rows
and now correctly render post-2.6.0 ones only if it knows about param 47.

**No soak yet on this build at time of writing.** Nothing is promoted to `mainstream`;
5C88 stays on 2.3.1 pending the acceptance run.
