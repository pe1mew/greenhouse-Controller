# Modbus Interface Contract — Window Opening Sensor ↔ Greenhouse Controller

| | |
|---|---|
| Document | Client-facing interface contract for the wire-encoder window-opening sensor |
| Version | 1.2 (2026-09-05 — §8 re-derived for the target window: 1.5 m in **171 s**, the M3 travel time; FR-WP20 threshold given margin) |
| Audience | The **greenhouse controller** (Modbus master) — its firmware author and its integrator |
| Derived from | `design/TDS.md` **v0.7**, which is normative. Every statement here cites the requirement it comes from; where the two disagree, the TDS wins and this document is wrong |
| Firmware | Build type `0x01`, version 1 — read it from register 30007 before trusting anything below |
| Verification | Every behaviour described here is HIL-verified unless marked **not yet verified**; evidence in `software/hil/testReport.md` |

---

## 1. What this document is for

The sensor is a pure Modbus RTU slave that reports **how far a window is open**,
whether it is **at an end stop**, and **whether its own readings can be
trusted**. This document tells the controller four things:

1. **Where** each piece of information lives (register, function code, unit,
   encoding).
2. **What it means** — including what it does *not* mean, which matters more.
3. **How to teach** the sensor its end points over the bus.
4. **Which indications are alarms**, which are health, and what the
   controller should do about each.

It is organised by *purpose* (§4–§7), with the flat register map in §3 for
reference. Read §2 first: two of the sensor's behaviours are deliberate
deviations from the Modbus standard, and one register is signed.

---

## 2. The link — facts the controller must build in

| Property | Value | Source |
|---|---|---|
| Physical / framing | RS-485, Modbus RTU, **9600 baud, 8N1** | FR-MB01 |
| Slave address | **40** (solder jumper open) or **45** (bridged). Fixed at power-on. **There is no address register** — the address cannot be read, set or discovered over the bus | FR-S03, FR-MB07 |
| Function codes | **FC04** read input, **FC03** read holding, **FC06** write single, **FC16** write multiple. Anything else → exception 01 | FR-MB08–12 |
| Byte order | Register data **big-endian** (high byte first). CRC-16 **low byte first** | FR-MB25 |
| Signedness | **Every register is unsigned except 30012**, which is two's-complement `int16` | §2.7 |
| Response time | ≤100 ms hard; 95 % within 15 ms. **Measured: 4.08 ms median, 4.14 ms worst over 1 000 requests** | FR-MB20/21 |
| Inter-frame gap | 3.5 character times ≈ **4.0 ms** at 9600. Leave ≥5 ms between requests | FR-MB03 |
| Max read | 15 input registers (`0x0000–0x000E`), 7 holding (`0x0000–0x0006`). A read that touches any address beyond the map → exception 02 **for the whole request**, no partial data | FR-MB13/14/27 |
| Exceptions | Only **01, 02, 03**. Code 04 is **never** emitted | FR-MB18/29 |
| Silence | A valid, addressed request **always** gets a reply — normal or exception. Silence means bad CRC, wrong address, broadcast, or a physical problem | FR-MB02/05/17 |

### 2.1 Deliberate deviations from the Modbus standard

- **Broadcast (address 0) is ignored** — not executed, not answered. The
  standard requires slaves to execute broadcast writes; this device does not,
  so that a fleet can never be reconfigured by one stray frame (FR-MB06).
- **No address register.** Two devices per bus segment, 40 and 45, period.
  More instrumented windows need a hardware change (FR-MB07, FR-WP11).

### 2.2 Write semantics the controller can rely on

- A write that returns success **took effect**. A write that returns exception
  03 **changed nothing** — the device never clamps a value into range and never
  echoes success while discarding (FR-MB19).
- **FC16 is atomic.** If any register in the request is invalid — including the
  two cross-register rules in §8 — the whole request is rejected and *no*
  register changes, not even the valid ones (FR-MB22).
- FC06's success response is a byte-exact echo of the request (FR-MB30).
- Settings **persist** across power loss (FR-S39). The controller need not
  re-send configuration after a sensor restart — with one exception, §5.5.

---

## 3. Register map — reference

Modicon numbering (3xxxx / 4xxxx) is what the tables use; the raw PDU address
is one less than the last digits. `30001` = raw `0x0000`.

### 3.1 Input registers — FC04, read-only

