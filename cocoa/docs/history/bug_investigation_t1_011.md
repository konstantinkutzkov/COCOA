# t1_011 undercount bug — investigation status

**Date**: 2026-04-22.
**Solver**: custom #SAT solver at `/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/sharpsat-separator` (fork of sharpSAT with separator branching, clause branching, guard-variable scoped learning, canonical-key content caching).

## Observed bug

Our solver produces an undercount on MC2025 instance **t1_011** (`/tmp/t1_011.cnf`, 6559 vars, 14515 clauses).

- Ground truth: **536,870,912,306** (ganak, verified in 113 s).
- Our solver on `/tmp/t1_011.cnf`: **534,590,063,026** (wrong; ~0.425% undercount).
- Our solver on `/tmp/t1_011_rev.cnf` (original with clauses in REVERSED line order): **530,339,778,483** (different wrong).
- Our solver on `/tmp/t1_011_pp.cnf` (a post-preprocess DIMACS dump produced by our own solver): **536,870,912,306** (correct).

**All wrong answers are undercounts.** The magnitude of the undercount depends on clause line-order in the input file.

## The bug is order-dependent, but in a specific way

We added `-sortClausePool` — a CLI flag that, after preprocessing and before search, rewrites `literal_pool_` with original clauses in canonical (lexicographically-sorted) order and rebuilds watch lists + occurrence lists against the new ClauseOfs values.

- `-sortClausePool` ALONE on `/tmp/t1_011.cnf`: **536,870,912,306** (CORRECT).
- `-sortClausePool` ALONE on the preprocessed-but-clause-shuffled `/tmp/t1_011_pp_shuf23.cnf`: **536,870,912,306** (CORRECT).

So the ENTIRE bug can be eliminated by canonicalizing `literal_pool_`'s clause order. Sorting binary_links_ / watch_list_ / occurrence_list_ alone does not suffice in general.

**This strongly implicates "clause-order-in-literal_pool_" as the order dimension the bug is sensitive to.** Something downstream of the pool's layout is consuming it in a way that breaks soundness.

## What clause-order-in-pool affects

The literal_pool_ holds all original long clauses consecutively, separated by `SENTINEL_LIT`. Each clause has a ClauseOfs (its byte offset in the pool). The pool's layout propagates into:

1. **ClauseID assignment**. `AltComponentAnalyzer::initialize()` walks the pool and assigns IDs 1, 2, 3, … to clauses in pool iteration order. So two pool orders → different clause→id mappings. ClauseIDs are stored in Components and used as array indices in the component archetype (`seen_[clause_id]`).

2. **Occurrence list build order**. Each var gets an `occ_long_clauses[v]` block; clauses are appended in pool iteration order.

3. **Unified variable links pool**. `unified_variable_links_lists_pool_` concatenates per-var binary, ternary, and long-clause blocks; within each var's block, clauses are in pool iteration order.

4. **BCP watch list initial population**. Watches are placed on positions 0 and 1 of each clause by `compactClauses()`; the ORDER in which clauses receive watches is pool order, so `watch_list_[lit]` has ofs values in pool order.

5. **Occurrence lists**. `occurrence_lists_[lit]` entries added in pool order per lit.

## Hypotheses ruled out

Each of these was experimentally tested:

- **Unsound clause learning.** Disabling clause learning (`-noLearn`) and failed-literal learning (`-noIBCP`) together: still wrong (same 530,339,778,483 on reversed file, same 534,590,063,026 on original). The bug reproduces with ALL learning disabled.
- **Component caching bug / key collision.** `-verify_cache` never fires (every cache hit recomputes to the same value). Disabling caching (`-noCC`) is extremely slow but does not change the buggy answer in tests done.
- **Separator or clause branching.** Plain mode (no `-sep`, no `-cb`) reproduces the bug identically.
- **Missing factor of 2 from component decomposition.** Added a runtime guard: `#active_vars(super) == Σ #active_vars(sub_i) + #isolated_peeled`. Never fires. Every variable is accounted for at decomposition time.
- **Preprocessing state residue.** Added `verifyPostPreprocessCleanSlate`. Passes on both correct and wrong runs.
- **Watch-list invariant broken for learned clauses.** Real, but fixing it does not change the count. Documented as efficiency defect only.

## Hypotheses not yet ruled out

- **ClauseID-based indexing corruption.** Since ClauseIDs are assigned in pool order, any code that depends on specific ID values for correctness (not just identity) would become order-sensitive.
- **Canonical key inconsistency.** The canonical key is supposed to be invariant over pool order by design (it sorts clause content). But the key's computation uses `clause_id_to_ofs` lookups; if an intermediate step indexes wrong, the final hash could differ in a way we haven't observed.
- **Some other indirect dependency** in the recursive search (solver_rec.cpp) that we haven't identified.

## What would definitively pinpoint the bug

Run the solver twice — once on `/tmp/t1_011.cnf` (wrong baseline), once with `-sortClausePool` (correct). At a corresponding checkpoint (e.g., entry of every `solveComponent` call, identified by canonical-key hash), compare:
- The set of active variables in the sub-component.
- The set of active clauses.
- Our solver's returned count.

The first point where the same sub-formula produces different counts in the two runs identifies the bug's manifestation.

## Task for a fresh agent

**Objective**: identify the single code location where different `literal_pool_` clause orderings cause soundness divergence.

**Input to the agent**:
- The source tree at `/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/sharpsat-separator/src/`.
- This document as context.
- The key files to focus on: `solver_rec.cpp` (recursive #SAT), `canonical_key.cpp` (cache key), `alt_component_analyzer.{h,cpp}` (component discovery, clause-id assignment), `content_cache.h` (cache).

**Concrete task**:
1. Trace every place in the source where the ORDER of clauses in `literal_pool_` (not just their content) could matter. Specifically:
   - Where are ClauseIDs used as array indices or lookup keys? Could a structure indexed by ClauseID ever be populated in one order and accessed expecting a different order?
   - Does `canonical_key.cpp::buildCanonicalKey` or its caller use `clause_id_to_ofs[*it]` in a way that depends on ID vs content?
   - Does `AltComponentAnalyzer::recordComponentOf` traverse structures (unified_variable_links_lists_pool_) in an order that could differ when pool layout differs?
2. For each suspect location, identify whether the output (count, component membership, or intermediate value) could legitimately differ under pool-order permutation while the formula is unchanged.
3. Report findings with specific line references and reasoning.

**Concrete starting experiment for the agent**:
Build the solver at `build/` directory (`cd build && cmake --build .`) and run both commands:

```
./build/sharpSAT /tmp/t1_011.cnf              # gives 534,590,063,026 (wrong)
./build/sharpSAT -sortClausePool /tmp/t1_011.cnf    # gives 536,870,912,306 (correct)
```

The `-sortClausePool` flag is implemented in `src/solver.cpp` as `Solver::sortClausePoolOrder()`. The delta between these two runs IS the bug. The wrong run reads `literal_pool_` in input-file clause order; the correct run reads it in sorted order. Everything else is identical.

**Expected output from agent**: a specific file:line citation and explanation of the code path whose behavior legitimately changes under pool-order permutation AND affects the final count.

**Expected time to answer**: 1-2 hours of directed code reading with fresh eyes. This has been investigated for ~12 hours without pinpointing the root cause; fresh perspective would help.
