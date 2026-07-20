# Remote Access Study — secure remote control of the greenhouse controller

Status: **DRAFT for decision** — architecture options + decision plan. Companion to
[remoteOTAstudy.md](remoteOTAstudy.md) (which solved the outbound *firmware* path); this study
covers the interactive *access* path (read + control).

Date: 2026-07-20. Firmware baseline: 2.2.16 (ROTA mainline).

---

## 1. Problem statement and requirements

Enable authorised remote (internet) access to the controller while prohibiting unauthorised
access. The operator (admin) workload and the number of components must be minimal.

Requirements as stated by the operator, restated as numbered items so the trade study and the
risk register can reference them:

| ID | Requirement | Type |
|---|---|---|
| R-RA01 | Unauthorised access to the controller shall be prohibited; authorised access from the web shall be possible. | Must |
| R-RA02 | Administrator management/maintenance labour shall be minimal. | Must |
| R-RA03 | The number of components (servers) shall be minimal — preferably none. | Must |
| R-RA04 | The controller shall not be accessed directly (no inbound path to the device). | Must |
| R-RA05 | Default access level is **read-only**; **farmer** (write) level is granted by promotion by an admin. | Must |
| R-RA06 | User access is bound to a **physical device** and cannot be transferred. | Must |
| R-RA07 | The CIA triad (Confidentiality, Integrity, Availability) applies at all times. | Must |
| R-RA08 | Usability: the secure path shall also be the easy path. | Must |
| R-RA09 | Every identified risk is either mitigated or explicitly accepted with a motivated decision. | Must (process) |

Existing FRS requirements that interact with this study:

- **TR-NW04** — web-interface traffic is *confined to same-LAN*. Remote access **changes this
  declared trust boundary**; the FRS must be amended whatever we choose.
- **FR-NW06** — web interface shall require authentication before *any* information is
  displayed. Current implementation drifts from this: `/api/status`, `/api/history`,
  `/api/config/limits` and `/ws` are public (acceptable on the LAN, material remotely).
- **FR-MQ01..05** — MQTT remote status + command path already envisioned as Could-have;
  task slot T12 reserved, currently disabled. FR-MQ05 (Must): controller fully functional
  without any network — this survives as the availability backstop for *every* option below.
- **FR-AC01..08** — two roles (farmer 4-digit / admin 8-digit PIN), salted-hash storage,
  lockout 5/300 s, hardware-only admin recovery. The remote access model must not weaken these.

## 2. Ground truth (what the platform can actually do)

Verified against the repo on 2026-07-20 (see file:line citations):

1. **5C88 is outbound-only behind a cell-NAT uplink; rfsee.net is its only communication path**
   (remoteOTAstudy.md:41). There is **no on-site gateway device** (no Pi, no router we control);
   the controller is a WiFi STA on the farm network. Outbound HTTPS and NTP are confirmed open.
2. **T14 already phones home**: canonical status JSON POSTed to `https://rfsee.net/hbwv/api.php`
   every 60–300 s with a `sourceidentifier` shared-secret header; server answers **204 with an
   empty body** (impact-analysis-statusReporting.md:44). A status webpage on rfsee.net already
   presents controller state (FR-SW02).
3. **One TLS session at a time is a hard rule.** Handshake costs 30–40 KB internal RAM
   transiently; steady-state largest free block ~31 KB; hard floor 20 KB (rota_tds.md R-R05).
   T14 and T16 already serialise on `s_tls_mx`. **Any option needing a persistent second TLS
   connection (MQTT keep-alive, reverse tunnel) fights this constraint.**
4. **Local GUI is HTTP-only, LAN-grade**: PIN → in-memory session (4 slots) → HttpOnly cookie;
   constant-time compare, NVS lockout. **No CSRF token, no SameSite attribute**; read
   endpoints and `/ws` (2 s status push, 5 clients, unauthenticated) are public. Deliberate
   per TR-NW04 — must be re-hardened before any option that proxies the GUI to the internet.
5. **ROTA gives reusable security primitives**: per-device HMAC request auth (`X-OTA-Auth`),
   pinned self-signed server cert for `ota.rfsee.net`, monotonic seq ledger
   (`manifest-<ver>.json`), staged apply in a night window, post-apply verification.
