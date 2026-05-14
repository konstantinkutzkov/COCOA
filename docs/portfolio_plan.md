# Portfolio Plan

**Status:** living document. Updated in dialogue; expect revisions as
measurements accumulate.
**Last substantive update:** 2026-05-14.
**This is the entry point.** A fresh agent should read this first, then
follow the cross-references in §10.

---

## 1. Goal

Build an analyzer that, given a CNF, picks the best **flag combination**
for our solver to count its models fastest. Empirically, every
non-trivial instance class has a configuration that beats alternatives
by 1.5×-30×, but no single fixed configuration wins across classes.
The portfolio's job is to route per-instance.

---

## 2. Mental model

The solver is a recursive #SAT counter (`solveComponent` →
`branchOnLiteral` / `branchOnClause` → `solveComponent`) with three
load-bearing mechanisms and one secondary one. Each is enabled or
shaped by flags; each contributes to "good performance" on different
instance classes.

| # | Mechanism | What it does | Primary flags |
|---|---|---|---|
| 1 | **Separator branching** | Use the precomputed ND hierarchy to pick a separator at each decision point; once consumed, the formula decomposes into independent sub-components whose counts multiply. | `-sep N`, `-cb N`, `-sepMode metis`, `-reactiveMetis [-reactiveMetisMin n -reactiveMetisSkip k]` |
| 2 | **BCP / propagation** | Pick variables whose assignment triggers long propagation chains; each decision shrinks the formula via UP. | `-adaptive -adaptiveAlpha f` (auto-picked by mean clause length), default Stage-3 picker (`freq + 10·act`) |
| 3 | **Cache amplification** | When the search visits canonically-equivalent sub-formulas via different decision paths, the count is retrieved from cache. Turns exponential trees into polynomial DAGs *when the picker produces stable residuals across paths*. | `-wlIter K`, anchor pinning (research-stage; see §5), the cache itself is always on |
| — | Conflict-driven learning | Standard CDCL; learn a clause when a path produces UNSAT, prune other paths. | `-learnLevel N` |

Critical property of mechanism 3 in our solver: **the L1 cache is
identity-keyed (raw active-var-IDs), the L2 cache is canonical (WL
cascade).** Cache amplification therefore depends not just on the
formula's structure but on whether the picker's branching policy
produces *the same physical labelings* across paths. Two structurally
isomorphic sub-formulas reached via different branch sequences may not
hit L1 — they only hit L2. This is why the solver is sensitive to
which physical variable is pinned (e.g. v242 vs v70 on t1_041 differ
30×) even when the residuals look isomorphic to the canonical-key
cascade.

### Conflict-driven learning's role

On the density-1 structured instances we care about, the solver records very
few conflicts (37 in 36k decisions for v242 on t1_041; 1 in 86k for
v70; 0 in some short runs of t1_021 family). Learning is therefore
not a primary lever. It stays at the safe default (`-learnLevel 4`)
unless explicitly diagnosing a soundness question.

---

## 3. The key empirical finding (2026-05-14)

For a long time the portfolio question was framed as **"find the right
*anchor variable* for density-1 structured instances"** — pin v as the first
decision and the search runs cheaply; pin a different v and it times
out. v242 vs v70 on t1_041 gave 30× swings, with no static feature
that reliably picked the good anchor.

Diagnostic on 2026-05-14 (see `portfolio_insights.md` §4 entry of that
date) showed the actual mechanism is **collapse of the ND hierarchy
under the bad anchor's BCP cascade**. The Phase-2 separator-acceptance
gate (`separator_max_ratio`, `separator_min_balance`) rejects the
precomputed cuts for the sub-components reached after a bad pin; the
search then falls through to plain variable branching outside the
hierarchy (`nd_node = -1` for 70% of decisions on v70 vs 30% on
v242). With no structural decomposition, the search wanders, generates
no conflicts, sees no cache amplification, never finishes.

**The fix: enable reactive METIS with aggressive throttle.** Result on
the same v70 anchor:
- Without reactive METIS: TIMEOUT > 60 s.
- With `-reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4`:
  **SOLVE in 88 s**, count verified, only 0.87% reactive-METIS
  overhead.

Implication: **the anchor problem reduces largely to a
parameter-selection problem.** The dominant question is no longer
"which variable to pin first" but "which solver flags to use."
Anchor choice still matters for residual speed (88 s vs 2 s on
t1_041), but it no longer determines SOLVE-vs-TIMEOUT.

This reframes the portfolio's central question: instead of searching a
huge variable space (~1000-2000 vars per instance), we search a small
flag space (~6 production flags, ~5-7 routing configs after pruning).

