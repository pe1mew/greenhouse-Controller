#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for gh#106: an active operator's session must outlive its first timeout.

THE DEFECT THIS EXISTS FOR
--------------------------
The cookie carried `Max-Age` = `session_timeout_min`, set only at login. The server
slides its own expiry on every request and never re-sends the cookie, so a browser
dropped it at login + the timeout, whatever the operator was doing, and the next
request came back 401.

WHY NOT bin/at_wp_ramp.py's Unit
--------------------------------
It keeps the token in a string and ignores the cookie's attributes, so it could
never see this. Here a real cookie jar does what a browser does with them.

WHAT IT DOES
------------
  active  log in, and call /api/whoami every POLL_S until the timeout + MARGIN_S has
          passed. Every call must answer the role. Before the fix the jar drops the
          cookie at the timeout and the next call is a 401.
  idle    stop calling for the timeout + MARGIN_S. The session must now be gone on
          the server: a fix that kept sessions alive for ever would pass `active`.
  logout  log in, log out: the jar must hold no session cookie.

M3 does not move and no setting changes. Takes about twice the timeout, 12 minutes
at the default 5 minutes.

USAGE
-----
    python bin/at_session_cookie.py --host 192.168.20.160
    python bin/at_session_cookie.py --host 192.168.20.160 active

Exit 0 = pass, 1 = fail. Any build. Refuses to run on 5C88.
"""

import argparse
import http.cookiejar
import json
import sys
import time
import urllib.error
import urllib.request

DEFAULT_PIN = "12345678"
POLL_S = 20.0
MARGIN_S = 45.0


def say(s):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), s), flush=True)


class Browser:
    """urllib with a cookie jar: expiry and deletion are the jar's business."""

    def __init__(self, host):
        self.base = "http://%s" % host
        self.jar = http.cookiejar.CookieJar()
        self.op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))

    def call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method,
                                     headers={"Content-Type": "application/json"})
        try:
            with self.op.open(req, timeout=10) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")

    def has_cookie(self):
        self.jar.clear_expired_cookies()
        return any(c.name == "session" for c in self.jar)

    def login(self, pin):
        sc, out = self.call("POST", "/api/login", {"role": "admin", "pin": pin})
        if sc != 200 or not self.has_cookie():
            sys.exit("login failed (HTTP %s): %s" % (sc, out[:100]))

    def logout(self):
        self.call("POST", "/api/logout", {})


def timeout_s(host, pin):
    b = Browser(host)
    b.login(pin)
    sc, out = b.call("GET", "/api/config")
    b.logout()
    return int(json.loads(out)["session_timeout_min"]) * 60


def active(host, pin, limit_s):
    print("--- active, %d s" % limit_s, flush=True)
    b = Browser(host)
    b.login(pin)
    t0 = time.time()
    n = 0
    try:
        while time.time() - t0 < limit_s:
            time.sleep(POLL_S)
            had = b.has_cookie()
            sc, out = b.call("GET", "/api/whoami")
            n += 1
            if sc != 200 or "admin" not in out:
                say("FAIL: call %d, %.0f s after the login: HTTP %s, cookie in the jar: %s"
                    % (n, time.time() - t0, sc, had))
                return False, b
        say("PASS: %d calls over %.0f s, every one admin" % (n, time.time() - t0))
        return True, b
    except Exception:
        b.logout()
        raise


def idle(b, limit_s):
    print("--- idle, %d s" % limit_s, flush=True)
    time.sleep(limit_s)
    sc, out = b.call("GET", "/api/whoami")
    ok = sc == 401
    say("%s: after %d s without a call: HTTP %s" % ("PASS" if ok else "FAIL", limit_s, sc))
    return ok


def logout(host, pin):
    print("--- logout", flush=True)
    b = Browser(host)
    b.login(pin)
    b.logout()
    ok = not b.has_cookie()
    say("%s: the cookie is %s after the logout" % ("PASS" if ok else "FAIL", "gone" if ok else "still there"))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("cases", nargs="*", help="active, or all three when left out")
    a = ap.parse_args()

    with urllib.request.urlopen("http://%s/api/status" % a.host, timeout=8) as r:
        y = json.load(r).get("system") or {}
    if "5C88" in str(y.get("unit_id", "")).upper():
        print("REFUSED: this is the production unit")
        return 1
    t = timeout_s(a.host, a.pin)
    print("%s fw %s, session timeout %d s" % (y.get("unit_id"), y.get("fw_ver"), t), flush=True)

    ok, b = active(a.host, a.pin, t + MARGIN_S)
    if a.cases == ["active"] or not ok:
        b.logout()
    else:
        ok = idle(b, int(t + MARGIN_S)) and ok
        ok = logout(a.host, a.pin) and ok
    print("=== %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