| Raw | Reg | Meaning | Unit | Range / encoding | Purpose (§) |
|---|---|---|---|---|---|
| `0x0000` | 30001 | Opening, **instantaneous** | 0.1 mm | 0–65534; **65535 = sensor fault** | §4 |
| `0x0001` | 30002 | Opening, **averaged** | 0.1 mm | as above | §4 |
| `0x0002` | 30003 | Opening, **minimum** in current averaging window | 0.1 mm | as above | §4 |
| `0x0003` | 30004 | Opening, **maximum** in current averaging window | 0.1 mm | as above | §4 |
| `0x0004` | 30005 | Raw ADC code, pre-calibration | counts | 0–1023 | §6, §8 |
| `0x0005` | 30006 | **Status bits** | bitfield | see §3.3 | §5, §7 |
| `0x0006` | 30007 | Identification | — | high byte build type (`0x01` release, `0x81` bench build — **do not deploy**), low byte firmware version | §9 |
| `0x0007` | 30008 | Uptime since reset | s | 0–65535 saturating | §7 |
| `0x0008` | 30009 | Bus CRC error count | — | wraps | §7 |
| `0x0009` | 30010 | Served request count | — | wraps | §7 |
| `0x000A` | 30011 | Seconds since last **valid** sensor reading | s | 0–65535 clamped | §7 |
| `0x000B` | 30012 | **Movement rate — SIGNED** | 0.1 mm/s | `int16`; + opening, − closing, 0 at rest | §4, §7 |
| `0x000C` | 30013 | Raw code captured at last **closed-end** stop | counts | 0 = none since reset | §6 |
| `0x000D` | 30014 | Raw code captured at last **open-end** stop | counts | 0 = none since reset | §6 |
| `0x000E` | 30015 | Opening as **percentage** of full travel | 0.1 % | 0–1000; 65535 = fault | §4 |

### 3.2 Holding registers — FC03 read, FC06/FC16 write

| Raw | Reg | Meaning | Unit | Valid | Default | Persists |
|---|---|---|---|---|---|---|
| `0x0000` | 40001 | Zero offset reported at the closed point | 0.1 mm | 0–65534 | 0 | yes |
| `0x0001` | 40002 | **Measurement window** | ms | 100–60000 | 1000 | yes |
| `0x0002` | 40003 | **Averaging window** | s | 1–600, and `40003 × 1000 ≥ 40002` | 10 | yes |
| `0x0003` | 40004 | **Full travel** between the calibration points | 0.1 mm | 1–65534 | 10000 | yes |
| `0x0004` | 40005 | Raw code with window **closed** | counts | 0–65534, and `|40006 − 40005| ≥ 64` | 0 | yes |
| `0x0005` | 40006 | Raw code with window **open** | counts | 1–65535, same rule | 1023 | yes |
| `0x0006` | 40007 | **Teach command** | — | 0 idle/abort, 1 arm | 0 | **no** — reads 0 after every reset |

**40006 may legally be less than 40005.** That is how a sensor mounted so the
raw code *falls* as the window opens is expressed. The device handles both
senses; the controller need not care (FR-E04).

### 3.3 Status register 30006 — bit definitions (FR-S33, normative)

| Bit | Mask | Set while… | Class | § |
|---|---|---|---|---|
| 0 | `0x0001` | no measurement window has completed since reset or since the last write to 40002 | **startup gate** | §4.4 |
| 1 | `0x0002` | the averaging accumulator has not filled since reset or since a write to 40002–40006 | startup gate | §4.4 |
| 2 | `0x0004` | **wiper fault** — the position sensor's wiper is open | **alarm** | §7.1 |
| 3 | `0x0008` | **end of travel reached** — exactly one end sensor active | state | §5 |
| 4 | `0x0010` | **end-switch loop fault** — both end sensors active at once | **alarm** | §7.1 |
| 5 | `0x0020` | teach in progress | state | §6 |
| 6 | `0x0040` | **implausible raw code** — position outside anything the window can reach | health | §7.2 |
| 7 | `0x0080` | **position not following the carriage** — the switches saw movement, the position did not | health | §7.2 |
| 8–15 | — | always 0 | — | — |

---

## 4. Polling for the opening distance

### 4.1 Which register, for which purpose

