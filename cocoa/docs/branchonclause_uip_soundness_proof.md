# Soundness of CDCL learning under clause-branching: a formal proof

**Context.** sharpSAT's `branchOnClause` implements the inclusion-exclusion
identity for counting models. Branching on a long clause C splits the count

    #SAT(F) = #SAT(F\{C}) − #SAT(F\{C} ∧ ¬C)

into two recursive sub-problems. The "negate" arm (`#SAT(F\{C} ∧ ¬C)`)
historically asserted all `¬l_i` for `l_i ∈ C` at a single decision level,
which violates the one-decision-per-DL assumption that CDCL UIP analysis
relies on. This document is the formal correctness argument behind the fix
that turns each `¬l_i` into the unique decision at its own DL.

## 1. Setup and notation

* `F` is a propositional formula in CNF — a set of clauses.
* `C ∈ F` is the clause we branch on. Write `C = l_1 ∨ l_2 ∨ ... ∨ l_k`.
* `F\{C}` denotes F with C dropped.
* `removed_clauses_` (in code) is a multiset of clause-IDs that have been
  marked "absent from BCP's view" via `markClauseRemoved`. It is **not
  monotonic** along a single root-to-leaf path: it grows on every
  `branchOnClause` entry and shrinks on exit. At any moment in the search,
  the "active formula" is `F \ removed_clauses_`.
* `S_learn(D)` denotes the value of `removed_clauses_` at the moment a
  learned clause `D` was added to the formula.
* `provenance(D)` denotes the multiset of clause-IDs that participated as
  resolution antecedents in the UIP derivation of `D` (transitively
  through any learned-clause antecedents until original clauses).

## 2. Inclusion-exclusion and the two sub-problems

The identity

    #SAT(F) = #SAT(F\{C}) − #SAT(F\{C} ∧ ¬C)

is immediate: a model of `F\{C}` either satisfies `C` (in which case it is a
model of `F`) or it satisfies `¬C` (in which case it does not satisfy `F`).
The two sub-problems are:

* **Drop arm.** Count `#SAT(F\{C})`. Implemented by `markClauseRemoved(C)`
  and recursing. No additional trail constraints.
* **Negate arm.** Count `#SAT(F\{C} ∧ ¬C)`. The constraint `¬C` is
  `⋀_{i=1..k} ¬l_i`. Implemented by `markClauseRemoved(C)` (so BCP does
  not enforce C — which would otherwise be falsified by the `¬l_i`
  assertions and abort the branch) **and** asserting each `¬l_i` on the
  trail.

Note that `markClauseRemoved(C)` is required in **both** arms; if we left
C in BCP's view in the negate arm, BCP would observe C as falsified
(all lits F_TRI) and immediately conflict, returning 0 instead of the
desired `#SAT(F\{C} ∧ ¬C)`. The "drop" of C from BCP's view in the
negate arm is bookkeeping — it does not signify that C is dropped from
the formula being counted; rather, the formula being counted is
`F\{C} ∪ {¬l_1, ¬l_2, ..., ¬l_k}` which is equivalent to
`F\{C} ∧ ¬C`.

## 3. Standard UIP / resolution correctness

The CDCL learning machinery we rely on:

**Lemma (UIP / resolution under unit assumptions).**
Let `Γ` be a clause set and `A_1, A_2, …, A_n` be unit literal
assumptions. Suppose `Γ ∪ {A_1, …, A_n}` is UNSAT (BCP, restricted to
clauses in `Γ`, derives a conflict from the trail formed by the
assumptions). Let `D_uip` be the 1-UIP clause produced by standard
backward UIP analysis where each `A_i` is set as the unique decision at
its own decision level. Then

    Γ  ⊨  D_uip ∨ ¬A_1 ∨ ¬A_2 ∨ … ∨ ¬A_n.

Equivalently, the clause

    D  :=  D_uip ∨ ¬A_1 ∨ ¬A_2 ∨ … ∨ ¬A_n

is a logical consequence of `Γ` alone, with no assumption-dependence
remaining.

**Sketch.** UIP performs sound resolution starting at the conflict
clause `CL_0` and resolving along the antecedent of each top-decision-
level trail literal. Each `A_i`, being a decision, has no antecedent;
when UIP reaches `A_i` while reducing the in-flight clause to a single
literal at the current DL, it terminates resolution and KEEPS the
asserting literal in the output. The lower-DL `A_j` (`j < i`) keep
appearing in the in-flight clause as literals (they were never resolved
away), and on output they appear as `¬A_j`. The resulting `D` is a
sound resolvent of `Γ`-clauses (plus instances of `A_i` treated as
assumptions, which surface as their negations). □

