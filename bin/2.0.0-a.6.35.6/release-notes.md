# 2.0.0-a.6.35.6 — Coredump retrieval via web GUI (last Phase-7 readiness gap)

The IDF panic handler has been writing coredumps to the dedicated 64 KB partition at 0x620000 since 1.19.0 (gh#21). Pre-this-patch the firmware never *read* them — a panic during the upcoming 14-day soak would have left a forensic gold mine in flash with no way out short of physically connecting `esptool.py` to the unit. This patch closes that gap end-to-end: boot-time detection, GUI surfacing, admin-only download, confirm-then-erase, audit logging for every access.

## What changed end-to-end

```
                       ┌──────────────────────────────────────────────────────────┐
                       │ T4 (data_manager) — once at boot                         │
                       │   esp_core_dump_image_check() == ESP_OK?                 │
                       │     → cache present=true, size=N                         │
                       │     → log_post(LOG_SYSTEM, value_a=18, value_b=KB)       │
                       └─────────────────────────┬────────────────────────────────┘
                                                 │
       ┌─────────────────────────────────────────┼─────────────────────────────┐
       │                                         │                             │
       ▼                                         ▼                             ▼
┌────────────────────┐         ┌─────────────────────────────────┐  ┌────────────────────────────┐
│ Canonical status   │         │ T11 /api/coredump/status (GET)  │  │ SD CSV: value_a=18 row     │
│ JSON: mode.flags[] │         │   → JSON { present, size_bytes, │  │   surfaces in daily-       │
│   += "coredump_    │         │           size_kb, fw_ver }     │  │   review grep              │
│   available"       │         └─────────────────────────────────┘  └────────────────────────────┘
└────────┬───────────┘
         │
         ▼
┌──────────────────────────────────────┐  ┌─────────────────────────────────────────┐
│ Local GUI Alarms card (blue badge)   │  │ Public status dashboard (operator can   │
│   + Log → Diagnostics panel:         │  │ mirror the flag → badge mapping to      │
│       size + fw_ver + Download/Erase │  │ render the same badge)                  │
└────────┬─────────────────────────────┘  └─────────────────────────────────────────┘
         │
         ▼
┌──────────────────────────────────────────────────────────────────────────────────┐
│ Operator workflow (admin session, browser):                                       │
│   1. Click Download → browser saves coredump-2.0.0-a.6.35.6-<ts>.bin              │
│      T11 streams in 4 KB chunks; audit row value_a=19; rate-limited 1/10 s        │
│   2. Decode offline: idf.py coredump-info -t raw -c <file> firmware-<ver>.elf     │
│   3. Click Erase → confirm dialog → POST /api/coredump/erase                      │
│      esp_core_dump_image_erase(); audit row value_a=20; flag cleared              │
└──────────────────────────────────────────────────────────────────────────────────┘
```

## Three new endpoints

All admin-only (existing `admin_only_or_send_error()` cookie session gate), rate-limited (1 op per 10 s for download + erase), audit-logged on every access.

| Method + URI | Behaviour |
|---|---|
| `GET /api/coredump/status` | JSON `{ok, present, size_bytes, size_kb, fw_ver}`. Not rate-limited — needed for the GUI's poll-on-tab-open. |
| `GET /api/coredump/download` | Streams partition bytes in 4 KB chunks as `application/octet-stream`. `Content-Disposition: attachment; filename="coredump-<ver>-<unix_ts>.bin"` so the browser's Save dialog produces a clearly-named file. Rate-limited. Audit row `value_a=19`. |
| `POST /api/coredump/erase` | Calls `esp_core_dump_image_erase()` + `dm_coredump_clear()`. Idempotent (clean partition → 200 OK with `"note":"no coredump to erase"`). Rate-limited. Audit row `value_a=20`. |

## Security envelope

| Layer | Mechanism | Validated on bench |
|---|---|---|
| Network | LAN-only, no public ingress | Existing infrastructure |
| Authentication | Cookie session (16-byte token) | `admin_only_or_send_error()` |
| Authorisation | Admin role required | No session → 401 ✓ / Farmer session → 403 ✓ |
| Rate limiting | 1 op per 10 s on download + erase | 2nd erase within 10 s → 429 + Retry-After ✓ |
| Audit logging | LOG_SYSTEM row per access, initiator=WEB | SD CSV rows verified ✓ |
| Wipe policy | Explicit POST after operator confirms decode | GUI button disabled until download in current session |
| Public dashboard | Flag string only, never partition contents | `mode.flags=['coredump_available']`, no bytes |
| Confirmation | JS `confirm()` dialog before erase, irreversibility called out | Visual verification post-deploy |

**Sensitive-content note**: a coredump is a RAM snapshot from the moment of panic. If a panic happens while WiFi creds / PIN / status-secret are in RAM, those values appear in the dump. The admin-only gate + LAN trust boundary are the operational protections. For a hardened deployment, put the unit on its own VLAN.

## GUI surfacing

**Alarms card (Status section)** — blue "Coredump available" badge whenever the canonical JSON's `mode.flags[]` includes `coredump_available`. Same rendering pattern as the existing `humidity_ctrl_off` / `wind_protect_off` flags from a.6.35.4. Operators don't need to dig into the Log tab to discover a coredump exists; it's visible from the main dashboard.

**Log tab — new Diagnostics section** above the SD-card panel:

```
─── Diagnostics ────────────────────────────────────────────
Coredump:  Available — 45828 bytes (45 KB) • captured on fw 2.0.0-a.6.35.6
           [ Download ]  [ Erase (greyed) ]
```

After download:
```
Coredump:  Available — 45828 bytes (45 KB) • captured on fw 2.0.0-a.6.35.6
           [ Download ]  [ Erase ]   ✓ Download started
```

After erase:
```
Coredump:  No coredump stored. Next panic will be captured automatically.
           [ Download (greyed) ]  [ Erase (greyed) ]   ✓ Erased
```

The Erase button stays disabled until the operator has downloaded in the current session — a fresh page-reload re-disables it to make a "double-click Erase by accident" harder.

## Audit-row rendering

The parser (logparser.py 1.6) renders the three new SYSTEM subtypes with operator-readable strings, initiator-gated to avoid mis-rendering pre-a.6.35.3 heartbeat-pollution rows that happen to land on the same `value_a` numbers:

```
2026-05-19 18:40:25  [SYSTEM ]  System    Coredump from previous panic detected in flash: ~45 KB
2026-05-19 18:43:01  [SYSTEM ]  Web UI    Coredump downloaded by admin (~45 KB transferred)
2026-05-19 18:43:28  [SYSTEM ]  Web UI    Coredump erased by admin (partition wiped)
```

## Offline decode

Once the operator has the `.bin` file saved locally:

```
idf.py coredump-info \
    -t raw \
    -c ~/Downloads/coredump-2.0.0-a.6.35.6-1779999999.bin \
    bin/2.0.0-a.6.35.6/firmware-2.0.0-a.6.35.6.elf
```

Output covers:
- Crashed task name + GDB process id
- Register state at panic (PC, A0-A15, EXCCAUSE, EXCVADDR)
- Full backtrace with `file.cpp:line` resolution (requires the matching ELF — kept in `bin/2.0.0-a.6.35.6/` for exactly this purpose)
- Every task's state + stack high-water mark at panic time

That's enough to root-cause without reproducing on the bench.

## Build delta vs a.6.35.5

| Metric | a.6.35.5 | a.6.35.6 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 350 944 B | **1 354 176 B** | +3 232 B |
| RAM static | 60 553 B | 60 568 B | +15 B |

+3.2 KB flash: three new T11 handlers (~1.5 KB), boot-time coredump check + accessors in T4 (~400 B), the streaming download loop's 4 KB buffer allocation path, the new JS Diagnostics functions in the asset bundle. +15 B RAM: snapshot field + cached coredump state + rate-limit timestamp. Final flash usage **64.6 %** of the 2 MB OTA bank.

## Phase 7 — green light

Pre-this-patch readiness assessment was *"mostly ready but the coredump-retrieval gap means any panic during the soak is hard to root-cause"*. With this patch, the soak can run unattended for 14 days; any panic that occurs is fully recoverable for analysis through the GUI without the operator needing to physically connect to the unit.

Recommended pre-soak sanity check: deliberately trigger a panic on the bench unit (e.g., a one-line `*(volatile int*)0 = 0;` in a test branch), walk the full recovery → notice-the-badge → Download → decode workflow once to prove every link works end-to-end. ~30 minutes; the alternative is discovering a hole in the workflow on day 8 of a soak.

After that sanity check, **soak is ready to start**.