| The controller wants… | Read | Why this one |
|---|---|---|
| A value to **position against** (closed loop, FR-WP09/FR-WP12) | **30001** | Instantaneous; never older than one measurement window (§4.2). No filtering to lag behind the actuator |
| A value that **ignores wind sway** (FR-WP21) | **30002** | Boxcar mean over 40003 seconds. With 40003 = 1 it is a 1 s mean with ≈0.45 s group delay, meeting FR-WP09 |
| **How much the leaf is moving** in the wind | **30003 / 30004** | Min and max over the same averaging window — the sway envelope, without polling at window rate (FR-E08) |
| A **percentage**, without dividing by the travel | **30015** | `(30001 − 40001) × 1000 / 40004`, 0–1000 = 0–100.0 %. Tracks 30001 exactly (FR-E20) |
| **Is it moving, and which way** | **30012** | Signed rate in 0.1 mm/s. **Decode as `int16`** — read unsigned, a closing window shows ≈65 000 (FR-E10) |

All position registers are in **0.1 mm** relative to the *closed* calibration
point plus 40001. The target 1.5 m window reads 0–15 000. The value is clamped to
`[40001, 40001 + 40004]`; the leaf may physically travel beyond the sensors
(§5.3) and the reading simply holds at the end value while it does (FR-E04).

### 4.2 Freshness — a number the controller sets, not a hope (FR-E17)

The value in 30001 at the instant a read is served is **never older than
40002 milliseconds**. The same bound applies to 30005 and 30015. There is no
"trigger a sample" command and no need for one: the controller bounds
staleness purely by configuring 40002.

| 40002 | Update rate | Max age of 30001 | Use |
|---|---|---|---|
| 100 | 10 Hz | 100 ms | only for a fast actuator. On the target window the leaf moves <1 ADC count per update at this rate (§8.1) |
| **500** | 2 Hz | 500 ms | **the target window, polled at 1 Hz** — fresh every poll (FR-WP08), 4.4 mm of staleness on a 1.5 m stroke, 0.29 % |
| 1000 (default) | 1 Hz | 1 s | the target window polled at ≤0.5 Hz — 8.8 mm, 0.58 % |
| 5000 | 0.2 Hz | 5 s | slow logging |

One exception: during the 2 s fault-hold grace after a wiper fault (§7.1), the
last valid opening is deliberately held and may be up to 2 s older. **30011 is
non-zero throughout**, so the controller can see it (FR-E17, FR-S36).

### 4.3 Coherence — one request, one snapshot (FR-S24, FR-WP12)

Every register in a single FC04 response comes from **the same measurement
update**. In particular 30001 and 30005 in one response always agree — the
controller will never see a position from one window and its raw code from
the next. **Read the registers the control loop needs in one FC04**, and the
snapshot is atomic with respect to motion for free.

**The recommended poll is a single FC04 of all 15 input registers**
(`0x0000`, quantity 15 — 30 data bytes). It costs ~35 ms of bus time at
9600 baud, is served in ~4 ms, delivers position, envelope, rate, every status
bit and every diagnostic atomically, and — as §6.3 explains — is exactly the
read that completes a teach.

### 4.4 The startup gate — bits 0 and 1

After any reset the position registers read **0** until the first measurement
window completes, and **bit 0 is set** for as long as that is true (FR-S23).
Zero is a legal opening, so **the controller must check bit 0 before
believing a position of 0.** Bit 0 also re-asserts after every write to 40002.

Bit 1 says the averaging accumulator has not filled. While it is set, 30002 is
the mean over only the samples collected so far — a real partial mean, not
zero-padded — so it is *usable*, just not yet over the full span (FR-S23).
Bit 1 re-asserts after any write to 40002–40006 and after a teach commits.

### 4.5 The fault sentinel — 65535

When the wiper is open for more than 2 s, **30001–30004 and 30015 all read
65535** and bit 2 is set (FR-E07). 65535 is outside the legal range of every
position register (max 65534), so it is unambiguous in-band — but the
controller should test **bit 2**, not the value, because bit 2 sets at the
same moment and reads cleanly regardless of which registers were requested.

### 4.6 Rate-of-change plausibility (FR-WP20)

The controller's jump-rejection rule is supported directly by 30012: any
|30012| well beyond the actuator's known speed is a reading to distrust. Rate is
computed from the last two completed windows, so it needs two windows of
movement before it is valid.

**Do not use the study's 0.58 %/s figure as the limit itself.** It *is* this
window's normal speed: 1.5 m in 171 s = 8.77 mm/s = **0.585 %/s**. A limit
placed at nominal has zero margin — the first quantisation step or a slightly
brisk actuator trips it. 30012 reads **±88** during a healthy traverse (±37 of
quantisation at 40002 = 500, so up to ±125 on a legitimate reading). Set the
limit at **|30012| > 175** (17.5 mm/s, twice nominal); the arithmetic is in
§8.1.

