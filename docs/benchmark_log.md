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
| **t1_041** | **1920** | **0.99** | **TIMEOUT > 60 s** | **TIMEOUT > 60 s** | **16 s** | **plain** | **both ours fail; ganak class** |
| t1_047 | 80 | 3.00 | TIMEOUT > 60 s | TIMEOUT > 60 s | TIMEOUT > 60 s | plain | inconclusive at 60 s |

All non-timeout sharpSAT runs produce the count that ganak's `c s exact arb int` line confirms:
- t1_025 → `134746112245856`
- t1_027 → `1115259056499565`
- t1_041 → `5516767…943259892051805732864` (long; matches ganak's exact arbitrary-int line precisely)

### What the rule predicted vs what we measured

| Quadrant | Predicted | Result | Notes |
|---|---|---|---|
| small + low-density (Q10 falsifier candidates) | legacy | **legacy wins on t1_027; ties on t1_025; inconclusive on t1_023** | No falsifier found. Rule holds where both finish. |
| large + low-density (previously untested) | plain | **both fail; ganak finishes** | Rule moot; new class identified |
| small + high-density | plain | inconclusive (all timeout on t1_047 at 60 s) | Need longer budget or different test instance |

### New finding: a third "ganak-class" quadrant

`t1_041` is the first measured instance in the **(large, low-density, mostly long-clauses)** quadrant. Both our configs time out at 60 s; ganak finishes in 16 s. Combined with the t1_021 family observations from 2026-04-27 §4 of `portfolio_insights.md`, the pattern across **t1_021/t1_023/t1_025/t1_027/t1_041** — all pure-or-near-pure 3-SAT with density ~1.0 — points to a structural class where our hierarchy-based search loses to ganak's tree-decomposition-driven DP, regardless of `-sepVarBias` / plain choice. The 2-feature `(density, n_vars)` rule **does not address this class** because it only predicts which of our two configs wins, not whether our solver is the right tool at all.

Detection signature for this class is cheap: `density ∈ [0.95, 1.10]` AND `binary_fraction ≤ 0.1`. On the available instances:
- t1_021 (full, 90 v / 90 c, density 1.00, pure 3-SAT) — ganak class
- t1_023 (102 v / 102 c, density 1.00, pure 3-SAT) — ganak class
- t1_025 (63 v / 66 c, density 1.05, pure 3-SAT) — borderline; sharpSAT solves at 7 s, ganak at 2 s
- t1_027 (66 v / 66 c, density 1.00, pure 3-SAT) — borderline; sharpSAT solves at 5 s, ganak at 3 s
- t1_041 (1920 v / 1910 c, density 0.99, mixed but mostly long) — ganak class (we time out)

Implication for the portfolio: extend the analyzer with a "ganak-class" detection (density ~1.0 + low binary fraction). When detected, the portfolio driver should attempt ganak first or fall through quickly. See updated portfolio_insights §4 / §5 / §8.

### Open questions

- **t1_023 and t1_047** specifically — both small instances that all three solvers timed out on at 60 s. Need a longer budget (300 s+) to characterize. Their structural placement is interesting: t1_023 looks like a harder t1_027 (102 v vs 66 v, same density); t1_047 is small but dense (n=80, density 3.0) — predicted plain but plain timed out.
- **t1_041 with our solver + longer budget**: does plain eventually finish, or is this fundamentally unreachable for us? Worth a 300 s probe.

---

## 2026-05-12 — t1_041 first-branch anchor study + min-rate probe metric

Investigation of why our solver TIMEOUTs on t1_041 (1920 v / 1910 c, ganak-class per 2026-05-11) but solves it in ~2 s when a specific variable is fixed as a first branching decision. Builds on the 2026-04-29 picker session work.

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

For ganak-class instances where our solver normally TIMEOUTs, a **probe-based picker with min-aggregation** identifies multiple good first-branch anchors, not just one "magic" variable. The picker would:

1. Compute literal-graph WL once (one O(n+m) pass × k iterations).
2. For each flip-symmetric variable (~half of all vars on this instance), run a 1 s solver probe at both polarities.
3. Rank by `min(rate_F, rate_T)`; pick the highest.

Cost on t1_041: 401 flip-sym vars × 2 polarities × 1 s ≈ 800 s — too expensive as a fixed preamble. But:
- Restricting to top-N by degree + low-degree-flip-sym (the 120-var sweep used here) covers it in ~8 minutes
- Or: probe just a few dozen flip-sym vars, since results suggest the top of the distribution is large — many vars have min_rate ≥ 0.04 in the top-10 / top-20

This is a **research finding, not a portfolio config yet**. No prediction on whether this picker structure transfers to other ganak-class instances; needs validation on t1_021 family, t1_023, t1_027.

### Open questions specific to this study

- Does v450's tie with v242 reproduce on a fresh process (or is it a single-run quirk)?
- Do the family of fast-anchor variables share any structural property beyond flip-symmetry that could be detected cheaply?
- Min-rate is non-monotone with total solve time (v5 / v459 rank in top 10 but have ~60 s polarities). What additional feature separates them? Hypothesis: max(rate_F, rate_T)/min(rate_F, rate_T) ratio — high ratio signals lopsidedness even when min is moderate.


