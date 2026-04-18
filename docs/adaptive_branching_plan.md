# Adaptive Branching for Dense Instances

## Motivation

Our precomputed METIS separator hierarchy (fixed in commit `fc08c7e`) is
excellent on sparse instances of low treewidth (e.g., `t1_065` with
tw=4 — 10× faster than sharpsat-td). On dense instances it fails
because METIS can only return large separators (`t1_049`: 47-var
separator / 90 vars = 52%, with balance 36/7 — not a disconnection in
any meaningful sense). Branching on such a "separator" is no better
than 2^n enumeration.

The goal is a branching strategy that smoothly degrades across the
density spectrum:

- Sparse (METIS works) → use the precomputed hierarchy.
- Intermediate → branch to force unit propagation, densifying the
  formula with cheap learned clauses.
- Dense (no small separator, no cascading propagation available) →
  branch to reduce density as a last resort.

The design principle: **polynomial per-decision overhead for
exponential reduction in tree size**.

---

## Three-tier architecture

At every decision point inside `solveComponent`, when a separator
reset is needed:

1. **Tier 1: METIS hierarchy lookup.** Gated by separator-quality
   thresholds. If the precomputed separator for the current hierarchy
   node is small and balanced enough, consume it as today.
2. **Tier 2: propagation-oriented variable branching.** Score
   candidates by combined BCP cascade size + structural improvement in
   the 2-clause population. Aggregate the two branches' scores via
   branching number τ.
3. **Tier 3: density-reduction tiebreaker.** Used when Tier 2's best
   candidates are indistinguishable (all similar scores, no cascade
   available).

Tier 1 is already implemented. Tier 2 is the focus. Tier 3 is a
light fallback.

---

## Tier 1: METIS separator with thresholds

```
if config_.perform_separator_branching
    and comp.num_variables() >= separator_min_active_vars
    and hierarchy_separator_is_acceptable(nd_node, comp):
    separator = lookupSeparator(...)
    # proceed with separator branching as today
```

Where `hierarchy_separator_is_acceptable` checks:

    sep_filtered_size / comp.num_variables() <= θ_sep_ratio    (default 0.20)
    balance(L_side_count, R_side_count) >= θ_balance            (default 0.30)

Both gates must pass. `sep_filtered_size` uses the already-implemented
filter (intersect with current component vars/clauses) from
`solver_rec.cpp`. `L_side_count` and `R_side_count` come from applying
`mapToChild` logic conceptually — how many active component vars fall
into each hierarchy child.

If thresholds reject, fall through to Tier 2.

**New config:**

```cpp
double separator_max_ratio = 0.20;     // sep/n gate
double separator_min_balance = 0.30;   // balance gate
```

---

## Tier 2: propagation-oriented branching

### Score function

For a candidate variable `v` and polarity σ ∈ {T, F}:

    s(v, σ) = vars_forced(v, σ)  +  ε · Δ_2clauses(v, σ)

where:

