# Safety analysis: passing precomputed canonical_key to solveComponent

**Goal.** Add an optional `const CanonicalKey *precomputed_key = nullptr`
parameter to `Solver::solveComponent`. When non-null, skip the redundant
`buildCanonicalKey` + `peek` at entry (the caller already did them and
got a miss).

**Why.** The decompose loop in `solveComponentImpl` does `buildCanonicalKey
+ peek` per sub-component (lines 1162, 1173 in `solver_rec.cpp`). On
miss, it recurses, and the recursion's entry (line 375, 405) repeats
the same work for the same component. ~17M duplicate calls per 60s
window on t1_049, ~780 ns saved per case → potentially ~13s in 60s.

**This document tracks the analytical safety check, not the implementation.**

## Invariants the entry-check must preserve

Before merging, the precomputed-key path must produce the same observable
state as the recompute path. Specifically:

1. `cached_key` (the local variable): same value.
2. `key_built` (the local boolean): same value (true).
3. `free_vars` (the local variable): same value.
4. `stats_misses` (cache counter): incremented exactly once per miss
   (line 421 currently).
5. `CANON_DIAG distinct_keys` set (lines 386-388): caller already
   inserted, recursion shouldn't double-insert.
6. The L2-hit branch (lines 407-419) MUST be unreachable when caller
   gave us a precomputed key — caller already established miss.

## Are these invariants preserved by the proposed change?

### Invariant 1: `cached_key` correctness

`buildCanonicalKey` is a pure function of `(comp, literal_pool_, literals_,
literal_values_, clauseIdToOfs, removed_clauses_, original_lit_pool_size_,
wl_iterations, static_wl_labels_)`.

Between the caller's build at `solveComponentImpl:1162` and the
recursion's entry at `solveComponent:375`, are any of these inputs
modified?

- `literal_pool_`, `literals_`, `literal_values_`, `clauseIdToOfs`,
  `static_wl_labels_`: none mutated between sites.
- `removed_clauses_`: not mutated by the intermediate code
  (`SubVarsetGuard` at 1226-1280 touches `current_sub_varset_` /
  `current_sub_var_list_` only).
- `original_lit_pool_size_`, `wl_iterations`: constants for the run.

Therefore `*precomputed_key == buildCanonicalKey(...)` bit-for-bit
identically. **Invariant 1: ✓.**

### Invariant 2: `key_built = true`

Trivially preserved by setting it in the precomputed path. **✓.**

### Invariant 3: `free_vars` correctness

Computed via `(cached_key.num_vars > cached_key.n_in_clauses) ?
(diff) : 0`. Identical formula in both branches. **✓.**

### Invariant 4: `stats_misses` incremented exactly once

Caller's `peek` at `solveComponentImpl:1173` does NOT bump stats
(neither `stats_hits` nor `stats_misses`). Currently the recursion's
miss increments `stats_misses` at `solveComponent:421`.

Proposed precomputed-key path must still bump `stats_misses` to
preserve count parity. **Will explicitly bump in the precomputed
branch. ✓.**

### Invariant 5: CANON_DIAG counters

`distinct_keys` + `total_calls` are `static` inside `solveComponent`,
tracking unique canonical_keys observed at solveComponent entries.

In the precomputed path the entry "observes" the same key the caller
built (the caller is in `solveComponentImpl`, which does NOT touch
this static set). If we skip the insert in the precomputed path,
the static set under-counts compared to the rebuild path.

However: **CANON_DIAG fires only with `-log_branches` enabled**
(diagnostic, default off). The undercount is acceptable for a
diagnostic, and the ratio `1 - distinct_keys.size() / total_calls`
remains interpretable (both are undercounted symmetrically — the
precomputed-key path doesn't insert and doesn't increment). **✓ as
diagnostic; document the behavior.**

### Invariant 6: L2-hit branch unreachable in precomputed-key path

The caller (line 1173) called `peek` and got `false`. Cache state
between that point and the recursion entry is unchanged (single-
threaded, no intervening cache writes). Therefore re-peeking would
return false. Safely skipped. **✓.**

## Call-site survey (all callers of `solveComponent`)

Five sites:

| Line | Path | Precomputed key available? | Pass |
|---|---|---|---|
| `solver_rec.cpp:304` | root post-root-decompose loop | No | `nullptr` |
| `solver_rec.cpp:854` | mid-separator decompose recursion | No | `nullptr` |
| **`solver_rec.cpp:1282`** | **post-consumption decompose loop** | **YES (built at 1162)** | **`&key`** |
| `solver_rec.cpp:2105` | branchOnLiteral recursion (after BCP) | No (BCP changed state) | `nullptr` |
| `solver_rec.cpp:2341` | branchOnClause recursion (after BCP) | No (BCP changed state) | `nullptr` |

Only line 1282 takes the precomputed-key fast path. The other four
default to `nullptr` and behave exactly as today.

## Concerns / known issues (orthogonal to this change)

While auditing, I noticed a **pre-existing** subtle issue worth flagging
but **NOT introduced by this change**:

**Double-store at line 1357-1361 vs solveComponent:480.**

When the sub-level L2-miss path runs the recursion (line 1282), the
recursion's own `solveComponent:480` stores `(cached_key, structural)`
where `structural = result / 2^free_vars`. Then on return, the caller's
`solveComponentImpl:1358` stores `(key, sub_count)` where
`sub_count = result` (the actual count with free_vars factor).

For sub-components with `free_vars > 0` (sub has X_TRI vars that
appear in no in-scope canonical clause — rare but possible if a var's
only clauses are all satisfied), these two stores write DIFFERENT
values to the SAME key. The caller's store happens last → cache
ends up holding the scaled (not structural) value, which would be
double-scaled on retrieval (entry-hit branch line 414 scales again).

**Whether this manifests depends on whether decomposition ever
produces a sub-component with `free_vars > 0`.** From the canonical_key
code, `free_vars = num_vars - n_in_clauses` where `n_in_clauses`
counts X_TRI vars appearing in surviving (non-satisfied, ≥2-active)
clauses. A var with all its clauses satisfied or under-active is
counted in `num_vars` (because s_active_vars includes all X_TRI vars
from comp.varsBegin) but NOT in `n_in_clauses`.

Result: bug latent. The proposed change does NOT introduce or worsen
this; flag separately for future audit. Test runs of `t1_049`,
`t1_011`, etc. have produced correct counts (confirmed against ganak),
suggesting this case is either not arising in practice or being
canceled out somewhere.

## Conclusion

The change is **analytically safe** under the verified invariants.

Implementation impact:
- Add optional parameter `const CanonicalKey *precomputed_key = nullptr`
  to `Solver::solveComponent`.
- In the entry's `if (can_cache)` block, branch on `precomputed_key`:
  null → existing code (rebuild + peek + maybe hit-return).
  non-null → set `cached_key`, `key_built`, `free_vars`, bump
  `stats_misses`, fall through.
- At call site `solver_rec.cpp:1282`, pass `&key`.

Total lines: ~20-25 (parameter, branch, comment, call-site update).

