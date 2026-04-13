# Phase B: Canonical Cache with Isomorphism Detection

## Overview

Phase A (done) implemented a content-based cache using raw variable
IDs. Phase B adds normalization steps that detect isomorphic
sub-formulas, increasing cache hit rates.

The steps are ordered from simplest to most complex. Each step is
independently useful and should be tested before proceeding to the
next.

---

## Step 1: Singleton Anonymization

**Goal:** Replace singleton variables with a generic marker.

A singleton variable appears exactly once in the formula. Its identity
and polarity don't affect the model count (see docs/canonical_caching.md
for the proof). Replacing singletons with marker `s` merges formulas
that differ only in which fresh variables occupy singleton positions.

**Implementation:**

1. In `buildCanonicalKey`, after collecting all clauses (long + binary),
   count total occurrences of each variable across all clauses.
2. Variables with exactly 1 occurrence are singletons.
3. Replace their literals with `SINGLETON_MARKER` (0) in the canonical
   clauses.
4. Remove singletons from the variable list in the key.

**Testing:**

- Construct two formulas that differ only in singleton variable names.
  Verify they produce the same key and get a cache hit.
- Run 200 stress tests to verify no false positives.

**Expected impact:** Modest. Singletons are common in sub-formulas
after BCP but the same singleton pattern tends to recur with the same
variable names.

---

## Step 2: Polarity Normalization

**Goal:** For each non-singleton variable, choose a canonical polarity
orientation.

Flipping a variable globally (positive ↔ negative everywhere) preserves
the model count. Two formulas related by variable flips are equivalent.

**Implementation:**

1. For each non-singleton variable, count positive and negative
   occurrences across all clauses in the component.
2. Orient the variable so that positive occurrences >= negative
   occurrences. If tied, keep as-is (or use a secondary rule such as
   comparing the sorted list of clause lengths for positive vs negative
   occurrences).
3. When a variable is flipped, negate all its literal occurrences in
   all clauses.
4. This must happen AFTER singleton anonymization (singletons don't
   need polarity normalization).

**Testing:**

- Construct two formulas related by flipping a variable. Verify same
  key.
- Run 200 stress tests.

**Expected impact:** Moderate. Variable polarity choices are
semi-arbitrary in many encodings. This catches cases where the same
structural constraint appears with opposite polarity conventions.

---

## Step 3: Remove Variable List from Key

**Goal:** The variable list currently in the key is redundant if the
clause content fully captures the variable set.

After Steps 1-2, every non-singleton variable appears in at least one
clause (by definition — it has >= 2 occurrences). Singletons are
anonymized and their count per clause is captured by the number of
`SINGLETON_MARKER` entries. So the clause content alone determines the
variable set.

**Implementation:**

1. Remove the `[-999999, v1, v2, ...]` entry from the key.
2. Verify correctness with stress tests.

**Risk:** If any variable appears in the component but in NO clause
(neither long nor binary), removing the variable list would lose
information. This should not happen (such variables are isolated and
handled separately), but must be verified.

---

## Step 4: WL-Based Variable Renaming (Iteration 0)

**Goal:** Assign canonical variable labels based on structural
properties, so isomorphic formulas get the same labels.

**Implementation:**

1. For each non-singleton variable, compute an initial signature:
   - Sorted list of clause signatures for each clause the variable
     appears in.
   - Each clause signature: (clause length, number of positive
     non-singleton lits, number of negative non-singleton lits,
     number of singletons, polarity of this variable in the clause
     after orientation).
2. Sort variables by their signatures.
3. If all signatures are unique: assign canonical IDs 1, 2, 3, ... in
   signature order. This is a bijective mapping. Rewrite all literals
   using canonical IDs.
4. If some signatures collide: variables with the same signature are
   structurally indistinguishable at this level. Apply a deterministic
   tie-breaking rule (e.g., keep original order within the collision
   block).

**Testing:**

- Construct isomorphic formulas with different variable numbering.
  Verify same canonical key.
- Run 200 stress tests.
- Measure cache hit rate increase on real instances.

**Expected impact:** Significant. Many sub-formulas encountered
during solving have the same structure with different variable names.

---

## Step 5: WL Refinement (Iteration 1)

**Goal:** Refine variable labels using neighbor information, capturing
2-hop structural patterns.

**Implementation:**

1. After Step 4's initial labeling, for each variable, aggregate the
   multiset of labels of its neighbors (variables that co-occur in at
   least one clause).
2. Combine with the variable's own label to produce a refined label.
3. Re-sort and re-assign canonical IDs.
4. At most 1 additional iteration (total 2 including iteration 0).

**Testing:**

- Construct formulas where iteration 0 doesn't distinguish variables
  but iteration 1 does. Verify correct distinction.
- Run stress tests.
- Measure incremental cache hit improvement over Step 4.

**Expected impact:** Incremental over Step 4. Most practical
isomorphisms are captured by iteration 0.

---

## Step 6: Collision Handling