A critical invariant of the lemma is **one decision per DL**: the
backward walk reaches one assumption at a time, terminates at that
decision, and adds its negation to the clause. The lemma does not hold
verbatim if multiple `A_i` sit at the same DL — see §5.

## 4. Application to `branchOnClause` negate arm

Set `Γ := F\{C}` and `A_i := ¬l_i`. The negate arm executes
`markClauseRemoved(C)` then asserts `¬l_1, ¬l_2, ..., ¬l_k` on the
trail. If a conflict arises inside the recursion under these
assumptions, UIP analysis produces some `D_uip`.

By the Lemma (with each `¬l_i` at its own DL):

    F\{C}  ⊨  D_uip ∨ ¬(¬l_1) ∨ ¬(¬l_2) ∨ … ∨ ¬(¬l_k)
           =  D_uip ∨ l_1 ∨ l_2 ∨ … ∨ l_k.

Define

    D  :=  D_uip ∨ l_1 ∨ l_2 ∨ … ∨ l_k.

`D` is the clause that should be stored as the result of UIP analysis
in the negate arm.

### 4.1 D is sound for F

**Theorem.**  `F ⊨ D`.

**Proof.** Let `M` be any model of `F`. Since `C ∈ F`, `M ⊨ C`, so at
least one `l_i` is True under `M`. That `l_i` appears as a literal in
`D`, so `M ⊨ D`. ∎

### 4.2 `C ∉ provenance(D)`

**Theorem.**  C is not an antecedent in the resolution chain that
produces `D`.

