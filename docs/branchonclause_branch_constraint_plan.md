# Branch-constraint antecedent implementation plan

**Goal.** Replace the current per-decision-DL fix in `branchOnClause`
(commit `2beffd8`) with a "branch-constraint" marker on ¬l_2, ..., ¬l_k
that lives at the same DL as ¬l_1 (the real decision). This recovers
the pre-fix cost profile (~+25% on t1_049 expected to vanish) while
preserving UIP soundness via marked dispatch.

**Approach.** Option D — per-`Variable` flag (`bool is_branch_constraint`)
orthogonal to `Antecedent`. See [[project_branch_constraint_antecedent]]
for the high-level design and soundness argument.

---

## 1. Audit of antecedent-reader sites

For each existing call site that inspects an antecedent or uses
`hasAntecedent`, the table below specifies what changes (if any) are
needed for branch-constraint correctness.

The PRIMARY semantic for a branch-constraint var `v`:

- `var(v).ante == Antecedent(NOT_A_CLAUSE)` (no real backing clause)
- `var(v).is_branch_constraint == true`
- Logically: **NOT a decision** (UIP should not break on it),
  **NOT a real propagation** (no clause to resolve through),
  but a **structural constraint imposed by the ¬C branch** that must
  appear in any learned clause derived through it.

### 1a. `hasAntecedent` semantics

