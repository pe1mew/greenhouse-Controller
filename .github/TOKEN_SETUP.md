# Token setup for bin/gh_issue.py

This directory holds the local-only GitHub Personal Access Token used by
`bin/gh_issue.py` to read and write issues on
[`pe1mew/greenhouse-Controller`](https://github.com/pe1mew/greenhouse-Controller).

## One-time setup

1. Generate a fine-grained Personal Access Token:
   - Open <https://github.com/settings/tokens?type=beta>
   - **Repository access**: *Only select repositories* → `pe1mew/greenhouse-Controller`
   - **Repository permissions**:
     - **Issues**: *Read and write*
     - **Metadata**: *Read-only* (auto-granted)
   - **Expiry**: pick a value you're comfortable with (90 days is sensible).

2. Store the token. Resolution order is `GITHUB_TOKEN`/`GH_TOKEN` env →
   `GH_ISSUE_TOKEN_FILE` (env, or the git-ignored `.github/gh_issue.local`
   pointer) → `.github/token.local`. Two common setups:

   **a. In the operator's secret store (current setup).** Keep the token file
   outside this repo and point `gh_issue.py` at it with a git-ignored
   `.github/gh_issue.local` (covered by the `*.local` ignore rule):

   ```
   GH_ISSUE_TOKEN_FILE=/path/in/operator-secret-store/gh_issue_token
   ```

   **b. In this folder (simple default).** Save the token to
   `.github/token.local` (git-ignored). From **Git Bash** (no BOM):

   ```bash
   printf '%s' 'github_pat_xxxx' > .github/token.local
   ```

   From **PowerShell** — pass an explicit encoding (5.1 defaults to UTF-16+BOM):

   ```powershell
   Set-Content -Path .github/token.local -Value 'github_pat_xxxx' -Encoding utf8 -NoNewline
   ```

   The reader tolerates BOMs and CRLF, so a wrongly-encoded file still works,
   but a clean single-line UTF-8 file is preferable either way.

3. Verify it works:

   ```bash
   python bin/gh_issue.py list
   ```

   You should see the current open issues. If you see
   `error: no GitHub token found.` the file isn't being read; check the path
   and that it has exactly one line of content.

## Day-to-day usage

| Command | What it does |
|---|---|
| `python bin/gh_issue.py list` | List open issues |
| `python bin/gh_issue.py list --state all` | Include closed ones |
| `python bin/gh_issue.py show 6` | Full body of issue #6 + every comment |
| `python bin/gh_issue.py create --title T --body-file note.md --label bug` | New issue |
| `python bin/gh_issue.py comment 6 --body "fixed in 1.17.26"` | Post a comment |
| `python bin/gh_issue.py close 6 --reason completed` | Close (with reason) |
| `python bin/gh_issue.py reopen 6` | Reopen |

The `GITHUB_TOKEN` / `GH_TOKEN` env vars hold the token value directly and take
precedence over any file. `GH_ISSUE_TOKEN_FILE` (env, or the
`.github/gh_issue.local` pointer) selects which file to read.

The token that publishes **releases** (needs Contents: Read and write) is a
separate token — see [`bin/rota_release.md`](../bin/rota_release.md).

## Token leak — what to do

If the token file ever leaks (accidentally committed, copied
somewhere visible, etc.):

1. Immediately revoke the leaked token at
   <https://github.com/settings/tokens>.
2. Generate a fresh one and rewrite the token file.
3. If the leak was via git, also rewrite history via `git filter-repo` to
   purge the bytes — revocation alone makes the token unusable but the
   string is still visible in the repo's history.
