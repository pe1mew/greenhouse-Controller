#!/usr/bin/env python3
"""
ROTA device simulator — server acceptance suite (implementation-plan Phase 2).

Exercises the FOTA server's wire contract v1.1 (rota_tds.md §4) the way the
firmware T16 client will: builds the X-OTA-Auth HMAC header, pins the server
certificate by SHA-256 fingerprint (faithful to esp_http_client cert_pem), and
asserts the contract's happy path and every negative case. No ESP32 required.

Stdlib only (matches bin/ota_push.py). Run against a deployed instance or a
local `php -S` (see greenhouse-Controller-FOTA-server/examples/README.md).

Usage:
    python bin/rota_sim.py --base-url https://ota.rfsee.net \\
        --cert /path/to/ota_server.pem \\
        --id <full-mac-12hex> --secret-file <path>        # or --secret <hex>
    # optional resolution check (needs a second, pinned unit configured):
        --pinned-id <mac> --pinned-secret <hex> --expect-pinned <version>

Exit code 0 = all cases passed, 1 = one or more failed.
"""

import argparse
import hashlib
import hmac
import http.client
import json
import os
import ssl
import sys
import time
from urllib.parse import urlsplit


# ── pinned HTTPS ────────────────────────────────────────────────────────────
def fingerprint_from_pem(path):
    der = ssl.PEM_cert_to_DER_cert(open(path).read())
    return hashlib.sha256(der).hexdigest()


class PinnedHTTPS(http.client.HTTPSConnection):
    """HTTPS connection that verifies the peer cert by SHA-256 fingerprint."""
    def __init__(self, host, port, expected_fp, timeout=15):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE  # we pin the exact cert ourselves
        super().__init__(host, port or 443, context=ctx, timeout=timeout)
        self._expected_fp = expected_fp

    def connect(self):
        super().connect()
        der = self.sock.getpeercert(binary_form=True)
        got = hashlib.sha256(der).hexdigest()
        if self._expected_fp and not hmac.compare_digest(got, self._expected_fp):
            self.close()
            raise ssl.SSLError(
                "cert pin mismatch: got %s… expected %s…"
                % (got[:16], self._expected_fp[:16]))


def connect(base_url, expected_fp):
    u = urlsplit(base_url)
    if u.scheme == "https":
        return PinnedHTTPS(u.hostname, u.port, expected_fp)
    if u.scheme == "http":
        return http.client.HTTPConnection(u.hostname, u.port or 80, timeout=15)
    raise ValueError("base-url must be http or https")


# ── request signing (§4.2) ─────────────────────────────────────────────────
def auth_header(dev_id, secret, request_uri, ts=None, nonce=None, tamper_mac=False):
    ts = str(int(time.time())) if ts is None else str(ts)
    nonce = os.urandom(8).hex() if nonce is None else nonce
    msg = "%s|%s|%s|%s" % (dev_id, ts, nonce, request_uri)
    mac = hmac.new(secret.encode(), msg.encode(), hashlib.sha256).hexdigest()
    if tamper_mac:
        mac = ("f" if mac[0] != "f" else "0") + mac[1:]  # flip one hex digit
    return "%s:%s:%s:%s" % (dev_id, ts, nonce, mac), nonce


def do(base_url, expected_fp, path, dev_id, secret, **kw):
    """Return (status, body_bytes). Signs `path` exactly as sent."""
    hdr, nonce = auth_header(dev_id, secret, path, **{
        k: v for k, v in kw.items() if k in ("ts", "nonce", "tamper_mac")})
    conn = connect(base_url, expected_fp)
    try:
        conn.request("GET", path, headers={"X-OTA-Auth": hdr})
        r = conn.getresponse()
        return r.status, r.read(), nonce
    finally:
        conn.close()