---

## 5. End switches

### 5.1 What the sensor reports

Two proximity sensors, one at each end of the *opening range* (§5.3), are read
through one supervised analogue input. The result is two status bits:

| Bit 3 | Bit 4 | Classification | Meaning for the controller |
|---|---|---|---|
| 0 | 0 | *neither active* | The leaf is somewhere between the stops — **or** a sensor cable is open (§5.4) |
| **1** | 0 | *one active* | **The leaf is at an end stop.** Which end: read the position (§5.2) |
| 0 | **1** | *both active* | **Fault.** Both sensors made at once — impossible on a healthy installation |

The state is debounced: a change is published only after 20 ms of stability,
so a controller polling at 50 ms never sees a bounce (FR-E15). The
classification runs at ≥10 Hz independently of the position path and of the
Modbus response time (FR-E14).

### 5.2 Which end? — the position says (deliberate design)

The switch input tells the device *that* a stop was reached, not *which*. It
spends its resolution on supervision instead. **The controller decides which
end from the position**: bit 3 set with 30001 near 0 is the closed stop; with
30001 near 40004 it is the open stop. This is also how the device itself
decides for the teach (§6.2), using direction of travel.

### 5.3 What bit 3 means — and the one thing it does not (FR-WP07)

**Bit 3 reports the sensor, not the window.** It is set while the end sensor is
*active*. On an installation built to `design/description.md` §8.1 — sensor
zone long enough that the sensor stays made from first contact all the way to
where the leaf comes to rest — bit 3 set means *fully closed / fully open*,
and FR-WP07 (fully-closed distinguishable from near-closed) is met.

Where the sensor zone is **shorter than the overtravel**, the sensor triggers
as the leaf passes and releases as it continues to its mechanical stop. The
window then sits fully closed while bit 3 reads *clear*. **No firmware can
correct this.** The controller must therefore treat "bit 3 clear" as *not
proven at a stop*, never as *proven away from one*, unless the installation
has been checked:

> **Installation check the controller can run.** Drive the window fully
> closed, wait, read bit 3. It must be set and stay set with nothing touching
> the window. Repeat fully open. If either clears, the sensor zone is too
> short. This is a commissioning check, not a runtime one.

### 5.4 Known limit — a broken sensor cable is invisible (FR-E16)

The end sensors are PNP normally-open. An inactive sensor, an open cable and a
signal shorted to 0 V all read identically: *neither active*. **The device
cannot detect a disconnected or shorted end-sensor cable and must not be
documented as if it can.** Bit 4 covers *both active at once* and nothing
else.

The consequence: if an end sensor's cable fails, the controller sees a window
that **never reaches a stop**. The health check in §7.2 (bit 7) will not fire
either, because it needs the switches to witness movement. The controller's
own cross-check is the defence — a window commanded closed, whose position
reads 0 and whose rate has been 0 for the expected travel time, but whose
bit 3 never set, has a sensor problem worth reporting (FR-WP19).

### 5.5 Independence

The switch path and the position path are separate front-ends. A switch fault
never suppresses, holds or alters the reported opening; a wiper fault never
alters bits 3/4. Bits 2 and 4 may be set in any combination (FR-E16, FR-S33).

---

## 6. Teaching the end points

The device ships knowing nothing about its window. It learns the two raw codes
that correspond to *closed* and *open* — registers 40005 and 40006 — by one of
two routes. **Neither route sets the full travel (40004)**; that is a tape
measurement the installer writes by hand (§6.4).

### 6.1 Route A — manual, three writes (FR-E05)

1. Close the window. Read **30005**. Write that value to **40005**.
2. Open the window. Read **30005**. Write it to **40006**.
3. Write the measured travel, in 0.1 mm, to **40004**.

Direction does not matter — write closed to 40005 and open to 40006 whatever
the numbers do. `|40006 − 40005|` must be ≥64 or the write is refused with
exception 03 (§8). All three can go in one FC16 to `0x0003`, quantity 3
(worked example in §10).

### 6.2 Route B — commanded teach from the end sensors (FR-E19)

The device captures the raw code **at the moment each end sensor makes**, so
the calibration points are the sensor positions exactly, with no operator
reading. Sequence:

