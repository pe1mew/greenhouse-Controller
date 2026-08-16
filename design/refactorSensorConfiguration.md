# Sensor architecture refactor — study

**Status:** STUDY — nothing implemented, nothing else in the repo changed. Names, keys and IDs below are proposals.

**Date:** 2026-07-31 (restructured after review)
**Goal:** support a growing sensor set (external T/RH, window position, rain, CO₂, soil moisture) and give control tasks **one uniform way to reach sensor data**, with polling cadence driven by the consuming task.

> **Conclusion up front, after review.** The valuable work is the **internal architecture** — roles, a scheduled bus with priorities, and consumer-owned averaging. A **runtime configuration subsystem is not worth building now** and has been moved to §7 as a deferred option: every new sensor needs a driver compiled in anyway, so a JSON/NVS config layer would be substantial machinery whose headline benefit (adding a sensor without a firmware release) does not materialise. The sensor set becomes a **static table in code** (§3), with **one generic Modbus register driver** (§4) to keep the per-sensor cost low.

---

## 1. Where we are today

`sensor_poll.cpp` (T5) calls two drivers by name at fixed addresses:

```c
t_ok = (fg6485a_read_measurements(FG6485A_DEFAULT_ADDR, &tm) == FG6485A_OK);   /* addr 1  */
w_ok = (s200_read_measurements(S200_DEFAULT_ADDR,  &wm) == S200_OK);           /* addr 44 */
```

Everything downstream inherits that fixed shape:

| Layer | Today | Consequence |
|---|---|---|
| T5 poll loop | two `if` blocks, two drivers | adding a sensor means editing the loop |
| `sensor_reading_t` | fixed fields (`t_c10`, `rh_pct`, wind…) | no room for CO₂/rain/position without a struct change |
| EG1 fault flags | two bits: `SENSOR_FAULT_T`, `SENSOR_FAULT_W` | N sensors need N fault states; EG1 bits are finite |
| Averaging | computed in T5, window sized in *samples* | couples the filter to the poll interval (see §2.3) |
| Cadence | one global `poll_interval_s` (30 s) | cannot express "1 Hz while a window travels, 30 s otherwise" |
| Status JSON / GUI | fixed `climate` / `wind` tiles | new quantities have nowhere to render |

**The two problems are separable, and only one is worth solving now.**

1. **Access** — how a control task obtains "the indoor temperature" without knowing which device produced it, and at the rate *it* needs. → §2, build this.
2. **Configuration** — where the list of devices lives. → §7, defer this.

---

## 2. What is worth building: the access abstraction

### 2.1 Roles

Control tasks ask for a **role**, never for a device:

```
ROLE_INDOOR_TEMP      ROLE_OUTDOOR_TEMP     ROLE_WIND_SPEED
ROLE_INDOOR_RH        ROLE_OUTDOOR_RH       ROLE_WIND_DIR
ROLE_WINDOW_POS_M1/M2/M3                    ROLE_RAIN
ROLE_CO2              ROLE_SOIL_MOISTURE    …
```

T4 keeps one table: **role → { value, timestamp, validity }**. Each table entry in §3 binds a device's channels to roles.

```c
meas_t m;
if (dm_measure(ROLE_INDOOR_TEMP, &m) == MEAS_OK) {
    /* m.value_scaled, m.timestamp */
}
```

Three properties follow, and they are what "uniform access" actually requires:

- **A new sensor needs no control-task change.** Bind it to a role; consumers already read that role.
- **A new control task needs no sensor knowledge.** It asks for roles and handles `MEAS_ABSENT` / `MEAS_STALE` / `MEAS_FAULT`.
- **Unconfigured is a first-class state.** `MEAS_ABSENT` (nothing bound) is distinct from `MEAS_FAULT` (bound but failing), so a task can disable itself cleanly rather than act on zeros.

> **Safety constraint — non-negotiable.** T3's wind override keeps its safe-fail behaviour: `ROLE_WIND_SPEED` absent or faulted ⇒ treat wind as unsafe and close, exactly as `SENSOR_FAULT_W` does today (FR-W04). The window-position sensor must degrade the **opposite** way — fall back to open-loop, never stop ventilating (`windowPositionSensorRequirements.MD` FR-WP17). The two are deliberately inverse and the difference must be explicit in the role definition, not left to each consumer to remember.

### 2.2 Cadence and priority are requested by the consumer

Different tasks need the same sensor at different rates, and the rate changes with the task's own state. The window-position case: **1 Hz while travelling, 30 s otherwise** (`windowPositionSensorRequirements.MD` §3 — at 30 s, positioning overshoot is 17.5 % of stroke). A single global `poll_interval_s` cannot express that.

