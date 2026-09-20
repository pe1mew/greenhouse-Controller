#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for gh#80: a stray LCD mode must not survive until the next restart.

THE DEFECT. On 2026-09-18 the display came up shifted one column to the right
and stayed that way for hours. After boot T8's redraw path sends only Display On
(0x0C) and two Set DDRAM Address commands, and on an HD44780-compatible
controller only Clear, Home or an opposite shift clear a display shift. So ONE
corrupted command byte parks the display for the rest of the run, and 0x1C
("shift display right") is a single bit from the 0x0C sent before every redraw.

THE FIX (2.10.1). T8 turns the preamble into a full re-assert of the modes
every LCD_REASSERT_MS (10 s): Function Set, Display On, Entry Mode, Return Home.
Home clears the shift, so the display heals within one interval.

THIS TEST. `POST /api/diag/lcd {"cmd":28}` sends 0x1C the way a corrupted byte
would (bench builds only -- the fault cannot be staged through any product API).
The test then reports the time; WHAT HAPPENS IS ON THE DISPLAY, so a person has
to watch it. It cannot be read back: the AiP31068L's serial interface is
write-only.

  - On `-DLCD_FAILFIRST_GH80` (the behaviour before 2.10.1) the display stays
    shifted until the unit restarts. Watch for 60 s: still shifted = FAIL-FIRST
    reproduced.
  - On the normal build it straightens within 10 s, at the next re-assert.

USAGE
-----
    python bin/at_lcd_gh80.py --host 192.168.20.160            # inject, watch 60 s
    python bin/at_lcd_gh80.py --host 192.168.20.160 --cmd 4    # entry mode: decrement
    python bin/at_lcd_gh80.py --host 192.168.20.160 --heal-only

`--cmd` takes any of the modes one bit flip from 0x0C can latch: 28 (0x1C, shift
right), 4 (0x04, cursor decrement -- rows written backwards), 44 (0x2C, a
Function Set with the bus-width bit cleared). All three are cleared by the same
re-assert; only the first matches the reported photo.

Exit 0 = the injection was accepted and the watch ran (the VERDICT is the
operator's), 2 = could not run. Stdlib only, ASCII output. Refuses 5C88.
"""

import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from at_wp_ramp import Unit                                   # noqa: E402
from at_wp_teach_standby import public_status                 # noqa: E402

DEFAULT_PIN = "12345678"
WATCH_S = 60
NAMES = {28: "0x1C shift display right", 4: "0x04 entry mode, cursor decrement",
         44: "0x2C function set, 4-bit bus", 8: "0x08 display off"}


def say(msg):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), msg))
    sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--cmd", type=int, default=28, help="instruction byte to inject (default 28 = 0x1C)")
    ap.add_argument("--watch-s", type=int, default=WATCH_S)
    ap.add_argument("--heal-only", action="store_true",
                    help="send the re-assert's effect without injecting: just watch")
    a = ap.parse_args()

    st = public_status(a.host) or {}
    sysb = st.get("system") or {}
    uid = str(sysb.get("unit_id", "?"))
    if "5C88" in uid.upper():
        sys.exit("REFUSING: %s is the production unit." % uid)
    fw = str(sysb.get("fw_ver", "?"))
    print("AT gh#80 -- unit %s fw %s" % (uid, fw))
    if not fw.endswith("-bench"):
        print("  the injection route is bench-only; this build will answer 404")

    u = Unit(a.host, a.pin)
    try:
        if not a.heal_only:
            sc, out = u._req("POST", "/api/diag/lcd", {"cmd": a.cmd})
            if sc != 200 or not (isinstance(out, dict) and out.get("ok")):
                print("injection refused: HTTP %s %s" % (sc, out))
                return 2
            say("injected %d (%s)" % (a.cmd, NAMES.get(a.cmd, "?")))
            print("\n  LOOK AT THE DISPLAY NOW. Expect it shifted one column right:")
            print("    row 0 starts with a blank, and the last character of each row is cut off.\n")
        t0 = time.time()
        for left in range(a.watch_s, 0, -10):
            time.sleep(min(10, left))
            say("%2.0f s since the injection" % (time.time() - t0))
        print("""
  VERDICT, from the display:
    still shifted after %d s  -> the defect (expected on -DLCD_FAILFIRST_GH80)
    straightened within ~10 s -> the fix works (expected on the normal build)
""" % a.watch_s)
    finally:
        u.logout()
    return 0


if __name__ == "__main__":
    sys.exit(main())