**Goal:** Handle variables that remain structurally indistinguishable
after WL refinement.

The equivalence relation `~` allows both variable renaming AND global
complementation (polarity flip). After Steps 2 and 4-5, most
variables have a unique label and a deterministic orientation. But
some variables may remain in "collision blocks" (same WL label) and
be "orientation-ambiguous" (equal positive/negative profiles, so the
polarity rule from Step 2 can't decide).

**Approach: Lexicographic minimum over within-block candidates.**

This is space-efficient (one key per formula) and exact within the
WL equivalence classes. See `docs/canonical_caching.md` for the full
description and examples.

1. After WL refinement, identify collision blocks B_1, ..., B_r
   (groups of variables sharing the same final WL label).
2. All variables NOT in any collision block are "anchored" — their
   canonical IDs and orientations are fixed.
3. For each collision block B_i:
   a. Enumerate all permutations of B_i (|B_i|! choices).
   b. For orientation-ambiguous variables in B_i (those where
      positive occurrences == negative occurrences after Step 2's
      rule), enumerate flip choices (2^|A_i| where A_i is the set
      of ambiguous variables in B_i).
4. For each combination of within-block permutations and flip masks,
   build the normalized clause multiset.
5. Take the lexicographically minimum result as THE canonical key.
6. Store only this minimum key.

The total number of candidates is:

    |T(F)| = ∏_i |B_i|! · 2^|A_i|

This is manageable when collision blocks are small (typical: 2-3
variables). For larger blocks, fall back to a bounded random sample
or deterministic tie-breaking (heuristic route).

**Example: permutations only.**

    F = (u ∨ x)(u ∨ y)(¬u ∨ p)(¬u ∨ q)(x ∨ p)(y ∨ q)

WL anchors u, leaves blocks {x,y} and {p,q}. Candidates: 2!·2! = 4.
Two distinct normalized forms arise; take the lex minimum.

**Example: flips needed.**

    F_xor  = (x1 ∨ x2)(¬x1 ∨ ¬x2)
    F_xnor = (x1 ∨ ¬x2)(¬x1 ∨ x2)

These are equivalent under x2 → ¬x2. WL gives one collision block
{x1, x2}, both orientation-ambiguous. Permutations alone produce
different minimums for F_xor and F_xnor. Including flip masks
(2^2 = 4 flip choices × 2! permutations = 8 candidates) ensures
both produce the same minimum.

**Implementation:**

1. After WL refinement, group variables by final label.
2. For blocks of size 1: anchored, no enumeration.
3. For blocks of size ≤ MAX_ENUM (e.g., 4):
   enumerate all permutations × flip masks, take lex minimum.
4. For blocks of size > MAX_ENUM:
   apply deterministic tie-breaking (arbitrary but fixed ordering
   within the block). This is the heuristic fallback — may miss
   some isomorphisms but never produces false positives.

**Testing:**

- Construct isomorphic formulas that require permutation to match.
- Construct isomorphic formulas that require flips to match (xor/xnor).
- Verify same canonical key in both cases.
- Run 200 stress tests.
- Measure collision block sizes on real instances.

**Expected impact:** Depends on collision block frequency and size.
Must measure empirically before investing in larger block handling.

---

## Implementation Order and Dependencies

```
Step 1 (singletons) ──→ Step 2 (polarity) ──→ Step 3 (remove var list)
                                                       │
                                                       ▼
                                            Step 4 (WL iteration 0)
                                                       │
                                                       ▼
                                            Step 5 (WL iteration 1)
                                                       │
                                                       ▼
                                            Step 6 (collision handling)
```

Each step can be tested and deployed independently. Steps 1-3 are
simple and low-risk. Steps 4-5 are the core isomorphism detection.
Step 6 combines permutation enumeration with flip enumeration for
orientation-ambiguous variables — its value depends on empirical
collision block sizes.

Note: Step 2 (polarity normalization) and Step 6 (flip enumeration)
are related but distinct. Step 2 fixes orientation deterministically
for MOST variables (those with unequal positive/negative counts).
Step 6 handles the remaining ambiguous variables by enumeration.

---

## Measurement Plan

At each step, measure on competition instances:

1. **Correctness:** 200 stress tests with `-cb 2` must pass.
2. **Cache hit rate:** Compare content_cache hits/misses/stores.
3. **Solve time:** Compare with Phase A baseline on test_cb_perf,
   test183, and larger instances.
4. **Collision frequency (Steps 4-6):** Count how many variables
   remain in collision blocks and their sizes. Also count how many
   are orientation-ambiguous (equal pos/neg counts).
5. **Enumeration cost (Step 6):** Measure time spent enumerating
   within-block candidates. If blocks are typically size 2-3, cost
   is negligible. If larger blocks appear, measure the fallback
   frequency.

---

## Files to Modify

All changes are in `src/canonical_key.cpp`. The cache infrastructure
(`content_cache.h`, `component_management.h`) does not need changes —
the canonical key is the only interface between the solver and the
cache.
