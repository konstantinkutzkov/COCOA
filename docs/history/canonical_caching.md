# Count-Preserving Canonical Caching for #SAT Solvers

## Motivation

Component caching is essential for the performance of #SAT solvers.
When the solver decomposes a formula into independent sub-formulas
(components), it can cache the model count of each component and reuse
it if the same component is encountered again during the search.

Existing solvers usually key the cache by internal variable and clause
IDs. This misses reuse opportunities:

- two structurally identical components with different IDs get
  different cache keys;
- the same structural component can reappear after different branching
  histories;
- dynamic formula modifications (e.g. clause branching, BCP, learned
  clauses) make ID-based keys solver-specific rather than content-based.

The goal of this note is to describe a formally correct way to cache
components by **count-preserving structure** rather than by internal
IDs.

The key idea is:

- if two component formulas are equivalent under a safe
  count-preserving normalization, then they have the same model count;
- therefore they may safely share a cache entry;
- a canonicalization or normalization procedure is only a mechanism for
  discovering this equivalence efficiently.

---

## Formula Equivalence Used for Caching

Let `F` be a CNF formula viewed as a multiset of clauses. Repeated
clauses are allowed; cache keys must preserve multiplicity.

A variable is a **singleton** if it appears in exactly one literal
occurrence in `F`. Equivalently, it appears in exactly one clause and
only once in that clause.

We define an equivalence relation `~` on CNF formulas as follows.

Two formulas `F` and `G` satisfy `F ~ G` if there exists:

1. a bijection `pi` from the non-singleton variables of `F` to the
   non-singleton variables of `G`;
2. for each non-singleton variable `u` of `F`, a sign bit
   `sigma(u) in {+1, -1}` indicating whether `u` is mapped directly or
   complemented;
3. for each clause of `F`, a matching clause of `G`;
4. such that after applying `pi` to the non-singleton variables and
   flipping the polarity of each occurrence of `u` when `sigma(u) = -1`,
   and replacing singleton literals by a generic singleton marker `s`,
   the two clause multisets become
   identical.

Informally:

- non-singleton variables may be renamed globally by a bijection;
- each non-singleton variable may also be complemented globally;
- singleton variables are anonymous and only contribute the fact that a
  clause contains singleton positions;
- clause multiplicities must still match.

This is the right notion for safe cache sharing.

---

## Why Singleton Variables Can Be Anonymized

Suppose variable `x` is singleton in `F`, so it occurs in exactly one
clause `C`.

Then:

- `x` does not connect two different clauses;
- changing the name of `x` clearly does not affect satisfiability or
  model count;
- flipping the polarity of `x` inside `C` also does not change the
  number of satisfying assignments, because the map `x -> not x`
  defines a bijection between satisfying assignments before and after
  the flip.

Therefore a singleton literal contributes only:

- that there is a fresh degree-1 variable attached to this clause, and
- whether there is one singleton position or several.

So replacing singleton literals by a generic marker `s` is sound, as
long as the number of singleton positions in each clause is preserved.

Example:

- `(a v b v x)` with singleton `x`
- `(a v b v not y)` with singleton `y`

are equivalent for counting purposes once singleton identities are
forgotten.

---

## Correctness Statement

**Theorem.** If `F ~ G`, then `#SAT(F) = #SAT(G)`.

### Proof

Fix a witness of `F ~ G`, i.e.:

- a bijection `pi` on non-singleton variables, and
- a choice, for each non-singleton variable, of whether it is preserved
  or complemented globally, and
- a clause-by-clause matching after replacing singleton literals by the
  generic marker `s`.

We construct a bijection between satisfying assignments of `F` and
satisfying assignments of `G`.

### Non-singleton variables

For every non-singleton variable `u` of `F`, define the corresponding
variable of `G` to be `pi(u)`.

- if `sigma(u) = +1`, assign `pi(u)` the same truth value as `u`;
- if `sigma(u) = -1`, assign `pi(u)` the opposite truth value.

This is a bijection on assignments to the non-singleton variables.

### Singleton variables

Consider a clause `C` of `F` and its matched clause `C'` of `G`.
Their non-singleton literals correspond under `pi`, and they contain
the same number of singleton positions.

For each singleton variable appearing in `C`, choose the corresponding
singleton variable in `C'` arbitrarily. Since singleton variables are
local to their clauses and have no interactions outside the clause,
this matching is independent across clauses.

Now define the singleton assignments clause-by-clause:

- if the non-singleton part of `C` already satisfies the clause, then
  the singleton values may be chosen arbitrarily on both sides;
- otherwise satisfaction depends only on the singleton positions, and
  since `C` and `C'` have the same number of singleton positions, there
  is a bijection between satisfying singleton assignments of `C` and of
  `C'`.

