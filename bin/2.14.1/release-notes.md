# Release 2.14.1

**Date:** 2026-09-27
**Built on:** 2.14.0
**Fixes:** [gh#87](https://github.com/pe1mew/greenhouse-Controller/issues/87): dead code found in a linker-map sweep

**Patch, with no behaviour change.** Five functions are removed. None of them was in the built image,
because the linker discarded them: nothing called them. Two pieces of live code that existed only to
serve them go too, and neither had any observable effect:
- `rotate_sd_file()` copied the closed file's name into a buffer nothing read;
- T9's loop checked a flag nothing ever set.

The image is 256 bytes smaller than 2.14.0's.

> **Published to the ROTA soak channel on the operator's instruction (2026-09-27), without a bench soak
> first.** The change only removes code, and the soak unit (2344) receives it as the release build.
> See *Verification*.

## What changed

- **`pin_auth_reset_admin()` removed.** It was written for a recovery design that was never built:
  TSDS §5.4's key combination at power-on with a hardware jumper fitted. Nothing ever called it.
  - The recovery that exists is **the IO0 BOOT button, stage 1** (hold 5–10 s, release). Both PINs go
    back to their defaults, no PIN is needed, and it takes physical access (FR-AC08/FR-AC09; beheerder
    manual §9 and §18).
  - The source comment and TSDS §5.4 now say so.
- **`event_logger_last_rotated()`, `event_logger_newest_closed()` and `event_logger_force_rotate()`
  removed**, together with the state only they used. T14 uploads through `upload_pending()` →
  `event_logger_next_pending()`, and the comments in `status_post.{h,cpp}` now describe that.
  - `LOG_SYSTEM value_a=6`, the force-rotate marker, is no longer emitted. The code stays reserved, and
    `log/logparser.py` still decodes it in old logs.
- **`web_any_active_session()` removed.** It was superseded by `web_any_active_session_except()`
  (gh#41), which the ROTA quiet gate calls.
- **Version 2.14.1**, so that no rebuild can overwrite the published `bin/2.14.0/` with different bytes.

## Verification

| Check | Result |
|---|---|
| Release, bench and Modbus-probe images build | **yes**, no errors and no "unused" warnings: nothing the removal left behind is now unused |
| The five functions in the linker maps | **gone**: neither the release nor the bench map names any of them (2.14.0's release map listed them as discarded) |
| `python bin/check_cfg_desc.py` | **PASS**: 51 keys, 40 published |
| Release image | 1 410 832 B, sha256 `b72eda80f2ad22f2…`, version string `2.14.1`, no bench suffix; assets stamped `2.14.1` |
| Release on GitHub | **verified**: not a draft or prerelease, tag `v2.14.1` on `9969aed`, both files downloaded back and hash-identical, manifest seq 58 |
| Channel | **verified from the device**: the first forced check (09:44, 13 min after publishing) was offered 2.14.1 |
| **ROTA pull on 2344** | **PASS, first attempt**: check 09:44:02, download and verify OK 09:44:12, apply committed 09:44:24. `fw_ver` **and** `asset_version` both read **2.14.1** at 09:45:07. All 9 rig settings survived, and M3 came up in Lineair (reason `setting`, gate `ok`) 73 s after the boot |
| Soak | **none**, by the operator's decision; the soak unit now runs this release build |

## Upgrading

- Nothing to do: no setting, key or payload changed.
- A log parser meets `LOG_SYSTEM value_a=6` only in logs written before 2.14.1.
