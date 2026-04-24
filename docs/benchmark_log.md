# Benchmark Log

Chronological record of solver timing measurements. Every run recorded
here includes: commit hash, compiler flags, solver CLI flags, input
instance, measured wall time, and any environmental notes.

## Conventions

- **Time**: wall-clock seconds reported by the solver's own `time:`
  output line (includes preprocessing, hierarchy build, and search).
- **Commit**: full 40-char SHA of the commit whose binary was run. If
  the tree had uncommitted changes, note "dirty" and list them.
- **Compile flags**: from CMake's `CMAKE_CXX_FLAGS_RELEASE` plus any
  `target_compile_options`. Default today: `-O3 -DNDEBUG -std=c++11
  -Wall -arch arm64`.
- **Solver flags**: exact argv after the binary name, up to (not
  including) the input filename.
- **Input**: absolute path + MD5 hash (protects against the CNF
  being silently edited).
- **Environment**: at minimum `uptime` load-average triple at run
  start; note any unusual system state (battery, high CPU processes,
  thermal).

Every benchmark run should produce one row. This file is append-only.

---

## Baselines

Reference measurements for comparison. Reproduce these when
validating environmental assumptions.

| Commit | Flags | Instance | Time | Environment | Notes |
|---|---|---|---|---|---|
| `bd6be09` | `-rec -sep 5 -cb 3 -sepMode metis` | t1_011.cnf | 26.66 s | (original, quiet) | Recorded in commit message at landing time. |
| `7629175` | `-rec -sep 5 -cb 3 -sepMode metis` | t1_011.cnf | 25.60 s | (original, quiet) | Recorded in commit message. |
| `a2610ec` | `-rec -sep 5 -cb 3 -sepMode metis` | t1_011.cnf | 25.91 s | (original, quiet) | Recorded in commit message. |

Note: `-rec` and `-sepMode metis` are no-ops in current main — the
first is accepted for backward compatibility, the second is an
unrecognized flag and silently treated as part of the input-file
argv (harmless). Effective invocation is `-sep 5 -cb 3` plus
`learn_level = 4` (default).

---

## Runs

Each row: `| date-time | commit | compile flags | solver flags | instance (md5) | time | uptime/notes |`

| Timestamp | Commit | Compile flags | Solver flags | Instance | Time | Env / notes |
|---|---|---|---|---|---|---|
| 2026-04-24 10:38 | `bd6be09` | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis` | /tmp/t1_011.cnf (md5 `4be5e40e8a1660b130a690981ebdea88`) | 44.44 s | load avg 3.63; VS Code + Claude desktop running; count correct. Does not reproduce commit-time 26.66s — env-dependent. |
| 2026-04-24 10:36 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis` | /tmp/t1_011.cnf (md5 `4be5e40e8a1660b130a690981ebdea88`) | 39.95 s | load avg 3.63; same env as bd6be09 row; cleanups verify ~5 s speedup vs bd6be09 under identical load. |

---

## Run template

Copy this block when adding a new entry:

```
| YYYY-MM-DD HH:MM | <short-sha> | <compile flags> | <solver flags> | <path> (md5 <hash>) | <time> s | load avg <x.xx>; <notes> |
```

Before adding a row, verify:

1. `git rev-parse HEAD` matches the commit column.
2. `git status` is clean (or note what's dirty and why).
3. Binary was rebuilt after the checkout (`cmake --build . && md5 build/sharpSAT` — record the binary hash if paranoid).
4. `md5 /tmp/t1_011.cnf` (or whatever input) matches the row's md5.
5. `uptime` output is captured before the run.

---

## Outstanding measurement issues

### Gap between recorded baselines and current reproductions

On this machine (M4 Max, macOS 26.4.1) the 26.66 s / 25.60 s / 25.91 s
baselines from commit messages cannot currently be reproduced. Every
commit tested runs ~17 s slower under today's conditions, across:
- `bd6be09`, `7629175`, `a2610ec`, `bf9c724` — all ~40-44 s.
- Clean rebuilds from scratch.
- Identical flags and CNF.
- AC power, no low-power mode.

The uniform offset rules out code regression (we'd see a step, not
a flat shift). Most likely cause: CPU thermal throttling from
sustained benchmarking over today's session, combined with elevated
competing load (load avg 3.5, VS Code renderer 30% CPU).

Reopening criteria:
- Close VS Code + Claude desktop, wait 10 min for thermal recovery,
  re-run. If numbers drop to ~27 s on bd6be09: thermal/load
  confirmed. If still ~40 s: real environmental change to
  investigate (system updates, library versions, etc.).

---

## Process discipline

- **One commit per row.** Never overwrite a previous row; always
  append.
- **Record environment.** `uptime` plus any noted heavy processes.
- **Record flag specifics.** Don't abbreviate; write the exact
  argv.
- **CNF hash.** If the instance file changes (e.g., a shrink, a
  preprocessing variant), it's a different row — the md5 makes
  this unambiguous.
- **Failed / timeout runs still logged.** Note "TIMEOUT" or the
  cause in the notes column; a missing run is worse than a
  logged failure.
