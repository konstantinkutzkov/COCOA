# Portfolio Analyzer — Collected Insights

Living document. Each entry captures something we have learned about
how instance structure relates to the "right" flag combination, so a
future automatic flag-picker does not have to re-derive it from
measurement. Companion to `portfolio_driver_plan.md` (architecture)
and `benchmark_log.md` (per-run measurements).

Update discipline: **append, don't rewrite**. When an insight turns
out wrong, add a correction rather than editing the original. The
value of this document is the history of what we thought and what
we measured, not a polished final answer.

---

## 1. Instance-class taxonomy (what we distinguish on)

Working classification, coarsest first. Each class needs a different
flag set.

### 1a. Sparse / low-treewidth

**Examples so far**: t1_011 (6559v / 14515c; tw ~4–5), t1_065 (592c),
t1_071 (small, well-structured).

**Signature**:
- Mean active clause length ≥ 6 (long clauses, many-ary structure).
- METIS root-level vertex separator is small (≤ 15% of active vars)
  and balanced.
- Binary clauses are a minority.
- Components decompose quickly via BCP + ND hierarchy.

**Preferred flags**:
- `-sep 5 -cb 3` — **essential**. Without the hierarchy the search is
  catastrophic.
- `-adaptive` — irrelevant. The adaptive picker only fires at
  hierarchy leaves (components below `adaptive_probing_min_vars=60`),
  which on these instances are mostly too small to probe anyway.
- `-reactiveMetis` — **hurts**. Measured −7% on t1_071. The
  precomputed hierarchy already gives good separators; reactive
  METIS's per-fallback overhead isn't recovered.
- `-learnLevel 4` (no minimization) — default, keep.

### 1b. Dense 3-SAT

**Examples so far**: t1_049 (90v/252c) and its `k*` shrunken
variants.

**Signature**:
- Mean active clause length close to 3 (e.g., 2.78 on
  t1_049_k10_s1 post-preprocess).
- METIS root-level vertex separator is large (> 20% of active vars)
  or heavily imbalanced — Phase-2 gate rejects it.
- Binary fraction is modest but non-trivial.
- Very few simplification opportunities at preprocessing
  (t1_049_k10_s1: only 2 subsumptions, 2 SSR shortenings on 200
  clauses).

**Preferred flags**:
- `-sep 5 -cb 3` — moderate value. Separator may be rejected at the
  root but still fires on smaller sub-components during search.
- `-adaptive` — **helps when combined with -sep**, provided the α is
  correctly set.
- `-adaptiveAlpha` (or auto via analyzer): **α ≈ 0.5** on these
  instances. α = 2.0 (the sparse-tuned default) is measurably worse
  — 50% slower search tree on t1_049_k10_s1.
- `-reactiveMetis` — not yet measured on dense; hypothesis: may help
  when the precomputed hierarchy is poor anyway.

### 1c. Medium

**Examples so far**: t1_011 post-preprocess (mean_len=3.53 — just
above the 3.5 threshold), t1_065 (mean_len=5.0).

**Signature**:
- Mean active clause length 3.5–6.
- Mix of short and medium clauses.
- Hierarchy quality varies.

**Preferred flags**: Context-dependent. α=1.0 by analyzer; manual
`-adaptive` usually not a win because the hierarchy handles most of
the decomposition. Needs more data.

### 1d. Outstanding categories not yet characterized

- **Circuit encodings** (XOR-heavy, linear-like structure). No
  sample in our current test set. Likely to need different
  treatment — potential hyper-binary-resolution target.
- **Counting-specific instances** (chain/grid/ladder structures).
  Historically handled well by sharpSAT-td via caching alone;
  may not need any branching sophistication.

---

## 2. Per-flag empirical notes

For each flag: what it does, when it helps, when it hurts, how to
detect the relevant condition cheaply.

### `-sep N` (separator branching)

**What**: uses the precomputed ND hierarchy to pick a separator at
each decision point. Also enables clause branching implicitly.

**When it helps**: the formula has good vertex separators at multiple
levels. Measured: t1_011, t1_065, t1_071 — near-essential.

**When it hurts**: dense instances where the root separator is
rejected anyway. Still pays the build cost.

**Detection**:
- Cheap: run METIS once at the root level (no recursion). Measure
  `sep_size / n_vars` and balance. If ratio ≤ 0.15 and balance
  ≥ 0.35, hierarchy is worth building.
