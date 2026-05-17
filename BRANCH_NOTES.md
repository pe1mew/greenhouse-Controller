# Branch: `dev/2.0.0-esp-idf` — full ESP-IDF migration

## What this branch is

This branch hosts the **v2.0.0 migration** of the greenhouse-Controller firmware from the **arduino-esp32** framework to **pure ESP-IDF** (PlatformIO `framework = espidf`). The migration plan lives at `~/.claude/plans/do-not-make-changes-toasty-salamander.md` and is summarised in the upcoming `[2.0.0]` section of `changelog.md`.

The last released arduino-esp32 build is **v1.20.3**, immutably preserved by the annotated git tag `v1.20.3-arduino-final`. That tag is the rollback point for the entire migration: as long as it exists, the full pre-migration state can be checked out at any time.

## Why a separate branch

- The 1.20.3 binary running on Units 1 and 2 is **production**. It must not be disturbed by experimental work.
- The migration is **multi-phase, multi-week**. A direct in-place rewrite on `main` would leave the project broken for an indeterminate period.
- A separate branch allows **independent acceptance testing** of each phase (`2.0.0-alpha.0` through `2.0.0-rc.1`) on bench hardware before any production unit is touched.

## Working policy — `main` vs this branch

| Concern | `main` (1.20.x line) | `dev/2.0.0-esp-idf` (this branch) |
|---|---|---|
| Active development | Maintenance only | Active migration |
| New features | No | Phase-by-phase |
| Bug fixes | Yes (1.20.4+) | Cherry-picked from `main` with `-x` |
| Forward merges | None planned | Final fast-forward to `main` at v2.0.0 |
| Force-pushes | Blocked by GitHub branch protection | Blocked by GitHub branch protection |
| Linear history | Required | Required |

### Backport discipline

Critical bugs found on production (Units 1/2 running 1.20.x) get fixed in this order:

1. Fix on `main`. Bump 1.20.x patch version. Release.
2. Cherry-pick the same commit onto `dev/2.0.0-esp-idf` with `git cherry-pick -x <sha>`.
3. The `-x` flag annotates the cherry-pick commit with the original SHA so future bisecting against `main` is straightforward.
4. If the fix conflicts with migration work already on this branch, resolve by hand. **Never drop the fix**; either rewrite it for the new architecture or open a tracking issue for it.

## Phase progression

Each phase produces a tagged pre-release. The full sequence:

| Tag | Phase | Description |
|---|---|---|
| `v2.0.0-alpha.0` | 0 | Branch + scaffolding (this commit) |
| `v2.0.0-alpha.1` | 1 | `framework = espidf` flip + smoke boot |
| `v2.0.0-alpha.2` | 2 | Driver layer (10 drivers, dependency order) |
| `v2.0.0-alpha.3` | 3 | Network stack (`WiFi` → `esp_wifi`) |
| `v2.0.0-alpha.4` | 4 | HTTPS client (`HTTPClient` → `esp_http_client`) — gh#23 payoff |
| `v2.0.0-alpha.5` | 5 | Web server (`ESPAsyncWebServer` → `esp_http_server`) — biggest chunk |
| `v2.0.0-alpha.6` | 6 | Misc Arduino cleanup (NeoPixel, GPIO, millis, headers) |
| `v2.0.0-rc.1` | 7 | 14-day verification soak on bench unit |
| `v2.0.0` | 8 | Merge + release (fast-forward into `main`) |

## How to switch back to the 1.20.3 production line

```bash
git checkout main                             # 1.20.x maintenance line
# or, for the exact 1.20.3 release state:
git checkout v1.20.3-arduino-final
```

## Decision log (chronological)

- **2026-05-17** — branch created from `main` at commit `d8436ad` (1.20.3 release commit). Build system decision: PlatformIO with `framework = espidf` (not native `idf.py`). Backport policy: cherry-pick from main to dev branch with `-x`.

## See also

- Migration plan: see the `2.0.0` section of `changelog.md` (full phase-by-phase narrative)
- gh#23 (mbedTLS root cause) — the technical motivation
- gh#27 (heap-sample timing) — orthogonal alternative, may close once Phase 4 lands
- Tag `v1.20.3-arduino-final` — pre-migration anchor
