# Stochastic-Skip Probe — Implementation Plan

**Status:** design only, no code.
**Date:** 2026-05-13
**Author of this turn:** the agent picking up `probing_portfolio_handoff.md`;
plan refined in dialogue with the user.

Companion to:
- `docs/probing_portfolio_handoff.md` — the conceptual handoff for the
  portfolio probing programme.
- `docs/portfolio_driver_plan.md` — the architectural sketch for the
  driver that will eventually consume probe signals.
- `docs/portfolio_insights.md` — empirical knowledge base (§4 dated
  measurements, §8 current routing rules).
- `scripts/probe_anchor.py` — slice 1 of the probing programme
  (variable-pinning anchor probe), shipped 2026-05-13.
- `/tmp/probe_runtime.py` — pure-Python prototype of the mechanism
  this plan implements in the solver.

---

## 1. What this is

A native solver-mode probe that characterises a configuration's
**runtime dynamics** on a formula via many fixed-budget walks through the
search tree. At each decision the probe flips a biased coin:

- With probability `p` (≈ 0.9): **skip** — commit to one polarity, do not
  record a backtrack point, descend.
- With probability `1−p`: **branch** — record a backtrack point, branch
  both polarities normally.

The walk terminates when (a) all variables are assigned (leaf), (b) BCP
derives a conflict (leaf), or (c) the wall-clock budget is hit. After
(a)/(b) the search backtracks to the most recent recorded branch point;
if none, the walk is done and a new walk starts from root.

The count produced is **wrong** — we accept that. What we get instead is
fast aggregate signals (decisions/sec, BCP per decision, cache hit rate
on large components, leaf depths) that classify what the configuration
*does* on this formula. Two configurations with very different signal
vectors on the same formula behave very differently in a full solve.

---

## 2. Why this plan exists separately from the handoff doc

The handoff doc (§5) says the solver-mode probe is the single highest-
leverage next engineering task, estimated at ~50-100 lines in
`solver_rec.cpp`. Reading the surrounding code revealed two design
questions the handoff glosses over (Stage-2 vs Stage-3 skip semantics;
learning-during-probe semantics) and one prerequisite that turned out to
not be free (`-forceDecisions`, fixed in slice 1, 2026-05-13). This plan
captures the resolved decisions and the remaining open questions, so the
implementation slice does not re-derive them.

---

## 3. Mechanism (the contract)

### 3.1 Skip-walk semantics

At every branching decision (both Stage-2 separator consumption and
Stage-3 variable branching), draw a uniform `r ∈ [0,1)` from a seeded
RNG:

- `r < probe_skip_prob`: pick one polarity (default: the polarity that
  the existing picker would have tried first — `t_first` for VARs;
  `removed_clauses_` semantics for clauses), recurse on that polarity
  only, return that recursion's count as the local result. **No
  backtrack point recorded.**
- `r ≥ probe_skip_prob`: standard behavior — branch both polarities,
  combine (sum for VAR, difference identity for CLAUSE), return.

A recursion that took the skip path may still encounter BCP conflicts
and recorded branch points further down; those backtrack normally. The
skip only affects whether *this particular* decision contributes a
backtrack point.

### 3.2 What is preserved

- **BCP.** Unchanged. Every assignment triggers BCP to fixpoint as in
  normal search.
- **Cache.** Stays on. Lookups and stores happen exactly as in normal
  search. Hit/store stats are accumulated normally; they are *the*
  primary signals the probe emits.
- **ND hierarchy descent.** Unchanged. Separator consumption order
  follows the precomputed cuts.
- **Reactive METIS.** Inherits the user's `-reactiveMetis` flag; the
  probe does not force it.

### 3.3 What is changed

- **Learning during the probe is OFF by default** (`learn_level=0`
  during a probe run). Reason: walks under skip semantics visit
  unsound count paths; clauses learned on those paths are not
  resolution-derivable from the original formula and would corrupt the
  cache / pool for any downstream solve sharing the process. If a
  future caller wants probe-time learning they can override.
- **The budget cutoff is honored at every BCP entry**, not only at the
  current `time_bound_seconds` check points. The probe budget is
  typically much smaller (~0.25–1.5 s) than full-solve budgets, so the
  existing coarse check can overshoot meaningfully.

### 3.4 What is accepted as wrong

- **The count.** Skip-walks do not enumerate all paths; the returned
  count has no logical meaning. Callers must not consume it.
- **Soundness of statistics.** Decisions, cache stores/hits, BCP
  propagations are all true counts of what happened during the probe.
  They are sound *measurements*, even though the count they would have
  produced under faithful search is not the value returned.

