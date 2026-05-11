# Probe-Based #SAT-Sound Preprocessing — Design Plan

**Status:** Design doc only — no code yet.
**Owner:** sharpSAT-separator preprocessing pass
**Date:** 2026-04-25

## 1. Contract

The pass takes a CNF formula `F` and produces a transformed formula `F'` such that

> **#SAT(F) = #SAT(F')**

where the variable set of `F'` may differ from that of `F` (definitional
elimination, R4 below, shrinks it).

This is the **only** correctness guarantee the pass must uphold. Nothing else
in the solver matters at this layer.

## 2. The framing: diff-and-lift

We already have a #SAT-sound simplification routine `A`, exposed by
`src/preprocessor.h::preprocess(...)`. It performs subsumption,
pure-duplicate resolution, SSR, and BCP — all #SAT-sound.

The new pass uses `A` as a **black box**. For each probe σ:

```
1.  W := clone(F)
2.  apply σ to W (assign σ's literals, propagate)        # produces W = F|σ
3.  W' := A(W)                                            # run A on F|σ
4.  diff (F|σ → W') tells us what A learned under σ
5.  lift each learned operation to a global learning on F by prefixing
    with ¬σ (the disjunctive negation of σ's literals)
6.  add lifted learnings to F
7.  discard W and W'
```

The probe pass owns the cloning, the σ-application, and the
diff-and-lift; it does **not** reimplement subsumption, SSR, BCP, or
pure-duplicate resolution. Whatever `A` does, the probe pass picks up
automatically.

## 3. Lifting table

Let σ be a partial assignment with literals `σ = {ℓ₁, ..., ℓₖ}`, and let
`¬σ = (¬ℓ₁ ∨ ... ∨ ¬ℓₖ)`. After `A(F|σ)`:

| What A did to F|σ                       | What we add to global F                             |
| --------------------------------------- | --------------------------------------------------- |
| Derived a unit literal `ℓ` (BCP)        | `(¬σ ∨ ℓ)`                                          |
| Derived `⊥` (F\|σ is UNSAT)             | `¬σ` (the disjunctive nogood — a clause)            |
| Strengthened clause `D` to `D'` (SSR)   | `(¬σ ∨ D')`                                         |
| Removed clause `D` (subsumed/satisfied) | nothing — operation has no global content           |
| Pure-duplicate-resolved on var `x`      | nothing — operation says "x is free under σ", which doesn't lift to a global fact about x |

## 4. Soundness theorem (single, covers all liftings above)

Let `A` be a #SAT-sound simplification routine: `#SAT(F') = #SAT(F)` for
any `F`, where `F' = A(F)`. Let σ be any partial assignment.

Then for any clause `C` that holds in every model of `A(F|σ) = (F|σ)'`,
the clause `(¬σ ∨ C)` holds in every model of F.

**Proof.** Pick any model M of F. Either M falsifies some literal of σ
(then (¬σ ∨ C) is satisfied via the ¬σ part) or M extends σ. In the
second case, M satisfies F|σ, hence — since A is #SAT-sound — M satisfies
A(F|σ). Since C holds in every model of A(F|σ), M satisfies C. Either
way, M satisfies (¬σ ∨ C). □

