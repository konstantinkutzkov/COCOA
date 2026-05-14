# Probing-Based Portfolio Optimization — Handoff Document

Audience: a fresh agent picking up this work. Written by the previous
instance to convey context, what's been tried, and what needs to be done.

This document is meant to be **read first**. Companion documents
(`portfolio_insights.md`, `benchmark_log.md`, `portfolio_driver_plan.md`)
contain detail; this one gives the conceptual shape.

---

## 1. The solver — what it is and what it does

`sharpsat-separator` is a recursive #SAT (model-counting) solver derived
from sharpSAT. It computes the exact number of satisfying assignments of
a CNF formula by depth-first search with:

- **Component caching**: when the search visits a sub-formula whose
  canonical key has been seen before, the count is retrieved from the
  cache rather than recomputed. Caching is what makes exponential search
  trees collapse to polynomial DAGs on structured instances.
- **Boolean Constraint Propagation (BCP)** at every decision point.
- **Conflict-driven learning** (CDCL-style; off-by-default at full
  strength, see `-learnLevel`).
- **A precomputed Nested Dissection (ND) hierarchy** via METIS — a
  static decomposition of the variable-clause incidence graph used to
  pick "separator" variables / clauses that, once fixed, split the
  formula into smaller independent pieces.
- **Two picker entry points**: the legacy Stage-3 picker
  (`pickBranchVariable`, occurrence-based) and the τ-based adaptive
  picker (`pickBranchVariableAdaptive`, length-weighted with optional
  Stage-2 BCP probing).

Code map:
- [src/solver.cpp](../src/solver.cpp), [src/solver_rec.cpp](../src/solver_rec.cpp) — search loop and recursion.
- [src/nd_hierarchy.cpp](../src/nd_hierarchy.cpp), [src/nd_hierarchy.h](../src/nd_hierarchy.h) — precomputed separator tree.
- [src/content_cache.h](../src/content_cache.h) — the L1/L2 component cache machinery.
- [src/canonical_key.cpp](../src/canonical_key.cpp) — WL-based canonical key computation (`-wlIter K`).
- [src/main.cpp](../src/main.cpp) — CLI flag parsing; full flag list is in `./sharpSAT` help output.

---

## 2. The objective — choose the right branching strategy per instance

The solver's runtime is dominated by branching choices: which variable
to pick at each decision point, in which order to consume the
separator, whether to bias toward separator vars throughout the search,
how sharply to refine canonical keys, and so on. The same formula can
solve in **seconds with one configuration and TIMEOUT with another** —
the empirical record (§4) makes this very concrete.

**The portfolio question**: given a CNF, can we cheaply (within a few
minutes) determine the best branching strategy to use, so the main
solve runs as quickly as possible?

The existing analyzer ([src/main.cpp](../src/main.cpp) plus rules in
`portfolio_insights.md` §8) makes flag choices from **static features**
(`n_vars`, `density`, `mean_clause_length`, `binary_fraction`). Static
features are necessary-but-insufficient — they classify instance
*shape*, but the actual solver behavior also depends on dynamic
properties (BCP cascade depth, cache hit potential, whether the
hierarchy actually decomposes during search) that only emerge under
search.

**We want to extend static analysis with probing**: short timed runs of
the solver that measure dynamic behavior, and use those measurements to
pick flags.

---

## 3. The four conceptual mechanisms our solver leverages

The flags are implementation details. The actual solver leverages four
distinct conceptual mechanisms — each formula has different proportions
of these available, and the strategy choice is "use what works."

### Mechanism 1 — Separator branching (hierarchy-driven decomposition)

Use a precomputed structural cut to split the formula into independent
sub-components. Branching on separator elements first, in a fixed
order, causes the rest of the formula to decompose.

*Implements via*: `-sep N` (threshold for when the hierarchy fires),
`-cb N` (clause branching for clauses that bridge components),
`-sepVarBias` (Stage-3 picker bonus for original separator vars
throughout the search), `-reactiveMetis` (recompute METIS at runtime
when the precomputed cut is rejected).

*Critical empirical fact*: t1_021_k7_s1 with `-sep 50` → 10.71 s,
`-sep 5` → 17.97 s, `-sep 100` → TIMEOUT. The threshold is
instance-class-dependent and has order-of-magnitude impact.

### Mechanism 2 — BCP / propagation (cascade-driven simplification)

