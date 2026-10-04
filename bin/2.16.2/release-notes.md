# Release 2.16.2

**Date:** 2026-10-04
**Built on:** 2.16.1
**Changes:** an SD unmount during a write no longer panics the unit
([gh#90](https://github.com/pe1mew/greenhouse-Controller/issues/90)), and the coredump's Erase button
says why it is greyed

**Patch. Bug fixes only: no setting, key or payload change.** In the GUI only the coredump row of the
Log tab and the look of a disabled button change.

> **Not soaked, not published.** The fix is in the SD driver's locking and the coredump row of the GUI.
> Every acceptance row below ran on hardware, with a fail-first for the lock.

## What changed

- **One lock around every SD operation** (`drivers/sdCard`, gh#90).
  - **The bug:** `POST /api/sd/unmount` ran in the HTTP task and unregistered FAT at once. A T9 write
    already inside FAT held FAT's per-volume lock, the unmount closed that lock, and newlib's
    `_lock_close()` asserted: a panic and a reboot. Every build with the route has it, 2.3.1 included.
    The window is one write, a few ms, and it is most likely just after a boot, when T9 writes
    most.
  - **Now:** a recursive mutex covers every driver function that touches FAT, mount and unmount
    included, and each checks the mount under it. That covers:
    - T9's writes, rotation and retention;
    - the web routes (status, listing, download, mount, unmount);
    - T15's upload reads;
    - the status snapshot's free-space query.
  - **The effect:** an unmount waits for the operation in flight, and the next write returns
    `STORAGE_ERR_NO_CARD`.
  - **No early return can leave the lock held:** each public function wraps a private `*_locked()`
    body. The lock is a function-local static, created on first use, so there is no init order.
  - **A foreach holds the lock across its callbacks.** None of them calls back into the driver, and the
    lock is recursive in case one ever does.
- **T9 takes a `STORAGE_ERR_NO_CARD` quietly while the gh#61 latch says the release was deliberate.**
  So an unmount the operator asked for no longer writes a `LOG_SYSTEM -1` row ("SD write failure")
  after the next mount. Any other write failure is logged as before.
- **The coredump's Erase button** (reported by the operator, 2026-10-04).
  - **Before:** Erase stayed greyed until a Download in the same session, which is intended, but
    nothing said so. No button in the GUI had a disabled style, so it looked pressable and did
    nothing. Straight after a Download the server then refused it for 10 s (one `/api/coredump`
    action per 10 s, shared by both buttons), and that error was the first explanation the operator
    saw.
  - **Now:** a line above the row says why Erase is greyed. After a Download it counts the server's
    10 s down, and Erase enables when the count ends. If the server refuses an Erase all the same,
    the wait starts again.
  - **Every disabled button is dimmed** (`button:disabled`: opacity 0.45, a not-allowed cursor). A
    disabled button inside a greyed block is not dimmed a second time (gh#74). SD **Mount** and
    **Unmount** now also look disabled when they are, and the SD status line beside them gives the
    reason.
- **Bench builds only, none of it in this image:**
  - `GET/POST /api/diag/sd`: a writer that keeps FAT busy with back-to-back appends through the
    driver, and its state;
  - `SD_FAILFIRST_NOLOCK`: the lock compiled out (it refuses to build without `MODBUS_BENCH`);
  - `bin/at_sd_unmount.py`: stages `race` and `idle`.
- **Version 2.16.2.**

## Verification

| Check | Result |
|---|---|
| Release image | 1 432 528 B, sha256 `a86f60fb30e1f8ec…`, version `2.16.2`, no bench suffix or routes; RAM 20.1 % (65 860 B), flash 68.3 %; assets stamped `2.16.2`, STORE |
| The image is the one tested | **byte-identical** to the image the `idle` stage ran on (`a86f60fb…`); `build_release.ps1`'s zip (`a28c13c2…`) has the tested zip's content, file for file |
| Host tests, `drivers/sdCard` | **17 of 17** (UT-SD-015..017 new: the lock is balanced on every success path and every error path, and a foreach callback runs under it). Three mutations each fail all three new tests: a wrapper that leaks the lock, foreach without it, a double lock |
| **`race`, fail-first** (`SD_FAILFIRST_NOLOCK`, the lock compiled out) | **FAILS in round 1, in both runs.** The unit rebooted during the unmount request, and stored a new coredump. Decoded: the `_lock_close()` assert in `esp_vfs_fat_unregister_path()`, under `unmount_locked()` and `event_logger_sd_unmount()`, in the HTTP task: the gh#90 path |
| **`race`, the fix** (2.16.2-bench; five rounds, a bench writer appending 16 KB at a time, the unmount mid-write) | **PASS, 16 of 16.** Each unmount answered `ok` in 0.30–0.41 s, the wait for the write in flight. No reboot, and no new coredump. The writer stopped on `no_card`, and the card mounted again |
| `idle` (an unmount and a mount on a settled unit) | **PASS** on 2.16.2-bench and on the 2.16.2 release |
| SD logging after the tests | Went on: 533 rows logged during the tests, none of them an SD-write-failure (`LOG_SYSTEM -1`) row |
| The GUI, on the mock (`webUiMock`, test PIN) | Before a Download: Erase greyed, with the reason above it. After a Download: the countdown runs from 10 s, then Erase enables, and the erase works. Disabled buttons dim |
| 2344 afterwards | 2.16.2 release (`fw_ver` and `asset_version` read `2.16.2`), SD mounted. It holds the fail-first build's coredump, for trying the Erase button on the unit. At 536 s of uptime: 66 KB heap free, 29 KB largest block |

## Upgrading

- **Nothing to do.** No setting, key or NVS record changed.
- **A rollback to 2.16.1 or earlier** brings both bugs back, and nothing else changes.

## Known limitations

- **Not soaked, not published.**
- **T9's quiet path is not forced by any test.** A T9 write that has passed its own mount check when
  the unmount lands now gets `STORAGE_ERR_NO_CARD` and logs nothing. That follows from the code
  (`event_logger_sd_unmount()` sets the latch before it unmounts), but the window is too narrow to
  hit on purpose. The `race` stage drives the bench writer, not T9.
- **The GUI was checked on the mock, not on the unit.** The browser test never types the admin PIN
  into the unit's own page.
