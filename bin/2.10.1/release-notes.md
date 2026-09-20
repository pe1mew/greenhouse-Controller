# Release 2.10.1

**Date:** 2026-09-20
**Built on:** 2.10.0
**Fixes:** [gh#80](https://github.com/pe1mew/greenhouse-Controller/issues/80) (the LCD stayed shifted until a restart), [gh#74](https://github.com/pe1mew/greenhouse-Controller/issues/74) (a greyed block dimmed its own reason), [gh#76](https://github.com/pe1mew/greenhouse-Controller/issues/76) (the mock dropped `deadzone_m3`)

**Patch.** Three bug fixes. No config key, no NVS change, no log-encoding change, and nothing in the ventilation path. The web assets change, so push firmware and assets together as always.

## What changed

### The LCD heals a stray mode (gh#80)

**The defect.** After boot, T8's redraw path sends only Display On (`0x0C`) and two Set DDRAM Address commands. On an HD44780-compatible controller **only** Clear, Return Home or an opposite shift clear a display-shift offset, so **one corrupted command byte parks the display for the rest of the run**. `0x1C`, "shift display right", is a single bit from that `0x0C`. On 2026-09-18, 2344's display sat shifted one column right for hours: a blank leftmost column, the last character of each row cut off, the text otherwise intact, on every screen.

**The fix.** Every **10 s** (`LCD_REASSERT_MS`) the preamble becomes a full re-assert of the controller's modes, through the driver's new `lcd_reassert_modes()`:

| Command | Undoes |
|---|---|
| Function Set | a stray bus-width or line-count change (`0x2C`) |
| Display On | display off, cursor, blink (`0x08`, `0x0D`, `0x0E`) — and still absorbs the chip's silent drop of the first transfer after ~2.5 s of bus idle, which is what the preamble was for |
| Entry Mode | a decrementing cursor or auto-shift (`0x04`, `0x07`) |
| Return Home | the display shift itself (`0x18`, `0x1C`) |

So every mode gh#80 lists as "lasting until a restart" now clears within one interval. It writes no DDRAM, so the row writes that follow are unaffected. Between re-asserts the cheap preamble is unchanged, so the cost on a ~1 Hz redraw is four commands and one 1.53 ms wait per 10 s.

**The busy times are now real.** `lcd_home()` did not wait at all, and `lcd_clear()` and `lcd_init()` used `vTaskDelay(2)`, which at a 1 kHz tick guarantees only just over 1 ms against the AiP31068L's 1.53 ms. All three use `LCD_BUSY_MS` = 3 ticks, which guarantees 2 ms. This matters more than before, because Home is now on the redraw path.

### A greyed block no longer dims its own reason (gh#74)

`style.css` carried `.disabled-block .disabled-why { opacity: 1 }`, and both the comment there and CLAUDE.md's GUI rule claimed the reason stays readable at full strength. It never did: **`opacity` applies to an element and its descendants as one group**, so a child cannot be less dim than its parent. The `pointer-events: auto` half did work, which is what made the rule look effective for a year.

- The commissioning card's body is now a `#cm-body` wrapper and `commSetAvailable()` greys that, leaving the heading and `#cm-unavailable` outside it — the shape `#wpos-unfitted-why` above `#wpos-dep` already had.
- The dead rule is deleted, and the CSS comment and CLAUDE.md now state why such a rule cannot exist.

### The mock stores `deadzone_m3` (gh#76)

`webUiMock/mock_server.py` accepted `motor/deadzone_m3` and dropped it, so the GUI's deadzone field read empty against the mock and a change there did not survive a reload. `cfg` gains `deadzone_m3_mm` (20) and `NVS_MAP` the entry. **The mock also answers 400 to an unknown key now**, as the firmware has since gh#53 — silent acceptance is what hid this. The four write-only `led_*` keys stay accepted-but-unstored, as on the unit.

### Bench only

- **`POST /api/diag/lcd {"cmd":N}`** sends one raw LCD instruction under MX1. gh#80's fault is a byte the firmware never sends, so no product API can stage it.
- **`-DLCD_FAILFIRST_GH80`** restores the pre-2.10.1 behaviour, and the source refuses the flag outside a bench build.
- **`bin/at_lcd_gh80.py`** drives the test and says what to look for.

## Size

|  | 2.10.0 | 2.10.1 | delta |
|---|---|---|---|
| app image | 1 395 584 | **1 395 728** | **+144 B** |
| `.flash.text` | 940 182 | 940 302 | +120 B |
| `.flash.rodata` | 310 700 | 310 716 | +16 B |
| `.iram0.text` | 120 535 | 120 535 | 0 |
| `.dram0.bss` | 42 208 | 42 216 | +8 B |
| web assets | 146 842 | 147 547 | +705 B |

The bench image is 1 411 504 B (+624 B, the new diag route).

## Verification

**gh#80, on 2344, with `bin/at_lcd_gh80.py`.** The verdict is the operator's eyes in both arms: the AiP31068L's serial interface is write-only, so the display cannot be read back.

| Build | `0x1C` injected | On the display |
|---|---|---|
| `2.10.1-bench` **with** `-DLCD_FAILFIRST_GH80` | 09:55:36 | **Still shifted after 60 s** — the defect, reproduced on demand and confirmed by the operator |
| `2.10.1-bench` | 10:00:40, 10:01:06, 10:01:31 | **Straightened within 10 s** each time, no restart, confirmed by the operator |

- **The injection reproduces the reported photo exactly:** both rows one column right, a blank leftmost column, the last character cut off, the text intact. That confirms gh#80's hypothesis about the mechanism, which the issue itself left open.
- **The two images differ only in that flag**, and they are byte-different as they must be.

**gh#74, against the mock in a browser.** With the commissioning card unavailable, `#cm-body` computes `opacity 0.4` and `pointer-events: none`, while the reason above it computes `opacity 1` and `pointer-events: auto`, and the card itself is no longer dimmed. Before the change the same check returned 0.4 for the reason.

**gh#76, against the mock over its API.** `POST motor/deadzone_m3 = 37` returns 200, the next `GET /api/config` reports 37, and the GUI's field reads 37 after a reload. An unknown key returns 400; a write-only `led_*` key still returns 200; array keys such as `travel_m3` still write through.

**Not covered by host tests.** `drivers/LCD1602_I2C`'s host suite in `main` predates the AiP31068L migration and **does not compile** (it still calls the PCF8574-era `lcd_backlight_on()`), so the driver change has no host coverage. The operator has discarded that remnant; the rig test above is the evidence for this release.

## Upgrading

- **No partition change, no NVS migration, no configuration change, no log-encoding change.**
- **The web assets change** (gh#74's markup and CSS), so the paired-commit rule applies as always: firmware and assets within 120 s.
- **Nothing in the ventilation, sensor or OTA paths changes.** 2.10.0's verdicts and travel check are untouched.
- **What an operator notices:** a display that comes up shifted now straightens itself within 10 s instead of waiting for a restart, and the commissioning card's reason is readable when the card is greyed.
- **Keep this release's ELF.**
- **Production runs 2.3.1.** Regenerate the release comparison before promoting, and settle gh#81 (the heap floor) first.

## Known limitations

- **The cause of the original corrupted byte is still unknown.** This bounds the damage to 10 s; it does not stop a bit error on the I2C wiring, which is shared with the DS1307 and the PCA9633 backlight under MX1. If shifts become frequent, that is a wiring or noise problem — and the re-assert will hide how often it happens.
- **No counter or log row marks a re-assert.** It runs unconditionally on a timer and cannot tell a healthy display from a healed one, so there is nothing to count. A display fault is still invisible to the SD log.
- **The 1.53 ms Home wait is inside MX1**, once per 10 s. T4's RTC read shares that mutex and waits for it with its own timeout.
- **The LCD host suite does not compile** (see Verification), so `lcd_reassert_modes()` and the busy-time constants are covered by the rig test only.
- **The internal-heap floor** ([gh#81](https://github.com/pe1mew/greenhouse-Controller/issues/81)) is unchanged and still gates any promote to `mainstream`.