Thus every satisfying assignment of `F` induces a unique satisfying
assignment of `G`, and vice versa.

Therefore the satisfying assignments of `F` and `G` are in bijection,
so `#SAT(F) = #SAT(G)`.

---

## Exact Canonicalization vs Heuristic Canonicalization

There are two different goals:

1. **Exact canonicalization**
   - produce the same unique representation for all formulas in the
     same equivalence class `~`;
   - this gives the strongest possible cache reuse.

2. **Heuristic canonicalization**
   - fix a deterministic procedure that maps each formula to a single
     representative;
   - this representative need not be canonical for the full equivalence
     relation `~`;
   - it is still useful if it merges many formulas in the same
     equivalence class and never merges formulas with different model
     counts.

This distinction is important:

- correctness requires only that equal keys imply equal counts;
- cache efficiency benefits from keys that identify more equivalent
  formulas.

The practical procedure described below should therefore be understood
as a **heuristic canonical form**:

- once the labeling and tie-breaking rules are fixed, every formula gets
  one deterministic key;
- this key is canonical for the heuristic procedure;
- but it may still be coarser or finer than the full equivalence
  relation `~`.

In particular, it may miss some valid cache merges (false negatives),
which only affects performance, not correctness.

---

## Practical Labeling Procedure

We compute labels using a refinement procedure inspired by
Weisfeiler-Leman (WL).

For implementation, there are two reasonable graph choices:

- the **incidence graph**: bipartite graph with variable nodes and
  clause nodes;
- the **primal graph**: graph on variables where two variables are
  adjacent if they co-occur in a clause.

The incidence graph retains more information and is conceptually
cleaner for exact canonicalization. The primal graph may be a useful
heuristic shortcut.

### Initial labels

Each non-singleton variable receives an initial structural signature
such as:

- multiset of incident clause lengths;
- for each incident clause:
  - clause length,
  - number of positive non-singleton literals,
  - number of negative non-singleton literals,
  - number of singleton positions,
  - polarity of the variable in that clause.

Clause nodes, if the incidence graph is used, receive initial labels
based on:

- clause length;
- counts of positive/negative non-singletons;
- number of singleton positions.

### Refinement

At each WL iteration, replace each node label by a hash or tuple built
from:

- its current label, and
- the multiset of labels of its neighbors.

This may be repeated for a fixed number of rounds or until labels
stabilize.

### If all labels become unique

If all non-singleton variables become uniquely identified, then they
can be renamed in sorted-label order and this yields a deterministic
normal form.

### If collisions remain

If several variables still share the same refined label, then WL has
not fully distinguished them. At this point there are two options:

1. **Exact route**
   - for **small collision blocks**, an exact and space-efficient option is:
     - keep all WL-unique variables fixed (anchored);
     - for each remaining collision block `B` (variables with the same final WL label),
       enumerate all **within-block permutations** of `B`;
     - additionally, if the caching equivalence `~` allows *global complementation* of
       non-singleton variables (as defined above), then for variables in `B` whose
       orientation cannot be fixed deterministically (e.g., equal positive/negative
       profile), also enumerate both **flip** choices;
       - in the worst case this is all flip masks on `B` (a factor `2^{|B|}`);
       - in practice, deterministic orientation rules can often fix most variables,
         and only *orientation-ambiguous* variables require enumeration;
     - for each choice of within-block permutations and (when needed) flip masks,
       build the normalized clause multiset;
     - take the **lexicographically minimum** normalized multiset as the canonical key;
     - store only this minimum key (e.g., hashed), not all candidates.
     
     The number of candidates is
     
     \[
     |T(F)| \;=\; \prod_{i} |B_i|! \cdot 2^{|A_i|}
     \]
     
     where `B_i` are the WL collision blocks and `A_i ⊆ B_i` are the variables in block `i`
     whose orientation remains ambiguous after deterministic rules (worst case `A_i = B_i`).
     
     which can be much smaller than `k!` if WL splits `k` unresolved variables into several
     smaller collision blocks.
     
     This is exact for the fixed refinement procedure because WL labels are
     isomorphism-invariant: isomorphic formulas induce the same multiset of collision
     blocks (up to renaming), and enumerating all within-block permutations and allowed
     flip choices enumerates all bijections consistent with these labels and with `~`.
   - run an additional canonical labeling search
     (individualization/refinement, nauty/bliss-style, etc.);
   - this can produce a true canonical form.

2. **Heuristic route**
   - apply a deterministic tie-breaking rule inside each collision
     block; or
   - enumerate a bounded set of candidate tie-breakings if multi-key
     lookup is desired.

#### Example: exact minimum over WL collision blocks

Let

