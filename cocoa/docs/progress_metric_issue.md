# Progress metric — issue and the correct accounting

## Model

The DPLL search walks a binary tree. The tree has 2^n_root leaves
where n_root = active variables at the root component after
preprocessing. The solver visits nodes via DFS, descending and
backtracking. Cache, BCP, base cases are implementation details of
*how* a subtree is processed; the progress accounting must be
independent of all of them.

## The single, correct accounting rule

> **Every time we backtrack from a fully-processed subtree of k active
> variables, the progress counter gains 2^k.**

That's it. Nothing else contributes. The act of backtracking IS the
signal that a subtree's contribution to the total count has been
determined — the means by which that determination happened
(further recursion, base case, cache lookup, conflict) are irrelevant
to the progress measurement.

### Properties of this rule

- **Monotone by construction.** Backtracks only add. The counter is
  non-decreasing.
- **Exact conservation.** When the search finishes, the sum equals
  exactly 2^n_root. No over-count, no under-count.
- **Process-independent.** Whether a subtree was solved by 100k
  decisions or by a single cache lookup, the same backtrack event
  fires and contributes the same 2^k.

### Display

- `closed_log_sum` = log_sum_exp over every backtrack event of (subtree
  active-var count at the time of entry to that subtree).
  Maintained incrementally via the standard log-sum-exp recurrence.
- `progress_bits` = n_root − log2(2^n_root − 2^closed_log_sum), the
  bits of remaining-worst-case work shaved off.
- `fraction_processed` = 2^(closed_log_sum − n_root), clamped to 1.
  Hits 100% exactly when the search finishes.

## Current implementation: wrong

The current code (`Solver::noteCachedSubtree` in `solver.h`,
hooked at the cache STORE sites in `solver_rec.cpp`) accounts on
**cache store events**, not on backtrack events. This is wrong for
two reasons:

1. **Conflates accounting with caching.** A subtree that gets cached
   AND a subtree that doesn't get cached should contribute identically
   to progress. The current code credits only the former.

2. **Double-counts via nesting.** A cache store at a node N happens
   *after* N's recursion completed — but the recursion's children
   also produced cache stores. Counting all of them adds nested
   subtree sizes (children's leaves are included in N's leaves), so
   the cumulative over-counts. Empirically: on t1_041 with
   react-agg+wlIter2, `closed_log_sum` reaches ~1030.24 at finish
   when n_root = 1030 — the 0.24-bit overshoot is the nesting
   over-count.

## Correct implementation

Add a single hook at the moment a `solveComponent(comp, ...)` call
returns (just before the `return` statement), counting `2^k` where
`k = comp.num_variables()` (the subtree's size at the time we entered
it — captured once at entry, used at exit).

This is the only call site. Cache stores stop contributing. Cache
hits don't contribute either (a hit returns the cached count without
us having descended into the subtree, so no backtrack from inside it
ever fires — but we DO backtrack out of `solveComponent` immediately,
which IS the backtrack event for the hit case and IS counted by this
single rule).

The hook also fires correctly for:
- **Base cases** (BCP closes the formula): single return path counts.
- **Conflicts** (UNSAT branch): same return path counts.
- **Branching computations**: counts ONCE on the final return — the
  recursive children's returns also count, but each child's subtree
  is a different node (different variable set), so no double-counting.

### Sketch of the fix

```cpp
// In solveComponent (top of function):
unsigned k_at_entry = comp.num_variables();

// ... existing body (cache check, recurse, etc.) ...

// At EVERY return path:
noteBacktrackedSubtree(k_at_entry);
return result;
```

A small RAII wrapper makes this exception-/early-return-safe.

Remove all calls to `noteCachedSubtree` at cache store sites. Cache
no longer enters into the accounting at all.

## Test

After the fix, run with `SHARPSAT_PROGRESS=1 SHARPSAT_PROGRESS_INTERVAL=0.05`
on `mc2025_track1_041.cnf` using `react-agg + wlIter 2`. Expected:

- `closed_log_sum` rises monotonically from 0 toward exactly
  n_root = 1030 (no overshoot).
- `progress_bits` rises monotonically from 0 toward n_root.
- At the moment the solver finishes, `closed_log_sum = 1030.0` and
  `fraction_processed = 100%` exactly.
- No drops anywhere in the trajectory.
