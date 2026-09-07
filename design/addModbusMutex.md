# Adding a Modbus bus lock — implementation plan (gh#49)

| Field | Value |
|---|---|
| Document | Implementation plan |
| Issue | [gh#49](https://github.com/pe1mew/greenhouse-Controller/issues/49) — `modbus_rtu.h` documents a UART mutex that `modbus_rtu.cpp` never creates |
| Date | 2026-09-07 |
| Status | **COMPLETE — all four phases.** The lock is verified on hardware (FDA4, 2026-09-07, fail-first protocol observed, §4.3c) and the single-owner policy is recorded in `modbus_rtu.h` and `memory/architecture.md`. gh#49 closed |
| Scope | `drivers/modBus/`, one comment in `firmware/src/main.cpp`, one test in `drivers/modBus/test/` |

---

## 1. The defect

`drivers/modBus/src/modbus_rtu.h` states, twice, that the driver serialises wire access with a UART mutex:

- `:35` — *"The driver serialises wire access internally with a UART mutex — two tasks may safely call any combination of these functions concurrently."*
- `:100` — `modbus_init()` *"creates the UART mutex."*

`modbus_rtu.cpp` contains **no semaphore, mutex or critical section of any kind**. `grep -iE "Semaphore|mutex|CRITICAL|SuspendAll"` over `drivers/modBus/src/` returns nothing; the only semaphore in the driver tree is in the hardware-loopback *test*.

The header describes an intention that was never implemented. It is a documentation defect first — a reader who trusts it will introduce a second caller and get intermittent, misattributed failures.

### 1.1 The premise holds — T5 really is the sole caller

Verified 2026-09-07. The heartbeat task's `fg6485a_read_measurements()` / `s200_read_measurements()` polls were **removed in alpha.6.8** (`main.cpp:368`), so the only production callers are T5's two reads (`sensor_poll.cpp:456`, `:493`) via the FG6485A and S200 wrappers.

**But `main.cpp:708-715` still claims the heartbeat polls the bus** — stale since alpha.6.8 and contradicted by the removal note 40 lines below it. That is a second false-documentation instance in the same subsystem and is fixed in Phase 1.

---

## 2. What a lock must actually cover

Three shared resources, not one. The third is the one most likely to be missed.

| # | Resource | Why concurrency breaks it |
|---|---|---|
| 1 | **DE/RE GPIO** (`gpio_set_rs485_direction`) | A second caller asserting DE mid-response corrupts the wire **and** blinds the first caller |
| 2 | **`s_frame_end_us`** (`:175`) | Read-modify-written by the inter-frame-gap guard (`:246-251`). Corrupting it breaks the Modbus RTU t3.5 silence that keeps frames legal |
| 3 | **The UART RX FIFO** | There is **one** FIFO. Two readers steal each other's response bytes. Worse: the receive path drains **exactly 8 bytes** of half-duplex echo (`:273`) assuming the FIFO holds only this transaction's echo. A second caller silently breaks that assumption, and the symptom is a CRC or framing error **attributed to the sensor** |

Resource 3 is why this is a real fix and not cosmetic: without it one might imagine "each caller toggles its own DE, it'll be fine". It will not be.

### 2.1 Extent and duration of the critical section

From the IFG guard (`:246`) through the last response byte — effectively the whole body after parameter validation, in **both** paths:

- `modbus_transaction()` `:217` — shared by FC03/FC04
- `modbus_write_multiple_registers()` `:357` — has its **own** DE toggle at `:399`/`:409`, so it does not go through `modbus_transaction`

**Worst case ≈ 215 ms**: IFG 4 ms + TX 8 bytes ≈ 8.3 ms + 2 ms DE guard + 1.5 ms settle + `MODBUS_TIMEOUT_MS` 200 ms. That figure sizes the lock timeout.

---

## 3. Two traps a naive implementation hits

### 3.1 `modbus_init()` is called twice, by design

`main.cpp:714` at boot, and T5 again at task entry (`sensor_poll.cpp:384`). This is deliberate and documented at `main.cpp:1025`: *"T5's own modbus_init reconfirms the driver state at task entry."*

A plain `s_bus_mtx = xSemaphoreCreateMutex();` inside `modbus_init()` would, on the second call, **orphan the first mutex** — leaking it and stranding anyone blocked on it forever.

> **Creation must be idempotent:** `if (s_bus_mtx == NULL) s_bus_mtx = xSemaphoreCreateMutex();`

### 3.2 Host builds have no FreeRTOS

The driver's `freertos/` includes are already `#ifndef NATIVE_TEST`-guarded (`:49-55`), and `drivers/modBus/test/` builds native. The mutex must sit behind the same guard with a no-op shim for host builds, or the host unit tests stop compiling.

---

## 4. Phases

### Phase 1 — make the documentation true *(no behaviour change)*

- Delete both false claims in `modbus_rtu.h` (`:35`, `:100`).
- State plainly that the driver is **not re-entrant**, that T5 is the sole permitted caller, and that the reason is **timing as well as correctness** (§4.4).
- Fix the stale heartbeat comment at `main.cpp:708-715`.

Ships independently of everything below and closes the defect as filed: documented behaviour now matches real behaviour.

### Phase 2 — add the lock

**Shape: single-exit restructure.** Rename the two existing bodies to `static modbus_status_t ..._locked(...)` and make the public functions thin wrappers that take → call → give.

Both bodies contain multiple `return`s inside the receive loop; a missed unlock deadlocks the bus permanently. Wrapping makes that **structurally impossible**, and the diff is smaller than threading `goto cleanup` through both.

```c
#ifndef NATIVE_TEST
static SemaphoreHandle_t s_bus_mtx = NULL;   /* created once, see 3.1 */
#endif

#define MODBUS_LOCK_TIMEOUT_MS 500u          /* ~2x the 215 ms worst case */
```

- `xSemaphoreCreateMutex()` — **not** a binary semaphore: the mutex flavour gives priority inheritance.
- Acquire with a **bounded** timeout, never `portMAX_DELAY`.
- If `s_bus_mtx == NULL` (host build, or a call before init), proceed unlocked — preserving today's behaviour rather than introducing a new failure.

### Phase 3 — prove the test has teeth

Extend `drivers/modBus/test/test_hw_loopback/` with a concurrency case: two tasks issuing transactions for ≥1000 exchanges; assert zero CRC/framing errors and every response matched to its own request.

> **Run it against the unpatched driver first and confirm it FAILS.** A concurrency test that passes on broken code is worse than no test — it manufactures false confidence. This step is not optional.

#### 4.3a Phase 3 is blocked — two independent obstacles *(found 2026-09-07)*

The test is written (`HW-MB-012`, `test_concurrent_callers_do_not_corrupt_each_other`) but **has never been executed**. Neither obstacle is caused by gh#49:

**1. The loopback env does not build — and has not for some time.**

```
src/modbus_rtu.cpp:257:22: error: 'UART_SCLK_DEFAULT' was not declared in this scope
```

`[env:lolin_s3_loopback]` is `platform = espressif32` (**unpinned**) with `framework = arduino`, whose bundled ESP-IDF predates `UART_SCLK_DEFAULT`. The driver moved to pure ESP-IDF in the v2.0.0 migration; this Arduino-framework test env was left behind. **Verified by building the env against `HEAD`'s `modbus_rtu.cpp` with all gh#49 changes removed — identical failure.** So the whole hardware suite (HW-MB-001…011) has been unbuildable since the migration, not just the new case.

Options, none yet chosen:

| | Fix | Cost / doubt |
|---|---|---|
| a | Pin the env to `espressif32@6.12.0` | Cheapest, but Arduino-framework 6.x ships IDF 4.4, which likely still lacks the symbol — may not work |
| b | Swap `UART_SCLK_DEFAULT` for a symbol both frameworks have | Changes **production** code to satisfy a test env; least attractive |
| c | Convert the env to `framework = espidf` to match the driver | The real fix, and the largest |

**2. No board to run it on.** The suite needs three jumper wires (GPIO 17→38, 21→18, 8→16) on a board that is not the live controller. FDA4 is the only unit in service, fully assembled and jumper-free; 2344 is stowed. Fitting jumpers to FDA4 means taking the greenhouse's only controller out of service.

**Consequence for the mutex:** phases 1–2 compile and pass host tests, but the lock has **never executed on hardware**. It must not be promoted to `mainstream` on the strength of a green host run — the host build stubs the lock out entirely (`NATIVE_TEST` shims `bus_lock()` to `return true`), so those 12 passing tests say nothing whatsoever about the mutex.

#### 4.3b Phase 3 alternative — the real-slave probe *(recommended route)*

Sidesteps both blockers in §4.3a entirely: **no loopback instrument, no jumper wires, no env conversion.** FDA4 carries a real FG6485A at address 1 and a real S200 at address 44, so the bus already has two slaves that answer. Two tasks poll them concurrently and the driver's own validation does the detection.

##### Why it detects a stolen frame for free

`modbus_rtu.cpp:391`, reached **only after the CRC check at `:381` has passed**:

```c
/* Validate address and function code */
if (resp[0] != device_addr || resp[1] != fc) {
    return MODBUS_ERR_FRAMING;
}
```

A frame that passes CRC is intact. An **intact** frame carrying the wrong address is therefore not corruption — it is *another transaction's response*, delivered to the wrong caller out of the shared RX FIFO. So:

> **CRC-valid + wrong address = proof of a stolen frame.**

This is a stronger signal than the loopback test's self-identifying payload, and it costs nothing to obtain. The discrimination is doubled by the two callers naturally differing in **both** fields the check tests: the FG6485A is read with **FC03** (holding registers) and the S200 with **FC04** (input registers), matching what `fg6485a.cpp` and `s200.cpp` actually issue.

##### What each status means during the probe

| Status | Reading |
|---|---|
| `MODBUS_ERR_FRAMING` | **The finding.** A stolen frame — the failure the mutex exists to prevent |
| `MODBUS_ERR_CRC` | Frames interleaved on the wire: two DE/RE assertions collided, or the t3.5 gap was violated via `s_frame_end_us` |
| `MODBUS_ERR_TIMEOUT` | A response was consumed by the other caller, or the slave was still in its inter-frame silence |
| `MODBUS_ERR_BUSY` | Lock contention beyond `MODBUS_LOCK_TIMEOUT_MS`. Should be **zero**; a non-zero count means a transaction held the bus longer than the 215 ms model predicts and wants investigating, not dismissing |
| `MODBUS_OK` | Clean transaction |

**Pass = FRAMING, CRC and BUSY all zero, with both callers completing every iteration.** Timeouts warrant judgement: a real bus can drop one, so a handful is not automatically a failure — but on a bus that is quiet outside the probe, they should be rare, and any timeout alongside a non-zero FRAMING count is part of the same story.

##### Shape

```c
#ifdef MODBUS_CONCURRENCY_PROBE
/* Two tasks, both pinned to core 1 so they genuinely contend rather than
 * being serialised onto separate cores by the scheduler. Each polls its own
 * real slave with the same function code its production driver uses. */
static void probe_task(void *arg)          /* arg -> {addr, fc03, counters} */
{
    for (uint32_t i = 0; i < PROBE_ITERATIONS; i++) {
        uint16_t regs[2];
        modbus_status_t s = ctx->fc03
            ? modbus_read_holding_registers(ctx->addr, 0x0000u, 2u, regs)  /* FG6485A */
            : modbus_read_input_registers  (ctx->addr, 0x0008u, 2u, regs); /* S200    */
        tally(ctx, s);
    }
    ctx->done = true;
    vTaskDelete(NULL);
}
#endif
```

**Register ranges verified against the production drivers:** `fg6485a.cpp:61` issues FC03 at `0x0000`; `s200.cpp:78` issues FC04 at `0x0008` (the wind block is `0x0008`–`0x0013`, 12 registers). The sketch reads a 2-register subset of that block to keep the two transactions symmetric. If the S200 rejects a partial read with an exception, use the driver's full 12-register read instead — the longer frame is *better* for this test, since more wire time means more overlap to contend over.

Sizing: a successful transaction is ~25–30 ms, so two contending tasks at 500 iterations each ≈ **1000 transactions in roughly 30 s**. Two minutes is a comfortable run.

##### The operational hazard — read this before running it

**T5 is polling the same two slaves throughout, and it is not a passive bystander.** With the probe saturating the bus, T5's reads can fail or return `MODBUS_ERR_BUSY`. T5 raises a sensor fault after **two consecutive failures** — and a **wind** sensor fault is safe-fail: T3 forces the windows shut (FR-W04).

> **Running this naively on a live controller can close the greenhouse's windows.**

Mitigation, in order of preference:

1. **Suspend T5 for the duration** (`vTaskSuspend` on its handle, resume after). No reads, so no failure counting, so no fault bits, so no safe-fail. Two probe tasks are ample contention without it.
2. Run when a spurious close is harmless — a cool night rather than a hot afternoon.
3. Accept it and reopen manually. Least attractive: it puts a real actuation cycle on M3's 171 s stroke for a test.

Note this hazard is *itself* evidence for the §4.4 policy: a second bus caller does not merely risk corrupt reads, it can propagate into actuator behaviour through T5's fault path.

##### Deployment

Behind a build flag, not an endpoint — test code does not belong in a production image:

1. Build with `-DMODBUS_CONCURRENCY_PROBE`, flash FDA4 over USB (**COM10**, CH340).
2. Watch the serial console; the probe logs its tally and stops.
3. Reflash the normal build and confirm recovery with an `/api/status` read: `eg1 = 0x0`, plausible T/RH and wind.

Roughly **five minutes with the greenhouse uncontrolled**, versus a half-day for the §4.3a env conversion.

##### Fail-first — still not optional

Run the probe **twice**: once against a build with the gh#49 mutex reverted, and once with it. The first run must show non-zero FRAMING and/or CRC. If the broken driver passes, the probe is not exercising contention — most likely the two tasks are not actually overlapping — and a green run on the fixed driver would prove nothing.

##### What this does and does not establish

**Does:** the lock works against the real transceiver, real slave turnaround, and real bus timing — conditions the loopback instrument only simulates.

**Does not:** replace `HW-MB-012`. The loopback test can inject malformed and adversarial frames a real sensor never emits, and runs without taking a controller out of service. This probe answers *"does the mutex work"*; the loopback suite is the regression test. §4.3a remains worth doing on its own ticket — it also unblocks `test_t2_relay`, which has been disabled since the v2.0.0 migration for the same reason.

#### 4.3c Result — verified on hardware, FDA4, 2026-09-07

Run via the real-slave probe (§4.3b) on the development unit, **twice**, as the fail-first rule requires. `bin/2.4.0` firmware, 500 iterations per caller, both pinned to core 1.

| Build | ok | FRAMING | CRC | timeout | BUSY | Verdict |
|---|---|---|---|---|---|---|
| **mutex reverted** (`80099b4`) | **0 / 1000** | 0 | 0 | **1000** | 0 | **FAIL** |
| **with mutex** (`027e2b4`) | **1000 / 1000** | 0 | 0 | 0 | 0 | **PASS** |

The lock works. The unpatched driver could not complete a *single* transaction under contention.

##### The prediction that was wrong — and what it means

§4.3b expected the unsynchronised driver to fail with **FRAMING** (stolen frames) and **CRC** (interleaved frames). It produced **neither**: every one of the 1000 transactions **timed out**, with zero FRAMING and zero CRC.

The mechanism is more destructive than predicted. Each transaction drains **exactly 8 bytes** of half-duplex echo (`modbus_rtu.cpp:273`) before reading its response. With two callers unsynchronised, each drain consumes bytes belonging to the other, so **no frame ever assembles** — there is nothing intact left to mis-address or to fail a CRC on. Corruption is not partial; it is total.

> **Do not read "zero FRAMING, zero CRC" as "no corruption".** On this driver, complete bus failure presents as **timeout**. The probe's verdict logic was corrected to treat `0 clean` as FAIL rather than INCONCLUSIVE, which is how the first run was initially misread.

The self-identifying-response reasoning in §4.3b is still sound — it is simply the *second* line of detection. The first is that nothing gets through at all.

##### A second finding: the bus does not survive boot

The first two attempts produced 1000 timeouts on **both** builds. The cause was placement, not the lock: the probe ran at **t ≈ 1.8 s**, on the `modbus_init()` that `main.cpp` performs at t ≈ 1.4 s. Between those two moments boot brings up the RTC (I²C), LittleFS, and the SD card over SPI — including an unmount — and **the Modbus bus does not survive it**.

Proof from the same build and boot: the probe got 1000/1000 timeouts at t = 1.8 s, while T5 — which calls `modbus_init()` again at its own task entry — polled both sensors perfectly (`T=29 °C RH=60 % ws=2.3 m/s`) moments later.

This is exactly what `main.cpp:1025` means by *"T5's own modbus_init **reconfirms** the driver state at task entry"*. The re-init is load-bearing, not defensive. The probe now calls `modbus_init()` itself rather than depending on boot ordering. Recorded in `memory/gotcha-log.md` (2026-09-07).

##### Also confirmed

- The S200 **accepts a 2-register partial read** of its 12-register wind block (500/500 OK). The §4.3b caveat about widening to 12 registers is unnecessary.
- **`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y` with a 5 s timeout** would have panicked the board: the driver's receive loop spins without yielding, so two priority-4 tasks on core 1 starve the idle task. The probe yields one tick per transaction. Caught before flashing, by reading `sdkconfig.lolin_s3`.
- FDA4 restored to released **2.4.0** afterwards (`fw_ver` = `asset_version` = 2.4.0, sensors reading, `eg1` clearing after T2's boot calibration).

##### What is still not covered

The probe exercises **two** callers on a **quiet** bus with **cooperative** slaves. It does not cover malformed or adversarial frames, three or more callers, or a slave that answers late — that is what `HW-MB-012` and the §4.3a env work are for. The lock is verified, not exhaustively characterised.

### Phase 4 — keep single-owner as policy — **DONE 2026-09-07**

Recorded in two places: `modbus_rtu.h`'s "Thread safety" block ("locked, but still one caller by policy") and the T5 row of `memory/architecture.md`. Both state the reason is **timing, not correctness** — the lock removes the corruption hazard, but ~215 ms of blocking must stay out of T2/T3 regardless.


The mutex buys **correctness, not permission**. A 200 ms `MODBUS_TIMEOUT_MS` inside High-priority, WDT-subscribed T2 or T3 could still delay a wind-override response.

Record in the header and in `memory/architecture.md` that all bus I/O stays in T5 — or in the dedicated bus task of [`refactorSensorConfiguration.md`](refactorSensorConfiguration.md) §2.2 — for **timing** reasons, independent of the lock.

---

## 5. Decision taken — the lock-timeout return code

**`MODBUS_ERR_BUSY` added** (operator decision, 2026-09-07).

Reusing the existing `MODBUS_ERR_TIMEOUT` conflates *"another task holds the bus"* with *"the slave did not answer"*. T5 raises a sensor fault after **two consecutive failures**, so contention would surface as a **sensor fault** — sending an operator to hunt a sensor that is perfectly healthy. The diagnostic cost outweighs the small API churn; callers overwhelmingly test `!= MODBUS_OK`.

**Version consequence:** Phase 1 alone is a **patch**. Phase 2 plus a new enum value is an internal API change → **minor** under the CLAUDE.md SemVer heuristic.

---

## 6. Risk and rollout

This touches the code path **every** sensor read uses, on the only controller currently in service. A regression takes out T/RH and wind on FDA4.

- Soak on **FDA4** ≥ 24 h, watching `eg1` fault bits and read-failure counts. FDA4 gained real sensors in 2026-09, so it can genuinely exercise this — it could not have a week earlier.
- 2344 is stowed, so there is **no second opinion**. Weigh that before promoting to `mainstream`.
- Phase 1 carries no runtime risk and can ship on its own.

### 6.1 Pre-existing hazard — logged, not fixed here

`modbus_init()` calls `uart_driver_delete()` (`:188`). If a transaction were ever in flight from another task, that is a use-after-delete. Safe today because T5 is the sole caller and inits before its first poll — but it is another reason not to open this bus to a second caller casually. Out of scope for gh#49; worth its own issue if a second caller is ever contemplated.

---

## 7. Out of scope

- The `refactorSensorConfiguration.md` bus-task refactor. This plan makes the *existing* driver honest and safe; it does not restructure ownership.
- Reducing `MODBUS_TIMEOUT_MS`. The 200 ms figure is the blocking term that keeps bus I/O out of T2/T3, and changing it is a separate argument with its own evidence.
