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
8. **Three-product separation is established design philosophy**: (1) greenhouse controller,
   (2) remote site status (`rfsee.net/hbwv` — passive telemetry sink + display), (3) ROTA
   server (`ota.rfsee.net`, separate `greenhouse-Controller-FOTA-server` repo). Precedent:
   ROTA did *not* extend the status API even though the controller already talked to
   rfsee.net — it got its own vhost, repo, auth scheme (`X-OTA-Auth` HMAC), pinned cert,
   and device-side task (T16), sharing only the TLS mutex. Any remote-control capability
   must respect this boundary: **the status site stays read-only/passive; control is a
   separate product.** This study introduces that separate product: the **Remote command
   service (RCS)** — the fourth independent product in the landscape, alongside
   (1) the greenhouse controller, (2) the remote site status, and (3) the ROTA server.
   Defined in §4-O1; "RCS" is used throughout.

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

### O1 — Heartbeat-pull **Remote command service (RCS)** — the fourth independent product ("ROTA for control")

The transport pattern: the controller polls out on a heartbeat and receives at most one
**signed command envelope** per poll:

```
T17 GET/POST cmd.rfsee.net ──► 200 + {seq, expiry, user, cmds[], sig}  (or 204 = none)
                               controller verifies sig + seq, applies as the REMOTE
                               principal (§5a) with user attribution, ACKs on next poll
```

**Product structure (respects the product separation, §2.8):** the command channel is the
**Remote command service (RCS)** — the fourth independent product in the landscape,
alongside (1) the greenhouse controller, (2) the remote site status, and (3) the ROTA
server — mirroring exactly how ROTA became the third: own vhost (working name
`cmd.rfsee.net`), own repo (analogous to `greenhouse-Controller-FOTA-server`), own auth
scheme and secrets, own device-side client (task slot T17, sharing `s_tls_mx` like
T14/T16 do), own NVS namespace and seq ledger. The status site (product 2) stays a
passive read-only sink/display; the ROTA server (product 3) stays firmware-only. The
RCS owns: the identity front-end and bookkeeping, the command queue + UI, and
the downlink endpoint — while **authorisation truth stays on the controller** (see §5b for
the administration split). Same physical shared host, zero new machines — separation of
responsibility is achieved at the vhost/repo/credential level, as ROTA already proved.

*Rejected variant — piggyback on the T14 status response* (the status server returns the
envelope in place of its empty 204): saves one TLS handshake per cycle and one task slot,
but turns the passive status sink into a control plane, coupling the status site's and
the RCS's deployment, secrets, and failure domains. Rejected for responsibility clutter; recorded
here as a motivated decision (R-RA09 style). The heap rule tolerates the extra
*sequential* handshake fine — only *concurrent* sessions are forbidden — and a dedicated
poll decouples command/revocation latency from the status cadence as a bonus (T17 can run
at 60 s while T14 stays at 60–300 s).

Farmer-writable command set = exactly today's `FARMER_KEYS` allowlist (setpoints, RH
control, wind-protect enable) + mode; admin keys stay LAN-only (or a later,
separately-decided extension).

- **New components: no new machines; one new product** (vhost + repo on the existing
  shared host — the ROTA precedent). The controller gains one small poll client on the
  existing serialized-TLS pattern; no new *concurrent* TLS session. This still satisfies
  R-RA03's spirit (no new servers to run) *and* the one-TLS-session heap rule by
  construction.
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
  **Device-facing flood immunity by construction**: the controller's inbound traffic is
  self-scheduled and bounded (pull) — one request per heartbeat, at most one size-capped
  envelope read, at most one signature verified, connection closed. No party can *initiate*
  traffic toward the device or choose its arrival rate. A flood against rfsee.net is a
  garden-variety web-server DoS: it exhausts the server, never the controller (→ R2).
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
- **C**: broker sees all data (TLS terminates *at* the broker — it protects against the
  network, not the broker; every payload is plaintext there); topic ACLs must be configured
  and maintained per user → admin labour. **I**: broker auth is bearer-credential-shaped;
  per-user device binding on the web side is not native — needs the same front-end work as
  O1 anyway, now split across two systems. E2E payload signing can be rebuilt on top, but
  then the broker does nothing the T14 heartbeat doesn't already do. **A**: broker outage
  kills both directions; free-tier SLAs are "best effort"; vendor lock-in on topic model.
