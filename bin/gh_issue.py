#!/usr/bin/env python3
"""
Tiny GitHub Issues client for greenhouse-Controller.

No external dependencies (uses the Python stdlib only). Talks to the GitHub
REST API v3 with a Personal Access Token.

## One-time setup

1. Generate a fine-grained Personal Access Token at
       https://github.com/settings/tokens?type=beta
   - Repository access:  Only select repositories → pe1mew/greenhouse-Controller
   - Repository permissions:
       Issues          : Read and write
       Metadata        : Read-only (auto-granted)
   - Expiry: pick whatever you're comfortable with (90 days is sensible).
2. Save the token to a local file Claude will read.

   From **Git Bash** (recommended — no BOM, no surprises):

       printf '%s' 'ghp_xxxxx...' > .github/token.local

   From **PowerShell** — beware the default encoding gotcha. PowerShell 5.1's
   `>` redirection and `Out-File` write UTF-16 LE with a BOM, which trips
   naive UTF-8 readers. Always pass an explicit encoding:

       Set-Content -Path .github/token.local -Value 'ghp_xxxxx...' `
                   -Encoding utf8 -NoNewline

   This script's token reader (see `_read_token_file()` below) is tolerant
   of UTF-16 LE/BE BOMs, UTF-8 BOM, and CRLF line endings — so a file
   written the wrong way still works — but a clean UTF-8 file is preferable.

   The .github/token.local path is in .gitignore — it won't be committed.
   The env vars GITHUB_TOKEN / GH_TOKEN are also recognised if you'd rather
   keep the token there.

## Usage

    python bin/gh_issue.py list                        # open issues
    python bin/gh_issue.py list --state all
    python bin/gh_issue.py show 6                      # full body + comments
    python bin/gh_issue.py create --title T --body-file body.md
    python bin/gh_issue.py create --title T --body "..." --label bug
    python bin/gh_issue.py comment 6 --body "..."
    python bin/gh_issue.py comment 6 --body-file note.md
    python bin/gh_issue.py close 6 --reason completed   # or not_planned
    python bin/gh_issue.py reopen 6
    python bin/gh_issue.py edit 6 --title "new title"
    python bin/gh_issue.py edit 6 --body-file revised.md  # body-only edit

All commands print a short status line to stdout. Errors go to stderr with
a non-zero exit code so they can be detected from shell scripts.
"""

import argparse
import json
import os
import pathlib
import sys
import urllib.error
import urllib.request

REPO   = "pe1mew/greenhouse-Controller"
API    = f"https://api.github.com/repos/{REPO}"
# Token resolution order: env GITHUB_TOKEN → env GH_TOKEN → .github/token.local
ROOT   = pathlib.Path(__file__).resolve().parent.parent
TOKEN_FILE = ROOT / ".github" / "token.local"


def _read_token_file(path: pathlib.Path) -> str:
    """Read the token file, tolerating common Windows encodings.

    PowerShell 5.1's `>` redirection and `Out-File` default to UTF-16 LE
    with a BOM (`FF FE`), which trips `read_text(encoding="utf-8")`. Some
    editors save UTF-8 with a BOM (`EF BB BF`). The token itself is
    always ASCII, so we sniff the BOM, decode appropriately, then strip.
    """
    raw = path.read_bytes()
    if   raw.startswith(b"\xff\xfe"):
        text = raw[2:].decode("utf-16-le", errors="ignore")
    elif raw.startswith(b"\xfe\xff"):
        text = raw[2:].decode("utf-16-be", errors="ignore")
    elif raw.startswith(b"\xef\xbb\xbf"):
        text = raw[3:].decode("utf-8", errors="ignore")
    else:
        text = raw.decode("utf-8", errors="ignore")
    # Strip whitespace, any stray nulls, and CR/LF (PowerShell's CRLF endings).
    return text.strip().replace("\x00", "").replace("\r", "").replace("\n", "")


def get_token() -> str:
    tok = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if tok:
        return tok.strip()
    if TOKEN_FILE.exists():
        try:
            tok = _read_token_file(TOKEN_FILE)
        except OSError as e:
            print(f"error: could not read {TOKEN_FILE}: {e}", file=sys.stderr)
            sys.exit(2)
        if not tok:
            print(
                f"error: {TOKEN_FILE} is empty or contained only whitespace.\n"
                f"  Write the token on a single line, e.g.:\n"
                f"    printf '%s' 'ghp_xxx' > .github/token.local",
                file=sys.stderr,
            )
            sys.exit(2)
        return tok
    print(
        f"error: no GitHub token found.\n"
        f"  Set env GITHUB_TOKEN, or create {TOKEN_FILE} with the token on a single line.\n"
        f"  See the docstring at the top of bin/gh_issue.py for setup steps.",
        file=sys.stderr,
    )
    sys.exit(2)


