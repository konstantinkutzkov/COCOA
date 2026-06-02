# sharpSAT.877a570

## Source

- Commit: `877a570` ("solveComponent: gate distinct_keys canonical-key diagnostic behind log_branches")
- Date: 2026-05-25
- Parent: `cf8082a` (L1/L2_STORE timer infra)

This is the tip of yesterday's work — the binary that produced the
**35.17 min t1_045 record** documented in `docs/benchmark_log.md` and
`memory/project_t1_045_solved.md`.

It is the immediate predecessor of the precomputed_key extension work
landed on 2026-05-26 (commits b7b089c, 4977ce5, 7b4b9b5, 838a621,
61d2583). Use it for A/B comparisons against the post-plumbing builds.

## Build

- Mode: Release (`-O3 -DNDEBUG`)
- Built from a `git worktree` at `/tmp/sharpsat-877a570` with library
  paths inherited from the main build's CMakeCache.txt:
  - `METIS_DIR`, `GKLIB_DIR`, `CMS5_DIR`, `CMS5_LIB`, `CADIBACK_LIB`,
    `CADICAL_LIB`, `SBVA_LIB`.
- No source patches required (unlike `sharpSAT.834f33a` which needed
  a `probe_preprocessor.h` stub).

## Reference characterization

Standard invocations for A/B testing:

```
./sharpSAT.877a570 -rec -sep 5 -cb 3 -sepMode metis temp_cnf/mc2025_track1_049.cnf
./sharpSAT.877a570 -rec -sep 5 -cb 3 -adaptive -wlIter 2 -t 2700 \
    temp_cnf/mc2025_track1_045.cnf
```

Yesterday's recorded measurements (these may have been on a contaminated
CPU — a stray sharpSAT process from earlier in the session was running
at 99% CPU, discovered on 2026-05-26):

| instance | wall (yesterday, suspect) |
|---|---:|
| t1_049 default | 321.59 s |
| t1_045 adaptive+wlIter=2 | 2110.22 s (35.17 min) |

A clean-CPU re-measurement using this binary is the gold reference
for evaluating the 2026-05-26 precomputed_key plumbing work.