```c
/* one-shot: "I need this by deadline_ms from now" */
dm_request_once(ROLE_CO2, 2000, PRIO_TELEMETRY);

/* read never blocks — returns cached value, its timestamp, and validity */
dm_measure(ROLE_WINDOW_POS_M3, &m);
```

The bus task keeps a **just-in-time queue** ordered by deadline rather than polling on a fixed tick:

- Each request contributes a **due time**; the task serves **priority band first, earliest deadline within band**.
- *Just-in-time* means a reading is taken so it is **fresh when needed**, not as early as possible — which is what lets a 1 Hz positioning consumer and a 30 s climate consumer share one bus without the fast one dictating the tick.
- **Priority classes**: `PRIO_SAFETY` (wind — T3), `PRIO_CONTROL` (climate, positioning), `PRIO_TELEMETRY` (logging, display, remote status). Safety is served ahead of everything else, unconditionally.
- **Bounded blocking, not pre-emption.** A Modbus transaction is atomic. At 9600 8N1 a byte is ~1.04 ms, so an 8-byte request + 9-byte response + 3.5-character silence each way is **≈ 25 ms**; worst case one `MODBUS_TIMEOUT_MS` = **200 ms** against an unresponsive device. A higher-priority request waits at most one transaction — bounded and acceptable (200 ms of M3 travel is ~2.3 mm) — but the **200 ms timeout is the dominant delay term** and is what any margin must be sized against.
- **Reads never block the caller.** `dm_measure()` returns the cached value with its age. **T2 and T3 must never block on bus I/O** — a 200 ms timeout inside a safety task could delay a wind-override response, which is why `relay_controller.cpp` contains no Modbus calls today.

#### Why not plain earliest-deadline-first