def request(method: str, path: str, payload=None) -> dict:
    url = f"{API}{path}"
    data = None
    headers = {
        "Authorization": f"Bearer {get_token()}",
        "Accept":         "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent":     "greenhouse-controller-gh-issue/1.0",
    }
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            body = resp.read().decode("utf-8")
            return json.loads(body) if body else {}
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        print(f"HTTP {e.code} {e.reason}: {body}", file=sys.stderr)
        sys.exit(1)
    except urllib.error.URLError as e:
        print(f"network error: {e.reason}", file=sys.stderr)
        sys.exit(1)


def cmd_list(args):
    state = args.state or "open"
    issues = request("GET", f"/issues?state={state}&per_page=50")
    for it in issues:
        if "pull_request" in it:
            continue            # filter out PRs
        labels = ",".join(l["name"] for l in it.get("labels", []))
        labels = f" [{labels}]" if labels else ""
        print(f"#{it['number']} [{it['state']}]{labels} {it['title']}")


def cmd_show(args):
    issue = request("GET", f"/issues/{args.number}")
    print(f"#{issue['number']} [{issue['state']}] {issue['title']}")
    print(f"by {issue['user']['login']}  created {issue['created_at']}")
    labels = ",".join(l["name"] for l in issue.get("labels", []))
    if labels: print(f"labels: {labels}")
    print()
    print(issue.get("body") or "(no body)")
    print()
    comments = request("GET", f"/issues/{args.number}/comments")
    for c in comments:
        print(f"--- comment by {c['user']['login']} at {c['created_at']} ---")
        print(c["body"])


def _read_body(args) -> str:
    if args.body and args.body_file:
        print("error: --body and --body-file are mutually exclusive", file=sys.stderr)
        sys.exit(2)
    if args.body_file:
        return pathlib.Path(args.body_file).read_text(encoding="utf-8")
    return args.body or ""


def cmd_create(args):
    payload = {"title": args.title, "body": _read_body(args)}
    if args.label:
        payload["labels"] = args.label
    issue = request("POST", "/issues", payload)
    print(f"created #{issue['number']}: {issue['html_url']}")


def cmd_comment(args):
    body = _read_body(args)
    if not body.strip():
        print("error: empty body — pass --body or --body-file", file=sys.stderr)
        sys.exit(2)
    c = request("POST", f"/issues/{args.number}/comments", {"body": body})
    print(f"commented on #{args.number}: {c['html_url']}")


def cmd_close(args):
    payload = {"state": "closed"}
    if args.reason:
        payload["state_reason"] = args.reason
    issue = request("PATCH", f"/issues/{args.number}", payload)
    print(f"closed #{issue['number']} ({issue.get('state_reason') or 'completed'})")


def cmd_reopen(args):
    issue = request("PATCH", f"/issues/{args.number}", {"state": "open"})
    print(f"reopened #{issue['number']}")


def cmd_edit(args):
    """Edit an issue's title and/or body (PATCH)."""
    body = _read_body(args) if (args.body or args.body_file) else None
    payload = {}
    if args.title is not None:
        payload["title"] = args.title
    if body is not None:
        payload["body"] = body
    if not payload:
        print("error: nothing to change — pass --title and/or --body/--body-file", file=sys.stderr)
        sys.exit(2)
    issue = request("PATCH", f"/issues/{args.number}", payload)
    print(f"edited #{issue['number']}: {issue['html_url']}")


def main():
    # Windows console is often cp1252; issue bodies routinely contain em-dashes
    # and other non-cp1252 chars. Reconfigure stdout to UTF-8 with replacement
    # so `show` doesn't crash mid-print. No-op on Linux/macOS terminals.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass

    p   = argparse.ArgumentParser(description="Minimal GitHub Issues client for this repo.")
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("list",    help="list issues")
    pl.add_argument("--state", choices=["open", "closed", "all"])
    pl.set_defaults(func=cmd_list)

    ps = sub.add_parser("show",    help="show full issue body and comments")
    ps.add_argument("number", type=int)
    ps.set_defaults(func=cmd_show)

    pc = sub.add_parser("create",  help="create a new issue")
    pc.add_argument("--title", required=True)
    pc.add_argument("--body",      help="issue body (inline)")
    pc.add_argument("--body-file", help="read body from file (use - for stdin)")
    pc.add_argument("--label", action="append", help="label name (can repeat)")
    pc.set_defaults(func=cmd_create)

    pm = sub.add_parser("comment", help="post a comment on an issue")
    pm.add_argument("number", type=int)
    pm.add_argument("--body")
    pm.add_argument("--body-file")
    pm.set_defaults(func=cmd_comment)

    px = sub.add_parser("close",   help="close an issue")
    px.add_argument("number", type=int)
    px.add_argument("--reason", choices=["completed", "not_planned", "duplicate"])
    px.set_defaults(func=cmd_close)

    pr = sub.add_parser("reopen",  help="reopen an issue")
    pr.add_argument("number", type=int)
    pr.set_defaults(func=cmd_reopen)

    pe = sub.add_parser("edit",    help="edit title and/or body of an existing issue")
    pe.add_argument("number", type=int)
    pe.add_argument("--title")
    pe.add_argument("--body")
    pe.add_argument("--body-file")
    pe.set_defaults(func=cmd_edit)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