- Alternative: look at mean clause length as a proxy — sparse
  implies good hierarchy.

**Status**: not yet wired into the analyzer. Currently user-chosen.

### `-cb N` (clause branching min length)

**What**: enables branching on long clauses via `#SAT(F) =
#SAT(F∖{C}) − #SAT(F∖{C} ∧ ¬C)`, for clauses of length ≥ N.

**When it helps**: implicitly coupled with `-sep`. When a separator
contains clause-nodes (long clauses that bridge components), clause
branching is how we consume them.

**When it hurts**: without `-sep` the clause-branching overhead
(managing `removed_clauses_`) is paid without the decomposition
benefit.

**Detection**: pair with `-sep`. Flag coupling is already baked into
the CLI — `-sep` turns on clause branching via `main.cpp:75-77`.

### `-adaptive` (Stage-0 + τ variable branching)

**What**: replaces `pickBranchVariable` (raw-count) with
`pickBranchVariableAdaptive` (length-weighted cheap score + Stage-2
probing) on the no-separator path.

**When it helps**: at hierarchy leaves of dense sub-components
(variable branching required, tree large enough that branch quality
matters). Requires correct α.

**When it hurts**: used alone (no `-sep`), it becomes responsible for
the entire decomposition. Catastrophic on hierarchy-friendly
instances like t1_011.

**Detection**: needs `-sep` to already be on (analyzer should chain
the decision).

**Status**: α is auto-picked post-preprocess (landed
2026-04-24 commit `7efc278`). Whether `-adaptive` itself should be
auto-enabled is still TBD — see §3 below.

### `-adaptiveAlpha f` (Stage-0 length decay)

**What**: sets α in `cheap_score(v) = Σ 2^(−α·len(C))`.

**When it helps**: instance-class-specific.

**Measured mapping** (k10_s1 with `-adaptive`):

| α | Time | Decisions |
|---|---|---|
| 0.25 | 2.49 s | 852k |
| 0.5 | **2.47 s** | 854k |
| 1.0 | 3.02 s | 1.03M |
| 2.0 (default) | 4.05 s | 1.37M |

On dense instances lower α is clearly better. On sparse the
hardcoded 2.0 was tuned on t1_071 and presumably is right there.

**Analyzer mapping** (first cut, landed 2026-04-24):
- mean_len < 3.5 → α = 0.5
- 3.5 ≤ mean_len < 6.0 → α = 1.0
- mean_len ≥ 6.0 → α = 2.0

### `-reactiveMetis` (runtime METIS fallback)

**What**: when the precomputed hierarchy's separator is rejected,
runs METIS afresh on the current sub-component.

**When it helps**: hypothetically, on mixed-density instances where
the precomputed hierarchy is wrong but the sub-component is still
partitionable. **Not yet measured to help on any real instance.**

**When it hurts**: sparse/well-structured instances. Measured −7% on
t1_071.

**Detection**: correlates with "hierarchy separator often rejected."
Could instrument this: if Phase-2 gate rejects > X% of separator
queries, reactive is plausibly useful.

### `-learnLevel N`

**What**: ladder of learning features (5=full, 4=no-minimize,
3=no-pad, 2=no-scope, 1=no-dedup, 0=no-learn).

**When it helps**: default 4 is the known-sound setting.

**When it hurts**: level 5 is currently unsound (see
`bug_investigation_t1_011.md`). Levels 0–3 remove soundness layers
that are otherwise helping.

**Detection**: n/a — just leave at 4 unless explicitly diagnosing.

### Preprocessing rules (`-noSubsumption`, `-noPureDup`, `-noSSR`)

**What**: turn off individual preprocessing rules.

**When it helps**: as diagnostics. For shipping, turning them off is
almost always worse.

**When it hurts**: nearly always worse.

**Detection**: leave all on; preprocessing is fast (under 50 ms on
MC2025 instances) and reliably harmless.

### Binary-harvest / hyper-binary resolution (not implemented)

**What**: probe each variable, record BCP-forced literals, learn
them as binaries.

**When it might help**: mixed-density instances where BCP cascades
are long enough that lifting them into explicit binaries enables
more preprocessing.

**When it would hurt**: sparse instances (densifies the incidence
graph, degrades separator quality).

**Status**: not implemented. Deferred behind the measurement
criteria in `portfolio_driver_plan.md`.

---

## 3. Cheap structural features to extract

These all run in O(n + m) or with one METIS call, i.e., under 1 s on
MC2025-sized instances.