**Consequence.** Each row of §3's lifting table is sound:
- BCP derives unit `ℓ` ⇒ `ℓ` holds in every model of A(F|σ) ⇒ `(¬σ ∨ ℓ)` is global.
- A(F|σ) derives `⊥` ⇒ A(F|σ) has no models ⇒ vacuously every model of A(F|σ) satisfies any clause; in particular take C = empty clause = ⊥, then (¬σ ∨ ⊥) = ¬σ is global.
- SSR strengthens `D` to `D'` in F|σ ⇒ A(F|σ) ⊨ D' ⇒ `(¬σ ∨ D')` is global.

The "removed clause" and "pure-duplicate" cases yield no new learnable
clause because they aren't of the form "C holds in every model of A(F|σ)" —
they are operations on the formula representation, not implications.

## 5. The cross-probe rule (R4): definitional elimination

The diff-and-lift schema captures everything `A` discovers under a
**single** probe σ. It does **not** capture inferences across two probes.
The one such inference we want is:

**R4. Definitional elimination.** For variables `a, b` (a ≠ b), if:
- `A(F|{a=0})` forces `b = v₀` (sat), and
- `A(F|{a=1})` forces `b = v₁` (sat), and
- `v₀ ≠ v₁`,

then substitute `b ← a` (if `(v₀, v₁) = (0, 1)`) or `b ← ¬a` (if
`(v₀, v₁) = (1, 0)`) throughout F and remove `b` from the variable set.

**Soundness.** Under either branch of `a`, `b` is determined by `a`
(`b = v_{a}`). Since every model assigns `a` to 0 or 1, every model
satisfies `b = a` (resp. `b = ¬a`). Substitute and drop `b`: counts
match because for every model M of F, the projection M|_{V\{b}} is a
model of the substituted formula, and the inverse extension `b := a`
(resp. `¬a`) takes a model of the substituted formula back to a model
of F. Bijection ⇒ counts equal.

If one branch is UNSAT, R4's precondition fails; we instead add the
unit `(a = ¬v)` for the UNSAT branch (this is also a §3-table case:
`A(F|{a=v}) = ⊥` lifts to the unit clause).

R4 is implemented as a separate step **on top of** the diff-and-lift
schema: for each variable `a` we run two probes and compare their
forced-literal sets.

## 6. Probe generation

For the diff-and-lift schema (single-probe rules):

1. **Variable selection.** Variables in short clauses first (binaries,
   then ternaries). Within a tier, prefer **high-degree variables**
   (more occurrences = more potential cascades and more clauses
   simplified by σ).
2. **Polarity selection.** Random per variable.
3. **Probe length k.** Default `k = 1`; allow `k ≥ 2` for chain
   discoveries. Higher k yields longer ¬σ disjunctions — capped via
   `lspMaxSize` (see §8).
4. **Pruning.** If `A(F|σ) = F|σ` (A did nothing), skip — no learnings.

For R4: enumerate variables `a` (highest-degree first), run both
σ = {a=0} and σ = {a=1} probes, intersect their forced-literal sets
to find candidate `b`'s.

## 7. Pass structure and termination

```
preprocess F with A first (existing behavior)
loop:
    fired := false
    for each chosen probe σ:
        run diff-and-lift on σ ; add learnings to F if any
        fired |= (any learnings added)
    for each chosen variable a:
        run R4 ; substitute and drop b if applicable
        fired |= (R4 fired)
    if not fired:
        break
    # mop up: re-run A so any subsumptions / SSR exposed by the new
    # learnings are applied
    F := A(F)
```

**Termination.** Each rule strictly reduces a monotone quantity:
learnings reduce model space (at least zero models removed) but more
importantly each fired rule either adds a clause that was not previously
implied (finite supply of useful learnings on a finite formula) or
shrinks the variable set (R4, finite supply). The outer time budget
caps the loop unconditionally.

## 8. Caps (CLI-tunable)

- `lspMaxProbes` (default 1000) — total probes attempted per pass.
- `lspMaxSize`   (default 4)    — maximum size of σ; constrains the
                                  ¬σ portion of every lifted clause.
- `lspMaxTotal`  (default 5000) — total clauses learned (across all
                                  rules) per preprocessing invocation.
- `lspBudgetMs`  (defaults to existing `preprocess_time_budget_ms`) —
  wall-time cap for the whole pass.

R4 has no separate cap; it's bounded by the variable count.

## 9. Flag surface

```
-localSearchPreprocess         # master toggle (default OFF)
-lspMaxProbes N                # default 1000
-lspMaxSize N                  # default 4
-lspMaxTotal N                 # default 5000
-lspBudgetMs N                 # default = preprocess_time_budget_ms
-lspNoR4                       # disable definitional elimination only
-lspVerbose                    # per-pass stats
```

R4 is the only individually-disable-able rule because it has the
distinct cross-probe shape and the variable-set bookkeeping
implications. The diff-and-lift rules are enabled or disabled as a
group via the master toggle — they share infrastructure and there's no
benefit to splitting them at the flag layer.

## 10. Test plan

The tests check **only the contract**: `#SAT(F) = #SAT(F')` after
preprocessing.