- **Cons — availability under attack**: the subscribed command topic makes the controller's
  inbound traffic **attacker-schedulable (push)**. E2E signing makes flooded junk
  *ineffective* but not *free*: every message still crosses the cell uplink, the single
  TLS session, and the ~31 KB heap world at a rate the attacker chooses — saturating the
  uplink, starving T14/T16, and burying legitimate commands. The only defence is broker-side
  publish ACLs and per-client rate quotas, i.e. the exact third-party configuration we
  neither want to maintain nor trust (one wrong ACL = world-writable command topic); and on
  a free tier an attacker can DoS you *within the broker's own rules* by exhausting the
  plan's connection/message quotas. Contrast O1: pull-with-bounded-response is
  device-flood-immune by construction.
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
non-transferable.

**Terminology note (operator decision, 2026-07-20)**: "device-bound" in this study never
implies *dedicated* secure-element hardware. Excluded: external user tokens
(YubiKey-class) and any discrete SE chip (ATECC608-class) added to the controller BOM.
Used instead: on the user side, the smartphone's **built-in** secure hardware (iOS Secure
Enclave, Android StrongBox / TEE-backed keystore) via the dedicated app; on the
controller side, the ESP32-S3's **on-chip** facilities for key material (V2 device key,
pinned certs, HMAC secrets) — NVS today, optionally hardened with flash encryption +
eFuse key storage (a Phase 3 configuration decision, not new hardware).

Candidates:

| Scheme | Device-bound? | Admin labour | Easy path? | Notes |
|---|---|---|---|---|
| **Dedicated smartphone app + platform-keystore key (PRIMARY, operator preference)** | **Yes** — key pair generated in the phone's hardware-backed keystore (iOS Secure Enclave / Android StrongBox-TEE): non-exportable, biometric-gated, and **excluded from cloud sync** | Enrol once (app shows key hash, admin approves on LAN, §5b); promote = role bit | Yes — open app, biometric, adjust; nothing to remember, no token to carry or buy | Strongest R-RA06 fit: keystore keys cannot sync or transfer, dissolving the synced-passkey caveat. No browser/WebAuthn constraints — the app signs the envelope with plain ECDSA and talks HTTPS to the RCS directly. Cost: the app is a client artifact of the RCS that must be built, maintained, and distributed (channel decided at G2: private APK / TestFlight / store). |
| WebAuthn passkey via the RCS web UI (FALLBACK) | Partly — *platform* passkeys may sync within the user's Apple/Google account (transferable within-account) | Same ceremony | Yes — fingerprint/face in the browser | Kept as the no-app path. Synced-passkey transferability = R4, now confined to fallback users. **Proposed policy invariant (G2): write privilege requires a hardware-bound, non-syncable credential, verified at enrolment — so the platform-passkey fallback is read-only.** |
| WebAuthn + external security key (YubiKey-class) on the RCS web UI (CONTINGENCY, not planned) | **Yes** — key in the token's SE, non-extractable, never syncs; FIDO2 PIN/touch to sign | Same ceremony; **attestation checked at enrolment** proves the authenticator is genuine hardware (this is what makes the write-invariant enforceable, not honor-system) | Carry + tap the token (NFC works on phones) | Would give the *web* path app-grade strength, incl. E2E command signing (`challenge = SHA-256(envelope)`). Satisfies the write invariant. Cost: €25–60/user, token logistics, and a **user-side partial reversal of the no-dedicated-SE decision** (§5 terminology note; controller BOM unaffected). Recorded as the contingency for a future user who can't/won't run the app — not part of the plan. |
| Passwords / PINs / shared secrets | No — freely transferable | Low | Familiar but weakest | Fails R-RA06 outright. Rejected for the remote path. |
| TLS client certificates per user device | Yes (if key non-exportable) | **High** — issuance, renewal, revocation, per-browser install pain | No — browser cert UX is hostile | Fails R-RA02/R-RA08. Rejected. |
| Tailscale node identity (only with O4) | **Yes** — node key is machine-bound | Low — admin approves nodes in console, tags = roles | Yes after one-time client install | Only exists in O4. Pairs with controller PIN for role. |
| IP allowlisting | No (networks change, CGNAT) | Medium | Invisible | Rejected as primary; optional belt-and-braces. |