| Step | Controller does | Device does | Visible as |
|---|---|---|---|
| a | **FC06 40007 = 1** | Arms. Discards any earlier captures | bit 5 set |
| b | Drives the window to **one** stop | On the debounced *one active* transition, captures 30005 into 30013 (closed end) or 30014 (open end) | bit 3 set; bit 5 still set |
| c | Drives to the **other** stop | Captures the other register | bit 3 set; bit 5 still set |
| d | **Reads both 30013 and 30014** — see §6.3 | When both are captured **and** both have been read since capture: commits them to 40005/40006, persists, clears bit 5, resets 40007 to 0 | **bit 5 clears within one measurement window**; FC03 shows the new 40005/40006; bit 1 re-asserts (FR-E05) |

**Which register a capture lands in** is decided by the **direction of the
last movement** (30012): a window that was opening has reached the open end.
Only when nothing has moved since reset — the window was already sitting at a
stop at power-on — does it fall back to whichever of 40005/40006 the code is
nearer. On a device whose calibration is still the factory default that
fallback is meaningless, so **always teach with movement**: start away from
both stops, or drive to one stop, then the other. Do not arm with the window
resting at a stop it has not moved to since power-on.

**Abort**: FC06 40007 = 0 at any point. Captures are discarded, 40005/40006
are untouched, bit 5 clears.

**Refusal**: if the two captured codes are closer than 64 counts (§8), the
commit does **not** happen. Bit 5 stays set, 40007 stays 1, and 30013/30014
remain readable so the operator can see what was captured. This is the device
telling you the sensor barely moved between the stops — the draw-wire is not
following, or the sensors are too close together. Abort, fix, re-teach.

**Not persisted**: 40007 reads 0 after any reset, and an armed teach does not
survive a power cut (FR-S39). A controller that reads bit 5 clear and 40007 = 0
after a restart has a device that is *not* in teach, whatever it was before.

### 6.3 The read handshake — why it exists and how to satisfy it

The commit waits until the controller has **read** both captured values. This
is what lets a controller polling at 1 Hz never miss a capture: the device
will not retire 30013/30014 until they have been collected (§2.7 note).

Precisely, from the implementation:

- A read is "seen" for a register when **any FC04 whose range includes it** is
  served. One FC04 of `0x000C` quantity 2 satisfies both; so does the 15-register
  full-map read of §4.3.
- Each **new capture clears the seen-flag** for that register. The commit
  therefore only ever happens after the controller has read the *fresh*
  values, never stale ones from before arming.
- The value alone does not tell you whether it is fresh — arming resets the
  flags, not the register contents. Do not try to infer capture from the
  number; watch **bit 5**.

**Controller procedure that always works:** arm; keep the normal full-map
FC04 poll running at ≥ once per measurement window; drive to each stop in
turn; when **bit 5 clears**, the teach is committed. Confirm with an FC03 of
40005/40006.

### 6.4 After either route — the full travel

Write the distance **between the two sensor operating points** — not between
the hard stops — to **40004** in 0.1 mm. Teach captures the sensor positions,
so the travel figure must match them. Getting this wrong scales every reading
by the same error; the percentage register 30015 is wrong by the same factor.

For the target window, **40004 = 15000** (1500.0 mm). The compliance study
assumed 2 m; the window actually being fitted is 1.5 m. Measure the real
sensor-to-sensor distance and write *that* — the round figure is a placeholder.

**Allow time.** A teach needs the window driven to both stops, and on this
window each traverse is **171 s**. Budget **≈6–7 minutes** from arm to bit 5
clearing, and do not abort early because nothing seems to be happening — bit 5
stays set throughout, and the captures land only as each sensor makes.

### 6.5 Drift check — 30013/30014 outside teach (FR-E18)

Outside teach, every arrival at a stop still updates 30013 or 30014 with the raw
code seen there. They are a **calibration-drift diagnostic**: compare them to
40005/40006. A growing difference means the mounting has shifted or the wire has
slipped — measured against a physical reference, not against a model. Both read
0 after a reset until a stop is next reached. Reading them outside teach has no
side effect.

---

## 7. Alarms and health

Two different classes. **Alarms** (bits 2, 4) mean a front-end is faulty *now*
and the reported value is either replaced by a sentinel or cannot be trusted.
**Health** indications (bits 6, 7) mean the position signal is *not credible*;
they change nothing in the reported value and are deliberately slow. Treat an
alarm as *act now* and a health bit as *schedule an inspection* — and never
stop the plant on either alone (FR-WP18: wind override must not depend on
position).

### 7.1 Alarms

