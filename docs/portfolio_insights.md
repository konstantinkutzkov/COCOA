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

**Threshold value (the `N` in `-sep N` = `separator_min_active_vars`) is non-obvious and instance-class-dependent.** Below this threshold, separator branching is skipped on the current sub-component and we fall through to plain variable branching. Measured on t1_021_k7_s1 (83v/80c, 1:1 var/clause sparse instance):

| `-sep N` | time | decisions |
|---|---|---|
| 5 (current default in our test runs) | 17.97 s | 29.6 M |
| 15 (config default) | 18.08 s | 29.6 M |
| 30 | 11.87 s | 12.3 M |
| 50 | 10.71 s | **6.16 M** |
| 70 | 10.66 s | 6.16 M |
| 100 / 1000 / no sep | TIMEOUT (> 30 s) | — |

Three regimes:

- **N ≤ 15**: separator fires on tiny sub-components where the cut is bad — wasteful build cost without benefit. Decision count maxes out (~30 M).
- **N = 30–70**: separator only fires on top-level / large sub-components. Sweet spot. Decisions plateau at 6.16 M from N=50 onward.
- **N ≥ 100** (or no separator): separator never fires; the search relies on raw variable branching at the root and explodes. Confirms the separator is **load-bearing at the root**, just **harmful below ~50 active vars** on this instance class.

ganak's baseline on the same instance: 9.53 s. Our best (N=50–70) lands at 10.7 s, closing most of the gap from 18 s.

**Hypothesis on why**: t1_021 is small (83 v); after the root separator splits, sub-components shrink below ~50 vars quickly, and applying the separator to those doesn't help enough to justify its build cost. On much larger instances (t1_011 with 6559 v) the sub-components stay above the threshold for many recursion levels, so a low threshold is fine — the existing default of 5 was tuned on those.

**Implication for the portfolio analyzer**: the right threshold scales with instance size and structural density. A reasonable cheap proxy might be `min(50, n_active_vars / 4)` or similar. Needs measurement across more instance classes before committing.

### `-unifiedPicker` and the rate / τ family (`-pickerRateFramework`, `-cascadeW`, `-pickerMode multiplicative`)

**What**: replaces the legacy two-stage picker (Stage 2 forced separator consumption + Stage 3 `freq + 10·act + 1000·bias` variable picker) with a single scoring contest over all active vars and clauses. Flags subdivide further: `-pickerMode multiplicative` selects the multiplicative-boost score `raw · (1 + α·exp(−λ·rel_k))`; `-pickerRateFramework` re-routes that score through the τ-based rate framework (`−n_active_vars · log τ`); `-cascadeW W` adds an additive cascade-gain term `W · cascade_score` into `raw`. Optional `-pickerNonSepKillsNd` drops `nd_node` for non-sep picks.

**When it helps in theory**: instances where the precomputed METIS ND-hierarchy cut is wrong, or where some non-sep candidate has dramatically better cascade / branching properties than the planned cut. The unified picker can override the cut.

**When it hurts in measurement (so far)**: every measured instance. See 2026-05-09 entries in §4. Across t1_065 / t1_071 / t1_021_k10_s1 / t1_011, plain (no unified picker) matches or beats every unified-picker variant tested. The override flexibility introduces failure modes (scattered picks at root, picker-overhead amplification on cache-heavy instances, stale-bias-style problems via the multiplicative boost on dense instances) without earning its keep on these instances.

**Quantitative cost on t1_011** (cache-heavy regime, see §4 2026-05-09 row): adding `-cascadeW 0.5` halves the search tree (215 K → 118 K decisions) but each decision becomes 6× more expensive, costing 4× wall time. The cascade signal is *informative* (smaller tree) but its computation (`computeBcpGainScore` per active var per pick) is too expensive to net out positive on instances where the picker isn't the bottleneck.

**Detection**: not yet identified. There is no measured instance where the unified picker is the right choice over plain. Until we find one, treat as research scaffolding.

**Status (2026-05-09)**: classify as **research path, off by default**. Production / portfolio recommendation: use plain (or legacy `-sepVarBias` per the rule in §4 2026-05-09) until an instance class is identified where the unified picker provably earns its keep.

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

