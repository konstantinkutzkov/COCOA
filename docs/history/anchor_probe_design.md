# Depth-Bounded Anchor Probe — Design Notes

Status: design draft. Not yet implemented.
Companion to `probing_portfolio_handoff.md` (the big-picture portfolio plan).
Owner: TBD.

---

## 1. Purpose

A solver-internal probe that estimates, for a given variable v, **how
much cache amplification a full solve would get if v were the first
branching decision**. Output: a score that ranks candidate anchor
variables.

Targets Mechanism 3 (cache amplification) from the portfolio framework.
Faster and more direct than the current 1-s `min(rate_F, rate_T)`
probe — instead of running the solver and measuring downstream cache
hit counts, this probe enumerates sub-components to bounded depth and
measures **canonical-key reuse directly**.

---

## 2. Motivation

Cache amplification is the property "the same sub-component is reached
via multiple decision paths and the second/third/Nth visit is a cache
hit instead of a recomputation." This converts an exponential search
tree into a polynomial DAG.

Cache amplification IS canonical-key reuse — same key from different
paths.

The existing min-rate probe measures this indirectly: it runs the
solver for 1 s with v pinned and counts cache hits on large sub-
components. This is correct but expensive (~1 s × N candidates × 2
polarities). At ~100 candidates that's ~200 s — too slow as a routine
preamble.

The depth-bounded probe measures the same property structurally and
much faster: enumerate the search tree to depth K, record canonical
keys at each sub-component, count repeats.

---

## 3. Algorithm

Pseudocode:

```
function probeAnchorDepth(formula F, var v, polarity val,
                          int maxDepth, int maxPaths):
    pin v = val; BCP
    if UNSAT: return EMPTY  # this polarity is dead, signal as such

    keys = MultiSet<CanonicalKey, SubComponentInfo>
    enumeratePaths(F, maxDepth, maxPaths, keys)
    return aggregate(keys)

function enumeratePaths(F, depth, budget, keys):
    if depth == 0 or budget exhausted: return
    decompose F into components C1..Cm
    for each Ci:
        key = buildCanonicalKey(Ci)
        keys.add(key, |Ci|)
        # branch on Ci
        v_branch = pickBranchVar(Ci)
        for pol in {True, False}:
            apply v_branch = pol; BCP
            if not UNSAT:
                enumeratePaths(Ci_residual, depth - 1, budget - 1, keys)
            undo
```

Equivalent in spirit to running the solver's `solveComponent` to
bounded depth, but instead of computing counts we only collect keys
and continue exploring.

Key implementation point: **we already have `buildCanonicalKey` and
the component analyzer.** The probe reuses them; it just replaces the
"compute and combine counts" logic with "record key and recurse."

---

## 4. CLI design

New flags:

| Flag | Purpose |
|---|---|
| `--probeAnchorMode` | Enable anchor-probe mode (skip main solve). |
| `--probeAnchorDepth K` | Max recursion depth per probe (default 8). |
| `--probeAnchorBudget B` | Max paths per probe-var-polarity (default 256). |
| `--probeAnchorVars v1,v2,...` | Comma-separated candidate var list. |
| `--probeAnchorOut FILE` | JSON output file (default stdout). |

Optional: `--probeAnchorAutoVars` could replace the explicit list with
a solver-side multi-feature union (top-K by degree ∪ top-K by BCP
cascade ∪ flip-sym ∩ ¬giant-orbit ∪ ND-root-sep). For now keep that
in Python and let the caller pass the list; integrate later.

Behavior: with `--probeAnchorMode`, after preprocessing and ND-
hierarchy build (we want the same state the real solver would have),
loop over candidate vars, probe each at both polarities, emit
aggregate stats. Then exit (no main solve).

---

## 5. Output format

Per-(var, polarity) record:

```json
{
  "var": 242,
  "polarity": "F",
  "status": "OK" | "UNSAT_AT_PIN" | "BCP_CLOSED",
  "depth_used": 8,
  "paths_explored": 234,
  "total_keys": 1842,
  "unique_keys": 1420,
  "repeated_keys": 80,
  "repeat_count": 422,
  "repeated_keys_by_size": {
    "small_<25": 12,
    "medium_25_100": 28,
    "large_100_400": 30,
    "xlarge_400+": 10
  },
  "amplification_score": 18230,
  "wall_time_ms": 142
}
```