### 10.1. Unified invariance test (the schema-level test)

For each random small CNF `F` (5–15 vars, 10–40 clauses, drawn from a
fixed seeded generator) and each random short σ:
1. Compute `#SAT(F)` by brute-force enumeration.
2. Run the probe pass, restricted to a single probe σ via a test hook.
3. Compute `#SAT(F')` by brute-force enumeration.
4. Assert equality.

Run ~200 such cases. This single test exercises every row of the §3
lifting table and is the strongest check against soundness regressions.

### 10.2. Per-rule targeted tests

Hand-built minimal CNFs that:
- Force a particular row of the lifting table to fire (BCP-unit, UNSAT,
  SSR-strengthen, subsumption, pure-duplicate).
- Trigger R4 in both polarity-pair shapes.
- Trigger R4 with one-branch-UNSAT (degenerate case).

Each test brute-force-counts before and after.

### 10.3. R4 variable-set bookkeeping test

A specific test that R4 fires and that the solver's reported count
matches the brute-force count over the **new** (smaller) variable set.
Most likely place for an implementation bug: forgetting to update the
component analyzer's variable-count, or counting the now-absent variable
as still active.

### 10.4. End-to-end regression on existing benchmarks

Run sharpSAT with `-localSearchPreprocess` on existing inputs (`t1_011`,
`t1_065`, `t1_071`, `t1_049_k6_s1`, `t1_049`). Counts must match the
previously-recorded values. Wall-time may change.

## 11. Module layout

- `src/probe_preprocessor.h` / `.cpp` — public entry point, probe
  generation, diff-and-lift harness, R4 driver.
- Reuses `src/preprocessor.h::preprocess(...)` as `A`. No reimplementation
  of subsumption / SSR / BCP.
- The clone step (clone F into a working CNF) needs careful attention
  to which structures get copied — at minimum: clauses, watch lists,
  binary links, occurrence lists, literal value array. Most of this
  scaffolding already exists in the preprocessor's input-CNF handling;
  we likely just need a copy-constructor or `cloneFormula(F)` helper.
- Tests under `tests/test_probe_preprocessor_*.cpp`:
  - `test_probe_preprocessor_invariance.cpp` (the §10.1 unified test)
  - `test_probe_preprocessor_per_rule.cpp`   (the §10.2 per-rule cases)
  - `test_probe_preprocessor_r4.cpp`         (the §10.3 R4 bookkeeping)

## 12. Implementation order

1. **Clone helper + diff harness.** Implement `cloneFormula(F)`,
   `applyAssignment(W, σ)`, `diffSimplifications(W_pre, W_post)`. Tests
   for the harness itself (assert clone independence, assert diff
   correctness on a hand-built example). No rules yet.
2. **Single-probe diff-and-lift.** Wire the harness to call `A` and
   emit lifted clauses. Initial test: §10.1 invariance test passes.
3. **R4 (definitional elimination).** Add the variable-set bookkeeping
   and the cross-probe driver. §10.3 passes.
4. **End-to-end regression.** §10.4. Record numbers in
   `docs/benchmark_log.md`.

Each step is a separate commit, with tests landing in the same commit
as the feature.

## 13. Open questions / explicit non-goals

- **Not in scope:** separator-aware gating that rolls back learnings
  worsening METIS cuts. That is a separate routine, by the user's
  explicit instruction.
- **Not in scope:** updating component analysis, separator code, cache,
  or branching to be aware of eliminated variables (R4). The pass
  produces a CNF; the rest of the solver consumes that CNF as input.
- **Open:** should `cloneFormula` be a deep copy of the full preprocessor
  state, or a lightweight CNF-only structure? Probably the latter — A
  builds its own internal state from a CNF input, so we just need the
  clauses + lit count.
- **Open:** should we cache `A(F|σ)` results across probes when σ overlaps?
  Premature — measure first.