6. **The data model is already relay-tolerant**: the GUI's live path is a 2 s WS push of the
   same ~2 KB JSON as `/api/status`; config/history/whoami poll at 60/60/120 s. A remote
   viewer fed at T14 cadence (60–300 s) loses only the 2 s liveness, nothing structural.
7. **tls_leak_audit.md predates the ESP-IDF migration** — any option adding client certs /
   mTLS or new persistent TLS requires a fresh TLS heap audit (its own stated re-walk trigger).

**Assumptions to verify early (marked A-#, they gate options):**

- **A-1**: rfsee.net is shared PHP hosting — no root shell, no long-lived processes, **no
  WebSocket support**. If true, "live" relay options via rfsee.net are dead on arrival and
  async polling is the only rfsee.net-shaped transport.
- **A-2**: farm uplink data budget tolerates the current T14 cadence indefinitely (it already
  does today; a command downlink adds only bytes on the existing response).
- **A-3**: no regulatory/insurance requirement forces on-prem-only control (assumed no).

## 3. Two orthogonal decisions

The architecture question decomposes into **(D1) transport** — how bytes reach the controller —
and **(D2) identity & device binding** — how a human on a physical device proves who they are.
Most transports can be paired with most identity schemes; conflating them is how studies go
in circles. They are treated separately and recombined in §6.

---

## 4. D1 — Transport options

### O1 — Piggyback command channel on T14 ("ROTA for control") — extend rfsee.net

The controller already opens an authenticated outbound TLS connection every 60–300 s and
receives an empty 204. Replace the empty body with an optional **signed command envelope**:

```
T14 POST status ──► api.php ──► 200 + {seq, expiry, cmds[], sig}   (or 204 = no commands)
                                controller verifies sig + seq, applies, ACKs on next POST
```

The web front-end on rfsee.net (same host that already shows status) gains a login and a
queue: authorised users read the last-posted status and enqueue commands; the controller
collects them at its next heartbeat. Farmer-writable command set = exactly today's
`FARMER_KEYS` allowlist (setpoints, RH control, wind-protect enable) + mode; admin keys stay
LAN-only (or a later, separately-decided extension).

- **New components: zero.** rfsee.net already exists and is already maintained; the
  controller gains no new task, no new TLS session, no new poll loop. This is the only
  option that fully satisfies R-RA03 *and* the one-TLS-session heap rule by construction.
- **C**: TLS on the leg (CA bundle today; can be upgraded to the pinned-cert pattern like
  ROTA). Status data confidentiality = front-end login (fixes the FR-NW06 drift for the
  *remote* view; today's rfsee.net status page needs auth added regardless).
- **I (the strong point)**: commands can be **end-to-end signed** so the server is a dumb,
  minimally-trusted queue — a compromised rfsee.net can withhold or delay commands
  (availability nuisance) but cannot forge them. Replay killed by the ROTA seq-ledger
  pattern (monotonic seq in NVS + expiry timestamp). Audit trail: every applied remote
  command logged through T9/T14 like any other config change (logparser.py learns the
  new records per the CLAUDE.md rule).
- **A**: fully asynchronous — server outage degrades to "no remote view/control", local
  control and LAN GUI untouched (FR-MQ05 philosophy). No persistent connection to babysit.
- **Latency**: command applies at next heartbeat → worst case = T14 interval (60–300 s).
  **This fits the domain**: greenhouse thermal time constants are tens of minutes, and
  design/todo.md already rules out remote manual window open/close. Must be stated honestly
  in the manual: "changes take effect within N minutes."
- **Cons**: not live (no 2 s tiles remotely — remote view refreshes at heartbeat cadence);
  PHP code on rfsee.net grows (queue + login + enrolment UI) and becomes security-relevant;
  emergency "revoke now" also rides the heartbeat (revocation latency = one interval).

### O2 — MQTT broker (managed cloud broker, activate T12 / FR-MQ01..05)

Controller keeps a persistent TLS connection to a managed broker (HiveMQ Cloud / EMQX
free tier); status published, commands subscribed; a static web page (or the broker's
dashboard) speaks WSS to the same broker.

- **Pros**: near-real-time both ways; already envisioned in the FRS (FR-MQ03); T12 slot
  reserved; brokers are commodity; no self-managed server (managed tier).
- **Cons — heap first**: a *persistent* TLS connection holds mbedTLS I/O buffers
  permanently and must interleave with T14/T16 handshakes under the one-session rule —
  either serialise (connect/disconnect each cycle, which erases the latency advantage and
  reduces O2 to a worse O1) or renegotiate the 20 KB floor with a fresh TLS audit and
  buffer tuning (MFL extension). **The main selling point (liveness) is exactly what the
  platform constraint taxes.**
- **C**: broker sees all data (third party in the confidentiality boundary); topic ACLs
  must be configured and maintained per user → admin labour. **I**: broker auth is
  bearer-credential-shaped; per-user device binding on the web side is not native — needs
  the same front-end work as O1 anyway, now split across two systems. **A**: broker outage
  kills both directions; free-tier SLAs are "best effort"; vendor lock-in on topic model.
- Net: pays the platform's scarcest resource for a liveness the domain doesn't need,
  and adds a third party without removing any front-end work.

### O3 — Reverse tunnel / relay (controller-initiated tunnel to a rented VPS; frp-style)

Controller (or a helper) holds an outbound tunnel open; remote browsers hit the VPS, which
forwards to the local HTTP GUI. Full live GUI remotely, LAN-identical UX.

- **Pros**: exact same GUI remotely, 2 s tiles and all; conceptually simple.
- **Cons**: needs a **real VPS** (violates "preferably none" — rfsee.net shared hosting
  can't do this, A-1); a **permanent TLS tunnel** from the ESP32 (same heap fight as O2,
  worse — it's a proxy socket, not a compact MQTT client, and no off-the-shelf ESP-IDF
  frp client exists → custom tunnel code in C on the controller, the highest-risk code
  in the whole option space); and it **exposes the LAN-grade GUI to the internet** —
  the no-CSRF / public-endpoints / HTTP-session surface (§2.4) becomes internet-facing
  behind whatever auth the relay adds. Hardening the GUI to internet grade is a project
  of its own. **A**: tunnel babysitting (reconnect logic, VPS patching = recurring admin
  labour, violating R-RA02).

### O4 — On-site gateway + zero-trust overlay (Tailscale / Cloudflare Tunnel on a Pi or router)

Add a small always-on device on the farm LAN running Tailscale (or cloudflared). Remote
users join the tailnet from enrolled devices and reach the controller's LAN GUI as if local.
Controller firmware: **zero changes**.

- **Pros**: no self-managed server (SaaS control plane + relays); **device binding is
  native and genuinely non-transferable** — a Tailscale node key identifies a machine, which
  is the cleanest possible R-RA06 story; ACLs give per-user, per-port reachability; full
  live GUI (2 s tiles) because traffic is LAN-speed end-to-end; controller heap untouched
  (the tunnel runs on the gateway).
- **Cons**: introduces the **first on-site hardware component** — power, SD-card wear,
  remote recovery of the gateway itself ("who tunnels to the tunnel box when it wedges?"),
  and a site visit when it dies: R-RA03 is violated in hardware rather than servers, at a
  NAT'd site that is expensive to visit. Users need client software installed+enrolled
  (fine for a handful of farmers; it *is* the easy path once enrolled). Third-party trust:
  Tailscale coordination plane (data plane is WireGuard E2E; relay sees ciphertext).
  Role model (read vs farmer) still enforced by the controller's PIN/session — the overlay
  authenticates the *device*, the PIN authorises the *role*; two-layer, which is defensible
  (defence in depth) but means the LAN GUI's gaps (no CSRF/SameSite) are now reachable by
  every enrolled device and should be hardened anyway.
- This is the strongest option **if** a live remote GUI is a hard requirement — it buys
  liveness with an on-site box instead of controller heap.

### O5 — Managed IoT platform (AWS IoT Core / Golioth / Arduino Cloud device-shadow)

Rejected early: persistent TLS (same O2 heap fight), vendor lock-in on the data model,
per-device cloud identity administration (R-RA02), confidentiality boundary includes a
hyperscaler, cost scales with fleet, and it replaces proven in-repo patterns (ROTA, T14)
with a platform SDK. Nothing it offers that O1/O4 don't, at higher coupling. **Discarded**
unless fleet size grows 10×.

### O6 — Null option: remote read-only, no remote control

Add login (device-bound, §5) to the existing rfsee.net status page; no command path at all.
Farmer changes remain LAN/site-only. Zero firmware change beyond none; smallest possible
attack surface. Kept in the matrix as the baseline that any control-capable option must
beat — and as the **fallback posture** if a chosen option's residual risk is later judged
unacceptable (R-RA09 needs a named retreat position).

## 5. D2 — Identity & device binding options

R-RA05/06: read by default, farmer by admin promotion, bound to a physical device,
non-transferable. Candidates:

| Scheme | Device-bound? | Admin labour | Easy path? | Notes |
|---|---|---|---|---|
| **WebAuthn/FIDO2 passkey, device-bound (security key or attestation-checked platform authenticator)** | **Yes** — private key in secure element, by design non-extractable | Enrol once, promote by flipping a role bit on the credential | Yes — fingerprint/face/key-tap, nothing to remember | The web-native answer. **Caveat**: consumer *synced* passkeys (iCloud/Google) replicate across a user's devices — R-RA06 then needs either **hardware security keys** (~€25–60/user, truly non-transferable) or accepting synced-passkey transfer-within-account as a motivated residual risk. This is a named decision point for the workshop. |
| Passwords / PINs / shared secrets | No — freely transferable | Low | Familiar but weakest | Fails R-RA06 outright. Rejected for the remote path. |
| TLS client certificates per user device | Yes (if key non-exportable) | **High** — issuance, renewal, revocation, per-browser install pain | No — browser cert UX is hostile | Fails R-RA02/R-RA08. Rejected. |
| Tailscale node identity (only with O4) | **Yes** — node key is machine-bound | Low — admin approves nodes in console, tags = roles | Yes after one-time client install | Only exists in O4. Pairs with controller PIN for role. |
| IP allowlisting | No (networks change, CGNAT) | Medium | Invisible | Rejected as primary; optional belt-and-braces. |

**The passkey scheme composes with O1/O6 exceptionally well** — beyond login: a WebAuthn
assertion is a signature over a server-supplied challenge. Set
`challenge = SHA-256(command envelope)` and the **farmer's secure element signs the command
itself**. The controller stores enrolled public keys (enrolment/promotion done by the admin
over the LAN GUI — a natural, physical "promotion ceremony") and verifies the ES256/Ed25519
assertion end-to-end. mbedTLS verifies ECDSA P-256 cheaply. Result: rfsee.net is out of the
integrity chain entirely — it cannot forge a command even if fully compromised. This is the
"E2E-signed variant" of O1 referenced below; the plain variant (server-verified login, HMAC
envelope from server key) is the simpler fallback if WebAuthn-on-the-front-end proves heavy.

## 6. Comparison matrix

Scores: ++ strong / + adequate / o neutral / − weak / −− disqualifying-ish. "C/I/A" against
the *remote path* (local FR-MQ05 autonomy is preserved by all options).

| Criterion | O1 heartbeat cmd | O2 MQTT | O3 tunnel+VPS | O4 gateway+overlay | O6 read-only |
|---|---|---|---|---|---|
| R-RA03 components | **++ (0 new)** | − (broker) | −− (VPS) | − (on-site box) | ++ (0) |
| R-RA02 admin labour | + (PHP you already run) | − (broker ACLs) | −− (VPS care) | o (console + box care) | ++ |
| R-RA04 no direct access | ++ (pull only) | + | − (GUI proxied) | − (GUI reachable to enrolled) | ++ |
| Confidentiality | + (TLS + front-end login) | − (broker sees all) | + | ++ (WireGuard E2E) | + |
| Integrity | **++ (E2E-signed cmds)** | o (broker-mediated) | o (session-based) | + (device-authed + PIN) | ++ (no cmds) |
| Availability (remote path) | + (async, stateless) | − (broker SPOF) | − (tunnel+VPS SPOF) | o (box SPOF, relays help) | + |
| Availability (local, on remote failure) | ++ | ++ | ++ | ++ | ++ |
| R-RA06 device binding | ++ (passkey) | − (bolt-on) | o (bolt-on) | ++ (node key) | ++ (passkey) |
| R-RA08 easy=secure | + (web page + fingerprint) | o | + (familiar GUI) | + (after enrolment) | ++ |
| Controller heap/TLS fit | **++ (zero delta)** | −− (persistent TLS) | −− (tunnel in C) | ++ (zero delta) | ++ |
| Liveness (remote) | − (60–300 s) | ++ | ++ | ++ | − |
| Firmware delta / risk | + (small: envelope verify) | − (T12 + TLS work) | −− (custom tunnel) | ++ (none) | ++ (none) |
| GUI hardening needed first | no (GUI stays LAN) | no | **yes, full** | yes (CSRF/SameSite at least) | no |

**Reading**: O1 wins on every stated Must (R-RA01..04, 06, 07) and concedes only liveness —
which the domain (§4-O1) and the FRS's own exclusions don't demand. O4 is the credible
runner-up and the *only* path to a live remote GUI; it trades a farm-site hardware
dependency for that liveness. O2/O3/O5 each pay the platform's scarcest resource (heap /
admin labour / new servers) for benefits O1 or O4 deliver cheaper. O6 is the fallback
posture and, notably, **O1 ⊃ O6**: O1's phase 1 *is* O6 (front-end login first, command
channel second) — so the build order de-risks itself.