Pick variables whose assignment triggers long propagation chains. Each
decision shrinks the formula via UP, so the search tree closes
quickly.

*Implements via*: default Stage-3 picker uses occurrence frequency (a
coarse proxy for cascade potential); `-adaptive` with `-adaptiveAlpha
f` is a sharper τ-based estimator.

*Critical empirical fact*: t1_049_k10_s1 with `α=0.5` → 2.47 s, `α=2.0` →
4.05 s. The α parameter must be tuned to mean clause length.

### Mechanism 3 — Cache amplification (canonical-key reuse)

When the search encounters the same sub-formula via different paths,
the canonical key matches and the count is retrieved from cache.
Exponential search becomes polynomial DAG. Depends critically on the
picker producing **stable** sub-formulas across paths.

*Implements via*: `-wlIter K` (sharpness of canonical-key WL
refinement); the cache itself is always on. The "anchor pinning" idea
(force a specific first decision) is also in this family — anchors are
variables whose fixing produces sub-formulas that cache-amplify.

*Critical empirical fact*: t1_041 with no anchor → TIMEOUT > 60 s; with
v242 (or v450 or several others) pinned as first decision → ~2 s.
Single-decision change, 30× runtime difference.

### Mechanism 4 — Conflict-driven learning

Standard CDCL — when a path produces UNSAT, learn a clause that prunes
other paths.

*Implements via*: `-learnLevel N`.

*Empirical status*: lower priority for our solver. On the ganak-class
instances we've measured, the solver records **zero conflicts** during
60s runs — these formulas aren't conflict-rich. Don't optimize for this
mechanism unless probe data shows the formula is generating conflicts.

---

## 4. Concrete results — strategy choice matters enormously

These are the measurements that justify the entire portfolio effort.
All from `docs/portfolio_insights.md` §4 and `docs/benchmark_log.md`.

### Separator threshold (Mechanism 1)

t1_021_k7_s1 (83 v / 80 c, sparse 1:1 var:clause):

| `-sep N` | time | decisions |
|---|---|---|
| 5 | 17.97 s | 29.6 M |
| 50 | **10.71 s** | **6.16 M** |
| 100 / off | TIMEOUT > 30 s | — |

### Adaptive α (Mechanism 2 strength)

t1_049_k10_s1 (90 v, dense 3-SAT, mean_len = 2.78):

| α | time | decisions |
|---|---|---|
| 0.5 | **2.47 s** | 854 K |
| 1.0 | 3.02 s | 1.03 M |
| 2.0 (sparse-tuned default) | 4.05 s | 1.37 M |

### `-sepVarBias` — fragile across instances

| Instance | Plain | Legacy `-sepVarBias` | Comment |
|---|---|---|---|
| t1_065 | 0.017 s | 0.53 s | bias 30× slower |
| t1_071 | 0.41 s | TIMEOUT > 60 s | bias makes it infeasible |
| t1_011 | 13.57 s | 13.73 s | bias no effect (deep ND tree absorbs it) |
| t1_021_k10_s1 | 5.25 s | **3.99 s** | bias is faster |

Same flag, opposite outcomes — the right choice depends on instance
class.

### Cache amplification — the anchor effect (Mechanism 3)

t1_041 (1920 v / 1910 c):
- Plain `-sep 5 -cb 3`: TIMEOUT > 60 s
- With v242 pinned as first decision: 2.17 s (F-side) + 2.36 s (T-side)
- v450, v405, v456, v526, etc. — equivalent fast anchors (verified counts match ganak; see `benchmark_log.md` 2026-05-12 entry)
- Most low-degree flip-symmetric vars solve fast; most high-degree vars don't
- We have no static feature that reliably predicts which var will be an
  anchor — anchors are dynamically detected via probing

### Hierarchy essentiality (Mechanism 1)

t1_011 (6559 v): plain `-sep 5 -cb 3` → ~25 s. Same instance with no
`-sep` → hangs > 5 min. The hierarchy isn't optional here.

### What this collectively says

Every measured instance has a configuration that beats alternatives
by 1.5× to >30×. There is no single fixed configuration that wins
across instances. **The whole game is per-instance configuration.**

---

## 5. The probing approach — current state and what's next

### The big idea

Instead of (or in addition to) using static features, **run the solver
in a short, controlled mode and measure what happens**. Use those
measurements to predict which strategy will be fast on the full
formula.

