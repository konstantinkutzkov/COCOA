# Phase 1 Implementation Contract

Scope: minimal primitives shared by `implicitBCP` and the future
`pickBranchVariableAdaptive`. Everything else in the adaptive-branching
plan depends on these primitives being correct.

---

## Locked-in design decisions (from plan discussion)

1. **`Δ_2clauses` stays in the scoring function** (with clamp + ε as
   in the plan). It is **computed lazily inside `probeLiteral`** via
   one O(L) scan of active clauses before and after BCP. No
   incremental `n_2clauses` counter, no `clause_length_` maintenance,
   no undo stack. The O(K·L) per-decision cost is dominated by the
   probes themselves.
2. **No incremental `clause_length_` maintenance.** If Stage 0 needs
   per-clause active length, compute it lazily by scanning the
   component's active clauses at the start of Stage 0 — O(L) per
   decision, dominated by K × BCP probes. (Same lazy pattern as
   `Δ_2clauses`.)
3. **`pickBranchVariableAdaptive` returns `VariableIndex` only.** No
   "preferred polarity" — Tier 2 branches on both sides anyway.
4. **Two shared primitives**, no mode/flag:
   - `probeLiteral` — one-shot BCP probe with rollback, populating
     `uip_clauses_` on failure (same side effect as today's
     `fail_test`). On success returns `vars_forced` and
     `delta_2clauses` computed via the in-probe scan.
   - `commitFailedLiteral` — installs `uip_clauses_` as antecedents
     for forced literals, runs BCP. Extracted from
     [solver.cpp:1013-1020](../src/solver.cpp#L1013-L1020).
5. **Phase 1 does NOT touch**: any branching policy, any scoring,
   Stage 0 filter, τ computation, implicant caching, separator
   thresholds. Those are Phases 2–4.

---

## Code-side deliverables

### New API (in `solver.h`)

```cpp
struct ProbeResult {
    bool success;          // false iff BCP derived a conflict
    int  vars_forced;      // size of BCP cascade including the probed
                           // lit; undefined when success == false
    int  delta_2clauses;   // (# active 2-clauses after BCP) − (before);
                           // undefined when success == false
};

// Probes `lit` under the current partial assignment. On success:
//   - baseline_2c = count_active_2clauses(current_component)
//   - save stack position
//   - startFailedLitTest, setLiteralIfFree(lit), BCP
//   - post_2c = count_active_2clauses(current_component)
//   - delta_2clauses = post_2c - baseline_2c
//   - stopFailedLitTest, unwind to the saved position
// On failure: BCP returned conflict, recordAllUIPCauses populates
// uip_clauses_, then we stopFailedLitTest + unwind. delta_2clauses
// is not computed on the failure path.
//
// Rollback invariant: on return, literal_stack_ is identical to its
// state at entry. No side effects other than uip_clauses_ (failure
// path only) and statistics counters.
ProbeResult probeLiteral(LiteralID lit);

// Precondition: uip_clauses_ was just populated by a failed
// probeLiteral. Installs each UIP clause as an antecedent for the
// forced literal at its front, then runs BCP. Returns false iff BCP
// derives a new conflict after installation (i.e., the current
// component is UNSAT).
bool commitFailedLiteral();
```

### 2-clause scan helper

`count_active_2clauses` scans the current component's active,
non-satisfied, non-removed, in-scope clauses and counts those whose
number of non-falsified literals equals 2. Binary clauses contribute
via `binary_links_`: for each active literal `l`, count active
neighbors in `binary_links_[l]` (divide by 2 at the end to avoid
double-counting). Non-binary clauses: scan `literal_pool_`, skip
literals with `isResolved(·) == true`, stop early if more than 2
non-falsified literals found. Cost O(L) in the worst case.

The helper is component-scoped: it takes a `Component &` and only
considers clauses that belong to that component (same filter logic
used elsewhere for separator and canonical-key computation).

Both replace direct inline usage of the existing `fail_test` body.

### Refactor: `implicitBCP`

Current: [solver.cpp:938-1030+](../src/solver.cpp#L938). The inner
probe+commit at [solver.cpp:980-1020](../src/solver.cpp#L980-L1020)
collapses to:

```cpp
for (auto lit : test_lits) {
    if (!isActive(lit)) continue;
    if (threshold > literal(lit).activity_score_) continue;

    ProbeResult pr = probeLiteral(lit);
    if (pr.success) continue;

    statistics_.num_failed_literals_detected_++;
    if (!commitFailedLiteral()) return false;  // component UNSAT
}
```

Activity-based candidate selection
([solver.cpp:946-975](../src/solver.cpp#L946-L975)) stays unchanged —
it's IBCP-specific policy, not a probe concern.

### Retire: inline `fail_test`

The existing `bool Solver::fail_test(LiteralID)` at
[solver.h:291-312](../src/solver.h#L291-L312) is superseded by
`probeLiteral`. Delete it and update callers. Current callers: search
shows `implicitBCP` is the only direct user; the conflict-analysis
machinery calls `recordAllUIPCauses` through different paths.

### What does NOT change

- `BCP`, `setLiteralIfFree`, `unSet`, `recordAllUIPCauses`,
  `addUIPConflictClause`, `addScopedUIPConflictClause`.
- The IBCP skip guard at [solver.cpp:869-870](../src/solver.cpp#L869-L870).
  Phase 1 refactors IBCP internally; the decision to call IBCP at all
  is still gated on `!config_.perform_separator_branching`.
- `stack_.startFailedLitTest` / `stopFailedLitTest` semantics.

### Acceptance criteria for Phase 1 code

- Existing regression suite (`test_separator_correctness.sh`,
  `test_clause_branching.sh`, `.test_baselines`) passes unchanged.
- `implicitBCP` behavior is bit-identical to pre-refactor:
  same set of failed literals detected, same UIP clauses learned,
  same model counts on all current tests.
- The new `probeLiteral` / `commitFailedLiteral` unit tests pass.

---

## Test artifacts

All `.cnf` files and `.expected` sidecars live in
[sharpsat-separator/tests/](../tests/). Unit-test C++ sources live in
the same directory (matches existing convention:
`test_adaptive_cut.cpp`, `test_balanced_cut.cpp`, etc.). Shell
harnesses live at the repo root (matches
`test_separator_correctness.sh`, `test_clause_branching.sh`).

### `.expected` file format

One key-value line per test assertion. Keys use dot notation.
Comments start with `#`. For each probe key (`probe.V=σ`), we record
`.success`, and on success also `.vars_forced` and `.delta_2clauses`.
On failure we record `.uip_clause` as a literal set (order
independent):

```
probe.1=T.success        false
probe.1=T.uip_clause     {-1}
```

`delta_2clauses` is the raw signed count change; the scoring-side
clamp is applied by the caller, not by the probe.

### Category A — BCP cascade triggers (efficacy)

| file | vars | clauses | description |
|------|------|---------|-------------|
| `cascade_binary_chain.cnf` | 5 | 4 | `(¬x1∨x2), (¬x2∨x3), (¬x3∨x4), (¬x4∨x5)` — pure binary chain, negative Δ_2clauses |
| `cascade_fanout.cnf` | 6 | 5 | five binaries sharing `¬x1`; symmetric Δ, asymmetric `vars_forced` |
| `cascade_3to2_shortening.cnf` | 6 | 3 | length-3 clauses that shorten to 2-clauses without any forcing — **positive** Δ_2clauses, `vars_forced = 1` |
| `cascade_ternary_primed.cnf` | 6 | 5 | length-3 clauses primed by unit clauses so they fire as units when the trunk is set — negative Δ, multi-step cascade through 3-clauses |
| `dense_random_30v.cnf` | 30 | ~130 | random 3-SAT near phase transition; brute-force model count |

Expected values for the first two (hand-derived):

```
# cascade_binary_chain.cnf — clauses (¬x1∨x2),(¬x2∨x3),(¬x3∨x4),(¬x4∨x5)
# Initial active 2-clauses: 4
# Models are monotone-step strings F^k T^(5-k), k=0..5 → 6 models.
model_count                 6
probe.1=T.success           true
probe.1=T.vars_forced       5          # x1,x2,x3,x4,x5 forced by chain
probe.1=T.delta_2clauses    -4         # all 4 clauses satisfied
probe.1=F.success           true
probe.1=F.vars_forced       1          # x1 only; clause 1 satisfied, no forcing
probe.1=F.delta_2clauses    -1

# cascade_fanout.cnf — 5 binaries sharing ¬x1
# Initial active 2-clauses: 5
# x1=T → 1 model; x1=F → 2^5 = 32 models. Total 33.
model_count                 33
probe.1=T.success           true
probe.1=T.vars_forced       6          # x1 + 5 fanout forcings
probe.1=T.delta_2clauses    -5
probe.1=F.success           true
probe.1=F.vars_forced       1          # x1 only; all 5 clauses satisfied, no forcing
probe.1=F.delta_2clauses    -5

# The asymmetry in vars_forced (6 vs 1) with symmetric Δ_2clauses
# (−5 vs −5) is intentional — demonstrates the two signals are
# independent. τ(6,1) ≈ 1.28, much better than τ(2,2) ≈ 1.41.

# cascade_3to2_shortening.cnf — three length-3 clauses all containing ¬x1
#   (¬x1 ∨ x2 ∨ x3), (¬x1 ∨ x4 ∨ x5), (¬x1 ∨ x6 ∨ x2)
# Initial active 2-clauses: 0
model_count                 47
probe.1=T.success           true
probe.1=T.vars_forced       1          # no clause fires — each becomes a 2-clause but not a unit
probe.1=T.delta_2clauses    +3         # three new 2-clauses created by shortening
probe.1=F.success           true
probe.1=F.vars_forced       1
probe.1=F.delta_2clauses    0          # all 3 clauses satisfied, no 2-clauses appear

# cascade_ternary_primed.cnf — unit clauses prime length-3 clauses
#   units: (¬x5), (¬x6)
#   rest:  (¬x1 ∨ x2 ∨ x5), (¬x1 ∨ x3 ∨ x6), (¬x1 ∨ x4 ∨ x5)
# Baseline state (units applied): x5=F, x6=F. Each of the three 3-clauses
# already has 2 non-falsified literals, so n_2clauses_baseline = 3.
model_count                 9          # x1=T forces x2,x3,x4=T; x1=F: 2^3
probe.1=T.success           true
probe.1=T.vars_forced       4          # x1,x2,x3,x4
probe.1=T.delta_2clauses    -3         # all 3 primed clauses satisfied
probe.1=F.success           true
probe.1=F.vars_forced       1
probe.1=F.delta_2clauses    -3         # ¬x1 satisfies all three
```

### Category B — failed-literal + UIP correctness

| file | vars | clauses | description |
|------|------|---------|-------------|
| `failed_lit_direct.cnf` | 3 | ~5 | `(¬x1∨y), (¬x1∨¬y)` + SAT shell over x2,x3 |
| `failed_lit_chain.cnf` | 5 | ~7 | `(¬x1∨y), (¬y∨z), (¬z∨w), (¬z∨¬w)` + shell |
| `both_polarities_fail.cnf` | ~4 | ~8 | formula where probe(x=T) and probe(x=F) both fail in initial state → UNSAT component |
| `no_failed_lit.cnf` | ~8 | ~15 | random SAT instance with no single-probe failures; all probes return success |

Expected values (hand-derived, numbering the "trunk" variable as 1):

```
# failed_lit_direct.cnf
probe.1=T.success     false
probe.1=T.uip_clause  {-1}       # unit: ¬x1
probe.1=F.success     true

# failed_lit_chain.cnf
probe.1=T.success     false
probe.1=T.uip_clause  {-1}       # unit after chain collapses

# both_polarities_fail.cnf
probe.1=T.success     false
probe.1=F.success     false
model_count           0
```

### C++ unit-test harness: `tests/test_probe.cpp`

Compiles to the `test_probe` binary (new CMake target). Links against
the same solver library as `test_metis_sep`.

Responsibilities:
- Load each Category-A / Category-B `.cnf`.
- Parse its `.expected` sidecar.
- Run unit propagation on the unit clauses (sets baseline state).
- For each `probe.X=σ.*` assertion, call `probeLiteral(LiteralID(X,σ))`
  directly and compare the struct to expected values.
- For `.uip_clause` assertions, read `uip_clauses_.front()` as a set
  of `LiteralID::toInt()` values and compare with expected.
- Rollback invariant check: after each probe, assert
  `literal_stack_.size()` equals the value recorded at entry.

Failure mode: print instance name, probe literal, expected vs. actual;
exit nonzero.

### Shell harness: `test_probe_correctness.sh`

Format: same style as `test_separator_correctness.sh`. For each
Category-A / Category-B CNF, run:

```
./build/sharpSAT -noCC <file>     # baseline (IBCP on, no separator)
./build/sharpSAT -sep <file>      # separator mode (IBCP off)
./build/sharpSAT <file>           # default
```

All three must report the same model count, which must equal the
`.expected` file's `model_count` value. Any mismatch is a failure.

Both harnesses must be green before Phase 1 is declared complete.

---

## Out of scope for Phase 1 (explicit non-goals)

- Implementation of `pickBranchVariableAdaptive` (Phase 3).
- Stage 0 cheap filter (Phase 3).
- Tier 1 gating thresholds (Phase 2).
- Clause-length tracking (deferred; lazy when needed).
- Implicant caching (Phase 4).
- Any changes to `solver_rec.cpp`.

---

## Open checklist before coding

- [ ] User confirms the 8 test CNF shapes and expected-value tables
      above (including the Δ_2clauses values).
- [ ] User confirms test binary naming (`test_probe`) and shell
      script naming (`test_probe_correctness.sh`).
- [ ] User confirms `count_active_2clauses` is component-scoped and
      that the "non-falsified literals == 2" definition matches how
      the scoring function should interpret "active 2-clause."

Once checked, implementation proceeds in this order: CNFs + expected
files → `test_probe.cpp` (fails initially, as expected) →
`probeLiteral` + `commitFailedLiteral` + IBCP refactor → unit test
green → shell test green → Phase 1 complete.