# ── test harness ────────────────────────────────────────────────────────────
class Runner:
    def __init__(self):
        self.rows = []
    def check(self, name, ok, detail=""):
        self.rows.append((ok, name, detail))
        mark = "PASS" if ok else "FAIL"
        print("  [%s] %-42s %s" % (mark, name, detail))
    def report(self):
        n = len(self.rows); p = sum(1 for ok, *_ in self.rows if ok)
        print("\n%d/%d passed" % (p, n))
        return 0 if p == n else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base-url", required=True)
    ap.add_argument("--cert", help="server cert PEM to pin (required for https)")
    ap.add_argument("--id", required=True, help="device id = full MAC, 12 lowercase hex")
    ap.add_argument("--secret")
    ap.add_argument("--secret-file")
    ap.add_argument("--fw", default="2.1.3", help="running version to report")
    ap.add_argument("--pinned-id")
    ap.add_argument("--pinned-secret")
    ap.add_argument("--expect-pinned", help="version the pinned unit must be offered")
    args = ap.parse_args()

    secret = args.secret
    if not secret and args.secret_file:
        secret = open(args.secret_file).read().strip()
    if not secret:
        ap.error("provide --secret or --secret-file")

    is_https = args.base_url.startswith("https")
    if is_https and not args.cert:
        ap.error("--cert is required for https (certificate pinning)")
    fp = fingerprint_from_pem(args.cert) if (is_https and args.cert) else None
    if fp:
        print("pinning server cert %s…%s" % (fp[:16], fp[-4:]))

    MANIFEST = "/manifest.php?fw=" + args.fw
    r = Runner()

    # 1. Happy path — valid auth → 200 + parseable manifest.
    st, body, _ = do(args.base_url, fp, MANIFEST, args.id, secret)
    ok = st == 200
    man = None
    if ok:
        try:
            man = json.loads(body)
            ok = all(k in man for k in ("version", "seq", "fw_file", "fw_sha256"))
        except Exception:
            ok = False
    r.check("happy path: manifest 200 + schema", ok,
            "version=%s" % (man.get("version") if man else "HTTP %d" % st))

    # 2. Wrong secret → 204.
    st, _, _ = do(args.base_url, fp, MANIFEST, args.id, secret + "00")
    r.check("wrong secret → 204", st == 204, "HTTP %d" % st)

    # 3. Tampered MAC → 204.
    st, _, _ = do(args.base_url, fp, MANIFEST, args.id, secret, tamper_mac=True)
    r.check("tampered MAC → 204", st == 204, "HTTP %d" % st)

    # 4. Clock skew (ts 6 min old) → 204.
    st, _, _ = do(args.base_url, fp, MANIFEST, args.id, secret, ts=int(time.time()) - 360)
    r.check("skew +360 s → 204", st == 204, "HTTP %d" % st)

    # 5. Replay: reuse a nonce that just succeeded → 204 on second use.
    hdr, nonce = auth_header(args.id, secret, MANIFEST)
    c = connect(args.base_url, fp); c.request("GET", MANIFEST, headers={"X-OTA-Auth": hdr})
    first = c.getresponse(); first.read(); c.close()
    st2, _, _ = do(args.base_url, fp, MANIFEST, args.id, secret, nonce=nonce,
                   ts=int(hdr.split(":")[1]))
    r.check("nonce replay → 204", first.status == 200 and st2 == 204,
            "first=%d replay=%d" % (first.status, st2))

    # 6. Unknown device id → 204.
    st, _, _ = do(args.base_url, fp, MANIFEST, "0000deadbeef", secret)
    r.check("unknown device → 204", st == 204, "HTTP %d" % st)

    # 7. Certificate pinning rejects a non-matching cert (https only).
    if fp:
        bad = "f" * 64
        try:
            do(args.base_url, bad, MANIFEST, args.id, secret)
            r.check("wrong-cert pin → rejected", False, "connection was NOT refused")
        except ssl.SSLError:
            r.check("wrong-cert pin → rejected", True, "SSLError as expected")
        except Exception as e:
            r.check("wrong-cert pin → rejected", False, "unexpected: %r" % e)

    # 8. Download the firmware artefact named by the manifest.
    if man:
        dpath = "/download.php?file=fw&v=" + man["version"]
        st, body, _ = do(args.base_url, fp, dpath, args.id, secret)
        r.check("download fw → 200 + bytes", st == 200 and len(body) > 0,
                "HTTP %d, %d bytes" % (st, len(body)))

    # 9. Optional: pinned-version resolution differs from mainstream.
    if args.pinned_id and args.pinned_secret and args.expect_pinned:
        st, body, _ = do(args.base_url, fp, "/manifest.php?fw=2.0.0",
                         args.pinned_id, args.pinned_secret)
        v = json.loads(body).get("version") if st == 200 else None
        r.check("pinned unit → pinned version", v == args.expect_pinned,
                "got %s expected %s" % (v, args.expect_pinned))

    sys.exit(r.report())


if __name__ == "__main__":
    main()
