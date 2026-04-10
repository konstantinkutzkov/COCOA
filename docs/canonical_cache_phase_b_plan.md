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

**Options (in order of complexity):**

A. **Deterministic tie-breaking:** Within each collision block, order
   variables by some arbitrary but fixed rule. This gives one canonical
   form per formula. May miss some isomorphisms but never produces
   false positives.

B. **Multiple candidate keys:** For each collision block of size k,
   store up to k! / (symmetry) candidate keys. On lookup, check all
   candidates. Increases cache hits but multiplies storage and lookup
   cost. Only practical for very small collision blocks (2-3 variables).

C. **Exact canonicalization (nauty/bliss):** Use a graph isomorphism
   tool to compute a true canonical form. Guarantees maximum cache
   reuse but adds a dependency and significant computational cost.

**Recommendation:** Start with option A. Measure collision frequency
on real instances to determine if B or C is worthwhile.

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
Step 6 is an optimization that depends on empirical data.

---

## Measurement Plan

At each step, measure on competition instances:

1. **Correctness:** 200 stress tests with `-cb 2` must pass.
2. **Cache hit rate:** Compare content_cache hits/misses/stores.
3. **Solve time:** Compare with Phase A baseline on test_cb_perf,
   test183, and larger instances.
4. **Collision frequency:** After Steps 4-5, count how many variables
   remain in collision blocks and their sizes.

---

## Files to Modify

All changes are in `src/canonical_key.cpp`. The cache infrastructure
(`content_cache.h`, `component_management.h`) does not need changes —
the canonical key is the only interface between the solver and the
cache.