### `-wlIter K` (max WL refinement iterations in canonical-key cascade)

**What**: caps how many Weisfeiler-Leman refinement passes are attempted in `buildCanonicalKey` before the cascade falls through to the static-WL-label combine and the raw-id identity fallback. The cascade always runs end-to-end on collision blocks regardless of `K`; this flag only bounds how much sharpening of dynamic WL we attempt before moving on.

Cascade structure (always on):
1. iter 1 (clause-type signature) — always.
2. iter 2..K (neighbor-aggregation WL) — gated on collisions remaining AND `iter ≤ K`.
3. static-WL-label combine — gated only on collisions remaining (independent of K).
4. raw-id (shifted) identity fallback for residuals — gated only on collisions remaining.

**When higher K helps**: in principle, formulas where dynamic WL needs more rounds to discriminate vars but the resulting splits open useful cache merges that the static-label step alone misses. **Not yet observed empirically** — on super_d3_id8 (the only measured pathological instance so far), `K=2` makes the run slower than `K=1` despite producing a sharper key. Reason: the sharper key creates more distinct cache buckets, so the search makes more decisions, dominating the per-call refinement saving.

**When higher K hurts**: any instance where the extra dynamic WL pass produces too-discriminative keys → more cache misses → more search work. Magnitude: measured 2.14 s (`K=1`) vs 4.02 s (`K=2`) on super_d3_id8 perm.

**When the cascade itself helps (K independent)**: any instance where iter 1's heuristic var_idx tie-break would produce false-positive cache hits across non-isomorphic sub-components. Confirmed: super_d3_id8 perm gives 26,575,110,112 (wrong) without the cascade, 26,843,545,568 (correct) with it.

**Detection**: run with `K=1` first. If correctness is the question, the cascade is unconditionally on, so `K=1` is safe by default. If runtime matters and the instance has many large iter-1 collision blocks (CANON_STATS shows `max_block_size > ~30` and `calls_with_any_collision/calls > 0.9`), `K=2` is worth a try in the portfolio.

**Status**: hyperparameter candidate for portfolio tuning. Default `K=1`. The tuning question is per-class: it's plausible (untested) that some sparse formulas with rare but very large WL-confounded blocks benefit from `K=2`, while pathological symmetric formulas like super_d3_id8 prefer `K=1`.

**Cost when `K=1` and iter 1 fully anchors**: zero extra work — steps 2–4 are gated and skipped entirely. The cascade only pays its cost on calls where iter 1 leaves a collision block.

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

### 2026-04-27

- **t1_021_k7_s1** (83 v / 80 c, sparse 1:1 var:clause ratio,
  produced by freezing 7 vars in t1_021). Hyperparameter sweep:
  - **`-sep N` is the dominant lever on this instance class.**
    `-sep 50` → 10.71 s (vs `-sep 5`: 17.97 s, **−40% wall-time, decisions
    cut 4.8×**). `-sep 70` matches it; `-sep 100`+ or no `-sep` times
    out > 30 s. Separator is load-bearing at the root, harmful below
    ~50 active vars on this instance.
  - **Adaptive picker (any α ∈ {0.5, 1.0, 2.0, auto}) is neutral-to-slightly-bad**
    here (~18.7-18.9 s vs 17.97 s baseline). t1_049's α=0.5 sweet spot
    doesn't transfer to t1_021. Different structural class.
  - **`-reactiveMetis`, `-wlIter 2`, `-learnLevel 5` all neutral** (within
    noise of baseline).
  - **`-cb N` (clause branching min length) variations are neutral**
    (cb=3, cb=5, cb=8 all ~18 s with `-sep 5`). The clause-branching
    threshold doesn't move the needle on t1_021.
  - ganak baseline on the same shrunken instance: 9.53 s. Our best
    (`-sep 50`) at 10.71 s closes most of the 1.88× gap.
  - **Implication**: portfolio analyzer should set `-sep N` based on
    instance size / structural density, not a fixed `-sep 5`. Cheap
    proxy candidate: `N ≈ min(50, n_active_vars / 4)`. Needs validation
    on more instances before committing to the formula.