---

## 4. Production flag inventory

After the audit informed by this reframe, the flag space partitions
into three buckets.

### KEEP — measurable, persistent effect across multiple instances

| Flag | Effect documented |
|---|---|
| `-sep N` | 17.97 s → 10.71 s on t1_021 changing 5 → 50; order-of-magnitude swings |
| `-cb N` | Coupled to `-sep`; clause-branching threshold for long-clause separator elements |
| `-sepMode metis` | Default, no measured alternatives |
| `-adaptive -adaptiveAlpha f` | 2.47 s vs 4.05 s on t1_049 at α 0.5 vs 2.0 (50% swing); auto-picked from mean clause length post-preprocess |
| `-sepVarBias` | 30× swing between instance classes (helps on small + low-density, hurts on dense + large); routed by 2-feature rule |
| `-reactiveMetis [-reactiveMetisMin n] [-reactiveMetisSkip k]` | **NEW PORTFOLIO LEVER (2026-05-14):** TIMEOUT → SOLVE on density-1 structured instances when paired with aggressive throttle |
| `-learnLevel 4` | Default; level 5 unsound, lower removes useful features. Fixed unless diagnosing |
| `-wlIter K` | K=1 default; K=2 tested to be slower on super_d3_id8. Hyperparameter candidate but not yet shown to help on portfolio instances |

### DROP — falsified or research scaffolding with no measured win

These have been shown to lose to plain on every measured instance
(see `portfolio_insights.md` §2 "unified picker" entry). They remain
in the codebase for research, but should not be presented to the
portfolio analyzer as choices on production-routed instances.

- `-unifiedPicker` (in default-routing mode)
- `-pickerMode multiplicative`
- `-pickerRateFramework`
- `-cascadeW W`
- All `-picker*` tuning knobs (`-pickerAlphaVar`, `-pickerLambda*`,
  `-pickerGamma`, `-pickerVarW`, `-pickerClauseW`, `-pickerRhoExp`,
  `-pickerFrontBonus`, `-pickerNonSepKillsNd`, `-pickerNoCascadeGain`,
  `-pickerRootSepOnly`)
- `-cheapScoreW`, `-clauseScoreW`, `-clauseLenMid`, `-clauseLenBeta`,
  `-sepImpA`, `-sepSizeNormP`, `-sepBiasW`
- `-decomposeInSep`, `-decomposeAfterK` (mid-consumption decomposition;
  was off by default after measurement)

**However:** the unified picker is retained as a **last-resort
fallback** when the analyzer cannot confidently route an instance to
any known-good config. See §5 routing rule (E).

### DIAGNOSTIC — never ship in portfolio, keep behind a flag

Forensic instrumentation. Belongs behind a `--research` umbrella or
clearly-marked diagnostic surface. Not exposed to the analyzer.

- `-verifyCache`, `-noAnonymization`, `-canonStats`, `-l2Strict`
- `-bruteForceCacheCheck`, `-bruteForceCacheDumpDir`,
  `-dumpRecursionDir`, `-dumpRecursionMaxDepth`
- `-dumpPreprocessed`, `-dumpNDAndExit`, `-dumpCompDir`,
  `-dumpCompMinVars`, `-dumpCompMaxVars`
- `-perm*Seed`, `-permWatchSelect`, `-sortBinaryLinks`,
  `-sortWatchLists`, `-sortOccLists`, `-sortClauseLits`,
  `-sortClausePool` (order-sensitivity probes)
- `-logBranches`, `-logConflicts`, `-anchorTrace`, `-stopAtBranch`,
  `-pathTraceOfs`, `-pathTraceCompVars`
- `-learnTrace`, `-analyzeDynamic*`, `-analyzeClausePool`
- `-checkLearnInvariants`, `-implicantLearning` and family (closed
  per `history/`)
- `-forceDecisions` (research utility; used by anchor probes and
  diagnostic experiments)

### Net portfolio-routing flag space

After pruning, ~6 production decisions:
1. `-sep N` (3 buckets: small/medium/large)
2. `-adaptive` on/off + `-adaptiveAlpha` (auto-mapped from mean_len)
3. `-sepVarBias` on/off
4. `-reactiveMetis` on/off + throttle (3 settings: off, default, aggressive)
5. `-cb` (always coupled to `-sep`)
6. `-learnLevel` (fixed at 4)

Plus a fallback (`-unifiedPicker`) for unrouted instances. Total
distinct portfolio configurations to consider: ~5-7 after dropping
the dominated ones.

---

## 5. Routing rules (production decision tree)