### 3a. Already extracted (analyzer has access)

- `n_active_vars` (post-preprocess).
- `n_active_clauses` (post-preprocess, both long and binary).
- `mean_active_clause_length`.

### 3b. Cheap to add

- **Clause length histogram** — n_binaries, n_ternaries, n_longer.
  Useful because "mean=3" can mean "all ternaries" or "equal-parts
  binaries and 5-clauses"; the distribution matters.
- **Variable degree histogram** — min, max, mean, variance. Hints
  at structure uniformity.
- **Binary fraction** — `n_binaries / n_active_clauses`. Separate
  signal from mean_len because binaries propagate immediately.
- **Variance of clause length** — low variance suggests uniform
  structure (e.g., pure 3-SAT); high variance suggests encoded
  formulas (circuit/CSP).
- **Root-level METIS probe** — one MET IS call on the full incidence
  graph. Records `sep_size / n_vars` and balance. Direct proxy for
  "will the hierarchy help?"
- **Connected-components count** — BFS on incidence graph. If > 1,
  each component can be solved independently; also suggests
  preprocessing or the driver should split.

### 3c. Moderately expensive

- **BCP-saturability probe** — pick ~10 random variables, force each
  to true, measure BCP cascade length. Mean cascade length indicates
  how much propagation the formula can do without branching.
- **Pairwise-probe failed-literal density** — count failed literals
  that single probing finds. Indicator of whether binary-harvest
  would pay off.

### 3d. Expensive (defer)

- **Treewidth estimate** — PACE-style algorithms. Minutes on our
  instances; only worth it if we believe the benefit over MET IS
  probe is large.
- **Trial solves** — actually running the solver with candidate flag
  sets on a small budget and observing progress. This is the
  portfolio driver itself, not a feature for the analyzer.

---

## 4. Measured observations (raw notes, dated)

Append-only log of observations that should feed the mapping.

### 2026-04-24

- **t1_011** (6559v/14515c) with `-sep 5 -cb 3`: 25–40 s depending on
  machine load. Without `-sep`: ~40 s on default picker; hangs
  (>5 min) with just `-adaptive`. Implication: hierarchy is essential
  for this class; picker choice secondary.
- **t1_065, t1_071**: sub-second with `-sep 5 -cb 3`;
  `-reactiveMetis` regresses them by 2-7%.
- **t1_049 full** (90v/252c, mean_len ≈ 3): times out > 10 min with
  `-adaptive` alone. 25 s default on the similar-size `k6_s1`
  variant (84v/223c).
- **t1_049_k10_s1** (80v/212c, mean_len=2.78):
  - default picker, no flags: 2.70 s.
  - `-adaptive` alone, α=2.0: 4.05 s (picker choice causes 1.48×
    tree).
  - `-adaptive` alone, α=0.5: 2.47 s (beats default by 9%).
  - `-adaptive -sep 5 -cb 3`, α=0.5: 2.47 s. `-sep` doesn't hurt
    here; hierarchy still fires at larger sub-components.
- **Analyzer decisions** (2026-04-24 commit `7efc278`):
  - t1_049_k10_s1 mean_len=2.78 → dense → α=0.5
  - t1_011 mean_len=3.53 → medium → α=1.0
  - t1_065 mean_len=5.00 → medium → α=1.0

---

## 5. Open design questions

Numbered so later entries can reference them.

### Q1: Should `-adaptive` be auto-enabled with `-sep`?

If the user picks `-adaptive` alone (no `-sep`) they almost always
get worse performance. The intended combination is `-sep ... -adaptive`.
Options:
- Print warning when `-adaptive` is alone.
- Have `-adaptive` imply `-sep 5 -cb 3` unless user overrides.
- Keep current behavior; rely on docs.

### Q2: What's the cheap "should we use the hierarchy?" test?

Hypothesis: run one top-level METIS call, reject hierarchy if root
sep ratio > 0.15 or balance < 0.35. Need measurement on
representative instances to set thresholds.

### Q3: Should the analyzer also pick `-reactiveMetis`?

Currently off by default. Helps in theory on instances where the
precomputed hierarchy is poor but sub-components are still
partitionable. No measurement yet showing it's a net win on any
real instance. Proposal: defer until we have such a case.

### Q4: Should preprocessing rules be gated?

Currently all three (subsumption, pure-dup, SSR) are always on. They
are cheap on measured instances. But for very large formulas
(hundreds of thousands of clauses) the O(n²) subsumption scan could
matter. Proposal: measure on a big formula; if > 10% of solve time,
add a size-based gate.