## 7. Provisional recommendation (operator decides — §8 workshop)

**O1 (heartbeat command channel) + device-bound passkeys, E2E-signed command variant,
built in two stages: stage 1 = O6 (authenticated read), stage 2 = command path.**
Motivation: it is the only option scoring ++ on components, heap fit, and integrity
simultaneously; it reuses three proven in-repo patterns (T14 heartbeat, ROTA seq ledger,
ROTA pinned-cert); its failure mode is graceful (remote goes dark, greenhouse doesn't care);
and its secure path is genuinely the easy path (open page → fingerprint → adjust setpoint).
**Fallback**: if the workshop rules a live remote GUI a Must, switch to O4 and accept the
on-site gateway with its own remote-recovery plan.

## 8. Decision plan — walking through all relevant elements

Each phase ends in a gate; R-RA09 discipline: no risk leaves a phase without a
**mitigated / accepted-with-motivation** disposition recorded in the register (§9).

**Phase 0 — Verify assumptions (gates everything).**
A-1 rfsee.net capabilities (PHP version, HTTPS config, can it hold a per-user table +
WebAuthn library?), A-2 data budget, A-3 regulatory. Deliverable: §2 assumptions resolved.
*Effort: hours.*

**Phase 1 — Threat model.**
Actors (farmer, admin, visitor-with-URL, internet scanner, compromised-server,
stolen-user-device, insider-ex-farmer), assets (setpoints, status data, PINs/credentials,
audit log, firmware path), STRIDE pass over each interface of the chosen shortlist (O1 +
O4 kept alive until the workshop). Deliverable: threat table feeding §9.
*Gate G1: threat table reviewed by operator.*

**Phase 2 — Architecture decision workshop (the operator decision this study serves).**
Inputs: this study, Phase 0 facts, Phase 1 threats. Decisions to take, explicitly:
D1 transport (O1 vs O4 vs O6-only), D2 binding (security keys vs synced-passkey residual
risk — €/user vs risk acceptance), command scope (FARMER_KEYS+mode only? admin keys ever?),
liveness requirement (is 60–300 s acceptable? if not → O4), revocation latency
(heartbeat-delay acceptable?). Deliverable: ADR-style decision record in design/, FRS
amendment list (new FR-RA section; TR-NW04 rewrite; FR-NW06 remote clarification).
*Gate G2: architecture chosen, risks from Phase 1 each assigned mitigate/accept.*

**Phase 3 — Security design (TDS).**
For O1: envelope format (seq, expiry, cmds, sig), key enrolment & promotion ceremony (LAN
GUI, admin session), revocation path, NVS schema (new namespace → minor version bump per
SemVer rule), audit-log records (+ logparser.py update), front-end auth design, rate
limits, server hardening checklist for the api.php additions. Fresh **TLS heap audit**
(tls_leak_audit.md re-walk) if any TLS profile changes. Deliverable: remoteAccess_tds.md
mirroring rota_tds.md structure with R-* requirement IDs.
*Gate G3: TDS review; every §9 mitigation traceable to a TDS requirement.*

**Phase 4 — Implement + adversarial test on the bench.**
Order: (a) front-end login + read (=O6, shippable value on its own), (b) enrolment +
promotion, (c) command envelope end-to-end on FDA4. Adversarial test list drawn from
Phase 1: replay old envelope, tampered envelope, expired envelope, revoked key, wrong-role
key writing non-FARMER key, lockout behaviour, server-compromise simulation (forge attempt
from server side must fail if E2E variant chosen). Add to test/ suite pattern
(test_10_remote_access.py). *Gate G4: all adversarial tests pass on FDA4.*

**Phase 5 — Soak on 2344 (≥ overnight; this touches greenhouse behaviour, so per
CLAUDE.md: soak before any production push), watching heap_min_kb (gh#40 counters) across
heartbeat+envelope cycles.** *Gate G5: soak clean, heap floor respected.*

**Phase 6 — Production rollout via ROTA** (`rota_release.py` promote; 5C88 applies in its
night window), post-apply verification of `fw_ver` + `asset_version`, then first remote
enrolment ceremony. Manuals: boerHandleiding (how to use — including the honest latency
statement) + beheerderHandleiding (enrol/promote/revoke) in the same changeset
(boer-manual-sync rule). *Gate G6: production verified; register (§9) fully dispositioned.*

## 9. Risk register (seeded — completed in Phases 1–2, dispositioned per R-RA09)

| # | Risk | Affected | Proposed disposition (to confirm) |
|---|---|---|---|
| R1 | rfsee.net front-end compromised → attacker enqueues commands | I | **Mitigate**: E2E-signed envelopes — server cannot forge. Residual: withholding/delay → R2. |
| R2 | Server withholds/delays commands or status (DoS on remote path) | A | **Accept with motivation**: remote path is convenience; local control autonomous (FR-MQ05). Detection: staleness indicator on front-end. |
| R3 | Stolen/lost enrolled user device | C, I | **Mitigate**: passkey needs local biometric/PIN to sign; admin revocation (latency ≤ 1 heartbeat — accept that window with motivation). |
| R4 | Synced passkey silently copies to user's other devices | R-RA06 | **Decide at G2**: hardware keys (cost) vs accept-with-motivation (credential still user-bound + biometric-gated). |
| R5 | Replay of captured command envelope | I | **Mitigate**: monotonic seq in NVS + expiry — ROTA ledger pattern, proven. |
| R6 | Heap regression from envelope parsing/verification on T14 path | A (device) | **Mitigate**: verify against 20 KB floor in soak (G5); size envelope ≤ one TLS record; no new TLS session by design. |
| R7 | Status data disclosure (greenhouse telemetry) to unauthenticated web | C | **Mitigate**: front-end login for status view (fixes FR-NW06 remotely); decide at G2 whether telemetry confidentiality is High or Low value (motivates depth). |
| R8 | Ex-farmer retains access (offboarding gap) | C, I | **Mitigate**: revocation list on controller (synced via heartbeat) + admin console; test in Phase 4. |
| R9 | LAN GUI weaknesses (no CSRF/SameSite, public /ws) exploited by a device on farm WiFi | C, I (local) | Out of the remote path in O1 (GUI never internet-exposed), but **flagged**: cheap hardening (SameSite=Lax + origin check) recommended regardless; decide at G2. |
| R10 | Admin loses own remote access (key lost) | A (admin) | **Accept with motivation**: recovery = LAN/site path, mirroring FR-AC08 hardware-recovery philosophy — remote lockout must never be softer than local. |

## 10. What this study deliberately does not decide

- Whether admin-level *remote* writes ever get enabled (Phase 2 decision; default: no —
  admin config stays a LAN/site act, matching the hardware-recovery philosophy).
- MQTT's fate: FR-MQ01..05 stay in the FRS as Could; O2's rejection here is about the
  *control path*, not about a future one-way telemetry publish.
- Multi-tenant / fleet-scale identity (out of scope below 10 units — revisit trigger).
