# Benchmark Log

Chronological record of solver timing measurements. Every run recorded
here includes: commit hash, compiler flags, solver CLI flags, input
instance, measured wall time, and any environmental notes.

## 2026-05-29 — Blocker literal (Glucose/MiniSat-2 style) → +75 % throughput on triple_no_lockstep t1_105

Added BCP path-frequency counters (path A/B/C/D/E/F/G + scan length) to
verify the synthesis claim that BCP is the bottleneck on t1_105.

**Profile finding before any code change** (5-min runs on t1_105):

| Config | BCP wall share | BCP per-call | Dominant BCP path |
|---|---:|---:|---|
| `-rec -sep 5 -cb 3 -sepMode metis` (default) | 9.7 % | 3.8 μs | n/a (CANONICAL = 80 %) |
| triple_no_lockstep + `-checkUnsat` | **94.4 %** | **86.9 μs** | B_sat = 52 %, C_scope = 38 % |

Under triple_no_lockstep, B + C = 90 % of all long-clause visits are
non-productive skips; the productive F_propagate path is 0.19 %.

**Change** (~80 LOC, uncommitted):

- `struct WatchEntry { ClauseOfs ofs; LiteralID blocker; }` replaces
  `ClauseOfs` as the `watch_list_` element type
  ([structures.h:82-92](src/structures.h#L82-L92)).
- BCP loop ([solver.cpp:1067-1140](src/solver.cpp#L1067-L1140)) checks
  `isSatisfied(itcl->blocker)` FIRST — single `literal_values_` read,
  no `beginOf(ofs)` cache-line miss. On miss-then-real-B, the stale
  blocker is refreshed.
- All `addWatchLinkTo` callers pass the OTHER watched literal as the
  initial blocker (instance.cpp:218-219, 318-321, instance.h:757-758,
  solver.cpp:294-295, solver_diagnostics.cpp:1518-1519).
- On replacement-found, the new entry's blocker = other watch
  (still X_TRI or T_TRI per the isSatisfied check we just passed).
- On propagation, the current entry's blocker is updated to the
  now-T other watch (helps post-backtrack re-descent).

**Result on triple_no_lockstep t1_105, 5-min budget**:

| metric | BEFORE | AFTER | delta |
|---|---:|---:|---:|
| decisions | 2 885 490 | **5 059 076** | **+75 %** |
| cache stores | 823 817 | 943 875 | +15 % |
| cache hits | 928 247 | **1 945 062** | **+110 %** |
| BCP visits | 2 246 569 943 | 5 069 928 679 | +126 % |
| BCP avg per-call (ns) | 86 898 | **53 037** | **−39 %** |
| BCP wall share | 94.4 % | 93.1 % | − |
| B_sat count | 1.18 B | 2.77 B | +136 % |
| C_scope share | 38.1 % | 39.1 % | — (next target) |

Invocation:
```
./sharpsat-separator/build/sharpSAT -checkUnsat -rec -sep 5 -cb 3 \
    -sepMode metis -wlIter 2 -reactiveMetis -reactiveMetisMin 10 \
    -reactiveMetisSkip 4 -unifiedPicker -decomposeAfterK 1000 \
    -cascadeW 0 -t 300 ./temp_cnf/mc2025_track1_105.cnf
```

**Soundness**: count canaries verified identical to memory on t1_065
(37 778 931 862 957 161 709 568), t1_071, t1_011 (536 870 912 306).
Default-flag runs also produce identical counts.

**Open**: path C (38 % of BCP visits = learned-clause scope/component
check at solver.cpp:1090-1093) is now the dominant overhead path.
Memoization via per-clause [min_var, max_var] or per-(clause,
sub_varset_version) cache is the candidate next lever.

## 2026-05-29 — Path C postmortem + BFS scratch buffers → +18.5 % more decisions on top of blocker

**Failed first attempt: per-clause outside-mask vars cache** keyed on
`current_sub_varset_version_` (bumped in SubVarsetGuard ctor/dtor).
On t1_105 triple_no_lockstep at -t 300: decisions 5.06 M → 5.00 M
(−1.2 %), BCP per-call 53.0 μs → 53.7 μs (+1.3 % regression). Cause:
mask-version churn (~10^3/s) leaves the cache cold; unordered_map
find overhead exceeds the ~10 ns walk it tried to replace; and the
cache targeted the wrong function entirely.

**Telemetry split (C1 = sound_fail, C2 = comp_fail)** at
solver.cpp:1115-1124 + new counters in [instance.h:543-617](src/instance.h#L543-L617)
([sound_calls_, sound_memo_hits_, sound_bfs_nodes_, in_component_calls_, in_component_lits_walked_]):

```
C1_sound = 1,981,705,593  (39.12 % of BCP visits)
C2_comp  =     517,190    ( 0.01 %)  ← negligible
sound_calls = 2.28 B   memo_hit% = 82.86 %
avg_bfs_per_miss = 6.79 nodes      in_component_avg_walk = 20.13 lits
```

**99.97 % of path C is the sound check.** Even at 83 % memo hit rate,
the 17 % BFS misses (388 M calls) each allocated a fresh
`unordered_set<ClauseOfs>` + `vector<ClauseOfs>` per call — that
allocation churn is what dominated path C wall time.

**Fix**: reused member scratch buffers + linear-search dedupe (typical
visited set ~7 entries; linear search beats hashmap at that size).
[instance.h:543-617 + scratch members at instance.h:564-575](src/instance.h#L543-L617).
~30 LOC.

**Result on triple_no_lockstep t1_105, -t 300**:

| metric | post-blocker baseline | + BFS scratch | delta |
|---|---:|---:|---:|
| decisions | 5 056 494 | **5 991 204** | **+18.5 %** |
| BCP avg per-call (ns) | 53 073 | **44 318** | **−16.5 %** |
| BCP visits | 5.07 B | 6.30 B | +24.4 % |
| cache hits | 1 943 913 | 2 287 733 | +17.7 % |
| sound_calls | 2.28 B | 2.64 B | +15.6 % |
| memo_hit % | 82.86 | 82.95 | — |
| BCP wall share | 93.2 % | 91.6 % | − |

**Cumulative gain vs original triple_no_lockstep baseline** (no blocker, no
BFS fix): 2 885 490 → 5 991 204 decisions = **+108 % (2.08× throughput)**
in the same 5-min budget. Counts on canaries t1_065, t1_011, t1_071
unchanged.

Invocation:
```
./sharpsat-separator/build/sharpSAT -checkUnsat -rec -sep 5 -cb 3 \
    -sepMode metis -wlIter 2 -reactiveMetis -reactiveMetisMin 10 \
    -reactiveMetisSkip 4 -unifiedPicker -decomposeAfterK 1000 \
    -cascadeW 0 -t 300 ./temp_cnf/mc2025_track1_105.cnf
```

**Open**: at 91.6 % BCP wall share, BCP is still the dominant op.
Within BCP, C1_sound is still 35 % of visits. Next candidate
optimizations (per [the workflow](postmortem)):
- Per-Clause sound-version stamp in the clause header (~2 ns header
  read replaces ~30 ns unordered_map find in the memo)
- Precomputed "minimal-witness" per learned clause at learn time
  (sound check becomes O(1-4) set intersection vs O(5-20) BFS)
- The 17 % memo-miss rate likely sustained by `removed_clauses_version_`
  churn from branchOnClause/deriv_cache probes — reducing the churn
  rate would amplify the memo



## 2026-05-26 — Clean-CPU re-measurement: t1_045 sound best is 34.41 min; new plumbing reverted

Tracked down a stray sharpSAT process (PID 83908) that had been running
at 99 % CPU for ~8 hours, leftover from a SHARPSAT_PROGRESS smoke test
earlier in the session that used `SHARPSAT_PROGRESS_INTERVAL=0.05` and
ground itself to a halt on t1_011. This contaminated every measurement
made since the stray started — including the 35.17 min t1_045 number
recorded earlier today (2026-05-26).

After killing the stray, re-measurements on a clean CPU:

| binary | wall on clean CPU | per-decision |
|---|---:|---:|
| `877a570` (post-`distinct_keys` gate, no precomputed_key plumbing) | **2064.70 s (34.41 min)** | **3.020 μs** |
| HEAD with all 5 precomputed_key plumbing commits | 2072.33 s (34.54 min) | 3.030 μs |

Both runs produced count `132 951 278 067 432` and decision count
`683 874 584` — bit-identical trajectory; the difference is purely
per-decision throughput.

The 877a570 binary is checkpointed at `binaries/sharpSAT.877a570` with
a sidecar `.md` documenting the build provenance.

### Conclusion: revert the precomputed_key plumbing for the 3 new sites

The new sites (1827, 1851, 2015 — separator-rest-continuation and
`branchOnLiteral` lit-already-T_TRI) don't fire often enough on
t1_045 / t1_049 to amortize the per-`solveComponent` snapshot
construction + parameter-passing overhead. Net effect:

- t1_045: plumbing cost +7.6 s / +0.37 % (2064.70 → 2072.33 s)
- t1_049: plumbing cost +3.2 s / +1.0 %  (321.59 → 324.79 s)

Both small but in the same direction across two independent
instances. Reverted commits 838a621 (site 2015 + branchOnLiteral
signature), 7b4b9b5 (site 1851), and 4977ce5 (site 1827 +
solveComponentImpl signature).

### What is kept

The supporting **infrastructure** is preserved:

- `b7b089c` PrecomputedKeySnapshot struct + shadow verifier
  (`SHARPSAT_VERIFY_PRECOMPUTED_KEY=1`). Cost is ~zero unless a
  site actually passes a snap.
- The existing site 1320 (post-consumption decompose loop, the only
  site that ever benefited from precomputed_key) continues to use the
  snapshot mechanism with the always-on size assertion.
- `61d2583` Release-safety fix for the assertion.
- `binaries/sharpSAT.877a570` checkpoint binary + sidecar `.md`.

If a future instance is found where the rest-continuation paths fire
often (instrumentable cheaply via a counter at each site), the
plumbings can be re-introduced one site at a time using the existing
infrastructure. The plan and per-site safety reasoning remain in
`docs/precomputed_key_extension_plan.md`.

### New sound best on t1_045

**34.41 min** (clean CPU, 877a570 binary equivalent). Beats the
2026-05-23 unsound build (34.87 min) by 27 s while remaining sound.
The previously-recorded 35.17 min was contaminated; supersede it.

### Lesson

Before any A/B perf measurement, **explicitly check for stray
sharpSAT processes** with `ps aux | grep "[s]harpSAT"`. Any
unexpected hit invalidates the comparison. See
`memory/feedback_check_cpu_before_measuring.md`.

---

## 2026-05-26 — `distinct_keys` diagnostic gated + cache density wins: t1_045 35.17 min (SUPERSEDED — see clean-CPU re-measurement above)

**SUPERSEDED**: the 35.17 min figure below was measured under stray-process CPU contention. Clean-CPU figure for the same binary (`877a570`) is **34.41 min** — see the entry above.

Today's four wall-time-relevant commits (51f85ac cache density, c4a6efe
redundant-lane gate, cf8082a L1/L2_STORE instrumentation, 877a570
distinct_keys gate) together close the post-branchOnClause-UIP-fix
regression on t1_045 to within 0.9 % of the unsound-build baseline.

The biggest single contributor was the `distinct_keys` gate
(commit `877a570`): a `static std::unordered_set<std::pair<uint64_t,uint64_t>>`
diagnostic that grew monotonically across the entire solve and ran
unconditionally on every CANONICAL build. Identified by a sub-agent
code review on 2026-05-25 after two days of OpTimer instrumentation
failed to localize the cost (the diagnostic was inside the timed
CANONICAL window, attributed there with the build itself).

### Invocation (the documented winning t1_045 configuration)

```
SHARPSAT_PROGRESS=1 SHARPSAT_PROGRESS_INTERVAL=60 \
./sharpSAT -rec -sep 5 -cb 3 -adaptive -wlIter 2 -t 2700 \
    temp_cnf/mc2025_track1_045.cnf
```

### Result

| run | wall | per-decision | count |
|---|---:|---:|---:|
| 34.87 min unsound build (2026-05-23, branchOnClause UIP bug) | 2092.2 s | 3.07 μs | 132 951 278 067 432 |
| post-fix regression (2026-05-25, after branch-constraint antecedent) | 2267.85 s (37.80 min) | 3.32 μs | 132 951 278 067 432 |
| **today (2026-05-26, all today's commits)** | **2110.22 s (35.17 min)** | **3.09 μs** | **132 951 278 067 432** |

Δ vs yesterday's post-fix: **−157.6 s / −6.95 %**. Δ vs unsound build: **+18 s / +0.9 %** (essentially parity, while remaining sound — the canary `t1_011 + -derivCacheBias 1` regression test passes).

Decision count `683,874,584` is bit-identical to the 2026-05-25 run, so
trajectory is unchanged; the entire improvement is per-decision
throughput (3.32 → 3.09 μs/dec). The improvement is consistent with
the 60 s window measurement showing +6.3 % decision throughput from
the `distinct_keys` gate alone.

### Per-minute pct_lin trajectory (today vs yesterday)

| t (min) | yesterday pct_lin | today pct_lin | Δ |
|---:|---:|---:|---:|
| 5 | 5.33 | 5.65 | +0.32 |
| 10 | 23.94 | **34.40** | **+10.46** |
| 15 | 62.78 | **78.13** | **+15.35** |
| 20 | 81.45 | 81.53 | +0.08 (both stalled in hard cluster) |
| 25 | 83.48 | 84.98 | +1.50 |
| 30 | 86.34 | **88.15** | +1.81 |
| 34 | 90.67 | **96.92** | **+6.25** |
| 35 | 91.69 | 98.86 | +7.17 |
| finish | 37.80 | **35.17** | **−2.63 min** |

Both runs traverse the same hard cluster around t=18–23 (closed_bits
nearly flat at 89.7), then today escapes it ahead of yesterday and
maintains the lead through to the deep-tail collapse phase.

### What this leaves open

Direction 1 from `docs/precomputed_key_extension_plan.md` (plumb
`precomputed_key` through the 3 remaining candidate recursive
`solveComponent` call sites at 1827, 1851, 2015) is still on the
table. The current 2.1× canonical_key-call-rate-per-decision inversion
vs historical 834f33a is the suspected source of any remaining gap on
other instances.

Counts verified across the standard suite: t1_065 (37 778 931 862 957 161 709 568),
t1_011 (536 870 912 306), t1_011 + `-derivCacheBias 1`
(536 870 912 306), t1_049 (8 695 763 196 077 742, full run today at
321.59 s — recorded separately), t1_045 (132 951 278 067 432). All
match historical entries; no count regressions.

---

## 2026-05-25 — is_branch_constraint to parallel std::vector<bool>: t1_049 329.35 s (−6 %); t1_045 37.80 min (still +8.4 % vs historical)

The `branch-constraint antecedent` work (2026-05-25) widened the
`Variable` struct from 8 → 16 bytes by adding `bool is_branch_constraint`.
Hypothesis: the widening hurt cache density on the hot `variables_[v]`
access path. Mitigation: keep `Variable` at 8 bytes by moving the flag
into a parallel `std::vector<bool> is_branch_constraint_` on `Instance`.

Build: Release, uncommitted working tree on top of `c0c460c`.

### Invocation

```
./sharpSAT -rec -sep 5 -cb 3 -sepMode metis temp_cnf/mc2025_track1_049.cnf
SHARPSAT_PROGRESS=1 SHARPSAT_PROGRESS_INTERVAL=60 ./sharpSAT \
    -rec -sep 5 -cb 3 -adaptive -wlIter 2 -t 2700 \
    temp_cnf/mc2025_track1_045.cnf
```

### t1_049 (5-min smoke)

| metric | `cf60d14` (pre, precomputed_key only) | **+ cache density (today)** | delta |
|---|---:|---:|---:|
| **wall time** | 332.24 s | **329.35 s** | **−2.9 s / −0.9 %** |
| decisions | 145 297 070 | ~145.1 M | ≈ 0 |
| count | 8 695 763 196 077 742 | 8 695 763 196 077 742 | ✓ |

Cumulative vs `c0c460c` (351 s) baseline: precomputed_key (−18.8 s) + cache density (−2.9 s) = **−21.7 s / −6.2 %**. Per-op stats showed ANALYZE/CANONICAL both dropped ~10–12 % per call, consistent with the cache-density hypothesis.

### t1_045 (45-min budget, winning historical config)

| metric | 2026-05-23 historical (`canonical_key` fix) | **today (branch-constraint + cache density)** | delta |
|---|---:|---:|---:|
| **wall time** | 2 092.2 s (34.87 min) | **2 267.85 s (37.80 min)** | **+8.4 % / +2.93 min** |
| decisions | 682.1 M | 683.9 M | +0.3 % |
| L2 stores | 680.4 M | **502.2 M** | **−26 %** |
| L2 hits | 226.7 M | 227.2 M | +0.2 % |
| L2 hit rate | 33.3 % | **45.2 %** | **+11.9 pp** |
| μs/decision | 3.07 | 3.32 | +8.2 % |
| count | 132 951 278 067 432 | 132 951 278 067 432 | ✓ |

**Per-minute pct_lin trajectory** (every 60 s, SHARPSAT_PROGRESS):

| t (min) | pct_lin | closed_bits | open | decisions (M) | notes |
|---:|---:|---:|---:|---:|---|
| 1 | 0.29 | 81.58 | 35 | 23.3 | |
| 5 | 5.33 | 85.77 | 31 | 112.5 | |
| 9 | 21.27 | 87.77 | 34 | 195.3 | first big collapse |
| 11 | 35.31 | 88.50 | 29 | 235.9 | second collapse |
| 12 | 43.59 | 88.80 | 34 | 255.7 | halfway |
| 15 | 62.78 | 89.33 | 21 | 311.7 | third collapse |
| 16 | 77.82 | 89.64 | 30 | 329.0 | |
| 17 | 79.70 | 89.67 | 40 | 346.3 | enters multi-min stall |
| 22 | 81.54 | 89.71 | 41 | 440.7 | flat — expensive cluster |
| 30 | 86.34 | 89.79 | 35 | 570.7 | |
| 33 | 90.27 | 89.85 | 16 | 614.5 | breaks out |
| 37 | 97.43 | 89.96 | 33 | 674.5 | |
| 37.80 | 100.0 | 90.00 | 0 | 683.9 | finish |

Search trajectory looks like: rapid ramp 0 → 50 % in 12 min, then a 5-min collapse phase to 80 %, then ~8 min stuck in a hard cluster (closed_bits crawled +0.001/min for several minutes), then deep-tail collapse to finish.

### Interpretation

- **Cache layer is now better than historical**: L2 stores fell 26 %, hit rate +12 pp. The `canonical_key` free-var fix + cache density change is unambiguously good for the cache.
- **Per-decision cost still elevated**: 3.32 vs 3.07 μs/dec = +8.2 %. The branch-constraint-antecedent rewrite of `branchOnClause` (Option D) introduces overhead the cache wins don't fully offset. Component management + UIP dispatch are the remaining suspects.
- Trajectory shape (sequence of collapses) tracks the historical run qualitatively; the slowdown is broadly distributed, not concentrated in one sub-tree.

Counts cross-confirmed: 132 951 278 067 432 (matches ganak + historical sharpSAT).

---

## 2026-05-26 — precomputed canonical_key for decompose-loop recursion: t1_049 332.24 s (−5.4 %)

Commit `cf60d14` adds optional `const CanonicalKey *precomputed_key`
parameter to `Solver::solveComponent`. The decompose-loop recursion at
`solver_rec.cpp:1305` passes the already-built key (computed at
`solver_rec.cpp:1162`) so the recursion's entry skips a redundant
`buildCanonicalKey` + L2-peek.

Safety verified analytically (six invariants) in
`docs/precomputed_key_safety_analysis.md`; soundness verified on
t1_011 + the original multi-decision-DL UIP-bug reproducer.

### Invocation

```
./sharpSAT -rec -sep 5 -cb 3 -sepMode metis temp_cnf/mc2025_track1_049.cnf
```

### Result

| metric | `c0c460c` (pre) | **`cf60d14`** | delta |
|---|---:|---:|---:|
| **wall time** | 351.0 s | **332.24 s** | **−5.4 % / −18.8 s** |
| decisions | 145 297 070 | 145 297 070 | 0 |
| L2 stores | 149 652 951 | 149 652 951 | 0 |
| L2 hits | 37 935 877 | 37 935 877 | 0 |
| count | 8 695 763 196 077 742 | 8 695 763 196 077 742 | ✓ |

Trajectory bit-identical (correctness invariant of the change).

### Gap to historical baseline `834f33a` (282.82 s)

| | wall | gap | gap recovered |
|---|---:|---:|---:|
| historical (834f33a, 2026-04-26) | 282.82 s | 0 | — |
| `c0c460c` (pre-change, 2026-05-25) | 351.0 s | +24.1 % | 0 |
| **`cf60d14` (precomputed_key)** | **332.24 s** | **+17.5 %** | **~28 % of gap** |

### Per-operation breakdown (60s window)

| operation | c0c460c | cf60d14 | delta |
|---|---:|---:|---:|
| canonical_key calls in 60 s | 35.25 M | 32.32 M | −8 % |
| canonical_key time | 25.5 s | 24.0 s | −1.5 s |
| L2_PEEK calls | 35.25 M | 32.32 M | −8 % |
| canonical+peek per decision | 2.31 μs | 1.93 μs | −0.38 μs/dec |
| decisions completed in 60 s | 12.22 M | 13.90 M | +14 % |

### Interpretation

The fix targets only ONE of five `solveComponent` call sites (the post-
consumption decompose loop). The other four (branchOnLiteral,
branchOnClause, etc.) still rebuild canonical_key at recursion entry —
legitimately, because BCP changed state between caller and recursion,
so the key COULD differ.

The 2.93 M canonical_key builds saved in the 60s window are exactly
the decompose-loop duplicates. Most canonical builds are NOT
duplicates; they're from branching-after-BCP recursions where the
precomputed-key trick doesn't apply.

Net effect on t1_049: 19 s saved, ~28 % of the historical gap
recovered. Real, measurable win from an analytically-verified change,
just smaller than my over-optimistic projection.

The remaining ~50 s gap to historical is still distributed across the
unmeasured "OTHER" bucket (cache_store, branchOnLiteral lifecycle,
mpz_class arithmetic, SubVarsetGuard, derivative-cache hooks). Open
investigation; needs further instrumentation.

---

## 2026-05-25 — branch-constraint replaces per-decision-DL on t1_049: only ~1% recovery

Replaced the per-decision-DL fix in `branchOnClause` (commit `2beffd8`)
with branch-constraint antecedent encoding (commits `409180d`,
`dfcee98`, `a13c09f`, `c0c460c`). The negate arm now uses one
StackLevel + one BCP call (vs. k+1 of each pre-replacement).

### Invocation

```
./sharpSAT -rec -sep 5 -cb 3 -sepMode metis temp_cnf/mc2025_track1_049.cnf
```

### Result

| metric | `2beffd8` (per-decision-DL fix) | **branch-constraint (`c0c460c`)** | delta |
|---|---:|---:|---:|
| **wall time** | 354.64 s | **350.998 s** | **−1.0%** |
| decisions | 145 297 070 | 145 297 070 | 0 |
| L2 stores | 149 652 951 | 149 652 951 | 0 |
| L2 hits | 37 935 877 | 37 935 877 | 0 |
| L1 hits | 51 146 054 | 51 146 054 | 0 |
| total lookups | 97 199 139 | 97 199 139 | 0 |
| count | 8 695 763 196 077 742 | 8 695 763 196 077 742 | ✓ |

Trajectory is **bit-identical** — confirming the two soundness fixes
are functionally equivalent for cache topology; the only difference is
per-call cost.

### Soundness verification

Ran the configuration that originally exposed the multi-decision-DL UIP
bug: `t1_011 + -derivCacheBias 1 -rec -sep 5 -cb 3 -sepMode metis`.
Count: `536 870 912 306` (correct). Verified ALL of t1_065, t1_071,
t1_011 (default and +bias) before launching t1_049.

### Honest finding

The +25% t1_049 slowdown previously attributed to "the per-decision-DL
fix" is mis-attributed: the per-DL pushes contribute only ~1% to the
total cost. The remaining ~24% (354 s vs historical `892a4ea` 282 s)
likely comes from other changes between those commits — primarily the
provenance-based `learnedClauseSound` BFS check (default ON in
2beffd8), which fires at every learned-clause-in-scope filter during
BCP. With 97 M cache lookups, that path is hot enough that the BFS
walk dominates the wall-time differential. Future investigation:
profile the provenance check, look at memoization gaps or DAG sizes.

### Environment

- Commit: `c0c460c branch-constraint: rewrite branchOnClause negate arm`
- Build: `-O3 -DNDEBUG` (Release), CMake `BUILD_TYPE=Release`
- Input md5: `05173bb86a04414d86c661007d00accd` (`mc2025_track1_049.cnf`)
- Hardware: Apple Silicon, on AC

---

## 2026-05-24 — branchOnClause UIP soundness fix on t1_045: +7.9% wall-time at same trajectory

After landing today's per-decision-DL fix in `branchOnClause` negate arm
(commit `2beffd8`) + provenance-based learned-clause-soundness check
(default ON), re-ran t1_045 with the documented winning config to gauge
overhead vs the 2026-05-23 baseline.

### Invocation

```
SHARPSAT_PROGRESS=1 SHARPSAT_PROGRESS_INTERVAL=60 \
  /usr/bin/time -l ./sharpsat-separator/build/sharpSAT \
    -rec -sep 5 -cb 3 -adaptive -wlIter 2 -t 2700 \
    temp_cnf/mc2025_track1_045.cnf
```

### Result

| metric | 2026-05-23 baseline (`c0b2e73`) | **2026-05-24 (`2beffd8`)** | delta |
|---|---:|---:|---:|
| **wall time** | 2092.2 s (34.87 min) | **2257.48 s (37.62 min)** | **+7.9% / +2.75 min** |
| decisions | 682.1 M | 682.14 M | ≈0% |
| L2 stores | 680.4 M | 680.4 M | ≈0% |
| L2 hits | 226.7 M | 226.7 M | ≈0% |
| L2 hit rate | 33.3% | 33.3% | 0 |
| conflicts | (not recorded) | 26 591 | — |
| learned clauses | (not recorded) | 22 412 (4 244 dedup-dropped) | — |
| avg comp at entry | (not recorded) | 14.08 | — |
| avg / max L2 hit size | (not recorded) | 6.96 / 39 | — |
| peak RSS | (not recorded) | 14.02 GB | — |
| count | 132 951 278 067 432 | 132 951 278 067 432 | ✓ identical |

vs ganak (`--maxcache 24000` ≈ 24 GB): **1.23× faster** (ganak 46.2 min).

### Environment

- Commit: `2beffd8 branchOnClause: per-decision-DL fix for UIP soundness + provenance check`
- Build: `-O3 -DNDEBUG` (Release), CMake `BUILD_TYPE=Release`
- Input md5: `cb381ada22ec6d20d39504e7ccb7bebb` (`mc2025_track1_045.cnf`)
- Hardware: Apple Silicon, 10 P-cores + 4 E-cores
- Load avg at start: 1.78 / 1.79 / 1.81
- Background: VS Code at ~15-30% CPU (fluctuating), no other heavy contender
- `/usr/bin/time -l` user-time: 2272.40 s; sys: 6.90 s

### Interpretation

Decisions / L2 stores / L2 hits / hit rate are all **identical to the 2026-05-23
baseline within rounding**. The search tree is structurally unchanged — same
trajectory through the same set of cached sub-components. The 7.9% wall-time
slowdown is purely **per-operation overhead** from the fix:

- The per-decision-DL StackLevel pushes/pops + per-`¬lit` BCP runs in the
  negate arm of `branchOnClause` (mechanical cost).
- The provenance-based `learnedClauseSound` check via memoized BFS at each
  in-scope BCP filter call (mechanical cost).

Neither alters search trajectory or cache topology on this instance. Counts
match. See `docs/branchonclause_uip_soundness_proof.md` for the soundness
argument behind the fix.

A documented overhead reduction path exists: in the negate arm, the loop
currently pushes a `StackLevel` for every active `¬lit` it can set. For
`¬lit`s that BCP propagates from a prior decision, no new `StackLevel` is
needed since the lit is already on the trail with a clause antecedent. The
`if (!isActive(*it)) continue;` check already skips those at the loop, so
the only excess overhead is the marginal `StackLevel` push for the rare
`setLiteralIfFree` returning false (race). That's already minimal; the
bulk of the 7.9% is the per-decision-DL BCP-recall structure being
inherently more expensive than the old "set-all-then-BCP-once". Larger
overhead-reduction work (e.g., batching multiple ¬lit decisions while
giving each its own logical DL for UIP attribution) is possible but
non-trivial and deferred.



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
| 2026-04-24 10:48 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis` | /tmp/t1_065.cnf (md5 `44068991f8280094f30665351898ac1e`) | 0.0163 s | load avg 2.09; count `37778931862957161709568`. |
| 2026-04-24 10:48 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -reactiveMetis` | /tmp/t1_065.cnf (md5 `44068991f8280094f30665351898ac1e`) | 0.0159 s | load avg 2.09; same count. Reactive METIS has no measurable effect on this tiny instance (sub-20 ms either way). |
| 2026-04-24 10:48 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis` | /tmp/t1_071.cnf (md5 `e88123bdbf87205e36a681f2a3111e7c`) | 0.471 s | load avg 2.09; count `456295684783698132731653351484293780287639045166077370506304563500761788632102076272640`. |
| 2026-04-24 10:48 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -reactiveMetis` | /tmp/t1_071.cnf (md5 `e88123bdbf87205e36a681f2a3111e7c`) | 0.505 s | load avg 2.09; same count. Reactive METIS regresses by ~7% — consistent with the known pattern: on well-structured / sparse instances where the precomputed hierarchy already gives good separators, reactive adds per-fallback overhead without usable benefit. |
| 2026-04-24 10:59 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -adaptive` | /tmp/t1_049.cnf (md5 `05173bb86a04414d86c661007d00accd`, 90 vars / 252 clauses) | **TIMEOUT > 600 s** | load avg 3.94 at timeout; no count, no completion. Surprises relative to adaptive-branching-plan.md naming t1_049 as adaptive's primary target. On the full 90-var dense 3-SAT instance adaptive does not finish within 10 min on this system. The known-helpful cases for adaptive may be the shrunken `t1_049_k10_s*` variants (bench_D/bench_E era), not the original. |
| 2026-04-24 11:47 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -adaptive` | /tmp/t1_049_k40_s1.cnf (md5 `24cd5c4547b9cc0737a04e00bfc44d42`, 50v / 114c) | 0.000356 s | load avg 4.59; count=0 (UNSAT). |
| 2026-04-24 11:47 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -adaptive` | /tmp/t1_049_k30_s1.cnf (md5 `773b41c9ed919c2562d5cb3bb7882474`, 60v / 156c) | 0.000353 s | load avg 4.59; count=0 (UNSAT). |
| 2026-04-24 11:47 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -adaptive` | /tmp/t1_049_k25_s1.cnf (md5 `dc209272b575a2c9dd4afdc47bda4589`, 65v / 165c) | 0.001093 s | load avg 4.59; count=1328176. |
| 2026-04-24 11:47 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -adaptive` | /tmp/t1_049_k20_s1.cnf (md5 `d5cd80031576efe241feeca4e2a26ec0`, 70v / 171c) | 0.012825 s | load avg 4.59; count=6932091728. |
| 2026-04-24 11:47 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -adaptive` | /tmp/t1_049_k15_s1.cnf (md5 `82beb12677d0d1e1c5383b0719900343`, 75v / 198c) | 0.045579 s | load avg 4.59; count=9173373000. |
| 2026-04-24 11:47 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -adaptive` | /tmp/t1_049_k10_s1.cnf (md5 `be26dd3a4c92568bc6a26f94170db418`, 80v / 212c) | 4.1657 s | load avg 4.59; count=3643255795493. |
| 2026-04-24 12:07 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -adaptive` | /tmp/t1_049_k7_s1.cnf (md5 `7a224a3d2f1d625c0cf240b120c6dc70`, 83v / 223c) | 2.35458 s | load avg 2.55; count=2181140355365. Non-monotonic — smaller than k10 despite more vars; shrinker-seed-sensitive. |
| 2026-04-24 12:07 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -adaptive` | /tmp/t1_049_k6_s1.cnf (md5 `2723c06a009cb87a8a64f49e6a1d4581`, 84v / 223c) | **TIMEOUT > 30 s** | load avg 2.55; no count. Crosses the 10s threshold — stop climbing the variant ladder. |
| 2026-04-24 12:11 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | (no flags) | /tmp/t1_049_k10_s1.cnf (md5 `be26dd3a4c92568bc6a26f94170db418`, 80v / 212c) | 2.708 s | load avg 1.87. Flag-matrix run 1/6. count=3643255795493. |
| 2026-04-24 12:11 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-sep 5 -cb 3` | /tmp/t1_049_k10_s1.cnf | 2.666 s | load avg 1.87. Flag-matrix run 2/6. Same count. |
| 2026-04-24 12:11 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-sep 5 -cb 3 -reactiveMetis` | /tmp/t1_049_k10_s1.cnf | 2.670 s | load avg 1.87. Flag-matrix run 3/6. Reactive-METIS has no effect — noise-level. |
| 2026-04-24 12:11 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-sep 5 -cb 3 -adaptive` | /tmp/t1_049_k10_s1.cnf | 2.675 s | load avg 1.87. Flag-matrix run 4/6. Adaptive+sep is essentially no-op: adaptive probing is gated by `adaptive_probing_min_vars=60`, and hierarchy-leaf components are mostly below that. |
| 2026-04-24 12:11 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-sep 5 -cb 3 -adaptive -reactiveMetis` | /tmp/t1_049_k10_s1.cnf | 2.709 s | load avg 1.87. Flag-matrix run 5/6. Both add no measurable cost on top. |
| 2026-04-24 12:11 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-adaptive` | /tmp/t1_049_k10_s1.cnf | 4.043 s | load avg 2.22. Flag-matrix run 6/6. **Significant regression** vs the other five (~50%). Without `-sep`, adaptive fires on every component; probing cost (K=20 × 2 polarities × full BCP) is not recovered by the heuristic on this instance. |
| 2026-04-24 12:18 | `bf9c724` (HEAD) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | (no flags) | /tmp/t1_049_k6_s1.cnf (md5 `2723c06a009cb87a8a64f49e6a1d4581`, 84v / 223c) | 25.43 s | load avg 2.06 start, 2.14 end. No time limit. count=131,621,464,405,104. 8.1M decisions, 620 conflicts, 850k cache stores / 4.5M hits. Default completes well under the `-adaptive` 30s timeout this same instance hit — adaptive is decisively worse here. |

### Analysis: why adaptive is slower on t1_049_k10_s1 (commit `bf9c724`, 2026-04-24 12:21)

Three-way comparison designed to isolate the source of the slowdown:

| Variant | Flags | Time | Decisions | Conflicts | Cache stores |
|---|---|---|---|---|---|
| Default (raw-count picker) | (none) | 2.70 s | 926,992 | 150 | 275,812 |
| Adaptive, Stage-0 only (τ probing off) | `-adaptive -adaptiveMin 200` | 3.83 s | 1,289,820 | 88 | 415,844 |
| Adaptive, full | `-adaptive` | 4.02 s | 1,371,666 | 85 | 441,603 |

- Decision ratio (1.48×) ≈ time ratio (1.49×). The slowdown is explained by search-tree size.
- Disabling τ probing (row 2) shaves only 0.19 s off the 1.32 s gap. **Probing overhead is ~15% of the slowdown; the remaining 85% is Stage-0's branching choices.**
- Cache stores scale 1.60× — adaptive's branches fragment the problem into more distinct sub-components.
- Adaptive has FEWER conflicts (85 vs 150). Not a win — it means adaptive's branches don't force BCP cascades as aggressively, so the tree stays "open" longer before hitting dead-ends.

**Root cause**: Stage-0's length-weighted score (`cheap_score[v] = Σ 2^(-α·len(C))` with α=2) favours vars in binaries/ternaries. On t1_049-style dense 3-SAT those vars are already pinned by BCP on the first partner-assignment; branching on them adds little. The default raw-count picker favours vars in MANY clauses regardless of length, which correlates with "each assignment produces many 3→2 shortenings" — a better proxy for BCP-cascade potential at this density.

**Design implication**: Stage-0's α=2.0 was tuned on sparse instances like t1_071 (per the config comment at solver_config.h:78-88). It mis-ranks variables on dense regimes. Possible fixes (each its own experiment):
  - Lower α (e.g. 0.5) to reduce length discrimination on dense instances.
  - Analyzer-driven α: estimate formula density, pick α accordingly.
  - Add a "force-propagation" signal (like raw-count) as a secondary axis.

Not acted on in this session — logged for future work.

### α sweep on t1_049_k10_s1 (commit `<pending>`, 2026-04-24 12:30)

Added `-adaptiveAlpha <f>` CLI flag exposing `stage0_length_decay`
for experimentation. Validated the hypothesis: lowering α flips
adaptive from a regression to an improvement on this instance.

| Flags | α | Time | Decisions |
|---|---|---|---|
| (no flags, default picker) | — | 2.70 s | 926,992 |
| `-adaptive -adaptiveAlpha 2.0` | 2.0 (current default) | 4.05 s | 1,371,666 |
| `-adaptive -adaptiveAlpha 1.0` | 1.0 | 3.02 s | 1,034,702 |
| `-adaptive -adaptiveAlpha 0.5` | 0.5 | **2.47 s** | 854,374 |
| `-adaptive -adaptiveAlpha 0.25` | 0.25 | 2.49 s | 852,012 |

**Key finding**: with α=0.5, `-adaptive` beats the default picker
by ~9%. The 50% regression we measured earlier with the default
α=2.0 is entirely attributable to length-weighting being too
aggressive for this instance's density.

Decision count tracks monotonically with α — lower α = smaller
tree on this instance. Time follows decisions 1:1.

Design implication: α should be instance-adaptive, not hardcoded
at 2.0. An analyzer could set α based on cheap structural
statistics (mean clause length, clause-length variance, binary
fraction). Research follow-up, not in this commit.

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

---

## 2026-04-24 12:30 — `buildCanonicalKey` profile on t1_049_k10_s1 (commit `bf9c724` + instrumentation, reverted before commit)

Per-site wall-time breakdown over the whole solve with `-sep 5 -cb 3`:

| Site | Calls | Time | % of 2.8s |
|---|---|---|---|
| buildCanonicalKey | 952,796 | 1.37 s | **49%** |
| discoverComponentsOf | 926,842 | 0.62 s | **22%** |
| cache ops (peek + store) | 1,228,608 | 0.32 s | 11% |
| BCP | 927,308 | 0.07 s | 2.5% |
| picker | 463,496 | 0.016 s | 0.6% |
| probeLiteral | 0 | 0 | 0 |

Adding `-adaptive` does not change the breakdown materially — the
adaptive picker stays sub-1% of runtime (probing never fires because
sub-components stay below `adaptive_probing_min_vars=60`).

**Key finding**: the picker is not the bottleneck. `buildCanonicalKey`
alone is half the solve time; `discoverComponentsOf` another 22%.
Probing, BCP, and the picker itself are all sub-3%.

## 2026-04-24 12:35 — L1 (ID-based) repeat-hash probe

Hashed the sorted active-ClauseID + active-var-ID multiset at every
canonical-key build; counted how often the hash had been seen before
in the same run.

| Instance | Builds | Repeats | Repeat rate |
|---|---|---|---|
| t1_049_k10_s1 | 952,796 | 622,378 | **65.3%** |
| t1_011 | 424,339 | 316,687 | **74.6%** |

A two-level cache (L1 ID-based fast-path → L2 canonical-based
fallback) would skip the canonical-key build on those repeat hits.
Estimated savings: ~30% wall-time on both instances if the L1
lookup is O(1).

Measurement overhead for just the hash probe was ~3% on t1_049
(2.80 s → 2.88 s) and negligible on t1_011 — so the L1 lookup is
genuinely cheap. The two-level cache is worth implementing.

## 2026-04-24 13:10 — two-level cache (L1 ID-based + L2 canonical) landed

Implementation: 64-bit order-independent hash of `(active var IDs, active clause IDs)` as L1 key. Compute once per sub-component, check L1 before canonical-key build, populate on miss.

Measurements at HEAD `134b71f` + L1 patch:

| Instance | Pre-L1 | With L1 | Speedup | L1 hit rate |
|---|---|---|---|---|
| t1_049_k10_s1 (80v/212c) | 2.70 s | **2.47 s** | 8.5% | 56.9% (435k/765k) |
| t1_049_k6_s1 (84v/223c)  | 25.43 s | **25.19 s** | 1.0% | 58.6% (4.19M/7.15M) |
| t1_011 (6559v/14515c)    | 40.08 s | **31.49 s** | **21.4%** | 74.6% (316k/423k) |

All counts correct (matching pre-L1 runs and ganak oracle).

Speedup tracks how big a fraction `buildCanonicalKey` was of the original solve. On t1_011, where canonical-build is a huge fraction (big cache, many re-visits), L1 is a 21% win. On t1_049_k6_s1 where the search is dominated by other work, L1 still hits at 59% but the wall-time payoff is only 1%.

Regression tests pass:
  - test_canonical_key_invariance ✓
  - test_canonical_key_learned ✓

## 2026-04-24 14:14 — k6_s1 L1 speedup re-measured (3 runs each)

Follow-up to the single-measurement reading that suggested k6_s1
got only 1% from L1. Ran 3× at both commits under comparable load.

Pre-L1  (134b71f): 25.68 / 25.28 / 25.47 s   → mean 25.48 ± 0.2 s  (load 3.7)
With L1 (b5ebaf2): 24.79 / 24.99 / 24.82 s   → mean 24.87 ± 0.1 s  (load 2.9)

Actual speedup: **2-3%** (depending on load correction). The
earlier single-datapoint 1% was noise, not a pathological case.
L1's benefit on k6_s1 is smaller than on t1_011 (21%) because
k6_s1's search tree is deeper and its average canonical-build cost
is smaller — L1's per-call overhead is a larger fraction of what
it saves. But still a genuine positive.

## 2026-04-24 14:31 — stashed L1 hash in Component struct

Moved L1 hash computation from per-lookup in solver_rec.cpp into
makeComponentFromState, which already iterates the same vars and
clauses to construct the Component. Added `l1_hash_` field to
Component.

3-run means on HEAD with this patch, load ~3-4:

  t1_049_k10_s1  2.43 s ± 0.03   (was 2.47 in-place)
  t1_049_k6_s1   24.78 s ± 0.2   (was 24.87 in-place)
  t1_011         31.46 s ± 0.03  (was 31.49 in-place)

Marginal improvement, much smaller than predicted. The in-place
hash was already iterating cache-hot contiguous memory in the
Component struct — re-scanning was ~50-100 ns per lookup, not
the 300+ ns I estimated. Locality eats most of the apparent cost.

Still correctness-neutral and structurally cleaner (hash lives
with the object it describes). Both regression tests pass.

## 2026-04-26 16:24 — canonical-key cascade landed (iter-1 dynamic WL → static WL combine → raw-id fallback)

Fix for the t1_011 / super_d3_id8 order-dependent miscount class. Replaces the heuristic var_idx tie-break for collision-block vars with a sound 4-step cascade: iter-1 dynamic WL (always) → iter 2..K dynamic WL (gated on collisions remaining AND iter ≤ K, K = wl_iterations) → static-WL-label combine for residual collision-block vars (gated only on collisions, K-independent) → raw-var-id + RESIDUAL_OFFSET identity fallback with original polarity (gated only on collisions). Default `wl_iterations = 1`. Static labels precomputed once at preprocessing finish via `computeStaticWLLabels`.

Verified counts against ganak ground truth where measured. All test instances retain (or restore) correctness; t1_049 (full) which previously hit the >600 s timeout under `-adaptive` now completes correctly under default cascade flags.

| Timestamp | Commit | Compile flags | Solver flags | Instance | Time | Env / notes |
|---|---|---|---|---|---|---|
| 2026-04-26 16:24 | `5788427` (dirty: cascade WIP) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3` | /tmp/t1_011.cnf (md5 `4be5e40e8a1660b130a690981ebdea88`) | 13.71 s | load avg 1.72; cascade auto-fires; count = `536870912306` (matches ganak 111.6 s; 8× faster). |
| 2026-04-26 16:24 | `5788427` (dirty: cascade WIP) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -permWatchIndep 7 -permWatchSelect 0x10` | /tmp/t1_011.cnf (md5 `4be5e40e8a1660b130a690981ebdea88`) | 15.65 s | load avg 1.72; original perm bug trigger now correct under cascade; count = `536870912306` (matches ganak). |
| 2026-04-26 16:24 | `5788427` (dirty: cascade WIP) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3` | /tmp/t1_065.cnf (md5 `44068991f8280094f30665351898ac1e`) | 0.15 s | load avg 2.65; count = `37778931862957161709568` (matches historical 0.0163 s entry); ~9× wall-time slowdown vs hist on this sub-20ms instance attributable to cascade overhead floor (470 cascade calls, 85% with collisions). Absolute cost ≈ 135 ms — negligible at any meaningful instance scale. |
| 2026-04-26 16:24 | `5788427` (dirty: cascade WIP) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3` | /tmp/t1_071.cnf (md5 `e88123bdbf87205e36a681f2a3111e7c`) | 0.13 s | load avg 2.65; count = `456295684783698132731653351484293780287639045166077370506304563500761788632102076272640` (matches hist); 3.6× FASTER than hist (0.471 s). 17,238 cascade calls, only 25% with collisions, max block 7 — cascade mostly skipped, speedup likely from unrelated improvements landed in the interim. |
| 2026-04-26 16:24 | `5788427` (dirty: cascade WIP) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3` | /tmp/t1_049.cnf (md5 `05173bb86a04414d86c661007d00accd`) | 325.94 s | load avg 2.65; count = `8695763196077742` (matches ganak 386.77 s; 16% faster). Previously TIMED OUT > 600 s with `-adaptive` (2026-04-24 10:59 row). Default cascade now completes. 94.9 M cascade calls, 40% with collisions, max block 16. |
| 2026-04-26 16:24 | `5788427` (dirty: cascade WIP) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -permWatchIndep 7 -permWatchSelect 0x10` | /tmp/dump_bl23/super_d3_id8.cnf (sub-component dump from t1_011 chain; 2040v/11637c) | 2.09 s | load avg 1.72; count = `26843545568` (matches ganak); without the cascade was `26575110112` (off by exactly −2^28). The original perm reproducer for this entire investigation. |

---

## 2026-04-26 18:05 — per-component learned-clause BCP filter + reactive-METIS originals-only

Two related changes that close the cross-sub-component propagation gap discussed in conversation:

1. **Per-component BCP filter for learned clauses.** When solveComponent's decomposition step splits the parent into multiple sub-components (or peels isolated vars), we install a stack-allocated `SubVarsetGuard` around each recursive `solveComponent(*sub)` call. While the guard is alive, BCP rejects any learned clause that has a variable outside the current sub's `varsBegin` AND that variable is still `X_TRI` (i.e., active in a sibling sub). Learned clauses whose outside vars are already assigned (`T_TRI`/`F_TRI`) by parent decisions are KEPT — their lits are determined and BCP handles satisfaction/falsification soundly. Branching paths (separator/clause/lit) recurse with the same `comp.varsBegin`, so they don't push another guard — the filter state set by the decomposition descent persists through them. When `discoverComponentsOf` returns a single sub with `trivial_factor == 1` (no decomposition), the guard is skipped entirely. Diff-tracked O(|parent ∖ child| + |child ∖ parent|) maintenance with a sorted-merge of var lists; never iterates the full bitmap. Filter check in BCP walks the clause body directly (no separate per-clause var-set storage).

2. **Reactive METIS uses original clauses only.** `buildMetisInputFromComponent` now skips learned clauses entirely. Including them gave METIS spurious connectivity from search-history artifacts that don't reflect the formula's intrinsic structure. The new per-component BCP filter would drop bridge learned clauses post-decomposition anyway, so feeding them to METIS only hurt separator quality.

Both changes are always-on (no flag).

| Timestamp | Commit | Compile flags | Solver flags | Instance | Time | Env / notes |
|---|---|---|---|---|---|---|
| 2026-04-26 18:05 | `8c8dfe6` (dirty: filter + reactive-METIS) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3` | /tmp/t1_065.cnf (md5 `44068991f8280094f30665351898ac1e`) | 0.01 s | load avg ~2.0; 5 consecutive runs all 0.01s; count `37778931862957161709568`; matches historical 0.0163s and post-cascade 0.15s. |
| 2026-04-26 18:05 | `8c8dfe6` (dirty: filter + reactive-METIS) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3` | /tmp/t1_071.cnf (md5 `e88123bdbf87205e36a681f2a3111e7c`) | 0.13 s | load avg ~2.0; count `456295684783698132731653351484293780287639045166077370506304563500761788632102076272640`; identical to post-cascade 0.13s. |
| 2026-04-26 18:05 | `8c8dfe6` (dirty: filter + reactive-METIS) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3` | /tmp/t1_011.cnf (md5 `4be5e40e8a1660b130a690981ebdea88`) | 14.0–14.3 s (3 runs) | load avg 2.17; count `536870912306`; +3% vs cascade-only 13.71s. Decisions 226,066 vs cascade-only 203,330 (+11%). Per-decision cost actually lower (BCP does less work because filtered learned clauses no longer propagate cross-sub) but the search tree is 11% larger for the same reason. Net +3% wall-time. |
| 2026-04-26 18:05 | `8c8dfe6` (dirty: filter + reactive-METIS) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -permWatchIndep 7 -permWatchSelect 0x10` | /tmp/t1_011.cnf (md5 `4be5e40e8a1660b130a690981ebdea88`) | 16.46 s | load avg ~2.0; count `536870912306`; +5% vs cascade-only 15.65s. Decisions 269,528. |
| 2026-04-26 18:05 | `8c8dfe6` (dirty: filter + reactive-METIS) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3` | /tmp/t1_049.cnf (md5 `05173bb86a04414d86c661007d00accd`) | 331.22 s | load avg 1.55; count `8695763196077742` (matches ganak 386.77s; 14% faster than ganak). +1.6% vs cascade-only 325.94s. 171M decisions. |
| 2026-04-26 18:05 | `8c8dfe6` (dirty: filter + reactive-METIS) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -permWatchIndep 7 -permWatchSelect 0x10` | /tmp/dump_bl23/super_d3_id8.cnf | 1.85 s | load avg ~2.0; count `26843545568`; **−11% vs cascade-only 2.09s** (faster). Filter likely shrunk the cache enough to reduce per-call canonical-key work. |

**Earlier (over-aggressive) version** of the filter — rejecting any learned clause with a var outside the current sub's `varsBegin`, regardless of whether that var was assigned — was measured at +28% on t1_011 default (17.3–17.9s, 3 runs) with decisions ballooning to 301,096 (+48%). Relaxing to "only block when the outside var is X_TRI" (which is the real cross-sub propagation hazard; assigned outside vars don't propagate via the clause) recovered most of the slowdown. The relaxed version is what landed.

---

## 2026-04-26 20:30 — replaced `static thread_local` with `static` (single-threaded only)

Profile-driven optimization. macOS `sample` profile of t1_049 (76,040 1-ms samples over 90 s) showed `_tlv_get_addr` (3,355 samples) + `__tls_init` (5,254 samples) accounted for ~11% of CPU — TLS access overhead from `static thread_local` buffers in hot functions, mainly the 9 scratch vectors in `canonical_key.cpp::buildCanonicalKey` (called 95M+ times on t1_049).

Each access to a `thread_local` variable on macOS goes through `_tlv_get_addr` (~5-10 ns) instead of compiling to a direct load. For C++ vectors with non-trivial constructors the TLS path emits multiple indirections, amplifying the cost.

Replaced all 16 `static thread_local` declarations across `canonical_key.cpp` (9), `instance.h::maybeDedupClause` (2), and `solver.cpp::analyzeDynamicSubsumption` (5) with plain `static`. Solver is single-threaded; if that ever changes (e.g., per-thread `Counter` à la Ganak's `OuterCounter`), these MUST be reverted to `thread_local` to avoid data races. Comments at each site flag this.

Counts identical across the suite (decision counts bit-identical where checked) → pure overhead reduction, zero algorithmic side-effect.

| Timestamp | Commit | Compile flags | Solver flags | Instance | Time | Env / notes |
|---|---|---|---|---|---|---|
| 2026-04-26 20:30 | `892a4ea` (dirty: TLS→static) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3` | /tmp/t1_065.cnf (md5 `44068991f8280094f30665351898ac1e`) | 0.01 s | load avg ~2.0; count `37778931862957161709568`; unchanged. |
| 2026-04-26 20:30 | `892a4ea` (dirty: TLS→static) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3` | /tmp/t1_071.cnf (md5 `e88123bdbf87205e36a681f2a3111e7c`) | 0.12 s | load avg ~2.0; count matches; within noise. |
| 2026-04-26 20:30 | `892a4ea` (dirty: TLS→static) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3` | /tmp/t1_011.cnf (md5 `4be5e40e8a1660b130a690981ebdea88`) | 8.71-8.83 s (3 runs, mean 8.79 s) | load avg 2.05; count `536870912306`; **−38% vs 14.0-14.3 s prior**. Decisions 226,066 — bit-identical to prior, confirms pure overhead removal. |
| 2026-04-26 20:30 | `892a4ea` (dirty: TLS→static) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -permWatchIndep 7 -permWatchSelect 0x10` | /tmp/t1_011.cnf | 10.39 s | count `536870912306`; **−37% vs prior 16.46 s**. |
| 2026-04-26 20:30 | `892a4ea` (dirty: TLS→static) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3` | /tmp/t1_049.cnf (md5 `05173bb86a04414d86c661007d00accd`) | 282.62 s | load avg 1.82; count `8695763196077742` (matches ganak 386.77 s; now **27% faster than ganak**); **−15% vs prior 331.22 s**. Decisions 171,479,716 — identical. |
| 2026-04-26 20:30 | `892a4ea` (dirty: TLS→static) | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -permWatchIndep 7 -permWatchSelect 0x10` | /tmp/dump_bl23/super_d3_id8.cnf | 1.37 s | count `26843545568`; **−26% vs prior 1.85 s**. |

The `canonical_key.cpp` TLS removal alone captured essentially all the benefit; the additional 7 sites (`maybeDedupClause`, `analyzeDynamicSubsumption`) were correctness-neutral but didn't move the needle on the measured instances — `maybeDedupClause` is per-conflict (~20k calls on t1_011) so its TLS overhead is too small to see, and `analyzeDynamicSubsumption` is opt-in (off by default).

---

## 2026-04-24 16:00 — L1 hash widened to 128-bit (collision-safe)

Replaced 64-bit IdKey with two-field 128-bit IdKey (hash_lo,
hash_hi). Both halves computed in the same iteration pass at
Component construction using independent mixers. Equality compares
both halves.

Collision probability at 10^9 entries: ~10^-20 (functionally zero).
The original 64-bit design had ~10^-8 per-run at 10^6 entries —
small but non-zero.

3-run means at HEAD:

  t1_049_k10_s1  2.43 → 2.46 s   (+0.03, noise)
  t1_049_k6_s1   24.78 → 24.95 s (+0.17, noise)
  t1_011         31.46 → 31.37 s (−0.09, noise)

No measurable regression. Extra ~150 ns per Component construction
amortized to < 1 s on the hardest instance; per-lookup cost
unchanged.

Both regression tests pass. All counts match.

---

## 2026-04-29 — Unified picker (`-unifiedPicker`) requires `-decomposeAfterK 1000`

Under `-unifiedPicker`, the picker bypasses the separator-consumption block; the carried `separator` argument is the immutable hint (VARs+CLAUSEs together) that the picker reads to apply the separator-bias bonus during scoring. The picker never shrinks `separator` — it's the picker's "what's structurally important here" reference, consumed implicitly as elements become inactive (BCP-set vars, removed/satisfied clauses).

The mid-consumption decompose-block (`config.decompose_in_separator || config.unified_picker`) fires every `decompose_after_k` BRANCHING DECISIONS when `separator` is non-empty. Under the unified picker `separator` is essentially ALWAYS non-empty (it's the immutable hint), so the gate fires every `decompose_after_k` decisions throughout the entire search.

With `decompose_after_k = 6` default, this aggressive firing **interferes with the picker's ND-hierarchy descent**:

1. The picker's natural mode is "branch on this nd_node's separator → all 4 elements get consumed via picks → post-consumption decompose-block fires (`sep_exhausted = true`) → splits cleanly into L/R sub-comps mapping to clean child nd_nodes → recurse with separator_reset=true → next nd_node's acceptance fires."
2. Mid-consumption decompose firing **before** all 4 separator elements are picked breaks this. `discoverComponentsOf` runs with the formula still partially connected via unbranched separator elements; sub-comps span both children → mapToChild returns -2 → softened to -1 → recurses without hierarchy info, so the next acceptance can't find a precomputed separator at the right node. The sub-tree degenerates to score-only (freq+activity) branching, search-tree size explodes.

**Concrete observation on t1_071** (640v / 1818c):

| Config | decisions | conflicts | wall |
|---|---|---|---|
| baseline `-rec -sep 5 -cb 3 -sepMode metis` | 62K | 5,094 | 0.63 s |
| picker, default k=6 | — | — | **TIMEOUT > 60 s** |
| picker, `-decomposeAfterK 1000` (effectively off) | 286K | 33,271 | 1.29 s |
| picker, `-decomposeAfterK 1000 -sepPositionW 100` | 314K | 35,831 | 1.39 s |

So:
- `decompose_after_k = 6` makes the picker un-usable (TIMEOUT).
- `decompose_after_k = 1000` (effectively disable) brings the picker to ~2× baseline on t1_071 — acceptable for "general score-driven branching" tradeoff.
- The position-bonus knob (`-sepPositionW`) was tested and removed: it doesn't help on t1_071 (slightly worse), and the design preference is to keep score-driven separator selection without forcing METIS list order.

**Open question**: the mid-consumption decompose throttle was originally intended to **help** by detecting BCP-induced disconnects between branchings. Under the unified picker its effect is reversed because the picker keeps `separator` non-empty. A cleaner future design would gate the mid-consumption decompose specifically on "BCP just made significant structural changes" rather than firing every k decisions when the carried separator happens to be non-empty. For now, recommended invocation under the picker:

```
-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -clauseScoreW 100 -sepBiasW 1000 -sepImpA 1.0 -decomposeAfterK 1000
```

| Timestamp | Commit | Compile flags | Solver flags | Instance | Time | Env / notes |
|---|---|---|---|---|---|---|
| 2026-04-29 00:18 | `<dirty: position-bonus removed>` | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis` | /tmp/t1_071.cnf (md5 `e88123bdbf87205e36a681f2a3111e7c`) | 0.63 s | on battery; baseline reference. count `4562956...076272640`. Decisions 62,046; conflicts 5,094. |
| 2026-04-29 00:18 | `<dirty>` | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -clauseScoreW 100 -sepBiasW 1000 -sepImpA 1.0 -decomposeAfterK 1000` | /tmp/t1_071.cnf | 1.29 s | on battery; picker ~2× baseline. Decisions 286,434; conflicts 33,271 (~6× baseline due to learning enabled in picker var-branches). |
| 2026-04-29 00:18 | `<dirty>` | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | (same picker flags, NO `-decomposeAfterK`) | /tmp/t1_071.cnf | TIMEOUT > 60 s | on battery; default `decompose_after_k=6` interferes with picker's hierarchy descent. |
| 2026-04-29 01:11 | `fdd0893` | `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64` | `-rec -sep 5 -cb 3 -sepMode metis -sepVarBias` | /tmp/t1_021_k7_s1.cnf | **20.81 s** | on battery; count `2978382486464633072`; decisions 5,582,534. With `-sepVarBias` (no `-unifiedPicker`), separator VARs strip into the bias bitmap and the legacy `pickBranchVariable` boosts them. Baseline (no `-sepVarBias`) historically TIMEd out at 60s on this instance — this is the qualitative "TIMEOUT → finishes" win documented in conversation but not previously logged. |

---

## 2026-05-03 23:06 — Ganak: `--td 0` is dramatically faster than default on t1_011

Fresh ganak build from upstream `main` at commit `8614351e868ba5241e030ecb2494639efa2d0881` (FetchContent of cryptominisat5, cadical, cadiback, arjun, approxmc, sbva, treedecomp at their `master`/`main` tips). Built static-release: `cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF` + `cmake --build . --target ganak-bin -j8`. Apple Clang 21.0.0, arm64-darwin. Note: deps' baked-in `-O2` (CryptoMiniSat) and `-fno-omit-frame-pointer` (CaDiCaL) currently override the parent `-O3` due to last-wins flag order — not yet patched.

Default invocation (`--td 1`, the default — flowcutter with `--tditers 900 --tdsteps 100000`) was killed at >9 min CPU time on t1_011. Disabling tree decomposition with `--td 0` finishes the same instance in **25.19 s** (count correct).

Implication: on t1_011 today, FlowCutter-based tree decomposition is the bottleneck — not search. The historical "ganak 111.6 s" reference in the 2026-04-26 cascade-landing row was likely captured against a different `treedecomp` upstream commit; we can't reproduce it because FetchContent pulls latest at configure time.

| Timestamp | Solver | Solver flags | Instance | Wall | Notes |
|---|---|---|---|---|---|
| 2026-05-03 23:06 | ganak `8614351` (fresh static-release) | `--verb 0 --td 0` | /Users/konstantin.kutzkov/Desktop/Code/SharpSAT/temp_cnf/mc2025_track1_011.cnf (md5 `4be5e40e8a1660b130a690981ebdea88`) | **25.19 s** (Total time Arjun+GANAK; real 25.35 s) | load avg 1.82 → 1.68 (cpptools suspended for the run); count `536870912306`; matches log; same md5 as the historical `/tmp/t1_011.cnf` row. |
| 2026-05-03 22:30 | ganak `8614351` (same build) | `--verb 0` (default `--td 1`) | same | **>9 min, killed** | load avg ~2.5; ganak running at 99% CPU, not stuck — TD computation eating the wall time. |
| 2026-05-03 23:10 | sharpSAT `fdd0893` (existing build) | `-rec -sep 5 -cb 3 -sepMode metis` | same | **12.72 s** | load avg 1.64 → 2.08 (cpptools suspended, same machine state as the ganak rows above); count `536870912306`; decisions 215,018. **~2× faster than ganak `--td 0`** on this instance under identical conditions. Note: today's 12.72 s is +45% over the log's 8.79 s for this instance under commit `892a4ea` (2026-04-26 20:30, line 339) — picker-related code paths landed at `fdd0893` may carry residual overhead even when the picker is off. |

---

## 2026-05-04 00:30 — t1_021 shrink-ladder probe to find a "ganak in [20s, 60s]" working instance

Goal: find a shrunken variant of t1_021 such that ganak (default or `--td 0`) finishes in 20–60 s — useful as a moderately-hard benchmark for measuring picker / sharpSAT improvements without burning minutes per probe.

Strategy: start fast (heavily-frozen, k=20) and unfreeze (smaller k = fewer frozen vars = harder) until the wall hits the band. Cheaper than starting full and shrinking.

CNF generation: `python3 sharpsat-separator/tests/shrink_cnf.py temp_cnf/mc2025_track1_021.cnf temp_cnf/mc2025_track1_021_k<K>_s1.cnf <K> --seed 1`. All variants persistent in `temp_cnf/`. Original `mc2025_track1_021.cnf` md5 unchanged across both originals, here as the parent.

Build: ganak `8614351` (fresh static-release, deps `-O2` not yet patched), sharpSAT `fdd0893`. cpptools suspended; load avg ~1.7 throughout.

| Variant | n / m | ganak default `--td 1` | ganak `--td 0` | sharpSAT `-rec -sep 5 -cb 3 -sepMode metis -sepVarBias` | Count |
|---|---|---|---|---|---|
| `t1_021_k20_s1` | 70v / 62c | — | 0.03 s | — | `275877611080320` |
| `t1_021_k15_s1` | 74v / 69c | — | 0.85 s | — | `3131285320468328` |
| `t1_021_k10_s1` | 80v / 75c | — | 9.43 s | — | `430052389882336036` |
| **`t1_021_k7_s1`** | **83v / 90c** | **9.48 s** | **38.58 s** | 20.81 s (log line 409, 2026-04-29) | `2978382486464633072` |

**Selected working instance: `temp_cnf/mc2025_track1_021_k7_s1.cnf`** (md5 from `shrink_cnf.py` --seed=1). Frozen vars: `x18=0, x73=1, x9=0, x33=1, x16=0, x64=0, x58=0`. Ganak `--td 0` lands at 38.58 s — in target band.

Observations:
- TD helps decisively when the formula has small separators: ganak default 9.48 s vs `--td 0` 38.58 s on k7_s1 (TD 4× speedup). On t1_011 — which lacks small separators — TD blew up at >9 min. The difference is structural (separator/treewidth), not the size of the formula.
- ganak `--td 0` time scales rapidly with k: 0.03 → 0.85 → 9.43 → 38.58 s as k goes 20 → 15 → 10 → 7. Roughly 4× per step.
- sharpSAT's 20.81 s on k7_s1 (with `-sepVarBias`) lands between ganak's two configs — slower than ganak default's 9.48 s, faster than ganak `--td 0`'s 38.58 s. Comparable-magnitude search.

---

## 2026-05-05 — Multiplicative-mode unified picker (Phase 1) on t1_011 / k15

Phase 1 of the unified-picker redesign documented in
[unified_picker_redesign.md](unified_picker_redesign.md). The
multiplicative score combines a type-pure `base(x)` with a smooth
multiplicative separator boost:

```
base(v)  = picker_var_weight · max(picker_base_floor,
                                   freq + 10·activity + cheapW·cheap)
base(C)  = picker_clause_weight · sigmoid(β·(L − mid))
boost(x) = 1 + picker_alpha · exp(−picker_lambda · rel_k) · 1[x ∈ sep]
S(x)     = base(x) · boost(x)
rel_k    = (active sep elements) / N_active_vars
```

with cascade boost gated behind `picker_gamma > 0` (Regime B,
empirically a no-op on these instances; see below).

**Default Regime A parameters** (calibrated to match the typical
var-base magnitude `freq + 10·activity ≈ 3–100` against the
sigmoid-clamped clause-base `(0,1)`):

| Symbol | Code field | Default | CLI flag |
|---|---|---|---|
| α | `picker_alpha` | **15.0** | `-pickerAlpha` |
| λ | `picker_lambda` | **5.0** | `-pickerLambda` |
| γ | `picker_gamma` | **0.0** (Regime A) | `-pickerGamma` |
| w_v | `picker_var_weight` | **1.0** | `-pickerVarW` |
| w_C | `picker_clause_weight` | **7.0** | `-pickerClauseW` |
| ε | `picker_base_floor` | **0.01** | (no CLI) |

The `clauseW = 7` default makes a length-4 clause base
(σ(1) ≈ 0.731 → base ≈ 5) comparable to a typical var base
`freq + 10·activity ≈ 5`. The `α = 15` default gives a separator
candidate at `rel_k = 0` a 16× score multiplier — strong enough to
match legacy `-sepVarBias`'s near-strict "consume separator first"
preference. An earlier draft of the design doc suggested
`clauseW ∈ [0.3, 3.0]`; that range is two orders of magnitude too
small relative to real var-base magnitudes.

**Build**: HEAD `e9e8bd1` (Phase 1 multiplicative picker landed),
`-O3 -DNDEBUG -std=c++11 -Wall -arch arm64`. cpptools suspended
throughout. `decomposeAfterK 1000` matches the picker's recommended
invocation (see 2026-04-29 row).

| Timestamp | Solver flags | Instance | Wall | Decisions | Conflicts | Stores | Notes |
|---|---|---|---|---|---|---|---|
| 2026-05-05 19:31 | `-rec -sep 5 -cb 3 -sepMode metis -sepVarBias` | t1_011.cnf (md5 `4be5e40e8a1660b130a690981ebdea88`) | 12.6 s | — | 17,447 | 131,239 | Today's legacy reference at HEAD `e9e8bd1`; +44% over historical 8.79 s (commit `892a4ea`, 2026-04-26 row 339). |
| 2026-05-05 19:31 | `-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000` | t1_011.cnf | **TIMEOUT > 60 s** | — | — | — | Default additive unified picker fails to finish within 60s. |
| 2026-05-05 19:31 | `-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000 -pickerMode multiplicative` | t1_011.cnf | **14.96 s** | 215,018 | 17,447 | 131,239 | Multiplicative Regime A with default α=15, λ=5, γ=0, w_v=1, w_C=7. **Tree size matches legacy exactly** (same decisions, conflicts, stores). +19% wall vs today's legacy (12.6 s) — overhead from per-pick cheap-score / sep-set lookups, not from worse picks. **Multiplicative finishes where additive times out.** |
| 2026-05-05 19:31 | (above) + `-pickerGamma 1` | t1_011.cnf | 21.37 s | 215,018 | 17,447 | 131,239 | Regime B (γ=1). Decision sequence bit-identical to Regime A — the cascade boost has zero effect because the median cascade depth across active vars is 0 (most vars don't have any forced lits in their `binary_links_` walk, and the `depth_med > 0` gate blocks the boost). +43% wall over Regime A is pure per-call cost from `computeCascadeScore` evaluated on every active var per pickBranchTarget call. Confirms the design doc's prediction that Regime B without sampled-median + lazy-skip is too expensive. |
| 2026-05-05 19:05 | `-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000` | t1_021_k15_s1.cnf (shrunken from t1_021 with 15 vars frozen, seed 1; see 2026-05-04 ladder section) | 0.60 s | 215,544 | 67 | 114,052 | Additive picker reference on the smaller k15 instance. |
| 2026-05-05 19:05 | (above) + `-pickerMode multiplicative` | t1_021_k15_s1.cnf | 0.78 s | 278,380 | 78 | 142,689 | Multiplicative Regime A. +29% decisions / +30% wall vs additive on this instance — within calibration-tunable range. |

**Headline finding**: on `t1_011`, the multiplicative-mode picker
finishes in 14.96 s while the **additive-mode picker times out
> 60 s**. Multiplicative reproduces the legacy's tree exactly
(decisions, conflicts, stores all bit-identical), with a +19% wall
overhead from picker per-call work. This is the first concrete
signal that the multiplicative redesign behaves better than the
additive one on a hard instance, not just differently.

**On k15** (smaller instance), the additive picker still works
(0.60 s) and multiplicative is slightly slower (+30%) — calibration
trade-off, expected to be tunable by sweeping α, λ, w_C.

**Regime B (γ > 0) is a no-op as currently designed** because the
cascade-depth median across active vars is 0 on every instance
tested (k15, k7_s1, t1_011), even on t1_011 with 17k learned
binaries. The `depth_med > 0` gate consequently blocks the boost
on every candidate. The median-relative cascade design needs
re-thinking before it can produce signal: either drop the gate,
use percentile-based ranking instead, or change the depth-counting
formula to surface the rare non-zero values.

**Open**: parameter sensitivity sweep across α, λ, w_C on t1_011
(can the multiplicative picker close the +19% wall gap to legacy?)
and a fix for Regime B's degenerate median (otherwise γ has no
effect we can measure).

---

## 2026-05-09 — Hybrid picker (mult during sep + τ post-sep) and α_var sensitivity on t1_071

Picker change under investigation in this session: the `-pickerRateFramework` (rate / τ-based) score is replaced by a regime-split scoring inside `pickBranchTargetRate`:

- **Sep-active regime** (`n_sep_active > 0`): mult-style score `raw · (1 + α·exp(−λ·rel_k))` for vars and `clauseW · sig · (1 + α_c·exp(−λ_c·rel_k))` for clauses. This is the same formula `pickBranchTargetMultiplicative` uses.
- **Sep-exhausted regime** (`n_sep_active == 0`): rate-style `−n_active_vars · log(τ)` where τ = `tauBranchingNumber(a, b)` for vars and `tauBranchingNumber(sig, L)` for clauses. Cascade gain from `computeBcpGainPolarities` always on (no opt-out flag in this configuration).

Build: HEAD with the regime-split patch in `solver_rec.cpp` (uncommitted, "dirty: regime-split picker"). Compile flags: `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64`. Default build present at `sharpsat-separator/build/sharpSAT`. Three parameters held fixed throughout this section: `clause_branch_min_length = 3` (via `-cb 3`), `cascade_score_depth = 3` (compile-time default), `picker_no_cascade_gain = false` (cascade always on).

CLI invocation under investigation:
```
-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -pickerMode multiplicative -pickerRateFramework -pickerNonSepKillsNd
```

### Results on three structurally different instances

Reference numbers were captured under load avg ~3.5–4.5 (VS Code + Claude Helper + WindowServer competing). Absolute timings inflated vs the historical clean-machine numbers earlier in this log; relative comparisons within today's session are valid.

| Instance | Plain (`-rec -sep 5 -cb 3 -sepMode metis`) | Default rate (untuned) | Hybrid (Design G) default | Notes |
|---|---|---|---|---|
| `mc2025_track1_065.cnf` | 0.01 s (hist) | 0.02 s | 0.02 s | Trivially fast; picker overhead dominates. No tuning needed. |
| `mc2025_track1_021_k10_s1.cnf` (80v / 75c) | ~0.4 s (hist) | TIMEOUT > 60 s | **12.7 s** (clean) / 32 s (loaded) | Pure rate framework times out; hybrid solves. Mult-only solves in ~9.6 s on clean machine. |
| `mc2025_track1_071.cnf` (640v / 1818c) | 0.43 s | 6.93 s | 6.93 s default (α_v=15) → **1.05 s** with α_v=100 | Big sensitivity to α_var (see sweep below). |

`mc2025_track1_011.cnf`: hybrid TIMEOUT > 60 s in this session (vs legacy `-sepVarBias` at ~14 s). Open question — held for future investigation.

### α_var sweep on t1_071 (15 s timeout per run)

All other parameters at defaults (α_clause=110, λ_var=5, λ_clause=5, varW=1, clauseW=1.5, frontBonus=2). Decision count reported alongside wall time.

| α_var | Wall time | Decisions |
|---|---|---|
| 1 | TIMEOUT > 15 s | — |
| 5 | TIMEOUT > 15 s | — |
| 15 (default) | 6.93 s | 308,904 |
| 30 | 7.77 s | 332,628 |
| **50** | **1.14 s** | **64,000** |
| **100** | **1.05 s** | **56,372** |
| 200 | 1.15 s | 64,384 |
| 500 | 1.11 s | 59,564 |
| 1000 | 1.14 s | 59,564 (saturated) |

Single-axis sweeps on the other parameters (one at a time, others at default) on t1_071:

| Parameter | Best non-default | Time at best | Time at default |
|---|---|---|---|
| `α_clause` | no movement (30..600 all ~7 s) | — | 6.94 s |
| `λ_var` | 10 → 3.87 s | 3.87 s | 6.98 s |
| `λ_clause` | no movement | — | 6.95 s |
| `picker_clause_weight` | 5.0 → 2.74 s | 2.74 s | 6.99 s |
| `picker_front_bonus` | no movement | — | 6.93 s |

Combinations of α_v=100 with the other improved values (λ_v=10 and/or w_c=5) did not beat α_v=100 alone on t1_071 (1.05 s remained the best). Saturation at α_v=50–100 is sharp: below that threshold the picker scatters non-sep picks into the sep-active regime; above that, sep is consumed first and the search shrinks ~5.5×.

At the best setting (α_v=100), Design (G) on t1_071 produces 56,372 decisions vs plain's 62,046 — fewer decisions than the plain configuration, with picker overhead accounting for the residual 1.05 s vs 0.43 s wall-time gap.

### Observations and tentative hypotheses

These are observations from this single session, on three instances. They have not been verified at scale.

1. **t1_071 (sparse, well-structured, small separators)** — the picker's value depends critically on consuming the separator early enough to trigger decomposition. With default α_var=15 the multiplicative sep boost at typical `rel_k ≈ 0.27` is only ~5×, which on this instance is below the threshold where a sep var beats a high-cascade non-sep var. At α_var=100 the boost at the same `rel_k` is ~32×, comfortably above that threshold. Sep is consumed first and decomposition fires on schedule.

2. **t1_021_k10_s1 (medium, decomposable shrunken instance)** — default Design (G) parameters work. The hybrid solves where the default rate framework times out. The earlier finding from this session: τ-based scoring during the sep regime corrupts cache reuse because cascade-driven τ varies across recursion paths visiting the same logical state, so sep elements get picked in different orders → different sub-problems → cache miss. Mult-style scoring during sep keeps ordering path-independent.

3. **t1_065** — trivially fast; picker overhead is the entire timing. Not informative for picker design.

4. **t1_011** — failed in this session under hybrid + Design (G) defaults. Per the existing benchmark log, this instance is dominated by `buildCanonicalKey` work and has very high cache hit rates (74.6% L1). Hypothesis (untested): the post-sep regime is the bulk of search on t1_011, so τ's path-dependent ordering hurts there too — not just during the sep regime. Either way, t1_011 is the next instance to characterize.

### Toward instance-driven parameter selection

The single-instance result on t1_071 suggests α_var should not be a fixed constant. The default α_var=15 was calibrated for instances like t1_011 / k15 (per the 2026-05-05 entry above); t1_071 needs ~100. Possible structural signals to drive selection (untested):

- **Separator quality fraction**: instances where the precomputed METIS hierarchy returns small balanced cuts (low `α + β` in the rate framework's `ρ_sep_strategic` formula) probably benefit from high α_var, because the decomposition payoff per consumed sep element is large. The 2026-04-24 row notes t1_071 fits this pattern; the same row notes reactive METIS regresses on it because the pre-computed hierarchy is already good.
- **Density**: dense instances (t1_011, t1_049) need different scoring than sparse ones (t1_071). The 2026-04-24 12:30 α-sweep on Stage-0 found a similar pattern (lower α for dense, higher for sparse) — a cross-validation of the same intuition.
- **Cache hit rate during search**: high-L1-hit-rate instances (t1_011) may need to *avoid* path-dependent picker variation altogether, even post-sep.

These are hypotheses to test, not conclusions. The point of this entry is to record the observations cleanly so the parameter-selection question can be approached with a concrete starting point on the next instance. The goal stated in conversation: not a per-instance optimum, but parameter values selectable from cheap structural statistics of the instance.

### Open questions for the next session

- Does α_var=100 hurt t1_021_k10_s1 (where defaults work)? Test before generalising.
- What parameter setting (or parameter-selection rule) closes the gap on t1_011?
- Is the post-sep τ regime *itself* the cost on t1_011, or just the within-sep portion as on t1_021_k10_s1? Diagnostic: check first-N picks under hybrid on t1_011 and compare to legacy's picks at corresponding states.

---

## 2026-05-09 (continued) — Cascade-weight sweep + the case for "plain" as a universal default

After the t1_011 sweep above showed that *no* parameter setting of the hybrid (rate-framework) picker solves t1_011 (19 configurations all timed out at 60 s), we stepped back from the rate-framework path and ran a wider experiment: **drop `-pickerRateFramework` entirely, sweep `cascade_score_weight`** (the additive cascade signal already exposed via `-cascadeW`) on instances spanning four structural regimes.

Build: same as the previous entry (HEAD with regime-split picker patch in `solver_rec.cpp`, "dirty"). The diagnostic patch is irrelevant when `-pickerRateFramework` is off — `pickBranchTargetMultiplicative` is invoked instead. Compile flags `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64`. Load avg ~3–4 throughout (VS Code + Claude Helper + WindowServer competing). 60 s timeout per run.

Configurations tested:
- **Legacy** (`-rec -sep 5 -cb 3 -sepMode metis -sepVarBias`) — separator VARs stripped to bias bitmap; carried sep has clauses only; Stage 3 picker scores `freq + 10·act + 1000·bias`.
- **Plain** (`-rec -sep 5 -cb 3 -sepMode metis`) — no unified picker; full sep (VARs + CLAUSEs) consumed sequentially in METIS order via Stage 2; Stage 3 scores `freq + 10·act` (no bias).
- **Mult c=W** (`-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -pickerMode multiplicative -cascadeW W`) — unified multiplicative picker with cascade weight `W`. No `-pickerRateFramework`, no `-pickerNonSepKillsNd`.

### Per-instance results

| Instance | n_vars | n_clauses | density (m/n) | Best | Legacy | Plain | Mult c=0 | Mult c=0.5 | Mult c=1 | Mult c=2 | Mult c=5 | Mult c=10 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| t1_065 | 112 | 592 | 5.29 | Plain (0.017 s) | **0.53 s** (75 860 dec) | 0.017 s (614 dec) | 0.016 s | 0.019 s | 0.015 s | 0.019 s | 0.020 s | — |
| t1_021_k10_s1 | 80 | 75 | 0.94 | Legacy (3.99 s) | 3.99 s (879 K dec) | 5.25 s (1.32 M dec) | 10.08 s (3.18 M) | T/O | T/O | T/O | T/O | T/O |
| t1_071 | 640 | 1818 | 2.84 | Plain (0.41 s) | T/O | 0.41 s (62 K dec) | T/O | 14.10 s (1.5 M) | 0.90 s (101 K) | 0.78 s (89 K) | T/O | T/O |
| t1_011 | 6559 | 14515 | 2.21 | Plain ≈ Legacy ≈ Mult c=0 (~13.6 s) | 13.73 s (215 018 dec) | 13.57 s (215 018 dec) | 14.69 s (215 018 dec) | 47.80 s (118 K) | 49.71 s (120 K) | 40.79 s (99 K) | T/O | T/O |

All counts verified correct against the historical entries above (t1_065 `377…568`, t1_021_k10_s1 `430052389882336036`, t1_071 `4562956…272640`, t1_011 `536870912306`).

### The (ii) ↔ (iii) tension on t1_011 — quantified

Three configurations, all on the same hardware run, with matching counts:

| Config | Decisions | Conflicts | l2_stores | l2_hits | Wall | μs / decision |
|---|---|---|---|---|---|---|
| Mult c=0 | 215 018 | 17 447 | 138 758 | 75 771 | 14.7 s | 68 |
| Mult c=0.5 | 118 292 | 13 897 | 74 594 | 39 984 | 47.8 s | 404 |
| Mult c=1 | 119 598 | 16 209 | 76 985 | 37 751 | 49.7 s | 416 |
| Mult c=2 | 99 218 | 14 527 | 61 857 | 31 310 | 40.8 s | 411 |

Adding cascade *halves* the search tree (215 K → 99 K decisions) but each decision becomes ~6× more expensive. Cache-hit ratio is roughly preserved (35 % → 34 %); cache-store volume scales down proportionally with decisions. So the 4× wall regression is **picker-time per call**, not a cache-reuse collapse: `computeBcpGainScore` runs per active var per pick, and on t1_011's deep sub-components that cost dominates everything the smaller tree saves.

This is concrete evidence for the (ii)↔(iii) tension we hypothesized: cascade signal is informative (smaller tree) but its computation is expensive enough that on cache-heavy instances it's a net loss.

### The plain vs `-sepVarBias` divergence — the bias-staleness story

Plain matches legacy on t1_011 *bit-for-bit* (215 018 decisions, 138 758 stores, 75 771 hits — identical search tree). But plain dramatically beats legacy on **t1_065** (×30) and **t1_071** (T/O → 0.41 s).

Legacy's only difference vs plain is the `-sepVarBias` mechanism: original separator VARs are stripped to a persistent global `bias_bitmap`, and Stage 3's picker adds `+1000·bias[v]` to every var's score for the entire search. The bias never expires.

On t1_065 (uniform 5-CNF, density 5.29, strong BCP cascades) and t1_071 (density 2.84, deep ND-hierarchy), the original sep VARs are quickly decided by BCP early in the search, but the `+1000` bonus survives into deeper sub-components where those vars are no longer at any separator boundary — they're just *historically* at one. The bonus then misleads picks for the rest of the search.

On t1_021_k10_s1 (low density 0.94, weak BCP) the bias stays accurate longer; legacy beats plain (3.99 s vs 5.25 s). On t1_011 the ND-hierarchy is so deep that the bias is mostly irrelevant by the time the search reaches deep sub-components — legacy and plain converge.

### Headline finding: a 2-feature rule fits all four instances

The data fits a simple if-then-else:

```
if  density > 1.5  OR  n_vars > 200:
    use Plain (-rec -sep 5 -cb 3)
else:
    use Legacy (-rec -sep 5 -cb 3 -sepVarBias)
```

Routing check across four instances: t1_065 (density 5.29 → plain ✓), t1_071 (n_vars 640 → plain ✓), t1_011 (density 2.21 → plain ✓), t1_021_k10_s1 (density 0.94, n_vars 80 → legacy ✓).

**Strength of this finding** (caveats explicit): four data points, all from the MC2025 track-1 family. No instance yet in the small-and-low-density quadrant where plain beats legacy (would falsify the rule). No instance yet in the large-and-low-density quadrant either. The mechanistic story (BCP cascade strength governing bias staleness) is consistent with the data but unverified.

**Implications recorded in `portfolio_insights.md` §4 (2026-05-09).**

### Why this re-frames the whole picker direction

The unified picker (`-unifiedPicker`) was built to allow the picker to *override* the precomputed cut when scoring suggests a non-sep candidate would be better. On these four instances **the override is consistently a net negative**:

- On t1_065 / t1_071 / t1_011 plain matches or beats every unified-picker variant.
- On t1_021_k10_s1 legacy matches plain after removing the bias bitmap's stale-bonus issue, and *no* unified-picker variant lands within 1.5× of legacy.

Plain's advantage is structural: Stage 2 hard-forces sep consumption in METIS order. There's no scoring competition, no soft preference. When the cut is correct (which it is on these instances), there's nothing to mess up.

**This means the entire unified-picker code path (`-unifiedPicker`, `-pickerMode multiplicative`, `-pickerRateFramework`, `-cascadeW`, the rate / τ machinery in `pickBranchTargetRate`) should currently be considered research scaffolding, not a production path.** It introduces failure modes (scattered picks, picker-overhead amplification, stale-bias amplification) and we don't yet have a single instance where it earns its keep over plain.

### Open questions logged for later

- Is there an instance class in the (small, low-density) quadrant where *plain* beats *legacy* — i.e., where the rule needs a third feature?
- What's the right tie-breaker between plain and legacy when both work but one is slightly better?
- Could the unified picker earn its keep on instance classes we haven't tested (XOR-encoded, circuit-encoded)?
- The unified picker's cascade-driven "smarter tree" *does* materialize on t1_011 (118 K vs 215 K decisions). If `computeBcpGainScore` could be made ~10× cheaper, the wall regression would flip to a win. Is per-pick caching of cascade gains plausible?

---

## 2026-05-11 — t1_073 single-instance test + ganak count verification

Built: HEAD `5819f6f` (the checkpoint commit). Compile flags `-O3 -DNDEBUG -std=c++11 -Wall -arch arm64 -fno-stack-protector -D_FORTIFY_SOURCE=0 -mcpu=native`. Load avg ~2.6 throughout. 60 s timeout per run.

Instance: `mc2025_track1_073.cnf` (md5 `<not computed>`, decompressed from `MC2025_Public/mc2025_track1_public/mc2025_track1_073.cnf.xz` into `temp_cnf/`).

Features: **n_vars=1140, n_clauses=2870, density=2.52, pure 3-SAT (all 2870 clauses length 3, no binaries, no longer)**.

2-feature rule (§4 2026-05-09 portfolio_insights) predicts: density 2.52 > 1.5 AND n_vars 1140 > 200 → **use plain**.

| Config | Solver flags | Wall | Decisions | Conflicts |
|---|---|---|---|---|
| **Plain** | `-rec -sep 5 -cb 3 -sepMode metis` | **0.336 s** | **40,614** | 3,401 |
| Legacy `-sepVarBias` | `-rec -sep 5 -cb 3 -sepMode metis -sepVarBias` | TIMEOUT > 60 s | — | — |
| Mult c=0 | `-unifiedPicker -pickerMode multiplicative -cascadeW 0` (+ base) | TIMEOUT > 60 s | — | — |
| Mult c=0.5 | (above with `-cascadeW 0.5`) | 18.81 s | 3,149,906 | 4,351 |
| Mult c=1 | `-cascadeW 1` | 7.83 s | 1,433,278 | 3,854 |
| Mult c=2 | `-cascadeW 2` | 1.43 s | 143,672 | 3,943 |
| Mult c=5 | `-cascadeW 5` | TIMEOUT > 60 s | — | — |
| **ganak default** | `--verb 0` (default `--td 1`) | **1.57 s** | (Arjun+GANAK total) | — |

All non-timeout runs produce the identical count: `11248871064896276502896868524655766525245520791381363474128840640031577001474457329282740807213785606722436176677209265814954891911179007205709375941011983760426356703232000` (log₁₀ ≈ 172.05, ~10^167 models). Ganak's output explicitly returns this number, confirming correctness.

**Pattern matches t1_071** (also density ~2.8, sparse pure 3-clauses): plain wins decisively, legacy times out (bias-staleness consistent with strong BCP on density-2.5 pure 3-SAT), mult c=0 times out (unified picker without cascade signal scatters), cascade sweet spot in [1, 2] but plain still beats it. **sharpSAT plain is 4.7× faster than ganak default** on this instance — ganak's Flowcutter-based tree decomposition (`--td 1`) consumes most of its 1.57 s.

This is the 2nd confirmed data point in the (n_vars > 200, density > 1.5) → plain quadrant; the 2-feature rule continues to hold.

---

## 2026-05-11 (continued) — Falsification hunt: t1_023, t1_025, t1_027, t1_041, t1_047

Same build, same load (~2.6). 60 s timeout per run. Goal: probe the **(small, low-density)** quadrant the rule predicts as "use legacy", and the **(large, low-density)** quadrant that's untested (the rule says plain). Features extracted before running:

| Instance | n_vars | n_clauses | density | mean_len | Composition | Predicted | Quadrant |
|---|---|---|---|---|---|---|---|
| t1_023 | 102 | 102 | 1.00 | 3.00 | pure 3-SAT | legacy | small + low-density (Q10 falsifier candidate) |
| t1_025 | 63 | 66 | 1.05 | 3.00 | pure 3-SAT | legacy | small + low-density |
| t1_027 | 66 | 66 | 1.00 | 3.00 | pure 3-SAT | legacy | small + low-density |
| **t1_041** | **1920** | **1910** | **0.99** | 3.52 | mixed (154 bin / 301 ter / 1351 long) | **plain** | **large + low-density** (previously untested) |
| t1_047 | 80 | 240 | 3.00 | 3.00 | pure 3-SAT | plain | small + high-density |

### Per-instance results

| Instance | n_vars | density | Plain | Legacy `-sepVarBias` | ganak default | Predicted | Verdict |
|---|---|---|---|---|---|---|---|
| t1_023 | 102 | 1.00 | TIMEOUT > 60 s | TIMEOUT > 60 s | TIMEOUT > 60 s | legacy | inconclusive at 60 s |
| t1_025 | 63 | 1.05 | 7.31 s (2.29 M dec) | 7.50 s (2.18 M dec) | 2 s | legacy | tie (legacy slightly fewer decisions, wall similar) |
| **t1_027** | 66 | 1.00 | 5.78 s (2.01 M dec) | **4.80 s (1.39 M dec)** | 3 s | legacy | **legacy clearly wins** (17 % faster wall, 30 % fewer decisions) |
| **t1_041** | **1920** | **0.99** | **TIMEOUT > 60 s** | **TIMEOUT > 60 s** | **16 s** | **plain** | **both ours fail; density-1 structured class** |
| t1_047 | 80 | 3.00 | TIMEOUT > 60 s | TIMEOUT > 60 s | TIMEOUT > 60 s | plain | inconclusive at 60 s |

All non-timeout sharpSAT runs produce the count that ganak's `c s exact arb int` line confirms:
- t1_025 → `134746112245856`
- t1_027 → `1115259056499565`
- t1_041 → `5516767…443259892051805732864` (long; matches ganak's exact arbitrary-int line precisely) [tail corrected 2026-06-04: prior `…943…` was a transcription typo; re-verified vs ganak --prob 0]

### What the rule predicted vs what we measured

| Quadrant | Predicted | Result | Notes |
|---|---|---|---|
| small + low-density (Q10 falsifier candidates) | legacy | **legacy wins on t1_027; ties on t1_025; inconclusive on t1_023** | No falsifier found. Rule holds where both finish. |
| large + low-density (previously untested) | plain | **both fail; ganak finishes** | Rule moot; new class identified |
| small + high-density | plain | inconclusive (all timeout on t1_047 at 60 s) | Need longer budget or different test instance |

### New finding: a third "density-1 structured" quadrant

`t1_041` is the first measured instance in the **(large, low-density, mostly long-clauses)** quadrant. Both our configs time out at 60 s; ganak finishes in 16 s. Combined with the t1_021 family observations from 2026-04-27 §4 of `portfolio_insights.md`, the pattern across **t1_021/t1_023/t1_025/t1_027/t1_041** — all pure-or-near-pure 3-SAT with density ~1.0 — points to a structural class where our hierarchy-based search loses to ganak's tree-decomposition-driven DP, regardless of `-sepVarBias` / plain choice. The 2-feature `(density, n_vars)` rule **does not address this class** because it only predicts which of our two configs wins, not whether our solver is the right tool at all.

Detection signature for this class is cheap: `density ∈ [0.95, 1.10]` AND `binary_fraction ≤ 0.1`. On the available instances:
- t1_021 (full, 90 v / 90 c, density 1.00, pure 3-SAT) — density-1 structured class
- t1_023 (102 v / 102 c, density 1.00, pure 3-SAT) — density-1 structured class
- t1_025 (63 v / 66 c, density 1.05, pure 3-SAT) — borderline; sharpSAT solves at 7 s, ganak at 2 s
- t1_027 (66 v / 66 c, density 1.00, pure 3-SAT) — borderline; sharpSAT solves at 5 s, ganak at 3 s
- t1_041 (1920 v / 1910 c, density 0.99, mixed but mostly long) — density-1 structured class (we time out)

Implication for the portfolio: extend the analyzer with a "density-1 structured" detection (density ~1.0 + low binary fraction). When detected, the portfolio driver should attempt ganak first or fall through quickly. See updated portfolio_insights §4 / §5 / §8.

### Open questions

- **t1_023 and t1_047** specifically — both small instances that all three solvers timed out on at 60 s. Need a longer budget (300 s+) to characterize. Their structural placement is interesting: t1_023 looks like a harder t1_027 (102 v vs 66 v, same density); t1_047 is small but dense (n=80, density 3.0) — predicted plain but plain timed out.
- **t1_041 with our solver + longer budget**: does plain eventually finish, or is this fundamentally unreachable for us? Worth a 300 s probe.

---

## 2026-05-12 — t1_041 first-branch anchor study + min-rate probe metric

Investigation of why our solver TIMEOUTs on t1_041 (1920 v / 1910 c, density-1 structured per 2026-05-11) but solves it in ~2 s when a specific variable is fixed as a first branching decision. Builds on the 2026-04-29 picker session work.

**Setup**: input = `temp_cnf/mc2025_track1_041.cnf`; binary = current `build/sharpSAT` (Release, M-series, on battery so timings ~5-10 % slower than AC); flags `-rec -sep 5 -cb 3 -sepMode metis -wlIter 2`; 60 s per-run budget via `-t 60`.

### Probe metric refinement: single-polarity → min(rate_F, rate_T)

Previous probe ranking (`/tmp/probe_picker.py`) ranked variables by `(L2 cache hits on components with ≥ 200 vars) / decisions` measured in a 1 s solver run with `v=val` fixed as a unit clause. The metric was scored per `(var, polarity)`. Six of the old top-10 turned out to be false positives — fast on the cherry-picked polarity, slow on the other. Since the solver visits **both** polarities of every decision, the true cost is dominated by the worse side.

**Min-aggregation** (`score(v) = min(rate_F, rate_T)`) corrects this. A variable is only as good as its worse polarity.

| Var | Old single-pol rank | Old single-pol time | New min-rank (of 120) | F | T |
|---|---|---|---|---|---|
| **v450** | not probed | — | **4** | SOLVE 2.25 s | SOLVE 2.25 s |
| **v242** | 10 | 2.2 s | 2 | SOLVE 2.17 s | SOLVE 2.36 s |
| v405 | not probed | — | 6 | SOLVE 2.41 s | SOLVE 2.43 s |
| v456 | not probed | — | 5 | SOLVE 2.74 s | SOLVE 2.83 s |
| v526 | not probed | — | 9 | SOLVE 2.81 s | SOLVE 2.79 s |
| v263 | not probed | — | 8 | SOLVE 1.08 s | SOLVE 5.53 s |
| v631 | not probed | — | 3 | SOLVE 3.91 s | SOLVE 3.00 s |
| v407 | not probed | — | 1 | SOLVE 5.79 s | SOLVE 2.99 s |
| v5 | not probed | — | 10 | **TIMEOUT** (60.51 s) | SOLVE 3.56 s |
| v459 | not probed | — | 7 | **TIMEOUT** (60.75 s) | SOLVE 4.10 s |
| v70 (false positive) | 2 | TIMEOUT | 31 | **TIMEOUT** (60.86 s) | SOLVE 45.13 s |
| v176 (false positive) | 3 | 4.0 s | 76 | **TIMEOUT** (60.20 s) | SOLVE 4.06 s |
| v24 | 4 | 4.2 s | 47 | SOLVE 4.2 s | (T not measured this run) |
| v33 (false positive) | 7 | 4.6 s | 111 | **TIMEOUT** (60.21 s) | SOLVE 4.57 s |
| v1 (false positive) | 9 | 7.3 s | 85 | **TIMEOUT** (60.74 s) | SOLVE 7.24 s |

### Probe sweep: 120 vars (60 top-degree + 60 lowest-degree-flip-symmetric)

Both polarities probed, `-t 1` budget each, ~474 s wall total. Top 10 by `min(rate_F, rate_T)`:

| Rank | Var | Flip-sym | Deg | min_rate | max_rate |
|---|---|---|---|---|---|
| 1 | v407 | YES | 2 | 0.1506 | 0.2252 |
| 2 | v242 | YES | 24 | 0.1124 | 0.1220 |
| 3 | v631 | YES | 2 | 0.1123 | 0.1220 |
| 4 | v450 | YES | 2 | 0.0858 | 0.0860 |
| 5 | v456 | YES | 2 | 0.0691 | 0.0897 |
| 6 | v405 | YES | 2 | 0.0635 | 0.0639 |
| 7 | v459 | YES | 2 | 0.0619 | 0.0807 |
| 8 | v263 | YES | 24 | 0.0552 | 0.0675 |
| 9 | v526 | YES | 2 | 0.0446 | 0.0775 |
| 10 | v5 | YES | 32 | 0.0419 | 0.0868 |

**All top-10 are flip-symmetric** (literal-graph WL: `+v` and `-v` end up in the same WL color class). That's not sufficient (1187 of 1920 vars are flip-symmetric) but it **is** necessary — every non-flip-symmetric var measured had asymmetric F vs T times (v153 F=18.7s T=60.9s).

### TIMEOUT vs SOLVE clarification

The full-solve runs below used `-t 60` (60 s budget). When the budget is hit, the solver still emits a `time: 60.XXs` line — initial labeling parsed this as a SOLVE. Correct interpretation: any run with `time ≥ 60 s` AND a `TIMEOUT !` marker is a TIMEOUT, NOT a solve. Re-classified results:

**Top-10 (corrected)**

| Var | F status | T status |
|---|---|---|
| v407 | SOLVE 5.79 s | SOLVE 2.99 s |
| **v242** | SOLVE 2.17 s | SOLVE 2.36 s |
| v631 | SOLVE 3.91 s | SOLVE 3.00 s |
| **v450** | SOLVE 2.25 s | SOLVE 2.25 s |
| v456 | SOLVE 2.74 s | SOLVE 2.83 s |
| v405 | SOLVE 2.41 s | SOLVE 2.43 s |
| **v459** | **TIMEOUT** (60.75 s) | SOLVE 4.10 s |
| v263 | SOLVE 1.08 s | SOLVE 5.53 s |
| v526 | SOLVE 2.81 s | SOLVE 2.79 s |
| **v5** | **TIMEOUT** (60.51 s) | SOLVE 3.56 s |

So **8 of 10** top-min-rate candidates solve both polarities; 2 (v459, v5) have one TIMEOUT polarity.

**Negative validation (corrected)** — every var with `min_rate < 0.02` had a TIMEOUT on at least one polarity:

| Var | F | T |
|---|---|---|
| v33  | TIMEOUT (60.21 s) | SOLVE 4.57 s  |
| v176 | TIMEOUT (60.20 s) | SOLVE 4.06 s  |
| v1   | TIMEOUT (60.74 s) | SOLVE 7.24 s  |
| v153 | SOLVE 18.68 s | TIMEOUT (60.86 s) |
| v70  | TIMEOUT (60.86 s) | SOLVE 45.13 s |

### Count verification (cleaner methodology)

Original methodology (renumbered probe-CNFs) gave per-polarity counts that don't sum to ganak's t1_041 total because renumbering drops isolated vars from the active set. Re-ran using the **original t1_041 + a single appended unit clause `±v 0`** — preserves variable numbers, no resolution, no renumbering.

Ganak total on `mc2025_track1_041.cnf`: `...43259892051805732864` (16.9 s).

| Var | F time | T time | F-count tail | T-count tail | F + T == ganak total? |
|---|---|---|---|---|---|
| v242 | 2.23 s | 2.40 s | ...025902866432 | ...025902866432 | **MATCH** |
| v450 | 2.30 s | 2.26 s | ...025902866432 | ...025902866432 | **MATCH** |
| v405 | 2.49 s | 2.43 s | ...025902866432 | ...025902866432 | **MATCH** |
| v456 | 2.80 s | 2.95 s | ...025902866432 | ...025902866432 | **MATCH** |
| v526 | 2.93 s | 2.86 s | ...025902866432 | ...025902866432 | **MATCH** |

All 5 anchors:
- Both polarities give identical counts (flip-symmetry confirmed at the count level).
- F + T equals ganak's total → counts are correct.
- All 5 give the **same count tail** (...025902866432) → they're equivalent under the formula's symmetry group; same orbit / interchangeable as anchors.

### Negative validation: bottom of min-rate

For min-rate < 0.05, at least one polarity is near-TIMEOUT:
- v33 (min=0.0009): F=60.2s, T=4.6s
- v176 (min=0.0038): F=60.2s, T=4.1s
- v1 (min=0.0030): F=60.7s, T=7.2s
- v153 (min=0.0083): F=18.7s, T=60.9s (only var where BOTH polarities are slow — v153 is the one with unbalanced F/T counts)
- v70 (min=0.0122): F=60.9s, T=45.1s

Combined with the top-10 results, the empirical cutoff is roughly:
- **`min_rate ≥ 0.04`**: both polarities solve in < 6 s with high probability (7 of 10 top-10 cases)
- **`min_rate < 0.02`**: at least one polarity is near-TIMEOUT

### Structural finding: zero BCP cascade at first decision

Confirmed via Python UP simulation: F has a 104-var backbone that root preprocessing (`simplePreProcess` → BCP → `HardWireAndCompact`) absorbs unconditionally. For every variable in the top 10 + the false positives, branching `v = val` adds **exactly 1** newly-assigned variable beyond the backbone — no further BCP cascade. The "magic" of v242 (and v450, v405, v456, …) is not from immediate simplification. The 30× difference between v242 (fast) and v70 (slow) emerges in **deeper cache-amplification dynamics**, not in static residual structure.

The BCP residuals R(v242=F) and R(v70=F) differ by only 36+36 = 72 clauses out of 1830 (each side: 36 clauses containing the OTHER variable, since the chosen one is assigned away). Connected-component counts, WL class distributions, and flip-symmetric var counts are all essentially identical between fast and slow cases. The static structure does not predict the dynamic difference.

### Practical takeaway for the picker

For density-1 structured instances where our solver normally TIMEOUTs, a **probe-based picker with min-aggregation** identifies multiple good first-branch anchors, not just one "magic" variable. The picker would:

1. Compute literal-graph WL once (one O(n+m) pass × k iterations).
2. For each flip-symmetric variable (~half of all vars on this instance), run a 1 s solver probe at both polarities.
3. Rank by `min(rate_F, rate_T)`; pick the highest.

Cost on t1_041: 401 flip-sym vars × 2 polarities × 1 s ≈ 800 s — too expensive as a fixed preamble. But:
- Restricting to top-N by degree + low-degree-flip-sym (the 120-var sweep used here) covers it in ~8 minutes
- Or: probe just a few dozen flip-sym vars, since results suggest the top of the distribution is large — many vars have min_rate ≥ 0.04 in the top-10 / top-20

This is a **research finding, not a portfolio config yet**. No prediction on whether this picker structure transfers to other density-1 structured instances; needs validation on t1_021 family, t1_023, t1_027.

### Open questions specific to this study

- Does v450's tie with v242 reproduce on a fresh process (or is it a single-run quirk)?
- Do the family of fast-anchor variables share any structural property beyond flip-symmetry that could be detected cheaply?
- Min-rate is non-monotone with total solve time (v5 / v459 rank in top 10 but have ~60 s polarities). What additional feature separates them? Hypothesis: max(rate_F, rate_T)/min(rate_F, rate_T) ratio — high ratio signals lopsidedness even when min is moderate.


## 2026-05-18 — t1_041 SOLVES in 9.7 s with reactiveMetis + wlIter 2

Supersedes the 2026-05-11 framing of t1_041 as "structural class where our solver loses to ganak regardless of config." A specific flag combination cracks it cleanly.

**Winning invocation** (Release build):
```
build/sharpSAT -rec -sep 5 -cb 3 -sepMode metis -wlIter 2 \
  -reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4 \
  -q ../temp_cnf/mc2025_track1_041.cnf
```
**Wall time: 9.74 s** (verified via `probe_flags.py --budget 30`). Count matches ganak.

### Synergy: neither flag alone is enough

7-variant `probe_flags.py` portfolio comparison (30 s/variant budget) on `mc2025_track1_041.cnf`:

| variant | wlIter | reactiveMetis | result | pct_lin | l2_hit_rate | big_hit_rate |
|---|---|---|---|---|---|---|
| **react-agg-wl2** | **2** | **aggressive (min 10, skip 4)** | **SOLVE 9.74 s** | 100% | 0.324 | 0.049 |
| plain-wl2 | 2 | OFF | TIMEOUT 30.98 s | ~0% | 0.222 | 0.022 |
| react-agg | 1 (default) | aggressive | TIMEOUT 30.86 s | 0.391% | 0.185 | 0.011 |
| react-default | 1 | default (no min/skip tuning) | TIMEOUT 30.86 s | 0.098% | 0.190 | 0.005 |
| plain | 1 | OFF | TIMEOUT 30.42 s | ~0% | 0.120 | 0.001 |
| clausesFirst | 1 | OFF + `-sepClausesFirst` | TIMEOUT 30.87 s | ~0% | 0.120 | 0.001 |
| adaptive | 1 | OFF + `-adaptive` | TIMEOUT 30.12 s | ~0% | 0.238 | 0.013 |

The combination is multiplicative, not additive: react-agg-wl2 has ~75 % higher overall L2-hit rate than react-agg, and **4.4× the big-hit rate** (hits on subcomponents ≥ 200 vars). Big hits are what crack the residual core; with `-wlIter 1` the canonical keys fail to identify structurally-equivalent residual sub-components, so the cache misses where it should hit, and the solver grinds.

### Failure mode of react-agg confirmed: hard residual plateau

Extended probe of react-agg (the strongest non-winning config) at `-t 3600`:

- t = 5 s: pct_lin = 0.0008 % (closed_bits 1013)
- t = 85 s: pct_lin = **3.22 %** (closed_bits 1025.04) — residual plateau begins
- t = 600 s (10 min): pct_lin = 3.229 %, closed_bits 1025.05
- t = 2760 s (46 min — exit, likely macOS memory pressure): pct_lin = **3.22938 %**

Over 44 min of "plateau", closed_bits moved 0.01 bits while the solver did 88 M decisions and 14 M cache hits — deep small subtrees that fall below the double-precision granularity of the log-sum-exp accumulator. **react-agg will not crack t1_041 with more budget.** Only the wlIter=2 combination breaks through.

### Connection to 2026-05-12 anchor-variable study

The 2026-05-12 anchor study probed `-rec -sep 5 -cb 3 -sepMode metis -wlIter 2` (no reactive METIS) and found that **specific anchor variables** solve t1_041 in ~2 s, but the default picker doesn't pick them. The portfolio finding here is consistent: wlIter=2 alone (= `plain-wl2`) doesn't solve at the default picker, but the reactive-METIS path supplies runtime separators that, combined with wlIter=2 canonicalization, expose the same residual structure that the anchor variables expose.

### Progress-metric correction landed alongside this measurement

This run used the [Progress metric fix](progress_metric_issue.md): `closed_log_sum_` is now credited at LEAF events with budget threading, not at cache STOREs. Old metric overshot to 1030.24 on react-agg-wl2 finish; new metric lands at exactly 1030.0 (n_root). This is what made the portfolio probe's `progress_bits=0.4 %` for react-agg meaningfully distinguishable from the other timed-out configs at ~0 %; the previous metric collapsed all timeouts to the same value.

### Catalog entry for folklore

> **If react-agg plateaus hard (pct_lin stuck below 5 % after ~60 s), try `-wlIter 2`.** Signal: low big-hit rate (≤ 0.02) suggests canonical-key collisions on residual sub-components.

Worth testing this folklore rule on the other density-1 structured instances flagged 2026-05-11: t1_021 family, t1_023, t1_025, t1_027.

### Follow-up — adding `-unifiedPicker -decomposeAfterK 1000` drops solve time to 3.04 s

Added the unified picker on top of `react-agg-wl2`:

```
build/sharpSAT -rec -sep 5 -cb 3 -sepMode metis -wlIter 2 \
  -reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4 \
  -unifiedPicker -decomposeAfterK 1000 \
  -q ../temp_cnf/mc2025_track1_041.cnf
```

**Wall: 3.04 s real, 3.02 s user.** Count verified against ganak (`…43259892051805732864`).

Comparison on t1_041:

| variant | wall | speedup vs `react-agg-wl2` |
|---|---|---|
| `react-agg-wl2` (today's portfolio winner) | 9.74 s | 1× |
| `react-agg-wl2 + -unifiedPicker -decomposeAfterK 1000` | **3.04 s** | **3.2×** |

### Re-frames the 2026-05-09 "unified picker is scaffolding" verdict

The 2026-05-09 cascade-weight sweep declared the unified-picker code path "research scaffolding, not a production path" because every variant lost to plain on the four instances tested (t1_065 / t1_071 / t1_011 / t1_021_k10_s1). **That sweep did not include `-wlIter 2` or `-reactiveMetis*`.** On those instances, plain already wins, so the picker has nothing to add.

This finding flips the verdict on t1_041 specifically: when the picker has GOOD candidates to score (because reactiveMetis supplies dynamic cuts the precomputed hierarchy misses) AND a reliable cache (because wlIter=2 keeps canonical keys consistent across structurally-equivalent residual sub-components), its "soft override of the precomputed cut" is a net WIN, not the net loss documented in the 2026-05-09 sweep.

The flag triple is fully multiplicative-synergistic:

| `-wlIter 2` | `-reactiveMetis*` | `-unifiedPicker` | wall | notes |
|---|---|---|---|---|
| ✓ | – | – | TIMEOUT 30 s | `plain-wl2` |
| – | ✓ | – | TIMEOUT 46 min (memory pressure exit at 3.23 %) | `react-agg`, plateaued |
| – | – | ✓ | (untested in this combination — would be one to fill in) | |
| – | ✓ | ✓ | **~20 min** | wlIter=1 + reactiveMetis + unifiedPicker. Solves cleanly (verified 2026-05-19; final OPEN_WORK in [20, 21] min interval, progress trajectory reached 96.9 % at t=20 min) |
| ✓ | ✓ | – | 9.74 s | `react-agg-wl2` |
| ✓ | ✓ | ✓ | **3.04 s** | best known |

### Picker breaks the plateau even at wlIter=1

The 4th row above is the key new datapoint (2026-05-19). Compared to `react-agg` (which plateaus permanently at 3.2 %), adding `-unifiedPicker -decomposeAfterK 1000` makes the same wlIter=1 config solve in ~20 min — categorically a different regime, not a quantitative speedup. **The unified picker is sufficient to make the instance solvable**; wlIter=2 then provides a ~400× multiplier on top.

Trajectory (PROGRESS every 60 s):

| t (min) | pct_lin | closed_bits | progress_bits |
|---|---|---|---|
| 1 | 4.15 % | 1025.41 | 0.06 |
| 2 | 7.54 % | 1026.27 | 0.11 |
| 3 | 9.81 % | 1026.65 | 0.15 |
| 4 | 12.46 % | 1026.99 | 0.19 |
| 5 | 23.30 % | 1027.90 | 0.38 |
| 10 | 50.10 % | 1029.00 | 1.00 |
| 15 | 75.64 % | 1029.60 | 2.04 |
| 20 | 96.92 % | 1029.95 | 5.02 |
| ~20-21 | 100 % (OPEN_WORK fired) | 1030.00 | 1030 |

Unlike react-agg's exponentially-decaying plateau, this trajectory is **roughly linear in pct_lin** with periodic big jumps (e.g. +12 pp at t=5, +12 pp at t=15, +10 pp at t=20). Those jumps correspond to upper-level subtree closures; their regularity suggests the picker is finding a balanced search order that progressively closes branches of similar abstract size.

### Updated catalog entry for folklore

> **On a density-1 structured instance, the right baseline to compare against is `react-agg-wl2 + -unifiedPicker -decomposeAfterK 1000`, not plain.** The 2026-05-09 "plain dominates" finding applies to instances where the precomputed cut is already correct; for instances where it isn't (t1_041, and likely the rest of the density-1 structured class), the picker contributes meaningfully once it has reactiveMetis-supplied cuts to score.

Open question: does this triple-flag also dominate on the 2026-05-09 sweep instances (t1_065 / t1_071 / t1_011 / t1_021_k10_s1)? If yes → universal recommendation. If no → density-1 structured is its own regime requiring its own folklore.


## 2026-05-19 — Triple-flag config does NOT generalize across density-1 structured class; "plain" must remain in any portfolio

### Findings

Tested the triple-flag config (`-wlIter 2 -reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4 -unifiedPicker -decomposeAfterK 1000 -cascadeW 0`) on the other density-1 structured instances flagged 2026-05-11 (t1_023, t1_025, t1_027, t1_047), with 600 s budget per run and ganak `--td 1` (and `--td 0` if td1 timed out) as baselines.

| Instance | ganak --td 1 | ganak --td 0 | sharpSAT triple-flag | Documented plain (2026-05-11) | Documented legacy (2026-05-11) |
|---|---|---|---|---|---|
| t1_023 | TIMEOUT > 600 s | TIMEOUT > 600 s | TIMEOUT > 600 s (progress_bits 3 × 10⁻⁷) | TIMEOUT > 60 s | TIMEOUT > 60 s |
| t1_025 | 1.69 s | — | 22.23 s ✓ | 7.31 s | 7.50 s |
| t1_027 | 2.81 s | — | 440.48 s ✓ | 5.78 s | 4.80 s |
| t1_047 | 480.87 s ✓ (count `11123864327733`) | — | TIMEOUT > 600 s (progress_bits 0.57) | TIMEOUT > 60 s | TIMEOUT > 60 s |

Re-ran plain on t1_027 to confirm: 5.28 s solver time, count matches `1115259056499565`. Plain regression-free.

### The triple-flag wins on t1_041 but loses badly on smaller density-1 instances

The 2026-05-18 t1_041 result (3.04 s with triple-flag, plain TIMEOUT > 30 s) does **not** generalize:

- t1_025: triple-flag 22.23 s vs. plain 7.31 s → **3.0× slower**.
- t1_027: triple-flag 440 s vs. plain 5.78 s → **76× slower**.

The density-1-structured class is not homogeneous: t1_041 is large (1920 vars, mixed clause lengths, density 0.99), while t1_023/t1_025/t1_027/t1_047 are small (63-102 vars, pure 3-SAT). The picker's value depends on instance size, not just density.

### 2D parameter sweep on t1_027 (8 runs, `-pickerAlphaVar` × `-wlIter` × `-reactiveMetis on/off`)

| wlIter | α_var | reactiveMetis | wall | progress_bits | decisions |
|---|---|---|---|---|---|
| 1 | 15 | on | TIMEOUT 61 s | 0.21 | 22.3 M |
| 1 | 50 | on | TIMEOUT 61 s | 0.14 | 22.0 M |
| 1 | 100 | on | TIMEOUT 61 s | 0.13 | 22.2 M |
| 1 | 200 | on | TIMEOUT 61 s | 0.13 | 21.9 M |
| 2 | 15 | on | TIMEOUT 61 s | 0.21 | 19.9 M |
| 2 | {50, 100, 200} | on | TIMEOUT 61 s | 0.10-0.12 | 20.2-20.4 M |
| 1 | {15..200} | off | TIMEOUT 61 s | 0.11-0.21 | 22.0-22.3 M |
| 2 | {15..200} | off | TIMEOUT 61 s | 0.11-0.21 | 20.0-20.5 M |

No setting solves t1_027 in 60 s. The picker generates ~10× more decisions than plain (22 M vs 2 M) at similar per-decision throughput (~370 K dec/s vs plain's ~350 K dec/s). The cost is the *search tree* the picker explores, not per-pick overhead.

Counter-intuitively, *higher* α_var produces *lower* progress (0.21 → 0.13 as α goes 15 → 200). Forcing the picker to favor sep more aggressively makes the search worse — strong evidence that sep-element *ordering within the cut* is wrong: picker scores sep vars by `freq + 10·act` while plain consumes them in METIS order (sep[0] first).

### Plain ≠ "picker with some α setting"

Reading `pickBranchTarget()` ([solver_rec.cpp:2210-2383](../src/solver_rec.cpp#L2210-L2383)):
- Plain: pops `separator[0]`, branches on it, recurses with `separator[1:]`. Zero scoring, METIS-fixed order.
- Picker: scores every active VAR and CLAUSE with multiplicative formula `score = varW · raw · (1 + α · exp(-λ · rel_k) · is_sep)`; picks argmax across all candidates of both kinds.

Even at extreme α the picker picks sep elements by *internal score*, not by *METIS index*. No flag currently restores plain's "sep in order" behavior. A new flag `-pickerSepLockstep` (return the first active sep element directly, skip scoring when sep is non-empty) would be needed to mimic plain exactly; that's a code change, not a parameter sweep.

### Policy update

> **`plain` (`-rec -sep 5 -cb 3 -sepMode metis`) must remain a mandatory portfolio config.** It is the only configuration we have that consumes the precomputed METIS separator in METIS order without per-pick scoring overhead. On small density-1 structured instances (t1_025, t1_027, t1_065, t1_071), this is decisively faster than any unified-picker setting. The 2026-05-18 t1_041 finding does NOT change this — picker-based configs win on *large* instances where the picker can override poor precomputed cuts, but on instances where the precomputed cut is already correct (and the search tree is small), plain has no overhead to amortize.

Until `-pickerSepLockstep` (or equivalent) is implemented, any portfolio driver must evaluate at minimum: `plain` AND the picker-based triple-flag, choosing per-instance.

### Open follow-ups

- Implement `-pickerSepLockstep` and validate against plain on t1_025/t1_027 (expect near-identical wall) and against the current picker on t1_041 (expect identical wall, since t1_041's win came from picker behavior *after* sep was exhausted, not from picker overriding the cut).
- Confirm the picker's relative-ordering hypothesis directly by dumping the first 20 picks of plain vs picker on t1_027 and comparing which separator indices each picks at each level.
- Re-evaluate the 2026-05-18 "react-agg-wl2 + unified picker" entry: the win on t1_041 is real but the entry's universal-recommendation framing was over-strong. Treat the triple-flag as one of N portfolio members, not a default.


## 2026-05-19 (continued) — Adaptive solver: separator-disabled is the right default

### Empirical finding

On t1_047 (80 vars, density 3.0, dense pure 3-SAT), tested adaptive (`-rec -sep 5 -cb 3 -sepMode metis -adaptive`) with and without the separator flag. Five `-adaptive` variants run at 15 min budget each:

| variant | wall | notes |
|---|---|---|
| `-adaptive -sep 5` (default) | 648.4 s | with precomputed METIS hierarchy |
| `-adaptive -sep 5 -wlIter 2` | 642.9 s | within noise of default |
| `-adaptive -sep 5 -adaptiveAlpha 0.5` | 651.9 s | within noise |
| `-adaptive -sep 5 -adaptiveAlpha 1.0` | 703.8 s | 8% slower |
| `-adaptive -sep 5 -reactiveMetis*` | TIMEOUT 908 s | reactive dynamic cuts destructive on dense small |
| `-adaptive` (NO `-sep`) | 600-660 s | indistinguishable from `-adaptive -sep 5` |

### Diagnostic confirming separator is unused

Verbose log on t1_047 with `-adaptive -sep 5 -v -logBranches`:
```
NDHierarchy: 37 tree nodes, 8 internal (sep), ..., max_sep=47 ...
ROOT_DECOMP: root_active=80 subcomps=1 trivial_factor=1 sub0=80
ROOT_ENTRY call=1 sep_size=0 sep_reset=1 comp_total_vars=80 comp_active=80 trail=0
  TIER1_REJECT nd=0 reason=size sep=47 n=80 allowed=20
  TIER2_PICK v=2 tau=1.61322 scored=20
BRANCH_ENTER id=1 lit=2 lit_orig=2 DL=1 ...
```

The first decision is `V2` (NOT in the root separator V1,V5,V7,...). Reason: the root separator has 47 elements, but the gate `Solver::hierarchySeparatorAcceptable` ([solver.cpp:1259](../src/solver.cpp#L1259), [solver.h:386](../src/solver.h#L386)) rejects whenever `sep_size > min(0.3·n_active, 100)`. For t1_047 root: `0.3·80 = 24`, so `47 > 24` → rejected. The print's "allowed=20" is stale (a `DIAG: relax 20→100` comment in the code shows the actual cap is 100). Same rejection cascades to deeper hierarchy nodes — adaptive runs end-to-end without consuming a single sep element.

### Why the size gate exists and why it's right here

The 2026-04-19 "Phase 1" rationale: a separator larger than ~30% of the active sub-component is structurally bad (branching on 24+ vars before any decomposition has the same effect as full variable branching, with extra bookkeeping overhead). The gate is correct *as a policy*. The issue on t1_047 is that **every** ND-hierarchy node has a separator that exceeds the gate, so the ND machinery is dead code throughout the search.

This is a property of dense small instances. METIS-style decompositions don't find small balanced cuts on density-3 graphs; the formula is too connected. The gate rejects everything, the search runs as if `-sep` wasn't passed.

### Policy update

> **When using `-adaptive`, the default should be NO `-sep` flag.** The precomputed METIS hierarchy is rejected at runtime on instances where it would matter least (dense small ones where adaptive's τ-based picker is already strong), and only ever helps when METIS finds small balanced cuts (which adaptive itself can't exploit — adaptive has no sep-awareness in its scoring). Dropping `-sep` saves the METIS build cost at startup (~1-2 seconds on larger instances) without changing the search.

This is the adaptive analogue of [[plain-baseline-required]] for the picker family: the right default depends on instance class, and `-sep` for `-adaptive` is an opt-in (paying startup cost to potentially help, but the runtime gate decides if it actually does).

### Open follow-ups

- Quantify the METIS startup cost on a range of instance sizes — is `-adaptive -sep 5` ever materially slower than `-adaptive` due to that cost alone?
- Test `-adaptive` (no sep) on the broader density-1 structured class (t1_023, t1_025, t1_027 — already documented; revisit with adaptive specifically).
- The size-gate-cascade-rejection pattern suggests an early-exit: at ND-build time, scan all separator sizes and if no node has an acceptable sep, **don't build the hierarchy at all**. ~5 lines in `countSATRec`. Saves 1-2 s on density-3 small instances.
- Implement sep-awareness in adaptive's score formula (a `+α_sep · is_sep(v)` boost in `cheap_score`) — could unlock the METIS hierarchy for adaptive on instances where the gate *would* accept it (i.e., medium-density instances with reasonably small precomputed cuts).


## 2026-05-20 — Per-decision throughput optimization stack: +102% on t1_045 (profile-driven)

Four-commit chain on top of `3882e3f` after macOS `sample` identified the real per-decision hotspots — none of which were the previously-attempted target (`count_active_2clauses`, 0.06% self-time). Same algorithm, same counts; pure overhead reduction.

### t1_045 adaptive+wlIter=2 (300 s budget, `-rec -sep 5 -cb 3 -sepMode metis -adaptive -wlIter 2 -t 300`)

| commit | decisions/300s | throughput (dec/s) | vs baseline | step Δ |
|---|---|---|---|---|
| `3882e3f` (baseline) | 53,745,480 | 178,914 | — | — |
| `bd48859` | 93,749,222 | 312,287 | +74% | +74% |
| `c61cefc` | 102,837,920 | 342,665 | +91% | +10% |
| `d7e2a1c` | 106,264,064 | 354,213 | +98% | +3.4% |
| `010ef37` | 108,672,794 | 362,234 | **+102%** | +2.3% |

`avg_bcp/dec` is 1.17128 → 1.18000 across the chain — essentially constant. The +102% is entirely per-decision overhead removed, not algorithmic change.

### t1_041 winning config (`-rec -sep 5 -cb 3 -sepMode metis -wlIter 2 -reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4 -unifiedPicker -decomposeAfterK 1000`)

| commit | wall time |
|---|---|
| `3882e3f` (documented baseline) | 3.04 s |
| `bd48859` | 2.82 s |
| `c61cefc` | 2.16 s |
| `d7e2a1c` | 2.03 s |
| `010ef37` | 2.05 s |

### What each commit changed

- **`bd48859` — static-scratch buffers in `buildCanonicalKey`.** Six per-call heap-allocated structures (`block_label`, `sig_k`, two `labels` collision vectors, `anchored_sorted`, two `unordered_map` count tables) became module-static buffers reused via `auto &` aliases. Allocator slice (`_xzm_*` family) dropped from ~14% to ~7% of CPU. Single biggest win at +74%.

- **`c61cefc` — sort-scan replacing post-WL count maps.** The two `unordered_map<uint64_t, int>` count tables answered the same "how many sig_pairs share this block_label?" question. Replaced both — and the separate `anchored_sorted` vector — with a single sorted vector of `(block_label, var_idx)` pairs (`s_labels_collision`) and two-pointer group scans. Hash insert/lookup overhead removed; one extra sort feeds three downstream uses.

- **`d7e2a1c` — throttle `gettimeofday`.** `solveComponent` + `solveComponentImpl` each invoke `stopwatch_.timeBoundBroken()` per recursive call. Profile showed the `gettimeofday` family at ~5% of wall time. Throttle to 1 in 1024 calls (counter + cached last result; once broken, sticky). Granularity ≤ ~3 ms at current throughput — well under any user-set time bound.

- **`010ef37` — merged WL inner-loop two-pass.** The long-clause branch of the WL refinement loop walked `literal_pool` twice per clause: pass 1 to accumulate `clause_h`, pass 2 to distribute `clause_h − h_i` into `sig_k`. Replaced with a single walk that gathers `(idx, mix64(block_label[idx]))` into a small static scratch buffer (`s_wl_clause_contribs`); pass 2 iterates the contiguous buffer instead of re-walking. Bitwise-identical arithmetic.

### Methodology note

The previous attempt to optimize the adaptive picker by incrementally maintaining `count_active_2clauses` (a `n_active_2c_long_phys_` counter + per-clause `active_len_`/`sat_count_` updates in `setLiteralIfFree`/`unSet` hooks) was a **net 1% regression** on t1_045. Cause: the function is called only from `probeLiteral`, which fires only on the adaptive picker's slow path (`n_active ≥ adaptive_probing_min_vars`, default 60). With `avg_comp_at_entry = 13.5` on t1_045, most adaptive calls hit the fast-path argmax and the slow scan was already amortized to nothing — while the per-flip incremental maintenance cost paid on every BCP step (~63M times per 300 s) accumulated.

The lesson: **profile before optimizing**. The `Explore` agent's structural prediction that `count_active_2clauses` was "60-70% of per-decision time" was based on call structure, not measurement. The actual `sample` profile showed `count_active_2clauses` at 0.06% — far below the noise floor. Optimizing it was both pointless and harmful. The first thing I did after reverting was the macOS `sample` profile run, which pointed straight at `buildCanonicalKey` (24.7%) and the allocator (~14%); both bd48859 and c61cefc fall out of that data directly.

### How to re-profile

```bash
# Build with frame pointers + debug symbols (Release-O3 otherwise):
mkdir -p build_prof && cd build_prof
cmake -DCMAKE_BUILD_TYPE=Profiling \
      -DCMAKE_CXX_FLAGS_PROFILING="-std=c++11 -O3 -g -fno-omit-frame-pointer -DNDEBUG -Wall" \
      -DMETIS_DIR=$HOME/Desktop/Code/METIS \
      -DGKLIB_DIR=$HOME/Desktop/Code/GKlib ..
make sharpSAT -j4

# Start solver in background, sample for 30 s, read flat profile from
# the "Sort by top of stack" section near the end of the output file.
./sharpSAT -rec -sep 5 -cb 3 -sepMode metis -adaptive -wlIter 2 \
           -t 70 ../../temp_cnf/mc2025_track1_045.cnf &
PID=$!; sleep 8; sample $PID 30 -file /tmp/prof.txt; wait $PID
```

### What's still on top after this chain

Most recent profile (post-`d7e2a1c`, pre-`010ef37`): `buildCanonicalKey` still ~30% (the inner clause-iteration loops + the new sort), component analysis (`recordComponentOf` + `setupAnalysisContext` + `makeComponentFromState` + `discoverComponentsOf`) ~13%, allocator ~7% (down from ~14% in baseline). The remaining `buildCanonicalKey` slice is now dominated by the WL clause iteration and the sort — both essential to the algorithm; further wins would require either (a) algorithmic changes (skip WL when canonical-id collisions can't happen) or (b) attacking component analysis next.


## 2026-05-20 — t1_045 first solve (verified by ganak)

After the four-commit optimization chain landed, re-ran t1_045 with the same adaptive+wlIter=2 config under a 45-min budget. **The solver finished in 40.5 min** — first time this instance has been solved end-to-end by sharpSAT.

### Invocation
```
build/sharpSAT -rec -sep 5 -cb 3 -sepMode metis -adaptive -wlIter 2 \
               -t 2700 ../temp_cnf/mc2025_track1_045.cnf
```

### Result
```
# solutions
132951278067432
time: 2429.5s   (≈ 40.5 min)
```

- `decisions = 717,595,836`
- `avg_bcp/dec = 1.19021`
- L2: 730.3 M stores, 204.6 M hits (28% hit rate)
- L1: 230.1 M stores, 195.1 M hits
- 22,539 learned clauses
- `closed_bits = 90` (full close), `n_open_comps = 0`

### Progress trajectory (per minute, abridged)

| t (min) | closed_bits | pct_lin | decisions |
|---:|---:|---:|---:|
| 2 | 83.64 | 1.2% | 44.6 M |
| 5 | 85.69 | 5.0% | 107.4 M |
| 10 | 87.83 | 22.2% | 208.2 M |
| 15 | 88.92 | 47.3% | 307.7 M |
| 20 | 89.68 | 80.0% | 395.4 M |
| 25 | 89.90 | 93.1% | 481.7 M |
| 30 | 89.91 | 93.6% | 563.4 M |
| 33 | 89.93 | 95.3% | 608.7 M |
| 37 | 89.96 | 97.6% | 665.8 M |
| 40 | 89.98 | 98.5% | 710.1 M |
| 40.5 | 90.00 | 100.0% | 717.6 M (SOLVE) |

The original (pre-optimization) wl2 run reached `closed_bits = 89.93` at t=20 min and `closed_bits = 89.96` at t=45 min then timed out. This run matched 89.93 at t=33 min (12 min earlier) and **finished the deep tail in the remaining 7-8 min** of budget the original never had access to.

### Verification

Ganak (default `--td 1`, `--maxcache 24000` for 24 GB, 4-hour budget) **confirmed the count** in 2772.81 s ≈ 46.21 min:

```
ganak --maxcache 24000 ../temp_cnf/mc2025_track1_045.cnf
...
c o intermediate count: 132951278067432
c o Total time [Arjun+GANAK]: 2772.81
c s exact arb int 132951278067432
```

- **Same count** `132,951,278,067,432` produced by an independent solver code base.
- The 16-GB cache used in prior ganak attempts was insufficient — those runs timed out around 40 min on the cache-thrash phase. 24 GB was enough to push through. The `c o cache pollutions call/removed 114879/7335588` and `c o cache miss rate 0.349` lines from ganak's final stats show the cache eviction was substantial even with 24 GB; smaller caches make this instance effectively unsolvable for ganak in any reasonable time.
- **Wall-time comparison on this instance**:
  - sharpSAT (this commit chain): **2429.5 s** (40.49 min)
  - ganak `--maxcache 24000 --td 1`: **2772.81 s** (46.21 min)
  - sharpSAT is **~12% faster** on this instance under these configs.

### Reproducibility note

Trajectory varies modestly between runs at the same wall time (PROGRESS emit is wall-clock-triggered, decision rate fluctuates ±5% from CPU jitter), but the search itself is deterministic (decisions → closed_bits is fixed for a given input + flags + binary). The 40.5-min solve time should reproduce within seconds across runs on the same machine, provided no other CPU load.


## 2026-05-21 — Portfolio finding: separator branching HURTS on binary-heavy / structurally-wide instances (t1_059)

Sweeping a refined 12-config portfolio (drops `clausesFirst`, `picker_vanilla`, one of the redundant ND-kill picker variants; adds `adaptive+wl2`) on **mc2025_track1_059.cnf** (264 vars, 1552 clauses, 1392 binary + 28 length-6 + 132 length-8, density 5.9) revealed that the *separator-branching machinery is actively harmful* on this instance class. Removing `-sep -cb` produced a **+12 closed_bits jump** on the best picker config.

### Instance structure
- 264 vars; **1392 binary clauses (90% of total)** + only 160 long (all length 6 or 8 — clear structured encoding signature, possibly cardinality / cryptographic / combinatorial design).
- Variable-incidence graph: avg degree ~15. Too dense for nested-dissection cuts.

### Decomposition tool agreement that the graph is structurally wide
- **METIS (ours):** `NDHierarchy: 437 tree nodes, 17 internal (sep), 201 passthrough, 219 leaves, 124 total sep elements, max_sep=43, sep_buckets=[1-2:11, 3-4:0, 5-7:0, 8-15:5, 16-31:0, 32-63:1, 64+:0]` — only 17 separator nodes out of 437 total; one of size 43 at the root.
- **Flowcutter (ganak):** `nodes: 324 ... [td] Calculated TD width: 97 ... centroid bag id: 0 bag size: 88` — treewidth 97 on 324 nodes (≈30 % of n).
- Both tools agree: **structural-width / n ≈ 30 %**. No useful nested-dissection cuts exist.

### v2 12-config sweep (60 s each, all TIMEOUT)

| Rank | Config | closed_bits | pct_lin |
|---:|---|---:|---:|
| 1 | plain+wl2+react | 239.54 | 4.3e-6% |
| 2 | triple+lockstep | 239.48 | 4.1e-6% |
| 3 | adaptive+wl2 | 239.34 | 3.8e-6% |
| 4 | plain | 239.21 | 3.4e-6% |
| 5 | plain+react | 239.15 | 3.3e-6% |
| 6 | legacy_sepVarBias | 239.13 | 3.3e-6% |
| 7 | adaptive | 238.88 | 2.7e-6% |
| 8 | picker_cascade2 | 233.83 | 8.3e-8% |
| 9 | picker_alpha100 \| picker_rootSepOnly \| picker_+react \| triple_no_lockstep | 230.49 (identical) | 8.2e-9% |

**Four picker variants converged to cb=230.49.** They make the same first-decision sequence under this instance's structure (picker scoring is deterministic and these flag overlaps don't change the early branches).

### No-sep mini-sweep (5 configs, 60 s each, all TIMEOUT)

| Rank | Config | closed_bits | pct_lin | Δ vs with-sep counterpart |
|---:|---|---:|---:|---:|
| **1** | **picker_cascade10d9_noSep** | **246.97** | 7.50e-4% | (new config) |
| 2 | picker_cascade2_noSep | 245.94 | 3.67e-4% | **+12.11** vs picker_cascade2 |
| 3 | picker_cascade5d6_noSep | 245.48 | 2.67e-4% | (new config) |
| 4 | plain_noSep | 240.86 | 1.08e-5% | **+1.65** vs plain |
| 5 | adaptive+wl2_noSep | 240.45 | 8.15e-6% | **+1.11** vs adaptive+wl2 |

Every config beat its with-sep counterpart. The picker-cascade family won by **+12 closed_bits** — a ~175× improvement in pct_lin terms.

### Diagnosis

The size gate at the root (`min(0.3·n_active, 100) = min(79.2, 100) = 79`) **accepts** the 43-element separator. So `-sep` forces every config to branch on those 43 *structural* vars first — even though they're not the variables with the strongest BCP cascade.

t1_059's 1392 binary clauses mean BCP cascades are massive (avg degree 10+ from binaries alone). The high-cascade vars and the structural-separator vars are essentially disjoint sets on this instance — and the structural ones are *worse* because:
- They don't trigger the deep BCP chains
- Picker / adaptive scoring (which knows about cascade depth) can't choose freely until the separator is exhausted

Removing `-sep -cb` lets the picker/adaptive use their own scoring (cascade-aware) directly. cascadeW=10, cascadeDepth=9 squeezes out the most progress: deeper chains visible to the score → better first-decision selection on binary-dominated structure.

### Test signal for "drop `-sep`"

> When **treewidth/n ratio ≥ ~25–30 %** (either reported by our METIS hierarchy as `max_sep/n_active`, or by flowcutter as `tw/n`), the separator is unlikely to disconnect the formula meaningfully and may force the search away from the high-BCP-cascade variables. Run the portfolio's `picker_cascadeNdM_noSep` variant instead.

### Portfolio implication

A **noSep cascade-deep** variant should be added as a permanent portfolio entry for binary-heavy / uniform-density / cryptographic-style instances:
```
-rec -sepMode metis -unifiedPicker -decomposeAfterK 1000 -cascadeW 10 -cascadeDepth 9
```

This is the **first instance class we've identified where the documented portfolio leaderboard is *wrong by 12+ closed_bits*** until we add a no-sep variant. The structural-width diagnostic (METIS sep + flowcutter tw) is the test of whether this matters.

### Open follow-ups
- Validate the noSep cascade-deep variant generalizes: try on at least one more binary-heavy instance with high tw/n.
- Check whether the same finding applies to other untouched instances in the queue (t1_003, t1_005, t1_163, t1_159, t1_053, t1_105) — all are size-class candidates for the same diagnostic.
- Should the solver auto-detect tw/n ratio and skip `-sep` runtime-internally? Currently the size gate handles only the *upper* size threshold; we may want a *ratio* gate too.


## 2026-05-21 — Derivative-cache probe Phase 1: STRONG positive signal

A new diagnostic flag `-derivCacheEvery N` (default 0 = off) was added. At every Nth `solveComponentImpl` cache miss it hypothetically fixes each of the top-K=5 active variables to T and F (with full BCP propagation), computes the resulting sub-formula's "clause-XOR" fingerprint (XOR of per-clause random uint64 hashes over still-in-scope long clauses), checks against an `unordered_set<uint64_t>` of every L2-stored sub-component's XOR (cheap pre-filter), and on a positive pre-filter does a full canonical-key lookup. Logs hits; does **not** modify search behavior. Counts verified unchanged on t1_065/t1_071/t1_041 with and without the flag.

### Phase 1 measurement (60 s, `-derivCacheEvery 100`)

| Instance | Probe sites | XOR-filter hits | Real (canonical-confirmed) hits | Real hits / probe site |
|---|---:|---:|---:|---:|
| **t1_045** (`-rec -sep 5 -cb 3 -sepMode metis -adaptive -wlIter 2`)         | 237,000 | 340,090 | **246,930** | **1.04** |
| **t1_059** (`-rec -sepMode metis -unifiedPicker -decomposeAfterK 1000 -cascadeW 10 -cascadeDepth 9`) | 117,000 | 474,656 | **168,776** | **1.44** |

Each probe site checks ~10 derivative candidates (K=5 vars × ±1 polarity). Both instances find on average ≥ 1 cached neighbor per probe site — far above the 5% threshold we set as the go/no-go bar.

The XOR pre-filter is doing real work:
- t1_045: 14 % of derivative checks pass the XOR filter; of those, 73 % confirm a real canonical-key hit (rest are XOR collisions correctly rejected by the canonical key).
- t1_059: 41 % pass the XOR filter; 36 % of those confirm.

### What this means

Even at the conservative every-100 probe rate, each second of search produced ~4 K cached-neighbor matches. The probe overhead at this rate is modest (~5 % wall-time bump on t1_041 in tests). Extrapolating to probe-at-every-miss (Phase 2 plan), the hit count would scale ~100 ×.

If we can convert "real hit" into "skip that branch of the upcoming branchOnLiteral and use the cached count directly," each hit saves a full sub-tree recursion. This is potentially a *substantial* speedup on cache-heavy instances like both t1_045 and t1_059.

### Implementation summary

- Config: `-derivCacheEvery N` (default 0), `-derivCacheTopK K` (default 5).
- New Solver members: `long_clause_hashes_` (lazy `unordered_map<ClauseOfs, uint64_t>`), `deriv_cache_xors_seen_` (`unordered_set<uint64_t>`), throttle counter + 3 stats.
- New Solver methods: `deriv_cache_init_`, `deriv_cache_component_xor_`, `deriv_cache_record_store_`, `deriv_cache_probe_`.
- Existing-code touches: 1 line at solve() init, 2 lines at L2 cache `store(...)` sites, 1 line at solveComponentImpl entry. All guarded by `config_.deriv_cache_every > 0`.
- ~190 lines total. No correctness risk to existing flows.

### Phase 2 plan (next session)

1. Probe at every miss (or every-N with small N) — measure raw throughput cost.
2. Actually use the hits: pick branch variable from probed candidates whose XOR matched, return cached count for one arm, recurse only on the other.
3. Heuristic to choose which var: prefer both-arms-cached (both branches free), else any cached arm. Among ties, use picker score.
4. Validate counts on the standard small-CNF set.
5. Test whether this lets us solve t1_059 or push significantly past the cb=249 plateau.

### Open follow-ups (Phase 3+)

- Migrate `long_clause_hashes_` to a vector indexed by ClauseOfs for cache locality.
- Replace `unordered_set` of seen XORs with a Bloom filter — drops memory from ~190 MB (at 12 M entries) to ~12 MB.
- Add clause-deletion derivative probes (currently var-branching only).
- Incremental WL canonical-key computation — defer until profiling shows canonical-key cost dominates the probe.


## 2026-05-23 — canonical_key free-var invariance fix → t1_045 in 34.87 min (13.9% faster than historical 40.5 min, 24.5% faster than ganak)

The L2 cache stores **structural** sub-component counts and multiplies back by 2^free_vars at retrieval, so `canonical_key` MUST be invariant to free-var addition. The implementation broke that invariant in two places:

1. `canonical_key.cpp` line ~294: free vars (sig == 0) were included in `s_sig_pairs`, shifting the 1..k anchor canonical_ids of in-clause vars. Fix: filter `s_sig[i] != 0` before pushing.
2. `canonical_key.cpp` line ~465: `RESIDUAL_OFFSET = max_var + 1` — `max_var` includes free vars, so when WL refinement falls back to residual IDs, the offset moved with free-var count. Fix: use the fixed sentinel `1 << 30`.

Both fixes are needed; either alone is insufficient. Two regression tests added (`tests/test_canonical_key_free_var.cpp` initial-state + `tests/test_canonical_key_free_var_midsearch.cpp` mid-search) to prevent regression of the documented-but-untested invariant.

### t1_045 result (winning config, post-fix)

Invocation:
```
SHARPSAT_PROGRESS=1 ./sharpsat-separator/build/sharpSAT \
    -rec -sep 5 -cb 3 -adaptive -wlIter 2 -t 2700 \
    temp_cnf/mc2025_track1_045.cnf
```
(equivalent to the 2026-05-20 historical command minus the dropped `-sepMode metis`)

| metric | 2026-05-20 historical | **2026-05-23 with fix** | delta |
|---|---:|---:|---:|
| **wall time** | 2429.5 s (40.5 min) | **2092.2 s (34.87 min)** | **−13.9 % / −5.62 min** |
| decisions | 717.6 M | 682.1 M | −5.0 % |
| L2 stores | 730.3 M | 680.4 M | −6.8 % |
| L2 hits | 204.6 M | 226.7 M | +10.8 % |
| L2 hit rate | 28.0 % | **33.3 %** | +5.3 pp |
| count | 132 951 278 067 432 | 132 951 278 067 432 | ✓ identical |

Versus ganak `--maxcache 24000 --td 1` (46.21 min, same instance, same count):

| solver | wall | speedup vs ganak |
|---|---:|---:|
| ganak 24 GB | 2772.8 s | — |
| sharpSAT 2026-05-20 | 2429.5 s | 1.14× |
| **sharpSAT 2026-05-23 + canonical_key fix** | **2092.2 s** | **1.33× (24.5 % faster)** |

### Independent A/B on t1_049 (same session, 2026-05-23)

Same binary, no other config differences, NOFIX vs WITH-fix on `-rec -sep 5 -cb 3` defaults:

| metric | NOFIX | WITH fix | delta |
|---|---:|---:|---:|
| time | 385.7 s | 353.6 s | **−8.3 %** |
| decisions | 153.8 M | 145.3 M | −5.5 % |
| L2 stores | 160.8 M | 149.7 M | −7.0 % |
| L2 hit rate | 20.4 % | 25.4 % | +5.0 pp |
| count | 8695763196077742 | 8695763196077742 | ✓ |

Same direction as t1_045: fewer duplicate stores, more hits, faster wall. Count cross-confirmed.

### Counts verified across the standard suite

t1_065 (37 778 931 862 957 161 709 568), t1_071 (4 562 956 847 836 …), t1_011 (536 870 912 306), t1_041 (5 516 767 … 943 259 892 051 805 732 864), t1_049 (8 695 763 196 077 742), t1_045 (132 951 278 067 432) — all identical to historical entries, no regressions from the fix.


---

## 2026-06-03 — Portfolio pipeline validation sweep (step-1 router + step-2 race)

End-to-end portfolio pipeline (`COCOA/portfolio/pipeline.py`, commit `efee5f9`):
step 1 routes to an engine family (small METIS separator → COCOA; substantial
Arjun reduction → Ganak; else UNDECIDED), step 2 races curated config archetypes
and resumes the frontrunner(s) via SIGSTOP/SIGCONT. Counting soundness checked
**differentially**: pipeline count vs an independent reference (`ganak --prob 0`,
or a documented ground-truth). Any mismatch = soundness bug.

Build: COCOA Release (`cocoa/build/sharpSAT`, `ganak-canonical/build/ganak`).
Env: clean CPU (verified no stray solvers), load ~1.8, Apple Silicon.

### Batches 1+2 — primary instances, budget 90 s (round1 15 / round2 15)

| inst | route | winner config | status | wall | count check |
|---|---|---|---|---:|---|
| t1_065 | COCOA | cocoa-plain | solved | 1 s | ✓ known |
| t1_071 | COCOA | cocoa-plain | solved | 0 s | ✓ known |
| t1_011 | COCOA | cocoa-plain | solved | 12 s | ✓ known |
| t1_041 | COCOA | **cocoa-reactive** | solved | 16 s | ✓ known (plain stuck pct_lin 2e-11) |
| t1_159 | Ganak | ganak-canonical | solved | 2 s | ✓ known |
| t1_025 | COCOA | cocoa-plain | solved | 4 s | ✓ ganak-ref (134746112245856) |
| t1_027 | COCOA | cocoa-plain | solved | 3 s | ✓ ganak-ref (1115259056499565) |
| t1_073 | COCOA | cocoa-plain | solved | 0 s | ✓ ganak-ref |
| t1_005 | COCOA | cocoa-plain | solved | 6 s | unverified (ref timeout) |
| t1_053 | COCOA | **cocoa-adaptive-nosep** | solved | 78 s | unverified (ref timeout) |
| t1_023 | COCOA | — | timeout | 90 s | — |
| t1_001 | COCOA | — | timeout | 90 s | — |
| t1_003 | COCOA | — | timeout | 90 s | — |
| t1_021 | COCOA | — | timeout | 90 s | — |
| t1_059 | COCOA | — | timeout | 90 s | — |
| t1_047 | UNDECIDED | — | timeout | 90 s | — |
| t1_149 | UNDECIDED | — | timeout | 96 s | — |
| t1_163 | UNDECIDED | — | timeout | 90 s | — |

**Headline: 0 count mismatches across all 10 solved instances** (8 independently
verified; 2 solved-but-reference-also-timed-out). 8 time out at the 90 s scout
budget — the genuinely hard ones (need the full ~3600 s budget).

Observations:
- Routing: mostly COCOA; Ganak on t1_159 (Arjun 256→117); UNDECIDED on t1_047/149/163.
- The race auto-selects non-default configs: **cocoa-reactive** wins t1_041
  (plain stuck), **cocoa-adaptive-nosep** wins t1_053.
- Every solved instance SHORT-CIRCUITED (a config finished within a round window),
  so the comparator/leader-pick is not yet exercised in producing a count.
- t1_023 (density-1 structured, t1_041 family) timed out — likely needs reactive
  but didn't short-circuit in the 15 s window; a comparator/window case to study.

### Batch 3 — public-set scout (previously-untested), budget 180 s (round1 20 / round2 15)

Decompressed from `MC2025_Public/mc2025_track1_public/` → `temp_cnf/mc2025_public/`.

| inst | size | route | winner config | status | wall | count check |
|---|---|---|---|---|---:|---|
| t1_067 | 150 v / 800 c | COCOA | cocoa-plain | solved | 0 s | ✓ ganak-ref (1267650600228229401496703205376) |
| t1_069 | 880 v / 2504 c | COCOA | cocoa-plain | solved | 6 s | ✓ ganak-ref (856…840, 114 digits) |
| t1_167 | 270 v / 18224 c | **UNDECIDED** | **cocoa-adaptive** | solved | 58 s | ✓ ganak-ref (244826150) |
| t1_101 | 729 v / 3174 c | COCOA | (nosep-cascade leader) | timeout | 180 s | — |
| t1_007 | 1441 v / 2575 c | COCOA | (plain leader) | timeout | 180 s | — |
| t1_013 | 1542 v / 4125 c | COCOA | (nosep-cascade leader) | timeout | 180 s | — |

**0 mismatches** (3 solved, all ganak-ref verified). Notes:
- **t1_167** = first comparator-EXERCISED solve: routed UNDECIDED, raced all 6,
  `cocoa-adaptive` short-circuited at 58 s (dense instance, 270 v / 18k clauses).
- t1_101/007/013 timed out at the 180 s scout budget but DID exercise the
  within-COCOA leader-pick (101/013 → nosep-cascade, 007 → plain). t1_013 gave a
  clean leader signal: nosep-cascade pct_lin 50.67 @20 s vs all others <18.
- These need the full ~3600 s budget; logged for the multi-day runs.

### Harness: streaming visibility (2026-06-03)

`race/sweep.py` + `scheduler.py` now stream every solver-option window result and
every instance verdict live (flushed; run with `python -u`) and append a durable
per-instance JSONL (`portfolio/sweep_log.jsonl`), so soundness problems are caught
as they arrive rather than at the end of a buffered run.

### closed_bits-velocity two-frontrunner policy + t1_013 (commit 9f54ed4)

COCOA progress is now judged by **closed_bits** (structure resolved) and its
**velocity** (recent back-half slope), NOT pct_lin — pct_lin flatlines at ~0 on
deep-tail instances (t1_001: every config 1e-18…1e-43) even while progressing.
Round 2 now fires WITHIN COCOA: round 1 scouts all configs, then keeps two
frontrunners — #1 highest closed_bits (leader), #2 highest closed_bits velocity
(rising challenger; tiebreak → runner-up by level). Fixes the old per-engine cull
that kept one COCOA config and skipped round 2.

**t1_013** (COCOA-routed, sep_ratio 0.007), budget 600 s, round1/2 = 30 s:

| config | closed_bits | velocity | pct_lin@30s |
|---|---:|---:|---:|
| cocoa-plain | 1195.2 | 0.571 | 0.003 |
| cocoa-reactive | 1181.7 | 0.078 | ~0 |
| cocoa-adaptive | 1207.5 | 0.000 | 18.3 |
| cocoa-adaptive-nosep | 1207.1 | 0.000 | 13.3 |
| **cocoa-nosep-cascade** | **1209.1** | 0.008 | **53.8** |

Frontrunners = nosep-cascade (#1 level) + plain (#2 velocity); round 2 fired;
leader = nosep-cascade. Round 3 → **TIMEOUT at 600 s** (no count): pct_lin climbed
53.9% → 61.9% through hard plateaus (flat at 60.05% for ~90 s) while closed_bits
barely moved (1209.26 → 1209.31) — slow-but-finishing deep tail, needs >600 s.
`ganak --prob 0` also timed out (180 s) → hard for BOTH engines (treewidth
correlation). Validates the policy: correct leader pick + honest "needs more
budget" reporting (no more `winner=None`). nosep-cascade is the right config here
(binary-heavy → drop separators).

---

## 2026-06-04 — Step-1 router: whole-ND-tree branching cost (nd_cost) replaces sep_ratio

**PROVISIONAL — this needs more work before the gate is trustworthy (see caveat below).**

New tool `cocoa/tools/nd_cost.cpp` (→ `build/nd_cost <cnf>`) builds the full
nested-dissection hierarchy (`NDHierarchy::build`, the exact recursive METIS
bisection the solver uses) and reports `nd_log2_cost` = L(root) in bits, where
`cost(C)=2^{|Sep(C)|}·Σ cost(child)`, leaf cost `2^{n_vars(leaf)}`, evaluated in
log2 (overflows otherwise). No depth weight needed — the nested `2^{sep}` already
weights deep separators by reach. Ignores caching/BCP.

Router wired (`portfolio/select_solver.py` rule 1 → `cocoa_feasible(nd)` =
`nd_log2_cost ≤ NDCOST_MAX_BITS`, **B=90**). Replaces the single-root-cut
`sep_ratio ≤ 0.2` gate, which a small ROOT separator passed even when the
instance was high-treewidth deeper.

### Calibration sweep (`portfolio/nd_calibrate.py`, 23 base t1_*, sub-50 ms each)

| band | instances (`nd_log2_cost`) | reality |
|---|---|---|
| low 18–40 | t1_003(17.9✓solved), t1_065(20), t1_071(38); **t1_021(28.8 timeout), t1_059(34.7 timeout)** | mostly COCOA |
| mid 50–77 | t1_041(52.5), **t1_049(61.9 beats ganak)**, t1_047(67.2 solves), t1_013(73.8 timeout), **t1_045(75.3 win)**, t1_001(76.9 timeout) | **MIXED, inseparable by cost** |
| catastrophic ≥100 | t1_007(101.6), t1_159/163(118), t1_011(208), t1_149(256) | hopeless, high confidence |

### Why B=90 is high, and the caveat

The cost **ignores caching/anonymization — COCOA's main lever** — so it
overestimates exactly the dense cache-friendly wins (t1_049 = 61.9 yet beats
ganak; t1_045 win sits between t1_001/t1_013 timeouts). So today it is only a
reliable **high-end filter**: reject the ≥100 tail (wide empty gap 76.9→101.6),
keep everything ≤90 on COCOA where step-2's race + fallback does the real
selection. To make it a real "will COCOA win" predictor: (1) caching-aware cost
correction; (2) calibrate against solve-TIME, not the 600 s-timeout bucket;
(3) revisit bipartite-vs-vars_only for binary-heavy instances (t1_059).

Improvement over the old gate is real in both directions: rejects small-root-but-
deep-hard instances it wrongly took (t1_007/011/149/159/163), and accepts
cache-friendly large-root wins it wrongly rejected (t1_049: sep_ratio 0.51 →
UNDECIDED before, cost 61.9 ≤ 90 → COCOA now).

---

## 2026-06-04 — First end-to-end run of the band-driven pipeline (t1_005)

First validation of the new architecture (step 1 = features only; step 2 =
band-driven `race_plan`; no binary COCOA/Ganak gate). Fresh, previously-untested
instance.

**t1_005** (378 vars, `nd_log2_cost=17.8` → band **low**):
- **Step 1** (features only): `nd_band=low`, Arjun **skipped** (low band, as designed).
- **Step 2**: band=low → raced `[cocoa-plain, cocoa-adaptive]` (sep configs), patient
  Ganak fallback (`<1.0%`).
- **Winner: cocoa-plain** (`-sep 5 -cb 3`), round-1 short-circuit, **5.7 s active**.
- **Count: 6526525587659843842641540018917626900729839747072** —
  **VERIFIED** identical to `ganak --prob 0`.

Findings:
- The whole new path — features → band → band-selected race → solve → count — worked
  first time, no bugs, no leaked processes.
- Confirms the **low band → precomputed sep** rule: t1_005 is an 8th low-band sep win
  (cross-tab was 7/7; now 8/8). The low-band signal remains the reliable part.
- Live monitoring via the Monitor tool streamed each transition ([step1]/[step2]/
  [r1]/[FINISH]) as it happened — the corrected reporting mechanism (Monitor on the
  log, not ScheduleWakeup) works.

---

## 2026-06-04 — Band-driven pipeline run #2: t1_041 (mid band, Arjun signal)

Second end-to-end run of the new architecture; first **mid-band** + first time the
**Arjun signal** fired live.

**t1_041** (1920 vars, `nd_log2_cost=52.5` → band **mid**, `worst_leaf=1`, `root_sep=0`):
- **Step 1**: `nd_band=mid`, **`arjun_substantial=True`** → bumped the Ganak-handoff
  threshold `fallback_pct` 2.0 → **5.0** (×2.5), as `race_plan` specifies.
- **Step 2**: band=mid → raced `[cocoa-plain, cocoa-reactive, cocoa-nosep-cascade]`.
  - `cocoa-plain` scouted its full 120 s window, **stuck at pct_lin ≈ 0** (5.4e-9),
    5.5 M decisions — confirms plain is the wrong config for t1_041.
  - `cocoa-reactive` then **finished in 1.1 s active** (round-1 short-circuit).
- **Winner: cocoa-reactive** (`-sep 5 -cb 3 -wlIter 2 -reactiveMetis -reactiveMetisMin 10
  -reactiveMetisSkip 4 -unifiedPicker -decomposeAfterK 1000 -cascadeW 0`).
- **Count VERIFIED**: 458-digit value `…057082925443259892051805732864`, **identical to
  `ganak --prob 0`**. (Caught + fixed a transcription typo in the old line-1316 abbrev:
  `…943…` → `…443…`.)

Findings:
- Confirms the **mid band → diverse-trio race** correctly surfaces the **reactive**
  regime as the winner — the first non-sep live win.
- The **Arjun signal works end-to-end** (raised the handoff threshold). Worth noting the
  latent tension: a *true* COCOA win (t1_041=reactive) had `arjun_substantial=True`, so
  had reactive needed > round-1 budget, the raised 5% threshold could have prematurely
  bailed to Ganak. Here reactive short-circuited first, so it didn't bite — but it's a
  case to watch as we run more Arjun-substantial mid-band instances.
- Verify-before-claiming paid off: the count tail looked off vs the doc, an independent
  ganak run settled it (doc typo, solver sound).

---

## 2026-06-04 — Band-driven pipeline run #3: t1_011 (HIGH band — the caching exception)

The decisive test of the "no binary gate" decision. t1_011 is why the B=90 gate was
removed: cost 208 bits (2nd-highest in the set) yet a fast precomputed-sep win because
caching annihilates its deep-but-repetitive tree.

**t1_011** (6559 vars, `nd_log2_cost=208.3` → band **high**, `worst_leaf=1`, `root_sep=0`):
- **Step 1**: `nd_band=high`, `arjun_substantial=True` → `fallback_pct` 5.0 → **10.0** (×2.5).
- **Step 2**: band=high → raced `[cocoa-plain, cocoa-nosep-cascade]` (short sep probe + nosep).
- **Winner: cocoa-plain** (`-sep 5 -cb 3`), **11.6 s active**, round-1 short-circuit.
- **Count: 536870912306** — matches the known-verified reference exactly. ✓

Findings:
- **The high-band short-sep-probe earns its place.** Keeping `cocoa-plain` in the high
  band rescued exactly the caching win that a hard cost-gate (B=90) would have wrongly
  routed to Ganak. This is the live confirmation of the cross-tab lesson and of the
  decision to make nd_cost a step-2 *signal*, not a verdict.
- All 3 runs now solved + verified across all three bands: low→sep (t1_005), mid→reactive
  (t1_041), high→sep-via-caching (t1_011). The band-driven race picked the right regime
  each time.

---

## 2026-06-04 — Per-config progress trajectories on t1_049 (answering "good progress over time")

`race/trajectory.py` t1_049 (mid band, 90 vars), 5 COCOA configs × 60 s, 10 s sampling.
No config solved in 60 s (t1_049 is a long grind); this is a PROGRESS characterization.

pct_lin (%) over time:
| config | t10 | t20 | t30 | t40 | t50 | t60 |
|---|---|---|---|---|---|---|
| cocoa-adaptive       | 1.12 | 3.66 | 3.83 | 3.86 | 4.42 | —    |
| cocoa-adaptive-nosep | 1.12 | 3.66 | 3.83 | 3.86 | 4.42 | 5.43 |
| cocoa-plain          | 0.34 | 0.76 | 1.10 | 1.38 | 1.55 | —    |
| cocoa-reactive       | 0.14 | 0.36 | 0.67 | 0.78 | 1.05 | 1.15 |
| cocoa-nosep-cascade  | 0.01 | 0.03 | 0.04 | 0.06 | 0.12 | 0.18 |

closed_bits is compressed (all 77–86 by t10 — weak discriminator); pct_lin spreads 100×.

How to evaluate "good progress over time" (data-grounded):
- closed_bits = EARLY signal (structure resolved, jumps then plateaus); pct_lin = OVER-TIME
  signal (separates configs). Good progress = sustained positive pct_lin velocity.
- Thrashing = decisions climb but pct_lin flat (nosep-cascade: 15 M decisions, pct_lin ≈ 0).
- A plateau is ambiguous for one sample (adaptive t30–40 looked stalled, then t50 jumped
  4.42 — a pause between deep-tail steps). Need the NEXT sample to call pause vs stall.

Findings:
1. **`-sep` is a no-op on t1_049** — cocoa-adaptive ≡ cocoa-adaptive-nosep bit-for-bit
   (root_sep=46 poor → hierarchy rejected by size-gate; `-adaptive` is the whole story).
2. **cocoa-adaptive is the best config (~3× plain) — and it's MISSING from race_plan("mid")
   = [plain, reactive, nosep-cascade].** The old "plain wins t1_049" was just the
   benchmarked config. So the mid race currently misses t1_049's best config.

Action (to discuss): add cocoa-adaptive to the mid set (or use root_sep: large root_sep
mid-band → prefer adaptive over plain). Also: closed_bits-first ranking is weak when the
range is compressed; pct_lin is the more sensitive signal on instances like t1_049.

---

## 2026-06-04 — t1_023 clean run validates the redesigned step-2 (6->3->1 + forecast)

t1_023 (low band, cost 27.6). Full 1h budget (3600s), new scheme. Result: TIMEOUT,
decided=ganak-native, count=None — UNSOLVED by both engines (hard-for-both, like t1_001/007).

Flow validated end-to-end (all as designed):
- 6 configs x 60s round 1; NO early bail (the old round-1 fallback bug is gone).
- frontrunners = top-3: adaptive(cb94.8)+plain(cb94.7) by level + unified-sep(vel0.066) accelerator.
- round 2 = 3 x 120s; leader = cocoa-plain (cb 96.5).
- [forecast] cocoa-plain -> GANAK (power-law on r: B=-0.236, R2=0.999, proj 14.4% @budget).
- [handoff] -> ganak-native got the FULL 2880s (~48min), 14.8-16.6GB. Timed out.

Forecaster post-mortem: the power-law-on-r=log2(100/pct_lin) is too pessimistic (projects 14%);
pct_lin is ~linear (~0.127%/10s) and a linear fit projects ~40% (finish ~129min) — still >1h,
so Ganak was the right call here, but the model over-pessimizes borderline cases. DECISION:
switch to linear-regression-on-pct_lin + a caching JUMP BONUS (big closed_bits increments ->
shrink ETA), since cache hits cause jumps and compound. NOT yet implemented.

## 2026-06-04 — t1_073 clean solve (new pipeline, round-1 short-circuit)
t1_073 (band=low, cost 33.9; tiny root cut sepsize=3/1140 vars). 6-config set;
cocoa-plain solved round-1 in 0.3s active. count ...356703232000 VERIFIED vs ganak --prob 0.
Forecaster not involved (short-circuit). Confirms new selection routes + short-circuits fine.

## 2026-06-04 — t1_003 clean solve + VERIFIED
t1_003: 264 vars, 1474 clauses (98.6% BINARY: 1453 len-2 + 1 unit + few mid + 11 len-12).
Structure: binary-heavy but LOW treewidth — metis one-shot cut=19, but ND root_sep=1,
worst_leaf=2, max_path=13, cost=17.9 (LOW band). cocoa-plain (sep+canonical) solved
round-1 in 30.8s active. count 1028511699317580344617598976 VERIFIED vs ganak --prob 0
(ganak: 174K cache entries, ~7500 confl/s; slow only due to its own distill/cadiback preproc).
Lesson reinforced: binary fraction does NOT decide sep-vs-nosep; treewidth does. t1_003 is
binary-heavy + narrow -> separator WINS (contrast noSep-for-binary-heavy which needs tw/n>~25%).

## 2026-06-04 — toolchain verification: t1_065 + t1_073 both reproduce (new pipeline, round-1 short-circuit)

Quick end-to-end toolchain check before any long benchmark. Env: CPU idle (no stray
solver procs), both builds Release (verified CMakeCache.txt), macOS. Invocation:
`python3 -u pipeline.py <cnf> --budget 3600` (round1=60s, round2=120s defaults);
winning config cocoa-plain = `-sep 5 -cb 3`, canonical hash (binary default, sound).
Both LOW band so cocoa-plain raced first and short-circuited in round 1 — forecaster
not involved.

- t1_065: band=low, cost=20.0, root_sep=4, sepsize=4/112 vars, balance=0.49. cocoa-plain
  solved round-1 in **0.1s active**. count `37778931862957161709568` — VERIFIED against
  BOTH documented ground truth and an independent `ganak --prob 0` run.
- t1_073: band=low, cost=33.9, root_sep=3, sepsize=3/1140 vars, balance=0.499. cocoa-plain
  solved round-1 in **0.2s active**. count `...356703232000` (170 digits) — VERIFIED against
  `ganak --prob 0` (exact match).

No count mismatches. Confirms step-1 routing + step-2 round-1 short-circuit work end-to-end
on the current build. (Re-confirms the earlier 2026-06-04 t1_073 solve; 0.2s vs 0.3s active.)

## 2026-06-04 — verification-gap closures: t1_005 + t1_053 now VERIFIED

Both were "solved but unverified (ref timeout)" in the Batches 1+2 scout. Re-run at full
`--budget 3600` with a patient independent `ganak --prob 0` cross-check. Env: CPU idle,
Release builds, macOS.

- t1_005 (band=low, cost 17.8, root_sep=0, sepsize=14/378): cocoa-plain solved round-1 in
  **5.6s** active. count `6526525587659843842641540018917626900729839747072` — VERIFIED vs
  ganak --prob 0 (exact). [previously unverified]
- t1_053 (band=low/boundary, cost 40.0, root_sep=20, sepsize=31/222): **exercised the round-1
  scout** — cocoa-plain (pct_lin→62.8%) and cocoa-adaptive (→76%) didn't finish their 60s
  windows; cocoa-unified-sep + cocoa-reactive FROZE at pct_lin 0.195% / closed_bits 213.0
  (vel 0); **cocoa-nosep-cascade** finished at **43.3s** active. count
  `95452062829869061511655876` — VERIFIED vs ganak --prob 0 (exact). [previously unverified]
  Reconfirms the nosep-family win (prior sweep: cocoa-adaptive-nosep) — separator configs
  stall on t1_053, no-separator configs win.

## 2026-06-05 — t1_101 (public): first full-budget run — COCOA→Ganak handoff validated end-to-end

t1_101 (729 v / 3174 c, public set), full `--budget 3600`. First on-this-build exercise of the
round-2 forecast + Ganak-handoff path; it fired correctly. Env: CPU idle, Release builds, macOS.

- step1: nd_cost band=**LOW** (cost 23.3, root_sep=8, sepsize=108/729). NOTE nd_cost UNDER-estimates
  this one — low cost yet COCOA can't crack it (mirror image of t1_011: high cost, secretly easy).
  Arjun not substantial.
- Round 1 (6×60s), closed_bits: nosep-cascade **583.1** (pct_lin 0.053%) > unified-sep 568.4 >
  reactive 556.0 > adaptive-nosep 555.5 > plain 511.0 > adaptive 473.6. Frontrunners = nosep-cascade,
  unified-sep, plain (velocity accelerator).
- Round 2 (3×120s): nosep-cascade stayed leader (cb 583.1, plateaued); unified-sep **DEAD** (7.8M
  decisions, closed_bits frozen at 568.42); plain glacial (cb 511→513).
- Leader = cocoa-nosep-cascade. **Forecast → GANAK** (B=−0.02 ≈ flat, ETA inf, proj pct_lin@budget 0.1%).
- Handoff: COCOA killed (free RAM), **ganak-native (`--prob 0 --maxcache 26000`) solved in 74.1s**.

count `4503632` — **VERIFIED**: ganak-native (`--prob 0`) and `ganak --cachehash canonical` agree
(Ganak-only since COCOA can't finish; cross-checked across cache paths). Small count (4.5M) yet
COCOA-hard — a clean COCOA-blind-spot / Ganak-strength case. The forecaster's known pessimism was
moot here (Ganak is genuinely the right engine).

## 2026-06-05 — t1_007 (high band): first full-budget run — TIMEOUT, hard-for-both now MEASURED

t1_007 (1441 v / 2575 c), first-ever full `--budget 3600` run (all prior data was ≤180s scouts).
Result: **TIMEOUT**, decided=ganak-native, count=None — UNSOLVED by both engines at 3600s. Env:
CPU idle at start, Release builds, macOS, `caffeinate -i` guarding (no idle-sleep mid-run).

- step1: band=**HIGH** (cost 101.6). METIS root cut sepsize=30/1441 (ratio 2.1%) — *small root, deep
  tree*: ND root_sep=0, max_path=99. **arjun_substantial=TRUE** (Ganak-favorable signal).
- Round 1 (6×60s) closed_bits: plain 586.0 (pct_lin 0.195%) > adaptive 585.0 > nosep-cascade 584.0 >
  unified-sep 583.1 > reactive 583.0 > adaptive-nosep 562.0. All pct_lin < 0.2%.
- Round 2 (3×120s): leader cocoa-plain stayed flat (cb 586.00→586.03, pct_lin 0.195→0.1996% on +22M
  decisions = 30.7M total). Forecast → GANAK (B=−0.00, ETA inf, proj 0.2%@budget).
- ganak-native (`--prob 0`, 26 GB) ran the remaining ~2822s (~47 min); RSS grew 5.9→11+ GB (actively
  counting, never emitted a parseable stat block) and did NOT finish → budget timeout.

Epistemic update: this is the **first measured full-budget evidence** that t1_007 is hard-for-both
(the prior "hopeless" label was only an nd_cost-band prediction + ≤180s scouts). Caveats: (1)
timeout at 3600s ≠ unsolvable — Ganak was still progressing (RSS climbing), may need more budget/RAM
or a different engine; (2) **arjun_substantial=TRUE did NOT yield a Ganak win** here — the
Ganak-favorable reduction signal over-promised on this instance (data point against over-trusting it).
No count to verify.

## 2026-06-05 — t1_013 (mid band): full-budget run — forecast→Ganak→TIMEOUT

t1_013 (1542 v / 4125 c), full `--budget 3600`. Result: **TIMEOUT**, decided=ganak-native, count=None.
Env: CPU idle at start, Release builds, macOS, `caffeinate -i` guarding. Almost certainly the instance
the earlier 600s-budget entry called "hard for both / nosep-cascade slow-but-finishing" (cb ~1209).

- step1: band=MID (cost 73.8). arjun_substantial=False.
- Round 1 (6×60s) closed_bits: **nosep-cascade 1209.1 (pct_lin 53.88%)** > adaptive 1207.5 (18.26%) >
  plain 1207.1 (13.53%) ≈ adaptive-nosep 1207.1 (13.28%) > unified-sep 1187.6 (~1e-5) > reactive 1185.7.
  nosep-cascade JUMPED 0→53.88% in 60s (config-specific, not BCP noise — every other early burst ≤18%).
- Frontrunner quirk: the #3 "accelerator" slot (closed_bits VELOCITY) went to unified-sep (pct_lin
  1.8e-5) over plain & adaptive-nosep (both ~13.5%) — the ~13–54% configs all plateaued (vel 0) while
  unified-sep had a tiny nonzero slope. The velocity-accelerator discarded two far-further-along configs.
- Round 2 (3×120s): leader nosep-cascade crept 53.88%→55.95% (cb 1209.11→1209.16); adaptive flat
  (18.26→18.31%); unified-sep negligible (0.39%).
- Leader = nosep-cascade (55.95%). FORECAST → GANAK: power-law B=−0.11, ETA 15984s (~4.4h), proj
  pct_lin@budget 65.9%. Handoff → ganak-native ran ~2822s (~47 min), never finished → budget timeout.

Forecaster data point (factual, for the pending rewrite — user is driving it): the leader was at
**55.95% pct_lin** and was handed to Ganak, which timed out. A constant-rate (linear) read of the SAME
back-half data projects ETA ~31–42 min (inside budget); the power-law's deceleration assumption +
back-half-only fit (which discards the 0→54% jump) produced the 4.4h ETA. Unknown whether nosep-cascade
would have finished round 3 (it was plateauing ~56%), but the handoff swapped a 56% COCOA leader for a
0% Ganak timeout. No count to verify.

## 2026-06-05 — t1_045: end-to-end framework SUCCESS (first durably-logged trajectory)

t1_045 (90 v, mid band cost 75.3), full `--budget 3600`. **SOLVED** — winner cocoa-adaptive-nosep,
round 3, active **2031.9s (~34 min)**, count `132951278067432` = **VERIFIED vs known ground truth**.
Env: CPU idle at start, Release builds, macOS. Durable: `portfolio/runlogs/t1_045.log` + the full
leader trajectory (48 pts, 0→97%) in `trajectory_log.jsonl` (first run captured under the always-log rule).

The framework got it right end-to-end:
- **step1:** nd_cost band=MID kept it on COCOA despite a BAD METIS root cut (sep_ratio 0.567, balance
  0.128) — the old `sep_ratio ≤ 0.2` gate would have mis-routed this to Ganak.
- **race:** adaptive family ranked top; `-sep 5 -cb 3` proved a NO-OP (adaptive ≡ adaptive-nosep — the
  ND hierarchy is rejected, so `-adaptive` is the whole story); leader = adaptive-nosep (edged adaptive
  by 0.0001 cb).
- **forecast** (β=2, MARGIN=0.8 at run time): base_eta ~105 min (linear rate ≫ margin) but the jump
  bonus B=6.15 → final ETA ~15 min → KEPT COCOA. **The bonus was load-bearing** — without it, or with a
  narrow 10%-margin gate, t1_045 mis-hands to Ganak. The counterexample proving the bonus is needed
  (earlier "not load-bearing" was a sampling artifact of the batch instances).
- **round 3:** super-linear tail collapse (86% → 97% → done in the last ~6 min — exactly the
  acceleration the bonus credited), solved at 2032s active.

Post-run forecaster knob changes (future runs only): β 2→1, MARGIN 0.8→0.9 — both still keep t1_045
(at β=1, final ETA ~28 min ≤ 0.9×budget). First real calibration data point; trajectory saved.

## 2026-06-05 — t1_009 (public, untried): COCOA 37.5s vs Ganak hours — the edge is ND+cache, NOT Arjun

t1_009 (5388 v / 12825 c, public set, first run). band=MID (nd_cost 94.1, root_sep 0), arjun_substantial=TRUE.
**SOLVED by cocoa-adaptive** (`-sep 5 -cb 3 -adaptive -wlIter 2`), round-1 short-circuit, active **37.5s**,
count **382252120612**. Durable: `runlogs/t1_009*.log` + `trajectory_log.jsonl`.

**Verification of `382252120612`:**
- COCOA `-l2Strict` (cache hit requires full clause-multiset match → cannot false-hit) → SAME count ⇒ the
  WL/canonical cache made no wrong reuse here (rules out a canonical-cache bug for this instance).
- Same config family produced t1_045's exact known count earlier (exact counting confirmed). Native ganak
  (identity hash, cross-codebase) is the fully-independent check but is hopelessly slow here (below).

**Ganak comparison (investigated why COCOA wins):**
- native `ganak --prob 0` (identity hash): **>45 min, UNFINISHED** (killed). ~70×+ slower.
- `ganak --cachehash canonical` (WITH Arjun): also slow. Arjun cut support 5388→~1758, but the residual had
  **tree-decomposition width 234** → ganak's TD-guided count is expensive.
- `ganak --cachehash canonical --arjun 0` (NO Arjun): **>13 min, UNFINISHED** (killed). Even without Arjun,
  ganak's TD of the connected core (2092 vars, after 3297 disconnected components) has **treewidth ~240** —
  same as with Arjun.
- **CONCLUSION: Arjun is NOT the culprit.** COCOA's edge is its DECOMPOSITION METHOD — METIS nested-dissection
  (5015 tiny leaves, worst = 1 var) + the WL canonical component cache collapsing isomorphic pieces
  (2⁹⁴-naive → ~30k decisions) — vs ganak's tree-decomposition (treewidth ~240), which the canonical cache
  can't rescue. COCOA runs no Arjun, so it sees the raw structure the cache feeds on.
- `arjun_substantial=TRUE` **over-promised again** (3rd time; cf. t1_007) — it did NOT mean Ganak-favorable.

**Soundness:** the count is from the real exact solver (mpz `# solutions` block), NOT the `-calibrateDive`
estimator (flag-gated, emits no count, used only by `calibrate.py`; the race never passes it).

---

## t1_089 (public, untried) — COCOA forecaster FALSE-POSITIVE, killed → native Ganak

**Instance:** 480 vars / 2040 clauses, nd_cost **204 (HIGH band)**, arjun_substantial=True.
**Pipeline:** full 6-config race → frontrunners [nosep-cascade #1 (cb level), plain #2 (level), unified-sep #3 (velocity)] → leader **cocoa-nosep-cascade** (`-unifiedPicker -decomposeAfterK 1000 -cascadeW 10 -cascadeDepth 9`).

**Round trajectory — the front-load trap:**
- r1 (60s): nosep-cascade **3.125%** — clear leader (others ~0.67%).
- r2 (→180s): **5.615%** — climb *looked* sustained (3.1→5.6), so the forecaster fired the jump-bonus.
- **Forecast @ r2-end → COCOA r3:** `eta 1362s ≤ 0.9×2879s; base 4328s /(1+B=2.18), rate=0.0218%/s, proj@budget=68.4%`. The linear base alone (proj **68.4%**) already says WON'T finish; the **bonus B=2.18 flipped it to "finishes"**.
- r3 reality: **plateau.** 5.6% → **7.13%** over ~23 min, decelerating to a steady **~0.04%/min** crawl (trough 0.009/min). At that rate 93% remaining ≈ **30+ hours**.
- **Killed at active 1561s / pct_lin 7.13% / cb 476.19**, ~25 min budget unspent (user call).

**VERDICT — forecaster false-positive.** The one-shot forecast at 180s had ONLY front-loaded samples (the fast r1→r2 climb, ~1.3%/min); the bonus over-credited those early jumps, and the slow post-jump regime did not exist in the data yet. → Motivates the agreed **round-3 re-check**: re-run `predict()` at ~5 min into r3 on the richer (post-jump) trajectory and hand to Ganak if eta>margin. Design agreed; **NOT yet implemented.**

**Handoff:** killed COCOA (frees RAM), launched **native Ganak** `--prob 0 --maxcache 26000 --verb 1` (sound; faster-than-canonical 5/5 fallback) → runlogs/ganak_089.log. Decomposition finished in 19.7s — but that's the smaller formula (480v vs 5388v), NOT tractability: **treewidth = 163** (vs t1_009's 238 — high either way). Counting then ran **1h 11m at 100% CPU, RSS flat ~238 MB** (CPU-bound search, not cache-filling) with **NO count** → killed (user). **t1_089 UNSOLVED by both engines:** COCOA plateaued at 7%, Ganak's tw-163 tree-decomposition count is infeasible in budget. Lesson (per user): a *small* formula (480v) can still be hard — treewidth is the wall, and tw 163 stops both paradigms (contrast t1_009, where COCOA's ND + WL-cache crushed Ganak's tw-238 TD; here neither wins).

---

## t1_015 (public, untried) — whole-portfolio WALL; predict_cb wired & validated

**Instance:** 4794v/13936c, HIGH band. All 6 configs converge to closed_bits ≈ **4785** (~0.96 bits short of n_root=4786, ≈ 50–51% pct_lin) and **freeze** — the last ~1 bit is intractable for COCOA. Not config-specific: high-decision plain (millions of decisions) and low-decision adaptive both stall at the same wall → genuine **Ganak case**.

**Leader-pick test:** the current scheme picked the *frozen* plain (closed_bits level) over the *climbing* adaptive. Killed plain, ran adaptive solo: it climbed 43.7→50.0% (cb→4785.00) then **walled too** (15 min frozen at 50.0008%, l2_hits stalled). "adaptive cracks it" — falsified.

**Forecaster replay (minute-by-minute on adaptive's trajectory):**
- **predict_cb (closed_bits):** ETA climbs as cb stays flat (596→3032s) → flips to **ganak at t≈540s (~9 min)**. Correct, timely handoff.
- **old predict (pct_lin):** stays `cocoa` throughout (eta 69→480s) — fooled by the early 0→50% warm-up jump + bonus → **false-positive**, would ride to a no-count timeout.

**Wired:** `predict_cb` now drives the scheduler's round-2 handoff (procctl `closed_bits_traj()`/`n_root_estimate()` + scheduler swap; 37 unit tests + dryrun 4/4). Remaining: a round-3 *monitoring re-check* to catch climb-then-wall leaders (t1_089-class) that pass the r2-end check then freeze.

---

## t1_123 (public, untried) — SOLVED by cocoa-plain (round-1 short-circuit; new funnel)

**HIGH band, arjun_substantial=True** — yet **cocoa-plain solved it in 23.3s** (round-1 short-circuit). The band over-predicted difficulty (sep-probe caught the structure, per the high-band note). **COUNT = 752061**, ganak-verified: native Ganak independently computed 752061 in 58.7s (cross-engine agreement; cocoa-plain 23.3s). Like t1_067, it short-circuited, so the ETA-narrowing funnel + monitoring handoff were not exercised — the new funnel's fast-finish path is confirmed correct on a 2nd real instance.

---

## t1_175 (public, untried) — SOLVED by cocoa-adaptive-nosep via the ETA funnel (NEW selection)

**HIGH band, NO separator** (root sep_ratio=0.73, sepsize=240/330, balance=0.0, nd_cost=317 bits, only 11 leaves), arjun_substantial=False. **Deep-tail**: pct_lin ~0 for most configs at round 1, so the new ETA funnel ranked by predict_cb's **closed_bits** forecaster (the exact case where the old pct_lin model gave 10^thousands garbage). Round-1 keep-4 by ETA = [adaptive-nosep, adaptive, nosep-cascade, plain] (cut unified-sep cb225, reactive cb222); **adaptive-nosep finished in round 2 at 68.4s**. **COUNT = 1048488033**, ganak-verified (native, 50.8s; cocoa-adaptive-nosep 68.4s). First real instance where the funnel narrowing mattered (no round-1 short-circuit) — it kept the eventual winner and solved.

---

## t1_031 (public, untried) — SOLVED via Ganak handoff (FIRST run of the 8-config set + memory gate)

**Instance:** 450v/12923c, **HIGH band** (nd_cost log2=312, root_sep=148, max_path=311, worst_leaf=30, **n_leaves=18**; metis root cut sep_ratio=0.49/balance=0.26/sepsize=219), arjun_substantial=False. The dense / low-decomposability corner — big root separator, few leaves → caching-hostile, Ganak-favorable structure.

**First end-to-end run of the new 8-config covering set** (adds cocoa-sep-cascade + cocoa-cache-max) + the round-1 memory-admission gate (R1_MEM_SKIP_GB=20) + per-round RSS logging.

**Scout (funnel 8→4→2→1):** narrowed to the two cascade configs, leader = **cocoa-sep-cascade** (a NEW config — earns its slot on this binary-heavy/cascade structure). RSS after r3 = 2.1 GB across 2 configs; the 20 GB admission gate never fired — confirms the "hard, low-cache instances stay small" finding (COCOA barely cached here; Ganak later did).

**Monitoring (cocoa-sep-cascade):** climbed closed_bits 404→**417** (incl. a real 414→417 jump at active ~721s) then **walled** at 416.999 for ~240s — ~50M decisions, pct_lin stuck ~4.65e-8% (deep tail, n_root≈448, ~31 bits short). **Handoff fired** at ~1696s elapsed (10 consecutive 10s forecasts of ETA > 1.25× remaining). NOTE: handoff took ~240s from the wall vs the 100s streak minimum — the extra ~140s is RATE_ALPHA=0.9's EWMA still "remembering" the pre-wall climb (~66s half-life). Candidate tuning knob (lower RATE_ALPHA → snappier bail) traded against stepwise-progress patience; here it cost ~4 min but did NOT change the outcome.

**Ganak handoff → SOLVED:** ganak-native (--prob 0, 26 GB) solved in **283.6s** (cache_K 4k→19k, ~2600 confl/s — found structure immediately where COCOA's caching couldn't). **COUNT = 164260022470**, authoritative (ganak --prob 0 deterministic exact; COCOA produced no count, so nothing to cross-check — a separate run reproduces it bit-for-bit). Total wall ~33 min of 60.

**Validates end-to-end:** 8-config scout + ETA funnel + monitoring/overshoot handoff + memory gate all worked on a real untried instance — a doomed COCOA grind correctly converted to a Ganak solve.

---

## t1_017 (public, untried) — SOLVED via Ganak handoff; FIRST live ARIMA-gated handoff

**Instance:** 3092v/8080c, **MID band** (nd_cost log2=98.8, **root_sep=0**, max_path=95, worst_leaf=2, **n_leaves=1544** — highly decomposable; metis sep_ratio=0.004/sepsize=13), **arjun_substantial=True**.

**Funnel (8→4→2→1, predict_cb):** the two adaptive configs led from round 1 (cb=2903 vs ~2877 for the grinding cluster) and were tied throughout — `cocoa-adaptive` (`-sep 5`) and `cocoa-adaptive-nosep` were **indistinguishable** (root_sep=0 → the static separator adds nothing; the adaptive picker does the work). 8→4 kept [adaptive, adaptive-nosep, reactive, cache-max]; 4→2 kept the two adaptives; leader = **cocoa-adaptive**. sep-cascade/nosep-cascade stuck at cb=2687 (cut round 1 — cascade doesn't fit a clean decomposition).

**MONITORING — first live `predict_cb_arima` handoff (per-10s trace in runlogs/forecast_mc2025_track1_017.jsonl):** leader walled at cb≈2905 (~40 bits short of n_root≈2946, pct≈5e-11). active 190–220s: ARIMA gave a fair chance — eta finite and *climbing* 933→1062s (mu eroding 0.044→0.038), over=F. active 230s: cb flatlined → **stuck-floor fired** (<0.5 bits over the last 90s) → eta=inf, over=T; streak built 1→10 over 230–321s → **handoff at active 321s (~140s into monitoring, ~110s after the wall — vs the old predict_cb's ~240s on t1_031)**.

**Ganak handoff → SOLVED:** ganak-native (--prob 0, 26 GB) solved in **4.7s** (arjun_substantial=True → heavy Arjun reduction). **COUNT = 3599131006304977231013044472083344916502383519383776808287612890357947726830001874874457511028743967475520422712474089947410146888355312205154237392283185971200000**, authoritative (ganak --prob 0 exact; COCOA produced no count).

**Validates:** ARIMA's stuck-floor + streak fired correctly on a real wall — faster and more principled than the old slope model, with the full 10s forecast trace captured. Prediction miss worth noting: "high decomposability → COCOA cache win" was wrong; `arjun_substantial=True` was the better signal (Ganak-favorable), and the handoff caught it.

---

## t1_019 (public, untried) — SOLVED via Ganak handoff; ARIMA's DISCRIMINATING test (creep→keep, wall→bail)

**Instance:** 1875v/4775c, **MID band** (nd_cost log2=66.6, root_sep=0, sepsize=6, worst_leaf=2, **n_leaves=941** — decomposable), **arjun_substantial=True**. First run with **ARIMA driving the round-2/3 funnel cuts** (not just monitoring) + the explicit 3-min fair-chance handoff guard.

**Funnel:** round-1 cut (8→4) by predict_cb → [adaptive-nosep, adaptive, cache-max, plain]; **round-2 cut (4→2) by ARIMA** → [adaptive-nosep, adaptive]; **round-3 cut (2→1) by ARIMA** → leader **cocoa-adaptive-nosep** (the no-sep adaptive config; root_sep=0 so the separator is moot, as on t1_017).

**MONITORING — the discriminating case (unlike t1_017's immediate wall):** leader CREPT first, then walled. active 180–330s: cb 1828→1830, eta ~870–907s, over=F, streak=0 — ARIMA correctly **KEPT** COCOA on slow-but-real progress (no premature bail; the keep-phase ETA ~870s was optimistic/stale-mu and wrong, but the decision is floor-driven, not ETA-driven). active ~341s: cb flatlined at 1830.1x → **stuck-floor fired** (eta=inf), streak 1→10 → **handoff at active 431s (~250s into monitoring)** — longer than t1_017's ~140s because the creep earned a fair chance.

**Ganak handoff → SOLVED in 15.1s.** **COUNT = 124330809073498638182336606522714496740835436618714014267086565149873565679807050547200**, authoritative (ganak --prob 0 exact).

**Verdict:** the forecaster did its job well on the case that actually tests it — it distinguished slow-progress (keep) from walled (bail), gave a fair chance, then bailed correctly (Ganak's 15s solve confirms). Routing note (not a forecaster issue): pct_lin≈1.6e-9 throughout + arjun_substantial=True → this was a Ganak instance from step 1; ~18 min elapsed before a 15s solve. Worth considering routing arjun_substantial=True to Ganak sooner.

---

# === MC2026 track1 (NEW set) ===
New competition benchmark, 100 instances at `SharpSAT/MC2026_Public/mc2026_track1_public/` (verified ZERO content overlap with the mc2025 set). Logged as `mc2026_track1_NNN` to avoid colliding with mc2025's `t1_NNN` (same numbers, different instances).

## mc2026_track1_001 — SOLVED by cocoa-plain (round-1 short-circuit); first mc2026 run

**LOW band** (56v/270c, nd_cost log2=16.3, n_leaves=4, arjun_substantial=False) — trivial, an end-to-end smoke test of the mc2026 setup. **cocoa-plain** short-circuited in round 1 (0.2s). **COUNT = 48**, ganak-verified (`ganak --prob 0` → `c s exact arb int 48`). Confirms the new folder → decompress → 8-config pipeline → verify path works.

## mc2026 trivial round-1 short-circuits (cocoa-plain, ganak-verified; ARIMA N/A — no funnel cut / monitoring)
- `003`: count **8** (30v/58c, band low, 0.1s)
- `005`: count **6** (228v, band low, 0.1s)
- `023`: count **449260070** (39v/45 lines, band low, 0.1s)

## mc2026_track1_007 — SOLVED by COCOA (cocoa-nosep-cascade, monitoring finish); first non-trivial mc2026 + ARIMA funnel-cut limitation found

**304v/8080c, band low but worst_leaf=32** (a 2^32 leaf → non-trivial, no short-circuit), arjun_substantial=False. **COUNT = 324611962730585548414761330000**, ganak-verified. First mc2026 instance to exercise the funnel + a **COCOA solve with NO handoff** (leader finished in monitoring at 341s).

**Funnel:** round-1 cut (predict_cb) kept [plain, cache-max, sep-cascade, nosep-cascade] — all at cb≈293.5 (~1%), far ahead of adaptive (cb275, cut) and reactive/unified-sep (cb244 on 18–21M decisions, the worst, cut). Round-2 cut (ARIMA) → the two cascade configs; round-3 (ARIMA) → leader **nosep-cascade**. The **cascade configs led AND were ~4× more decision-efficient** (1.2M vs 4.5–4.8M decisions for the same cb) — the new sep/nosep-cascade earning their slots on a BCP-cascade-friendly leaf. Leader climbed pct 1%→4%→12%→37%→done.

**CRITICAL — ARIMA funnel-cut limitation (reconstructed per-config ETAs at the 4→2 cut):** plain/cache-max had WALLED in round 2 (rate ~0.000 b/s) yet ARIMA gave them **finite optimistic ETAs ~46–48s, not inf** — because (a) `mu` is the round-1-burst-dominated mean, and (b) at active=120s the 90s stuck-floor window still spans the burst, so it can't flag the round-2 stall. All four ETAs clustered 42–48s ≈ rank-by-remaining-bits; the cut kept the cascade configs only because they were *further along* (rem 4.1 vs 6.4), NOT because ARIMA detected the wall, and the ETA was ~5× too optimistic (predicted 42s; actual ~221s to finish). So **the round-2 funnel cut is NOT wall-aware** (too little post-burst data) — it degrades to "closest-to-done." ARIMA in **monitoring** (clean post-burst window) was sound: it tracked the climbing leader (short eta, over=F, no spurious bail) to the finish.

**RESOLUTION (follow-up commit):** investigated by simulation + a backtest on real traces. The "weight the recent rate" idea was **REJECTED** — t1_045 (recovers from 3-min walls) shows a short trailing window false-bails a config mid-recovery (it's too twitchy). The actual fix: **removed the myopic 90s stuck-floor** from `predict_cb_arima`, so the ETA is now pure full-data ARIMA — it grows *smoothly* as a wall lengthens (`mu` dilutes with each flat sample, no 143s→inf cliff) and bails only via the scheduler's consecutive-over-budget streak. Backtest: t1_017 bails @870s, t1_019 @1080s (later than the old floor's 321/431s, but Ganak still gets >2/3 of the budget), and t1_045 is **no longer false-bailed**. The round-2 cut's rank-by-closest-to-done is left as-is (acceptable; the wall-aware bail correctly lives in monitoring).

## mc2026_track1_009 — SOLVED in round 1 by cocoa-adaptive (config-diversity short-circuit); ARIMA N/A

**8307v, band mid** (log2_cost=78.6, worst_leaf=1, 8143 leaves), arjun **substantial=True**. **COUNT = 110680464442257309696** (~2⁶⁶·⁶), ganak-verified. Solved in **round 1** — no funnel cut, no monitoring, **ARIMA not exercised**.

**Config-diversity win:** `cocoa-reactive` scouted first (full 60s window) and churned **5.2M decisions** to cb 319.6 / pct 37.5% **without finishing** (grinding a ~2³²¹ search tree). `cocoa-adaptive` then started and **short-circuited in 4.5s** — its decomposition/branching collapsed the count where reactive stalled. Concrete evidence that the 8-config covering set earns its slots: the winner here (adaptive) is a *different* config from 007's winner (nosep-cascade). Note: `closed_bits`/`n_root` measure SEARCH size, not log2(count) — reactive's n_root≈321 is the work it faced, unrelated to the 2⁶⁶·⁶ model count adaptive found cheaply.

## Forecaster redesign — ARIMA → recency-weighted progress rate (power-law tail); sweep table (LIVING, update over time)

**Why ARIMA was dropped.** Investigating mc2026_011's monitoring (a band=high deep-tail grinder) exposed that the ETA model assumed a **constant** progress rate, but `closed_bits` **decelerates** hard: cb = n_root + log2(pct_lin/100), so a constant cb-rate assumes pct_lin keeps *doubling* at a fixed cadence — and on 011 the doubling cadence lengthened ~100× (10s → 1000s+/doubling). ARIMA(1,1,0)+drift (drift = arithmetic mean of all rates) therefore stayed optimistic and predicted ~500s for a config that wasn't finishing (pct_lin was ~8×10⁻¹⁰% at that point). A linear *rate-slope* fix was tested and **rejected** — it over-bails: nearly every hard instance decelerates (t1_045 too), so a negative slope doesn't distinguish recovering from terminal. The real discriminator is **remaining_bits / rate**: t1_045 keeps because it's *close* (~1 bit left), 011 bails because it's *far* (~33 bits).

**The model now:**
```
eta  = remaining_bits / rate
rate = Σ_i w_i·r_i / Σ_i w_i ,  r_i = per-interval cb rate (b/s),  w_i = (1 + age_i/TAU)^(−POWER)
```
Power-law recency tail: **sharp near age 0** (a fresh wall dominates → prompt bail) + **heavy tail** (a strong early burst is remembered → a config creeping to a finish is never endangered). Time-indexed in seconds (sampling-invariant). eta=inf only if rate ≤ 0 (truly flat from the start). Bail is the scheduler's 10-consecutive-over-budget streak (unchanged).

**Sweep** (replay the leader trajectory of each instance through the model + the streak; BAIL walls extended to budget). Two objectives: **(1) t1_045 never near a handoff** (low peak-danger), **(2) bailers flagged fast**.

| weight setting | 007 KEEP | t1_045 KEEP | 011 BAIL | t1_017 BAIL | t1_019 BAIL | t1_031 BAIL |
|---|---|---|---|---|---|---|
| exp τ=120 | 0.01 | 0.66 | @501 | @400 | @480 | @1091 |
| exp τ=175 | 0.01 | 0.39 | @611 | @450 | @540 | @1111 |
| exp τ=240 | 0.01 | 0.16 | @651 | @500 | @600 | @1151 |
| exp τ=300 | 0.01 | 0.08 | @682 | @540 | @640 | @1201 |
| pow p=1, τ=120 | 0.01 | 0.03 | @932 | @570 | @690 | @1561 |
| **pow p=2, τ=120 (CHOSEN)** | **0.01** | **0.04** | **@621** | **@420** | **@510** | **@1151** |

KEEP cols = peak danger ratio (eta / 1.25·budget-left; <1 safe, lower=better). BAIL cols = handoff active-time in s (smaller=better). pow-p2-τ120 is the only setting that is both very safe on t1_045 (0.04, like the slow exponentials) AND bails fast (≈ exp τ=120). Implemented as `predict_cb_recency` with `RWR_POWER=2`, `RWR_TAU=120` — **PROVISIONAL** knobs, not derived constants.

**Caveat / TODO:** the KEEP set is thin — only t1_045 is a true recoverer (007 finished easily). exp τ=240 (0.16, bails ~80s slower) is the closest runner-up. **Re-run this sweep and update the table as more keep/bail traces accumulate; re-fit POWER/TAU then.** Ground truth used here: KEEP = {007 solved-in-monitor, t1_045 recovers→97%}; BAIL = {011, t1_017, t1_019, t1_031 — all terminal walls Ganak should take}.

## mc2026_track1_013 — SOLVED in round 1 by cocoa-reactive (short-circuit); forecaster N/A

**3025v, band mid** (log2_cost=47.3, worst_leaf=1, 2824 leaves), arjun substantial=True. Solved in **round 1** by `cocoa-reactive` in **3.8s** — short-circuit, no funnel cut / monitoring, recency forecaster not exercised. First run on the new `predict_cb_recency` build (forecaster unused, but pipeline import/trace path exercised cleanly). **723-digit count, ganak-verified** (full-string match):

`296476034789978134120813694105889196637143099477633324816082314536891061330578960717457630202115461307371403034595170935724489293025075877350504677578646496935046858594479715738345467364856518143562563193206102470734007555612914096207105269178488890947743301293297650838442152727760046517446830295902003198032258606113584103529903220339673080606794891142197240305184142245671365903090895689843737237511812085698333701706561779824300625327604726460130427583460035274205809575857633345902990376717064770455839258773171510507099132110028077728160266796559445408422233980009645488034469133404238749146357925975400309760081625787339024177663736163185805751165416483569137375444258463220131699965267987276097342929294919389413376`

mc2026 round-1 short-circuits so far: 009 (adaptive), 013 (reactive) — different winners on easy-structured (worst_leaf=1) instances, more evidence the covering set earns its slots.

## mc2026_track1_015 — TRIVIAL: cocoa-plain in 0.1s (band low, arjun not-substantial)

**2784v, band low** (log2_cost=33.6, worst_leaf=1, arjun **substantial=False**). `cocoa-plain` solved it in **0.1s** in round 1 — trivial search, no funnel/monitoring (like 001/003/005, but a large count). **665-digit count, ganak-verified** (full-string match):

`47231357286853767522716755927596215561986116650661432804728678927314249532098526380077034785580457260247952241007887375273000474840328294138062827690568625295902758435629765020876675007650887240298461920809005107679471980423130606355155258698193159389560724524486703687418766300787462745455454840419015572190253146649400636972799438629962334231348365404499947909495159747380366091782990708788254353238300987924815581261908398640232974138357658032571267179157147109522541069514133699125447168383688965455196623597661591747482353177871522320071696622482801372011199249874325447348286802406804399292729136049684688723519643837248433295481915701800013423925974328672256`

## mc2026_track1_017 — TIMEOUT (UNSOLVED by both engines); first recency-forecaster live handoff + warm-filter/cb=0 finding

**7180v/20792 lines, band HIGH** (log2_cost=**184.2**, deepest mc2026 yet; max_path=181; worst_leaf=2; arjun **substantial=False**). **Result: `status=timeout`, count=None @ 3600s — neither COCOA nor Ganak solved it.** (Pipeline exits 1 on timeout; that's the "no solution" signal, not a crash.)

**COCOA side:** the two **cascade** configs cliff-jumped to **cb 6496 / pct 25%** in the FIRST 10s interval (cb 0→6496, only 42k decisions), then **walled dead** — cb pinned at 6496 while decisions kept climbing ~25k/10s (grinding the hard ~2-bit core, not idle). The other 6 configs sat at cb≈5227 with **pct=0** (underflow ⇒ n_root=∞ ⇒ search ≥2⁶³⁰⁰). The **new `predict_cb_recency` made its first live handoff:** leader nosep-cascade read as flat (see below) ⇒ rate 0 ⇒ eta=∞ ⇒ over=True ⇒ 10-streak ⇒ handoff to Ganak at ~active 280s.

**Ganak side:** ran ~2640s and made **literally zero progress** — `cache_K=0, cubes=0, conflicts=0, confl/s=0, cache_miss_rate=1.000`. The fallback contributed nothing; 017 is pathological for both engines.

**FORECASTER FINDING (the run's real payoff).** The leader's ETA was **∞ from the first monitoring recheck**, never evolving — because the rate is a *difference series* and the **warm filter (`if cb>0`) drops the `cb=0` sample**, deleting the only non-flat interval (the 0→6496 jump). So the forecaster saw an all-flat trajectory ⇒ rate=0 ⇒ ∞. Why the filter exists (verified): if `cb=0` is KEPT, the single 0→6496 jump (649 b/s) is remembered by the power-law tail and pins eta at **0.1–34s for the ENTIRE 3600s budget** ⇒ the config would *never* bail (ride to timeout). So the filter is **load-bearing** (guards the "one-jump-hostage" case), but **blunt**: it can't distinguish *cliff-then-dead* (bail, correct) from *cliff-then-grinding-the-hard-core* (017: decisions growing → maybe keep). The missing signal is **decisions** (growing=working, flat=idle). **Open fix:** bail on cb-flat only when decisions are *also* flat; bound patience while still grinding. Also (Q1 from the analysis): seed the handoff streak from the leader's scouting trajectory so an already-walled leader bails at the 180s floor instead of 180+90s. NOTE: for 017 the keep-vs-bail debate was **moot** — Ganak also walled — but handing 25%-COCOA-progress to a 0-progress Ganak was a pure loss.

## mc2026_track1_019 — SOLVED in round 1 by cocoa-plain (1.0s) despite band=high

**3766v, band HIGH** (log2_cost=126.2 — 2nd-deepest mc2026 after 017's 184; worst_leaf=2, arjun substantial=True). Yet `cocoa-plain` solved it in **1.0s** in round 1. **COUNT = 948731481813003893206929197567363893059348940398470365088269067920407148829343744000** (84 digits), ganak-verified. Reinforces that band/log2_cost does NOT predict difficulty: 017 (band=high, log2_cost 184) → portfolio TIMEOUT; 019 (band=high, log2_cost 126) → 1s plain solve. Tree handoff not exercised (round-1 short-circuit). First run on the live `predict_cb_tree` build (handoff path present but unused).

## mc2026_track1_021 — SOLVED by cocoa-plain in monitoring (263s); FIRST live exercise of the escape-history tree

**87v / 93-line CNF, band low** (log2_cost=32.2, worst_leaf=2, arjun substantial=False) — a **tiny CNF but a genuine grinder** (deep search, n_root≈87; ~31M decisions in the first 60s). The starkest "size ≠ difficulty" case yet: 93 lines took COCOA **263s active**, while 019 (10406 lines, band high) fell in 1s. **COUNT = 37854886389693351111** (~2⁶⁵), ganak-verified.

**Funnel:** round-1 cut → [plain, cache-max, adaptive, sep-cascade]; round-2 (recency) → [plain, adaptive]; round-3 → leader **plain**. plain climbed **pct 21% → 34% → 54%** across r1/r2/r3, then to 100% in monitoring.

**Tree forecaster — first LIVE deployment, correct:** 8 monitoring forecasts, **all `keep`** with reason "progressing recent_rate>eps", riding plain from rem 0.71 → 0.03 bits to the finish. The `recent_rate>eps` first branch fired throughout (a climbing leader); the escape logic never engaged (no wall to detect) — exactly the easy-correct case. No spurious handoff. (The wall-detection branches remain validated only on the offline harness; a live grinder that *walls* is still wanted to exercise them in production.)

## mc2026 early-index backfill (003 / 005 / 011 / 023) — recorded retroactively

Run in an earlier session but never given individual entries; logged here for completeness from their `runlogs/mc2026_t1_0NN.log` pipeline-result lines (all band=low trivial round-1 cocoa-plain solves):
- **003** — SOLVED, count = **8** (cocoa-plain r1, 0.1s; arjun=False); ganak `--prob 0` = 8 (0.00s) — match ✓.
- **005** — SOLVED, count = **6** (cocoa-plain r1, 0.1s; arjun=False); ganak `--prob 0` = 6 (0.01s) — match ✓.
- **023** — SOLVED, count = **449,260,070** (cocoa-plain r1, 0.1s; arjun=False); ganak `--prob 0` = 449,260,070 (0.29s) — match ✓.
- **011** — KILLED mid-run, no pipeline result (UNRESOLVED, like 083). NB: the `mc2025_track1_011` rows elsewhere in this log are a different, prior instance set — not this one.

## mc2026_track1_025 — TIMEOUT at 91% (cocoa-plain, 0.14 bits from done); tree correctly kept a slow progressor

**102v / 105-line CNF, band low** (log2_cost=27.6, arjun substantial=False) — another small-but-hard grinder (n_root≈102). **TIMEOUT @ 3600s, count=None**, but cocoa-plain reached **pct 90.96% / cb 101.86 / rem 0.14 bits** on **1.27 BILLION decisions** (338M L2 hits) — ~0.14 bits (≈ the last top-level component) from a finish at the 1h limit.

**Tree's 2nd live deployment — patience validated.** 274 monitoring forecasts, **all `keep`** (recent_rate>eps the whole time; plain crept 4.5%→91% over ~50 min of monitoring, decelerating to ~0.0005 b/s but never stalling). Never bailed — which was CORRECT: plain was the genuine best option, progressing to 91%; bailing to a cold Ganak would have thrown that away (and Ganak made ZERO progress on the structurally-similar 017).

**Revises the mid-run "budget-aware bail" idea:** I'd flagged that the tree keeps a progressing-but-too-slow leader to timeout rather than trying Ganak — but here that was the RIGHT call. A budget-aware bail would have abandoned a 91%-done leader for a Ganak that almost certainly also fails. So 025 is a **"needs-more-budget" instance** (0.14 bits short at 3600s), NOT a forecaster failure — with a longer budget, plain finishes. (Caveat retained: a budget-aware bail could still help when the leader is *far* from done, not 91%.)

Tree status after 021+025: both live runs were KEEP-a-progressor cases (021 → finish, 025 → near-finish timeout). The **bail/escape branches are still only harness-validated** — a live leader that genuinely WALLS is still wanted to exercise them in production.

## mc2026_track1_027 — SOLVED, count = 4 (Ganak-solo `--prob 0`, 254s — backbone mirage); COCOA runaway was KILLED (~70 min wall = WALL-BUDGET OVERRUN bug). See RESOLUTION below.

**760v / 43783-line CNF, band HIGH, log2_cost=624.3 (deepest mc2026 by far; 017 was 184), worst_leaf=38, n_root≈760, arjun not-substantial.** Funnel: the standard configs front-loaded to cb≈727 then crawled; the **cascade configs won the cut** (sep/nosep climbed fast in bursts). Leader = `cocoa-sep-cascade`. In monitoring it burst-and-plateaued (cb 716→719→…→737, the 716 plateau was transient — it resumed) then crept to cb 737 / rem ~23 / **pct≈10⁻⁵%**. **Tree kept it throughout (114 forecasts, all `keep`)** — correct (genuinely progressing, if glacially); the bail branch never fired (it kept resuming). Manually **KILLED** at the user's instruction.

**BUG — wall-budget overrun (fix item).** The pipeline ran **~70 min of wall** (`pipeline` etime 01:09:13) on a `--budget 3600` (60 min) run **without stopping**. The leader's active time was only **1321s of 3600s**, and `sharpSAT` had **22:58 CPU over 66 min elapsed (~35% CPU)** — it was **memory/cache-bound** on this deepest instance. So the budget is effectively spent against the leader's *active/CPU* progress (the deadline check is gated by the leader's progress emission, which a CPU-starved leader emits slowly), letting a cache-thrashing deep instance run **~3× its wall budget** (projected ~3 h to the active-budget). **FIX: enforce a hard wall-clock cap independent of the leader's emission rate** (e.g. a watchdog), so wall ≈ `--budget` regardless of CPU starvation.

Outcome (initial): killed runaway (would have been a runaway/timeout); exposed the wall-budget overrun.

**RESOLUTION — SOLVED, count = 4 (Ganak-solo, `--prob 0`, 254s).** After killing the COCOA runaway, ran Ganak alone for 1 h on 027 (`--prob 0 --maxcache 26000 --verb 1`). It finished in **254s** via **backbone simplification** — cadiback found **756 of 760 variables are backbone** (forced units), collapsing the formula to a trivial residual → **count 4**. This RECONTEXTUALIZES everything above: **027 is NOT a deep instance — its count is 4 (2 bits).** COCOA's `n_root≈760` / `pct_lin≈10⁻⁶%` / "27 bits remaining" was a **MIRAGE** — its decomposition engine (no backbone detection) was grinding a mostly-FORCED search space (separator 529/760 → no decomposition), mistaking forced-variable churn for genuine depth. There was no hard tail. **The eta-gate handoff would SOLVE 027**: hand off at ~24 min → Ganak 254s → solved ~28 min (the old forecaster never handed off → runaway). LESSON: COCOA's closed_bits/n_root can be a mirage on backbone-heavy / poorly-decomposable instances; the Ganak handoff is exactly the right escape and the eta-gate now triggers it. (Count 4 is Ganak's exact `--prob 0` result — the oracle; COCOA never finished, so no cross-check.)

## mc2026_track1_029 — UNSOLVED (both engines); first LIVE eta-gate handoff

**2301v / 267196-cl, band HIGH, log2_cost=1877 (deepest by COCOA's metric), sep_ratio=0.77, arjun not-substantial.** CNF structure (analyzed directly): **binary-heavy** — 267137/267196 clauses are 2-lit and ALL `¬a∨¬b` (conflict / not-both); plus **59 all-positive length-39 clauses that PARTITION the 2301 vars into 59 groups of 39** ("≥1 true per group"). So it's a **selection-with-conflicts / coloring-family** count. The conflict graph is dense + regular (mean degree 233, max 409) → no small separator, and **ganak skipped tree-decomposition** ("too many edges").

Run: COCOA funnel → leader `cocoa-adaptive-nosep` → **eta-gate handoff at ~16 min wall** — the **FIRST live production firing of the eta-gate** (`progressing but doomed: eta=5754s>2×2719s, rem=601`). The in-pipeline ganak then showed a 3.5 h `etime` that turned out to be **system sleep**, NOT a watchdog failure (`time.monotonic()` ignores sleep, so the budget/watchdog correctly never fired — actual compute ~25 min). Re-ran **ganak SOLO**: Arjun found **0 backbone units, 2238/2301 independent vars** → no simplification; ganak ground **~65 min, NO count** → killed.

Outcome: **UNSOLVED in budget by both engines** — genuinely hard (NOT a 027-style mirage; confirmed by 0 backbone + near-full free support + both engines grinding the real count). Note: clause branching (`-cb`) is useless on this family — the binaries are length-2 (< `clause_branch_min_length` 3), and negating the *positive* group clauses only sets vars FALSE (a false satisfies `¬a∨¬b` with zero propagation); the negate-arm cascade would need *negative*-polarity wide clauses.

## mc2026_track1_031 — SOLVED, count = 215 (cocoa-plain r1, 30.8s; ganak-verified)

**595v / 29707-cl, band HIGH, log2_cost=490, sep_ratio=0.625, arjun not-substantial.** SAME family as 029 — 35 all-positive length-17 clauses partitioning the 595 vars into **35 groups of 17**, plus 29672 `¬a∨¬b` binary conflicts — but **~4× smaller** (mean degree 101 vs 029's 233). **SOLVED by cocoa-plain in round 1, 30.8s active, count = 215**; `ganak --prob 0` independently returns **215** (match ✓; ganak took 156s).

Insight: here **COCOA (30.8s) BEAT ganak (156s)** — the count is small (215), so component-caching closes the search fast, while ganak pays for Arjun preprocessing on a binary-heavy formula. So this family's tractability is governed by the **count magnitude**, not the structure type: small count → easy for COCOA; huge count (029) → grinds both engines. (027 count=4 was COCOA-easy-in-principle but a backbone mirage that fooled COCOA's metric; 031 count=215 is a genuine small count COCOA closes directly.)

## mc2026_track1_033 — UNSOLVED (timeout, both engines); validates the corrected funnel + exposes handoff economics

**312v / 3701-cl, band HIGH, log2_cost=275, sep_ratio=0.548, arjun not-substantial.** Same family as 029/031 (35 all-positive length-17 group clauses partition the 312 vars; ¬a∨¬b binary conflicts) but the ND tree shatters into 245 trivial leaves — looked decomposable.

**Validated the corrected STAGE-AWARE funnel (`8a8fa7b`) end-to-end** — the same instance that exposed the round-1 bug. First run (pre-fix) the 6-sample ETA cut the cascade leaders at 8→4 and ground a wrong leader (`reactive`). Re-run with the fix: **8→4 by closed_bits LEVEL kept both cascades** (cb 299.2, vs the adaptives at 294), **4→2 hedge kept BOTH cascades** (nosep=max-cb, sep=min-eta — proximity gave them the lower ETA too: rem 3.5 beats the adaptives' rem 8.5 despite 2× lower rate), **2→1 banded near-tie** (Δcb 0.04 ≪ 1.5) → `nosep-cascade`. The configs the old ETA killed sailed through all three cuts.

Monitoring: `nosep-cascade` crept the tail — over ~15 min closed only ~0.73 bits (cb 300.1→300.85, pct 13%→22.6%, rate ~0.0008 b/s, never accelerated). The **eta-gate "progressing but doomed" bailed (2nd live production firing)** at ~31 min wall → handoff to Ganak with **only 1754s (~29 min)** left. **Ganak ran its full ~29 min, NO count → TIMEOUT.** 033 UNSOLVED by both engines (029-style), despite being tiny (312v) — so this family's count is genuinely large/hard at this size.

**LESSON — handoff economics (open fix):** the late handoff gave Ganak 29 min and was futile (timeout); meanwhile a still-*creeping* (not walled) COCOA was abandoned. Proposed **`GANAK_HANDOFF_FLOOR` (~30 min)**: only hand off while ≥ that remains for Ganak; past it, keep COCOA (heavy sunk investment + still progressing + a time-starved Ganak won't finish anyway). On 033 this would have *suppressed* the handoff (fired at t_rem=29.2 min < 30) → kept `nosep-cascade` creeping (no worse, 033 unsolved either way). Recommended variant (b): floor the "progressing-but-doomed" eta-gate bail, but still allow a genuine-wall (rate≈0) handoff so a fast Ganak can rescue a truly-stalled-late instance.

## mc2026_track1_035 — SOLVED, count = 16,588,420 (cocoa-reactive r1, 13.4s; ganak-verified)

**105v / 989-cl, band=MID (first mid-band in this stretch), log2_cost=82.86, sep_ratio=0.514, n_leaves=127 / worst_leaf=0 (decomposes cleanly), arjun not-substantial.** SOLVED by **cocoa-reactive** in round 1, **13.4s** active, count = **16,588,420**; `ganak --prob 0` independently returns 16,588,420 (5.95s) — match ✓. Unlike the band=high deep-grinder family (029/031/033, both engines lose), 035's mid band + small size + clean ND decomposition made it a fast solve for both — here ganak (5.95s) even edged COCOA (13.4s). Reinforces: band/decomposability, not size, drives tractability.

### wlIter-2 side-experiment on 033 (negative result)
Ran `nosep-cascade -wlIter 2` solo on 033 for comparison vs the wlIter-1 solo (both 1 h, per-minute progress). wlIter-2 **tracked wlIter-1 within ~0.03 bits the whole way** (bouncing, never separating), creeping the same sticky tail; killed at ~21 min. So the "more isomorphic-component collapsing helps the binary-heavy tail" hypothesis does **not** hold on 033 — wlIter-2 is net-equivalent (marginally slower per decision). 033 stays a both-engines-lose instance regardless of wlIter.

## mc2026_track1_037 — SOLVED, count = 1,234,116 (cocoa-plain r1, 0.3s; ganak-verified)

**60v / 366-cl (smallest in the sweep so far), band=LOW, log2_cost=38.5, sep_ratio=0.4, sepsize=24, n_leaves=57 / worst_leaf=0, arjun not-substantial.** SOLVED by **cocoa-plain** in round 1, **0.3s** active, count = **1,234,116**; `ganak --prob 0` = 1,234,116 (0.46s) — match ✓. Trivial for both engines (band=low + tiny + clean ND decomposition). Continues the clean split: band low/mid + decomposable ⇒ instant for both; band high + large separator ⇒ the deep-grinder family (029/033) both lose.

## mc2026_track1_039 — SOLVED, count = 279,857,462,060 (cocoa-nosep-cascade r1, 7.8s; ganak-verified)

**1413v / 29,487-cl (largest in the sweep so far), band=HIGH, log2_cost=289.6, sep_ratio=0.063, sepsize=89, worst_leaf=16, n_leaves=372, arjun substantial=True.** SOLVED by **cocoa-nosep-cascade** in round 1, **7.8s** active, count = **279,857,462,060** (~2.8e11); `ganak --prob 0` = 279,857,462,060 (6.41s) — match ✓.

**KEY — mirage caught by the diverse race:** the SEPARATOR configs were mirage-grinding — `cocoa-plain` / `cocoa-cache-max` ground to ~1040 closed_bits / pct_lin ~1e-20 within 60s (large separator → no decomposition → deep churn), looking like doomed deep-grinders. Meanwhile the NO-separator **`cocoa-nosep-cascade`** (`-unifiedPicker -decomposeAfterK 1000 -cascadeW 10 -cascadeDepth 9`) cracked the whole thing in 7.8s. The round-1 **diverse 8-config race is exactly what caught this**: the apparent leaders (by closed_bits) were mirages; the structurally-different config was the real winner — a concrete case where ranking round-1 survivors by closed_bits LEVEL alone would have been misled, but running all 8 to completion-or-cut saved it.

**CONFIRMS the arjun-substantial signal:** band=high + **arjun=True ⇒ tractable** (039 here; cf 017/019/031), vs band=high + **arjun=False ⇒ both-engines-lose** (029/033). arjun=True also made ganak fast here (Arjun preprocessing did the heavy lifting, 6.41s). Strengthens the open "route arjun_substantial=True to Ganak sooner" thread — though note COCOA's nosep-cascade matched it without help.

## mc2026_track1_041 — SOLVED, count = 564,153,552,511,417,968,750 (cocoa-reactive r1, 0.1s; ganak-verified)

**939v / 3785-cl, band=MID, log2_cost=82.4, sep_ratio=0.0149, sepsize=15, worst_leaf=9, n_leaves=307, arjun substantial=True.** SOLVED by **cocoa-reactive** in round 1, **0.1s**, count = **564,153,552,511,417,968,750** (~5.6e20, 21 digits); `ganak --prob 0` = same (0.27s) — match ✓. A HUGE count yet structurally trivial — clean reminder that count **magnitude ≠ difficulty**: tractability is set by structure (band=mid + small separator/clean decomposition + arjun=True ⇒ instant for both engines), not by how large the answer is.

## mc2026_track1_043 — SOLVED, count = 738,969,640,920 (cocoa-reactive r1, 0.1s; ganak-verified)

**550v / 2001-cl, band=MID, log2_cost=72.3, sep_ratio=0.0418, sepsize=23, worst_leaf=4, n_leaves=179, arjun substantial=True.** SOLVED by **cocoa-reactive** in round 1, **0.1s**, count = **738,969,640,920** (~7.4e11); `ganak --prob 0` = same (0.09s) — match ✓. Same trivial profile as 041 (band=mid + small separator + arjun=True + clean decomposition ⇒ instant for both). Run of easy ones continues (037/041/043 all sub-second).

## mc2026_track1_045 — SOLVED, count = 32,334,741,710 (cocoa-nosep-cascade r1, 6.0s; ganak-verified)

**1337v / 24,777-cl, band=HIGH, log2_cost=287.7, sep_ratio=0.066, sepsize=88, worst_leaf=12, n_leaves=328, arjun substantial=True** — essentially the **twin of 039** (1413v/29487cl, same band/arjun profile). SOLVED by **cocoa-nosep-cascade** in round 1, **6.0s** active, count = **32,334,741,710** (~3.2e10); `ganak --prob 0` = same (10.36s) — match ✓, and here **COCOA (6.0s) beat ganak (10.36s)**. Same mechanism as 039: the separator configs (cocoa-plain/cache-max) mirage-ground to ~900 closed_bits / pct ~1e-34 while nosep-cascade cracked it in 6s. **Two-for-two on the band=high + arjun=True ⇒ nosep-cascade-wins-fast profile** (039, 045) — distinct from band=high + arjun=False ⇒ both-lose (029, 033). The arjun signal is looking like a reliable tractability discriminator within band=high.

## mc2026_track1_047 — TIMEOUT (both engines lose); eta-gate handoff validated; pct-gate fix

**90v / 234-cl (tiny), band=MID, log2_cost=78.7, but sep_ratio=0.6 / sepsize=54-of-90 / balance=0.083 (a HUGE, lopsided separator), arjun substantial=False.** Both engines failed. **arjun=False ⇒ both-lose now holds even at band=mid** — the big separator made a 90-var instance undecomposable. Confirms **arjun (not band) is the tractability discriminator**: 029/033/047 all arjun=False ⇒ both lose; 039/045 band=high+arjun=True ⇒ solved fast.

**Run:** round 1 — all 8 configs converged to the *same* plateau in their 60s windows (separator configs ~76.5 cb, cascade configs ~68; nosep-cascade did NOT crack it, unlike 039/045). Full funnel fired cleanly: **8→4 by closed_bits LEVEL** → {plain, unified-sep, cache-max, reactive}; **4→2 hedge** {top-cb, min-eta} → {unified-sep, plain}; **2→1 banded** (near-tie |Δcb|≈0.006<1.5 → min-eta) → **cocoa-plain**. Monitoring: a decaying staircase that, once on a *continuous* run (no 60s window resets), escaped repeatedly — cb 79.8→82→…→84.64 over ~22 min, rate bouncing 0.05–0.93 b/min, pct_lin 0.09%→**2.44%** — then a 4-min monotonic decline to near-stall (0.16→0.11→0.085→0.036 b/min) at cb 84.64 (~5.4 bits short of n_root=90). Handoff fired at ~+37m wall: **10 consecutive 'bail' verdicts → killed COCOA (verified: 0 sharpSAT procs left, RAM freed), ganak-native to completion (1380s)** → ganak ran its full ~22-min window with **no count → TIMEOUT**. Box clean afterward (no orphans).

**eta-gate validation (cleanest yet):** the escape-history tree *kept* cocoa-plain through ~22 min of genuine, bouncy escapes (never bailed on a plateau it later climbed out of), then bailed only at the terminal wall. Recoverer-patience and wall-bail both exercised correctly on one live instance.

### FIX — log-space **pct-ETA** gate (replaces the linear bits-ETA)
047 exposed the deceleration trap in the "progressing-but-doomed" branch. The old gate used `eta = remaining_bits / recent_rate` (linear in closed_bits). But **closed_bits = log₂(count)**: each further bit DOUBLES the linear work, so the cb-rate must decay as cb→n_root, and fitting a line to that decelerating curve predicts arrival too early. At 047's +28m the linear eta said ~22 min → *keep*, while only **2.4%** of the count was actually closed. So the old gate only emitted 'bail' once the rate finally collapsed near the wall (+33–37m); the handoff came ~17 min late.
- **New rule:** bail iff `remaining > log₂(ln2 · recent_rate · margin · t_remaining)` — the honest pct-space ETA `(2^remaining − 1)/(ln2·rate)` tested in log space. Same inputs (remaining bits, cb-rate), threshold changed from **linear** to **logarithmic**.
- **Numerics:** never materializes `pct_lin = 2^(cb−n_root)`, which **underflows to 0.0** on large instances (n_root in the thousands → exponent below ~2^−1074). The exponential lives only inside one `log2`; all operands well-conditioned. 2^remaining shown in the bail reason is overflow-guarded (display only).
- **Edges:** near-finishers (remaining ≤ REM_FLOOR=2) still always ridden out, and there `2^x−1 → x·ln2` so the two ETAs agree on the last bit; n_root inf/None → frozen-path fallback.
- On 047 this would have bailed ~+18–20m → ganak ~40 min instead of ~23 (whether that solves 047 is unknown — ganak showed zero progress — but earlier-doomed-bail is strictly better for the fallback). `forecast.py` `predict_cb_tree` eta-gate; regression `tests/test_tree.py` #11 seeded with 047's logged cb trajectory (old gate keeps it, new gate bails). Suite 86 passed / 9 skipped. closed_bits remains the LEVEL signal for the funnel cut (unchanged) — the bug was only time-extrapolation-to-completion.

## mc2026_track1_049 — SOLVED, count = 12,930,673,567,682 (cocoa-adaptive monitor, 414s; ganak-verified 433s)

**80v / 240-cl (tiny), band=MID, log2_cost=71.6, sep_ratio=0.575 / sepsize=47-of-80 / balance=0.059 (big lopsided separator), arjun substantial=False — the SAME profile as 047.** SOLVED by **cocoa-adaptive** in the monitor phase, **414s** active, count = **12,930,673,567,682** (~1.29e13); `ganak --prob 0` = same (**433s**, ~7.2 min) — match ✓. Both engines solved it in ~7 min (COCOA edged ganak).

- **arjun=False is NOT strictly both-lose.** 047 and 049 share the profile (tiny, band=mid, sep_ratio ~0.6, arjun=False) yet 047 timed out *both* engines while 049 solved on *both* in ~7 min. So arjun=False marks "undecomposable → COCOA must grind → *may or may not* finish" — hardness is set by the actual count, not the structure flag. Softens the earlier arjun=False ⇒ both-lose claim (029/033/047 happened to be hard; 049 is not).
- **The ADAPTIVE PICKER, not wlIter 2, was decisive.** Both `-adaptive` configs (cocoa-adaptive, cocoa-adaptive-nosep) broke ahead to ~75 in round 1 and cracked the tail; the *other* two wlIter-2 configs (cocoa-reactive `-reactiveMetis`, cocoa-cache-max `-cs 21000`) only tracked the **wlIter-1** cocoa-plain at ~73. So among the four wlIter-2 configs only the two with `-adaptive` won. Consistent with 033 (wlIter 2 alone = no benefit). Evidence for the adaptive-probe picker on dense-residual / arjun=False structure, NOT for wlIter 2.
- **pct-gate near-finisher floor validated live (first exercise of the new gate).** The leader sat at ~1.0–1.3 bits (pct 40→73%, slow) for minutes; the `rem ≤ REM_FLOOR(2)` rule rode it out (a floorless pct-eta would have bailed it ~45 min), and it finished. No misfire.
- **Verification:** ganak needed 433s (>7 min). A short cap (I initially tried 300s) would have wrongly declared it unverified — full-hour verification budget is the right default ([[verify-full-budget]]).

## mc2026_track1_051 — SOLVED, count = 3,393,485,555,651,045 (cocoa-plain monitor, 277.9s; ganak-verified 306.7s)

**90v / 234-cl (047's exact size), band=MID, log2_cost=65.1, sep_ratio=0.511 / sepsize=46-of-90 / balance=0.25, arjun substantial=False — the 047/049 family.** SOLVED by **cocoa-plain** in the monitor phase, **277.9s** active, count = **3,393,485,555,651,045** (~3.39e15); `ganak --prob 0` = same (**306.7s**, ~5.1 min) — match ✓. Both engines ~5 min (COCOA edged ganak again; fastest of the arjun=False solves).

- **Winning config varies within arjun=False:** 049 → cocoa-adaptive (adaptive picker led to ~75), 051 → cocoa-plain (the *separator* configs led to ~86.83 and the adaptive configs sat a hair behind at ~86.22, cut at 8→4). So no single config dominates the family — concrete payoff of the diverse 8-config race. arjun=False tally now: **047 both-lose, 049 + 051 both-solve (~5–7 min)** — a *mixed* family, not a both-lose class; hardness is set by the actual count.
- **pct-gate near-finisher floor confirmed again (2nd live):** the leader rode the endgame from ~1.1 bits / pct 46% to the finish, under the rem≤2 floor; no misfire.

## mc2026_track1_053 — SOLVED, count = 6,827,710,664,219 (cocoa-adaptive-nosep monitor, 187.5s; ganak-verified 199s)

**80v / 224-cl, band=MID, log2_cost=69.1, sep_ratio=0.5875 / sepsize=47-of-80 / balance=0.091, arjun substantial=False — the 047/049/051 family.** SOLVED by **cocoa-adaptive-nosep** in the monitor phase, **187.5s** active, count = **6,827,710,664,219** (~6.83e12); `ganak --prob 0` = same (**199s**, ~3.3 min) — match ✓. Both engines ~3 min (fastest family solve; COCOA edged ganak). Adaptive config won (like 049; 051 went to plain). **arjun=False family tally: 047 both-lose; 049/051/053 all solve in 3–7 min** — confirms a *mixed* family where the winning config rotates (adaptive ↔ separator), vindicating the diverse 8-config race. (Round-1 here was tightly clustered ~75–76.6 with even the cascades keeping pace, unlike the wide spread on 047/049/051.)

## mc2026_track1_055 — SOLVED, count = 18,014,398,509,481,984 = 2⁵⁴ (cocoa-plain r1, 0.1s; ganak-verified)

**80v / 416-cl, band=LOW, log2_cost=16.4, sep_ratio=0.025 / sepsize=2 (TINY separator) / balance=0.5, arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **18,014,398,509,481,984 = exactly 2⁵⁴**; `ganak --prob 0` = same (0.01s) — match ✓.

**Sharpens the discriminator: it's the SEPARATOR (decomposability), not arjun, that sets COCOA tractability.** 055 is arjun=False and tiny like 047–053, but has a *2-variable* separator → trivially decomposable → instant for COCOA. So the cleaner picture: **large separator ⇒ COCOA must grind** (047–053; finishes only if the count is small enough), **tiny separator ⇒ COCOA instant regardless of arjun** (055). arjun=True is the orthogonal *Ganak* tractability signal (preprocessing cracks it, e.g. 039/045). The count being a clean 2⁵⁴ fits a near-fully-independent-component structure (hence the tiny separator).

## mc2026_track1_057 — SOLVED, count = 18,889,465,931,478,580,854,784 = 2⁷⁴ (cocoa-plain r1, 0.1s; ganak-verified)

**110v / 576-cl, band=LOW, log2_cost=17.7, sep_ratio=0.018 / sepsize=2 (tiny separator), arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **18,889,465,931,478,580,854,784 = exactly 2⁷⁴**; `ganak --prob 0` = same (0.01s) — match ✓. Twin of 055 (band=low + 2-var separator ⇒ trivially decomposable ⇒ instant, arjun=False irrelevant), and again a clean power of two (2⁷⁴ ⇒ near-fully-independent components). Two-for-two on "tiny separator ⇒ COCOA instant."

## mc2026_track1_059 — SOLVED, count = 549,755,813,888 = 2³⁹ (cocoa-plain r1, 0.1s; ganak-verified)

**58v / 304-cl, band=LOW, log2_cost=16.5, sep_ratio=0.052 / sepsize=3 (tiny separator), arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **549,755,813,888 = exactly 2³⁹**; `ganak --prob 0` = same (0.00s) — match ✓. Third in the band=low / tiny-separator / power-of-2 run (**055 = 2⁵⁴, 057 = 2⁷⁴, 059 = 2³⁹**), all instant for both engines. "Tiny separator ⇒ COCOA instant" now three-for-three.

## mc2026_track1_061 — UNSAT, count = 0 (cocoa-plain r1, 0.1s; ganak-verified)

**1492v / 6600-cl, band=LOW, log2_cost=14.6, sep_ratio=0.0 / sepsize=0, arjun substantial=False.** **UNSATISFIABLE — count = 0.** cocoa-plain reported count=0 in round 1, 0.1s; `ganak --prob 0` = `s UNSATISFIABLE`, count 0 (0.02s) — match ✓. **First UNSAT in the sweep**; both engines agree, so count=0 is a genuine UNSAT, not a false-UNSAT soundness bug (the most important case to cross-check). sepsize=0 reflects the formula collapsing under BCP/decomposition to a contradiction.

## mc2026_track1_063 — SOLVED, count = 302,231,454,903,657,293,676,544 = 2⁷⁸ (cocoa-plain r1, 0.1s; ganak-verified)

**116v / 608-cl, band=LOW, log2_cost=19.2, sep_ratio=0.034 / sepsize=4 (tiny separator), arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **302,231,454,903,657,293,676,544 = exactly 2⁷⁸**; `ganak --prob 0` = same (0.01s) — match ✓. Continues the band=low / tiny-separator / power-of-2 run (055=2⁵⁴, 057=2⁷⁴, 059=2³⁹, 063=2⁷⁸); instant for both engines.

## mc2026_track1_065 — SOLVED, count = 3,066 (cocoa-plain r1, 0.1s; ganak-verified)

**2394v / 3986-cl (large var count), band=LOW, log2_cost=29.4, sep_ratio=0.002 / sepsize=5 (tiny separator), n_leaves=1709, arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **3,066**; `ganak --prob 0` = same (0.02s) — match ✓. Same "tiny separator ⇒ instant" pattern despite 2394 vars — **size doesn't matter, decomposability does**. Count 3066 is small and NOT a power of two (so components aren't fully independent, unlike 055/057/059/063), but the 5-var separator still decomposes it trivially.

## mc2026_track1_067 — SOLVED, count = 4 (cocoa-plain round2, 93.2s; ganak-verified 110s) — MIRAGE

**22,711v / 42,833-cl (largest in the sweep), band=HIGH, log2_cost=857, sep_ratio=0.0298 / sepsize=754, n_leaves=20675, arjun substantial=True.** SOLVED by **cocoa-plain** in round 2, **93.2s** active, count = **4**; `ganak --prob 0` = 4 (110s) — match ✓. Both ~1.5–2 min.

**SECOND count-4 MIRAGE (after 027).** The live metrics screamed "gigantic" — n_root≈7276, closed_bits 7272, "pct 7.6%, ~4 bits left" — but the true count is **4**. closed_bits was measuring decomposition-SEARCH depth (a 7272-bit search tree), NOT count proximity; arjun=True correctly flagged the heavy definability/backbone constraining that collapses the count. Two differences from 027: (1) here COCOA **solved the mirage directly** (cocoa-plain round 2 — the search closed all the way down to 4), rather than running away pre-fix; (2) nosep-cascade did NOT help (lagged ~99 bits back — so band=high + arjun=True does *not* always mean a nosep-cascade win, cf 039/045). Reinforces [[cocoa-metrics-mirage]]: a 22k-var / n_root≈7276 instance can have count 4 — never trust the "deep tail" framing.

## mc2026_track1_069 — TIMEOUT (both engines lose); clean frozen-path handoff

**1602v / 23,281-cl, band=HIGH, log2_cost=203, sep_ratio=0.175 / sepsize=424, n_leaves=1447, arjun substantial=False.** Both engines failed — the 029/033 profile (band=high + arjun=False + large separator). Round 1: separator configs led to ~1596 closed_bits (~6 bits from n_root≈1602, pct ~1.6%); cascades (~1528) and adaptives (~1489) lagged and were cut. Round 2: all four survivors (cache-max/reactive/plain/unified-sep) **converged to the same 1596.05 wall and STUCK** — no progress across rounds 2/3 (a hard wall, multiple diverse configs at the identical level). Monitoring leader cocoa-unified-sep frozen at 1596.05 → stall-path bail streak → handoff at ~+16m to ganak-native (2598s / ~43 min); ganak ran its **full ~43-min window (active 2582s) with NO count → TIMEOUT**. Box clean afterward.

- **Both-lose set now 029 / 033 / 047 / 069** — all arjun=False with a large separator (047 band=mid, others band=high). The ~6-bit COCOA tail (pct 1.6%) holds the bulk of the count and neither engine can enumerate it.
- **Handoff economics:** ganak got a *healthy* ~43 min (well above the ~30-min floor) and still lost → for this family the bottleneck is the count's intrinsic hardness, not handoff timing. (Reassuring for the pct-gate: bailing COCOA earlier only helps; it can't hurt a case ganak loses with 43 min.)
- **pct-gate note:** the handoff fired via the FROZEN/stall path (leader rate≈0), not the pct-gate (needs recent_rate>EPS). The pct-gate's *creeping-but-doomed* bail is still unexercised live (049/051/053 near-finishers kept by the floor; 067 mirage solved; 069 froze outright).

## mc2026_track1_071 — SOLVED, count = 25,205,389,326 (cocoa-plain r1, 2.3s; ganak-verified 13.4s)

**387v / 7106-cl, band=HIGH, log2_cost=134, sep_ratio=0.207 / sepsize=118, n_leaves=850, arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **2.3s**, count = **25,205,389,326** (~2.5e10); `ganak --prob 0` = same (13.4s) — match ✓ (COCOA edged ganak). **Breaks the "band=high + arjun=False ⇒ both-lose" expectation:** 071 has that exact profile (large separator included) yet solved in 2.3s.

**Consolidated picture of the predictors (so far):** the only *clean* signals are (a) **arjun=True ⇒ solvable** (039/045 nosep-cascade; 067 mirage; via ganak otherwise) and (b) **tiny separator ⇒ COCOA instant** (055/057/059/063/065). **arjun=False + large separator is UNPREDICTABLE** — solvable {049,051,053,071} vs both-lose {029,033,047,069} — because it comes down to the intrinsic count hardness (071's 2.5e10 is modest), which the structural features only weakly predict. Lesson: don't pre-route arjun=False+large-sep instances; run the race and let the runtime decide.

## mc2026_track1_073 — SOLVED, count = 2,268 (cocoa-plain r1, 1.3s; ganak-verified 1.4s)

**381v / 6918-cl, band=HIGH, log2_cost=135, sep_ratio=0.189 / sepsize=127, n_leaves=835, arjun substantial=True.** SOLVED by **cocoa-plain** in round 1, **1.3s**, count = **2,268**; `ganak --prob 0` = same (1.4s) — match ✓. Near-twin of 071 (387v, band=high, large sep) but **arjun=True** → solved even faster (1.3s vs 071's 2.3s). A mini-mirage (band=high large search, but tiny count 2268, flagged by arjun=True). Confirms the clean signal: **arjun=True ⇒ solvable**.

## mc2026_track1_075 — SOLVED, count = 6,905,169,454 (cocoa-plain r1, 0.5s; ganak-verified)

**37v / 107-cl (tiny), band=LOW, log2_cost=32.2, sep_ratio=0.541 / sepsize=20, arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.5s**, count = **6,905,169,454** (~6.9e9); `ganak --prob 0` = same (0.62s) — match ✓. Tiny (37 vars): high sep_ratio (0.54) but a trivially small search → band=low → instant for both. (Reinforces: at tiny var counts the separator ratio is moot — there's just not enough search to matter.)

## mc2026_track1_077 — SOLVED, count = 1,214,819,892,986 (cocoa-reactive r1, 14.4s; ganak-verified 1.9s)

**80v / 238-cl, band=MID, log2_cost=45.4, sep_ratio=0.3875 / sepsize=31, arjun substantial=False.** SOLVED by **cocoa-reactive** in round 1, **14.4s**, count = **1,214,819,892,986** (~1.2e12); `ganak --prob 0` = same (1.9s) — match ✓. Band=mid + arjun=False family (049/051/053), solved in round 1. Here **ganak (1.9s) was much faster than COCOA (14.4s)** — so arjun=False is NOT always slow for ganak (cf 049/051/053 where ganak took 199–433s); 077's structure was ganak-friendly despite arjun not-substantial.

## mc2026_track1_079 — SOLVED, count = 30,689,663,761,877 (cocoa-nosep-cascade r1, 24.6s; ganak-verified 8.1s)

**90v / 268-cl, band=MID, log2_cost=48.7, sep_ratio=0.389 / sepsize=35, arjun substantial=False.** SOLVED by **cocoa-nosep-cascade** in round 1, **24.6s**, count = **30,689,663,761,877** (~3.07e13); `ganak --prob 0` = same (8.1s) — match ✓ (ganak edged COCOA). Band=mid + arjun=False family. **NOTABLE: the cascade config won** — the same nosep-cascade that *lagged* on every other arjun=False family member (047/049/051/053/067/069) cracked 079's tail in 24.6s while the separator configs stalled at ~2.5 bits in their 60s windows. **Fourth distinct winner type in this family** (049/053→adaptive, 051→plain, 077→reactive, 079→nosep-cascade) — a strong empirical case for the diverse 8-config race: no single config dominates, even within one structural family.

## mc2026_track1_081 — TIMEOUT (both engines lose); cascade-led wall

**320v / 2880-cl, band=HIGH, log2_cost=225, sep_ratio=0.506 / sepsize=162 (very large separator), n_leaves=254, arjun substantial=False.** Both engines failed. Round 1: the **cascade** configs (nosep/sep-cascade) led to 316.342 closed_bits (~3.66 bits from n_root≈320, pct 7.9%) — far ahead of separators (~286, ~34 bits) and adaptives (~300) — then **walled there** (no rounds 2/3 progress; both cascades identical at 316.342). 8→4 kept {cascades, adaptives}; 4→2 hedge kept {nosep-cascade (top-cb, walled), adaptive (min-eta, creeping ~17 bits)}; 2→1 banded took the cascade (|Δcb|≈13 ≫ 1.5). Monitoring leader nosep-cascade frozen at 316.342 → stall-path handoff (~+16m) → ganak-native (2659s). Ganak ran its **full ~44-min window → NO count → TIMEOUT**.

- **Both-lose set now 029/033/047/069/081** — all arjun=False + large separator.
- **Cleanest both-lose telemetry yet:** ganak's final sample = 43.2M conflicts @ ~17k/s, **cubes_resolved=0, cache_entries=0, cache_miss_rate=1.000** → ganak got ZERO decomposition/caching traction; pure brute conflict search, no answer. Neither engine has a structural handle on this family's hard members — it's intrinsic count hardness.
- **pct-gate again unexercised:** the funnel monitored the *walled* cascade (frozen-path bail), not the creeping adaptive. Pattern holds — the banded 2→1 keeps the higher-level config, which tends to be the walled one, so the creeping-but-doomed pct-gate path rarely gets the leader slot.

## mc2026_track1_083 — KILLED mid-ganak (no result; was trending both-lose)

Launched, walled exactly like 081 (cascades led to 295.193 then stuck across rounds 1–3), handed off; ganak ran ~13 min (18k confl/s, cache_K=0 — the both-lose signature) when **killed at the user's request** (had to step away). No count → UNRESOLVED (like 011). Can be re-run to settle it, but was trending both-lose.

## mc2026_track1_085 — TIMEOUT (both engines lose); third cascade-led wall (081/083 triplet)

**320v / 2880-cl, band=HIGH, log2_cost=218.5, sep_ratio=0.475 / sepsize=152, arjun substantial=False** — an exact structural triplet with 081 (320v/2880cl) and 083. Both engines failed. Round 1: cascades led to 312.398 closed_bits (~7.6 bits, pct 0.515%) then **walled** (identical across rounds 1/2/3, both cascade configs); adaptives crept to ~299 (~21 bits, doomed); separators stuck ~285. Funnel: 8→4 {cascades+adaptives} → 4→2 {nosep-cascade walled + adaptive-nosep creeping} → 2→1 nosep-cascade → frozen-path handoff (~+16m) → **ganak-native ran its FULL ~44-min window → NO count → TIMEOUT**. (Run fully at the user's call, to see if ganak would surprise — it did not.)

- **Both-lose set now 029 / 033 / 047 / 069 / 081 / 085** — all arjun=False + large separator.
- ganak telemetry again the both-lose signature: 24.5M conflicts @ ~18k/s, **cubes_resolved=0, cache_entries=0, cache_miss_rate=1.000** — zero decomposition/caching traction, pure brute search. The **081/083/085 triplet** (near-identical 320-var structures) is consistently both-lose.

## mc2026_track1_087 — SOLVED, count = 17,528,422,464 (cocoa-plain r1, 1.8s; ganak-verified)

**39v / 256-cl (tiny), band=LOW, log2_cost=25.5, sep_ratio=0.308 / sepsize=12, arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **1.8s**, count = **17,528,422,464** (~1.75e10); `ganak --prob 0` = same (0.19s) — match ✓. Tiny + band=low → trivial for both engines; a clean break from the 081/083/085 both-lose triplet (those were 320v / band=high).

## mc2026_track1_089 — UNSAT, count = 0 (cocoa-plain r1, 0.1s; ganak-verified)

**8281v / 15,164-cl (large), band=LOW, log2_cost=35.5, sep_ratio=0.0006 / sepsize=5, arjun substantial=False.** **UNSATISFIABLE — count = 0.** cocoa-plain reported count=0 in round 1, 0.1s; `ganak --prob 0` = `s UNSATISFIABLE`, count 0 (0.02s) — match ✓. **Second UNSAT in the sweep** (after 061); both engines agree → genuine UNSAT, not a false-UNSAT soundness bug. Large (8281 vars) but the contradiction surfaces instantly under BCP (tiny 5-var separator, fully decomposable).

## mc2026_track1_091 — UNSAT, count = 0 (cocoa-reactive r1, 0.1s; ganak-verified)

**29,456v / 54,597-cl (largest instance in the sweep), band=MID, log2_cost=72.3, sep_ratio=0.0004 / sepsize=14, arjun substantial=True.** **UNSATISFIABLE — count = 0.** cocoa-reactive round 1, 0.1s; `ganak --prob 0` = `s UNSATISFIABLE`, count 0 (0.10s) — match ✓. **Third UNSAT** (061/089/091); both engines agree → genuine. Largest instance yet (29k vars) but the contradiction surfaces instantly under BCP.

## mc2026_track1_093 — SOLVED, count = 1 (cocoa-plain r1, 0.1s; ganak-verified)

**182v / 385-cl, band=LOW, log2_cost=15.7, sep_ratio=0.022 / sepsize=5, arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **1** (uniquely satisfiable — exactly one solution); `ganak --prob 0` = 1 (0.00s) — match ✓. band=low + tiny separator → trivial for both.

## mc2026_track1_095 — SOLVED, count = 4,503,632 (ganak-handoff, 74.8s) — FIRST LIVE pct-gate bail → ganak rescue

**729v / 3174-cl, band=LOW but a DEEP decomposition search (live n_root≈594 ≫ log2_cost 23), sep_ratio≈0, arjun substantial=False.** COCOA configs ground to ~500–583 closed_bits; the monitored leader (cocoa-nosep-cascade) **creeped-but-doomed** at 583.4 (~10.6 bits from n_root, pct 0.066%, ~0.08 b/min). **FIRST LIVE FIRING OF THE pct-GATE:** every bail verdict read `progressing but doomed (pct-eta): rem=10.6 > log2budget=2.65 (eta_pct≈1.31M s ≈ 15 days, rate=0.0017 b/s)` — the log-space pct-ETA correctly judged the *creeping* leader doomed (the old linear eta would've been ~1.75 h — borderline-keep). 10 verdicts → handoff → **ganak-native solved in ~75s → count = 4,503,632** (ganak's exact `--prob 0`, the oracle; COCOA was bailed so no cross-check applies).

- **MILESTONE — the pct-gate's designed flow worked end-to-end in production:** creeping-but-doomed COCOA leader → pct-ETA bail → ganak rescue → solve. 069/081/083/085 never exercised this (they FROZE → stall-path bail; all both-lose); **095 is the first where the creeping-doomed bail → ganak actually WON.** Validates the log-space pct-ETA gate live (rem > log2(ln2·rate·margin·t_rem)), and confirms the numerics hold (eta_pct printed cleanly at ~1.3e6 s, no overflow; pct_lin never materialized).
- **Funnel note:** reactive's round-2 +20.8-bit jump was a ONE-TIME cache-hit — it then sat dead flat at 576.209 through all of round 3 (zero bits closed, ~84k decisions burned). So the banded 2→1 keeping the higher-level nosep-cascade over reactive was correct; the apparent "fast mover" had already walled.

## mc2026_track1_097 — SOLVED, count = 3 (cocoa-reactive r1, 0.1s; ganak-verified)

**64v / 387-cl (tiny), band=MID, log2_cost=41.2, sep_ratio=0.5 / sepsize=32, arjun substantial=True.** SOLVED by **cocoa-reactive** in round 1, **0.1s**, count = **3**; `ganak --prob 0` = 3 (0.00s) — match ✓. Tiny + arjun=True → small count cracked instantly (the arjun=True ⇒ solvable signal again; cf 067/073's small counts).

## mc2026_track1_099 — SOLVED, count = 4,503,632 (cocoa-cache-max round3, 179.6s; ganak-verified 59.3s)

**729v / 10,545-cl, band=HIGH, log2_cost=538, sep_ratio=0.497 / sepsize=362, arjun substantial=True.** SOLVED by **cocoa-cache-max** in round 3, **179.6s**, count = **4,503,632**; `ganak --prob 0` = same (59.3s) — match ✓.

**Twin of 095:** identical count (4,503,632) AND identical var count (729), but different clauses (10,545 vs 3,174) and band (high vs low) — almost certainly the same underlying problem in two encodings. Instructive contrast in *how* they solved: **095** (band=low, deep search n_root≈594) → COCOA leader creeped-but-doomed → **pct-gate bail → ganak rescue**; **099** (band=high) → COCOA **solved directly** (cache-max broke the ~5-bit plateau in round 2 and closed in round 3). Same answer, opposite engine paths — the diverse race + handoff covers both. arjun=True ⇒ solvable held again.

## mc2026_track1_101 — SOLVED, count = 3 (cocoa-plain r1, 0.1s; ganak-verified)

**64v / 147-cl (tiny), band=LOW, log2_cost=8.0, sep_ratio=0.125 / sepsize=22, arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **3**; `ganak --prob 0` = 3 (0.00s) — match ✓. Tiny + band=low → trivial for both.

## mc2026_track1_103 — SOLVED, count = 1 (cocoa-plain r1, 0.1s; ganak-verified)

**64v / 148-cl (tiny), band=LOW, log2_cost=8.0, sep_ratio=0.0625 / sepsize=20, arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **1** (uniquely satisfiable); `ganak --prob 0` = 1 (0.00s) — match ✓. Near-twin of 101 (64v/147cl, band=low, log2_cost 8) — sibling instances (count 1 vs 101's 3).

## mc2026_track1_105 — SOLVED, count = 288 (cocoa-reactive r1, 0.1s; ganak-verified)

**64v / 384-cl (tiny), band=MID, log2_cost=41.17, sep_ratio=0.484 / sepsize=31, arjun substantial=False.** SOLVED by **cocoa-reactive** in round 1, **0.1s**, count = **288**; `ganak --prob 0` = 288 (0.42s) — match ✓. Same structural shape as 097 (64v, band=mid, identical log2_cost 41.17) but arjun=False / count 288 vs 097's arjun=True / count 3 — another sibling pair.

## mc2026_track1_107 — SOLVED, count = 2.7×10¹³³ (134 digits) (cocoa-adaptive r1, 20.9s; ganak-verified)

**1271v / 3027-cl, band=MID, log2_cost=53.7, sep_ratio=0.021 / sepsize=27 (tiny separator → fully decomposable), arjun substantial=True.** SOLVED by **cocoa-adaptive** in round 1, **20.9s**, count = **27074324552188263115193324525043910411466079287194737862239975111039283194478178486570350758010456761223622424071136573497534086266600** (~2.7e133, ≈2⁴⁴³); `ganak --prob 0` = same (2.33s; exact full-string match ✓). **NOT a mirage** — a genuinely enormous count, computed efficiently *because* the tiny separator makes it a product over near-independent components (huge count ⇏ hard, when decomposable). **(134 digits)** — the largest in the sweep at the time (later surpassed by 109/111/113). Reinforces: count magnitude is orthogonal to difficulty — decomposability is what matters.

## mc2026_track1_109 — SOLVED, count = 3.6×10¹⁴¹ (142 digits) (cocoa-plain r1, 0.1s; ganak-verified)

**631v / 1312-cl, band=LOW, log2_cost=14.3, sep_ratio=0.006 / sepsize=4 (tiny separator), arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **3599239755983329331332100508562451780508192148493160801718199944973008026807919208513108710328389951098075842967611059200000000000000000000000** (~3.6e141, 142 digits); `ganak --prob 0` = same (0.22s; exact full-string match ✓). **New sweep record** (142 digits, beats 107's 134) yet **instant** — band=low + 4-var separator → fully decomposable. Magnitude ⊥ difficulty, again.

## mc2026_track1_111 — SOLVED, count = 2.7×10¹²³ (124 digits) (cocoa-adaptive r1, 5.3s; ganak-verified)

**1271v / 3156-cl, band=MID, log2_cost=54.9, sep_ratio=0.0197 / sepsize=25 (tiny separator), arjun substantial=True** — near-twin of 107 (1271v, band=mid, tiny sep, arjun=True). SOLVED by **cocoa-adaptive** in round 1, **5.3s**, count = **2716090255555394786865673931721659870192662048106344246114776105307491592972573691035248570056672524173798505601634040233480** (~2.7e123, 124 digits); `ganak --prob 0` = same (1.89s; exact full-string match ✓). Decomposable → another huge count computed fast (cf 107's 134-digit solve).

## mc2026_track1_113 — SOLVED, count = 2.4×10¹²⁶ (127 digits) (cocoa-adaptive r1, 8.4s; ganak-verified)

**1253v / 3039-cl, band=MID, log2_cost=56.2, sep_ratio=0.0208 / sepsize=26 (tiny separator), arjun substantial=True** — 107/111 family. SOLVED by **cocoa-adaptive** in round 1, **8.4s**, count = **2386179727798317443182387728759025280637542166208758384036037409346857065068615755489422934430477849440649064528671759799828200** (127 digits); `ganak --prob 0` = same (1.94s; exact full-string match ✓). Third in the ~1253–1271-var / band=mid / tiny-sep / arjun=True / huge-count cluster (107/111/113) — all cocoa-adaptive round-1 decompose-solves.

## mc2026_track1_115 — SOLVED, count = 3,865,470,566,400 (cocoa-reactive r1, 53.5s; ganak-verified 1.3s)

**6470v / 26,053-cl (large), band=MID, log2_cost=79.0, sep_ratio=0.0015 / sepsize=13 (tiny separator), arjun substantial=True.** SOLVED by **cocoa-reactive** in round 1, **53.5s**, count = **3,865,470,566,400** (~3.87e12, 13 digits); `ganak --prob 0` = same (1.31s) — match ✓. Same decomposable / arjun=True family as 107/111/113 but a **modest** count (13 digits). Here the large size (6470v) made COCOA's decomposition take ~53s while **ganak's Arjun cracked it in 1.3s** (ganak much faster) — so within this family COCOA's time is driven by var count (decomposition work), not by the count magnitude.

## mc2026_track1_117 — SOLVED, count = 259,288,096 (cocoa-reactive r1, 0.7s; ganak-verified)

**2344v / 9045-cl, band=MID, log2_cost=63.6, sep_ratio=0.014 / sepsize=33 (tiny separator), arjun substantial=True.** SOLVED by **cocoa-reactive** in round 1, **0.7s**, count = **259,288,096** (~2.6e8); `ganak --prob 0` = same (0.46s) — match ✓. Decomposable + arjun=True + modest count → fast for both.

## mc2026_track1_119 — SOLVED, count = 1,000,000 = 10⁶ (cocoa-plain monitor, 275.8s; ganak-verified 286s)

**47,647v / 204,121-cl (BY FAR the largest in the sweep), band=HIGH, log2_cost=1279 (deepest yet), sep_ratio=0.013 / sepsize=620, arjun substantial=True.** SOLVED by **cocoa-plain** in the monitor phase, **275.8s**, count = **1,000,000** (exactly 10⁶); `ganak --prob 0` = same (285.95s) — match ✓. Both engines ~4.7 min (comparable). Despite 47k vars / 204k clauses, it decomposed to ~0.9 bits (pct ~55%) within 1 min; the remaining ~21% (one last big component) took the rest of the time to enumerate exactly to 10⁶. Clean round count (10⁶ = 2⁶·5⁶). Largest instance in the sweep — both engines solve it.

## mc2026_track1_121 — SOLVED, count = 55,143 (ganak-handoff, 93.9s) — MIRAGE + fragile-but-correct handoff

**5259v / 15,676-cl, band=HIGH, log2_cost=188.5, sep_ratio=0.020 / sepsize=105 (tiny metis separator — OVER-PROMISED), arjun substantial=False.** SOLVED via ganak-handoff, count = **55,143** (ganak's exact `--prob 0`, the oracle; COCOA was bailed so no cross-check).

**The count is TINY (55,143 ≈ 2¹⁶) — a mirage.** COCOA's live metrics screamed deep (n_root≈5099, closed_bits ~5098, "~1 bit remaining"), but closed_bits measures decomposition-search depth (≈ var count), not the count. The tiny metis separator split the top cleanly, but one resulting **component** was the hard part: COCOA decomposed ~65% then got stuck crawling that one component (decision rate ~96/s — barely searching; pct bursting 47%→65% over 25 min in fits-and-starts), while ganak's Arjun cracked the whole thing in 94s.

**Handoff — right outcome, fragile mechanism, and it exposed two design bugs (both confirm the eta-vs-bits critique):**
1. **REM_FLOOR(2 bits) is a bits-cutoff that contradicts the pct-gate.** It kept the creeping-but-doomed near-finisher (rem~1 bit, pct-ETA ~hours) out of the eta-bail for ~25 min. The handoff only fired by luck when the leader fully **STALLED** at +43m → dropped into the frozen/stall branch (reason: "no escapes ever") which REM_FLOOR doesn't guard. A steadily-creeping leader would have ridden to a timeout instead.
2. **The escape/patience detector is closed_bits-based, blind to pct-bursts near the finish.** 121's bursts were big in pct (count-chunks resolving) but tiny in cb (~0.06-bit steps near n_root), so the tree credited **0 escapes** → gave the leader **no patience** → bailed at the first stall. The opposite of "show patience after a strong beginning."

**Cost of the bugs:** had the gate been pct-aware + ETA-driven (not bits-floored), it would have handed off ~+16m → solved ~+18m total, vs the ~+45m actually spent. **FIX queued:** REM_FLOOR(2) → tiny sliver guard (~0.2 bits); route near-finishers through the escape-history-gated pct-ETA; and move escape/patience detection into pct-space near the finish (so count-chunk bursts earn patience). Regression test to be seeded with 121's trajectory.

## mc2026_track1_123 — UNSAT, count = 0 (cocoa-plain r1, 0.2s; ganak-verified)

**10,445v / 41,110-cl (large), band=HIGH, log2_cost=587.6, sep_ratio=0.038 / sepsize=401, arjun substantial=True.** **UNSATISFIABLE — count = 0.** cocoa-plain round 1, 0.2s; `ganak --prob 0` = `s UNSATISFIABLE`, count 0 (0.42s) — match ✓. **Fourth UNSAT** (061/089/091/123); both engines agree → genuine. Large + band=high, but the contradiction surfaces instantly under BCP/conflict (arjun=True notwithstanding).

## mc2026_track1_125 — SOLVED, count = 1 (cocoa-plain r1, 0.1s; ganak-verified)

**1v / 1-cl — a DEGENERATE instance** (the single clause is the unit `x1`; nd_cost N/A, sep_ratio=-1, n_root=None). SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **1** (only x1=true satisfies); `ganak --prob 0` = 1 (0.00s) — match ✓. Edge-case instance in the set; both engines trivially agree.

## mc2026_track1_127 — TIMEOUT, both engines lose (count unknown) — FIRST arjun=True both-lose

**2550v / 7126-cl, band=MID, log2_cost=84.26, sep_ratio=0.020 / sepsize=52 (tiny root separator), arjun substantial=True; nd_cost max_path=78.0, n_leaves=1206, worst_leaf=1.** **TIMEOUT — neither engine returned a count in the 60-min budget.** COCOA raced all 8 configs; the funnel cut to **cocoa-reactive** (banded near-tie |Δcb|<1.5b → min ETA), which **walled at closed_bits=2494.91 / n_root≈2525 (~30 bits unclosed, pct_lin≈8.8e-8%)** — decisions churned 10M→18M with cb dead-flat (pure forced-search churn, zero decomposition progress). The escape-history tree fired the handoff after 10 consecutive 'bail' verdicts (leader walled) at **~+15.7 min** (ganak given 2659s left). **ganak-native then ran its full 2659s (~44.3 min) budget** — RSS fluctuating 5.5–14 GB (well under the 26 GB cap → NOT memory-bound), ~100% CPU throughout — and **also returned no count.** No count to verify (both engines exhausted their budgets).

**Why it matters — 127 breaks BOTH of the sweep's "easy" structural signals at once:**
- **arjun substantial=True ⇏ ganak-tractable.** Every prior both-lose (029/033/047/069/081/083/085) was arjun=False + large separator; 095/121 were arjun-driven ganak rescues. 127 is the **first arjun=True instance in the sweep to beat both engines** — Arjun's definability/backbone preprocessing did NOT crack it.
- **Tiny root separator (0.020) ⇏ COCOA-decomposable.** Structurally 127 is a near-twin of the 107/111/113/115/117 family (band=mid, sep_ratio≈0.020, arjun=True), all of which COCOA decompose-solved in round 1 (0.7–53s) — yet 127 walled. At runtime COCOA stalled ~30 bits short of n_root (decisions churned 10M→18M with closed_bits dead-flat).

So 127 is a genuine hard instance for the current portfolio: cleanly decomposable at the root but with a tail that neither COCOA's component search nor ganak's caching+preprocessing could close in an hour. **Count remains unknown.**

> **⚠️ CORRECTION (added after mc2026_129, same session):** an earlier draft of this entry blamed 127's hardness on `nd_cost max_path=78` ("the deep dissection path is the real predictor"). **mc2026_129 refutes that** — 129 had `max_path=1054` (13× larger) plus a *large* root sep (0.564) yet cocoa-plain solved it in **23.5s**. So max_path does NOT predict difficulty either. **Honest takeaway: NO step-1 structural feature (sep_ratio, max_path, band, arjun) reliably predicts whether 127-class instances are solvable — the runtime race + handoff is the only reliable mechanism (cf. the mirage entries 027/067/121).** 127's hardness remains unexplained by available features.

## mc2026_track1_129 — SOLVED, count = 752,061 (cocoa-plain r1, 23.5s; ganak-verified)

**1225v / 42,876-cl (clause-DENSE, ~35 cl/var), band=HIGH, log2_cost=1054.9, sep_ratio=0.564 / sepsize=704 (LARGE separator, balance 0.172), max_path=1054.0, n_leaves=78, arjun substantial=True.** SOLVED by **cocoa-plain** in round 1, **23.5s**, count = **752,061** (~7.5×10⁵); `ganak --prob 0` = 752061 (fast; exact match ✓). Trajectory shows the decompose burst: at 10s closed_bits=491 / pct≈3e-6% (looked stuck), at 20s closed_bits=513 / pct≈12.5% (a jump), finished at 23.5s.

**Why it matters — 129 REFUTES the `max_path` lesson I drew from 127 (correction noted in the 127 entry above).** 129's features all screamed HARD — band=high, a *large* root separator (0.564, 704/1225 vars), and **max_path=1054** (13× larger than 127's 78, near 119's 1279) — yet cocoa-plain cracked it in 23.5s. 127 (max_path=78, tiny sep) walled both engines; 129 (max_path=1054, large sep) solved instantly. **So max_path does NOT predict difficulty, and neither do sep_ratio / band** — the nd_cost estimate (log2_cost≈1055) massively over-predicted. Mirage theme again: step-1 metrics measure a worst-case decomposition *shape*, not what the search actually hits. Routing decision stands: race + handoff, don't pre-judge from features.

## mc2026_track1_131 — SOLVED, count = 336,067,810 (ganak-handoff, ~714s ganak; COCOA walled) — 127's twin, opposite outcome

**2304v / 110,593-cl (clause-DENSE, ~48 cl/var), band=HIGH, log2_cost=1938.7, sep_ratio=0.682 / sepsize=1577 (LARGE separator, balance 0.249), max_path=1938.0, n_leaves=108, arjun substantial=True.** SOLVED via **ganak-handoff**, count = **336,067,810** (~3.36×10⁸; ganak's exact `--prob 0`, the oracle — COCOA was bailed, so no cross-check, per the 095/121 precedent).

**COCOA walled like 127.** All 8 configs deep (funnel 8→4→2→1 over ~14 min; the 4 survivors clustered tightly at 823–825 closed_bits, all decelerating — r1 vel ~1.3 b/s collapsing to ~0.05). Leader **cocoa-unified-sep** monitored, walled ~168 bits short of n_root (≈996, pct≈2.5e-49); the escape-tree handed off at **~+15.7 min** (10-bail streak, "leader walled").

**But unlike 127, ganak SOLVED it** — in **~714s (~11.9 min)**: ~8 min of Arjun/cadiback/CMS preprocessing, then ~4 min of cached counting (cache peaked only ~285K entries, ~9000 conflicts/s) → 336,067,810. **Same arjun=True / large-sep / deep-wall shape as 127, opposite outcome** — 127's ganak ran the full 44-min budget, cache ballooning to 12–14 GB, and timed out. The differentiator is **post-Arjun count tractability**, which no step-1 feature exposes (both are band=high, arjun=True, large sep, huge max_path); only the runtime race + handoff reveals it. So arjun=True is non-deterministic in BOTH directions (127 ganak-timeout, 131 ganak-solve).

**Observability note (corrects the "ganak has no progress signal" claim from the 127 entry/session):** the pipeline's `[ganak~live]` line transitioning from BLANK (`cache_K=- confl/s=-`, active 60–421s) to POPULATED (`cache_K=285 confl/s=8961.80`, by active 541s) IS the visible marker that **Arjun/preprocessing finished and the cached-counting phase began** (~+8 min into ganak here). Blank = still in preprocessing (no periodic conflict/cache stats emitted under `--verb 1`); populated = counting. RSS corroborates (0.4 GB during preprocessing → grows once counting starts). [Still not surfaced: Arjun's exact reduced var/clause numbers — the handoff reads ganak's stdout into an in-memory parser (count + 5 stat regexes) and does NOT tee the raw `c o` lines to disk; teeing them would expose the reduction size too.]

## mc2026_track1_133 — SOLVED, count = 1,262,816 (cocoa-plain r1, 39.0s; ganak-verified)

**1296v / 46,657-cl (clause-dense, ~36 cl/var), band=HIGH, log2_cost=1091.5, sep_ratio=0.594 / sepsize=770 (LARGE separator, balance 0.030 — very imbalanced), max_path=1090.0, n_leaves=78, arjun substantial=True.** SOLVED by **cocoa-plain** in round 1, **39.0s**, count = **1,262,816** (~1.26×10⁶); `ganak --prob 0` = 1262816 (match ✓). Burst-and-finish like 129: pct climbed 1.4e-6 (10s) → 7.6e-4 (20s) → 0.39 (30s) → done at 39s.

**The 129/131/133 triple — near-identical static profile, three different outcomes.** All three are band=high + huge max_path (1054 / 1938 / 1090) + arjun=True (129 tiny sep, 131/133 large sep), yet: **129 COCOA-solved in 23.5s, 131 COCOA-walled→ganak-solved (~12 min), 133 COCOA-solved in 39s.** Static step-1 features are useless for routing across these. What actually separated them was the RUNTIME early trajectory: 129/133's pct climbed toward feasibility within ~30s (→ COCOA finishes), while 131's stayed flat at ~1e-85 (→ 168-bit wall → handoff). Takeaway: the funnel's live closed_bits/pct trajectory is the real solvability signal; the structural metrics only set the (pessimistic) band. Three more confirmations that decomposability/count-tractability is invisible up front.

## mc2026_track1_135 — SOLVED, count = 13 (cocoa-plain r1, 0.1s; ganak-verified)

**675v / 2194-cl (small/sparse, ~3.3 cl/var), band=LOW, log2_cost=31.1, sep_ratio=0.033 / sepsize=24 (tiny separator), max_path=23.0, n_leaves=866, arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **13**; `ganak --prob 0` = 13 (match ✓). Trivial — small + sparse + band=low + tiny separator → instant for both. A breather after the 129/131/133 band=high cluster.

## mc2026_track1_137 — SOLVED, count = 18 (cocoa-reactive r1, 0.9s; ganak-verified)

**6316v / 20,932-cl (sparse, ~3.3 cl/var), band=MID, log2_cost=63.0, sep_ratio=0.0079 / sepsize=55 (tiny separator), max_path=51.0, n_leaves=8354, arjun substantial=False.** SOLVED by **cocoa-reactive** in round 1, **0.9s**, count = **18**; `ganak --prob 0` = 18 (match ✓). The tiny root separator (0.0079) made it fully decomposable despite 6316 vars → instant solve, small count. (Reminder from 127: a tiny root_sep doesn't *guarantee* easiness — but here, with band=mid and a shallow tree, it delivered.)

## mc2026_track1_139 — TIMEOUT, both engines lose (count unknown) — arjun=False + deep-wall pattern

**4029v / 3876-cl (UNDERconstrained, ~0.96 cl/var — fewer clauses than vars), band=HIGH, log2_cost=161.8, sep_ratio=0.028 / sepsize=113 (tiny separator), max_path=153.0, n_leaves=3876, arjun substantial=False.** **TIMEOUT — neither engine returned a count in the 60-min budget.**

COCOA: all 8 configs deep (funnel 8→4→2→1 over ~14 min; r1 closed_bits clustered ~3902–3918, n_root≈4029 = the full var count since arjun=False ⇒ no preprocessing reduction → **~111–127 bits unclosed**, leader ~111; vel ~0.04–0.10 b/s, decelerating). Leader **cocoa-reactive** monitored, walled, escape-tree handed off at **~+15.7 min** (10-bail streak).

ganak-handoff: ran its full **2659s (~44.3 min)** budget — RSS grew to **14.8 GB**, 100% CPU, and the `[ganak~live]` heartbeat stayed **BLANK the entire run** (never emitted parseable count-phase stats → never cleanly entered/finished counting; contrast 131, where it flipped populated ~+8 min then solved). No count.

**Second both-lose of the sweep (after 127), but a *predictable* one.** Unlike 127 (arjun=True surprise both-lose), 139 fits the established **arjun=False + deep-wall both-lose pattern** (029/033/047/069/081/083/085 — all arjun=False). The tiny root separator (0.028) did NOT save it (same lesson as 127: tiny root_sep ⇏ easy). Underconstrained/huge-count formula with no definability structure for Arjun to exploit ⇒ neither COCOA's decomposition search nor ganak's caching cracks it in an hour. **Count remains unknown.**

## mc2026_track1_141 — TIMEOUT, both engines lose (count unknown) — the SCALE wall (1.17M vars)

**1,171,761v / 933,978-cl (BY FAR the largest in the sweep — ~25× 119's 47k vars; UNDERconstrained, ~0.80 cl/var), band=UNKNOWN (nd_cost could not compute the tree — all metrics None), sep_ratio=0.000456 / sepsize=534 (extraordinarily tiny separator, smallest in the sweep), arjun substantial=False.** **TIMEOUT — neither engine returned a count in the 60-min budget.** A distinct both-lose CLASS: the SCALE wall, not a deep tail.

- **nd_cost itself failed** — `band=unknown`, log2_cost/root_sep/max_path all `None`. The 1.17M-var dissection tree exceeded what nd_cost can compute (first time in the sweep with no band signal); the pipeline fell back to the default 8-config set.
- **COCOA never closed a single bit.** All 8 configs spent their entire round-1 60s windows just LOADING the instance (parse + implication graph + BCP on 1.17M vars / 934k clauses) → every funnel cut (8→4→2→1) was a degenerate tie at `closed_bits=0`, resolved by list order/ETA. The leader (cocoa-plain) reached only ~295 decisions / **18.2 GB RSS** / 0 closed_bits in 180s active before the escape-tree handed off at ~+17 min. (System stayed 83% free — not thrashing; the 18 GB is the genuine footprint of the 1.17M-var structures.)
- **ganak spent its ENTIRE ~43-min handoff budget in PREPROCESSING** — RSS pinned flat at **1.7 GB** and the `[ganak~live]` heartbeat blank from active 60s through 2523s (never populated ⇒ never reached the counting phase). Arjun/cadiback/CMS simplification on a million-variable formula did not finish in 43 min. Clean full-budget timeout (no crash/OOM/watchdog).

**Third both-lose of the sweep, but a new kind.** Unlike 127 (arjun=True surprise) and 139 (arjun=False deep-wall), 141 is a pure SIZE/setup wall: the instance is so large that neither engine completes even its *preparatory* phase (COCOA's load, ganak's preprocessing) within the hour. The tiny separator (0.000456) is moot when the decomposition never gets started. **Operational note:** on ~1M-var instances nd_cost returns band=unknown and the 60s funnel windows aren't even enough to LOAD the CNF, so the funnel is fully degenerate — if more million-var instances appear, expect this same scale-wall signature. **Count remains unknown.**

## mc2026_track1_143 — TIMEOUT, both engines lose (count unknown) — 139's twin (arjun=False deep-wall)

**5016v / 4845-cl (UNDERconstrained, ~0.97 cl/var), band=HIGH, log2_cost=180.3, sep_ratio=0.025 / sepsize=127 (tiny separator), max_path=172.0, n_leaves=4845, arjun substantial=False.** **TIMEOUT — neither engine returned a count in the 60-min budget.** A near-exact repeat of **139** (4029v, band=high, tiny sep 0.028, arjun=False, deep wall): same funnel shape (8 configs clustered ~4871–4878 closed_bits, n_root≈5016 = full var count, **~138 bits unclosed**, vel ~0.05 decelerating), same survivors at every cut (8→4 reactive/cache-max/plain/unified-sep; 4→2 reactive+cache-max; leader **cocoa-reactive**), handoff at **~+15.7 min**. ganak then ran its full **~44-min** budget — RSS grew to ~12 GB by minute 13 then plateaued (flat, `[ganak~live]` heartbeat blank throughout, never reached a parseable count phase), no count. Clean full-budget timeout (no crash/watchdog).

**Fourth both-lose of the sweep**, and the second of the **arjun=False + tiny-sep + band=high + underconstrained** flavor (139, 143). The pattern is now firm: this structural class is a reliable both-lose — COCOA can't decompose the deep tail, and ganak (no definability/backbone shortcut with arjun=False) grinds the whole budget without finishing. (Sweep both-lose tally now 127/139/141/143 — three mechanisms: arjun=True surprise [127], arjun=False deep-wall [139/143], pure scale [141].) **Count remains unknown.**

## mc2026_track1_145 — SOLVED, count = 1 (cocoa-nosep-cascade monitor, 583.5s; ganak-verified, ganak 73s)

**1371v / 4512-cl (~3.3 cl/var), band=HIGH, log2_cost=102.7, sep_ratio=0.028 / sepsize=38 (tiny separator), max_path=98.0, n_leaves=1028, arjun substantial=False.** SOLVED by **cocoa-nosep-cascade** in the monitor phase, **583.5s (~9.7 min)**, count = **1** (uniquely satisfiable); `ganak --prob 0` = 1 (match ✓), ganak's own time **73.4s** (Arjun+GANAK).

**Breaks the 139/141/143 both-lose streak — and shows again that runtime, not structure, is the discriminator.** Same band=high + tiny-sep + arjun=False *flavor* as 139/143, but it SOLVED. The difference was visible at runtime from the first window: 145's leader sat at **pct 25.7% / ~2 bits remaining after 60s** (n_root≈1047; smaller/shallower — 1371 vars, max_path 98) and then climbed steadily 25→52→85→96→100% to close every branch and confirm the single solution; 139/143 sat at pct≈1e-41 / ~140 bits and never moved. So the live early-pct trajectory cleanly separated this solvable instance from its both-lose look-alikes — the static features (band=high, tiny sep, arjun=False) were identical. Footnote: ganak solved it in 73s vs COCOA's 583s (~8× faster), but COCOA finished inside the budget so no handoff fired — within-budget speed gaps don't matter to the portfolio outcome.

## mc2026_track1_147 — SOLVED, count = 65,536 = 2¹⁶ (cocoa-plain monitor, 539.3s; ganak-verified, ganak 135s)

**1541v / 5081-cl (~3.3 cl/var), band=HIGH, log2_cost=110.0, sep_ratio=0.025 / sepsize=38 (tiny separator), max_path=105.0, n_leaves=1152, arjun substantial=False.** SOLVED by **cocoa-plain** in the monitor phase, **539.3s (~9 min)**, count = **65,536 = 2¹⁶**; `ganak --prob 0` = 65536 (match ✓), ganak's own time **135.3s** (Arjun+GANAK).

Near-twin of 145 (band=high, tiny sep, arjun=False) and same outcome — a COCOA monitor-phase solve. The leader (cocoa-plain) rode the same climbing trajectory (r1 pct 34.5% / ~1.5 bits → steady 49→59→68→76→82→89→95→100% over the monitor phase) to confirm exactly 2¹⁶ solutions. **Two consecutive solves (145/147) of the same band=high / tiny-sep / arjun=False profile that 139/143 both-lost** — the plausible divergence is sheer size: 145/147 are ~1400–1500 vars (n_root ~1050–1180), vs 139/143's ~4000–5000 vars (n_root ≈5016), so far fewer components to close and COCOA finishes the decomposition in ~9 min where the larger pair walls. (Consistent across these four, but per 129's refutation of static-feature predictors, the only *reliable* discriminator remains the runtime early-pct trajectory — 145/147 showed ~1–2 bits remaining at 60s, 139/143 showed ~140.) ganak (135s) again faster than COCOA (539s), but COCOA finished in budget → no handoff.

## mc2026_track1_149 — SOLVED, count = 4 (cocoa-adaptive r1, 28.4s; ganak-verified)

**289v / 4096-cl (small but HEAVILY constrained, ~14.2 cl/var — densest in the sweep), band=MID, log2_cost=82.2, sep_ratio=0.118 / sepsize=34, max_path=80.0, n_leaves=84, arjun substantial=False.** SOLVED by **cocoa-adaptive** in round 1, **28.4s**, count = **4**; `ganak --prob 0` = 4 (match ✓, ganak 39.6s). Small + dense (14 cl/var) ⇒ heavily constrained ⇒ tiny count; quick for both. (n_root≈289 looked moderately deep, but cocoa-adaptive closed the decomposition to 4 in <30s — while cocoa-reactive was still at pct 67.6% in its window, adaptive finished.)

## mc2026_track1_151 — SOLVED, count = 65,536 = 2¹⁶ (cocoa-nosep-cascade monitor, 889.0s; ganak-verified, ganak 177s)

**361v / 5184-cl (small + dense, ~14.4 cl/var), band=MID, log2_cost=89.3, sep_ratio=0.105 / sepsize=38, max_path=86.0, n_leaves=145, arjun substantial=False.** SOLVED by **cocoa-nosep-cascade** in the monitor phase, **889.0s (~14.8 min)**, count = **65,536 = 2¹⁶**; `ganak --prob 0` = 65536 (match ✓, ganak 177s). Near-twin of 149 (small/dense/band=mid/arjun=False) but a longer haul — the leader rode the climbing trajectory (r1 ~2.3 bits → steady ~6%/min over a ~13-min monitor phase) to confirm 2¹⁶ (same count as 147). **Branch-sensitive:** the r1 configs split widely — adaptive/plain/cache-max landed at ~2 bits remaining while reactive/unified-sep sat at ~14 (different decomposition branches from the same instance) — and the level-cut correctly kept the near-finishers. ganak 177s vs COCOA 889s, but COCOA finished in budget → no handoff.

## mc2026_track1_153 — SOLVED, count = 6 (cocoa-plain r1, 0.1s; ganak-verified)

**509v / 26,029-cl (small but EXTREMELY dense, ~51 cl/var — densest in the sweep), band=HIGH, log2_cost=182.0, sep_ratio=0.287 / sepsize=146, max_path=182.0, worst_leaf=104, n_leaves=61, arjun substantial=True.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **6**; `ganak --prob 0` = 6 (match ✓, ganak 0.02s). **Another band=high mirage** — the nd_cost estimate (log2_cost 182, worst_leaf 104) screamed hard, but the count is 6 and both engines solved instantly (extreme density + arjun=True ⇒ heavily constrained ⇒ tiny count; Arjun/cadiback collapsed it in 0.02s). Reinforces: band/log2_cost is a worst-case decomposition estimate, orthogonal to actual difficulty.

## mc2026_track1_155 — SOLVED, count = 309,623,079,113,133,176,160 (~3.1×10²⁰) (cocoa-plain r1, 0.1s; ganak-verified)

**111v / 203-cl (tiny/sparse, ~1.8 cl/var), band=LOW, sep_ratio=0.018 / sepsize=2 (tiny separator), arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **309623079113133176160** (~3.1×10²⁰, 21 digits, ≈2⁶⁸); `ganak --prob 0` = same (exact full-string match ✓, ganak 0.03s). Tiny + band=low + a 2-var separator → fully decomposable → instant, with a large count emerging as the product of near-independent components. Magnitude ⊥ difficulty again (cf the 107/109/111 huge-count family).

## mc2026_track1_157 — SOLVED, count = 1.24×10¹²⁵ (125 digits) (cocoa-adaptive r1, 10.9s; ganak-verified)

**1254v / 2518-cl (sparse, ~2.0 cl/var), band=MID, log2_cost=49.3, sep_ratio=0.025 / sepsize=31 (tiny separator), max_path=45.0, n_leaves=844, arjun substantial=True.** SOLVED by **cocoa-adaptive** in round 1, **10.9s**, count = **12374748797617855764050198121191304103511416559087400449928139456888070478909044293083079786511928252392690782684663014702600** (~1.24×10¹²⁵, 125 digits); `ganak --prob 0` = same (exact 125-digit match ✓, ganak 1.93s). The 107/109/111/113 family pattern — tiny separator + arjun=True → fully decomposable → enormous count computed fast as a product over near-independent components. (cocoa-reactive took a bad branch first, stuck at pct≈0, but cocoa-adaptive's decomposition closed it in 10.9s.)

## mc2026_track1_159 — TIMEOUT, both engines lose (count unknown) — 139/143 deep-wall at 11k-var scale

**11,233v / 50,177-cl (~4.5 cl/var), band=HIGH, log2_cost=240.9, sep_ratio=0.013 / sepsize=219 (tiny separator), max_path=237.0, n_leaves=9041, arjun substantial=False.** **TIMEOUT — neither engine returned a count in the 60-min budget.** The 139/143 arjun=False deep-wall, now at 11k-var scale: all 8 configs walled at closed_bits ~8294–8309 (n_root≈8436, **~128–142 bits unclosed**), frozen flat through rounds 2–3 (vel ~0, closed_bits unchanged window-to-window); leader cocoa-adaptive handed off at ~+15.7 min. ganak then ran its full ~44-min budget — RSS grew to ~7 GB, `[ganak~live]` blank throughout (to active 2642s ≈ the 2658s budget), never a parseable count phase, no count. Clean full-budget timeout (no crash/watchdog).

**Fifth both-lose of the sweep (127/139/141/143/159), third of the arjun=False deep-wall flavor (139/143/159).** The tiny separator (0.013) again gave no traction — the runtime decomposition froze ~128 bits short, reconfirming (as 127 first showed) that a tiny root_sep is no guarantee of decomposability. **Count remains unknown.**

## mc2026_track1_161 — TIMEOUT, both engines lose (count unknown) — arjun=True but ganak's cache USELESS at 51k-var scale

**51,532v / 320,185-cl (~6.2 cl/var; 2nd-largest in the sweep after 141), band=HIGH, log2_cost=1747.7, sep_ratio=0.018 / sepsize=1025 (tiny separator), max_path=1747.0, n_leaves=42157, arjun substantial=True.** **TIMEOUT — neither engine returned a count in the 60-min budget.**

COCOA: the tiny separator gave a *partial* decomposition — the cascade configs (nosep/sep-cascade) burst from loading to closed_bits 31484 (~137 bits unclosed, n_root≈31621) then **stalled** (the burst was a one-time load-jump, not a plateau-escape), while plain/cache-max stuck ~489 bits short. Leader cocoa-nosep-cascade frozen at 31484 (71M decisions, 0 progress), handed off at ~+15.7 min.

**ganak: arjun=True did NOT rescue it (unlike 131) — its component cache was useless.** Ran the full ~44-min budget; RSS ballooned to 18.2 GB but the final leader_sample tells the story: **0 conflicts, 0 cubes, 0 cache entries, cache_miss_rate=1.000** (every component lookup missed), klookup_s≈16.7k. So ganak was enumerating components against a 100%-miss cache (no repeated substructure to exploit) with no CDCL conflict pruning — effectively brute force on 51k vars. No count. Clean full-budget timeout (no crash/watchdog).

**Sixth both-lose (127/139/141/143/159/161), and the SECOND arjun=True both-lose (127, 161).** Confirms again arjun=True is no guarantee: here the post-Arjun residual was too large + structurally uncacheable for ganak at scale — a distinct mechanism from 159's arjun=False deep-wall ("uncacheable scale" vs deep tail). **Monitoring tell:** a populated-but-zero heartbeat (`cache_K=0`, `cache_miss_rate=1.0`, 0 conflicts) while RSS balloons = ganak brute-forcing with a useless cache → reliable timeout signature. **Count remains unknown.**

## mc2026_track1_163 — TIMEOUT, both engines lose (count unknown) — large-sep + arjun=False; ganak preprocessing-bound

**4872v / 51,385-cl (~10.5 cl/var, dense), band=HIGH, log2_cost=3442.3, sep_ratio=0.585 / sepsize=2856 (LARGE separator), max_path=3438.0, n_leaves=30021, arjun substantial=False.** **TIMEOUT — neither engine returned a count in the 60-min budget.**

COCOA: despite the large separator, the cascade configs (nosep/sep-cascade) decomposed to closed_bits 4023 / **pct 52.8% — ~0.92 bits unclosed** (n_root≈4024), then STALLED on the last component (a burst-from-load, no further progress through rounds 2–3 — the 025/121 "stuck on the final bit" pattern). plain/cache-max stuck ~9 bits short. Leader cocoa-nosep-cascade frozen at 52.8%, handed off at ~+14 min.

ganak: ran the full ~44-min budget but spent ALL of it in PREPROCESSING — `[ganak~live]` blank (`cache_K=-`) from start to active 2642s, RSS to 13.3 GB, never reached a parseable counting phase. The dense + large-separator structure was too hard to simplify (cadiback/CMS distill) in 44 min. No count. Clean full-budget timeout.

**Seventh both-lose (127/139/141/143/159/161/163).** The 121 hope (stuck ~1-bit residual ⇒ ganak cracks the small remainder) did NOT pan out — and the differentiator vs 121 is density: 121 was 5259v/**15.7k**-cl (ganak preprocessed fast → solved in 94s), 163 is 4872v/**51.4k**-cl, so ganak's preprocessing never finished. Reinforces the arjun=False + large-separator + band=high both-lose class (029/033/…/163). **Count remains unknown.**

## mc2026_track1_165 — TIMEOUT, both engines lose (count unknown) — arjun=True but ganak preprocessing-bound (again)

**11,731v / 40,805-cl (~3.5 cl/var), band=HIGH, log2_cost=1660.2, sep_ratio=0.158 / sepsize=1860 (moderate separator), max_path=1654.0, n_leaves=6488, arjun substantial=True.** **TIMEOUT — neither engine returned a count in the 60-min budget.**

COCOA: all configs walled — the leading ~20-bit group (plain/cache-max/reactive/unified-sep at closed_bits 9673, n_root≈9693) frozen flat through rounds 1–3; the cascade configs were *far* deeper here (~203 bits, the opposite of 161/163 — branch advantage is instance-specific). Leader cocoa-plain frozen at ~20 bits, handed off at ~+14 min.

ganak: ran the full ~44-min budget entirely in PREPROCESSING — `[ganak~live]` blank (`cache_K=-`) from start to active 2642s, RSS to 13.3 GB, never reached counting. Despite being smaller than 161 (11.7k vs 51k vars) and arjun=True, the simplification (cadiback/CMS distill) didn't finish. No count. Clean full-budget timeout.

**Eighth both-lose (127/139/141/143/159/161/163/165), THIRD arjun=True one (127/161/165), and the FOURTH CONSECUTIVE (159/161/163/165).** arjun=True keeps failing to deliver in this band=high region — ganak's *preprocessing* is the bottleneck, not Arjun specifically (it never even reaches counting). The sweep has hit a hard band=high cluster (159–165 all both-lose). **Count remains unknown.**

## mc2026_track1_167 — SOLVED, count = 10 (cocoa-plain r1, 0.1s; ganak-verified)

**15v / 34-cl (tiny), band=LOW, log2_cost=7.9, sep_ratio=0.2 / sepsize=3, max_path=6.0, n_leaves=7, arjun substantial=False.** SOLVED by **cocoa-plain** in round 1, **0.1s**, count = **10**; `ganak --prob 0` = 10 (match ✓). Trivial 15-var instance — instant for both. Breaks the four-instance both-lose streak (159–165).

## mc2026_track1_169 — ⚠️ SOUNDNESS MISMATCH: cocoa-sep-cascade UNDERCOUNT vs ganak — sweep PAUSED, sep-cascade quarantined

**636v / 1816-cl (sparse, ~2.9 cl/var), band=MID, log2_cost=65.1, sep_ratio=0.030 / sepsize=19 (metis_sep_vars=19, metis_sep_clauses=0 — variables-only separator), max_path=62.0, n_leaves=336, arjun substantial=False.**

**FIRST COUNT MISMATCH OF THE ENTIRE VERIFIED SWEEP (mc2025 + mc2026).** Machine-compared full strings, both engines exited cleanly:
- cocoa-sep-cascade ("solved", monitor phase, 1450.0s, ~7.0M decisions — the longest COCOA grind of mc2026): **2,337,405,402,708,146,114,560**
- `ganak --prob 0` (3040.7s — also its hardest verify of the sweep): **2,337,406,655,942,795,198,464** ← the oracle
- **Δ = 1,253,234,649,083,904 (~1.25×10¹⁵): COCOA UNDERCOUNTED by 5.4×10⁻⁷ of the count (0.000054%).**

**Triage.** An undercount = skipped work — the signature of a **false cache merge** (a component wrongly treated as cache-equal to a previously-solved, smaller-count one). Flag-delta isolation:
- cocoa-plain (`-sep 5 -cb 3`) — verified exact on many instances;
- cocoa-**nosep**-cascade (`-unifiedPicker -decomposeAfterK 1000 -cascadeW 10 -cascadeDepth 9`) — verified exact SIX times (007/039/045/079/145/151);
- cocoa-**sep**-cascade = the **union** of those two flag sets — and **169 was its first-ever ganak-verified count, which failed.** (cocoa-unified-sep, the sep+picker subset without cascade, has never had a verified win either — so the untested surface is {static separator × unifiedPicker/cascade}.)
- The run used **defaults wl_iterations=1** (no `-wlIter` passed; coarsest WL canonicalization) and default hashMode.
- Tiny relative error + first appearance on the instance with the largest cache population is consistent with **one/few rare false merges** (hash or WL-equivalence collision), rather than a systematic key omission (which should have corrupted the six exact nosep-cascade verifies). Owner's hypothesis set: (1) hash collision, (2) clause missing from the hash key, (3) caching logic error. Variables-only separator + no clause branching performed ⇒ the cb/clause-branch bookkeeping is unlikely to be the trigger here.

**Actions:** (1) 169's count recorded as ganak's **2,337,406,655,942,795,198,464** (oracle); NOT credited to COCOA. (2) **cocoa-sep-cascade QUARANTINED** from the racing set pending root cause. (3) Multi-agent read-only code audit launched (key construction / lookup equality / WL canonicalization / cascade-BCP state / separator interaction / count assembly). (4) Sweep paused before 171.

**WAVE-1 PROBES (same day; runlogs/probe169_{1,2,3}\*.log):**
| probe | knob | count | Δ vs ganak |
|---|---|---|---|
| 1 verbatim rerun | — | 2337405402708146114560 (= incident) | −1.25×10¹⁵ |
| 2 `-wlIter 2` | deeper WL | 2337397011807753781248 (3rd value, WORSE) | −9.64×10¹⁵ |
| 3 `-cs 21000` | capacity | = probe 1, byte-identical cache stats | −1.25×10¹⁵ |

Conclusions: the false merge is **(a) fully deterministic** (probe 1 = incident, probe 3 = probe 1 bit-for-bit incl. l2_stores=4429506/l2_hits=1773037); **(b) eviction/capacity-independent** (evictions=0 throughout, 476 MB live); **(c) NOT a WL-coarseness monotone** — deeper canonicalization made it 7.7× WORSE, killing the "wlIter 1 too coarse" story and pointing at hash-trust / poisoned-entry mechanics; **(d) correlated with the sep-cascade-unique mid-separator path**: MIDSEP decomp_attempts/splits = 1/0 → error 1.25e15 (probes 1/3) vs 2/1 → error 9.64e15 (probe 2). Audit interim (map + 6 lenses complete): cache hits trust the 128-bit hash alone (content_cache.h: "no stored clause vectors — relies on hash quality for correctness"; operator== ignores even stored sanity counts); random 128-bit collision ≈ 4e-26 ⇒ systematic mechanism; leading cluster = learned-clause context-poisoned counts cached context-free (sharpSAT's removeAllCachePollutionsOf purge has NO counterpart in ContentCache) + the mid-separator decomposition block (solver_rec.cpp:886–1041, the ONLY cache-relevant path unique to sep-cascade: plain lacks unifiedPicker, nosep-cascade installs no separator) which allegedly recurses without the learned-clause membership filter and whose decomp-site L2 hit skips the 2^free_vars rescale (solver_rec.cpp:1401) and copies the un-rescaled value into L1. Adversarial verification + synthesis pending.

**AUDIT VERDICT (workflow complete: 83 agents, 21 confirmed / 4 refuted findings; ~45 late refuter calls lost to a session-rate-limit — the rank-1 cluster's refuters all completed, zero refutations):**
1. **🥇 Learned-clause cache pollution, purge dropped (NOT sep-cascade-specific).** sharpSAT's `removeAllCachePollutionsOf` existed in COCOA's initial commit (9ff3ea4) and was **explicitly removed in commit e9f8df6** ("pollution management") with no replacement — ContentCache has no invalidation API at all. Mechanism: a globally-learned clause confined to component R prunes R's count during its solve; that is sound only while all siblings are satisfiable — but the store executes BEFORE the sibling-UNSAT short-circuit (solver_rec.cpp:644/1568 vs 1594–1603), so the too-small count persists under a learned-blind key; the CANONICAL key then transports it to isomorphic components in satisfiable contexts (undercount), and the decomp-site hit even back-poisons L1 (1394–1397). The code itself concedes the direction (solver_rec.cpp:1515–19: the 'with learned' count "would be SMALLER"). Exposure was maximal on 169: 618,320 learned clauses, 667,694 conflicts, 1.77M L2 hits. **Affects ALL configs in principle** — sep-cascade's trajectory merely exposed it; every COCOA count must remain ganak-verified until fixed.
2. **🥈 Systematic canonical false merge:** hash-only equality (`operator==` compares 128 hash bits, ignores the stored sanity counts); the only construction hole reachable at clause length 3 is the s_sig==0 zero-sum aliasing (canonical_key.cpp:298–300, ~2⁻⁶⁴/var — unlikely but deterministic).
3. **🥉 Unguarded mid-sep recursion (solver_rec.cpp:1021, no SubVarsetGuard) — real latent hole unique to sep-cascade but EXCLUDED for the incident:** the unguarded path is inside the splits>0 branch and the bit-exact repro shows decomp_splits=0 (it fired 0 times). ⚠️ It DID fire in the wlIter-2 probe (splits=1) and may contribute to that run's different undercount. *(This corrects the earlier "MIDSEP engagement tracks the error" reading — engagement correlates across probes but the incident itself had zero unsound recursions.)*
4. Free-var rescale/floor-division family: cannot initiate (fresh components have free_vars=0); add asserts.
5. Random 128-bit collision: numerically excluded (~4e-26, and two different wrong counts across wlIter settings).

**Instrumentation discovered (the "verify collisions first" answer): COCOA already ships cache-verification tooling** — `-bruteForceCacheCheck N` + `-bruteForceCacheDumpDir` + env `SHARPSAT_VERIFY_STORE/VERIFY_LEARNED/DUMP_UNSOUND` (solver_rec.cpp:1298/1374/1493; solver_diagnostics.cpp:1127–1209). One verbatim Tier-0 rerun discriminates the classes: **HIT-side abort = false merge (rank 2); STORE-side abort = pollution (rank 1)**. Coverage limit: brute checks only components ≤N vars (hit traffic averages 98.5 vars), hence a proposed ~35-line Tier-1 patch (peek-time sanity-count compare + independent non-additive digest) for full coverage. Decisive falsifier for rank 1: verbatim + `-learnLevel 0` — if the count snaps to ganak's, pollution confirmed. *This is why every count gets a ganak verification — a fast plausible answer is not a correct one.*
