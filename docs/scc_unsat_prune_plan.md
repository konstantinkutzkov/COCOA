# Plan: Polynomial-time UNSAT pruning via 2-SAT SCC on the binary-implication graph

**Status:** Draft plan. Do not implement without per-step measurement.

## Goal

Add a polynomial-time UNSAT detector that runs *proactively* at decision
nodes (between BCP and the recursive `solveComponent` call). If the
binary-implication graph induced by the residual formula has any
variable `x` such that `x` and `¬x` lie in the same strongly-connected
component, the residual formula is unsatisfiable — close the branch
immediately and credit its full abstract budget.

This is the **Aspvall–Plass–Tarjan (1979) 2-SAT satisfiability test**
applied to the 2-CNF subset of the residual formula. Sound for UNSAT
detection on the full formula (long clauses can only *strengthen*
unsatisfiability), incomplete but useful.

## Motivation

The t1_105 (13-clue Sudoku) measurements (2026-05-26 session) show:
- Our solver gets stuck after closing 525–607 bits out of 612 within
  the first minute.
- The remaining 4 % – 14 % of the abstract tree is then frozen for
  tens of minutes.
- ganak solves the same instance in ~30 min using its TD-derived
  variable ordering + identity-based cache.

Sudoku-like instances have a *binary-heavy* structure:
- t1_105: 2 916 binary clauses out of 3 172 total (92 %).
- After branching on a few of the 243 length-9 "at-least-one" clauses,
  the residual is essentially pure 2-CNF (the at-most-one binaries).

BCP catches conflicts that arise from *forward* unit propagation, but
the residual 2-CNF can be UNSAT even when no single unit propagation
step from the current trail produces a conflict — specifically, when
the implication graph contains a chain `x → … → ¬x` AND `¬x → … → x`
without those chains being triggered by any currently-assigned literal.

SCC over the implication graph detects this directly in O(V + E).

## Algorithm

For the implication graph G of the residual binary-clause set:

1. Build G:
   - Vertices: the 2 × |active vars| literals.
   - For each active binary clause `(a ∨ b)`: add directed edges
     `¬a → b` and `¬b → a`. Unit clauses `(a)`: add `¬a → a`.
2. Compute SCCs via Tarjan's or Kosaraju's algorithm.
3. For each variable `x`: if `SCC(x) == SCC(¬x)`, the residual is UNSAT.

Cost: O(V + E) = O(active_vars + active_binaries). On t1_105 post-
preprocess: ~6 K operations per call. Microseconds.

## Integration points

### Where to call SCC-prune

In `solveComponent` / `branchOnLiteral` / `branchOnClause`, after BCP
succeeds but before the recursive call. Pseudocode:

```cpp
bool branchOnLiteral(LiteralID lit, ...) {
    setLiteralIfFree(lit);
    if (!BCP(...)) {
        // existing conflict handling
        return ...;
    }
    if (config_.use_scc_unsat_prune
        && residualIs2CnfDominated()
        && sccDetectsUnsat()) {
        // Branch is unsatisfiable. Close it.
        noteResolved(abstract_budget);
        // (optional) generate a learned clause from the SCC cycle
        // — see "Learned clauses from SCC" below.
        // Backtrack normally.
        return 0;  // count of this branch
    }
    // existing recursive call
    return solveComponent(...);
}
```

### Throttle

Don't run the SCC check at every node — most decisions have BCP
catching the conflict already. Reasonable throttles:

- Only when **|active long clauses| / |active clauses| < 0.05** (the
  formula is ≥95 % binary).
- Or, only at every Nth decision (some `--sccPruneEveryN` config).
- Or, only when BCP just made many propagations (suggesting the
  formula is in flux and might have new SCC cycles).

The right throttle depends on the per-instance cost/benefit ratio.
Start with: "every decision when the residual formula's long-clause
count is below a fixed threshold".

### Learned clauses from SCC

When SCC detects `x` and `¬x` in the same SCC, there exist paths
`x → ℓ₁ → … → ¬x` and `¬x → m₁ → … → x` in the implication graph.
Each edge `a → b` in the graph corresponds to a binary clause
`(¬a ∨ b)`. Resolving along the path yields a learned clause that
records the cycle's conclusion. This is analogous to 1-UIP learning.

In the first cut, we can **skip clause learning** — the SCC check
just detects UNSAT and closes the branch. Learning makes future SCC
detection faster but is optional for soundness.

## Soundness argument

Claim: if the binary-implication-graph SCC test reports UNSAT on the
residual formula, the full formula (including long clauses) is also
UNSAT.