Change the definition at [`instance.h:63-65`](../src/instance.h#L63-L65):

```cpp
bool hasAntecedent(LiteralID lit) {
    return variables_[lit.var()].ante.isAnt()
        || variables_[lit.var()].is_branch_constraint;
}
```

This ensures every existing `if (!hasAntecedent(curr_lit)) break;`
site in UIP loops does NOT take the decision-break path on a
branch-constraint var. The break still fires correctly on the one
real decision ¬l_1.

### 1b. UIP loop dispatch sites — SOUNDNESS-CRITICAL

**Both `recordLastUIPCauses` and `recordAllUIPCauses` need the same
three-way dispatch.** After `seen[curr_lit.var()] = false` and the
`lits_at_current_dl--` decrement, BEFORE the existing
`if (isAClause()) {...} else { binary }` dispatch:

```cpp
if (var(curr_lit).is_branch_constraint) {
    // Don't resolve through anything. Preserve curr_lit's negation
    // in the learned clause. If curr_lit's DL < conflict DL, it's
    // already in tmp_clause via the seen-add path. If curr_lit's DL
    // == conflict DL (typical for branch-constraint at branchOnClause
    // DL), the lit was processed via lits_at_current_dl, removed from
    // seen, but NOT yet added to tmp_clause. Add it now.
    tmp_clause.push_back(curr_lit.neg());
    curr_lit = NOT_A_LIT;  // matches the existing end-of-iter sentinel
    continue;
}
```

| Site | File:Line | Action |
|---|---|---|
| `recordLastUIPCauses` dispatch | [`solver.cpp:2097`](../src/solver.cpp#L2097) | Insert branch-constraint case BEFORE `isAClause()` dispatch |
| `recordLastUIPCauses` break check | [`solver.cpp:2087`](../src/solver.cpp#L2087) | No change (hasAntecedent's new def handles it) |
| `recordLastUIPCauses` final emit | [`solver.cpp:2178`](../src/solver.cpp#L2178) | No change (¬l_1 break path still gives a real curr_lit) |
| `recordAllUIPCauses` dispatch | [`solver.cpp:2237`](../src/solver.cpp#L2237) | Insert branch-constraint case BEFORE `isAClause()` dispatch |
| `recordAllUIPCauses` break check | [`solver.cpp:2225`](../src/solver.cpp#L2225) | No change |
| `recordAllUIPCauses` second emit | [`solver.cpp:2267-2269`](../src/solver.cpp#L2267-L2269) | No change |

### 1c. Clause-minimization (`minimizeAndStoreUIPClause` helper)

The minimization at [`solver.cpp:1867-1881`](../src/solver.cpp#L1867-L1881)
walks a candidate's antecedent to check if all its other lits are in
the current minimized clause. For a branch-constraint var, there's no
real antecedent, so:

```cpp
auto ante_all_in_clause = [&](LiteralID lit) -> bool {
    if (!hasAntecedent(lit)) return false;
    if (var(lit).is_branch_constraint) return false;  // NEW
    if (getAntecedent(lit).isAClause()) { ... }
    else { ... }
};
```

Returning `false` here means the lit is NEVER eligible to be minimized
away — correct behavior, since the branch-constraint literal MUST
appear in the learned clause.

### 1d. Replay / drop-log audit ([`solver.cpp:1989-1997`](../src/solver.cpp#L1989-L1997))

Need to read this site carefully and add the equivalent guard.

### 1e. `setLiteralIfFree` ([`solver.h:944-1011`](../src/solver.h#L944-L1011))

Existing function takes an `Antecedent` parameter (default
`Antecedent(NOT_A_CLAUSE)` for decisions). For branch-constraint setup
we want different counter semantics:

- `num_implications_` should NOT increment (not a real implication)
- `decisions_since_connectivity_check_` should NOT increment (not a
  real branching decision)
- Clause header score should NOT change (no real clause)
- INV_C antecedent-in-scope check: branch-constraint has no clause, so
  vacuously satisfied; skip

**Cleanest option: introduce a separate function**

```cpp
bool setLiteralAsBranchConstraint(LiteralID lit) {
    if (literal_values_[lit] != X_TRI) return false;
    var(lit).decision_level = stack_.get_decision_level();
    var(lit).ante = Antecedent(NOT_A_CLAUSE);
    var(lit).is_branch_constraint = true;
    literal_stack_.push_back(lit);
    literal_values_[lit] = T_TRI;
    literal_values_[lit.neg()] = F_TRI;
    if (deriv_cache_hooks_enabled_) deriv_cache_track_lit_assign_(lit);
    // NB: no num_implications_ bump, no
    // decisions_since_connectivity_check_ bump, no INV_C check,
    // no clause header score change — none of those apply to a
    // branch-constraint imposition.
    return true;
}
```

### 1f. `unSet` ([`instance.h:31-57`](../src/instance.h#L31-L57))

Need to clear `is_branch_constraint = false` alongside the existing
`ante = NOT_A_CLAUSE` reset. Also: the INV_R7 check at
[`instance.h:51-56`](../src/instance.h#L51-L56) asserts the antecedent
got cleared. Need to also assert `is_branch_constraint == false` (or
expand INV_R7 to cover both).

### 1g. Variable initialization

Sites that init `Variable` to a clean state:

- Default constructor (struct member default `= false` — done automatically if we declare it inline)
- [`solver.cpp:232`](../src/solver.cpp#L232) `variables_[v].ante = Antecedent(NOT_A_CLAUSE);` — does NOT reset is_branch_constraint. Add `variables_[v].is_branch_constraint = false;`
- [`solver.cpp:2300`](../src/solver.cpp#L2300) similar
- [`instance.h:368`](../src/instance.h#L368) guard-variable setup — same

### 1h. Decision-vs-implication classification sites

These check `ant.isAnt()` or `var(l).ante.isAnt()` to distinguish
decisions from BCP propagations. Branch-constraint should classify as
NEITHER (third category). For most of these, treat as "not a
decision" (so they don't pollute decision counters):

| Site | File:Line | Current check | For branch-constraint |
|---|---|---|---|
| Conflict logging | [`solver.cpp:961`](../src/solver.cpp#L961), [`solver.cpp:1047`](../src/solver.cpp#L1047) | `!var(l).ante.isAnt()` prints decision lits | Print branch-constraint separately or skip; diagnostic only |
| Stats: implications counter | [`solver.h:997`](../src/solver.h#L997) | `if (ant.isAnt()) statistics_.num_implications_++` | N/A — branch-constraint uses separate setter |
| Stats: decisions counter | [`solver.h:1009`](../src/solver.h#L1009) | `if (!ant.isAnt()) decisions_since_connectivity_check_++` | N/A — branch-constraint uses separate setter |
| BCP scope-firing log | [`solver.h:966`](../src/solver.h#L966) | `!var(l).ante.isAnt()` | Same as 961 |
| Diagnostic: trail residue | [`solver_diagnostics.cpp:823`](../src/solver_diagnostics.cpp#L823) | `!var(tl).ante.isAnt()` | Update if classification matters |
| Diagnostic: var-decision-check | [`solver_diagnostics.cpp:1418`](../src/solver_diagnostics.cpp#L1418) | `variables_[v].ante.isAnt()` | Update |
| `solver_rec.cpp:1967, 1971, 2082` | Various trail walks | `!var(l).ante.isAnt()` | Each needs case-by-case audit |
| `solver_rec.cpp:2212` | Antecedent-clause replay | `if (!ant.isAnt() || !ant.isAClause()) continue;` | Branch-constraint has isAnt false in the Antecedent-method sense; continue (skip) is correct |

### 1i. Activity score updates

[`solver.h:999-1000`](../src/solver.h#L999-L1000):
```cpp
if (ant.isAClause() && ant.asCl() != NOT_A_CLAUSE)
    getHeaderOf(ant.asCl()).increaseScore();
```

For branch-constraint, ante is NOT_A_CLAUSE → condition false → no-op.
But if we ever set ante to a non-NOT_A_CLAUSE sentinel, this would
fire spuriously. Stick with NOT_A_CLAUSE encoding for branch-constraint.

### 1j. `isAntecedentOf` ([`instance.h:67-69`](../src/instance.h#L67-L69))

```cpp
bool isAntecedentOf(ClauseOfs ante_cl, LiteralID lit) {
    return var(lit).ante.isAClause() && (var(lit).ante.asCl() == ante_cl);
}
```

For branch-constraint, ante = NOT_A_CLAUSE → isAClause true, asCl == 0.
If `ante_cl == 0` is passed, this returns true — but the caller
probably never passes 0 (NOT_A_CLAUSE). Add `&& !var(lit).is_branch_constraint`
to be safe.

### 1k. `instance.cpp:345` clause relocation

```cpp
var(*beginOf(clause_ofs)).ante = Antecedent(new_ofs);
```

This updates the antecedent pointer when a clause is moved in the pool
(part of clause pool compaction). For branch-constraint vars, the
"antecedent" is NOT_A_CLAUSE so this shouldn't fire. But to be safe:
verify clause pool compaction doesn't run during a negate-arm scope
(it shouldn't — compaction is between solves).

---

## 2. `branchOnClause` negate-arm rewrite

Current code: [`solver_rec.cpp:2238-2276`](../src/solver_rec.cpp#L2238-L2276)
pushes a fresh StackLevel + runs BCP per ¬l_i. Replace with:

```cpp
bool conflict = false;
if (negate_literals) {
    bool first = true;
    for (auto it = beginOf(cl_ofs); *it != SENTINEL_LIT; ++it) {
        if (isSatisfied(*it)) { conflict = true; break; }
        if (!isActive(*it)) continue;
        if (first) {
            // ¬l_1: real decision. StackLevel was already pushed at
            // branchOnClause entry, so just set the lit.
            if (!setLiteralIfFree(it->neg())) continue;
            first = false;
        } else {
            // ¬l_2 .. ¬l_k: branch-constraint imposition.
            if (!setLiteralAsBranchConstraint(it->neg())) continue;
        }
    }
    // Single BCP at the end. Same DL as ¬l_1.
    if (!conflict) {
        if (!BCP(lit_save)) conflict = true;
    }
}
```

Then re-enable learning on conflict (the
[`// Learning is not enabled here yet`](../src/solver_rec.cpp#L2291)
comment can be deleted), and the per-decision-DL StackLevel pop loop
goes away.

---

## 3. Implementation order (single sitting, no partial commits except marked)

1. **Add `is_branch_constraint` to `Variable` struct.** [`structures.h:211-214`](../src/structures.h#L211-L214). Default-initialize false.
   *Risk: zero — adding an unused bool.*  → COMMIT 1
2. **Modify `hasAntecedent`.** [`instance.h:63-65`](../src/instance.h#L63-L65).
   *Risk: zero — flag defaults false, so behavior unchanged everywhere.*  → COMMIT 2
3. **Modify `unSet`.** [`instance.h:31-57`](../src/instance.h#L31-L57). Clear flag, expand INV_R7.
   *Risk: zero — flag is always false right now.*  → COMMIT 2
4. **Init-site cleanup.** [`solver.cpp:232`](../src/solver.cpp#L232), [`solver.cpp:2300`](../src/solver.cpp#L2300), [`instance.h:368`](../src/instance.h#L368). Reset flag.
   *Risk: zero — defensive.*  → COMMIT 2
5. **Add `setLiteralAsBranchConstraint`.** New method on `Instance` (or `Solver`).
   *Risk: zero — new method, not yet called.*  → COMMIT 2
6. **Add branch-constraint case to UIP loops.** [`solver.cpp:2097`](../src/solver.cpp#L2097) and [`solver.cpp:2237`](../src/solver.cpp#L2237).
   *Risk: low — flag still defaults false, so this case never fires until step 8.*  → COMMIT 3
7. **Add branch-constraint guard to minimization** ([`solver.cpp:1868`](../src/solver.cpp#L1868)) and any other antecedent-reader sites identified above.
   *Risk: low — flag defaults false.*  → COMMIT 3
8. **Audit the remaining sites in 1h.** Either confirm they're fine (most diagnostics) or add the flag check.
   *Risk: case-by-case.*  → COMMIT 3
9. **REWRITE branchOnClause negate arm.** [`solver_rec.cpp:2238-2276`](../src/solver_rec.cpp#L2238-L2276). This is where branch-constraint actually starts firing.
   *Risk: high — this is the point of no return.*  → COMMIT 4 — SEPARATE so it can be reverted independently.
10. **Re-enable learning at the negate-arm BCP failure path** ([`solver_rec.cpp:2291`](../src/solver_rec.cpp#L2291)).
    *Risk: medium — depends on UIP machinery being right.*  → COMMIT 5 — SEPARATE.

After each of commits 1-3, run `test_canonical_key_invariance` +
small smoke. All should pass — flag defaults false, no semantic
change yet.

After commit 4, run t1_011 + bias reproducer (count must match
`536870912306` if I have that right — verify from benchmark_log first).
Then t1_065/t1_071/t1_049 — counts must match historical, t1_049
should drop back to ~282s.

After commit 5, same battery, plus check that learned clauses are
indeed being produced and used in the negate-arm scope.

---

## 4. Acceptance criteria

- **Counts unchanged** on: t1_065 (`37 778 931 862 957 161 709 568`),
  t1_071 (`4 562 956 847 836...`), t1_011 (`536 870 912 306`),
  t1_041 (`5 516 767...`), t1_045 (`132 951 278 067 432`),
  t1_049 (`8 695 763 196 077 742`).
- **t1_011 + `-derivCacheBias 1` + line-1346-removed reproducer**:
  count matches the historical correct count (NOT `536870912306 - 6×2²⁴`).
  This is the soundness test.
- **t1_049 wall-time**: back to ~282 s (vs current 354 s with per-DL fix).
- **All `test_canonical_key_*` tests**: pass.
- **No `INV_*` assertion fires** in `check_learn_invariants` builds.

---

## 5. Risks and how to mitigate

- **Missed audit site causing soundness regression.** Run with
  `SHARPSAT_VERIFY_LEARNED=N` (large N) to brute-force-verify learned
  clauses against originals on the small instances. Any unsoundness
  surfaces as a `STORE_UNSOUND` log line.
- **Incorrect minimization dropping branch-constraint lits.** The
  guard in 1c prevents this. Test: deliberately disable minimization
  via config and compare; the produced clauses should be a subset.
- **Decision counter pollution.** If branch-constraint vars
  accidentally bump `decisions_since_connectivity_check_` or
  `num_implications_`, search behavior subtly shifts. Statistics
  comparison vs the `2beffd8` baseline catches this.

---

## 6. Open questions

- The current per-decision-DL fix file/diff is in commit `2beffd8`.
  Once branch-constraint is shipped and verified, we should REMOVE
  the per-DL pushes (currently uncommented at
  [`solver_rec.cpp:2248-2275`](../src/solver_rec.cpp#L2248-L2275))
  cleanly rather than just commenting them out. Step 9 of the plan
  does this.
- Do we want to keep the option to flip back to per-DL via a config
  flag, for safety? My recommendation: no — once branch-constraint is
  validated, the per-DL approach is strictly worse (slower AND no
  functional benefit). Removing it cleanly reduces code surface.
