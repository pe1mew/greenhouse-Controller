# Memory index

Layer-3 index for the greenhouse-Controller project. Auto-loaded by Claude Code after `CLAUDE.md`. Topic files are reachable on-demand via the table below and via the CLAUDE.md "Before You Start" pointers.

## Topic files

| Topic | File | When to read |
|---|---|---|
| Architecture (task graph + subsystem map + partitions) | [architecture.md](architecture.md) | Before touching any FreeRTOS task or subsystem |
| Before You Start, long form | [before-you-start.md](before-you-start.md) | Before touching the audit log format, a config key, motor travel/dwell, the Modbus bus, the window sensor or SD logging — the full text CLAUDE.md's rows used to carry, moved 2026-09-24 |
| Gotcha log | [gotcha-log.md](gotcha-log.md) | When something weird happens, before debugging from scratch |
| Gotcha archive | [gotcha-archive.md](gotcha-archive.md) | Rarely — only when tracing the history of a fix that can no longer recur. Never for triage |

## Conventions

- `gotcha-log.md` is append-only. Newest at top. Format: **Problem → Root cause → Fix → Where it lives.**
- Topic files are reference material extracted to keep `CLAUDE.md` under the ~100-line audit threshold. Move stable content here; session narrative stays out.
- User-global preferences and personal positions live in `~/.claude/projects/C--Users-drasv-github-greenhouse-Controller/memory/`, not here. That includes field-unit IPs and machine-specific access routes.

## See also

- [../CLAUDE.md](../CLAUDE.md) — auto-loaded project guide
- agent-ready-projects v1.10.3 — framework version this adoption tracks
