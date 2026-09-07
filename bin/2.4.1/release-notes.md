# Release 2.4.1

**Date:** 2026-09-07
**Built on:** 2.4.0
**Closes:** gh#49 (`modbus_rtu.h` documented a UART mutex that `modbus_rtu.cpp` never created)

## Why a patch, not a minor

The plan and the gh#49 comment both called this a **minor**. That was wrong, and it is corrected here.

The CLAUDE.md heuristic asks: a user-visible feature, a new task, a new NVS namespace/key, or a payload-shape change? **None of those.** `MODBUS_ERR_BUSY` is a new C enum value — internal plumbing, not a payload shape — and under the single-caller policy the mutex is never contended, so runtime behaviour is unchanged. Web assets are **byte-identical** to 2.4.0.

## The defect

`modbus_rtu.h` claimed, in two places and since before 2.0, that the driver serialised wire access with a UART mutex. It never did: no semaphore, mutex or critical section existed anywhere in `modbus_rtu.cpp`. Harmless in practice only because T5 is the sole caller — but a reader trusting the header would have added a second caller and got intermittent, misattributed failures.

## The fix

- **Bus mutex** (`xSemaphoreCreateMutex`, so priority inheritance), held across a whole transaction — the critical section must span the inter-frame-gap guard through the last response byte, because a transaction touches three shared resources: the DE/RE GPIO, the `s_frame_end_us` t3.5 timestamp, and the single UART RX FIFO.
- **Single-exit wrappers.** The three public functions became thin take → call → give wrappers around bodies that run locked (`modbus_write_multiple_registers` → `static write_multiple_locked`). Those bodies have multiple returns; one unlock per function makes a missed release structurally impossible rather than a review obligation. A missed release would wedge the bus permanently.
- **`MODBUS_ERR_BUSY`**, deliberately distinct from `MODBUS_ERR_TIMEOUT`: T5 raises a sensor fault after two consecutive read failures, so folding contention into TIMEOUT would send an operator hunting a healthy sensor. `MODBUS_LOCK_TIMEOUT_MS` = 500, roughly twice the ~215 ms worst-case hold.
- **Idempotent creation.** `modbus_init()` is deliberately called twice (boot, then T5 at task entry), so an unconditional create would orphan the first handle and strand anything blocked on it.
- **`UART_SCLK_DEFAULT` version guard** — a build fix, not a runtime one. That symbol is IDF 5.0+, and the Arduino-framework driver envs run IDF 4.4. Four envs had been dead since 2026-06-09: `modBus -e lolin_s3_loopback`, `modBus -e lolin_s3`, `FG6485A -e lolin_s3`, `s200 -e lolin_s3`. The guard resolves to `UART_SCLK_APB` there and leaves the firmware byte-identical on IDF 5.5.

## Verification — two independent hardware tests, fail-first both times

Neither result was trusted until the same test had been run against a **lock-disabled** build and confirmed to fail.

**1. Real-slave probe, FDA4, two tasks polling the actual FG6485A@1 (FC03) and S200@44 (FC04):**

| Build | clean | FRAMING | CRC | timeout | BUSY |
|---|---|---|---|---|---|
| lock disabled | **0 / 1000** | 0 | 0 | 1000 | 0 |
| lock enabled | **1000 / 1000** | 0 | 0 | 0 | 0 |

**2. Hardware loopback suite, 12F0 bench board, `pio test -e lolin_s3_loopback`:**

| Build | result |
|---|---|
| lock disabled | **533 CRC errors — FAIL** |
| lock enabled | **12/12 PASS** |

That suite's other 11 tests — DE/RE timing, TX frame content, CRC, both round trips, and the timeout/CRC/exception paths — **had not run since the ESP-IDF migration in June**, because the env did not compile. They all pass.

**A prediction that was wrong, worth recording:** the plan expected the unsynchronised driver to fail with FRAMING and CRC. On real slaves it produced **neither** — everything timed out, because each transaction's 8-byte echo drain consumes the other caller's bytes so no frame ever assembles. On this driver, *"zero FRAMING, zero CRC" does not mean "no corruption"* — check the clean count first.

## What did NOT change

- No user-visible behaviour. Under the single-caller policy the lock is never contended.
- Web assets **byte-identical** to 2.4.0 (`app.js` hash-compared).
- No task, NVS key, log format or payload shape.
- The dev-only concurrency probe (`-DMODBUS_CONCURRENCY_PROBE`) is **not** in this build — verified by symbol table.

## The lock buys correctness, not permission

Recorded in `modbus_rtu.h` and `memory/architecture.md`: a transaction can hold the bus ~215 ms (`MODBUS_TIMEOUT_MS` dominates), and blocking that long inside High-priority, WDT-subscribed **T2** or **T3** could delay a wind-override response. **All bus I/O stays in T5.** Two further traps are documented there: `modbus_init()` deletes and reinstalls the UART driver (a re-init during another task's transaction is a use-after-delete), and the receive loop never yields, so a back-to-back poller starves the idle task and trips the 5 s TWDT.

## Build artefacts

```
bin/2.4.1/greenhouse-controller-2.4.1.bin   1,379,264 B
bin/2.4.1/web-assets-2.4.1.zip                117,139 B  (STORE, method=0)
```

Payload verified before release: the 2.4.1 ELF carries the mutex symbol; **2.4.0's does not**.

## Rollout

**Not yet released to GitHub** — built and staged, awaiting operator commit + push. Then `python bin/rota_release.py release 2.4.1` (→ GitHub Release, tags `v2.4.1`, points **soak**, next seq after 45). FDA4 pulls on its next hourly check; its apply window is 01:00–23:00, so it does not wait for a night window — but the quiet gate still applies, and **an active browser session on the unit will defer it**.

5C88 (`mainstream`) is untouched. Promote only after FDA4 has run this long enough to matter — and note the mutex has never been exercised on a unit under real 30 s polling, only under deliberate saturation.
