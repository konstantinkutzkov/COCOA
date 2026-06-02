# sharpSAT.834f33a

## Source

- Commit: `834f33a098fb51167f8522fadbdd595f58140e2d`
- Message: "Replace 'static thread_local' with 'static' (single-threaded only)"
- Date: 2026-04-26

This commit is the historical 282 s t1_049 baseline cited throughout
`docs/benchmark_log.md` and in memory notes (`project_t1_049_per_decision_slowdown`).
Predecessor commit `892a4eac` is functionally equivalent for performance.

## Build

- Mode: Release
- Built from a `git worktree` at `/tmp/sharpsat-834f33a` (detached HEAD).

## Build-time patches applied to the source tree

The original 834f33a does NOT build cleanly against the current
toolchain because:

1. `probe_preprocessor.{h,cpp}` is referenced indirectly but missing
   in the tree (it was added in a later commit). Stub versions of
   both files are present in the worktree to satisfy the linker;
   they do nothing functional at this commit.

2. `src/CMakeLists.txt` was modified to add `probe_preprocessor.cpp`
   to SOURCES.

## Diagnostic backport

`OpTimer` instrumentation (the `OP_ANALYZE / OP_CANONICAL / OP_L1_PEEK
/ OP_L2_PEEK / OP_PICK / OP_BCP` timer struct plus `printOpStats`)
was backported from the current code into this binary, so OP_STATS
lines can be parsed for bucketed per-op cost analysis.

The OpTimer overhead is also present in the current binary, so it
cancels in any current-vs-historical comparison. Per-call BCP numbers
will be inflated by ~5–10 ns from the chrono::steady_clock pair, but
uniformly inflated on both sides.

## Reference characterization

Standard invocation:

```
./sharpSAT.834f33a -rec -sep 5 -cb 3 -sepMode metis temp_cnf/mc2025_track1_049.cnf
```

t1_049 with `-t 60` (2026-05-25 measurement):
- **decisions in 60 s**: 33.6 M
- **BCP ns/call (10 % bucket → 100 % bucket)**: 44.7 → 55.9 ns
- **BCP climb across run**: +25 % (same as current binary, so this is
  inherent to the t1_049 deep-tail trajectory — NOT a regression in
  current code)

Current binary (post commits `51f85ac` + `c4a6efe`) on the same run:
- decisions in 60 s: 25.9 M (-23 % throughput vs historical)
- BCP ns/call (10 % → 100 %): 52.5 → 65.3 (+~10 ns uniform overhead vs historical)

The uniform ~10 ns/call BCP overhead in the current binary is consistent
with the Literal struct widening (+48 bytes from `binary_link_ids_` and
`redundant_binary_links_` added between 834f33a and current). Throughput
gap is larger (30 %) than BCP-only gap (20 %), so other ops (CANONICAL,
ANALYZE) also contribute.