**Proof.** The resolution chain for `D_uip` only uses clauses that
BCP could see during the negate arm — i.e., clauses in
`(F\{C}) ∪ (in-scope learned clauses)`. Because `markClauseRemoved(C)`
was called before any BCP step, `C` is filtered out of BCP's watched-
clause traversal ([solver.cpp:1022](../src/solver.cpp#L1022)). `C` can
therefore never be the conflict-trigger clause, and no trail literal is
ever set with `C` as antecedent. Hence `C ∉ provenance(D_uip)`.

The literals `l_1, …, l_k` are added to `D` not via resolution against
`C`, but via the UIP machinery's standard "decision-negation" step
applied to the trail decisions `¬l_i`. So `C` does not appear in
`provenance(D)`. ∎

### 4.3 Provenance-based soundness check is correct

**Theorem (Provenance soundness).** Let `S_use` be the current
`removed_clauses_`. Then `D` is sound for `F \ S_use` iff
`S_use ∩ provenance(D) = ∅`.

**Proof.**  (⇐) If no clause in `provenance(D)` is in `S_use`, every
antecedent of `D`'s resolution chain is currently in the active
formula `F \ S_use`. Resolution is sound, so `F \ S_use ⊨ D`.

(⇒) If some `C' ∈ S_use ∩ provenance(D)`, then `C'` was used in
`D`'s derivation but is currently removed. `D` cannot be derived from
`(F \ S_use)`-clauses alone; the derivation chain depends on `C'`,
and `D` is not provably entailed by `F \ S_use`. ∎

Note that this is **strictly more permissive** than the legacy scope-
subset check (`S_use ⊆ S_learn(D)`). The legacy check pretends every
clause not removed at learn time was a derivation antecedent. The
provenance check uses the actual derivation chain.

In particular: for a learned clause `D` from the negate arm,
`C ∉ provenance(D)`, so the provenance check returns TRUE at any
state where the rest of D's antecedent chain is intact — **regardless
of whether C is removed**. The learned clause from branch 2 can be
soundly reused even in branches where C is back in the formula.

## 5. Why the single-DL implementation breaks the Lemma

In sharpSAT's earlier implementation, `branchOnClause` pushed one
`StackLevel` for the entire negate arm and then called
`setLiteralIfFree(¬l_i)` for each i — placing all decisions at the
SAME decision level. The UIP loop ([solver.cpp:2068-2087](../src/solver.cpp#L2068-L2087))
is structured around the invariant

    "exactly one literal at the current DL has no antecedent,
     and that literal is the asserting literal".

With `k > 1` decisions at one DL, the loop reaches a state where
`lits_at_current_dl > 1` AND the current lit has no antecedent. In
that branch, the code falls through:

    if (lits_at_current_dl-- == 1) {
        if (!hasAntecedent(curr_lit)) break;     // 1-UIP terminator
    }
    assert(hasAntecedent(curr_lit));             // ← multi-decision: ABORT in debug
    if (getAntecedent(curr_lit).isAClause()) { … }  // multi-decision in release: skip

In **Release builds** (no NDEBUG), the assert is compiled out. The
subsequent `isAClause()` branch is not taken (the lit has no
antecedent), and the loop continues without including the lit's
negation in the in-flight clause. The literal is silently discarded.

The output is `D_uip` plus the negation of **only the last-encountered
multi-decision lit**, missing the negations of the other `k − 1`
decisions. Call this buggy output `D_bug`.

### 5.1 D_bug is unsound for F

**Theorem.**  `F ⊭ D_bug` in general.

**Proof.** Suppose UIP correctly recovers only `l_i` for some single
index `i`, missing all other `l_j` (j ≠ i). Then

    D_bug  =  D_uip ∨ l_i.

Take a model `M` of `F` in which `l_i` is False, all other `l_j` are
False, and `l_j_0` is True for exactly one `j_0 ≠ i`. `M ⊨ C` (since
`l_j_0` is True), so `M ⊨ F`. Under `M`:

* `l_i` is False, so the `l_i` disjunct of `D_bug` is False.
* `D_uip` was derived under the **assumption** that all `¬l_j` hold
  for all `j`. Under `M`, `¬l_j_0` is False, so the assumption is
  violated. The Lemma does not guarantee `M ⊨ D_uip`. In fact,
  `D_uip` may be False under `M`.

If `D_uip` happens to be False under `M`, `D_bug` is False under `M`
even though `M ⊨ F`. Hence `F ⊭ D_bug`. ∎

In particular, any subsequent BCP step that propagates via `D_bug`
may force literals that exclude models of `F`. That is the empirical
mechanism behind the wrong count observed on `t1_011 + -derivCacheBias 1 + line-1346-removed`.

## 6. The fix

Push one fresh `StackLevel` per active `¬l_i` decision in the negate
arm and run BCP between each. The change satisfies the Lemma's
one-decision-per-DL precondition. Concretely (pseudo-code):

```cpp
if (negate_literals) {
    for (lit l_i in clause C) {
        if (isSatisfied(l_i)) { conflict = true; break; }
        if (!isActive(l_i)) continue;          // ¬l_i already True via prior BCP
        push new StackLevel;
        setLiteralIfFree(¬l_i);                // unique decision at this DL
        if (!BCP(...)) { conflict = true; break; }
    }
}
```

Lits forced by BCP from a prior `¬l_j` do not need their own decision
level (they are propagated, not decided). The loop's `isSatisfied` /
`isActive` checks skip those naturally.

With this structure:

* Each `¬l_i` that survives BCP propagation becomes the unique
  decision at its own DL.
* UIP backward-walking encounters each `¬l_i` one at a time,
  terminates resolution at that DL, and adds `l_i` to the output.
* `D = D_uip ∨ l_1 ∨ … ∨ l_k` is constructed correctly.

By §4.1, the resulting `D` is sound for `F`. By §4.2,
`C ∉ provenance(D)`. By §4.3, the provenance-based soundness check
correctly admits `D` in any state where its (other) antecedents are
intact.

## 7. Why the legacy scope check looks sound but isn't, in practice

The legacy check `current_removed ⊆ S_learn(D)` is **mathematically
sound for clauses whose derivation chain consists only of clauses
present in `F \ S_learn(D)`** — which is true for any UIP clause
produced under the one-decision-per-DL discipline. Under that
discipline the legacy check is correct (over-conservative but never
unsound).

The bug observed was an artefact of UIP producing structurally wrong
clauses in the multi-decision-DL regime. The legacy check, asked to
classify `D_bug`, has no way to detect that `D_bug` is structurally
deficient — it only checks scope, not derivation. So `D_bug` slips
through legacy's check (and provenance's, for the same reason: the
recorded antecedent chain for `D_bug` is also incomplete). The bug
manifests when `D_bug` is later used in BCP.

Once the per-decision-DL fix is in place, UIP produces well-formed
clauses, and both legacy and provenance checks are sound. Provenance
remains the right design — it captures the correct soundness criterion
in closed form and is strictly more permissive than legacy — but
either check is mathematically valid once the underlying UIP
implementation respects its precondition.

## 8. Summary

| Concept                              | Drop arm `F\{C}`     | Negate arm `F\{C} ∧ ¬C`          |
| ------------------------------------ | -------------------- | -------------------------------- |
| `markClauseRemoved(C)` called?       | yes                  | yes                              |
| Lits added to trail                  | none                 | `¬l_1, …, ¬l_k`                  |
| Required DL discipline               | (no constraint)      | one decision per `¬l_i`          |
| `C ∈ provenance(D)` for D learned?   | no (C absent in BCP) | no (C absent in BCP)             |
| Form of learned `D`                  | `D_uip`              | `D_uip ∨ l_1 ∨ … ∨ l_k`          |
| `F ⊨ D`?                             | yes                  | yes (via `M ⊨ C ⇒ some l_i ⊨ D`) |

The fix in §6 ensures the negate arm's "one decision per DL" hypothesis
of the Lemma holds, restoring the standard correctness of UIP and
making the provenance-based soundness check tight.