- **t1_021 family (sparse 1:1 var:clause) — ganak's home turf, not ours.**
  Continued the sweep on the same instance shrunken at multiple k values.
  Growth rate per added variable:
  | k | shrunken size | ganak | ours `-sep 5` |
  |---|---|---|---|
  | 10 | 80 v | 1.50 s | 2.40 s |
  | 7  | 83 v | 9.53 s | 17.97 s |
  | 4  | 86 v | 22.19 s | TIMEOUT > 600 s |
  | full | 90 v | TIMEOUT > 1200 s | (also TIMEOUT) |
  - Per-variable growth: ganak ≈ 1.32×, ours ≈ 3.2× → gap widens
    super-linearly. At k=4 we can't even confirm correctness within
    a 10-min budget while ganak finishes in 22 s.
  - **Hypothesis**: ganak's structural advantages (Arjun gate
    detection / equivalence reasoning, tree-decomposition–driven
    branching, possibly vivification) are doing real work on this
    instance class that no flag combination of ours can compensate for.
    Confirmed: `-sep` threshold sweep, `-adaptive`/`-adaptiveAlpha`
    sweep, `-reactiveMetis`, `-wlIter 2`, `-learnLevel 5` — none close
    the gap measurably at k=7 (~1.6× best vs ganak), and at k=4 we
    don't finish at all.
  - **Portfolio implication**: classify "1:1 var:clause sparse" as
    ganak's-home-turf class. Detection is cheap (`n_clauses ≈ n_vars`
    after preprocessing). Driver should attempt ganak-first or
    fall through to ganak quickly if our solver doesn't make
    progress in a small probe budget.

  - **Mechanism diagnosed (2026-04-27)**:
    - On t1_021_k5_s1, our cache hit:store ratio is **12.5%** at best
      (771,716 stores vs 96,358 hits with `-sep 50`); raising to
      `-wlIter 2` reduces collision-block size 50× (max 7→2) but
      barely changes the hit rate. So the cache machinery is fine —
      the search tree just doesn't have many repeated sub-components.
    - Periodic logging shows our solver makes **800k+ `solveComponent`
      calls per second at sustained recursion depth 50–58** for the
      whole 60s window — a deep, wide tree with no shortcut.
    - **0 conflicts** during the run, so conflict-driven learning
      isn't filling the gap either.
    - ganak's verbose output reveals it computes a **tree decomposition
      of width 26** via Flowcutter and uses Korhonen–Järvisalo's
      tree-decomp guided counting (CP 2021). That gives 2^26 ≈ 67M
      effective branching cost vs our naive ~2^50 (since our METIS
      separator branching only structures one level, not the whole
      search). The exponential gap of 2^(50−26) = 2^24 ≈ 16M
      explains the 30+× wall-clock difference.
    - **What we'd need**: tree-decomposition-driven variable branching
      order. Significant implementation effort (compute TD via
      Flowcutter or similar, then drive `pickBranchVariable` from
      the TD's elimination order). Filed as "open architectural gap"
      rather than a tunable. Don't try to close this with flag-tuning.

  - **Refined diagnosis (2026-04-27 follow-up)**: our **ND hierarchy
    IS already a tree decomposition** in the structural sense. Probed
    NDHierarchy stats across the t1_021 family:
    | variant | n_vars | our `max_sep` | ganak's TD width |
    |---|---|---|---|
    | k=7 | 83 | 19 | (not measured) |
    | k=5 | 85 | **21** | **26** |
    | k=4 | 86 | 23 | (not measured) |
    | full | 90 | 27 | (timed out) |
    Our max separator is *smaller* than ganak's reported tree-decomp
    width, so the decomposition isn't the bottleneck. **The gap is in
    HOW WE USE the hierarchy**:
    - **Our approach (search-based)**: branch on separator elements
      *sequentially*. A 21-element top separator yields 2^21 ≈ 2 M
      paths at that level alone, each followed by full BCP + recursive
      search. Cache keyed by the residual sub-component's canonical
      form — different separator-decision orders produce different
      residuals, which often miss cache reuse.
    - **TD dynamic programming (ganak)**: at each ND node, enumerate
      assignments to the bag and TABULATE the partial count, combining
      child contributions via DP. Cache keyed by (ND node, bag-state).
      Same 2^|bag| at heart, but per-leaf work is a table lookup, not
      a search.
    - **Implementation sketch**: keep our existing ND hierarchy. Replace
      the separator-branching loop with: for each ND node, enumerate
      all bag assignments compatible with the current trail, recursively
      compute child contributions, store in a `(node_id, packed_bag_state)`
      cache. Substantial but bounded: the structural primitive is
      already there.

### 2026-05-09 — Picker-design session: rate-framework dead-end, plain rises, 2-feature rule emerges

This session pursued the unified-picker / rate-framework redesign on three
structurally different instances (t1_071 sparse / small-sep, t1_021_k10_s1
small-decomposable, t1_011 large / cache-heavy). All measurements are in
`benchmark_log.md` 2026-05-09 entries; this section captures the **insights**
that should feed the analyzer.

#### Three signals the picker reasons about

A useful conceptual decomposition that came out of this session. The picker
trades off three distinct kinds of "good":

| Signal | Rewards | Captured by | Path-dependence |
|---|---|---|---|
| **(i) Separator progress** | picks on the planned ND-cut → eventually decompose | `picker_alpha_var`, `picker_lambda_*`, `picker_alpha_clause`; or implicitly Stage-2 forced consumption | Low (sep set fixed at ND-build time) |
| **(ii) BCP simplification** | picks producing strong cascades, balanced branches | `pn_pos`/`pn_neg` from `computeBcpGainPolarities` feeding into τ; or additive `cascade_score_weight` | Medium-high (BCP walk includes learned binaries; cascade-gain depends on path) |
| **(iii) Cache reuse** | picks producing sub-components likely already cached | **— not captured by any current parameter** | Inherently global |

Two empirical lessons from this session:

- **(ii) and (iii) are in tension.** Stronger cascade signal in the score
  produces smaller search trees but more path-dependent picks (different
  recursion paths to the same logical sub-state pick differently → different
  canonical keys → cache miss). Quantified on t1_011: adding `cascade_score_weight = 0.5`
  halves decisions (215 K → 118 K) but each decision becomes 6× more
  expensive (68 µs → 404 µs), so net wall time is 4× worse. The cascade
  signal is informative but expensive to compute every time.
- **Plain (no unified picker, Stage-2 hard-forced sep consumption) is currently
  the most universal config.** Wins or ties on all four instances we measured
  this session (t1_065, t1_071, t1_011) and is competitive on t1_021_k10_s1
  (5.25 s vs legacy's 3.99 s, 1.3× slower but solves). No unified-picker
  variant matched plain across the four.

#### The bias-staleness mechanism (`-sepVarBias` failure mode)

`-sepVarBias` strips separator VARs into a global persistent `bias_bitmap`;
Stage 3's picker adds `+1000·bias[v]` to every var's score, **for the entire
search**. The bias never expires. This is fine while the original sep VARs
are still load-bearing in the current sub-component, but on dense /
high-density instances BCP cascades decide most sep VARs early — yet the
`+1000` bonus survives into deeper sub-components where the var is no longer
at any separator boundary. The bonus then misleads picks for the rest of the
search.

Empirical demonstration (2026-05-09):

| Instance | n_vars | density | Legacy `-sepVarBias` | Plain | Comment |
|---|---|---|---|---|---|
| t1_065 | 112 | 5.29 | **0.53 s, 75 860 dec** | 0.017 s, 614 dec | Bias 30× slower than plain on uniform 5-CNF |
| t1_071 | 640 | 2.84 | TIMEOUT > 60 s | 0.41 s, 62 K dec | Bias times out where plain is sub-second |
| t1_011 | 6559 | 2.21 | 13.73 s, 215 018 dec | 13.57 s, 215 018 dec | Bias bit-identical (no effect; deep ND tree absorbs bias staleness) |
| t1_021_k10_s1 | 80 | 0.94 | **3.99 s, 879 K dec** | 5.25 s, 1.32 M dec | Low-density: bias still useful, no staleness |

Bias works when BCP cascades are *weak* (low density) and the ND tree is
*shallow* (small instance). It breaks when BCP cascades pin sep VARs early
(high density) — the bonus survives the relevance window. It's irrelevant
when ND tree is deep enough that the bias is dwarfed by other picks anyway
(t1_011 case).

#### Proposed 2-feature analyzer rule (hypothesis from 4 data points)

```
if  density > 1.5  OR  n_vars > 200:
    use Plain (-rec -sep 5 -cb 3)
else:
    use Legacy (-rec -sep 5 -cb 3 -sepVarBias)
```

Routing check on the four 2026-05-09 instances: t1_065 (density 5.29 → plain ✓),
t1_071 (n_vars 640 → plain ✓), t1_011 (density 2.21 → plain ✓), t1_021_k10_s1
(density 0.94, n_vars 80 → legacy ✓).

This rule is consistent with the measurements but is built from four data
points spanning two orders of magnitude in size. Specifically untested: the
**(small, low-density)** quadrant where plain might still beat legacy
(would require a third feature), and the **(large, low-density)** quadrant.

The mechanistic story above (bias-staleness governed by BCP-cascade strength)
is consistent with both kept and ruled-out quadrants but should be confirmed
on more instances before this rule lands in `analyzer.cpp`.

#### Cascade-weight sweep findings (no universal value)

`-cascadeW W` was swept on three instances (W ∈ {0, 0.5, 1, 2, 5, 10}) with
mult-only picker (no rate framework, no killNd). **No single W is universally
good:**

- t1_071 wants `W ∈ [1, 2]`: `c=2` → 0.78 s; `c=0` and `c=5+` time out. Sharp
  sweet spot around 1.5.
- t1_021_k10_s1 wants `W = 0`: any cascade > 0 dramatically hurts (10 s →
  TIMEOUT). On this small decomposable instance, cascade actively misleads.
- t1_011 wants `W = 0`: matches legacy/plain at 14.7 s with bit-identical
  trees. `W ≥ 0.5` introduces 4× wall regression purely from `computeBcpGainScore`
  per-pick overhead.

Implication: cascade as a fixed analyzer-set weight is a non-starter. If
cascade ever lands in production it needs an instance-feature switch, and
none of the cheap features identified so far (n_vars, density) cleanly
predict the right value.

#### Implications for §1 instance-class taxonomy

Provisional refinements to the taxonomy in §1:

- **§1a (sparse / low-tw)** — plain Stage-2 consumption is sufficient and
  likely optimal. Confirmed on t1_011, t1_071. Cascade signal is harmful on
  cache-heavy variants; sep-bias (`-sepVarBias`) is harmful on small /
  shallow-ND variants like t1_071.
- **§1b (dense 3-SAT)** — t1_021_k10_s1 (density 0.94) doesn't really fit
  here despite the original log calling t1_049 dense; t1_021's structure is
  closer to "small, low-density, decomposable" and is the one case where
  `-sepVarBias` still wins. Suggests a fourth class.
- **NEW §1e (small + low-density + decomposable)** — proposed. Examples:
  t1_021_k10_s1 (and presumably the rest of the t1_021 family). Signature:
  `n_vars ≤ ~100`, `density ≤ ~1.5`, ND-hierarchy max_sep modest. Preferred
  flags: `-rec -sep 5 -cb 3 -sepVarBias`. Distinguishing feature from §1a:
  small enough that bias staleness doesn't develop within the search.
- **§1c (medium)** — characterisation needs revisiting now that t1_065
  (density 5.29) and t1_011 (density 2.21) are both "medium" by mean-clause-length
  but very different in size. Density alone may not be the right axis.

#### What would falsify the 2-feature rule

A small (n_vars ≤ 100), low-density (≤ 1.5) instance where **plain beats
legacy `-sepVarBias`** by a meaningful margin. We don't currently have one in
the test set. Proposed candidates to characterise: `mc2025_track1_023.cnf`,
`mc2025_track1_025.cnf`, `mc2025_track1_027.cnf`, `mc2025_track1_041.cnf`,
`mc2025_track1_047.cnf` — extract `(n, m, density, mean_len, ND-stats)` and
run plain + legacy + mult c=0 with a 30 s budget. See open question Q10 below.

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

### Q8: When to override default `-wlIter 1`?

The cascade with `K=1` is correct on the only known pathological instance (super_d3_id8 perm). `K=2` was measured to be **slower** there, not faster. Open: are there instance classes where the extra dynamic WL pass yields fewer-enough cache misses to net out positive? Hypothesis: very large formulas with many WL-resolvable-but-not-with-iter-1 collision blocks. Needs sweep across MC2025 to confirm or refute. Until then, leave at `K=1`.

### Q9: Signal robustness

Some of the features we're considering (e.g., binary fraction) could
be manipulated by trivial syntactic changes to the formula that
don't change the count. The analyzer should be robust to encoding
choices. Proposal: make mappings coarse enough (e.g., three buckets
rather than a linear response) so small perturbations don't flip
decisions.

### Q10: Validate or falsify the (density, n_vars) → {plain, legacy} rule

Provisional rule from §4 (2026-05-09): `density > 1.5 ∨ n_vars > 200 → plain;
else → legacy -sepVarBias`. Built from 4 data points. To validate or
falsify:

- Characterise unmeasured MC2025 track-1 instances (`023, 025, 027, 041, 047,
  053, 023, 027_reduced`, ...) along `(n_vars, n_clauses, density, mean_len,
  ND-stats)`.
- Run plain + legacy + mult c=0 with 30 s timeout on each.
- Look specifically for a **falsifier**: a small (n ≤ 100), low-density
  (≤ 1.5) instance where plain beats legacy. If we can't find one in say
  10–15 instances, the rule holds for now. If we find one, the rule needs
  a third feature (likely a structural property of the ND-hierarchy:
  max_sep / n_vars ratio, fraction of vars that ever appear in any
  separator, etc.).

Until the rule is validated, the analyzer's recommendation should still be
"use plain" as the safe default; legacy `-sepVarBias` is a special-case
override for the small-decomposable quadrant.

### Q11: Is there an instance class where the unified picker earns its keep?

The 2026-05-09 sweep showed every unified-picker variant (rate framework,
hybrid mult-during-sep, mult-only with cascade) loses to plain on every
measured instance. The unified picker's design intent was to override the
precomputed cut when scoring suggests a non-sep candidate is dramatically
better. **No measured instance has shown this happening profitably.**

Hypotheses to test:
- XOR-encoded instances (none in current test set) where the cut is bad
  due to symmetry-induced cycles in the incidence graph.
- Circuit encodings (none) where binary-clause structure dominates and the
  precomputed sep is poor.
- Very large instances where plain's overhead is significant and a
  smarter cut decision pays off.

If none of these earn the unified picker's keep either, the entire
`-unifiedPicker` code path can be deprecated or moved behind a research-only
flag. Keep the code for now (documented in §2 entry "the unified picker"),
deprecate after one more round of falsification attempts.

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

**Refined picker recommendations (2026-05-09; supersedes earlier picker
guidance for instance routing):**

- **Default for unknown instances**: **plain** — `-rec -sep 5 -cb 3`. No
  unified picker, no `-sepVarBias`, no cascade. Stage-2 hard-forces sep
  consumption in METIS order; Stage-3 picks by `freq + 10·act`. Wins or
  ties on every measured instance class.
- **Override only for small + low-density**: when `n_vars ≤ 200` AND
  `density ≤ 1.5`, switch to **legacy `-sepVarBias`** — it gives an
  additional `+1000·bias` boost in Stage-3 that targets original sep VARs.
  Confirmed faster on t1_021_k10_s1; harmful on dense / large instances
  due to bias-staleness (see §4 2026-05-09).
- **Avoid the unified picker (`-unifiedPicker`, `-pickerMode multiplicative`,
  `-pickerRateFramework`, `-cascadeW`) for production**: across all measured
  instances it loses to plain. Treat as research scaffolding, not a
  portfolio choice. See Q11 for the falsification programme.

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