Proof sketch: An UNSAT certificate from SCC is a cycle of binary
implications producing `x ⇒ ¬x ∧ ¬x ⇒ x`. This cycle uses only
binary clauses. Removing any subset of clauses (e.g., the long ones)
cannot make this UNSAT certificate invalid — the binary clauses
involved are still present. So the binary subset alone is UNSAT,
hence the full formula is UNSAT. ✓

## What this does NOT solve

- **It does not help with SAT branches.** Schöning's random walk
  could give SAT witnesses but doesn't help #SAT counting (we still
  need to enumerate all solutions). SCC-prune is purely a UNSAT
  shortcut.
- **It misses UNSAT branches that require long-clause reasoning.** If
  the residual 2-CNF is satisfiable but the long clauses make the
  full formula UNSAT, SCC-prune won't catch it. BCP-on-decision
  remains the workhorse for those cases.
- **It does not change the asymptotic complexity of #SAT counting.**
  #2-SAT is itself #P-complete (Valiant 1979). The pruning is a
  constant-factor improvement on the search tree size.

## Implementation steps

1. **Static SCC implementation** (~100 lines): Tarjan's algorithm
   parameterized on a graph view. Standalone function in a new
   file `src/scc_unsat.cpp` / `.h`. Unit-tested independently.
2. **Graph view from solver state**: function `buildImplicationGraph(
   active_vars, binary_clauses)` returning a CSR-style adjacency.
   Lives in `instance.h` as a helper.
3. **Decision-time SCC call**: wire into `branchOnLiteral` /
   `branchOnClause` / `branchOnLiteral_negate_arm`. Gated by a
   `config_.use_scc_unsat_prune` flag (default off).
4. **Throttle**: simple counter / ratio test gating the call.
5. **Optional: learned-clause synthesis** from SCC cycle.

## Test plan

For each step:

| Test | Pass criterion |
|---|---|
| Unit test SCC | Tarjan-correct on 5–10 hand-crafted graphs |
| t1_065 count | `37778931862957161709568` |
| t1_011 default count | `536870912306` |
| t1_011 + `-derivCacheBias 1` count | `536870912306` (UIP canary) |
| t1_049 60 s decision throughput | ≥ baseline, no >2 % drop |
| t1_049 full run count | `8695763196077742` |
| t1_049 full run wall time | ≤ 321.6 s (current baseline) + 5 % |
| t1_105 1 h with SCC-prune on | progress_bits should exceed our current ≈0.07 / 612 |

Commit only if all pass.

## Per-step risk and mitigation

| Step | Risk | Mitigation |
|---|---|---|
| SCC implementation | Tarjan bug → wrong UNSAT or missed UNSAT | Unit tests on small hand-crafted graphs (cycles, no cycles, isolated SCCs) before any integration |
| Graph-build | Incorrect edges → wrong UNSAT verdict | Test with `SHARPSAT_VERIFY_SCC_AGAINST_CMS` env-gated flag that runs an external SAT solver on the same residual formula and compares |
| Decision-node call | Soundness violation if `noteResolved(budget)` is wrong | Verify against full t1_065 / t1_011 counts |
| Throttle | Always-on hurts perf | Default off; benchmark and tune |
| Learned clauses | Wrong learned clauses break soundness | Skip in first cut |

## Estimated effort

- SCC implementation + unit tests: ~3 hours
- Graph-build wrapper: ~1 hour
- Decision-time integration + throttle: ~2 hours
- Test pass through standard regression suite: ~1 hour
- Shadow verifier (`SHARPSAT_VERIFY_SCC_*`): ~1 hour optional
- **Total:** ~7–8 hours of focused work.

If after Phase 1 (SCC-prune on) the t1_105 run still doesn't visibly
progress, the diagnosis is that t1_105's UNSAT branches don't arise
from pure 2-CNF cycles — and we'd need a different pruning mechanism
(e.g., SAT-solver call at every decision node, more expensive but
catches long-clause-induced UNSAT too).

## Out of scope for this plan

- **Schöning's random walk** as a SAT-witness finder. Discussed
  separately; useful for finding SAT branches quickly but doesn't
  help our UNSAT pruning need.
- **Incremental SCC maintenance.** Static batch SCC is O(V + E) and
  fast enough for our graph sizes. Incremental algorithms (e.g.,
  Bender et al.) are complex; defer unless static profiling shows
  the SCC call dominating.
- **TD-based variable ordering** (the other potential ganak parity
  feature). Larger architectural addition; out of scope here.
- **Learned-clause synthesis from SCC cycles.** Sound implementation
  is non-trivial; defer to Phase 2 if the basic UNSAT-prune is
  measured to help.