\[
F = (u\vee x)\wedge(u\vee y)\wedge(\neg u\vee p)\wedge(\neg u\vee q)\wedge(x\vee p)\wedge(y\vee q),
\]

and let `G` be the same structure with renamed variables (an isomorphic copy):

\[
G = (u'\vee y')\wedge(u'\vee x')\wedge(\neg u'\vee q')\wedge(\neg u'\vee p')\wedge(y'\vee q')\wedge(x'\vee p').
\]

WL refinement (on the incidence graph with polarity labels) typically:

- uniquely identifies `u` (and `u'`) as *anchored*;
- leaves two collision blocks of size 2: `{x,y}` and `{p,q}` (and similarly `{x',y'}` and `{p',q'}`).

So the exact enumeration has only `2! * 2! = 4` candidates, rather than `4! = 24`.
If we rename `u -> 1` and enumerate the two swaps inside each block, two distinct normalized
clause multisets appear (after sorting literals inside clauses and sorting clauses):

- `R1 = [(-1,4), (-1,5), (1,2), (1,3), (2,4), (3,5)]`
- `R2 = [(-1,4), (-1,5), (1,2), (1,3), (2,5), (3,4)]`

For `F`, these arise from different within-block permutations (e.g., swapping `{p,q}` changes which
of `(2,4)`/`(3,5)` vs `(2,5)`/`(3,4)` appears). For `G`, enumerating its collision-block permutations
produces the **same candidate set** `{R1, R2}`. Therefore:

- `T(F) = T(G) = {R1, R2}`
- and thus `min(T(F)) = min(T(G))`

This is the sense in which “taking the minimum over all within-block permutations” yields a
guaranteed cache hit for isomorphic formulas, as long as the candidate set `T(·)` is fully enumerated.

#### Example: why sign flips may be required

The counting equivalence `~` allows complementing non-singleton variables globally. For example:

- `F_xor  = (x1 v x2) ∧ (!x1 v !x2)`
- `F_xnor = (x1 v !x2) ∧ (!x1 v x2)`

These are equivalent under the flip `x2 -> !x2`. WL refinement typically leaves `{x1, x2}` in a
single collision block and both variables are orientation-ambiguous. Enumerating only permutations
may not merge these formulas, but enumerating the relevant flip choices ensures the candidate sets
match and therefore `min(T(F_xor)) = min(T(F_xnor))`.

The heuristic route is still correct if used carefully:

- every stored key corresponds to a concrete normalized formula;
- a cache hit is accepted only on exact key equality;
- this may cause false negatives (missed reuse), but not false
  positives.

For implementation simplicity, a deterministic tie-breaking rule is the
cleanest option because it keeps the cache interface unchanged: one
formula, one key.

---

## Cache Key Construction

Once the variables are normalized, construct the normalized clause
multiset:

1. replace every singleton literal by the marker `s`;
2. choose a canonical global orientation for each non-singleton
   variable;
3. rewrite every non-singleton literal using its normalized variable ID
   and its polarity after canonical orientation;
4. sort literals inside each clause into a fixed order;
5. sort the clause multiset lexicographically;
6. preserve repeated clauses.

A simple first orientation rule is:

- flip a variable if its number of positive occurrences is smaller than
  its number of negative occurrences;
- otherwise keep it as is.

This already merges formulas that differ only by global sign choices of
variables.

For ties, a stronger deterministic rule is preferable, for example:

- compare the positive-occurrence profile and negative-occurrence
  profile lexicographically, where a profile records the multiset of
  incident clause signatures;
- orient the variable so that the lexicographically larger profile is
  treated as positive.

The key requirement is not that the rule be perfect, but that it be
deterministic and count-preserving.

However, for **fully symmetric / orientation-ambiguous** variables (e.g., equal positive and
negative profiles, and tied refined labels), no deterministic local rule can reliably merge all
equivalent formulas that differ by complementing such variables. In the **exact route**, this is
handled by enumerating the remaining flip choices inside WL collision blocks. In the **heuristic
route**, one may try a bounded number of flip masks / tie-breakings and accept false negatives.

The resulting representation may be serialized directly or hashed.

Because the singleton marker forgets both identity and polarity, it is
deliberately chosen to maximize safe merges. Using separate markers for
positive and negative singleton literals would preserve more syntax than
needed for model counting and would create avoidable cache misses.

For correctness:

- the final cache key must be based on the exact normalized clause
  multiset;
- hashes alone are not enough unless collisions are resolved by
  comparing the full normalized structure.

---

## Integration with the Solver

The caching scheme operates independently of the solver's internal
formula modifications (variable branching, clause branching, BCP,
clause learning). The solver maintains the formula state through its
own data structures. When a component is identified for caching:

