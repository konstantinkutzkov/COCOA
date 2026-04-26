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
