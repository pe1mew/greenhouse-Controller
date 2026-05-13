# GitHub integration

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

2. Save the token to `.github/token.local` (this folder, file is `.gitignored`).

   **From Git Bash** (recommended — no BOM, no trailing newline):

   ```bash
   printf '%s' 'ghp_xxxxxxxxxxxx' > .github/token.local
   ```

   **From PowerShell** — beware the encoding default. Plain `> file` and
   `Out-File` in Windows PowerShell 5.1 write **UTF-16 LE with a BOM**,
   which trips naive UTF-8 readers. Always pass an explicit encoding:

   ```powershell
   Set-Content -Path .github/token.local -Value 'ghp_xxxxxxxxxxxx' `
               -Encoding utf8 -NoNewline
   ```

   (The `gh_issue.py` token reader is tolerant of common BOMs and CRLF
   line endings — so a file written with the wrong encoding still works
   — but a clean UTF-8 file is preferable.)

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

The `GITHUB_TOKEN` and `GH_TOKEN` env vars are also recognised if you'd
rather keep the token in your shell environment than in a file. The env vars
take precedence over `.github/token.local`.

## Token leak — what to do

If `.github/token.local` ever leaks (accidentally committed, copied
somewhere visible, etc.):

1. Immediately revoke the leaked token at
   <https://github.com/settings/tokens>.
2. Generate a fresh one and rewrite `.github/token.local`.
3. If the leak was via git, also rewrite history via `git filter-repo` to
   purge the bytes — revocation alone makes the token unusable but the
   string is still visible in the repo's history.