---

## 4. Resolved design decisions

| # | Decision | Rationale |
|---|---|---|
| D1 | Skip applies to BOTH Stage-2 separator consumption AND Stage-3 variable branching. | On hierarchy-rich instances (t1_071 etc.) Stage-2 dominates. Skipping only Stage-3 would not see the dominant work. |
| D2 | Caches stay on (L1 + L2). | Cache amplification *is* the signal we want to measure. |
| D3 | Learning OFF during probe (`learn_level=0` override). | Walks visit unsound count paths; learned clauses on those paths are not entailed by F. |
| D4 | Budget cutoff checked at every BCP entry, not only periodically. | Probe budgets are short; coarse checks overshoot. |
| D5 | PRNG seeded by `--probeSeed` (default 42, matching the Python prototype). | Reproducibility. |
| D6 | Probe stats reuse existing `DIAG_STATS` / `L2_HIT_HIST` / `FULL_CACHE_STATS` lines. | They already emit on early exit via `-t`. Adding a new aggregate would duplicate. One new `PROBE_STATS` line summarises walk count + mean leaf depth (signals not in the existing triple). |
| D7 | When `probe_skip_prob > 0`, `-forceDecisions` still pins at root (as fixed 2026-05-13). | Many probes will want to pin a candidate anchor and then skip-walk under that pin. |
| D8 | Stage-2 CLAUSE skip uses the SAME difference identity, but only on one polarity. | i.e. on skip, pick removed-vs-kept arbitrarily and descend; do not compute the diff. The signal we want is "what happens during search on this configuration"; the actual identity does not matter for that. |

---

## 5. Open design decisions