**The app-key scheme composes with O1/O6 exceptionally well** — beyond login: the app
signs the command envelope directly with the keystore key (**the farmer's phone signs the
command itself**), a plain ECDSA P-256 signature that mbedTLS verifies cheaply on the
controller against the enrolled public keys. For web-fallback users the same effect is
reachable via WebAuthn (`challenge = SHA-256(command envelope)`), though under the
read-only policy above the fallback never signs commands at all. Result: the server is
out of the integrity chain entirely — it cannot forge a command even if fully
compromised. This is the "E2E-signed variant" of O1 referenced below; the plain variant
(server-verified login, HMAC envelope from server key) remains the simpler fallback if
per-user signing proves heavy.

**Is WebAuthn signing (passkey or security key) really E2E to the controller?** Yes,
verifiably: the authenticator signs `authenticatorData || SHA-256(clientDataJSON)`, and
the controller can verify the full chain itself — challenge in `clientDataJSON` equals
the envelope hash, origin equals the RCS's (anti-phishing), then one ECDSA verify
against the enrolled pubkey (mbedTLS; ~250 extra envelope bytes; bonus: the UV flag and
the monotonic signature counter are controller-checkable — PIN-presence and clone
detection). **But note the WYSIWYS caveat**: the page code that *builds* the envelope
and requests the signature is served by the RCS itself, and an authenticator has no
display — so a compromised RCS cannot forge autonomously, yet could trick an
*actively present* user into signing a chosen (allowlist-bounded, fully attributed)
envelope. The app closes exactly this gap: its signing code is distributed out-of-band,
so the server can neither forge **nor deceive**. This grades the paths: app > attested
security key via web > platform passkey via web — and is a further motivation for the
write-invariant and app-primary decisions above.

### 5a. The REMOTE principal — a third logical user on the controller

The controller gains a third principal, **REMOTE**, alongside FARMER and ADMIN:

- **Not a login.** REMOTE has no PIN and cannot authenticate locally or via the LAN GUI;
  it exists only as the actor under which T17 applies pulled commands. Its authorisation
  ceiling is fixed at (a subset of) the farmer allowlist — it can never widen past what a
  local farmer may do, whatever the envelope claims.
- **User attribution rides the envelope.** Every envelope carries the issuing user's
  enrolled-credential identity (compact: enrolled-key slot index + short label set at the
  enrolment ceremony). Audit records then read `initiator=REMOTE, user=<slot>` —
  distinguishing "farmer at the LCD", "farmer on the LAN GUI", and "farmer remotely via
  user X" in the same T9/T14 log stream. Per the CLAUDE.md rule, the new record encoding
  lands together with a `logparser.py` update (slot→label resolution) in the same change.
- **Precedent in the firmware**: the audit initiator enum already reserves `LOG_BY_MQTT`
  for the never-activated T12 command path — REMOTE generalises that allocation instead
  of inventing a parallel mechanism.
- **Attribution strength differs per variant — record at G2**: in the E2E-signed variant
  the controller verifies the envelope signature against *that user's* enrolled public
  key, so the logged identity is cryptographically bound — the server cannot mis-attribute
  a command (non-repudiation). In the plain HMAC variant the user field is a
  server-asserted claim, trusted only as far as the server.
- **Accountability is also usability** (R-RA08): the per-command ACK on the next poll
  carries the applied/rejected result with the same user attribution, so the command UI
  can show each user "your change was applied at 14:32" — closing the loop that an
  async channel otherwise leaves open.

### 5b. User administration — split along the privilege gradient

"Where is user administration — remote or controller?" Answer: **administered once, on
the controller; stored twice (master → mirror); the privilege gradient decides which side
may *initiate* which change.** The RCS is a workflow front-end and viewport, never an
administrative authority of its own.

- **Authorisation truth = controller NVS.** The enrolled-key table (slot, public key,
  short label, role bit read/farmer, revoked flag) lives on the controller and is the
  *only* store consulted when T17 verifies an envelope. A compromised RCS therefore
  cannot mint, promote, or impersonate a user — consistent with R1's disposition.
