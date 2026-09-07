# Adding a Modbus bus lock — implementation plan (gh#49)

| Field | Value |
|---|---|
| Document | Implementation plan |
| Issue | [gh#49](https://github.com/pe1mew/greenhouse-Controller/issues/49) — `modbus_rtu.h` documents a UART mutex that `modbus_rtu.cpp` never creates |
| Date | 2026-09-07 |
| Status | **Phases 1–2 complete** (code written, builds + host tests green, **not yet run on hardware**). Phase 3 (concurrency test) and Phase 4 (policy record) not started |
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

### Phase 4 — keep single-owner as policy

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