EDF is the textbook answer for mixed-period real-time work and is optimal under preemption (`U ≤ 1` versus Rate-Monotonic's `≈ 0.69`). **Here it is the wrong tool.**

**1. The utilisation that would justify it does not exist.** `U = Σ (transaction_time / period)`:

| Consumer | Period | U |
|---|---|---|
| Indoor T/RH | 30 s | 0.08 % |
| Wind | 30 s | 0.08 % |
| Window position ×1 @ 1 Hz | 1 s | 2.5 % |
| *worst case: 3 windows travelling at once* | 1 s | 7.5 % |

**≈ 8 % worst case, realistically ~3 %.** EDF's advantage lives entirely in the high-utilisation regime; at `U ≈ 0.08` any work-conserving scheduler meets every deadline.

**2. Pure EDF contradicts the priority requirement.** It is deadline-driven and importance-blind: a telemetry read due in 100 ms outranks a safety read due in 500 ms. That is EDF working correctly, not a corner case.

**3. Its overload behaviour is the wrong shape.** Under transient overload EDF suffers the **domino effect** — cascading misses, bus time spent on already-doomed requests. Fixed priority degrades predictably: telemetry starves first, safety never does.

Also: **Modbus transactions are non-preemptible**, so classic EDF optimality does not hold anyway and a blocking term must be carried regardless.

**Adopted: priority bands, earliest-deadline within band** — EDF scoped inside fixed-priority classes. One queue keyed on `(priority, deadline)`, no overrun framework.

#### Request lifecycle: continuous, bounded series, cancellation

A consumer must be able to ask for an open-ended stream **or** a bounded burst, and to control a plan in flight:

```c
h = dm_request_continuous(ROLE_INDOOR_TEMP, 30000 /*ms*/, PRIO_CONTROL);
h = dm_request_series(ROLE_WINDOW_POS_M3, .count = 200, .interval_ms = 1000, PRIO_CONTROL);

dm_request_reset(h);   /* restart: count refilled, interval phase re-based to now      */
dm_request_stop(h);    /* graceful: no more scheduled; in-flight completes and is used */
dm_request_abort(h);   /* immediate: no more scheduled; result not attributed to h     */
```

Mapped onto an M3 stroke, which is what motivates it:

| Event | Call | Why |
|---|---|---|
| Stroke starts | `series(count = 200, 1 Hz)` | 200 s covers a 176 s stroke with margin, and **self-terminates** if the requester dies — a crashed task cannot pin the bus |
| Window reverses mid-stroke | `reset(h)` | travel restarts; refill count, re-base phase |
| Target reached | `stop(h)` | graceful — the reading already on the wire is still useful |
| Wind override fires | `abort(h)` | position is now irrelevant |

Semantics to pin down, each a bug if left implicit:

- **`stop` vs `abort` differ only in the in-flight reading.** `stop` delivers it; `abort` does not attribute it and fires no completion notification.
- **A Modbus transaction cannot be aborted mid-frame.** Once bytes are on the wire the frame must finish, or the bus is left undefined and the slave may reply into silence, desynchronising the next exchange. **`abort` is logical, never physical** — worst-case latency to actually stop is one transaction (25 ms, or 200 ms against a dead device).
- **Series completion notifies the requester**, so "my 200 readings finished" is distinguishable from "still waiting".
- **Failed reads consume the count**, and completion reports `attempted` / `succeeded`. Counting only successes would let a dead device turn a bounded series unbounded — reintroducing the pinning problem the bounded form exists to prevent.
- **`reset` on a completed handle is an error**, not a silent restart.
- **`stop` / `abort` are idempotent**, and handles carry a **generation counter** so a stale handle cannot control a newly-issued request that reused the slot (ABA).

#### Per-device failure backoff — the failure mode that actually threatens the schedule

At ~8 % utilisation contention is not the risk; **one unresponsive device is.** A dead sensor costs 200 ms per attempt, so at 1 Hz it consumes **20 % of the bus while returning nothing** and delays everything queued behind it, including higher bands. A single failed device can do more timing damage than every legitimate consumer combined.

The scheduler must **back a failing device off**: after N consecutive timeouts, drop it to a slow retry cadence (30–60 s) regardless of what consumers request, and restore the requested cadence on first success. Consumers see `MEAS_FAULT` meanwhile and degrade per their own policy (§2.1). T5's existing two-consecutive-failure detection is the trigger; the missing piece is the *cadence consequence*.

**This is where the design effort belongs.** Scheduling sophistication buys little at this load; failure containment buys a lot.

### 2.3 Averaging belongs to the requesting task

**The bus task publishes raw samples only: `(value, timestamp, validity)`.** It computes no averages. Each consumer keeps its own filter with its own window.

- `avg_win_t`, `avg_win_rh`, `avg_win_wind` are **control-loop tuning**, not sensor parameters — they always belonged to T6/T3.
- Two consumers of the same role can filter differently (T6 on 3 min, a logger on raw, a future task on 15 min) with no coordination.
- It decouples the filter from the poll interval, which is what makes variable cadence safe at all.

**Jitter resilience is then mandatory.** With a JIT queue, irregular arrival is *by design*. A naïve mean over a time window **over-weights clustered samples** — three readings 1 s apart plus one 60 s later would let the cluster dominate a 3-minute average that is mostly represented by the old sample. The filter must be **time-weighted**:

```
zero-order hold:  avg = Σ vᵢ · (tᵢ₊₁ − tᵢ) / W
trapezoidal:      avg = Σ (vᵢ + vᵢ₊₁)/2 · (tᵢ₊₁ − tᵢ) / W
```

Three details to specify with it:

1. **Gap handling** — no sample for longer than some multiple of the expected interval ⇒ mark the average **degraded/invalid** rather than extrapolating the last value across the gap.
2. **Partial windows** — at startup or after a fault clears, report coverage so a task can wait rather than act on 20 s of data labelled as a 3-minute mean.
3. **Circular quantities need vector averaging.** Wind *direction* cannot be scalar-averaged (350° and 10° average to 180°, not 0°). T5 already vector-averages direction. **Averaging semantics therefore belong to the role definition** (scalar / circular / event), even though window length belongs to the consumer.

---

## 3. The sensor set: a static table in code

Sensors are declared in firmware, not configured at runtime. Adding one means writing (or reusing) a driver, adding a row, and releasing — the same release the driver needs anyway.

```c
static const sensor_def_t k_sensors[] = {
  { .id = "th_in",  .drv = DRV_FG6485A, .addr = 1,  .log_ch = 0,
    .channels = { { ROLE_INDOOR_TEMP }, { ROLE_INDOOR_RH } } },

  { .id = "wind",   .drv = DRV_S200,    .addr = 44, .log_ch = 1,
    .channels = { { ROLE_WIND_SPEED }, { ROLE_WIND_DIR } } },

  /* generic register driver — no new driver module needed (§4) */
  { .id = "pos_m3", .drv = DRV_MODBUS_REG, .addr = 12, .log_ch = 3,
    .reg = { .fc = 4, .start = 0, .count = 1, .scale = 0.1f, .is_signed = false },
    .channels = { { ROLE_WINDOW_POS_M3 } } },
};
```

Note what is **absent**: no averaging window (that is the consumer's, §2.3) and no poll interval (that is requested, §2.2). The table describes *the device and what it means*, nothing about how anyone uses it.

**Validation is a build-time concern**, which is a genuine advantage over runtime config: duplicate Modbus addresses, duplicate `log_ch`, two sensors bound to the same role, or a missing wind binding can all be caught by `static_assert` or a small build-time check rather than at runtime on a live greenhouse.

---

## 4. One generic Modbus register driver

This survives the "you need a driver anyway" objection on its own merits, because **the existing drivers are barely device-specific**. The whole FG6485A measurement path is:

```c
modbus_read_holding_registers(slave_addr, 0x0000, 2, regs);
out->humidity_pct  = (int16_t)regs[0] / 10.0f;
out->temperature_c = (int16_t)regs[1] / 10.0f;
```

That is `{fc = 3, start = 0, count = 2, scale = 0.1, signed}` — parameters, not logic. The remaining ~200 lines of that module are info/diagnostic helpers, error mapping and comments; the S200 module is 131 lines in total.

A single `DRV_MODBUS_REG` driver taking `{fc, start, count, scale, endianness, signedness}` therefore covers most of the expected additions — **rain contact, CO₂ ppm, soil moisture, window position** are all one register and a scale factor.

**The saving is per-sensor effort, not configurability:** a new simple sensor becomes ~5 lines of table entry instead of a new 130–230 line driver module, header, and CMakeLists entry. Bespoke drivers remain for genuinely quirky devices — the S200's vector-averaged direction earns one; a rain contact does not.

This is also the precondition that would later make runtime configuration worth revisiting (§7).

---

## 5. Consequences to plan for

1. **SD log channels are a fixed, append-only registry** *(operator decision, 2026-07-31)*. Existing channels keep their meaning permanently; new ones are appended; **an assigned number is never reused or renumbered.**

   | `ch` | Meaning | Status |
   |---|---|---|
   | 0 | indoor T + RH (`value_a` = t_c10, `value_b` = rh_pct) | assigned — fixed |
   | 1 | wind speed + direction | assigned — fixed |
   | 2 | window state bitmask | assigned — fixed |
   | 4, 5 | T5 sensor-fault rows on `ALARM` | assigned — fixed |
   | 3, 6, 7, … | next free | allocate in order, then freeze |

   Because numbering is static: **historical logs keep parsing unchanged**, `logparser.py` and `plot_daily.py` gain one `elif ch == N` case per sensor (still a same-changeset obligation per CLAUDE.md, but a small one), and a log file stays self-describing without the config that produced it — which matters because logs are analysed offline months later. The allocation table belongs in `event_logger.h` beside the existing `value_a`/`value_b` catalogue. The project already uses this "allocate permanently, never renumber" convention for `LOG_PARAM_*` and the 240–243 ALARM discriminator band.

2. **Per-sensor fault state replaces two EG1 bits.** `SENSOR_FAULT_T` / `_W` are consumed by T3, T6, T8, T11, T14. With N sensors, fault state belongs in the role table; EG1 keeps only *aggregate* bits so T3's wind safe-fail path is unchanged.

3. **Averaging moves out of T5 into the consumers (§2.3)** — a real refactor. `sensor_reading_t` loses its `*_avg_*` fields; T6 and T3 gain time-weighted filters; `avg_win_*` become control-task keys. Note `avg_win_t` already differs per unit (5C88 = 3, 2344 = 6), so migration must preserve each unit's values. Also decide **who owns the published average**: `/api/status` exposes `temp_avg_c`, which after this is one consumer's view rather than a global truth.

4. **Bus budget** ≈ 8 % worst case, ~3 % realistically (§2.2). Contention is not the risk; a timing-out device is. The 25 ms figure is calculated, not measured — worth confirming on the bench, as a slow-turnaround device could be several times that.

5. **Status JSON and the remote site** gain new quantities; the canonical JSON builder assumes fixed tiles today.

---

## 6. Staging

| Phase | Content | Risk |
|---|---|---|
| **1 — Roles + `dm_measure()`** | Role table in T4; migrate T3/T6/T8/T11/T14 to role-based reads. Sensors still hard-coded exactly as now. **No behaviour change.** | Low — pure refactor, verifiable by diffing logged behaviour |
| **2 — Averaging to consumers, time-weighted** | T5 publishes raw `(value, timestamp)`; T6/T3 gain time-weighted filters; direction stays vector-averaged. Still a fixed 30 s cadence. **Prerequisite for phase 4.** | Medium, easy to get subtly wrong — validate by replaying logs (`model/vent_step_replay.py` reproduces T-demand to 97.8 % and would expose drift) |
| **3 — Static sensor table** | Replace the two hard-coded `if` blocks with `k_sensors[]` + a driver dispatch table. Build-time validation. | Low |
| **4 — JIT scheduler** | Request API (`once`/`continuous`/`series`, `reset`/`stop`/`abort`, generation-counted handles), `(priority, deadline)` queue, **per-device backoff**. Requires phase 2. | Medium — modest scheduling at ~8 % load; the risk is the silent failure modes, above all a timing-out device |
| **5 — Generic register driver** | `DRV_MODBUS_REG`. First new sensor added as a table row rather than a module. | Low |
| **6 — Per-sensor logging + status** | Allocate next free `ch`, add the matching case to `logparser.py` **and** `plot_daily.py` in the same changeset; extend status JSON. | Low-medium — additive per sensor, historical logs untouched |

Phases 1–3 are pure refactor with no user-visible change. A window position sensor needs 1, 2, 4 and 5.

---

## 7. Deferred: runtime configuration

Considered and **not recommended now**. Recorded here because the analysis is worth keeping, and because the conditions that would revive it are specific.

**The argument against:** every new sensor needs a driver compiled in, so a firmware release is required regardless. A JSON/NVS config layer — blob storage, parser, validation, SD import, upload endpoint, `config_version` precedence, GUI view — is substantial machinery whose main benefit is *avoiding* that release. With 2 sensors today and perhaps 5–6 ever, and all three field units carrying identical hardware, it solves a problem that does not exist.

**Two findings worth preserving**, because anyone attempting this later will hit them:

- **Config must never live in LittleFS.** `lfs0`/`lfs1` **are** the web-asset partitions (`partitions.csv`) and are overwritten by every asset OTA. Config there would be silently wiped by a routine update. **NVS** (own 64 KB partition at `0x10000`) is the only OTA-survivable writable store; the SD card is removable and cannot be a source of truth.
- **If it is ever built, gate SD import on an anti-downgrade `config_version`**, exactly like the ROTA `seq`: apply only if the file's version exceeds the stored one. That makes leaving the card in the slot harmless and idempotent, with no rename-after-use dance.

**Revisit only if both become true:**

1. `DRV_MODBUS_REG` (§4) exists — so a new simple sensor genuinely needs *no* firmware change; **and**
2. units diverge in sensor topology — so one firmware image can no longer serve the fleet.

Until then the cost of runtime config is real and its benefit is hypothetical. Adding it later is cheaper than carrying unused machinery from the start.

**What is given up in the meantime:** per-unit sensor differences would need per-unit builds. Not a live problem — all three units carry the same two sensors, and FDA4's difference is simply having none attached.

---

## 8. Recommendation

- **Build the access abstraction** (§2): roles, JIT queue with priority bands, consumer-owned time-weighted averaging, per-device backoff. This is the durable value and none of it depends on configurability.
- **Static sensor table in code** (§3), with build-time validation — which catches address/role/channel clashes earlier than any runtime check could.
- **Write the generic register driver** (§4) as an effort saver, not as a configurability play. It is justified by how thin the existing drivers are.
- **Log channels fixed and append-only** (§5.1) — keeps five months of campaign data parseable and reduces per-sensor log work to one additive case.
- **Spend scheduling effort on failure containment, not the algorithm** (§2.2). At ~8 % utilisation, per-device backoff matters more than EDF ever would.
- **Defer runtime configuration** (§7) until the two stated conditions hold.

## 9. Open questions

1. **May a role have more than one sensor?** Redundancy (pick/average/vote) versus distinct roles (`indoor_temp` vs `outdoor_temp`). Distinct roles are simpler and probably sufficient.
2. **Window position: a role per window** (`window_pos_m1/m2/m3`) or one role with a channel index? Per-window is simpler to consume.
3. **What happens when the bus cannot satisfy all requests?** Refuse, downgrade the least critical, or degrade proportionally — and it must be **visible**, since silent degradation makes a control loop behave inexplicably.
4. **Does the LCD need to show new sensors**, or is the web view enough? Affects T8 and the menu structure.
5. **Rain sensor semantics** — instantaneous contact or latched-with-timeout? Determines whether it is a measurement or an event, and whether it belongs in the role table at all.
6. **Do failed reads count toward a bounded series?** Proposed yes (§2.2); confirm.