- **Bookkeeping and front-end = the RCS, including a *derived mirror* of the key table.**
  Login sessions, WebAuthn ceremony pages, pending-enrolment records, queue history,
  display names — plus a mirror of credentials/roles, needed for one reason only: users
  also log into the website itself (view status, queue commands, read ACKs), and the RCS
  must verify those login assertions against the public keys. The mirror is derived, never
  administered: poll ACKs carry a key-table version/digest and the RCS reconciles after
  every change. On divergence the controller wins, with asymmetric failure in the safe
  direction — **control fails closed** (command signatures are verified only against the
  controller's master, so a stale mirror can never authorise anything) while **viewing
  fails open for ≤ 1 poll** (a locally-revoked user may still log into the site until the
  mirror syncs — bounded window on the least sensitive asset; accept at G2, noted under
  R8). Losing or corrupting the RCS's store never changes what the controller accepts.
- **Privilege-increasing operations (enrol, promote read→farmer) require a local admin
  act.** Primary (app) flow: the app generates its key pair in the phone's keystore and
  submits the pending enrolment (pubkey, key hash, requested label) to the RCS over
  HTTPS → it rides down to the controller → the **admin approves it in a LAN-GUI admin
  session**, comparing the key hash shown in the user's app with the one the controller
  received, then sets role and label → slot becomes active. Web-fallback flow: identical,
  except the key is a WebAuthn passkey registered on the RCS pages (WebAuthn needs
  a secure HTTPS context, so browser-based key generation can never run on the HTTP LAN
  GUI — one more reason the app is the cleaner path). The ceremony is once per user, so
  R-RA02 labour stays bounded; it mirrors the FR-AC08 philosophy that security-critical
  admin acts are local/physical.
- **Privilege-decreasing operations (revoke, demote) are additionally accepted from
  remote** (RCS admin console → synced at next poll). Motivation for the asymmetry:
  revocation fails safe — the worst a compromised server can do with it is lock users out,
  which is a bounded availability nuisance (⊂ R2), never an escalation; while *waiting*
  for a LAN visit to revoke a stolen device would be the dangerous direction (R3/R8).
  Local revocation via the LAN GUI works too, and wins ties (controller table is truth).
- Residual noted: a compromised RCS could spam pending enrolments (admin-side
  annoyance, nothing activates without local approval) or mass-revoke (availability,
  ⊂ R2). Both accepted with motivation at G2.

### 5c. Credential inventory — controller↔RCS exchange and compromise blast radius

**What authenticates the controller↔RCS leg (T17)?** Machine credentials only, per
the ROTA precedent — user credentials never travel on this leg:

1. **Server → controller**: TLS with a **pinned certificate** for the `cmd.` vhost
   (ROTA pattern: embedded default + GUI-uploadable replacement, as for `ota.rfsee.net`).
2. **Controller → server**: per-device request authentication on every poll/ACK.
   Two variants for G2/Phase 3:
   - **V1 — HMAC header** (`X-CMD-Auth`, HMAC-SHA256 over method|path|body|timestamp|nonce
     with a per-device secret provisioned via the LAN admin GUI into NVS) — the proven
     `X-OTA-Auth` pattern, symmetric: the secret also lives on the RCS.
   - **V2 — device signature**: the controller signs requests with a device key pair
     (mbedTLS ECDSA, key generated on-device at provisioning; the reserved flash region at
     `0x630000` was already earmarked for `esp_secure_cert`-style material in
     remoteOTAstudy.md). The RCS stores **only the device public key** → the server holds
     *zero* usable machine secrets.
3. **User authentication is not a channel credential.** The user's identity enters only as
   the envelope signature (§5/§5a) — data verified at the endpoint, regardless of transport.

**Blast radius of a full RCS compromise, per credential:**

| Credential | Held where | Leaks? | Consequence / disposition |
|---|---|---|---|
| User **private** keys (app keystore key; web-fallback passkey) | Phone hardware-backed keystore (app) / platform authenticator (fallback) — user device only | **No** — non-extractable by keystore/WebAuthn design; never sent anywhere | None. This is the headline answer: *user credentials do not leak.* |
| User public keys + labels | Controller (master) + RCS (mirror) | Yes — but public material | Zero authentication value. Mild privacy exposure (who has access) → fold into R7. |
| Website session tokens | The RCS, short-lived | Yes (active sessions) | Hijacked sessions can *view* and fiddle the queue UI, but cannot sign envelopes (E2E) → confidentiality only, bounded by session TTL. |
| Device channel secret (V1 HMAC) | Controller NVS + the RCS | **Yes** (symmetric) | Attacker can impersonate the *controller to the server*: post fake ACKs/liveness, read the pending queue. **No path to command the controller** (it only pulls; envelopes are user-signed). Persists past server recovery → **rotation runbook required** (LAN GUI re-provision + server config). → R11. |
| Device public key (V2) | The RCS | Yes — public | Nothing. V2's selling point: the RCS stores no machine secret at all. |
| TLS private key (`cmd.` vhost) | The RCS host | Yes | Subsumed by the compromise itself; controller-side pinning means recovery = re-key + push new pinned cert (GUI-uploadable, ROTA precedent). |
| Farmer/admin PINs, WiFi credentials | Controller NVS only (PINs salted-hashed) | **No** — never sent to any product | None. FR-AC05/AC06 separation of local and remote credentials holds by construction. |

Net: with V2 chosen, a fully-owned RCS yields an attacker privacy-grade data
(telemetry, names, command history) and active viewing sessions — **no credential that
authenticates anyone to anything**, and no control over the greenhouse. With V1, add one
rotatable machine secret whose abuse is limited to impersonating the controller's
*reporting*, not its *control*. The remaining integrity/availability consequences of a
compromise (withhold, delay, revoke-spam) are already dispositioned under R1/R2/§5b.

## 6. Comparison matrix

Scores: ++ strong / + adequate / o neutral / − weak / −− disqualifying-ish. "C/I/A" against
the *remote path* (local FR-MQ05 autonomy is preserved by all options).

| Criterion | O1 heartbeat cmd | O2 MQTT | O3 tunnel+VPS | O4 gateway+overlay | O6 read-only |
|---|---|---|---|---|---|
| R-RA03 components | **++ (0 machines; 1 new vhost+repo)** | − (broker) | −− (VPS) | − (on-site box) | ++ (0) |
| R-RA02 admin labour | + (PHP you already run) | − (broker ACLs) | −− (VPS care) | o (console + box care) | ++ |
| R-RA04 no direct access | ++ (pull only) | + | − (GUI proxied) | − (GUI reachable to enrolled) | ++ |
| Confidentiality | + (TLS + front-end login) | − (broker sees all) | + | ++ (WireGuard E2E) | + |
| Integrity | **++ (E2E-signed cmds)** | o (broker-mediated) | o (session-based) | + (device-authed + PIN) | ++ (no cmds) |
| Availability (remote path) | **++ (pull: device flood-immune, stateless)** | −− (broker SPOF **+ attacker-schedulable inbound, quota-exhaustion DoS**) | − (tunnel+VPS SPOF) | o (box SPOF, relays help) | + |
| Availability (local, on remote failure) | ++ | ++ | ++ | ++ | ++ |
| R-RA06 device binding | ++ (app keystore key) | − (bolt-on) | o (bolt-on) | ++ (node key) | ++ (app/passkey) |
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

**O1 (heartbeat command channel) + app-held device-bound keys (web-fallback = read-only
passkeys), E2E-signed command variant, built in two stages: stage 1 = O6 (authenticated
read), stage 2 = command path.**
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
D1 transport (O1 vs O4 vs O6-only), D2 binding (confirm app-primary; app distribution
channel — private APK / TestFlight / store; ratify the **write-invariant**: write privilege
requires a hardware-bound, non-syncable credential verified at enrolment — platform-passkey
fallback stays read-only, attested security keys admissible as contingency only, §5/R4),
command scope (FARMER_KEYS+mode only? admin keys ever?),
liveness requirement (is 60–300 s acceptable? if not → O4), revocation latency
(poll-delay acceptable?), the **§5b administration split** (confirm: privilege-up = local
ceremony only, privilege-down also from remote — and accept its two residuals), the
**device-channel auth variant** (§5c: V1 ROTA-style HMAC + rotation runbook vs V2
device signature with no server-side machine secret), and the
**identity boundary across products**: user
identity/enrolment lives in the RCS, but R7 wants the status view
(product 2) authenticated too — does product 2 delegate login to the RCS (SSO-lite,
one identity store) or keep its own simpler read-only auth (fully independent products,
two logins)? Deliverable: ADR-style decision record in design/, FRS
amendment list (new FR-RA section; TR-NW04 rewrite; FR-NW06 remote clarification;
FR-AC01 amendment: third logical principal REMOTE, §5a).
*Gate G2: architecture chosen, risks from Phase 1 each assigned mitigate/accept.*

**Phase 3 — Security design (TDS).**
For O1: envelope format (seq, expiry, cmds, sig), key enrolment & promotion ceremony (LAN
GUI, admin session), revocation path, NVS schema (new namespace → minor version bump per
SemVer rule), REMOTE principal + audit-log records (`initiator=REMOTE` + user slot
attribution, generalising the reserved `LOG_BY_MQTT` code; logparser.py updated in the
same changeset per the CLAUDE.md rule), front-end auth design, rate
limits, server hardening checklist for the new RCS vhost (own
repo, own secrets, status-site api.php untouched). Fresh **TLS heap audit**
(tls_leak_audit.md re-walk) if any TLS profile changes. Deliverable: remoteAccess_tds.md
mirroring rota_tds.md structure with R-* requirement IDs.
*Gate G3: TDS review; every §9 mitigation traceable to a TDS requirement.*

**Phase 4 — Implement + adversarial test on the bench.**
Order: (a) front-end login + read (=O6, shippable value on its own), (b) enrolment +
promotion, (c) command envelope end-to-end on FDA4. Adversarial test list drawn from
Phase 1: replay old envelope, tampered envelope, expired envelope, revoked key, wrong-role
key writing non-FARMER key, lockout behaviour, server-compromise simulation (forge attempt
from server side must fail if E2E variant chosen; remote enrolment/promotion **without the
local LAN approval** must never activate a slot; remote revocation must land within one
poll). Add to test/ suite pattern
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
| R1 | Remote command service (RCS) compromised → attacker enqueues commands | I | **Mitigate**: E2E-signed envelopes — server cannot forge. Residual: withholding/delay → R2. Product separation (§2.8) also confines the blast radius: status site and ROTA server have distinct secrets/deployments. |
| R2 | Server withholds/delays commands or status (DoS on remote path) | A | **Accept with motivation**: remote path is convenience; local control autonomous (FR-MQ05). Detection: staleness indicator on front-end. |
| R3 | Stolen/lost enrolled user device | C, I | **Mitigate**: keystore key/passkey requires local biometric or device PIN to sign; admin revocation (latency ≤ 1 poll — accept that window with motivation). |
| R4 | Synced passkey silently copies to user's other devices | R-RA06 | **Mitigated by the app-primary decision** (§5): keystore keys are excluded from cloud sync. Residual confined to web-fallback users — **proposed policy: fallback = read-only, so no write credential can ever sync**; confirm at G2. |
| R5 | Replay of captured command envelope | I | **Mitigate**: monotonic seq in NVS + expiry — ROTA ledger pattern, proven. |
| R6 | Heap regression from envelope parsing/verification on T14 path | A (device) | **Mitigate**: verify against 20 KB floor in soak (G5); size envelope ≤ one TLS record; no new TLS session by design. |
| R7 | Status data disclosure (greenhouse telemetry) to unauthenticated web | C | **Mitigate**: front-end login for status view (fixes FR-NW06 remotely); decide at G2 whether telemetry confidentiality is High or Low value (motivates depth). |
| R8 | Ex-farmer retains access (offboarding gap) | C, I | **Mitigate**: revocation list on controller (synced via heartbeat) + admin console; test in Phase 4. Per-user attribution in the audit log (§5a) gives a forensic trail of anything done before revocation took effect. |
| R9 | LAN GUI weaknesses (no CSRF/SameSite, public /ws) exploited by a device on farm WiFi | C, I (local) | Out of the remote path in O1 (GUI never internet-exposed), but **flagged**: cheap hardening (SameSite=Lax + origin check) recommended regardless; decide at G2. |
| R10 | Admin loses own remote access (key lost) | A (admin) | **Accept with motivation**: recovery = LAN/site path, mirroring FR-AC08 hardware-recovery philosophy — remote lockout must never be softer than local. |
| R11 | Device channel secret leaks in an RCS compromise (V1 HMAC variant only) | I (reporting) | **Mitigate**: choose V2 device-signature at G2 (no server-side machine secret), or keep V1 + rotation runbook (LAN re-provision after any server incident). Abuse ceiling either way: impersonate controller *reporting*, never *control* (§5c). |

## 10. What this study deliberately does not decide

- Whether admin-level *remote* writes ever get enabled (Phase 2 decision; default: no —
  admin config stays a LAN/site act, matching the hardware-recovery philosophy).
- MQTT's fate: FR-MQ01..05 stay in the FRS as Could; O2's rejection here is about the
  *control path*, not about a future one-way telemetry publish.
- Multi-tenant / fleet-scale identity (out of scope below 10 units — revisit trigger).
