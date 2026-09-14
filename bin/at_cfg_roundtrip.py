#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT-CFG64 — prove every config key still reaches the RIGHT shadow field (gh#64).

gh#64 replaced a 51-arm shadow ladder with one offsetof-driven write. That
removed the last hand-written key list on the write path, and in exchange
created a failure mode the ladder could not have: name the wrong field in the
descriptor and the firmware compiles, runs, logs "Q4 applied", and quietly
writes a real but different setting. No type error, no missing arm, no ERROR.

bin/check_cfg_desc.py catches that statically. This catches it on hardware,
which is the check gh#64 actually asks for, and it does so by the only
argument that is airtight end to end:

    change ONE key, and confirm EXACTLY ONE field in GET /api/config moved,
    and that it is the field the descriptor names.

A wrong offset fails that immediately -- the intended field stays put and some
other one moves. A test that only asserted "the value I wrote came back" would
pass happily while two keys shared a field.

GET /api/config emits the cfg_shadow_t FIELD names (poll_interval_s, travel_s[],
ap_timeout_min), not the NVS key names, so the mapping to the descriptor's
CFG_SH() field is direct and needs no second table -- which is the whole point.
GET /api/web is merged into the same snapshot for the six status_*/log_upload_*
keys it alone displays; that endpoint predates the descriptor and uses its own
names, so WEB_FIELDS below translates them.

All six of those are written through POST /api/config like everything else.
They declare CFG_P_Q4, so Q4 accepts them and the descriptor's offsetof write
applies them -- an earlier version of this test called them unreachable, which
was wrong: they were invisible, not unreachable, and being invisible is exactly
how a key goes unverified for years.

GET /api/ota/config is merged too, for the four ota_* keys. Those do not
declare CFG_P_Q4, so POST /api/config refuses them -- and the refusal set is
DERIVED from that flag rather than hardcoded -- but their BOOT path is the
descriptor like everything else, so reading them is what puts those four
offsets under test at all.

Coverage is reported honestly, and in three parts: written-and-checked,
read-only (boot-load covered by --expect-defaults), and not readable at all.
Only the four led_* fall in the last group, because no endpoint reads them back
-- a pre-existing gap, not a gh#64 one.

Usage:
    python bin/at_cfg_roundtrip.py --host 192.168.20.x [--pin 12345678]
    python bin/at_cfg_roundtrip.py --host ... --reboot-check    # persistence
    python bin/at_cfg_roundtrip.py --host ... --expect-defaults # after an NVS wipe

