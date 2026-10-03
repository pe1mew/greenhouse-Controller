# Archived rig data for the m3Char host tests

Raw data from the development rig (module 2344, M3's rope rig, the draw-wire encoder at Modbus
address 40), copied unchanged from Shuttle2's `~/ghc-soak/` on 2026-10-03.
`../../tools/gen_fixtures.py` turns it into `../test_m3_char/fixtures.h`.
The tests then require the library to reproduce what the bench harnesses printed for these runs.
**Do not edit these files.** A new rig run goes in as new files, with a row below.

## Minimum move (plan §3.6), `bin/at_wp_minmove.py`, 2.15.0-bench, 2026-10-02

| Files | Run | Used for |
|---|---|---|
| `minmove_1002.csv` / `.log` | 12:05, the first sweep, with no take-up pulses | **Reversal loss:** after every change of direction its short widths measured the rope's slack, and the log prints that comparison width by width. Its floor-2 figures are contaminated by the same slack |
| `minmove_1002b.csv` / `.log` | 12:16, **invalid** | The run nothing should be derived from. The wind safe-fail closed M3 mid-run, and the harness went on pulsing it against its closed end switch (gotcha 2026-10-02). It replays to "no floor 2, no fit", as the harness printed |
| `minmove_1002d.csv` / `.log` | 12:34, **the recorded run** | **Floor 2 and the dead time** as plan §3.6 records them: take-up pulses, 2 passes × 5 pulses per width and direction |

Each CSV has one row per pulse: the commanded width, the width at the relay's own GPIO edges, and
the live readings before and after.

The rest readings at the head of each log set the noise threshold. The harness took σ over all six
readings in run 1002, and over the last five from 1002b on, because the first can catch the leaf
still settling. The generator picks the rule that reproduces each run's printed σ.

## AT-WP02 (plan §0 item 4, §5b), `bin/at_wp02.py`, 2.15.0-bench, 2026-10-01

Ten approaches to 50 % each, starting alternately from the closed and the open end:

| File | Build | Printed result |
|---|---|---|
| `wp02_cuts_run1.log` | step 1's stop log, the old stop rule | spread 2.2 %, FAIL |
| `wp02_cuts_run2.log` | the same | 2.0 %, PASS |
| `wp02_ff8192.log` | step 2's build with fail-first bit 8192 (the old rule) | 1.7 %, PASS |
| `wp02_extrap_run1.log` | step 2, the 420 ms default lead, leads still learning | 2.4 %, FAIL |
| `wp02_extrap_run2.log` | the same, leads learned | 0.8 %, PASS |
| `wp02_final.log` | step 2 with the 250 ms default, from a boot: 2.15.1's code | 1.0 %, PASS |

The cuts JSON files of those runs stay on Shuttle2 (`~/ghc-soak/scatter_2026-10-01/`). The library
does not use them.