### Q5: Alpha buckets — are 3 enough?

Current mapping: α ∈ {0.5, 1.0, 2.0} by bucket. Could go finer
(α ∈ {0.25, 0.5, 1.0, 1.5, 2.0, 3.0}) based on mean_len alone, or
bring in variance. Needs empirical tuning across instance classes
(MC2025 suite would be ideal).

### Q6: Does the analyzer need to re-run mid-search?

Currently runs once post-preprocess. As the search deepens and
components shrink, the "right" α might change. E.g., a hierarchy-
leaf sub-component could be dense-3-SAT-shaped even though the full
formula is medium. Proposal: leave single-shot for now; revisit if
measurements show it matters.

### Q7: Probing cost vs. budget

For very large formulas, running even a single METIS call or BCP
probe takes seconds. For competition-size instances this is small;
for much larger instances it could dominate. Proposal: make
analyzer cost proportional to time budget — if budget < 10 s skip
structural probing entirely.

### Q8: Signal robustness

Some of the features we're considering (e.g., binary fraction) could
be manipulated by trivial syntactic changes to the formula that
don't change the count. The analyzer should be robust to encoding
choices. Proposal: make mappings coarse enough (e.g., three buckets
rather than a linear response) so small perturbations don't flip
decisions.

---

## 6. Relationship to portfolio driver

This document is the **decision space** the portfolio driver
(documented in `portfolio_driver_plan.md`) will eventually optimize
over. The portfolio driver adds:

- Multiple solve attempts with different flags, subprocess-isolated.
- Timeout / fallback sequencing.
- Optional top-level variable branching outside the solver.
- Benchmarking harness for continuous tuning.

The analyzer's job (whether embedded in the solver as today, or
lifted into the driver later) is to pick a single "best guess" flag
set. The portfolio driver's job is to retry with alternatives if the
first guess times out. Both layers benefit from the observations
collected here.

---

## 7. What we would like to have but don't yet

Gap list — items that would move the decision boundary further:

- **Automated calibration harness**: given a flag and a set of
  instances, sweep flag values and record time + count. Right now we
  do this manually per measurement. The benchmark log captures it;
  nothing sweeps automatically.
- **Cross-instance α calibration**: α sweep across the MC2025 suite
  with one fixed value at a time; find the α that's most-rarely-bad.
- **Hierarchy-quality histogram per instance**: during a successful
  run, log how often Phase-2 gates rejected hierarchy separators vs.
  accepted them. Would validate/refute Q2's threshold intuitions.
- **Decision log from the analyzer**: currently prints one line
  ("analyzer: ..."). Should go into a structured side-log that
  benchmark_log.md can reference. Low priority until we have many
  runs to compare.

---

## 8. Rules of thumb (speculative; to be confirmed)

Short-form summary for someone reading quickly. Each line is a
hypothesis that should be validated with measurement before we
commit to it in code.

- **Sparse + low-tw**: hierarchy essential; α irrelevant;
  reactive hurts; learning default is fine.
- **Dense 3-SAT**: hierarchy marginal; α ≈ 0.5 essential; reactive
  status unknown; `-adaptive` is a real win.
- **Medium**: use hierarchy; α ≈ 1.0; reactive probably neutral.
- **Unknown/mixed**: try hierarchy first, fall back to adaptive +
  low α if root separator rejected.
- **Tiny (< 500 vars)**: anything finishes fast; heuristic choice
  doesn't matter; just solve.
- **Very large**: preprocess first, then decide (analyzer can use
  post-preprocess shape).

---

## Appendix A: quick-reference flag combinations

For manual experimentation while the analyzer matures:

```
# Sparse / low-treewidth
./build/sharpSAT -sep 5 -cb 3 input.cnf

# Dense 3-SAT (known)
./build/sharpSAT -sep 5 -cb 3 -adaptive -adaptiveAlpha 0.5 input.cnf

# Unknown / try analyzer-picked
./build/sharpSAT -sep 5 -cb 3 -adaptive input.cnf
  (analyzer picks α from post-preprocess density)

# Diagnostic: no preprocessing
./build/sharpSAT -sep 5 -cb 3 -noSubsumption -noPureDup -noSSR input.cnf

# Diagnostic: no learning
./build/sharpSAT -sep 5 -cb 3 -learnLevel 0 input.cnf
```