The probe outputs are signals like decisions/sec, BCP cascade per
decision, cache hit rate, leaves visited per second, mean recursion
depth. Different strategies (different picker, different flag set)
produce different signal vectors. The strategy with the best signals
under probing should be the best for the full solve.

The probe answers questions like:
- Is separator branching producing decomposition on this formula?
- Is BCP cascading meaningfully on this formula?
- Is the cache amplifying (many hits on large sub-components)?
- Does pinning a specific variable unlock cache amplification?

### Stochastic-skip probing (the current design)

Run the solver for a fixed wall-clock budget (e.g., 1.5 s) in a
modified mode: at each decision, with probability p (≈ 0.9) **skip**
(pick one polarity arbitrarily, no backtrack point recorded) and with
probability 1−p **branch normally** (record as backtrack target, try
both polarities on UNSAT).

Properties:
- Many paths explored in the budget (thousands per second)
- Cache fills naturally; cache hits measurable
- Backtracking happens at branch points (the 10% decisions)
- The count produced is wrong — but we don't need the count. We need
  the runtime characterization.

Per-probe signals:
- `decisions/sec` — raw search throughput
- `bcp/dec` — propagation strength per decision (BCP-rich vs BCP-starved)
- `cache_hit_rate` — does amplification fire on this formula
- `large_comp_hit_rate` — cache hits on ≥200-var components (the
  meaningful amplification signal)
- `mean leaf depth` — how deep walks descend before terminating
- `walks completed` — total path count in the budget

### What's been prototyped