| # | Question | Current best guess |
|---|---|---|
| O1 | Single-process multi-probe mode: should one `sharpSAT` invocation iterate over a list of candidate forced literals, emitting one stats block per probe? | Defer. Subprocess overhead is ~80 ms on M-series; for 60 candidates that's ~5 s of overhead on top of the ~15 s probe work. Not a bottleneck unless we widen to 400+ candidates. Add later if cost matters. |
| O2 | Should the probe also disable the failed-literal test (`perform_failed_lit_test`)? | Probably yes — it's an extra BCP cost per branch that would distort decisions/sec. But it's also part of the *real* configuration's runtime; if we want to predict full-solve cost we should preserve it. Resolve via a `--probeRealisticBcp` flag, default OFF. |
| O3 | What about preprocessing? Probes don't want preprocessing to run repeatedly for every (var, polarity) pair. | Slice-1 evidence: preprocessing is ~5 ms on t1_041 (DIMACS parse dominates). At that cost it's free to repeat. Revisit only if probing a 10k-clause instance shows it as a bottleneck. |
| O4 | When the recursion returns from a skip path with a "count" that is meaningless, what do we *report*? | The driver layer reads stats from stderr, not the count from stdout. So we emit the (wrong) count for protocol compatibility, but a `PROBE_STATS` line includes a `unsound_count=1` flag so consumers know to ignore it. |
| O5 | Polarity choice on skip: first polarity tried by existing picker (preserves picker's intent) or random? | Default to picker's `t_first`. Add `--probeRandomPolarity` for the alternative if we ever want it. |

---

## 6. Implementation outline

### 6.1 Files touched

| File | Change | Approx LoC |
|---|---|---|
| `src/solver_config.h` | Add `probe_skip_prob` (double, default 0), `probe_seed` (unsigned, default 42), `probe_budget_seconds` (double, default 0 = no probe). | +15 |
| `src/main.cpp` | Parse `--probeSkipProb`, `--probeSeed`, `--probeBudget`. When `probe_budget_seconds > 0`, also set `learn_level = 0`, `time_bound_seconds = probe_budget_seconds`. | +30 |
| `src/solver_rec.cpp` | The skip-coin at the Stage-2 and Stage-3 branching sites. Plus a member RNG. | +40 |
| `src/solver.cpp` | Per-BCP-entry budget check. Emit `PROBE_STATS` on early exit if probe mode is on. | +20 |
| `src/solver.h` | RNG member declaration; `is_probe_mode()` helper. | +5 |
| `docs/portfolio_insights.md` | New §4 entry on first probe-mode measurements (after the implementation lands, not now). | + later |

Estimated total: ~110 lines new code, no deletions.

### 6.2 Code sketch at the Stage-3 branching site

[solver_rec.cpp:1987-1997](src/solver_rec.cpp#L1987-L1997) post-slice-1
shape:

```cpp
v = config_.perform_adaptive_branching
        ? pickBranchVariableAdaptive(comp, comp_unsat)
        : pickBranchVariable(comp);
if (v == 0) return 1;
LiteralID lit_t(v, true), lit_f(v, false);
bool t_first = literal(lit_t).activity_score_ >
               literal(lit_f).activity_score_;

if (in_probe_mode() && probe_rng_() < config_.probe_skip_prob) {
    // Skip: descend one polarity only.
    return branchOnLiteral(t_first ? lit_t : lit_f, comp, {}, false,
                           depth, -1, /*from_separator=*/false,
                           reactive_metis_skip_until_depth);
}

mpz_class A = branchOnLiteral(t_first ? lit_t : lit_f, ...);
mpz_class B = branchOnLiteral(t_first ? lit_f : lit_t, ...);
return A + B;
```

Stage-2 VAR consumption ([solver_rec.cpp ~1951-1978](src/solver_rec.cpp#L1951-L1978))
gets a structurally identical wrapper at each `A + B` and `A - B` site.

### 6.3 Budget cutoff

In `branchOnLiteral`, between the entry assertion and the
`setLiteralIfFree`, add:

```cpp
if (in_probe_mode() &&
    stopwatch_.getElapsedSeconds() >= config_.probe_budget_seconds) {
    probe_budget_hit_ = true;
    return 0;  // count is meaningless in probe mode anyway
}
```

`probe_budget_hit_` is checked at the top of `solve()` after
`countSATRec` returns, to switch the exit path to "emit stats, exit
cleanly, suppress count."

### 6.4 New stats line

```cpp
if (in_probe_mode()) {
    std::cerr << "PROBE_STATS"
              << " skip_prob=" << config_.probe_skip_prob
              << " budget=" << config_.probe_budget_seconds
              << " walks=" << probe_walks_completed_
              << " mean_leaf_depth=" << probe_mean_leaf_depth_
              << " unsound_count=1"
              << std::endl;
}
```

The existing `DIAG_STATS` / `L2_HIT_HIST` / `FULL_CACHE_STATS` already
emit everything else the consumer needs.

---

## 7. Verification plan

### 7.1 Smoke test (must pass before any benchmarking)

`--probeSkipProb 0 --probeBudget 60` on a known-fast instance
(t1_071) must produce bit-identical decision count, cache stats, and
final count to the no-probe baseline run. If `probe_skip_prob = 0`, no
skip should ever fire, and the only behavioral change is the per-BCP
budget check.

Acceptance: exact match of `decisions`, `l2_stores`, `l2_hits`, model
count vs `./build/sharpSAT -rec -sep 5 -cb 3 -wlIter 2 t1_071.cnf`.

### 7.2 Behavioral test (the probe actually fires)

`--probeSkipProb 0.9 --probeBudget 1.5` on t1_071 should produce
*qualitatively* different stats than the smoke test: many fewer
decisions per walk (walks terminate at leaves quickly under skip),
significantly higher decisions/sec (no exponential branching),
multiple `walks_completed`.

### 7.3 Cross-validation against the Python prototype

`/tmp/probe_runtime.py` produces the documented 6-instance signature
table (t1_011 BCP-rich, t1_065/071/049/021_k10 moderate, t1_041
BCP-starved). The solver-mode probe must reproduce the same regime
classification on the same 6 instances. The absolute numbers will
differ (the solver has caching, the Python prototype does not), but
the rank order on `bcp_per_dec` and the `t1_041 mean_depth > 500`
signature must hold.

---

## 8. Calibration plan (after the implementation lands)

This is what slice 3 looks like, summarised here so the rule layer is
not invented from scratch later.

### 8.1 Six-instance baseline grid

Run probe at `-probeSkipProb 0.9 -probeBudget 1.5` across the
documented test instances:

- t1_011 (sparse, well-decomposable)
- t1_065 (uniform 5-CNF, dense)
- t1_071 (sparse pure 3-SAT)
- t1_049_k10_s1 (dense 3-SAT)
- t1_021_k10_s1 (small + low-density + decomposable)
- t1_041 (ganak-class, large)

For each: record `(bcp_per_dec, cache_hit_rate, large_comp_hit_rate,
mean_leaf_depth, walks_completed)`. This is the signal vector.

### 8.2 Probe-flag-comparison

For each instance, run the probe under each candidate configuration:

- plain `-rec -sep 5 -cb 3`
- legacy `-rec -sep 5 -cb 3 -sepVarBias`
- `-rec -sep 5 -cb 3 -adaptive -adaptiveAlpha 0.5`
- `-rec -sep 5 -cb 3 -adaptive -adaptiveAlpha 2.0`
- (later) configs that pin specific anchors via `-forceDecisions`

Compare signal vectors. The configuration with the strongest signals
under probe should be the fastest full-solve choice. Cross-check
against documented full-solve times in `benchmark_log.md`.

### 8.3 Rule-layer design

Once the calibration produces (instance, config, signal_vector,
full_solve_time) tuples, design a rule layer mapping signal_vector →
config_choice. Style: extend `portfolio_insights.md` §8 with
probe-derived rules. Avoid linear scoring formulas (insufficient data,
hard to debug); use rules that key on specific signal thresholds.

---

## 9. Broader architectural corrections logged this turn

These were surfaced in the same conversation as this plan was drafted.
They are not part of this plan's implementation, but they reframe how
adjacent subsystems should evolve:

1. **Separator-acceptance ratio gates should be removed.** The Phase-2
   gates `separator_max_ratio = 0.20` and `separator_min_balance = 0.30`
   reject separators by static shape. A "bad-shape" separator can be a
   *great* branching target if it triggers heavy BCP cascades; the right
   filter is dynamic effect, not structural ratio. Replacing these gates
   with probe-driven acceptance is consistent with the philosophy of
   this whole programme.
2. **Reactive METIS should be probe-driven.** Naive per-fallback reactive
   invocation was measured at 22-93× slowdown; that's not a fix, that's
   a confirmation that "rebuild METIS on every rejection" is the wrong
   trigger. The right trigger is "the probe shows the precomputed
   hierarchy isn't separating anything useful here." Sampling-based
   activation.
3. **Implicant learning is deprioritised.** Per the implicant_learning
   memory record it shipped 2026-04-21 but is uniformly 3-4× slower and
   off by default. The goal it tried to serve (shortening BCP cascades
   via learned clauses) is not load-bearing for the portfolio's wins
   and is unlikely to move the priority list back up.
4. **The mental frame is "dynamic effect, not static shape."** Across
   separator gating, reactive METIS, anchor selection, and picker
   tuning, the observed pattern is the same: features derived from F's
   syntactic structure are necessary but insufficient; the things that
   actually predict full-solve cost emerge only under search. Probing is
   the cheapest mechanism we have to observe those things without paying
   for a full solve.

Items 1 and 2 are candidates for their own slices after the stochastic-
skip probe lands. Item 3 is a "do not invest further" note. Item 4 is a
durable design principle for the portfolio programme as a whole.

---

## 10. Open verifications before we lean on `-forceDecisions` for probing

Slice 1 (2026-05-13) made `-forceDecisions` enqueue forced literals into
`unit_clauses_` before `simplePreProcess`. Verified on v242=F (single
test: 2.244 s, count tail matches doc). Before we use this primitive in
hundreds or thousands of probes, we should also verify:

- **Multi-literal pinning.** `-forceDecisions 242,450` (two anchors) —
  does BCP propagate both at root, or does one silence the other? (Same
  orbit; should sum to the same total count.)
- **Contradictory pinning.** `-forceDecisions 242,-242` — does the
  solver cleanly detect UNSAT at root, or does it crash on a state
  invariant?
- **Pinning a derivable unit.** If a variable is in the 104-var
  backbone and we force its known value, the redundant unit should be
  benign; if we force its negation, BCP must produce UNSAT, not a
  silent wrong count.
- **Pinning a preprocessed-away variable.** If subsumption / SSR / BCP
  eliminates a variable during root preprocessing, what happens if a
  later `-forceDecisions` references it? Currently the code in
  `Solver::solve` enqueues the unit before preprocessing, so this is
  fine — but the verification confirms the ordering matters.
- **Sandwich test.** Take a known sound run, force a redundant unit,
  confirm bit-identical count and decision sequence. This is the
  cleanest negative test: pinning a "no-op" anchor should not perturb
  any solver behavior.

Each of these is one or two solver invocations. The whole sandbox runs
in under a minute.

---

## 11. Cross-references and ground rules

- **Don't** invent a separate aggregate-count for probes — emit the
  meaningless count for protocol compatibility, flag it via
  `unsound_count=1` in `PROBE_STATS`.
- **Don't** persist learned clauses from probe runs. `learn_level=0`
  in probe mode.
- **Don't** assume the probe primitive will be useful in production
  solves — its job is offline characterisation. Production
  invocations should set `probe_skip_prob = 0` (or omit the flags
  entirely).
- **Do** record probe-mode measurements in `benchmark_log.md` alongside
  full-solve measurements; the comparison is the calibration data.
- **Do** update `portfolio_insights.md` §4 with probe-derived insights
  as they accumulate.
