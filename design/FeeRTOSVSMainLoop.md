Read memory [](file:///c%3A/Users/miprc/AppData/Roaming/Code/User/workspaceStorage/5140ceb1847e1b17b8231a42bd2d05be/GitHub.copilot-chat/memory-tool/memories/YjgwOTBjOWQtMDY4Mi00N2Q3LWE4OGQtNzM5Yzk4ZjhhZmMy/plan.md)

Good. I have all the context. The file should go in design alongside the other design documents. Let me create it now. 

Create File

Compacted conversationFile editing tools are currently disabled. Here is the complete content ready to save as `design/FreeRTOS vs Main Loop.md`:

<details>
<summary>design/FreeRTOS vs Main Loop.md — full content (click to expand)</summary>

```markdown
# FreeRTOS vs Main Loop — Architecture Decision

## Greenhouse Ventilation Controller — Firmware Concurrency Model

| Field        | Value                                                  |
|--------------|--------------------------------------------------------|
| Document     | Software Architecture Decision — Concurrency Model     |
| Project      | Greenhouse Ventilation Controller                      |
| Version      | 0.1 (draft)                                            |
| Date         | 2026-03-27                                             |
| Status       | Draft                                                  |
| Related docs | `technicalDesignSpecification.md` §5                   |
|              | `functionalRequirementsSpecification.md`               |

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [The Two Candidates](#2-the-two-candidates)
3. [Requirements That Drive the Choice](#3-requirements-that-drive-the-choice)
4. [Structured Comparison](#4-structured-comparison)
5. [Recommended Task Architecture](#5-recommended-task-architecture)
6. [Maintainability Mitigations](#6-maintainability-mitigations)
7. [Decision Record](#7-decision-record)

---

## 1. Purpose

The firmware toolchain (PlatformIO, ESP32-S3, Arduino or ESP-IDF framework) was selected in TDS §2.3, but the choice between two concurrency models was deliberately deferred to the software design phase:

- **Option A — Arduino main loop with polling and interrupts:** a single `setup()` / `loop()` with `millis()`-based timers, ISR callbacks, and a state machine spread across one or more `loop()` iterations.
- **Option B — FreeRTOS tasks:** each major concern runs as an independent FreeRTOS task communicating through queues and semaphores. FreeRTOS is included automatically when using either PlatformIO's Arduino or ESP-IDF framework on the ESP32.

This document analyses both options against the concrete requirements of this project and records the decision made.

---

## 2. The Two Candidates

### 2.1 Arduino Main Loop with Polling

The classic Arduino programming model: a single execution thread that repeats continuously.

```c
setup() {
    // one-time initialisation
}

loop() {
    pollModbus();          // read sensors if interval elapsed
    evaluateClimate();     // assess T/RH vs setpoints
    manageWindSafety();    // check wind speed/direction
    updateWindows();       // issue relay pulses if needed
    scanKeypad();          // check for key presses
    updateDisplay();       // refresh LCD
    handleMqtt();          // pump MQTT keep-alive
    logEvents();           // flush pending log entries
}
```

Time-based concurrency is achieved with `millis()` / `micros()` comparisons. Urgent events can use hardware interrupts (`attachInterrupt()`), but the ISR must be very short and communicate back through a volatile flag.

**Inherent limitations relevant to this project:**

| Limitation | Consequence |
|------------|-------------|
| Single thread: every action in `loop()` delays all others | A blocking Modbus transaction (several milliseconds) delays the wind-safety check |
| Long operations must be broken into state machines spread across many `loop()` calls | The 171 s M3 motor run-time requires a multi-loop state machine with `millis()` bookkeeping |
| Per-window independent dwell timers need `millis()` timestamps stored per window, per direction — 6 independent counters | Error-prone bookkeeping; off-by-one bugs are common |
| `loop()` completion is the only liveness indicator | Watchdog only catches total freeze; a slow subtask can starve others without triggering it |
| WiFi / MQTT internally run FreeRTOS tasks (the ESP32 SDK); the application `loop()` is itself just one FreeRTOS task at low priority | Pretending there is no RTOS while RTOS runs underneath increases complexity |

### 2.2 FreeRTOS Tasks (Arduino + FreeRTOS API)

Each major concern runs as an independent FreeRTOS task. The Arduino framework on the ESP32 already includes FreeRTOS — no extra library is needed. The full FreeRTOS API (`xTaskCreate`, `xQueueSend`, `vTaskDelay`, `xSemaphoreTake`, etc.) is available alongside familiar Arduino functions (`Serial`, `Wire`, `SPI`, `digitalRead`, etc.).

```c
// Each task is a simple while(1) loop responsible for exactly one concern
void taskWindSafety(void *) {
    for (;;) {
        WindData w = readModbusWind();
        if (w.speed > cfg.v_max || inExclusionZone(w.direction)) {
            xQueueSend(qWindowCmd, CLOSE_ALL, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(500));   // yield; other tasks run here
    }
}
```

Tasks are assigned a priority. The scheduler runs the highest-priority ready task. If a safety task is always ready before a display task, it always runs first — this is a property of the scheduler, not something that must be programmed into every `loop()` iteration.

---

## 3. Requirements That Drive the Choice

The following FRS requirements have direct implications for the concurrency model. All are **Must** unless noted.

| Req. ID | Requirement (abbreviated) | Timing / concurrency implication |
|---------|---------------------------|----------------------------------|
| FR-WS02 | Close all windows **immediately** when wind speed exceeds v_max | Requires preemptive priority over climate control and display logic |
| FR-WS05 | Wind safety **shall take priority over all other window commands, including manual commands** | A polling loop cannot guarantee this unless every other subtask checks a global flag before acting |
| FR-WS06 | Show wind-override alarm on LCD **for as long as override is active** | Display must remain responsive even when relay/motor logic is running |
| FR-A03 | Issue relay pulses for the **full motor run-time** (21 s for M1/M2; 171 s for M3) | A 171 s blocking wait would freeze `loop()` entirely |
| FR-A06 | Transition window state to target **after motor run-time has elapsed** | Requires an independent per-window timer |
| FR-A11 | **Not issue a new command until dwell time has elapsed** (independently per window, per direction) | Up to 6 independent dwell timers running in parallel |
| FR-S03 | Sample sensors at **configurable interval (default 60 s)** | Periodic polling must not block safety checks |
| FR-ST02 | **Close all windows before entering normal operation** on power-on/restart | Startup sequence: sequential, blocking — must complete before main operation begins |
| FR-UI03 | Display **current T, RH, mode, and window states** | Must update even while relays are active and timers are running |
| FR-LG01 | Maintain event log with **timestamp** and **identity** | Log writes must not block the caller (safety task, climate task) |
| FR-NW / FR-MQ | WiFi and MQTT (Should/Could) | Inherently asynchronous; must never block safety or control tasks |
| TR-SW03 | **Watchdog timer** to recover from software lockups | A per-task watchdog verifies each concern is alive, not just the overall loop |

**Key observation:** Requirements FR-WS02 and FR-WS05 together require that wind safety *preempts* all other logic with guaranteed latency. In a single-thread polling loop this can only be approximated — if any subtask takes longer than the polling interval (Modbus can take 10–50 ms), the safety check is delayed. With FreeRTOS, the wind safety task at a higher priority will preempt any lower-priority task within one scheduler tick (1 ms default on ESP32).

---

## 4. Structured Comparison

| Criterion | Arduino main loop + polling | FreeRTOS tasks | Verdict |
|-----------|----------------------------|----------------|---------|
| **Guaranteed wind safety preemption (FR-WS05)** | Cannot guarantee; Modbus or relay code can delay the wind check | High-priority task preempts all others within 1 ms | **FreeRTOS** |
| **171 s motor run-time (FR-A03)** | Requires non-blocking state machine spanning hundreds of `loop()` calls; error-prone | `vTaskDelay(pdMS_TO_TICKS(171000))` in windowController task; readable, testable | **FreeRTOS** |
| **Six independent dwell timers (FR-A11)** | Six `millis()` timestamps stored per-window × per-direction; easy to miscount | One `vTaskDelay()` per motor direction in the window task; trivially independent | **FreeRTOS** |
| **Display stays responsive during relay operations (FR-UI03, FR-WS06)** | Must explicitly yield from every blocking delay or split into state machine | Display task runs at its own priority; relay blocking does not affect it | **FreeRTOS** |
| **WiFi / MQTT must not block safety logic** | MQTT `loop()` must be called periodically; if it stalls, it stalls everything | WiFi/MQTT tasks run on Core 0; safety/control pinned to Core 1 — genuine parallelism | **FreeRTOS** |
| **Watchdog coverage (TR-SW03)** | Watchdog verifies only that `loop()` completes; a slow subtask is invisible | Per-task watchdog: any task that stops feeding its wdt triggers a reset | **FreeRTOS** |
| **Unit testability of logic modules** | Logic embedded in `loop()` is coupled to Arduino timing; harder to extract | Each task module is a standalone .cpp; logic functions compile and run on host | **FreeRTOS** |
| **Conceptual entry barrier for new contributors** | `setup()/loop()` is universally understood; no RTOS concepts required | Requires understanding of tasks, priorities, queues, and stack sizing | **Main loop** |
| **Debugging — linear single-path execution** | Breakpoint in `loop()` stops everything; predictable execution order | Multiple tasks; need RTOS-aware debugger or serial logging per task | **Main loop** |
| **Risk of RTOS-specific bugs** | No priority inversion, no deadlocks, no stack overflow (per task) | Possible priority inversion, deadlock if mutexes misused, stack overflow if undersized | **Main loop** |
| **Code volume for this project** | Additional state machines add lines, not reduce them; complexity is hidden | More setup (task creation, queue creation) but each task body is straightforward | **Neutral** |
| **ESP32 SDK compatibility** | WiFi, BLE, NVS already run on FreeRTOS internally — `loop()` is one RTOS task | Native to the SDK; all internal services are first-class peers | **FreeRTOS** |

**Summary:** The main loop wins on familiarity and simplicity-in-isolation. FreeRTOS wins on every criterion that matters for the safety-critical timing requirements of this project.

---

## 5. Recommended Task Architecture

### 5.1 Framework Selection

**Selected framework: Arduino + FreeRTOS**

The Arduino framework is used (not bare ESP-IDF) because:
- All sensor libraries (Modbus, RTC, LCD, SD) have Arduino-compatible implementations — no rewriting required.
- The Arduino API (`Wire`, `SPI`, `Serial`, `digitalRead`, etc.) is familiar to the target contributor community.
- FreeRTOS is included automatically; the full FreeRTOS API is available alongside the Arduino API.
- PlatformIO's `platform = espressif32` + `framework = arduino` provides this combination with a single line in `platformio.ini`.

### 5.2 Task Map

The following tasks are proposed. Each task owns exactly one concern and communicates with others only through queues or semaphores — no shared global variables.

| Task name | Core | Priority | Period / trigger | Owned concern | FRS refs |
|-----------|------|----------|-----------------|---------------|----------|
| `taskWindSafety` | 1 | **SAFETY (3)** | 500 ms | Read wind sensor; evaluate speed/direction; publish override flag and close-all command | FR-WS01–WS08 |
| `taskModbusSensors` | 1 | CONTROL (2) | 60 s (config.) | Poll SenseCAP S200 and FG6485A over RS485; post readings to sensor queue | FR-S01–S05, FR-W01–W04 |
| `taskClimateControl` | 1 | CONTROL (2) | Triggered by new sensor reading | Evaluate T / RH vs setpoints; determine desired ventilation level; post window commands | FR-C01–C10 |
| `taskWindowController` | 1 | CONTROL (2) | Triggered by command queue | Execute relay pulses; enforce OPEN/CLOSE mutual exclusion; run motor timers; track OPEN/CLOSED/MOVING state; enforce dwell times | FR-A01–A12, FR-ST02 |
| `taskUI` | 1 | BACKGROUND (1) | 200 ms | Scan keypad; update LCD1602; navigate menus; feed display queue | FR-UI01–UI10, FR-M06 |
| `taskLogger` | 1 | BACKGROUND (1) | Triggered by log queue | Write events to NVS ring buffer; optionally write to SD card | FR-LG01–LG09 |
| `taskHeartbeat` | 1 | BACKGROUND (1) | 500 ms | Toggle HB LED; feed watchdog | TR-SW03, §4.9 |
| `taskWifiMqtt` | 0 | BACKGROUND (1) | Event-driven | Manage WiFi connection; publish MQTT telemetry; serve web interface | FR-NW01–NW04, FR-MQ01–MQ04 |

**Priority levels (3 levels only — deliberately minimal):**

| Level name | FreeRTOS priority value | Tasks |
|------------|------------------------|-------|
| SAFETY | 3 | `taskWindSafety` |
| CONTROL | 2 | `taskModbusSensors`, `taskClimateControl`, `taskWindowController` |
| BACKGROUND | 1 | `taskUI`, `taskLogger`, `taskHeartbeat`, `taskWifiMqtt` |

Using only three priority levels avoids the most common FreeRTOS maintainability trap: priority proliferation (many tasks at many levels, resulting in non-obvious execution ordering that is hard to reason about).

### 5.3 Inter-Task Communication

All data exchange between tasks uses FreeRTOS primitives:

| Queue / semaphore | Sender → Receiver | Content |
|-------------------|--------------------|---------|
| `qSensorData` | `taskModbusSensors` → `taskClimateControl` | Latest T, RH, wind speed, wind direction |
| `qWindowCmd` | `taskClimateControl`, `taskWindSafety`, `taskUI` → `taskWindowController` | Window command: `{window, OPEN/CLOSE/STOP, source}` |
| `qWindowState` | `taskWindowController` → `taskUI`, `taskLogger`, `taskWifiMqtt` | Current estimated state of each window |
| `qLogEvent` | All tasks → `taskLogger` | Log entry: `{timestamp, source, event_type, data}` |
| `semWindOverride` | `taskWindSafety` → `taskClimateControl`, `taskUI` | Binary semaphore: wind override active / cleared |

No task reads from another task's private variables. All shared state is mediated through these primitives.

### 5.4 Core Pinning

The ESP32-S3 has two cores (Core 0 and Core 1).

| Core | Assigned tasks | Rationale |
|------|----------------|-----------|
| Core 1 | All safety, control, UI, and logging tasks | Deterministic; not shared with WiFi interrupts |
| Core 0 | `taskWifiMqtt` | WiFi driver runs on Core 0 by default; co-locating avoids cross-core contention |

### 5.5 Startup Sequence

The startup sequence is implemented in `setup()` before tasks are launched:

1. Initialise hardware peripherals (UART, I2C, SPI, GPIO).
2. Read DS3231 RTC; if WiFi later syncs NTP, update RTC.
3. Close all three windows (issue CLOSE relay pulse for M1, M2, M3) and wait for their respective motor run-times to elapse (21 s + 21 s + 171 s, serialised) — satisfies FR-ST02.
4. Set all window estimated states to `CLOSED`.
5. Create queues and semaphores.
6. Create and start all FreeRTOS tasks.
7. Enter scheduler (`loop()` body is empty or unused).

---

## 6. Maintainability Mitigations

The most common concern raised about FreeRTOS in community projects is that it is unfamiliar and adds subtle failure modes (priority inversion, stack overflow, deadlock). The following practices are adopted to mitigate this for less experienced contributors.

### 6.1 One file per task

Each task has exactly one header (`taskXxx.h`) and one implementation file (`taskXxx.cpp`). A contributor who wants to understand or modify, for example, the climate control logic opens `taskClimateControl.cpp` and finds only that logic. There is no need to navigate a large `loop()` function to find the relevant lines.

### 6.2 No shared global variables

All cross-task data exchange goes through the queues and semaphores defined in §5.3. A contributor can trace any data path by following queue puts and gets — there is no hidden dependency on a global variable modified elsewhere.

### 6.3 Logic separated from RTOS machinery

Each task file is split into two parts:
- A **logic layer**: pure functions (e.g. `evaluateClimate(SensorData, Config) → WindowMask`) that take inputs and return outputs with no RTOS calls. These are the functions exercised by unit tests on the host.
- A **task wrapper**: the `void taskXxx(void *)` function that reads from queues, calls the logic layer, and writes to queues.

A contributor who only wants to change the climate control algorithm works entirely in the logic layer and never touches RTOS primitives.

### 6.4 Only three priority levels

As noted in §5.2, only three priority levels are used: SAFETY, CONTROL, and BACKGROUND. These names are defined as constants in a central `taskConfig.h`:

```c
#define PRIORITY_SAFETY     3
#define PRIORITY_CONTROL    2
#define PRIORITY_BACKGROUND 1
```

Any contributor can see at a glance which tasks win when they compete for CPU time, without needing to understand the numeric FreeRTOS priority scale.

### 6.5 Stack sizes documented centrally

All stack sizes are defined as named constants in `taskConfig.h` with a comment explaining why the value was chosen. Example:

```c
#define STACK_WIND_SAFETY   2048   // Modbus read + float arithmetic; measured peak ~1.4 kB
#define STACK_WIFI_MQTT     8192   // ESP WiFi + MQTT client libraries; large by necessity
```

Stack overflow detection (`configCHECK_FOR_STACK_OVERFLOW = 2`) is enabled in `FreeRTOSConfig.h` so that an incorrectly sized stack causes an immediate, identifiable crash rather than silent memory corruption.

### 6.6 README for firmware/src

The README.md lists every task, its priority level, its sole responsibility, and the queues it reads from and writes to. This is maintained alongside the source and reviewed at every pull request. A new contributor can understand the overall architecture without reading any source code.

### 6.7 Unit tests cover the logic layer

The test directory contains host-native unit tests (Unity framework via PlatformIO) for the logic layer of each task. These tests run without hardware and without the RTOS. A contributor can verify that a change to the climate control algorithm is correct by running `pio test -e native` without a connected board.

---

## 7. Decision Record

| Field | Value |
|-------|-------|
| **Decision** | Use FreeRTOS tasks (Arduino framework + FreeRTOS API) |
| **Date** | 2026-03-27 |
| **Alternatives considered** | Arduino main loop with `millis()` polling; bare ESP-IDF with FreeRTOS |
| **Rejected: Arduino main loop** | Cannot provide guaranteed preemption for wind safety (FR-WS05). The 171 s M3 motor run-time and six independent dwell timers produce unmanageable `millis()`-based state machines. The WiFi/MQTT stack already runs on FreeRTOS internally — the `loop()` approach fights the SDK. |
| **Rejected: bare ESP-IDF** | All sensor libraries target Arduino API; rewriting or porting them costs effort without benefit. The Arduino framework on ESP32 wraps ESP-IDF; contributors get both APIs. |
| **Accepted: Arduino + FreeRTOS** | Satisfies all timing and safety requirements. Mitigated maintainability concerns through the practices in §6. Community members familiar only with Arduino can contribute to the logic layer (§6.3) without needing RTOS knowledge. |
| **Key risk** | Stack overflow in an under-sized task; mitigated by stack-overflow detection (§6.5) and centralised stack size documentation. |
| **Owner** | To be confirmed (project maintainer) |
```