- `vars_forced(v, σ)` = number of variables assigned (including v)
  after setting v=σ and running BCP to fixpoint from the current
  component state. Minimum 1. Zero indicates a conflict → **failed
  literal** (force v=¬σ as unit, don't branch).
- `Δ_2clauses(v, σ)` = (# of active length-2 clauses after BCP) − (# of
  active length-2 clauses before). Net change: positive if BCP created
  new 2-clauses (from 3→2 shortening) net of those destroyed (via
  satisfaction or firing).
- `ε` = small weight, default 0.1.

### Aggregation via branching number

Both branches contribute to the search tree cost. Given scores
`(a, b) = (s(v,T), s(v,F))`, the branching number `τ` satisfies:

    τ^(-a) + τ^(-b) = 1

We pick the variable minimizing τ. Branching number is a more
accurate predictor of tree size than simpler aggregations:

- `min(a, b)` can rank `(1, 10)` below `(2, 2)`, but τ(1,10) ≈ 1.193
  while τ(2,2) ≈ 1.414 — the asymmetric case has a much smaller tree.
- `a + b` ignores imbalance entirely.

Computing τ: Newton's method on `f(τ) = τ^(-a) + τ^(-b) − 1`,
starting from τ = 2^(1/max(a,b)), converges in ~5 iterations to
double precision. Trivial cost per candidate.

### Failed literal handling

Before scoring, run a quick failed-literal check: if probing (v, T)
produces a conflict, force v = F and skip scoring v. If both
directions conflict, the component is UNSAT — return 0.

### Stage 0: cheap pre-filter for top-K

Scoring all active vars by full BCP would cost O(n_active × BCP). Instead:

    For each candidate v:
        cheap_score(v) = |binary_links_[LiteralID(v, false)]|
                      + |binary_links_[LiteralID(v, true)]|

This is O(1) per var, O(n_active) total. Sort by `cheap_score`, take
top-K (default K = 20) for full BCP probing.

### Stage 1: binary implication closure (optional)

For the current component, compute the transitive closure of the
binary-implication graph:

    reachable_binary(l) = all literals forced via chains of binary
                          clauses starting from l

This can be computed via SCC decomposition + DAG reachability in
O((V + E) · output_size), typically O(n · c̄) where c̄ is average
cascade size (usually small).

With this closure, a "cheap BCP simulation" for any seed literal is
O(1) lookup. It approximates the true BCP (missing length-≥3 cascade
effects, which are handled in Stage 2).

**Decision:** include this only if Stage 2 profiling shows BCP
simulation dominating. Otherwise skip and go directly to Stage 2 on
the top-K candidates.

### Stage 2: full BCP probing for top-K

For each top-K candidate v, each polarity σ:

    save_state()
    set_literal(v, σ)
    run_BCP()
    record vars_forced = size of BCP cascade (including v)
    record Δ_2clauses = current n_2clauses − baseline n_2clauses
    rollback_state()

Cost per probe: O(cascade size × avg_watches_per_literal), roughly
proportional to `fail_test`'s existing cost. The incremental
2-clause counter adds O(1) per literal processed.

### Implicant caching as scoped learned clauses

During a BCP cascade in a probe, when we deduce that a set of
literals `S = {l_1, ..., l_k}` (with `k ≤ 4`) jointly forces a
literal `l`, we record the implicant clause:

    C_imp = (¬l_1 ∨ ¬l_2 ∨ ... ∨ ¬l_k ∨ l)

This clause is logically implied by the formula (proved by the BCP
derivation). Add it to the solver's learned clause pool, tagged with
the current scope `S_learn = removed_clauses_` (see
`instance.h`'s `learned_clause_scope_`). Subsequent probes benefit
from the added clause via standard watched-literal BCP.

**Caps:**
- Implicant size ≤ 4 literals (so clause ≤ 5 literals). Longer
  implicants are too specific to fire often.
- At most 5 implicants cached per target literal. When full, evict
  longest implicant first. Prefer shorter implicants (fire sooner in
  BCP).

**Scope management:** the existing `learnedClauseInScope` check makes
cache invalidation on clause branching automatic — learned implicants
tagged at scope `S_learn` are inactive when scope shifts to a state
where `S_use ⊄ S_learn`.

### Incremental 2-clause counter

Maintain `n_2clauses` globally, updated incrementally during all BCP
activity (probes AND real branches):

    On literal l becoming true:
        for each clause C containing +l (will be satisfied):
            if current_length(C) == 2: n_2clauses--
            mark C satisfied
        for each clause C containing ¬l (will be shortened):
            current_length(C) -= 1
            if new_length == 1: n_2clauses-- (it becomes unit)
            else if new_length == 2: n_2clauses++ (3→2)

    On backtrack:
        reverse the above

This requires tracking `current_length(C)` per active clause — an
`int` per clause, ~4 bytes × n_clauses storage. Clean integration
with existing `BCP()` and `unSet()` via an undo stack.

---

## Tier 3: density-reduction tiebreaker

Used when Tier 2 can't distinguish candidates — defined concretely as
`max_over_candidates(τ) / min_over_candidates(τ) < θ_τ_ratio`
(default 1.05). Meaning: all Tier 2 candidates have nearly-identical
branching numbers.

Tiebreaker score:

    density_score(v) = (# clauses v appears in)   
                     + 0.5 × (# distinct variables sharing a clause with v)

Higher is better. Pick the variable with highest `density_score`
among the Tier 2 top-K.

This is a lightweight, no-BCP-simulation fallback.

---

## Cost analysis

Per decision, the estimated cost:

| stage | worst-case | typical |
|-------|-----------|---------|
| Tier 1 lookup + threshold check | O(n_active) | O(n_active) |
| Tier 2 Stage 0 cheap filter | O(n_active) | O(n_active) |
| Tier 2 Stage 1 binary closure (optional) | O(n·(n+e)) | O(n·c̄) |
| Tier 2 Stage 2 full BCP × 2K probes | O(K · L) | O(K · c̄ · d̄) |
| τ via Newton × K | O(K) | O(K) |
| Tier 3 tiebreaker | O(K · n_avg_clause_sharing) | O(K) |

Default K=20, L = total literal occurrences. For `t1_049`
(90v, 252 cls, L≈756): per-decision cost ~15k ops ≈ 150 μs.

The incremental 2-clause counter adds amortized O(1) per BCP literal
step everywhere (not just Tier 2), so has a small but pervasive cost.

---

## Integration with existing solver

### Touched files

- `solver.h`, `solver.cpp`:
  - Add config fields for thresholds and K.
  - Extend `fail_test` to return the forced-literal count and
    Δ_2clauses, not just a success bool.
  - New method `pickBranchVariableAdaptive(comp)` returning the chosen
    variable (and polarity preference).

- `solver_rec.cpp`:
  - Modify the "no separator → variable branching" path to use the new
    adaptive picker instead of `pickBranchVariable`.

- `instance.h`:
  - Integrate implicant-clause learning with the existing
    `learned_clause_scope_` map.

- Clause-length tracking: new `std::vector<uint8_t> clause_length_`
  maintained alongside `literal_pool_`. Update in `setLiteralIfFree`,
  `unSet`, clause addition/removal.

### Infrastructure reuse

- `fail_test` (existing) handles the probe state save/restore.
- `binary_links_` (existing) for cheap filter scores.
- `learned_clause_scope_` (existing) for implicant-clause scope
  management.
- BCP watched-literal machinery (existing) propagates learned
  implicant clauses automatically.

### What does NOT change

- Tier 1 path in `solver_rec.cpp` — recently fixed and working.
- Separator filtering logic (intersect with component).
- ND hierarchy build, canonical-key computation, content cache.

---

## Implementation phases

Each phase must pass existing regression tests before moving on.

### Phase 1: infrastructure

- [ ] Add `clause_length_` vector and maintain it in `setLiteralIfFree`
  / `unSet`.
- [ ] Add `n_2clauses` counter with incremental updates on literal
  assignment/unassignment.
- [ ] Extend `fail_test` to return `{bool success, int vars_forced, int
  delta_2clauses}`.
- [ ] Unit tests on small CNFs verifying the 2-clause counter is
  consistent after random assignment/unassignment sequences.

### Phase 2: Tier 1 gating

- [ ] Add config fields `separator_max_ratio` and
  `separator_min_balance`.
- [ ] In `solver_rec.cpp`, after hierarchy filter, check thresholds.
  If rejected, `separator` stays empty, falling through to variable
  branching as today.
- [ ] Verify regression: all current tests still pass; on `t1_049`,
  confirm hierarchy separator gets rejected and we fall through to
  regular branching.

### Phase 3: Tier 2 basic scoring

- [ ] Implement `pickBranchVariableAdaptive`:
  - Enumerate candidates, apply Stage 0 cheap filter → top-K
  - For each top-K, run Stage 2 probes → compute `(a, b)`
  - Handle failed-literals
  - Compute τ via Newton
  - Pick argmin τ
- [ ] Wire into `solver_rec.cpp`'s no-separator path.
- [ ] Measure on `t1_049`: does τ-based selection outperform current
  `pickBranchVariable`? Time + decision count.

### Phase 4: implicant caching

- [ ] Modify BCP to record implicant derivations: when clause C fires
  and forces literal l, record `(S = negations of other C literals, l)`.
- [ ] Filter implicants by size (≤ 4 antecedents). Add as scoped
  learned clauses.
- [ ] Enforce per-target-literal cap (5).
- [ ] Measure impact: cache hit rate, clause count growth, effect on
  solve time.

### Phase 5: Tier 3 tiebreaker

- [ ] Implement `density_score(v)` and the τ-ratio gate.
- [ ] Empirical tuning of `θ_τ_ratio`.

### Phase 6 (optional): Stage 1 binary closure

- [ ] If profiling shows BCP probes dominate per-decision cost,
  implement binary-implication closure precomputation.
- [ ] Use closure to serve fast approximate probe lookups; fall back
  to full BCP only for top-K refinement.

---

## Measurement plan

Before committing to values, measure on a selected set:

- `t1_065` (sparse, tw=4) — must stay fast (Tier 1 owns this)
- `t1_049` (dense 3-SAT) — primary Tier 2 target
- `t1_025` (tiny dense) — stress-test
- `bug_rec_sep_cache` — regression
- `test_cb_perf` — regression

For each instance and each proposed threshold/parameter combination,
record:

- solve time
- decision count
- cache stores/hits
- # Tier 1 vs Tier 2 vs Tier 3 decisions
- avg BCP cascade size per probe
- τ distribution of selected branches

Initial parameter values (all tunable after measurement):

| parameter | default |
|-----------|---------|
| `separator_max_ratio` | 0.20 |
| `separator_min_balance` | 0.30 |
| `K` (top-K probes) | 20 |
| `ε` (2-clause weight) | 0.1 |
| `θ_τ_ratio` (Tier 2→3 trigger) | 1.05 |
| `max_implicant_size` | 4 |
| `max_implicants_per_literal` | 5 |

---

## Open questions

1. **How often does Tier 2 actually hit the implicant cache?** Needs
   measurement. If hit rate is low, simplify to "no implicant cache,
   just scoring."
2. **When should Stage 1 (binary closure) be enabled?** Depends on
   cascade characteristics per instance. Start without it.
3. **Does the 2-clause weight `ε = 0.1` work across instance types?**
   Might need to be instance-adaptive (e.g., higher for very dense
   instances where vars_forced is mostly 1, lower for sparse where
   BCP cascades are already strong).
4. **Does clause-length tracking slow the existing solver?** Measure
   against current performance on sparse instances — overhead should
   be negligible but must be confirmed.

---

## Risks

- **2-clause counter bugs.** Easy to get wrong across edge cases
  (learned clauses, clause branching, undo sequences). Requires
  careful unit tests.
- **Implicant-clause bloat.** Per-literal cap must be enforced or
  storage grows unbounded.
- **Tier 2 becomes slow on medium instances.** If probing overhead
  outweighs the savings from better branching, we lose. The threshold
  machinery must cleanly skip Tier 2 when unnecessary.
- **τ numerics.** `a = 0` or `b = 0` degenerate cases need explicit
  handling (failed literal path), not Newton on a singular equation.

---

## What's NOT in this plan

- Clause-branching enhancements (stays out of Tier 2/3 as agreed).
- Preprocessing improvements (separate effort to port sharpsat-td's
  `FPVSEGV` pipeline).
- Hierarchy rebuild at runtime for mixed components (separate concern,
  not needed for this plan).
