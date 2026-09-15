# Release 2.7.1

**Date:** 2026-09-15
**Built on:** 2.7.0
**Implements:** [gh#64](https://github.com/pe1mew/greenhouse-Controller/issues/64) — one
descriptor table replacing the six config-key lists the issue named, and a seventh it did not

**Patch, not minor.** All four of the cadence questions are no: no user-visible feature, no
new task, no new NVS namespace or key, no payload-shape change.

The fourth was the one worth checking rather than asserting, because `GET /api/config/limits`
stopped being a hand-written literal and started being generated. Extracting the 2.7.0
literal at its tag, resolving its `CFG_MIN_*`/`CFG_MAX_*` macros from that tag's
`cfg_limits.h`, and diffing against the descriptor's published bounds:

```
old keys: 40   new keys: 40
PASS: all 40 published keys carry identical bounds; only key ORDER differs
```

Key order is the one thing that did move, and it is not part of the contract: JSON object
order is not significant and `app.js` does `limits[key]`, never a positional index.

That is a statement about the contract, not about the size of the change. This rewrites the
write path for **every** config key, which is why it got a full hardware round rather than
riding along with something else. SemVer classifies the interface; the soak manages the risk.

**2.8.0 stays reserved for the window-position sensor**, which genuinely earns a minor — a
new task and new log encodings. Letting this one take it would have spent the number on a
refactor.

## What changed

Six hand-maintained lists carried the config-key set — `cfg_key_kind()`, the
`apply_config_update()` shadow ladder, `cfg_clamp()`, `ns_key_to_log_id()`, `LIMITS_JSON`
and the mock server — and they drifted **every single time**:

| | what drifted | what shipped |
|---|---|---|
| gh#53 / 2.4.6 | `cfg_key_kind()` written as a boolean | the LCD WiFi-AP toggle went dead |
| gh#57 part 1 / 2.5.0 | 11 keys in the ladder, in neither the clamp nor the limits | `POST /api/config` stored any `int32` |
| gh#57 part 2 / 2.5.1 | `cfg_limits.h` wider than T5's own clamp | a stored 300 was polled at 120, averaging collapsed to 1 sample |
| gh#51 group D | a motor key changed unaudited | — |

A key missing from one list produced no error, just a quietly weaker write path.

**Now a key is declared once**, in `firmware/config/cfg_desc.inc` — one row carrying its
namespace, kind, write routes, bounds, factory default, audit id, channel and shadow slot.
The clamp, the shadow write, the boot loader, the audit row, the kind check and the
published limits all read it. Adding a key is that row.

### The seventh list

gh#64 counted six. The **boot loader** was a seventh — `nvs_load_climate/wind/motor/
system/web()` — carrying the key set *and* the factory defaults, and it was in none of the
six. A key present everywhere else but absent there was accepted, clamped, audited and
published, then **silently reset to 0 on every reboot**, with nothing logged.

The five helpers are now one `cfg_load_group()` call each.

### Write routes are a flag mask

A key is not writable from everywhere, and before this that fact lived only in whichever
handler happened to check. Each row now declares its routes — `CFG_P_Q4` (`POST /api/config`,
the LCD and T10's geo sync), `CFG_P_WEB` (`POST /api/web`), `CFG_P_OTA`
(`POST /api/ota/config`) — across the 51 rows:

- **47** declare `CFG_P_Q4`, **6** of those also `CFG_P_WEB`
- **4** are `CFG_P_OTA` only — the `ota_*` keys, which `POST /api/config` must refuse

That last line used to be a hand-kept list of two. Deriving the refusal set from the flag is
what makes a fifth non-Q4 key tested from the moment its row exists.

### Also in this release

- **`_Static_assert` that the config bound equals its consumer's clamp.** gh#57 part 2 was
  `cfg_limits.h` and T5 disagreeing, with the agreement asserted in a *comment*. The
  compiler now says it. Proven fail-first: restoring `CFG_MAX_POLL_S = 300` reproduces the
  defect and the build refuses.
- **`GET /api/config/limits` is generated from the table**, not a hand-written literal.
- **`bin/check_cfg_desc.py`**, wired into `.githooks/pre-commit`.
- Documentation and tooling: the M2k bus-probe constraint, `bin/resolve_md_rebase.py`,
  `bin/check_gotcha_index.py`, and 30 backfilled gotcha-log index lines.

## Size

|  | 2.7.0 | 2.7.1 | delta |
|---|---|---|---|
| app image | 1 385 456 | **1 378 256** | **−7 200 B** |
| `.flash.text` | 937 750 | 930 190 | −7 560 B |
| `.flash.rodata` | 306 188 | 306 556 | +368 B |
| static RAM (`.data`+`.bss`) | 62 776 | 64 064 | **+1 288 B** |

The `strcmp` ladders cost more code than the table costs data: dropping them takes 7 560 B
of text, while the descriptor rows minus the deleted `LIMITS_JSON` literal are a net +368 B
of rodata.

**The RAM is one buffer, deliberately.** `GET /api/config/limits` is now built from the
table into a `static char s_json[1280]` on first request and cached — every bound is a
compile-time constant, so the payload can never change at runtime. That buffer plus its
`s_built` flag *is* the +1 288 B. The alternative was rebuilding ~850 B of JSON on every
page load; this trades 1 280 B of `.bss` for that, on a part with 320 KB of internal RAM.

## Verification

**Static** — `python bin/check_cfg_desc.py`, wired into `.githooks/pre-commit`: bounds,
shadow fields, boot defaults, route declarations, and agreement with
`webUiMock/mock_server.py`, which is the one copy of the limits still outside the table.
It also fails if any consumer starts growing its own key ladder again.

**The table was *derived* from the six it replaces, not retyped**, and that derivation
stays re-runnable as the equivalence proof:

```
python bin/gen_cfg_desc.py --from-rev ':/tools.gh#64' --check
```

(git's search-by-message syntax rather than a hash, because the commit gets cherry-picked
between branches and that rewrites it.) `check_cfg_desc.py` also takes `--golden FILE` to
diff the generated payload against a captured one; no capture is committed, so that arm is
opt-in and not part of the hook.

**Fail-first** — 20 injected defects across four suites, every one caught, sources restored
byte-identical.

**Hardware** — `AT-CFG64` (`bin/at_cfg_roundtrip.py`) on bench board 12F0:

- **42 of the 46 Q4-writable shadow fields**, each proving **exactly one field moved and it
  was the one the descriptor names**. That is the assertion that matters: the shadow write is
  `offsetof`-driven, so naming the wrong field compiles, runs, logs `Q4 applied`, and writes
  a real but different setting. A test asserting only "the value came back" would pass while
  two keys quietly shared a field.
- **12 clamp probes** — 6 fields × both edges — each written one past the bound and required
  to come back *at* the bound, not stored.
- **6 refusals**: the four `ota_*` (they declare `CFG_P_OTA`, not `CFG_P_Q4`), an unknown
  key, and an unknown namespace. The refusal set is **derived from the flags**, so a fifth
  non-Q4 key is tested the day it is added; two of the four used to be listed by hand.
- Reboot persistence, and **a wiped NVS booting to 42 keys at exactly their descriptor
  default** — which is what proves the defaults survived moving out of the boot loader.
  (`lat_*`/`lon_*` legitimately do not read as defaults there: T10's geo sync posts all four
  within seconds of boot.)
- **Unverified: the four `led_*`**, because no endpoint reads them back at all — so their
  clamp cannot be checked over the network. Pre-existing, not a gh#64 regression, now filed
  as [gh#67](https://github.com/pe1mew/greenhouse-Controller/issues/67).

12F0 is the retired bench board, not a unit in service: no rig hardware, so this exercises
the config path alone.

## Upgrading

Nothing to do. No NVS migration, no config change, no client change. Existing stored values
are untouched.

**One behavioural note for non-GUI clients:** `GET /api/config/limits` returns its keys in a
different order. Same keys, same values. Anything parsing it as JSON is unaffected; anything
depending on key order was already relying on something JSON does not guarantee.

## Known limitations

- **`validate` / `path` as table fields are not implemented**, deliberately. gh#64 asked for
  them; the model does not fit. Six keys are writable by two routes with *different*
  policies — `status_expose` is clamped via Q4 and rejected (400) via `/api/web` — so a
  per-key `validate` would have to encode a falsehood. Which routes may write a key *is*
  per-key and is now a flag mask; what a route does with an out-of-range value belongs to
  the route. See the issue for the full argument.
- The two whole-form endpoints still keep their own copy of their keys' bounds. The flags
  make that duplication **declared and checked** rather than invisible; removing it would
  mean both handlers calling a shared validator, which is a separate change.