Static features used:
- `n_active_vars`, `n_active_clauses` (post-preprocess)
- `density = n_active_clauses / n_active_vars`
- `mean_active_clause_length`
- `binary_fraction = n_binary_clauses / n_active_clauses`

Routing precedence (highest priority first):

**(A) Ganak-class detected** — `density ∈ [0.95, 1.10]` AND
`binary_fraction ≤ 0.1`:
- Configuration: **plain `-sep 5 -cb 3` + `-reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4`**.
  Optionally with `-sepVarBias` if also small (`n_active_vars < 200`).
- Validated on t1_041 (was TIMEOUT, now SOLVES at 88 s).
- Open: validate on t1_021 family, t1_023, t1_025, t1_027.
- **Last resort if even reactive METIS doesn't make progress within
  the user's budget**: route to the unified picker (rule E), NOT to
  ganak. Routing to ganak is intentionally avoided — the goal is for
  our solver to handle these classes. Falling back to ganak is a
  research-time observation, not a production routing target.

**(B) Sparse / well-decomposable** — `mean_active_clause_length ≥ 6`
AND `binary_fraction < 0.3`:
- Examples: t1_011, t1_065 (after preprocess), t1_071.
- Configuration: **plain `-sep 5 -cb 3`**. Hierarchy is essential and
  works as designed.
