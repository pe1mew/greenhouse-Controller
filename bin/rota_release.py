#!/usr/bin/env python3
"""ROTA release toolchain -- publish a built release to the FOTA server.

Wire contract: rota-contract-v1.1 (design/rota_tds.md section 4). Matches the
greenhouse-Controller-FOTA-server store layout (its examples/README.md):

    ota-store/
      releases/<version>/greenhouse-controller-<version>.bin
      releases/<version>/web-assets-<version>.zip
      releases/<version>/manifest-<version>.json     (section 4.3 manifest)
      channels/soak.json          {"<unit_type>": {"version": "<version>"}}
      channels/mainstream.json
      devices.json  checkins.csv  nonce-cache/        (server-managed)

Subcommands (roles per the server repo's examples/README.md):
  publish <version>   Emit the manifest and upload artefacts, then point the
                      SOAK channel at the release (R-T01 -- one command from
                      a built bin/<version>/ to soak-offered).
  release <version>   Create a GitHub Release (tag v<version>) with the .bin +
                      .zip + manifest as assets, for the PULL-BASED deploy: the
                      FOTA server retrieves the release and points soak itself.
                      Full release -> soak; --prerelease stages without soak.
  promote <version>   Point the MAINSTREAM channel at an already-published
                      release (the "ota_promote" role), after a soak cycle.
  status              Show what each channel currently offers + known releases.

Transports:
  remote (default)    ssh/scp to the VPS. SSH public-key auth with host-key
                      verification ON (R-T07); prefer an ~/.ssh/config alias so
                      the user/key/known-host are pinned there.
  local (--local DIR) Write straight into a local ota-store directory -- for
                      testing against `php -S` (server repo examples/README.md).

seq (anti-downgrade, R-V01/02) is assigned automatically as max(existing)+1;
re-publishing the same version reuses its seq (idempotent, R-S08). Override
with --seq. A publish that would not strictly advance the soak seq is refused.

Config + defaults come from bin/.rota_release.env (git-ignored; copy from
bin/rota_release.env.example) or CLI flags. Stdlib only; ASCII output.

Usage:
    python bin/rota_release.py release 2.2.12             # -> GitHub Release (pull deploy)
    python bin/rota_release.py release 2.2.12 --prerelease --dry-run
    python bin/rota_release.py publish 2.2.12             # -> ota-store soak (scp/local)
    python bin/rota_release.py publish 2.2.12 --local /tmp/ota-store
    python bin/rota_release.py promote 2.2.12
    python bin/rota_release.py status
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN_DIR = REPO_ROOT / "bin"
ENV_FILE = BIN_DIR / ".rota_release.env"

DEFAULT_STORE = "/var/www/ota-store"
DEFAULT_UNIT_TYPE = "ghc1"
DEFAULT_MIN_VERSION = "2.1.0"
CONTRACT = "rota-contract-v1.1"

# GitHub Releases transport (public repo; creating a release needs a write token).
GH_REPO = "pe1mew/greenhouse-Controller"
GH_API = "https://api.github.com/repos/" + GH_REPO
TOKEN_FILE = REPO_ROOT / ".github" / "token.local"   # legacy fallback
_TOKEN_FILE = None   # dedicated release-token path (ROTA_TOKEN_FILE); set in main()

# Same token rule the server enforces (rota_lib.php rota_valid_version).
VERSION_RE = re.compile(r"^[0-9A-Za-z.\-]+$")
UNIT_TYPE_RE = re.compile(r"^[0-9A-Za-z._-]+$")
SEQ_RE = re.compile(rb'"seq"\s*:\s*(\d+)')


def die(msg):
    sys.exit("ERROR: " + msg)


# --------------------------------------------------------------------------
# Config
# --------------------------------------------------------------------------
def load_env():
    """Read bin/.rota_release.env (KEY=VALUE lines) then the process env."""
    cfg = {}
    if ENV_FILE.is_file():
        for line in ENV_FILE.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            cfg[k.strip()] = v.strip()
    for k in ("ROTA_SSH", "ROTA_STORE", "ROTA_UNIT_TYPE", "ROTA_TOKEN_FILE",
              "ROTA_MIN_VERSION", "ROTA_BASE_URL", "ROTA_CERT"):
        if os.environ.get(k):
            cfg[k] = os.environ[k]
    return cfg


# --------------------------------------------------------------------------
# GitHub Releases API (public repo; creating a release needs a write token)
# --------------------------------------------------------------------------
def _read_token_file(path):
    """Read the PAT, tolerating UTF-16/UTF-8 BOMs + CRLF (matches gh_issue.py)."""
    raw = path.read_bytes()
    if raw.startswith(b"\xff\xfe"):
        text = raw[2:].decode("utf-16-le", "ignore")
    elif raw.startswith(b"\xfe\xff"):
        text = raw[2:].decode("utf-16-be", "ignore")
    elif raw.startswith(b"\xef\xbb\xbf"):
        text = raw[3:].decode("utf-8", "ignore")
    else:
        text = raw.decode("utf-8", "ignore")
    return text.strip().replace("\x00", "").replace("\r", "").replace("\n", "")


def gh_token():
    tok = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if tok:
        return tok.strip()
    for cand in (_TOKEN_FILE, str(TOKEN_FILE)):     # dedicated release token, then legacy
        if cand and pathlib.Path(cand).is_file():
            tok = _read_token_file(pathlib.Path(cand))
            if tok:
                return tok
    die("no GitHub token (needs 'Contents: Read and write' on %s): point "
        "ROTA_TOKEN_FILE (in bin/.rota_release.env) at the token file, or set "
        "GITHUB_TOKEN." % GH_REPO)


def _gh_headers(extra=None):
    h = {
        "Authorization": "Bearer " + gh_token(),
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "greenhouse-controller-rota-release/1.0",
    }
    if extra:
        h.update(extra)
    return h


def gh_api(method, path, payload=None, allow_404=False):
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    headers = _gh_headers({"Content-Type": "application/json"} if data else None)
    req = urllib.request.Request(GH_API + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            body = r.read().decode("utf-8")
            return json.loads(body) if body else {}
    except urllib.error.HTTPError as e:
        if allow_404 and e.code == 404:
            return None
        die("GitHub %s %s -> HTTP %d %s: %s" % (method, path, e.code, e.reason,
            e.read().decode("utf-8", "replace")[:300]))
    except urllib.error.URLError as e:
        die("network error talking to GitHub: %s" % e.reason)


def gh_upload_asset(upload_url_tmpl, path, content_type):
    """POST one asset to uploads.github.com (host differs from the REST API)."""
    url = upload_url_tmpl.split("{", 1)[0] + "?name=" + urllib.parse.quote(path.name)
    req = urllib.request.Request(url, data=path.read_bytes(), method="POST",
                                 headers=_gh_headers({"Content-Type": content_type}))
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        die("asset upload (%s) -> HTTP %d %s: %s" % (path.name, e.code, e.reason,
            e.read().decode("utf-8", "replace")[:300]))
    except urllib.error.URLError as e:
        die("network error uploading %s: %s" % (path.name, e.reason))


def gh_release_by_tag(tag):
    return gh_api("GET", "/releases/tags/" + tag, allow_404=True)


def git_head():
    r = subprocess.run(["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
                       capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def git_dirty():
    r = subprocess.run(["git", "-C", str(REPO_ROOT), "status", "--porcelain"],
                       capture_output=True, text=True)
    return bool(r.stdout.strip())


# --------------------------------------------------------------------------
# Stores (transport abstraction)
# --------------------------------------------------------------------------
class LocalStore:
    """ota-store on the local filesystem (testing / php -S)."""
    kind = "local"

    def __init__(self, root, dry_run):
        self.root = pathlib.Path(root).expanduser().resolve()
        self.dry = dry_run
        self.label = str(self.root)

    def read_text(self, rel):
        p = self.root / rel
        return p.read_text(encoding="utf-8") if p.is_file() else None

    def exists(self, rel):
        return (self.root / rel).exists()

    def list_seqs(self):
        out = []
        for mf in self.root.glob("releases/*/manifest-*.json"):
            for m in SEQ_RE.finditer(mf.read_bytes()):
                out.append(int(m.group(1)))
        return out

    def put_file(self, local_path, rel):
        dest = self.root / rel
        if self.dry:
            print("  [dry-run] copy %s -> %s" % (local_path, dest))
            return
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(str(local_path), str(dest))
        print("  put  %s  (%d bytes)" % (rel, dest.stat().st_size))

    def write_text(self, rel, text):
        dest = self.root / rel
        if self.dry:
            print("  [dry-run] write %s  (%d bytes)" % (dest, len(text)))
            return
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(text, encoding="utf-8")
        print("  write %s  (%d bytes)" % (rel, len(text)))


class RemoteStore:
    """ota-store on the VPS, reached via ssh/scp (R-T07: key auth, host-key ON)."""
    kind = "remote"

    def __init__(self, target, root, dry_run):
        self.target = target
        self.root = root.rstrip("/")
        self.dry = dry_run
        self.label = "%s:%s" % (target, self.root)
        self.opts = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes"]

    def _ssh(self, remote_cmd):
        return subprocess.run(["ssh"] + self.opts + [self.target, remote_cmd],
                              capture_output=True, text=True)

    def read_text(self, rel):
        r = self._ssh("cat %s/%s 2>/dev/null" % (self.root, rel))
        return r.stdout if (r.returncode == 0 and r.stdout) else None

    def exists(self, rel):
        return self._ssh("test -e %s/%s" % (self.root, rel)).returncode == 0

    def list_seqs(self):
        r = self._ssh("grep -h -o '\"seq\"[[:space:]]*:[[:space:]]*[0-9]*' "
                      "%s/releases/*/manifest-*.json 2>/dev/null || true" % self.root)
        return [int(x) for x in re.findall(r"[0-9]+", r.stdout or "")]

    def _scp(self, local_path, remote_rel):
        r = subprocess.run(["scp"] + self.opts +
                           [str(local_path), "%s:%s/%s" % (self.target, self.root, remote_rel)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            die("scp failed (%s): %s" % (remote_rel, (r.stderr or "").strip()
                or "check ROTA_SSH alias / known_hosts / key"))

    def put_file(self, local_path, rel):
        if self.dry:
            print("  [dry-run] scp %s -> %s/%s" % (local_path, self.label, rel))
            return
        d = os.path.dirname(rel)
        if d:
            self._ssh("mkdir -p %s/%s" % (self.root, d))
        self._scp(local_path, rel)
        print("  put  %s" % rel)

    def write_text(self, rel, text):
        if self.dry:
            print("  [dry-run] write %s/%s  (%d bytes)" % (self.label, rel, len(text)))
            return
        d = os.path.dirname(rel)
        if d:
            self._ssh("mkdir -p %s/%s" % (self.root, d))
        tmp = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8")
        try:
            tmp.write(text)
            tmp.close()
            self._scp(tmp.name, rel + ".tmp")           # upload beside target
            mv = self._ssh("mv %s/%s.tmp %s/%s" % (self.root, rel, self.root, rel))
            if mv.returncode != 0:
                die("remote mv failed (%s): %s" % (rel, (mv.stderr or "").strip()))
        finally:
            os.unlink(tmp.name)
        print("  write %s  (%d bytes)" % (rel, len(text)))


def make_store(args, cfg):
    if args.local:
        return LocalStore(args.local, args.dry_run)
    target = args.ssh or cfg.get("ROTA_SSH")
    if not target:
        die("no SSH target: pass --ssh <alias|user@host>, set ROTA_SSH in "
            "bin/.rota_release.env, or use --local <dir> for a local store")
    root = args.store or cfg.get("ROTA_STORE", DEFAULT_STORE)
    return RemoteStore(target, root, args.dry_run)


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------
def store_json(store, rel):
    t = store.read_text(rel)
    if not t:
        return None
    try:
        return json.loads(t)
    except ValueError:
        return None


def sha256_size(path):
    h = hashlib.sha256()
    n = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
            n += len(chunk)
    return h.hexdigest(), n


def make_manifest(version, seq, unit_type, min_version, fw, fw_sha, fw_size,
                  zipf, as_sha, as_size):
    """The §4.3 manifest, emitted verbatim to the store / release asset."""
    return {
        "version": version, "seq": seq, "unit_type": unit_type,
        "min_version": min_version, "key_id": "",
        "fw_file": fw.name, "fw_sha256": fw_sha, "fw_size": fw_size,
        "assets_file": zipf.name, "assets_sha256": as_sha, "assets_size": as_size,
        "released_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }


def local_bin_seqs():
    out = []
    for mf in BIN_DIR.glob("*/manifest-*.json"):
        for m in SEQ_RE.finditer(mf.read_bytes()):
            out.append(int(m.group(1)))
    return out


def resolve_release(spec):
    """(version, release_dir) from a version, a .bin path, or a bin/<version> dir."""
    p = pathlib.Path(spec)
    if p.suffix == ".bin" and p.exists():
        m = re.match(r"greenhouse-controller-(.+)\.bin$", p.name)
        if not m:
            die("bin filename must be greenhouse-controller-<version>.bin: %s" % p.name)
        return m.group(1), p.parent
    if p.is_dir():
        return p.name, p
    return spec, BIN_DIR / spec


def channel_version(store, channel, unit_type):
    c = store_json(store, "channels/%s.json" % channel) or {}
    entry = c.get(unit_type) or {}
    return entry.get("version")


def release_manifest(store, version):
    if not version:
        return None
    return store_json(store, "releases/%s/manifest-%s.json" % (version, version))


def confirm(args, prompt):
    if args.yes or args.dry_run:
        return True
    try:
        return input(prompt + " [y/N] ").strip().lower() in ("y", "yes")
    except EOFError:
        return False


# --------------------------------------------------------------------------
# publish  (-> soak)
# --------------------------------------------------------------------------
def cmd_publish(args, cfg, store):
    version, reldir = resolve_release(args.version)
    if not VERSION_RE.match(version):
        die("invalid version token: %s" % version)
    unit_type = args.unit_type or cfg.get("ROTA_UNIT_TYPE", DEFAULT_UNIT_TYPE)
    if not UNIT_TYPE_RE.match(unit_type):
        die("invalid unit_type: %s" % unit_type)

    fw = reldir / ("greenhouse-controller-%s.bin" % version)
    zipf = reldir / ("web-assets-%s.zip" % version)
    for p in (fw, zipf):
        if not p.is_file():
            die("missing artefact: %s  (build it first: bin/build_release.ps1)" % p)

    print("== ROTA publish -> soak (%s) ==" % CONTRACT)
    print("version   : %s" % version)
    print("unit_type : %s" % unit_type)
    print("store     : %s%s" % (store.label, "   [DRY-RUN]" if args.dry_run else ""))
    print("firmware  : %s" % fw.name)
    print("assets    : %s" % zipf.name)

    fw_sha, fw_size = sha256_size(fw)
    as_sha, as_size = sha256_size(zipf)
    print("fw_sha256 : %s  (%d bytes)" % (fw_sha, fw_size))
    print("as_sha256 : %s  (%d bytes)" % (as_sha, as_size))

    # ---- seq (anti-downgrade, R-V01/02) --------------------------------
    existing = release_manifest(store, version)
    soak_ver = channel_version(store, "soak", unit_type)
    soak_seq = (release_manifest(store, soak_ver) or {}).get("seq") if soak_ver else None

    if args.seq is not None:
        seq = args.seq
    elif existing and isinstance(existing.get("seq"), int):
        seq = existing["seq"]                       # re-publish: keep seq (R-S08)
        print("note      : re-publishing %s -- reusing existing seq %d" % (version, seq))
    else:
        baseline = store.list_seqs() + local_bin_seqs()
        if not baseline:
            die("cannot determine next seq (no existing manifests). Pass --seq N "
                "for the first publish; the current baseline is 2.2.0 -> seq 30.")
        seq = max(baseline) + 1

    # Refuse a non-advancing publish (would be rejected by devices as a downgrade),
    # unless it's an exact re-publish of the current soak release.
    if soak_seq is not None and seq <= soak_seq and version != soak_ver:
        die("seq %d does not advance the current soak seq %d (release %s) -- "
            "devices would reject it as a downgrade. Use --seq to override." %
            (seq, soak_seq, soak_ver))
    print("seq       : %d%s" % (seq, "" if soak_seq is None else "  (soak is %d)" % soak_seq))

    # ---- min_version: --flag > existing > carry-forward > default ------
    if args.min_version:
        min_version = args.min_version
    elif existing and existing.get("min_version"):
        min_version = existing["min_version"]
    else:
        cur = release_manifest(store, soak_ver) if soak_ver else None
        min_version = (cur or {}).get("min_version") \
            or cfg.get("ROTA_MIN_VERSION", DEFAULT_MIN_VERSION)
    print("min_ver   : %s" % min_version)

    manifest = make_manifest(version, seq, unit_type, min_version,
                             fw, fw_sha, fw_size, zipf, as_sha, as_size)
    manifest_text = json.dumps(manifest, indent=2) + "\n"
    manifest_name = "manifest-%s.json" % version

    # Keep a master copy in the repo (R-S08: bin/<version>/ is the master copy).
    local_manifest = reldir / manifest_name
    if args.dry_run:
        print("\n-- manifest (%s) --\n%s" % (local_manifest, manifest_text), end="")
    else:
        local_manifest.write_text(manifest_text, encoding="utf-8")
        try:
            shown = local_manifest.relative_to(REPO_ROOT)
        except ValueError:
            shown = local_manifest                       # release dir outside the repo
        print("\nwrote master copy: %s" % shown)

    if not confirm(args, "\nUpload artefacts + manifest and point SOAK at %s?" % version):
        print("aborted.")
        return 1

    print("\n[1/2] upload release artefacts")
    relbase = "releases/%s" % version
    store.put_file(fw, "%s/%s" % (relbase, fw.name))
    store.put_file(zipf, "%s/%s" % (relbase, zipf.name))
    store.write_text("%s/%s" % (relbase, manifest_name), manifest_text)

    print("[2/2] point soak channel")
    chan = store_json(store, "channels/soak.json") or {}
    chan[unit_type] = {"version": version}
    store.write_text("channels/soak.json", json.dumps(chan, indent=2) + "\n")

    print("\nOK -- soak now offers %s (seq %d) for unit_type %s." % (version, seq, unit_type))
    _verify_hint(cfg, version)
    return 0


# --------------------------------------------------------------------------
# release  (-> GitHub Release; the FOTA server pulls it and points soak)
# --------------------------------------------------------------------------
def _release_body(reldir, version, seq, unit_type, min_version,
                  fw, fw_sha, fw_size, zipf, as_sha, as_size):
    notes = reldir / "release-notes.md"
    if notes.is_file():
        return notes.read_text(encoding="utf-8")
    return (
        "ROTA firmware release **%s** (seq %d, unit_type `%s`, %s).\n\n"
        "| artefact | bytes | sha256 |\n|---|---|---|\n"
        "| `%s` | %d | `%s` |\n| `%s` | %d | `%s` |\n\n"
        "min_version: `%s`\n\n"
        "The FOTA server retrieves this release into `ota-store/` and points the "
        "**soak** channel. Mark it *pre-release* to stage without pointing soak; "
        "promotion to mainstream stays a manual server-side step.\n"
    ) % (version, seq, unit_type, CONTRACT,
         fw.name, fw_size, fw_sha, zipf.name, as_size, as_sha, min_version)


def cmd_release(args, cfg):
    version, reldir = resolve_release(args.version)
    if not VERSION_RE.match(version):
        die("invalid version token: %s" % version)
    unit_type = args.unit_type or cfg.get("ROTA_UNIT_TYPE", DEFAULT_UNIT_TYPE)
    if not UNIT_TYPE_RE.match(unit_type):
        die("invalid unit_type: %s" % unit_type)

    fw = reldir / ("greenhouse-controller-%s.bin" % version)
    zipf = reldir / ("web-assets-%s.zip" % version)
    for p in (fw, zipf):
        if not p.is_file():
            die("missing artefact: %s  (build it first: bin/build_release.ps1)" % p)

    tag = "v" + version
    kind = "pre-release (stage only)" if args.prerelease else "release (-> soak)"
    print("== ROTA release -> GitHub (%s) ==" % CONTRACT)
    print("version   : %s   tag %s" % (version, tag))
    print("unit_type : %s" % unit_type)
    print("repo      : %s   %s%s" % (GH_REPO, kind, "   [DRY-RUN]" if args.dry_run else ""))
    print("firmware  : %s" % fw.name)
    print("assets    : %s" % zipf.name)

    fw_sha, fw_size = sha256_size(fw)
    as_sha, as_size = sha256_size(zipf)
    print("fw_sha256 : %s  (%d bytes)" % (fw_sha, fw_size))
    print("as_sha256 : %s  (%d bytes)" % (as_sha, as_size))

    # seq: the repo's bin/<version>/manifest-*.json ledger is the master copy
    # (R-S08); a re-release of the same version reuses its seq.
    manifest_path = reldir / ("manifest-%s.json" % version)
    existing = None
    if manifest_path.is_file():
        try:
            existing = json.loads(manifest_path.read_text(encoding="utf-8"))
        except ValueError:
            existing = None
    if args.seq is not None:
        seq = args.seq
    elif existing and isinstance(existing.get("seq"), int):
        seq = existing["seq"]
        print("note      : re-releasing %s -- reusing existing seq %d" % (version, seq))
    else:
        baseline = local_bin_seqs()
        if not baseline:
            die("cannot determine next seq (no bin/*/manifest-*.json). Pass --seq; "
                "current ledger baseline is 2.2.0 -> seq 30.")
        seq = max(baseline) + 1

    if args.min_version:
        min_version = args.min_version
    elif existing and existing.get("min_version"):
        min_version = existing["min_version"]
    else:
        min_version = cfg.get("ROTA_MIN_VERSION", DEFAULT_MIN_VERSION)
    print("seq       : %d" % seq)
    print("min_ver   : %s" % min_version)

    manifest = make_manifest(version, seq, unit_type, min_version,
                             fw, fw_sha, fw_size, zipf, as_sha, as_size)
    manifest_text = json.dumps(manifest, indent=2) + "\n"
    body = _release_body(reldir, version, seq, unit_type, min_version,
                         fw, fw_sha, fw_size, zipf, as_sha, as_size)
    head = git_head()

    if args.dry_run:
        print("\n-- manifest (%s) --\n%s" % (manifest_path.name, manifest_text), end="")
        print("\n[dry-run] would create GitHub release:")
        print("  tag/name  : %s" % tag)
        print("  prerelease: %s   draft: %s" % (bool(args.prerelease), bool(args.draft)))
        print("  target    : HEAD %s" % (head[:8] if head else "?"))
        print("  assets    : %s, %s, %s" % (fw.name, zipf.name, manifest_path.name))
        return 0

    rel = gh_release_by_tag(tag)
    if rel and not args.force:
        die("release %s already exists: %s  (use --force to update it + replace assets)"
            % (tag, rel.get("html_url")))
    if git_dirty():
        print("warning   : working tree has uncommitted changes; the release tags HEAD "
              "(%s), which may not match these artefacts." % (head[:8] if head else "?"))
    if not confirm(args, "\nCreate GitHub release %s and upload 3 assets?" % tag):
        print("aborted.")
        return 1

    manifest_path.write_text(manifest_text, encoding="utf-8")

    payload = {"tag_name": tag, "name": tag, "body": body,
               "draft": bool(args.draft), "prerelease": bool(args.prerelease)}
    if rel is None:
        if head:
            payload["target_commitish"] = head
        rel = gh_api("POST", "/releases", payload)
        print("\ncreated release %s" % rel.get("html_url"))
    else:
        gh_api("PATCH", "/releases/%d" % rel["id"], payload)
        print("\nupdating existing release %s" % rel.get("html_url"))

    have = {a["name"]: a["id"] for a in rel.get("assets", [])}
    for path, ctype in ((fw, "application/octet-stream"),
                        (zipf, "application/zip"),
                        (manifest_path, "application/json")):
        if path.name in have:                    # replace an existing same-named asset
            gh_api("DELETE", "/releases/assets/%d" % have[path.name])
        gh_upload_asset(rel["upload_url"], path, ctype)
        print("  uploaded %s" % path.name)

    tail = ("stage without pointing soak" if args.prerelease else "point the soak channel")
    print("\nOK -- release %s published (seq %d): %s" % (tag, seq, rel.get("html_url")))
    print("       The FOTA server will retrieve it and %s." % tail)
    return 0


# --------------------------------------------------------------------------
# promote  (-> mainstream)
# --------------------------------------------------------------------------
def cmd_promote(args, cfg, store):
    version = args.version
    if not VERSION_RE.match(version):
        die("invalid version token: %s" % version)
    unit_type = args.unit_type or cfg.get("ROTA_UNIT_TYPE", DEFAULT_UNIT_TYPE)

    print("== ROTA promote -> mainstream (%s) ==" % CONTRACT)
    print("version   : %s" % version)
    print("unit_type : %s" % unit_type)
    print("store     : %s%s" % (store.label, "   [DRY-RUN]" if args.dry_run else ""))

    man = release_manifest(store, version)
    if not man:
        die("release %s is not published (no releases/%s/manifest-%s.json). "
            "Run: rota_release.py publish %s" % (version, version, version, version))
    if not store.exists("releases/%s/%s" % (version, man.get("fw_file", ""))):
        die("manifest present but firmware artefact missing on the store for %s" % version)

    soak_ver = channel_version(store, "soak", unit_type)
    if soak_ver != version and not args.force:
        die("%s is not the current soak release (soak = %s). Soak it first, or "
            "pass --force to promote anyway." % (version, soak_ver))
    main_ver = channel_version(store, "mainstream", unit_type)
    print("seq       : %d   (mainstream is %s)" %
          (man.get("seq", -1), main_ver or "unset"))

    if not confirm(args, "Point MAINSTREAM at %s (was %s)?" % (version, main_ver)):
        print("aborted.")
        return 1

    chan = store_json(store, "channels/mainstream.json") or {}
    chan[unit_type] = {"version": version}
    store.write_text("channels/mainstream.json", json.dumps(chan, indent=2) + "\n")
    print("\nOK -- mainstream now offers %s for unit_type %s." % (version, unit_type))
    print("Reminder: production units may be pinned (devices.json pinned_version); "
          "unpin on-site after a good soak cycle (R-T05).")
    return 0


# --------------------------------------------------------------------------
# status
# --------------------------------------------------------------------------
def cmd_status(args, cfg, store):
    unit_type = args.unit_type or cfg.get("ROTA_UNIT_TYPE", DEFAULT_UNIT_TYPE)
    print("== ROTA store status ==")
    print("store     : %s" % store.label)
    print("unit_type : %s" % unit_type)
    for channel in ("soak", "mainstream"):
        v = channel_version(store, channel, unit_type)
        m = release_manifest(store, v) if v else None
        if m:
            print("  %-10s -> %-14s seq=%s min_version=%s released_at=%s" %
                  (channel, v, m.get("seq"), m.get("min_version"), m.get("released_at")))
        else:
            print("  %-10s -> %s" % (channel, v or "(unset)"))
    seqs = sorted(set(store.list_seqs()))
    if seqs:
        print("releases  : %d on store, seq range %d..%d" % (len(seqs), seqs[0], seqs[-1]))
    else:
        print("releases  : none found (or store unreachable)")
    return 0


def _verify_hint(cfg, version):
    base = cfg.get("ROTA_BASE_URL")
    cert = cfg.get("ROTA_CERT")
    if not base:
        return
    parts = ["python bin/rota_sim.py --base-url %s" % base]
    if cert:
        parts.append("--cert %s" % cert)
    parts.append("--id <full-mac> --secret-file <path> --fw <running-version>")
    print("verify    : %s" % " ".join(parts))


# --------------------------------------------------------------------------
def build_parser():
    # Shared options live on parent parsers so they work AFTER the subcommand
    # (e.g. `status --local DIR`, `publish 2.2.12 --dry-run`). `store_opts` apply
    # only to the ota-store transports (publish/promote/status), not `release`.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--dry-run", action="store_true",
                        help="print planned actions; make no changes")
    common.add_argument("--unit-type", help="unit_type key (default %s)" % DEFAULT_UNIT_TYPE)
    common.add_argument("--yes", action="store_true", help="skip the confirmation prompt")

    store_opts = argparse.ArgumentParser(add_help=False)
    store_opts.add_argument("--ssh", help="SSH target (alias or user@host); or ROTA_SSH")
    store_opts.add_argument("--store", help="ota-store path on the VPS (default %s)" % DEFAULT_STORE)
    store_opts.add_argument("--local", help="use a LOCAL ota-store dir instead of ssh")

    p = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Config: bin/.rota_release.env (copy from bin/rota_release.env.example).")
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("publish", parents=[common, store_opts],
                        help="publish a built release to the ota-store and point soak")
    sp.add_argument("version", help="version, bin/<version> dir, or path to the .bin")
    sp.add_argument("--seq", type=int, help="override the auto-assigned seq")
    sp.add_argument("--min-version", help="anti-downgrade floor for the manifest")
    sp.set_defaults(func=cmd_publish)

    sp = sub.add_parser("release", parents=[common],
                        help="create a GitHub Release (pull-based deploy)")
    sp.add_argument("version", help="version, bin/<version> dir, or path to the .bin")
    sp.add_argument("--seq", type=int, help="override the auto-assigned seq")
    sp.add_argument("--min-version", help="anti-downgrade floor for the manifest")
    sp.add_argument("--prerelease", action="store_true",
                    help="mark pre-release: FOTA server stages it without pointing soak")
    sp.add_argument("--draft", action="store_true", help="create as an unpublished draft")
    sp.add_argument("--force", action="store_true",
                    help="update an existing release + replace its assets")
    sp.set_defaults(func=cmd_release)

    sp = sub.add_parser("promote", parents=[common, store_opts],
                        help="point mainstream at a published release")
    sp.add_argument("version", help="version to promote (must already be published)")
    sp.add_argument("--force", action="store_true",
                    help="promote even if it is not the current soak release")
    sp.set_defaults(func=cmd_promote)

    sp = sub.add_parser("status", parents=[common, store_opts],
                        help="show channel + release status")
    sp.set_defaults(func=cmd_status)
    return p


def main():
    args = build_parser().parse_args()
    cfg = load_env()
    global _TOKEN_FILE
    _TOKEN_FILE = cfg.get("ROTA_TOKEN_FILE")     # dedicated release-token path, if configured
    if args.cmd == "release":                    # GitHub transport, no ota-store
        return cmd_release(args, cfg) or 0
    store = make_store(args, cfg)
    return args.func(args, cfg, store) or 0


if __name__ == "__main__":
    sys.exit(main())