| Bit | Fault | What the controller sees | What it should do |
|---|---|---|---|
| **2** | **Wiper open** (FR-E07) | For the first 2 s the last valid opening is held and 30011 counts up. After 2 s: 30001–30004 and 30015 = **65535**, bit 2 set. Clears within 2 s of the wiper returning | **Fall back to time-based control** (FR-WP17). Surface to the operator (FR-WP19). The end switches still work — bit 3 remains a valid stop indication |
| **4** | **Both end sensors active** (FR-E16) | Bit 4 set within 200 ms; 30001 continues to track the window unchanged | Distrust bit 3 until cleared; position is still good. Surface to the operator |

**What bit 2 does not cover.** A wiper **shorted** to either rail is
electrically identical to the wiper resting at the corresponding end stop, and
**is not detected** by bit 2 (FR-E07, narrowed 2026-09-01). Plainly: *a wiper
shorted to 0 V reports the window as fully closed.* The defence against that
is bit 6 (§7.2), and it only works on an installation with electrical headroom
(§7.3).

### 7.2 Health — is the position signal credible?

| Bit 6 | Bit 7 | Read it as | Where to look |
|---|---|---|---|
| 0 | 0 | Nothing to report. Note bit 7 is only *tested* when the window passes through a stop | — |
| 0 | **1** | **The mechanism.** The switches say the window moved through three full stop-to-stop sequences; the position moved <16 counts each time | Tangled, snapped or slipping draw-wire; seized drum; loose coupling. The potentiometer itself is usually fine |
| **1** | 0 | **Signal out of range** but still moving | Partial short on the wiper, or a calibration that no longer matches the installation |
| **1** | **1** | **Position path dead.** Neither follows the window nor reads anything reachable | Wiper conductor shorted to a rail, or broken |

- **Bit 7** (FR-E23) sets after **three consecutive** departure sequences
  (at-stop → away ≥ 2 windows → at-stop) each with <16 counts of wiper
  excursion; it clears on the first good sequence. It needs no calibration and
  detects the one fault no electrical test can — a mechanically dead draw-wire
  with an electrically perfect potentiometer. Verified by detaching the
  draw-wire on the bench: seven stop arrivals while the reading moved one count.
- **Bit 6** (FR-E24) sets when the raw code sits outside
  `[40005 − M, 40006 + M]`, M = 25 % of the calibrated span, for ≥2 windows;
  clears on return. **It is self-disabling**: on the factory default
  calibration (0/1023) the band covers every code and the bit never sets.

### 7.3 Bit 6 depends on the installation — the controller should know whether it is armed

Bit 6 reports a reading the window *cannot physically produce*. That only
exists if the draw-wire has electrical range to spare at both ends
(`description.md` §8.1: ≥10 % unused at each end). Where the draw-wire is
sized so fully-closed sits at one electrical extreme, there is no spare range,
bit 6 is inert, and a shorted conductor reads exactly like a correctly closed
window.

**The controller can check this once, at commissioning:** with the window fully
closed and fully open, read 30005. Neither reading should be near 0 or near
1023. If either is, bit 6 will never fire on that installation and the
controller must not rely on it.

### 7.4 Liveness and restart detection

| Register | Use |
|---|---|
| **30011** | Seconds since the last valid reading. Rising while bit 2 is clear means readings have stopped arriving before the fault detector has tripped — poll this alongside position (FR-S36) |
| **30008** | Uptime. **Went backwards → the sensor restarted.** Re-read the holdings to confirm configuration survived (it will — FR-S39 — but 40007 is 0 and bits 0/1 are set) (FR-S34) |
| **30009** | CRC errors seen on the bus, any address. Rising with no corresponding failure at the controller means noise or another device's frames are being corrupted (FR-S35) |
| **30010** | Requests this device answered. Difference between two reads = requests served in between; compare with what the controller sent to find lost frames (FR-S35) |

A **silent** sensor — no response at all to an addressed request within 100 ms
— is a physical or addressing problem, never a firmware state: the device
always answers a valid addressed request (FR-MB17). After a supply dip it
answers within 1 s of rail recovery (FR-S22).

---

## 8. Configuration the controller may set