- `-adaptive` is irrelevant here (hierarchy handles decomposition;
  picker's choices at hierarchy leaves are usually correct enough).
- `-reactiveMetis` is **OFF** (measured -7% on t1_071; the precomputed
  hierarchy is good).

**(C) Dense 3-SAT** — `mean_active_clause_length < 3.5` AND not
density-1 structured:
- Examples: t1_049 and shrunken variants.
- Configuration: **`-sep 5 -cb 3 -adaptive -adaptiveAlpha 0.5`**.
  α = 0.5 critical (sparse-tuned default 2.0 is 50% slower).
- `-reactiveMetis` status: not yet measured on dense; provisional OFF.

**(D) Small + low-density (non-density-1 structured)** — `n_active_vars ≤ 200`
AND `density ≤ 1.5`:
- Examples: t1_021_k10_s1 (and likely the rest of the small t1_021
  family for which the `-sepVarBias` rule held).
- Configuration: **`-sep 5 -cb 3 -sepVarBias`**. Bias staleness
  doesn't develop in shallow ND trees.

**(E) Default — unknown / mixed / nothing else fits:**
- Configuration: **`-unifiedPicker`** with default settings (additive
  mode, picker_var/clause weights at defaults).
- Justification: the unified picker is a single scoring contest over
  all sep VARs, sep CLAUSEs, and free vars — it doesn't lock onto
  any one mechanism, so when we don't know which mechanism dominates
  for the instance, it's the most general-purpose path.
- This is the **last resort**, not the first choice. It loses to
  plain on every measured instance class with a fitting rule, but
  provides a safety net when no rule fits.

For a prior version of these rules (without the reactive METIS lever
or the unified-picker fallback), see `portfolio_insights.md` §8.

---

## 6. Probe-based refinement (the stochastic walks probe)

Static features cover most routing decisions. The cases they don't
cover well are the ones a probe should refine:

- **"Is the ND hierarchy actually working on this formula?"** — i.e.,
  should reactive METIS be enabled? Static features `(density,
  binary_fraction)` give a coarse hint; the probe can directly
  observe the Phase-2 rejection rate during random walks.
- **"Which `-sep N` threshold?"** — the right `N` scales with
  `n_active_vars` but the proxy `min(50, n/4)` is unvalidated; a
  probe could measure decomposition quality at different N.
- **"Should adaptive be on?"** — currently on/off depends on whether
  α is auto-picked confidently; probe could measure cascade strength
  directly.

### Probe contract

A short stochastic walk through the search tree (see §6.1) gathers:

- Mean BCP cascade per decision
- % of decisions inside the ND hierarchy (vs `nd_node = -1`)
- Phase-2 rejection rate
- Cache hit rate by component-size bucket
- Decomposition events per K decisions
- Mean leaf depth, walks completed

These signals map to parameter decisions via rules of the form:

| Signal | Action |
|---|---|
| Phase-2 rejection > threshold | Enable reactive METIS |
| `% nd_node = -1` > threshold | Enable reactive METIS |
| BCP cascade > threshold | Stay default; -adaptive unnecessary |
| Cache hit rate on [400+) low | Try aggressive reactive throttle |
| ... | ... |

The probe's job is **NOT** to rank anchors. The anchor problem is
deferred (§7).

### 6.1 Stochastic walks probe — implementation outline

(Absorbed and pruned from the previous `stochastic_probe_plan.md`,
moved to `history/`.)

The probe runs the solver in a modified mode: at each branching
decision, with probability `p` (~0.9) **skip** (commit to one
polarity, no backtrack point recorded); with probability `1-p`
**branch normally** (record a backtrack point, try both polarities on
UNSAT). Walks are bounded by wall-clock budget (typically 0.25-1.5
s).

Properties:
- Many paths explored per budget; cache fills naturally; cache hits
  become measurable.
- Polarity sampled uniformly at random (per 2026-05-14 design).
- Cache stays on; learning OFF (`learn_level=0` during probe).
- Counts produced are meaningless — we don't read them.
- Same DIAG_STATS / L2_HIT_HIST / FULL_CACHE_STATS infrastructure
  emits the signals.

**Status:** not yet implemented in the solver (still Python prototype
at `/tmp/probe_runtime.py`). When implemented, ~50-100 lines hooking
into the picker site at `solver_rec.cpp:~1987` plus a budget check
in BCP entry.

The Python prototype already shows distinctive per-instance regime
signatures (BCP-rich vs BCP-starved vs compute-bound); the in-solver
version would have access to the real cache, giving richer signals.

### 6.2 What the probe will NOT do

- Rank anchor candidates. Anchor selection is a separate Mechanism-3
  question (§7), and the depth-bounded structural probe approach
  (`history/anchor_probe_design.md`) was empirically falsified
  2026-05-14: it can't distinguish v242 from v70 even though they
  produce 30× full-solve time differences.
- Predict full-solve runtime. The probe characterises the solver's
  *behaviour regime* on the formula; the analyzer maps regime to
  configuration.

---

## 7. What's deferred / falsified / on the back burner

Items that are not on the critical path right now, with brief notes
and pointers to history docs.

### Anchor-selection probing (Mechanism 3 specifically)

The depth-bounded structural anchor probe in
`history/anchor_probe_design.md` was empirically falsified on
2026-05-14: v242 (fast anchor) and v70 (TIMEOUT anchor) produce
bit-identical structural metrics under the K=8, B=256 enumeration
because their post-pin residuals are isomorphic to the canonical-key
cascade. The 30× full-solve gap emerges in dynamic search behaviour
(conflict generation, cache amplification at scale), not in static
depth-K enumeration. **Slice 1 of the anchor probe (the
`-forceDecisions` root-level fix and the `scripts/probe_anchor.py`
runtime probe wrapper) remains useful** as the runtime min-rate probe
primitive. Slice 2 (the depth-K enumeration body) is committed but
should be considered research scaffolding.

The runtime min-rate probe (1-second solver run with a candidate var
pinned, measure cache hit rate on large components) is a working
mechanism for ranking anchors when needed. It just isn't the
critical path anymore — see §3.

### Unified picker as default

Falsified on every measured instance. Retained only as a fallback
(rule E above). Implementation lives in `solver_rec.cpp` behind
`-unifiedPicker`. Design notes in `history/unified_picker_redesign.md`.

### Implicant learning

Phase 4, shipped 2026-04-21, sound but uniformly 3-4× slower than
default. Off by default. Not on the portfolio path.

### Mid-consumption decomposition (`-decomposeInSep`)

Off by default. The per-call connectivity check dominates on
instances with rare disconnects (t1_071). Not currently on the
portfolio path.

---

## 8. Implementation roadmap

Ordered by leverage and feasibility, smallest first.

### 8.1 Done as of 2026-05-14

- `-forceDecisions` fix (root-level enqueue) — committed `99c34e2`.
- `scripts/probe_anchor.py` (in-process runtime min-rate probe) —
  committed `99c34e2`.
- Anchor probe slice 1 (CLI + dispatch + pin/BCP/rollback primitive)
  — committed `e14a336`. Useful infrastructure even though the depth-K
  enumeration body (slice 2) is a research dead-end.
- Anchor trace instrumentation (`-anchorTrace`) — uncommitted as of
  this writing; should be committed under DIAGNOSTIC umbrella.

### 8.2 Immediate (next 1-2 sessions)

1. **Commit the anchor-trace instrumentation** as a DIAGNOSTIC tool,
   alongside today's planning consolidation.
2. **Validate the aggressive reactive-METIS routing on the
   calibration set.** Run with `-reactiveMetis -reactiveMetisMin 10
   -reactiveMetisSkip 4` on t1_065, t1_071, t1_011, t1_049 (one each,
   single short timeout) to verify it doesn't regress them. If any
   regress, the rule needs an instance-feature gate.
3. **Replicate the t1_041 + reactive-METIS rescue on t1_021_k4_s1**
   (the other persistent density-1 structured TIMEOUT). If reactive METIS
   rescues that too, the routing rule (A) is empirically validated.

### 8.3 Short-term (next ~few sessions)

4. **Stochastic walks probe in the solver.** Implement
   `--probeWalkBudget T` and `--probeSkipProb p` at the picker site;
   reuse existing stats; emit a single `PROBE_STATS` line on early
   exit with the signals named in §6. ~50-100 lines.
5. **First rule: "should reactive METIS be enabled?"** Map the
   probe's `% nd_node = -1` signal to the on/off decision. Validate
   against the calibration set.
6. **Drop / archive the unified-picker variants documented as
   falsified.** Keep `-unifiedPicker` (the default additive mode);
   remove or `--research`-gate the multiplicative / rate-framework /
   picker-tuning flags. Reduces the production CLI surface.

### 8.4 Medium-term

7. **Build the analyzer's static-feature extraction** as a single
   pre-solve pass. Output a `(n_active_vars, density, mean_len,
   binary_fraction)` tuple plus the routing decision. Currently this
   is implicit in `analyzeAndSetHyperparameters`; lift it into a
   structured analyzer module per `portfolio_driver_plan.md`.
8. **Sequential-fallback driver.** Driver tries primary
   configuration with budget T1; on TIMEOUT, falls back to secondary
   with T2. The driver is what consumes the routing decision tree;
   the analyzer just produces the configuration sequence.

### 8.5 Long-term

9. **Multi-process portfolio.** Run K configurations concurrently;
   first to finish wins. Per `portfolio_driver_plan.md` §4 capability
   level 4.
10. **Top-level branching with partial counts.** Per
    `portfolio_driver_plan.md` capability level 3. Out of scope for
    near-term.

---

## 9. Discipline / how to update this document

- **Append to `portfolio_insights.md` §4 first when you have a new
  measurement.** That's the empirical record. This document then
  references it.
- **Update §5 routing rules here when a measurement changes a
  routing decision.** Don't let the routing rules drift from
  evidence.
- **When a flag becomes DROP or KEEP, edit §4 here directly.**
- **Don't add new docs at the top level for new plans.** Either
  extend this one or absorb the new plan into the relevant section.
  We just consolidated a dozen docs; let's keep the entry-point
  clean.
- **Move closed work to `docs/history/`** as it's superseded.

---

## 10. Cross-references

| Doc | Role |
|---|---|
| `docs/portfolio_insights.md` | Empirical knowledge base. §1 instance taxonomy, §2 per-flag notes, §4 dated measurements, §8 prior routing rules (mostly superseded by §5 here). |
| `docs/portfolio_driver_plan.md` | Architectural sketch for the multi-process driver. Implementation deferred. |
| `docs/benchmark_log.md` | Per-run measurements with commit hashes. |
| `docs/probe_preprocessing_plan.md` | Different probe track — diff-and-lift sound preprocessing. Parked. |
| `docs/history/` | Closed / falsified / superseded plans, kept for reference. Includes the original handoff doc, the depth-K anchor probe design, the stochastic probe plan (absorbed into §6 here), the unified-picker redesign, the bug investigations. |
| `scripts/probe_anchor.py` | Runtime min-rate anchor probe (used when anchor selection is needed; not on critical path). |
| `src/anchor_probe.{h,cpp}`, `src/solver.{h,cpp}` | Anchor-probe wiring + stub. |
| `src/solver_config.h` | All solver flags as a config struct. |

---

## 11. One-paragraph summary

We have a #SAT solver with several branching mechanisms (separator,
BCP, cache amplification, learning) and a flag space large enough that
no single configuration wins across instance classes. The portfolio's
job is to pick flags per-instance. We previously thought the dominant
hard problem was anchor selection on density-1 structured instances. As of
2026-05-14, the diagnosis is sharper: bad anchors collapse the ND
hierarchy via the Phase-2 separator-acceptance gate, and reactive
METIS with aggressive throttle (`-reactiveMetisMin 10
-reactiveMetisSkip 4`) rescues this — t1_041's previously-TIMEOUT v70
anchor now solves in 88 s. So the central question shifts from
"which variable to pin" to "which solver flags to use", which lives in
a much smaller search space (~6 flags, ~5-7 routing configs). Static
features cover most routing; a stochastic walks probe (still
unimplemented in the solver) refines the cases static can't decide.
The unified picker is retained as a safety-net fallback when the
analyzer can't confidently route. Routing to ganak is intentionally
avoided as a production target.