1. **Extract**
   - read the current component formula:
     - all active clauses belonging to the component,
     - all active literals in those clauses,
     - including binary clauses and learned clauses if they are part of
       the solver state being counted.

2. **Normalize / canonicalize**
   - compute singleton positions;
   - compute the normalized form or canonical form.

3. **Lookup**
   - hash the normalized representation and probe the cache;
   - on collision, compare exact normalized representations.

4. **Store**
   - if there is no hit, solve the component and store the result under
     the normalized key.

This separation between solver state and cache key ensures correctness
regardless of how the solver modifies the internal formula
representation.

---

## Practical Optimizations

The canonicalization itself is only part of the story. The following
optimizations can make the scheme substantially more effective in
practice.

### 1. Two-level cache lookup

First try the ordinary exact-ID cache key. Only if that misses, compute
the canonical key.

This gives:

- zero extra overhead on easy exact hits;
- canonicalization cost only when it may actually help.

### 2. Cheap invariants before WL

Before any refinement, compute inexpensive fingerprints such as:

- number of clauses;
- number of non-singleton variables;
- histogram of clause lengths;
- histogram of singleton counts per clause;
- degree multiset of non-singleton variables;
- counts of positive and negative literal occurrences.

If these differ, the formulas cannot share a canonical key.

### 3. Canonicalize only above a payoff threshold

Canonicalization has overhead, so it should be enabled only when it is
likely to pay off, for example:

- only for components with at least a minimum number of clauses or
  literal occurrences;
- only for components that survived basic exact-ID cache lookup;
- optionally only at shallow search depths.

### 4. Clause-internal normalization first

Before running WL, normalize each clause locally:

- replace singleton literals by `s`;
- sort literals inside the clause;
- sort clauses.

This alone may already collapse many superficially different components.

### 5. Refine on the incidence graph, not only the primal graph

The incidence graph retains clause-node information explicitly, which is
exactly the information that matters for CNF structure. Using incidence
graph refinement is likely to produce more cache merges than a pure
primal-graph view.

### 6. Exploit decomposition shape

After separator branching, many sub-components are created by deleting a
small boundary and then simplifying. These often share the same global
shape. A useful optimization is to include:

- separator size;
- component size profile;
- boundary clause statistics

as quick pre-filters before full canonicalization.

### 7. Cache normalized forms, not just counts

If a component is canonicalized once, store the normalized clause
multiset alongside the count. Then future exact comparisons after a hash
match are cheap and do not require rebuilding a full comparable
structure from scratch.

### 8. Use deterministic tie-breaking first

Random permutations of collision blocks may improve recall, but they
complicate the cache and make experiments harder to reproduce. A good
starting point is:

- deterministic WL refinement;
- deterministic secondary tie-breakers based on local clause signatures;
- optional multi-key probing only later if needed.

### 9. Combine with cheap solver-side simplifications

Canonicalization is more powerful after cheap simplifications that the
solver would perform anyway:

- unit propagation;
- failed literals;
- removal of isolated satisfied clauses.

Singleton anonymization is different:

- it is a **cache-key normalization step**, not a solver-side formula
  transformation;
- the solver should not rewrite its internal state just because a
  variable is singleton;
- instead, when a component is extracted for caching, singleton
  variables are forgotten only in the normalized representation used as
  the cache key.

All of these shrink the structure before canonicalization and make
heuristic labels more informative.

### 10. Consider canonicalization only for cache stores, or only for misses

There are two deployment choices:

- canonicalize on every lookup and every store;
- or canonicalize only after exact lookup misses.

The second option is usually the best first implementation.

---

## Complexity

Let `m` be the total number of literal occurrences in the component.

- extracting the component formula is `O(m)`;
- identifying singleton variables is `O(m)`;
- `k` rounds of WL-style refinement are roughly `O(k * m)` with
  appropriate hashing and sorting of small local multisets;
- sorting the normalized clauses is `O(m log m)` in a straightforward
  implementation, or near-linear with specialized encodings.

The total overhead is polynomial and should be applied only where it is
likely to pay off, for example:

- for components above a minimum size threshold;
- or only after ordinary exact-ID cache lookup fails.

---

## Summary

The formally correct caching principle is:

- cache together exactly those component formulas that are equivalent up
  to global renaming and optional global complementation of
  non-singleton variables, together with anonymization of singleton
  literals.

An exact canonical form would realize this perfectly. In practice, a
WL-style procedure can be used to build a **heuristic canonical key**:

- once the procedure is fixed, each formula gets one deterministic key;
- if it maps two equivalent formulas to the same key, we gain a cache
  hit;
- if it fails, we only lose a cache opportunity;
- correctness is preserved as long as equal keys imply equivalent
  normalized clause multisets.