Where:
- `status` distinguishes "nothing left after pinning" (BCP_CLOSED — formula trivially solved), "this polarity is UNSAT" (interesting signal in itself), and "OK" (we explored).
- `total_keys` is the count of sub-components visited during enumeration.
- `unique_keys` is `|set(keys)|`.
- `repeated_keys` is keys with multiplicity > 1 (these are cache-amplification opportunities).
- `repeat_count` is the total events where we hit a repeated key (sum over multiplicities − 1).
- `repeated_keys_by_size` is the size distribution of components corresponding to repeated keys; the >100-var entries are what matters for amplification of meaningful work.
- `amplification_score` is the scalar metric (see §6).

Per-var summary aggregates both polarities:

```json
{
  "var": 242,
  "score_min": 15800,
  "score_max": 18230,
  "lopsided": false
}
```

`score_min` is the min-aggregated score (`min(F.amplification_score,
T.amplification_score)`) — the analog of `min(rate_F, rate_T)` in the
older probe.

---

## 6. Scoring

The amplification score per (var, polarity):

```
amplification_score = sum over repeated keys k of:
    multiplicity(k) * |sub-component(k)|^β
```

with `β` controlling how much we reward LARGE repeated components over
small ones. Default `β = 1.0` (linear in size). Higher β (e.g., 2.0)
emphasizes large-component reuse where the actual time savings are.

Final per-var score: `min(score_F, score_T)` — both polarities must
amplify for the variable to be a good anchor (as established by the
2026-05-12 min-aggregation study).

Selection rule for the portfolio analyzer:
- Top candidate's `score_min` ≥ τ_strong (TBD) → pin it as first decision.
- Else: no anchor pinning, fall back to default picker.

τ_strong needs calibration. From the existing data on t1_041, v450 had
~10 % cache-hit rate during a 1-s probe; the depth-bounded probe
should produce a corresponding score level. Initial calibration:
probe v450, v242 on t1_041 and use their scores as a reference for
"strong anchor."

---

## 7. Implementation pointers

### Files to touch

- [src/solver_config.h](../src/solver_config.h) — add the new flags as config fields.
- [src/main.cpp](../src/main.cpp) — parse flags, dispatch to probe mode.
- [src/solver_rec.cpp](../src/solver_rec.cpp) — add `probeAnchorMode()` entry point that loops over candidate vars and calls `probeOneAnchor(v, polarity)`.
- New header `src/anchor_probe.h` + `anchor_probe.cpp` — the depth-bounded enumeration logic, ~100-150 lines.

### Reused machinery

- `buildCanonicalKey` (in [src/canonical_key.cpp](../src/canonical_key.cpp)) — already computes the keys we want.
- Component analyzer ([src/alt_component_analyzer.cpp](../src/alt_component_analyzer.cpp)) — for decomposition.
- BCP machinery — standard.
- Decision stack / trail — for push/pop during enumeration.

### Key implementation details

1. **State management**: each "branch" pushes onto literal_stack_,
   modifies cls_status, etc. On unwind, restore state. This is
   exactly what the existing recursive solver does — replicate the
   push/pop discipline.

2. **Avoid the cache**: during probing, don't store results in the
   component cache. The point is to test "would the cache help" not
   to actually solve. (Alternatively, allow cache stores but disable
   cache lookups — then the keys we collect mirror what a fresh run
   would compute.)

3. **Path budget**: `maxPaths` is an upper bound on the total
   enumeration. Hit it and stop the recursion at this depth (recurse
   only if remaining budget > 0). Prevents pathological cases on
   formulas with no BCP closure.

4. **Aggregation**: maintain a hashmap `key → (multiplicity,
   max_component_size)`. After enumeration, iterate to compute
   `unique_keys`, `repeated_keys`, scores.

5. **Memory**: at K=8 with 256-path budget we have at most a few
   thousand keys per probe. Negligible.

6. **Picker during probe**: which var to branch on at each level? Use
   the solver's default picker (the one the real solve would use).
   The probe should mirror real branching behavior up to depth K.
   This is important — using a different picker would mean we measure
   key reuse under a counterfactual policy.

---

## 8. Open design questions

### Q1: How deep is deep enough?

K=8 is the initial guess. Calibration test: probe v450 and v242 on
t1_041 with K ∈ {4, 6, 8, 10, 12, 16}. See where the amplification
score stabilizes. Pick the smallest K that captures the signal.

### Q2: Should we use BCP-only or also clause learning?

The full solver learns clauses during a real solve, which can change
key reuse patterns at depth. The probe likely shouldn't learn (each
candidate's probe is independent; learning would let one candidate's
probe pollute another's). Decision: BCP only, no learning.