`/tmp/probe_runtime.py` (a pure-Python stochastic-skip walker, no cache
machinery since Python BCP doesn't have one):

Results on six instances clearly separate them:

| Instance | bcp/dec | mean depth | regime |
|---|---|---|---|
| t1_011 | 113.4 | 21 | BCP-rich; propagation closes branches |
| t1_065/071/049/021_k10 | 1.2–2.9 | 44–75 | moderate / compute-bound |
| **t1_041** | **0.83** | **741** | BCP-starved; descends forever |

Even without cache machinery, the runtime profile per instance is
distinctive. With the solver-mode probe (which has the real cache),
the signal would be richer.

### Candidate selection for variable-pinning probes

When the probe needs to test variable-pinning anchors (for Mechanism
3), the candidate pool matters. We measured on t1_041:

- Top-degree alone — **fails** (misses the low-degree flip-sym anchors)
- Top-degree + low-degree flip-sym (the working pool used in the 120-var probe)
- Multi-feature union recommended:
  ```
  candidates = top-K by degree
            ∪ top-K by static BCP cascade size
            ∪ top-K flip-sym ∩ (NOT in giant orbit)        ← WL contribution
            ∪ vars in ND root separator
  ```
  Each axis catches a different anchor type.

WL orbit grouping was **explored and falsified** as a compression
technique — orbit-mates can probe wildly differently (v449/v450/v451/v452
range from TIMEOUT to fast solve). Don't reuse this idea without a much
sharper test. See `benchmark_log.md` 2026-05-12 entry for the data.

### What's implemented and what's not

- ✅ Pure-Python stochastic-skip prototype (`/tmp/probe_runtime.py`)
  produces meaningful signals.
- ✅ Variable-pinning probes via subprocess (works but expensive due to
  CNF re-loading; ~1 s per probe).
- ✅ Multi-polarity (min-aggregation) probe metric for anchor selection.
- ✅ Negative-validation that low min-rate predicts at-least-one slow
  polarity.
- ❌ **Solver-mode stochastic-skip probe (`--probeSkipProb p --probeBudget T`)**
  — not implemented. ~50–100 lines in [src/solver_rec.cpp](../src/solver_rec.cpp). This is the
  highest-leverage next engineering task. It gives access to the real
  cache, runs thousands of paths per second within a single solver
  invocation, and is the production-ready version of the prototype.
- ❌ Strategy-comparison framework that probes multiple flag combos and
  picks a winner.
- ❌ Mapping from probe signals to flag decisions (rule-based; should
  follow the style of `portfolio_insights.md` §8).
- ❌ Calibration: we've measured ~6 instances. The taxonomy in
  `portfolio_insights.md` references many more (t1_023, t1_025, t1_027,
  t1_047, t1_073, full MC2025 suite). Need to expand the calibration
  set over time.

---

## 6. Where to start

In rough order of leverage:

1. **Add `--probeSkipProb p --probeBudget T` to the solver.** The
   single biggest enabler. ~50–100 lines in
   [src/solver_rec.cpp](../src/solver_rec.cpp): at the picker entry, gate "branch both
   polarities" behind a coin flip; on UNSAT, the existing backtracking
   handles flip semantics. Add a budget-exit check. Emit aggregate
   stats (decisions, bcp_propagations, leaves, cache_hits,
   cache_hits_on_large_components, mean_depth) at exit.

2. **Build a Python orchestrator** that calls the solver with this
   flag under different strategies (different `-sep`, with/without
   `-sepVarBias`, with/without `-adaptive`, with first-decision pins
   from the multi-feature candidate union) and compares signals.

3. **Write the rule layer** that maps probe signals to flag decisions.
   Style: extend `portfolio_insights.md` §8 with probe-derived rules.
   Avoid linear scoring formulas — we don't have enough data, and
   rules are easier to debug and extend.

4. **Calibrate against the measured instances**. For each instance with
   known-good flags, verify the probe-based picker chooses the right
   flags. The doc records ground truth for ~10 instances.

5. **Expand the calibration set**. The MC2025 suite has many more
   instances; characterize them via the probe and grow the rule set.

---

## 7. Where the previous instance went wrong (lessons)

To save the next instance from the same mistakes:

- **Don't drift between anchor-specific findings and general portfolio
  design.** Anchors are one instance of Mechanism 3, not the whole
  picture. Most formulas don't need anchors at all.
- **Don't use top-degree as a separator proxy.** It picks heavily-
  constrained vars, not decomposition-quality vars. Either get the
  actual ND root separator (via solver flag — would also be a useful
  addition) or compute METIS in Python.
- **`time:` in solver output does NOT mean solved.** On `-t` budget
  kill, the solver still emits `time: ...s`. Use `TIMEOUT !` marker to
  distinguish. (This bug made it into doc commits before I caught it.)
- **WL orbit grouping does NOT compress probe candidates.** Falsified
  empirically. Use it only for the "skip the giant orbit" filter on
  t1_041-like cases.
- **Don't talk about "ganak-class" or other descriptive labels as if
  they were solver actions.** The solver chooses flags; "class" is
  human documentation, not an analyzer output.
- **Single-polarity probes mislead.** Always probe both polarities and
  min-aggregate.
- **0.25-s probes preserve rank** (Spearman 0.98 vs 1-s probes). Don't
  pay for long probes unless an explicit signal demands it.

---

## 8. Useful references

- `docs/portfolio_insights.md` — the empirical knowledge base. §1
  (instance taxonomy), §2 (per-flag notes), §4 (dated measurements),
  §8 (current routing rules).
- `docs/benchmark_log.md` — per-run measurements with commit hashes.
  The 2026-05-12 entry has the t1_041 anchor study.
- `docs/portfolio_driver_plan.md` — architectural sketch for the
  driver.
- `docs/probe_preprocessing_plan.md` — a separate (sound) probing
  effort for #SAT-correct preprocessing. Different from the
  portfolio-probing work but the infrastructure may be reusable.
- [src/solver_config.h](../src/solver_config.h) — all flags as a config struct. The new
  `probeSkipProb` / `probeBudget` should land here.
- [src/main.cpp](../src/main.cpp) — flag parsing; help text generation.
- The solver's existing `DIAG_STATS` / `L2_HIT_HIST` /
  `FULL_CACHE_STATS` output (see end-of-run logging in
  [src/solver.cpp](../src/solver.cpp)) already exposes all signals the probe needs. The
  probe mode just needs to emit them on budget exit, not on natural
  termination.

---

## 9. One-paragraph summary for impatient reading

We have a #SAT solver with multiple branching strategies (separator
branching, BCP-driven, cache-amplifying, conflict-learning), each
implemented via flags. Empirical measurements show that on every
non-trivial instance, the right strategy beats alternatives by
1.5×–30×, but the right strategy is instance-dependent and static
features alone don't predict it. We want a portfolio analyzer that
probes the formula (short timed solver runs with stochastic skipping)
to measure dynamic behavior, then picks flags accordingly. The probe
mechanism is prototyped in Python and works; the highest-leverage next
step is adding it to the solver itself as `--probeSkipProb p
--probeBudget T`, then building the rule layer that maps probe signals
to flag decisions. Calibrate against ~10 known-good instances first;
expand the calibration set as new instances are characterized.