| Register | Set it for | Constraint | Recommended for M3 |
|---|---|---|---|
| 40002 | Update rate / freshness bound (§4.2) | 100–60000 ms; `40003 × 1000 ≥ 40002` | **500** for a 1 Hz poll (FR-WP08 needs the window shorter than the poll period); **1000** if polling at ≤0.5 Hz. 100 buys nothing on this window — §8.1 |
| 40003 | Averaging span for 30002–30004 | 1–600 s; same rule | **1–2** (a 1 s mean satisfies FR-WP09 and FR-WP21) |
| 40001 | Offset at the closed point | 0–65534 | 0 |
| 40004 | Full travel | 1–65534, 0.1 mm | **15000** (1.5 m, the target window) |
| 40005/40006 | Calibration points | `|40006 − 40005| ≥ 64` | by teach (§6) |

### 8.1 Sizing 40002 to the window's speed — the numbers behind the recommendation

The target window — the M3 — takes **171 s to open or close fully over 1.5 m**:
**8.77 mm/s**, or 0.585 % of stroke per second. Everything in this table follows
from that one figure; a faster or slower window re-derives it the same way.
(An earlier draft used "≈2 minutes"; the exact travel time changes the numbers
below by about 30 % and the FR-WP20 conclusion in kind, so use 171 s.)

| Quantity | Value | Consequence |
|---|---|---|
| Speed | 8.77 mm/s | 30012 reads **±88** during normal travel |
| Resolution (§7.3 headroom, ~80 % of range used) | ≈1.8 mm / count | the raw code advances ≈**4.8 counts per second** |
| Movement per update, 40002 = 100 | 0.88 mm ≈ **0.5 count** | the position changes on roughly every other update; 30012 is quantised to steps of ≈180 — **twice the speed it is measuring** — and is useless |
| Movement per update, 40002 = 1000 | 8.8 mm ≈ 4.8 counts | position resolves each update; 30012 resolves to ≈±18, about 20 % of the true rate |
| Movement per update, 40002 = 2000 | 17.5 mm ≈ 9.6 counts | 30012 to ≈±9; freshness bound 17.5 mm = 1.2 % of stroke |
| Max staleness at 40002 = 1000 (FR-E17) | 8.8 mm = 0.58 % of stroke | inside FR-WP04's 1 % resolution ask — a 1 s window is *not* the limiting factor |
| Full traverse | 171 s = **171 windows** at 1 s | FR-E23's "away ≥ 2 windows" and FR-E24's "≥ 2 windows" are met 85× over; **a teach (§6) takes at least two full traverses, ≈6 minutes** |

| Movement per update, 40002 = 500 | 4.4 mm ≈ 2.4 counts | position resolves each update; 30012 to ≈±37, about 40 % of the true rate |

**So: 40002 = 500 for a 1 Hz poll, 1000 for slower polling.** FR-WP08 asks
for a fresh value at *every* 1 Hz read, and a window equal to the poll period
can alias — two consecutive polls landing in the same window and repeating a
value. 500 ms is the shortest window that both guarantees a fresh value per
1 Hz poll *and* still moves the leaf several counts per update. Below it, a
100 ms window costs ten times the bus traffic for position updates that differ
by less than one ADC count, and it destroys the rate register. If the
controller polls at ≤0.5 Hz, 1000 is the right value and 2000 is still
defensible; the freshness bound stays under 2 % of stroke.

**FR-WP20's threshold needs margin, not a different basis.** The study's
plausibility rule rejects jumps above **0.58 %/s** — and 0.585 %/s is this
window's *normal* speed, so the study's figure was evidently derived from the
171 s travel time. Used directly as the limit it has **zero margin**: a healthy
reading of 88 ± 37 quantisation reaches 125 and would be rejected. The
controller must set its threshold above nominal with margin: **|30012| > 175**
(twice the nominal speed, 17.5 mm/s) rejects genuine jumps and never the
actuator. §4.6 is updated to match.

Two cross-register rules, both enforced atomically (exception 03, nothing
changes):

- **FR-S31**: `40003 × 1000 ≥ 40002` — the average must span at least one
  measurement window. Write 40003 *before* raising 40002, or send both in one
  FC16.
- **FR-E06**: `|40006 − 40005| ≥ 64` — a degenerate calibration, where one LSB
  of noise would swing the whole travel, is refused. Applies to teach commits
  too, and to values loaded from storage (a corrupt pair boots on defaults).

Side effects worth knowing: a write to **40002** aborts the window in progress
and re-asserts bit 0; any write to **40002–40006** clears the averaging
accumulator and re-asserts bit 1 (FR-S30, FR-E05). Writing an unchanged value
does nothing and does not wear the flash.

---

## 9. What the controller should do at start-up