### Q3: Should the probe see SHARED state across candidates?

E.g., if v_a and v_b are probed in sequence, should v_b's probe see
keys recorded during v_a's probe?

Two arguments:
- YES: it's structurally correct — in a real solve the cache is
  shared across all branches. The first probe seeds the cache; later
  probes might "hit" it.
- NO: we want each candidate evaluated independently. Probe v_a's
  results shouldn't depend on probe order.

Recommendation: NO (independent per-candidate probes). Sharing would
make scoring path-dependent in a confusing way.

### Q4: How do candidate variables get selected?

Out of scope for this document — the caller passes the list. The
upstream selection (multi-feature union: degree / cascade /
flip-sym ∩ ¬giant-orbit / sep-membership) is done in Python or as a
later solver extension.

### Q5: Multi-polarity output

Always probe both polarities? Or skip the second if the first scores
zero?

Recommendation: always probe both. The metric is min-aggregated; we
need both numbers. Early termination is a small optimization.

### Q6: What about clauses, not just variables?

The current design probes variable-pin candidates. Mechanism 1
(separator branching) sometimes wants to branch on clauses. Extending
the probe to clause candidates is mechanically similar (use the
clause-branching identity `#SAT(F) = #SAT(F∖{C}) − #SAT(F∖{C} ∧ ¬C)`)
but the existing solver code paths for clause branching need to be
hit instead of variable branching. Defer to a follow-up.

---

## 9. Validation plan

1. **Smoke test**: run on t1_041 with v242 and v70 as candidates.
   Expectation:
   - v242: high `repeated_keys`, high `amplification_score`
   - v70: low both
2. **Score calibration**: run on t1_041 with the 10 known fast anchors
   (v242, v450, v405, v456, v526, v407, v631, v263, v5, v459) and
   v70 / v153 / v176 / v33 / v1 (known slow or false-positive). Verify
   score ranking matches actual solve-time ranking.
3. **Sanity against existing min-rate**: for each candidate, compare
   the new depth-bounded score to the existing 1-s `min(rate_F,
   rate_T)`. They should correlate strongly (Spearman ≥ 0.7).
4. **Depth sweep**: K ∈ {4, 6, 8, 10, 12}. Find the smallest K where
   the score ranking stabilizes.
5. **Generalization**: probe on t1_021_k10_s1 (small ganak-class).
   Expectation: no good anchors (the doc says `-sepVarBias` handles
   it via Mechanism 1, not Mechanism 3). All candidate scores should
   be low.

Validation set lives in `docs/benchmark_log.md` 2026-05-12 entry —
ground truth times for the candidate set.

---

## 10. Cost budget

For 100 candidates × 2 polarities × ~100 ms (K=8, BCP-closure typical)
= **~20 s total** per formula. Falls well within a 1-3 minute portfolio
budget, leaves room for other probes (Mechanism 1, 2, 4 measurements).

Worst case (no BCP closure, full 2^8 = 256 paths per probe):
100 × 2 × ~250 ms = ~50 s. Still tractable.

---

## 11. What this replaces / complements

**Replaces**: the current Python-orchestrated 1-s subprocess probe per
candidate (slow due to subprocess + preprocessing overhead per
candidate).

**Complements**: doesn't replace the broader stochastic-skip
runtime probe (which measures all four mechanisms, not just
amplification). The anchor probe is targeted at Mechanism 3
specifically. A full portfolio campaign would run both.

---

## 12. Pointer to related work

- `docs/benchmark_log.md` 2026-05-12 — original min-rate study and
  ganak-verified counts.
- `docs/portfolio_insights.md` §4 2026-05-12 — flip-symmetry + min-rate
  combined findings.
- `docs/probing_portfolio_handoff.md` — the broader portfolio
  framework this fits inside.
- [src/canonical_key.cpp](../src/canonical_key.cpp) — the canonical-key WL cascade we
  reuse.

---

## 13. One-paragraph summary

A solver-internal anchor probe that, for each candidate variable v,
enumerates the search tree to bounded depth K (~8) after pinning v,
collects canonical keys at each sub-component, and scores by the
amount of repeated-key activity weighted by component size. Targets
Mechanism 3 (cache amplification). Roughly 100-150 lines of new code,
reuses `buildCanonicalKey` and the component analyzer. Output: a per-
candidate score, min-aggregated across polarities, that ranks
candidates by their expected cache-amplification value. Estimated
cost: ~20 s for 100 candidates on a typical instance — 10× faster
than the current 1-s-subprocess-per-candidate Python probe.