Exit 0 = every covered key round-trips to its own field. Stdlib only.
"""

import argparse
import http.client
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import check_cfg_desc as D          # reuse the descriptor parser, not a copy

PIN_DEFAULT = "12345678"
SETTLE_S = 4.0          # POST /api/config is async: Q4 -> T4 applies a loop later

# GET /api/web JSON name -> cfg_shadow_t field name.
#
# The one hand-written mapping in this file, and it has to be: unlike
# /api/config, the web endpoint does NOT emit the shadow field names. It
# predates the descriptor. Keep it short and keep it here rather than letting
# it spread -- a second name table is how six of these got out of step before.
WEB_FIELDS = {
    "interval_s": "status_interval_s",
    "enable":     "status_enable",
    "expose":     "status_expose",
    "log_h":      "log_upload_h",
    "log_m":      "log_upload_m",
    "log_rot":    "log_upload_rot",
}

# GET /api/ota/config JSON name -> cfg_shadow_t field.
#
# These four are CFG_KEY_NOT_Q4: POST /api/config refuses them, so the sweep
# below cannot write them. Their BOOT path is the descriptor all the same --
# cfg_load_group() restores them through shadow_off like everything else -- so
# reading them here is what puts those four offsets under test. Without this
# they were the only shadow fields nothing on hardware ever checked.
OTA_FIELDS = {
    "enable":  "ota_enable",
    "check_h": "ota_check_h",
    "win_lo":  "ota_win_lo",
    "win_hi":  "ota_win_hi",
}


# ----------------------------------------------------------------- transport --

class Unit(object):
    def __init__(self, host, pin):
        self.host = host
        self.pin = pin
        self.cookie = None
        self._login(pin)

    def _req(self, method, path, body=None, _retry=True):
        """One request, re-authenticating once on a 401.

        The admin session expires after session_timeout minutes (default 5) and
        this sweep runs longer than that, so a mid-run 401 is expected rather
        than exceptional -- it stopped the first full run dead. Same reason the
        OTA push client re-logins (OTAimplementation.md 8.7)."""
        sc, body_out = self._raw(method, path, body)
        if sc == 401 and _retry and path != "/api/login":
            self.cookie = None
            self._login(self.pin)
            return self._raw(method, path, body)
        return sc, body_out

    def _raw(self, method, path, body=None):
        c = http.client.HTTPConnection(self.host, 80, timeout=10)
        hdr = {"Content-Type": "application/json"}
        if self.cookie:
            hdr["Cookie"] = self.cookie
        c.request(method, path, json.dumps(body).encode() if body is not None else None, hdr)
        r = c.getresponse()
        raw = r.read()
        sc = r.getcode()
        sk = r.getheader("Set-Cookie")
        if sk:
            self.cookie = sk.split(";")[0]
        c.close()
        try:
            return sc, json.loads(raw.decode("utf-8"))
        except Exception:                                      # noqa: BLE001
            return sc, raw.decode("utf-8", "replace")

    def _login(self, pin):
        sc, _ = self._req("POST", "/api/login", {"role": "admin", "pin": pin})
        if sc != 200 or not self.cookie:
            sys.exit("login failed (HTTP %s) -- wrong PIN, or the unit is not up" % sc)

    def config(self):
        """One merged snapshot of every shadow field any endpoint will show.

        The six status_*/log_upload_* keys are NOT on GET /api/config, which is
        why the first version of this test called them unreachable. They are
        perfectly writable through POST /api/config -- they are CFG_KEY_SHADOW,
        so Q4 accepts them and the descriptor's offsetof write applies them --
        they were merely invisible. GET /api/web shows them.

        Merging both into one snapshot makes the central assertion stronger
        rather than just wider: changing a climate key must now also leave the
        web block untouched, and vice versa. A descriptor offset that strayed
        across that boundary had nothing to trip over before."""
        sc, j = self._req("GET", "/api/config")
        if sc != 200:
            sys.exit("GET /api/config returned HTTP %s" % sc)
        out = flatten(j)
        sc, w = self._req("GET", "/api/web")
        if sc != 200:
            sys.exit("GET /api/web returned HTTP %s" % sc)
        for jname, field in WEB_FIELDS.items():
            if isinstance(w.get(jname), int) and not isinstance(w.get(jname), bool):
                out[field] = w[jname]
        sc, o = self._req("GET", "/api/ota/config")
        if sc != 200:
            sys.exit("GET /api/ota/config returned HTTP %s" % sc)
        for jname, field in OTA_FIELDS.items():
            if isinstance(o.get(jname), int) and not isinstance(o.get(jname), bool):
                out[field] = o[jname]
        return out

    def post_cfg(self, ns, key, value):
        return self._req("POST", "/api/config", {"ns": ns, "key": key, "value": value})[0]

    def status(self):
        """The identity block. /api/status nests these under "system" -- reading
        them flat yields None, which silently defeated the production guard
        below. A safety check that cannot see what it is guarding must fail
        closed, so this raises rather than shrugging."""
        j = self._req("GET", "/api/status")[1]
        if not isinstance(j, dict) or "system" not in j:
            sys.exit("GET /api/status has no `system` block -- refusing to run "
                     "without knowing which unit this is")
        return j["system"]

    def settle(self, field, want, secs=SETTLE_S):
        """Block until `field` reads `want`, or give up.

        POST /api/config is ASYNCHRONOUS: it enqueues on Q4 and T4 applies it a
        loop later. Every write here needs this -- including the restore at the
        end of each key, which is the one the first version of this script
        skipped. The restores then landed during the NEXT key's measurement and
        showed up as a second field moving, which is indistinguishable from the
        wrong-offsetof bug this test exists to find. 12 of 36 keys failed that
        way and every one of them was a false alarm."""
        # Poll only the endpoint that owns this field. A full three-endpoint
        # snapshot per poll made the sweep outrun the 5-minute session timeout.
        if field in WEB_FIELDS.values():
            path, names = "/api/web", WEB_FIELDS
        elif field in OTA_FIELDS.values():
            path, names = "/api/ota/config", OTA_FIELDS
        else:
            path, names = "/api/config", None

        deadline = time.time() + secs
        while time.time() < deadline:
            time.sleep(0.35)
            sc, j = self._req("GET", path)
            if sc != 200 or not isinstance(j, dict):
                continue
            got = flatten(j).get(field) if names is None else \
                next((j.get(n) for n, f in names.items() if f == field), None)
            if got == want:
                return True
        return False


def flatten(cfg):
    """{field_name: int} with arrays expanded to travel_s[0] .. [2].

    Matches how the descriptor writes them, so the two can be compared without
    a translation table in between."""
    out = {}
    for k, v in cfg.items():
        if isinstance(v, list):
            for i, item in enumerate(v):
                if isinstance(item, int):
                    out["%s[%d]" % (k, i)] = item
        elif isinstance(v, int) and not isinstance(v, bool):
            out[k] = v
    return out


# --------------------------------------------------------------- the checks --

def pick_new(row, cur):
    """A different, in-range value. Deliberately not a fixed constant: writing
    the same number to every key would let a shared-field bug pass."""
    lo, hi = row["min"], row["max"]
    for cand in (cur + 1, cur - 1, lo, hi, (lo + hi) // 2):
        if lo <= cand <= hi and cand != cur:
            return cand
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--pin", default=PIN_DEFAULT)
    ap.add_argument("--reboot-check", action="store_true",
                    help="after the sweep, reboot and confirm values survive "
                         "(exercises the nvs_load_* boot path, the seventh table)")
    ap.add_argument("--expect-defaults", action="store_true",
                    help="assert every reachable key equals its descriptor "
                         "default and exit. Run this on a unit whose NVS has "
                         "just been wiped: it is what proves the factory "
                         "defaults survived being moved out of nvs_load_*() "
                         "into the table (gh#64's seventh list).")
    ap.add_argument("--reset-port", metavar="COMx",
                    help="serial port used to reset the board for --reboot-check. "
                         "There is NO /api/reboot route, so the reset has to come "
                         "from outside; without this you are asked to power-cycle.")
    args = ap.parse_args()

    macros = D.macro_values()
    rows = D.parse_desc(macros, D.key_constants())
    if D._FATAL or D._ERRORS:
        sys.exit("descriptor does not parse; run bin/check_cfg_desc.py first")
    by_field = {r["shadow"]: r for r in rows if r["shadow"]}

    u = Unit(args.host, args.pin)
    st = u.status()
    uid = str(st.get("unit_id", "?"))
    print("unit %s  fw %s  assets %s" % (uid, st.get("fw_ver"), st.get("asset_version")))

    # This writes every config key it can reach. That is fine on a bench board
    # and on the open-loop dev rig; it is not fine on the greenhouse.
    if "5C88" in uid.upper():
        sys.exit("REFUSING: %s is the production unit. This test writes every "
                 "config key it can reach." % uid)

    if args.expect_defaults:
        # T10's do_geo_sync() posts all four coordinate keys to Q4 within
        # seconds of boot, so on a wiped unit they hold the GEOLOCATED position,
        # not the compiled default. That is the feature working (CLAUDE.md:
        # "lat/lon self-heal via T10 geolocation"), and it is also incidental
        # proof that the descriptor's write path serves a non-GUI Q4 producer.
        # Excluded by name and with a reason -- never silently.
        GEO_SELFHEAL = ("lat_deg", "lat_frac", "lon_deg", "lon_frac")
        defs = D.default_values()
        live = u.config()
        bad, okn, skipped = [], 0, []
        for r in rows:
            f = r["shadow"]
            if r["key"] in GEO_SELFHEAL:
                continue
            if f is None or f not in live:
                if f is not None:
                    skipped.append(r["key"])
                continue
            tok = r["def_tok"]
            want = int(tok, 0) if re.fullmatch(r"-?(?:0[xX][0-9a-fA-F]+|\d+)", tok) \
                else defs.get(tok)
            if want is None:
                bad.append("%s: default %s does not resolve" % (r["key"], tok))
            elif live[f] != want:
                bad.append("%s: is %s, descriptor default is %s (%s)"
                           % (r["key"], live[f], want, tok))
            else:
                okn += 1
                print("  ok   %-16s %-7s (%s)" % (r["key"], want, tok))
        print("\n%d keys at their descriptor default" % okn)
        print("excluded, self-healed by T10 geolocation at boot: %s"
              % ", ".join(GEO_SELFHEAL))
        if skipped:
            print("not readable over the network: %s" % ", ".join(sorted(skipped)))
        if bad:
            print("\nMISMATCHES (%d):" % len(bad))
            for b in bad:
                print("  - %s" % b)
            return 1
        print("DEFAULTS OK -- a wiped unit boots to exactly what the table says")
        return 0

    live = u.config()
    # Everything below writes config. Keep the unit's real settings so they can
    # be put back at the end -- the reboot-check in particular used to leave its
    # marker values in place, which on the dev rig would silently have changed
    # cr_priority and the wind exclusion zone.
    original = dict(live)
    # Only CFG_KEY_SHADOW keys are writable through Q4. The four ota_* are
    # CFG_KEY_NOT_Q4 -- POST /api/config refuses them with 400, and that refusal
    # is itself asserted further down. They are still READ here, so a stray
    # write landing in one of their fields would be caught; their own boot-load
    # correctness is what --expect-defaults covers.
    writable = {f: r for f, r in by_field.items() if "CFG_P_Q4" in r["paths"]}
    covered = sorted(f for f in writable if f in live)
    readonly = sorted(f for f in by_field if f not in writable and f in live)
    missing = sorted(f for f in by_field if f not in live)

    print("\n%d shadow fields; %d writable via Q4 and readable, %d read-only, %d not readable"
          % (len(by_field), len(covered), len(readonly), len(missing)))
    if readonly:
        print("  read-only here, another route owns the write: %s"
              % ", ".join(by_field[f]["key"] for f in readonly))
    if missing:
        print("  not readable at all: %s"
              % ", ".join(by_field[f]["key"] for f in missing))

    fails, done = [], []
    print("\n--- one key at a time: exactly one field must move ---")
    for field in covered:
        row = by_field[field]
        before = u.config()
        cur = before[field]
        new = pick_new(row, cur)
        if new is None:
            fails.append("%s: no alternative value inside [%d, %d]"
                         % (row["key"], row["min"], row["max"]))
            continue

        sc = u.post_cfg(row["ns"].replace("NVS_NS_", "").lower(), row["key"], new)
        if sc != 200:
            fails.append("%s: POST returned HTTP %s" % (row["key"], sc))
            continue

        u.settle(field, new)
        after = u.config()

        moved = sorted(k for k in after if before.get(k) != after.get(k))
        if moved == [field] and after[field] == new:
            print("  ok   %-16s -> %-6s (%s)" % (row["key"], new, field))
            done.append(row["key"])
        elif not moved:
            fails.append("%s: nothing changed (wanted %s=%s)" % (row["key"], field, new))
        elif moved == [field]:
            fails.append("%s: %s became %s, expected %s"
                         % (row["key"], field, after[field], new))
        else:
            # The signature of a wrong offsetof: some OTHER setting moved.
            fails.append("%s: expected only %s to change, but these did: %s"
                         % (row["key"], field, ", ".join(
                             "%s %s->%s" % (m, before.get(m), after.get(m)) for m in moved)))

        # Wait for the restore to LAND before the next key is measured. A bare
        # sleep here is what produced 12 false failures on the first run.
        u.post_cfg(row["ns"].replace("NVS_NS_", "").lower(), row["key"], cur)
        if not u.settle(field, cur):
            fails.append("%s: did not restore to %s within %.0fs"
                         % (row["key"], cur, SETTLE_S))

    # ---- bounds are enforced server-side, not just published ----------------
    print("\n--- out-of-range writes must clamp, not store ---")
    for field in covered[:6]:
        row = by_field[field]
        keep = u.config()[field]
        for probe, want, edge in ((row["min"] - 1, row["min"], "min"),
                                  (row["max"] + 1, row["max"], "max")):
            u.post_cfg(row["ns"].replace("NVS_NS_", "").lower(), row["key"], probe)
            u.settle(field, want)
            got = u.config()[field]
            if got == want:
                print("  ok   %-16s %-7s -> %s" % (row["key"], probe, got))
            else:
                fails.append("%s: wrote %s (past %s), expected clamp to %s, got %s"
                             % (row["key"], probe, edge, want, got))
        u.post_cfg(row["ns"].replace("NVS_NS_", "").lower(), row["key"], keep)
        u.settle(field, keep)

    # ---- keys another route owns must still be refused here -----------------
    # Derived from the table, not hardcoded: every key that does NOT declare
    # CFG_P_Q4 must be refused on this route. Previously two of the four were
    # listed by hand, so adding a fifth non-Q4 key would have gone untested.
    print("\n--- keys Q4 must refuse ---")
    refuse = [(r["ns"].replace("NVS_NS_", "").lower(), r["key"],
               "not CFG_P_Q4; %s" % (" | ".join(r["paths"]) or "no write route"))
              for r in rows if "CFG_P_Q4" not in r["paths"]]
    for ns, key, why in refuse + [("climate", "not_a_key", "unknown key"),
                                  ("nosuchns", "t_max_day", "unknown namespace")]:
        sc = u.post_cfg(ns, key, 1)
        if sc == 400:
            print("  ok   %-12s %-14s 400 (%s)" % (ns, key, why))
        else:
            fails.append("%s/%s: expected HTTP 400 (%s), got %s" % (ns, key, why, sc))

    # ---- persistence: the nvs_load_* path gh#64 did not count ---------------
    if args.reboot_check:
        print("\n--- reboot: do the values survive? (the seventh table) ---")
        mark = {}
        for field in covered[:8]:
            row = by_field[field]
            v = pick_new(row, u.config()[field])
            u.post_cfg(row["ns"].replace("NVS_NS_", "").lower(), row["key"], v)
            u.settle(field, v)
            mark[field] = v
        # This firmware registers no /api/reboot route -- the first version of
        # this check POSTed to one anyway, got a 404, waited, and then
        # "confirmed" persistence across a reboot that never happened. The
        # reset has to come from outside the HTTP surface.
        if args.reset_port:
            import glob as _glob
            et = _glob.glob(os.path.join(os.path.expanduser("~"), ".platformio",
                                         "packages", "tool-esptoolpy", "esptool.py"))
            if not et:
                sys.exit("esptool.py not found; reset the board yourself and re-run")
            import subprocess
            print("  resetting %s over serial..." % args.reset_port)
            subprocess.call([sys.executable, et[0], "--port", args.reset_port,
                             "--after", "hard_reset", "--no-stub", "read_mac"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            input("  no --reset-port given. Power-cycle the board now, then press Enter: ")
        print("  waiting for the unit to come back...")
        time.sleep(12.0)
        for _ in range(30):
            try:
                u2 = Unit(args.host, args.pin)
                break
            except SystemExit:
                time.sleep(2.0)
        else:
            sys.exit("unit did not come back after reboot")
        u = u2                      # the old session died with the reboot
        back = u2.config()
        for field, want in mark.items():
            if back.get(field) == want:
                print("  ok   %-22s survived as %s" % (field, want))
            else:
                fails.append("%s: was %s before reboot, is %s after -- nvs_load_* "
                             "does not restore it" % (field, want, back.get(field)))

    # Leave the unit as it was found. A test that quietly rewrites settings is
    # only safe on a board nobody relies on, and this one should be runnable on
    # the dev rig too.
    drifted = sorted(k for k in original
                     if k in by_field and u.config().get(k) != original[k])
    if drifted:
        print("\n--- putting the unit back as it was ---")
        for field in drifted:
            row = by_field[field]
            u.post_cfg(row["ns"].replace("NVS_NS_", "").lower(),
                       row["key"], original[field])
            if u.settle(field, original[field]):
                print("  ok   %-22s restored to %s" % (field, original[field]))
            else:
                fails.append("%s: could NOT be restored to %s -- the unit is "
                             "left changed" % (field, original[field]))

    print("\n" + "=" * 70)
    print("verified %d of %d Q4-writable keys round-trip to their own field"
          % (len(done), len(writable)))
    if missing:
        print("unverified over the network: %s"
              % ", ".join(by_field[f]["key"] for f in missing))
    if readonly:
        print("read-only on this path, boot-load verified by --expect-defaults: %s"
              % ", ".join(by_field[f]["key"] for f in readonly))
    if fails:
        print("\nFAILURES (%d):" % len(fails))
        for f in fails:
            print("  - %s" % f)
        return 1
    print("AT-CFG64 PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