1. **FC04 30007.** High byte must be `0x01`. `0x81` is a bench build carrying a
   deliberate hang hook — refuse to run against it. Low byte is the firmware
   version this document was written for (1).
2. **FC03 `0x0000` quantity 7.** Check 40005/40006 are not both at the factory
   default (0/1023) on a window that should have been taught; check 40004 is the
   expected travel. If the calibration looks factory-fresh, the window has not
   been taught — §6.
3. **Set 40003 then 40002** if the defaults do not suit (§8).
4. **Poll FC04 `0x0000` quantity 15** at the chosen rate. Ignore position while
   bit 0 is set. Treat the first values after bit 0 clears as valid — there is
   no settling sequence (FR-E01, FR-S18).
5. Record **30008** to detect restarts thereafter.

---

## 10. Worked frames (unit 40 = `0x28`; CRC computed with the project's codec)

```
Read every input register (recommended poll)
  → 28 04 00 00 00 0F  B7 F7
  ← 28 04 1E  <30 data bytes: 30001..30015 big-endian>  <crc>

Read only the status word
  → 28 04 00 05 00 01  26 32

Read the two teach captures (satisfies the §6.3 handshake for both)
  → 28 04 00 0C 00 02  B6 31

Read every holding register
  → 28 03 00 00 00 07  03 F1
  ← 28 03 0E  <14 data bytes: 40001..40007>  <crc>

Arm teach                      Abort teach
  → 28 06 00 06 00 01  AF F2     → 28 06 00 06 00 00  6E 32
  ← (byte-exact echo)            ← (byte-exact echo)

Set a 1 s average
  → 28 06 00 02 00 01  EE 33

Manual calibration in one atomic write: 40004 = 15000, 40005 = 100, 40006 = 800
  → 28 10 00 03 00 03 06  3A 98  00 64  03 20  10 57
  ← 28 10 00 03 00 03  77 F1

A refused write (e.g. 40002 = 65000, or a pair violating FR-E06)
  ← 28 86 03  D3 A9          (function | 0x80, exception 03; nothing changed)
```

Decoding 30012: data bytes `FE 0C` → `0xFE0C` → as `int16` = **−500** =
closing at 50.0 mm/s. Read as unsigned it is 65036 — the same bits.

---

## 11. Limits the controller must design around — summary

| Limit | Consequence | Controller-side mitigation |
|---|---|---|
| Wiper **short** not detected (FR-E07) | Shorted-to-0 V reads *fully closed* | Rely on bit 6 — which requires the §7.3 headroom check to have passed |
| Open **end-sensor cable** not detected (FR-E16) | Window "never reaches a stop" | Cross-check: commanded closed + position 0 + rate 0 + bit 3 never set → report |
| Bit 3 reports the **sensor**, not the window (FR-WP07) | On a short sensor zone, fully closed reads *not at a stop* | Commissioning check in §5.3; treat bit 3 clear as *not proven*, not *proven away* |
| Bit 7 needs **three** stop-to-stop movements | A draw-wire failure is reported after the third traverse, not the first | Accept the latency; the alternative is false alarms on a window rocking at a stop |
| **Two** devices per segment (FR-MB07) | Addresses 40 and 45 only | Fleet growth is a hardware change |
| Broadcast **ignored** (FR-MB06) | A broadcast write does nothing | Address each device |
| 30012 is **signed** | Reading it unsigned shows ≈65 000 when closing | `int16` decode |

---

## 12. Traceability

| Section | TDS requirements | Verified by |
|---|---|---|
| §2 link | FR-MB01–07, 08–12, 13–15, 17–22, 25, 27–30, FR-S03, FR-S19 | Group B (TP-B01–B35) |
| §4 opening | FR-E01, E02, E04, E08, E10, E17, E20, FR-S23, S24, S30 | Group C, stage D, `movement.py`, `fr_e01.py` |
| §5 end switches | FR-E14, E15, E16, FR-WP07 | `fr_e14.py`; **FR-E15 bounce injection not yet verified** (needs a 5 ms injector) |
| §6 teach | FR-E05, E06, E18, E19, FR-S39 | `teach.py` 7/7, TP-B20 |
| §7 alarms / health | FR-E07, E23, E24, FR-S33–S36 | `fr_e07.py`, TP-C01, TP-C02 |
| §8 configuration | FR-S31, FR-E06, FR-MB19, MB22 | Group B, `group_c.py` |
| accuracy claims | FR-E03 | **Not yet verified at five ratios** (needs a precision resistance box); ≤3 LSB stability half passes |
