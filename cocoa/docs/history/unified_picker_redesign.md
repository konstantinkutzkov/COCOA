# Unified Picker Redesign: Multiplicative Score with Smooth Gates

## Status

Design proposal. Targets the existing `-unifiedPicker` path
([pickBranchTarget](../src/solver_rec.cpp#L2312)) and replaces the additive
score combination with a multiplicative one driven by smooth, threshold-free
gates over structural and dynamic features.

## Problem

The current unified picker computes a single additive score per candidate:

```
S(x) = scoreOf(v) + cheapW·σ_static(v) + sep_bonus_m·1[v∈sep]      (if VAR)
S(x) = clauseW·sigmoid(β·(L−mid)) + sep_bonus_m·1[C∈sep]           (if CLAUSE)
```

with `sep_bonus_m = sepW · a^(−k_eff)` decaying in the carried separator's
relative size.

Three failure modes have shown up in practice across t1_049 / t1_071 /
t1_065:

1. **Implicit thresholds.** With monotone weights, argmax of a sum is, in
   the limit, argmax of the dominant term. The crossover point at which
   `sep_bonus_m` starts to matter is an *implicit* magic threshold determined
   by the ratio of weights — invisible, instance-dependent, and brittle.
2. **No graded contribution.** A separator of medium relative length should
   still tie-break among comparable candidates. With the current additive
   form, either the bonus dominates (short sep + high `sepW`) or it's
   irrelevant (long sep or low `sepW`). The "soft preference" regime is
   absent.
3. **Mode oscillation.** When two candidates have comparable additive
   scores, tiny BCP changes flip the winner between sibling decisions in
   the same component, leading to inconsistent branching strategies down
   the search tree.
4. **Type cross-contamination.** Var-branching and clause-branching are
   different actions (sum vs difference of children). Their quality
   measures are not commensurable as additive components of one score.

## Design Goals

- Single closed-form score, computed per candidate, no tier hierarchy and
  no piecewise/cliff thresholds.
- Smooth, monotone responses to the two structural features:
  *separator membership/length* and *predicted BCP cascade depth*.
- Type-pure base scores: variable quality and clause quality never sum
  inside a single candidate's score.
- Tunable surface that consists of **gain** and **decay-rate** parameters
  of smooth functions, not threshold cutoffs. Each parameter has a
  monotone, separable interpretation suitable for portfolio search.
- Preserves the existing infrastructure: cheap scores
  ([stage0_cheap_scores](../src/solver.cpp#L1213)), cascade scores
  ([computeCascadeScore](../src/solver.cpp#L1271)), and the carried
  `separator` vector.

## The Formula

For each active branch candidate `x` in the current sub-component:

```
base(x) =
    varW · max(ε, freq(v) + activity(v) + cheapW · cheap(v))      if x is variable v
    clauseW · sigmoid(β · (L(C) − mid))                            if x is clause C

boost(x) =
    1                                                              (default)
  + α · exp(−λ · rel_k)                                            if x ∈ separator
  + γ · max(0, depth(v) / depth_median − 1)                        if x is variable v
                                                                   (γ = 0 in Regime A)

S(x) = base(x) · boost(x)

pick = argmax_x S(x)
```

with the per-component features:

```
rel_k        = (active separator elements in this comp) / N_active_vars
depth(v)     = log₂( max(1, computeCascadeScore(v)) )       (only if γ > 0)
depth_median = median over a sampled subset of active vars
ε            = small positive floor (default 0.01) to keep base(v) > 0
```

## Component Definitions

### `base(x)`: type-pure quality score

The quality of a candidate **standalone**, without any structural
adjustment. Two type branches:

**Variable v.** The classical heuristic content, in the same units it
already has in [scoreOf](../src/solver.h#L687), floored to keep the
multiplicative form well-behaved:

```
base(v) = varW · max( ε, freq(v) + activity(v) + cheapW · cheap(v) )
```

`ε` defaults to 0.01. The floor prevents the rare-but-real case where
a variable in only long clauses with fully-decayed activity would have
`base = 0`, becoming structurally invisible to the boost (boost is a
multiplier, not an absolute additive). With the floor, any variable in
at least one active clause is always a viable candidate; the boost
then determines whether it is competitive against stronger alternatives.

A startup-time and per-call assertion check should verify `base(x) > 0`
for every active candidate. If the assertion fires, the floor is
masking a deeper bug (e.g., freq computed incorrectly) that should be
fixed before tuning the boost shape.

`freq(v)` is the component-level literal-occurrence count.
`activity(v)` is the VSIDS-style decay activity.
`cheap(v)` comes verbatim from
[stage0_cheap_scores](../src/solver.cpp#L1213): a static, length-decayed
clause-density proxy

```
cheap(v) = Σ over active clauses C ∋ v of  2^(−α_decay · active_len(C))
         + (#active binary partners of v) · 2^(−2·α_decay)
```

`α_decay` is `config_.stage0_length_decay`, auto-tuned at search start
(see [autoSelectStage0Decay](../src/solver.cpp#L349)).

**Clause C of length L (in active literals).** A length-sigmoid gated
quality:

```
base(C) = clauseW · 1 / (1 + exp(−β · (L − mid)))
```

with `β = config_.clause_length_steepness`,
`mid = config_.clause_length_midpoint`. Gated by
`L ≥ config_.clause_branch_min_length` so binaries are never considered
as clause candidates (they collapse to var-branching anyway).

**Critical separation:** a variable's score never sums a clause-shaped
term and a clause's score never sums a variable-shaped term. The
between-type competition happens only through the relative magnitudes
of `varW` and `clauseW`.

### `boost(x)`: structural multiplicative bonus

A **multiplier** ≥ 1 reflecting how much the structural evidence at
this node argues for picking this candidate beyond its standalone
quality. Two contributions, both ≥ 0, summed:

#### Separator contribution

```
boost_sep(x) = α · exp(−λ · rel_k)    if x ∈ separator
             = 0                       otherwise
```

The boost is **constant across all separator elements** at a given
node — it does not depend on which other candidates happen to be in
the separator. Within-separator ranking is preserved purely through
`base(x)`, since for any two separator elements x and y:

```
S(x) / S(y) = base(x) / base(y)
```

i.e., the strongest-base separator element wins among them. Good
separator candidates beat weak ones by exactly their base ratio.
This was a deliberate change from an earlier draft that included a
`q(x)/q_sep_max` factor — that factor introduced a non-locality
(one candidate's score depended on the distribution of other
candidates' base scores, breaking the standard locality property of
argmax) without adding expressive power that base-ranking didn't
already provide.

Two factors:

- `exp(−λ · rel_k)`: the **separator-shortness gate**. Smooth
  exponential decay in the relative separator size, with `rel_k`
  defined relative to active *variable* count only:

  ```
  rel_k = (active separator elements) / N_active_vars
  ```

  The vars-only denominator (rather than `N+M` mixing vars and
  clauses 1:1) matches the standard treewidth/separator-quality
  measure on the primal graph and avoids a portability artifact
  across instance classes with very different var:clause ratios.

  At `rel_k = 0` the gate is 1 (maximum pull); at `rel_k → 1` it
  asymptotes to `e^(−λ)` (negligible). No threshold. Concrete shape
  with `λ = 5`:

  | rel_k | exp(−λ·rel_k) |
  |---|---|
  | 0.00 | 1.00 |
  | 0.05 | 0.78 |
  | 0.10 | 0.61 |
  | 0.20 | 0.37 |
  | 0.40 | 0.14 |
  | 1.00 | 0.007 |

  A medium separator (`rel_k = 0.2`) still contributes 0.37·α — a
  real tie-breaker, not all-or-nothing.

  The instance-portability claim ("same `λ` works across instance
  classes") is a **testable hypothesis**, not a theorem. The
  validation requires sweeping `λ` on at least two instances with
  contrasting var:clause ratios and checking that the optimal `λ`
  agrees within the noise. If it doesn't, the denominator choice
  is wrong — not the gate shape.

- `α`: the **gain**. With `α = 5`, a separator candidate at
  `rel_k = 0` gets `boost = 1 + 5 = 6` — a 6× score multiplier on
  top of its base quality. With `α = 1` it becomes 2×.

#### Cascade contribution

```
boost_cas(v) = γ · max(0, depth(v)/depth_median − 1)    if v is a variable
             = 0                                         otherwise (clause)
```

Built on [computeCascadeScore](../src/solver.cpp#L1271), the existing
depth-bounded BCP-walk that returns `2^k` for a chain of `k` forced
literals on the binary-implication-graph side. We convert to a
predicted-depth scale:

```
depth(v) = log₂( max(1, computeCascadeScore(v)) )
```

so depth is in units of "expected forced-literal cascade length on the
weaker polarity." A variable that triggers no cascade has depth 0;
one that triggers a chain of 4 on its weaker polarity has depth 4.

The boost is **relative to the local median**, not absolute:

- `max(0, depth(v)/depth_median − 1)` is 0 for any var at or below the
  median (no penalty), and grows linearly with how far above the
  median the cascade signal is.
- A var with depth = 2·depth_median gets boost 1·γ.
- A var with depth = 5·depth_median gets boost 4·γ.

Why median-relative: it auto-calibrates per node. On a "cascade-rich"
component (most vars trigger long chains), only the top outliers get
boosted; on a "cascade-poor" component, modest absolute cascades are
correctly recognized as locally exceptional. No absolute threshold.

Why linear-above-median, not exponential: `computeCascadeScore` already
returns `coeff^k = 2^k`. Taking `log₂` gives k. We want the score
contribution to be **linear in k** ("each additional forced literal
buys one unit of preference") — this is the natural, local-evidence
linear-in-bits framing.

#### Cascade cost: two regimes

`computeCascadeScore` is bounded by `cascade_score_depth` and walks
the binary-implication graph. Per-call cost is O(b·d) where b is the
binary-clause branching factor and d is the depth bound. Across all
active vars per `pickBranchTarget` invocation, the cost is
O(n_active · b·d) — and unlike `cheap`, this cannot be cached across
calls because the trail (and hence the active partner set in
`binary_links_`) changes between calls. Earlier prototypes that
naively computed cascade per var per call regressed wall-time. So
the cost question is real, not hand-waveable.

The redesign splits delivery into two regimes:

**Regime A — γ = 0 (default first ship).** Cascade boost off. The
picker reduces to type-pure base + separator boost only. No
per-call cost regression risk; this is the regime in which Phase 1
ships. It validates the multiplicative form and the separator
gate independently of the cascade-cost question.

**Regime B — γ > 0 with sampled-median + lazy candidate evaluation.**
Cascade boost enabled. Two cost levers applied together:

- **Sampled `depth_median`**: estimate the median over a uniform
  random sample of K = 30 active vars rather than the full set.
  Bounds the median-estimation cost at O(K·b·d) regardless of
  `n_active`.
- **Lazy per-candidate `depth(v)`**: iterate candidates in
  descending `base(x)` order. For each candidate, compute its
  upper-bound score `S_ub = base(x) · (1 + α + γ · max_observed_ratio)`
  where `max_observed_ratio` is the largest `depth(v)/depth_median`
  seen so far. If `S_ub` is below the running best `S`, skip the
  cascade computation entirely. Most candidates are pruned without
  paying the cascade cost.

Regime B is sound (still produces argmax over the full set) but
needs profiling. Ship Regime A first; gate Regime B behind a
separate flag and enable only after profiling confirms acceptable
overhead on the documented baselines.

#### Combined boost

```
boost(x) = 1 + boost_sep(x) + boost_cas(x)
```

The two contributions add **inside the multiplier**, not at the
top-level score. So a variable that is BOTH in a short separator AND
has an outlier cascade is multiplicatively rewarded for having both
properties — which is correct: those are the dream candidates.

### `S(x) = base(x) · boost(x)`

The picker chooses `argmax_x S(x)`. One pass over all active vars and
qualifying active clauses. Within-type ranking is preserved (boost is
strictly positive for each candidate of a given type, so multiplying
monotone within type). Across types, the relative magnitudes of
`varW · base_var_max` and `clauseW · base_clause_max` (each times their
respective boosts) decide.

## Why Multiplicative

The user-specified semantics is "exponentially preferred when feature
strong, gracefully fading, never penalizing." Three combinator
candidates were considered:

| Combinator | Behaviour | Verdict |
|---|---|---|
| Additive (`base + bonus`) | Bonus magnitude must be tuned against base magnitude. Crossover is implicit. | ✗ This is exactly the failure mode of the current implementation. |
| Max (`max(base, bonus)`) | A candidate strong on two axes scores the same as one strong on a single axis. Information discarded. | ✗ Wrong on compound evidence. |
| Multiplicative (`base · (1 + bonus)`) | Bonus modifies the *rate* at which we bet on this candidate, proportional to its standalone quality. Compound evidence amplifies. | ✓ Matches the verbal spec. |

Concretely:

- A high-base candidate in a short separator gets `base · (1 + α)`.
- A low-base candidate in a short separator gets `low_base · (1 + α)` —
  amplified, but capped by the small `low_base`. It cannot override a
  much better base candidate elsewhere.
- A high-base candidate outside the separator gets `high_base · 1`.
  Whether it beats the boosted separator candidate depends on the
  base ratio vs `(1 + α)`. With `α = 5`, the separator candidate wins
  unless the alternative has > 6× the base quality. This is the
  desired soft dominance regime.

## Tunable Parameters

Five new shape parameters, each smooth and separable:

| Parameter | Role | Reasonable initial range |
|---|---|---|
| `varW` | Default preference for variable branching | 1.0 (anchor) |
| `clauseW` | Default preference for clause branching, relative to varW | 0.3 – 3.0 |
| `α` (sep gain) | Max separator boost magnitude when rel_k=0 | 2.0 – 8.0 |
| `λ` (sep decay) | Rate at which separator preference fades with rel_k (vars-only denominator) | 3.0 – 10.0 |
| `γ` (cas gain) | Max cascade-outlier boost gain. **Default 0 in Regime A**; enable in Regime B with sampled-median + lazy evaluation. | 0.0 (Regime A) / 0.5 – 4.0 (Regime B) |
| `ε` (base floor) | Floor on `base(v)` to keep multiplicative form well-defined | 0.01 |

The existing parameters keep their roles inside `base(x)`:

- `cheapW` (cheap-score weight inside variable base)
- `β`, `mid` (clause-length sigmoid shape)
- `clause_branch_min_length` (binary cutoff)
- `stage0_length_decay`, auto-tuned (cheap-score length decay α)
- `cascade_score_depth` (BCP-walk depth bound)

None of the new parameters is a threshold. `λ` controls a decay rate;
`α` and `γ` control gains; `varW`/`clauseW` is a relative anchor.
Crucially, each has a **monotone** effect on solver behavior over its
useful range, so portfolio sweeps do not have to navigate
discontinuities.

## Mapping from Current Parameters

The existing `-unifiedPicker` flag set largely survives, with these
mappings:

| Old parameter | New role |
|---|---|
| `separator_bias_weight` | Becomes `α` (separator gain). |
| `separator_importance_base` (`a`) and `separator_size_norm_p` (`p`) | Replaced by `λ`. The current `a^(−k_eff)` with `k_eff = k/(N+M)^p` collapses to `exp(−λ·rel_k)` with `λ = log(a)·(N+M)^(1−p)`. The new shape is dimensionless and instance-portable. |
| `cascade_score_weight` | Becomes `γ` (cascade gain). The role moves from "additive bonus inside scoreOf" to "median-relative multiplicative boost." |
| `cheap_score_weight` (`cheapW`) | Unchanged — stays inside `base(v)`. |
| `clause_score_weight`, `clause_length_midpoint`, `clause_length_steepness` | Unchanged — stay inside `base(C)`. |

## Implementation Plan

### Phase 1: side-by-side mode

Add `-pickerMode multiplicative` alongside the current additive path.
Both routed through [pickBranchTarget](../src/solver_rec.cpp#L2312); the
mode flag selects the score combinator. No behavior change for runs
without the new flag.

Steps:

1. Add `unified_picker_mode` enum to `solver_config.h`
   (`{ ADDITIVE, MULTIPLICATIVE }`, default ADDITIVE).
2. CLI flag `-pickerMode {additive|multiplicative}` in `main.cpp`.
3. New parameters `picker_alpha`, `picker_lambda`, `picker_gamma` in
   `solver_config.h`, defaulting to the values above.
4. In `pickBranchTarget`, when the mode is multiplicative:
   - Compute `rel_k`, `q_sep_max` after the existing per-call
     `sep_var_set` / `sep_clause_set` build.
   - Compute `depth_median` from `computeCascadeScore` over active vars
     (cache once per call).
   - For each candidate, compute `base(x)` with the type-pure formula,
     compute `boost(x)`, and track the argmax of `base · boost`.
5. The existing additive path stays untouched.

### Phase 2: cost optimization

The redesign calls `computeCascadeScore` per active variable per
`pickBranchTarget` call when `γ > 0`. On large components this is
dominant. Two cost levers, in order of preference:

1. **Compute once per node, reuse across both var loops** (separator
   filtering + main scoring loop). Already partially done via
   `stage0_cheap_scores`; extend to cache `depth(v)` similarly.
2. **Reduce `cascade_score_depth`** when `n_active` is small enough
   that the depth-bound is rarely hit anyway. Empirical only.

Profile on t1_049_k7 before deciding whether either is necessary.

### Phase 3: oscillation diagnostic — concrete metrics

The hypothesis that the multiplicative form produces more decisive
and more stable picks than the additive form is testable. Two
metrics, defined precisely **before** running so we don't invent
them to fit a result.

Instrument every `pickBranchTarget` call to write one log line:

```
log_picker:  comp_canon_key, picked_id, base, boost, S_best, S_2nd_best
```

`S_2nd_best` is the runner-up score (computed cheaply alongside the
argmax — track top-2 instead of top-1).

**Metric A — decision confidence gap.** Per call:

```
gap = (S_best − S_2nd_best) / S_best        ∈ [0, 1]
```

`gap` near 0 means the picker was on a fence between two candidates
(small perturbations could flip it). `gap` near 1 means a clear
winner. Aggregate as the **mean gap** and the **10th-percentile gap**
across all picker calls in the run. Higher on both is better.

The multiplicative form should produce higher gaps because the
boost is amplifying the dominant-feature signal rather than adding
small bonuses across multiple terms. If the gap distribution is
**not** improved over the additive form, the multiplicative redesign
is not delivering its claimed semantic advantage.

**Metric B — same-component pick stability.** For each canonical-key
`C` visited at least twice during the run:

```
mode_share(C) = max_x #{calls on C that picked x}  /  #{calls on C}
```

`mode_share = 1` means the picker always picked the same element on
the same component (no oscillation). `mode_share < 1` means it
flipped between candidates across visits. Aggregate as a CDF over
`C` weighted by visit count.

Multiplicative should shift the CDF to the right (more components
picked the same way every visit). If it does not, the oscillation
hypothesis is wrong and we have to find a different explanation
for why the additive form misbehaves.

Both metrics are computed entirely from the picker log and require
no timing measurement. They are falsifiable independently of any
performance result.

### Phase 4 (open question): portfolio integration

Phase 4 in an earlier draft was framed as a planned phase. That
overclaimed: portfolio search over the parameter surface is a
separate engineering project, and timing-signal noise is the genuinely
hard part that no redesign of the score formula can address.

Restated honestly: the deliverable from Phase 1 is **not** "find the
optimal parameter setting." It is "find ONE manually-chosen setting
that demonstrably beats the additive form on the documented
baselines." That is a feasible single-engineer-day exercise.

The "monotone" claim about the parameter surface is what makes
manual sensitivity analysis tractable: sweep `λ` alone on one
instance, find a good setting, then sweep `α` alone, etc., under
the assumption of approximate parameter independence. This is **not**
coordinate descent in the optimization sense; it's targeted
sensitivity analysis to find a starting point.

Whether any of this scales to a real auto-config / portfolio driver
is an open question. The `5⁴ = 625` coarse-grid number is a
real cost; coordinate descent on noisy timing is hard; instance-
class transfer is unproven. None of these are blockers for the
multiplicative-picker redesign per se — they are the pre-existing
challenges of portfolio search applied to whatever score formula
ends up in place.

## Worked Example (Regime B, γ > 0)

Consider a sub-component with `N_active_vars = 80`, a carried
separator of size 3 (2 vars, 1 clause), so `rel_k = 3/80 = 0.0375`.
With `λ = 5`, the separator-shortness gate evaluates to
`exp(−0.1875) = 0.83`.

Three candidates:

| Candidate | type | base | in sep? | depth | depth/depth_med |
|---|---|---|---|---|---|
| v_1 (sep var) | variable | 12.0 | yes | 3 | 1.5 |
| v_2 (cascade outlier) | variable | 8.0 | no | 6 | 3.0 |
| C_3 (long clause) | clause | 14.0 | no | — | — |

With `α = 5`, `γ = 1`:

```
v_1: boost = 1 + 5·0.83 + 1·max(0, 1.5 − 1) = 1 + 4.15 + 0.5 = 5.65
     S    = 12 · 5.65 = 67.8

v_2: boost = 1 + 0 + 1·max(0, 3.0 − 1) = 1 + 2 = 3.0
     S    = 8 · 3.0 = 24.0

C_3: boost = 1 + 0 + 0 = 1
     S    = 14 · 1 = 14.0
```

Pick: **v_1** (separator var, with both short-separator pull and a
modest cascade compound). Decision-confidence gap relative to v_2:
`(67.8 − 24.0) / 67.8 = 0.65` — a clear pick.

Now consider the same configuration but with the separator size
inflated to 20 elements (`rel_k = 0.25`, `exp(−1.25) = 0.29`):

```
v_1: boost = 1 + 5·0.29 + 0.5 = 1 + 1.45 + 0.5 = 2.95
     S    = 12 · 2.95 = 35.4

v_2: boost = 3.0   →   S = 24.0
C_3: boost = 1     →   S = 14.0
```

Still **v_1**, but the lead has narrowed (gap = 0.32). If v_2's
cascade depth were larger (say `4·depth_median`, giving boost = 4),
v_2 would overtake at S = 32 — exactly the intended graceful
handover from "use the separator" to "chase the cascade" as
separator quality degrades.

With the separator further inflated to 60 elements
(`rel_k = 0.75`, `exp(−3.75) = 0.024`):

```
v_1: boost = 1 + 5·0.024 + 0.5 = 1 + 0.12 + 0.5 = 1.62
     S    = 12 · 1.62 = 19.4

v_2: S = 24.0    ← winner
C_3: S = 14.0
```

The cascade outlier wins. No threshold was crossed; the smooth gate
simply diluted the separator pull below the cascade pull. In
**Regime A** (γ = 0), the third row of all three scenarios above
collapses: cascade contribution is zero, so v_2's score is just 8.0
and v_1 wins in all three cases. That's the conservative first ship
— the separator gate alone, without the cascade-cost overhead.

## Out of Scope

- **Hysteresis / parent-tier persistence.** The earlier proposal
  included a small recency bonus for the parent's chosen tier. The
  multiplicative form does not have tiers, but a similar effect can
  be achieved later by adding a per-subtree multiplicative prior
  on the dominant feature. Defer until baseline numbers are in.
- **Cache-aware scoring.** The expected #SAT-counting work is also a
  function of cache hit probability for the residual sub-components.
  The current design is greedy on local features only. A cache-aware
  extension is a separate research thread.
- **Replacement of the additive path.** Phase 1 keeps both. Removal
  of the additive path waits until the multiplicative one demonstrates
  no regressions across the documented t1_049 / t1_065 / t1_071
  baselines and at least one win.

## Success Criteria

The multiplicative picker (Regime A) is justified if **all** of the
following hold on the documented baseline set
([benchmark_log.md](benchmark_log.md)):

1. **Correctness.** Counts match across t1_049 (full / k6 / k7 / k10),
   t1_065, t1_071. The `base(x) > 0` assertion never fires during
   correctness runs.
2. **No regression.** No timing regression > 10% on any documented
   baseline relative to the additive `-unifiedPicker` form.
3. **Picker-quality signal.** Phase 3's two diagnostic metrics improve:
   - Mean and 10th-percentile **decision-confidence gap** strictly
     higher than additive on at least 4 of the 6 baselines.
   - **mode_share CDF** shifts right (more cache-revisited components
     pick the same candidate every visit) on at least 4 of the 6
     baselines.
4. **At least one timing win.** A measurable speedup on at least one
   baseline, with parameter setting found by the manual sensitivity
   sweep described in Phase 4.

Criteria 3 and 4 are independent. If 3 is met but 4 is not, the
formula is doing what it claims semantically but the speed payoff
isn't there — likely meaning the picker isn't the bottleneck on these
instances. If 4 is met but 3 is not, the speed win is real but its
attribution to the redesign is suspect — investigate whether some
unrelated side effect (e.g., a tie-break order change) is responsible.

The "monotone parameter sensitivity" claim from earlier drafts is
**downgraded to a hypothesis** to be verified during Phase 4's
manual sweep, not asserted upfront. If `λ` swept alone shows a
non-monotone timing response on a single instance, that's a signal
the gate shape is wrong, not a Phase-1 blocker.

Regime B (γ > 0) has additional success criteria, evaluated only
after Regime A ships:

5. **Cascade-cost overhead.** With sampled-median + lazy candidate
   evaluation, no `pickBranchTarget` call exceeds 2× the wall-time
   of the corresponding additive call on profiled traces.
6. **Cascade signal payoff.** At least one baseline shows a strict
   improvement under Regime B over Regime A (otherwise the cost is
   unjustified).
