# Portfolio / Orchestration Driver Plan

## Context

The solver and preprocessor are already separate, pure-function modules
(see commit `7629175`). Preprocessing is a function `CNF → CNF'`; the
solver consumes a simplified CNF and produces a count. They do not
share state, and a bug in one cannot corrupt the other.

This separation was the enabling refactor for a larger piece of work:
a top-level orchestration driver that analyzes the formula, decides
which preprocessing rules and which solver flags to apply, and —
optionally — runs the solver multiple times with different flags on
different residual sub-problems, aggregating the partial counts.

This document records the agreed design so a future implementation
proceeds from a fixed plan rather than a fresh reconstruction.

---

## What Part B (portfolio driver) actually is

An orchestration module that:

1. **Inspects the input formula** (the analyzer piece).
2. **Decides on a preprocessing plan** — which rules to apply.
3. **Decides on a solving plan** — which solver flags to use
   (`-sep`, `-cb`, `-adaptive`, `-learnLevel`, `-reactiveMetis`,
   `-sepMode`, threshold values, etc.).
4. **Optionally branches the assignment space strategically** —
   picks a handful of variables, branches on their values at the
   driver level, runs the solver (possibly with different flags) on
   each residual sub-problem. Combines results by summation.
5. **Returns** the final count.

This is fundamentally different from "the solver runs once with some
flags." It is a driver that may run the solver multiple times, with
different flags, on different sub-problems, and aggregate.

---

## Why the current architecture supports this cleanly

- **Preprocessing is already a pure function.** No contamination when
  re-running or forking.
- **The solver's logical entry is `createfromFile` + `countSATRec`**
  — a single solve loop whose result is a count. Wrapping that in a
  reusable `count(CNF, flags) → mpz_class` function is a small
  refactor.
- **`createfromFile` + `simplePreProcess` is the clean-slate entry.**
  Each new `Solver` instance is a fresh state. Re-running the solver
  on a sub-CNF means constructing a new `Solver` from that sub-CNF —
  no shared state to invalidate.

What is missing today for the portfolio design:

- The solver's entry point is wrapped in a main loop that expects a
  command-line invocation and writes to stdout. A pure
  `count(CNF, flags) → count` function is not yet exposed.
- Variable branching at the meta level means constructing sub-CNFs
  (assign `a=1`, BCP, simplify → F_a). The preprocessor already does
  exactly this. We would want to expose this pipeline as a reusable
  primitive.

---

## Four-layer stack

```
Driver (portfolio + orchestration)
  ├─ Analyzer (CNF → preprocessing plan + solver plan + branching suggestions)
  ├─ Preprocessor (CNF × plan → CNF')
  ├─ Solver (CNF × flags → count)
  └─ Result aggregator (summing partial counts)
```

- The solver stays simple: single CNF in, single count out.
- The driver is the new top layer. It decides "run the solver on F
  with flags X. If that times out, branch on `v` and run on F|v=T and
  F|v=F separately with flags Y. Aggregate."

Each layer is a pure function of the layer below. Adding rules to
the preprocessor, or flags to the solver, does not require changes
to the layers above; it only extends the surface the analyzer can
choose from.

---

## Capabilities, simplest to most ambitious

1. **Static policy.** Analyzer picks one flag set; driver runs the
   solver once with those flags. Simplest possible driver; immediate
   value from the analyzer alone.
2. **Sequential fallback.** Try fast flag set A with a short time
   budget; on timeout, try slower-but-more-robust flag set B with the
   remaining budget. Protects against "-adaptive-is-wrong-for-this-
   instance"-style failures.
3. **Branch-and-orchestrate.** Analyzer picks a small set of
   high-leverage variables (high-degree; long probing cascades).
   Driver branches on them at the top level, runs the solver on each
   residual sub-problem with flags tuned to the residual, and sums.
4. **Parallel portfolio.** Run K solver instances concurrently with
   different flag sets, different branching choices, or different
   preprocessing plans. First to finish wins (for decision problems);
   for #SAT with orthogonal sub-problem splits, all contribute partial
   counts.
5. **Adaptive portfolio.** Learn from per-instance experience which
   policies work on which instance classes. Instance-specific
   heuristics, possibly driven by a logged history of
   (instance features, flag set, runtime).

Start at (1) and (2). (3) is the natural extension once the driver
exists. (4) and (5) are research.

---

## Critical design decisions

### 1. How does the driver invoke the solver?

Two options:

**A. In-process library call.** Driver links the solver as a library
(`libsharpSAT` is already a target). `mpz_class count(CNF, flags)` is
a plain function call.
- Pros: fast (no IPC overhead); can share data structures.
- Cons: a solver state bug becomes a driver state bug; running
  multiple solver instances concurrently needs thread safety; the
  solver's global-ish state (stats, time trackers) gets in the way.

**B. Subprocess invocation.** Driver writes the sub-CNF to a temp
file, forks `sharpSAT` as a child process, reads its stdout.
- Pros: total isolation (a solver bug cannot corrupt the driver);
  natural parallelism via multiple OS processes; clean timeout
  handling via SIGKILL; stdio-based contract is debuggable by hand.
- Cons: ~50 ms per invocation overhead; the stdio contract is
  slightly fragile and must be version-locked.

**Decision for #SAT workloads**: B is the right default. Per-invocation
overhead is trivial relative to expected-minutes solve times. Isolation
is worth a lot given how delicate the solver's state has been during
the recent bug-hunt phase. An A-mode could be offered later for tight
loops.

### 2. Partial-count bookkeeping and timeouts

If the driver branches at the top level, it aggregates partial
counts. With arbitrary-precision integers (`mpz_class`), aggregation
is addition: branch `a=1` gives count `c_1`, branch `a=0` gives `c_0`,
total is `c_1 + c_0`. Trivial when all branches complete.

If a branch times out, the driver has a partial count on some
branches and missing data on others. The policy must be explicit.
Options:
- Return "partial count, incomplete" with the branches that finished
  and a count of missing branches.
- Return "unknown" (conservative — matches current timeout semantics).

Recommended: start with "unknown" on any incomplete branch, move to
partial counts only when the downstream tooling explicitly handles
them.

### 3. Where does policy data live?

As the driver grows smarter, it accumulates heuristics. Three places
they could live:

- Hardcoded in the driver's source.
- In a config file shipped alongside the binary (JSON/YAML).
- Learned at runtime from a logged history of
  (instance features, flag set, runtime).

Recommended: start hardcoded, evolve via git history. Move to config
file once the policy surface grows large enough that multiple people
need to edit it. Learned policy is a research direction, not a
prerequisite.

### 4. Timeout handling

If a forked subprocess is taking too long, SIGKILL is simple. But if
the driver wants to *continue* after the kill (try a different flag
set), then timeout handling must be per-invocation, not global.

The driver needs its own budget tracker that slices the user's
top-level time budget across multiple attempted solver invocations.
Classical portfolio-solver bookkeeping; straightforward with the
subprocess model.

---

## What the driver should NOT do

- **Share search-time state between invocations** (e.g., reusing
  learned clauses across runs). Tempting but dangerous given how
  pool-order-sensitive the codebase has been. If this is ever wanted,
  it is a major separate project with its own correctness argument.
- **Cache canonical keys across invocations.** Same reason.
- **Modify the solver to support driver-initiated interrupts/resume.**
  Just use subprocess + kill. Simple.

---

## Concrete module layout

```
driver/
  driver.h          -- top-level API: mpz_class drive(CNF, user_config)
  driver.cpp        -- orchestration logic
  analyzer.h        -- analyze_formula(CNF) → DriverPlan
  analyzer.cpp
  plan.h            -- DriverPlan struct: preprocessing flags, solver
                       flags, branching variables, time budget slice
  solver_invoker.h  -- invoke_solver(CNF, flags, time_budget) → SolverResult
  solver_invoker.cpp
```

Entirely new subdirectory. Zero touches to `src/`. Consumes
`libsharpSAT` in A-mode or forks `sharpSAT` in B-mode. Has its own
tests directory.

At the CLI level: a new binary `sharpSAT-drive` takes CNF + top-level
time budget and runs the driver. The existing `sharpSAT` binary
remains as the single-invocation entry point for tooling that
expects it.

---

## Analyzer features

The analyzer is itself a pure function `CNF → DriverPlan`. Starts
simple and grows. Initial feature set:

- Clause-length histogram.
- Variable-degree histogram (mean / max).
- Number of connected components of the original incidence graph.
- Ratio `n_binaries / n_clauses`.
- Density proxy: `Σ|C| / (n_vars · avg_degree)`.
- **BCP-saturability probe**: pick ~10 random variables, count BCP
  cascade length under `a=1`. Small = dense-random-ish;
  medium/long = structured.
- **METIS quality probe**: build the incidence graph, run a single
  top-level METIS vertex separator (not the full hierarchy), measure
  separator size and balance.

Decision logic — starts simple, grows:

- If METIS separator ratio ≤ 0.10 and balance ≥ 0.4 → instance is
  "well-structured." Apply sparsifying rules only (subsumption,
  pure-dup, SSR). Skip binary harvest. Proceed to normal ND-hierarchy.
- If METIS separator ratio > 0.30 or unbalanced → instance is
  "dense." Apply sparsifying rules + binary harvest (capped); re-
  probe METIS after harvest to verify it has not made things worse.
  If harvest increased separator size, roll back harvest.
- Middle cases → sparsifying rules only; log stats for later tuning.

Analyzer runs ONCE per formula, takes 100 ms – a few seconds on
typical MC2025-sized instances. Output can be cached by CNF-hash for
repeat runs.

---

## Preprocessing rules behind per-rule flags

Each rule is a config field + CLI flag. The analyzer outputs a
`PreprocessorConfig` that picks which rules fire. Current set:

- `perform_preprocess_subsumption` — present, default on.
- `perform_preprocess_pure_duplicate` — present, default on.
- `perform_preprocess_ssr` — present, default on.

Candidates for future addition (each its own commit, behind its own
flag, default off):

- `perform_preprocess_binary_harvest` — probing-based hyper-binary
  resolution. Sound for #SAT (adds entailed binaries only, no
  variable elimination). Density-vs-separator-quality trade-off
  means it is a bad default for sparse instances; analyzer should
  enable only when METIS separator is already poor.
- `perform_preprocess_equivalence_merge` — detect `a ≡ b` pairs via
  paired probing. Substitution is delicate for #SAT; requires its
  own correctness note before landing.
- `perform_preprocess_hyper_ternary` — binary-harvest generalization
  allowing 2-literal antecedents.

Discipline: each new rule is additive, self-contained inside the
preprocessor module, and has a regression test pinning input CNF →
expected simplified CNF bit-identically.

---

## Solver flags behind the driver

Flags the analyzer can choose from, with the per-instance-class
policy summary:

- `-sep` / `-cb` / `-sepMode` — enabled by default on; analyzer may
  tune thresholds.
- `-adaptive` — off by default; analyzer enables for dense instances
  where the ND-hierarchy separator is poor.
- `-reactiveMetis` — off by default; analyzer may enable with failure
  throttle tuned per instance density.
- `-learnLevel N` — currently 4 (no minimization) due to known
  unsoundness at level 5. Analyzer could raise/lower once the
  remaining unsoundness is resolved.
- `-preprocessBudget ms` — analyzer picks a budget proportional to
  formula size.

---

## Commit sequence

Each step is a self-contained commit, independently testable.

### Step 1: Refactor solver entry point

Expose `mpz_class count(CNF, SolverConfiguration) → mpz_class`
alongside the existing main. No driver yet; this is just the
library-mode API. ~200 lines, low risk.

### Step 2: Skeleton driver binary

`sharpSAT-drive` that calls `count(CNF, default_flags)`. Functional
equivalence to current `sharpSAT`. ~100 lines.

### Step 3: Subprocess invoker

Driver forks `sharpSAT` as a child process. End-to-end test: driver
+ subprocess produces the same count as direct `sharpSAT`. Proves
the orchestration model works. ~300 lines.

### Step 4: Minimal analyzer

Computes 3-4 structural features, picks between 2-3 flag presets.
Hooked into the driver. ~200 lines.

### Step 5: Timeout + fallback

Driver tries flag set A for T1 seconds; on timeout falls back to B
for T2 seconds. No aggregation yet. ~150 lines.

### Step 6: Top-level branching

Driver picks 1-2 high-leverage variables, solves sub-problems,
aggregates counts. ~500 lines. Requires careful testing — this is
where aggregation bugs could produce wrong answers.

### Step 7: Per-flag analyzers

Each flag gets its own "should we enable this" heuristic. Tuning
work, per-instance-class measurement. Incremental; one heuristic at
a time.

Skipping ahead (step 6 without step 5, for example) is risky — a
timeout with no fallback is fatal for the portfolio premise.

---

## Benchmarking harness

A portfolio driver is essentially a research project. It requires
measurement to tune. This implies a benchmark harness: an automated
way to run the driver across the MC2025 suite (or a chosen subset)
with multiple policy configurations and compare outcomes.

Benchmark-harness concerns:

- Per-instance timing and count correctness (compared to ganak
  oracle where available).
- Per-policy aggregate performance: speedup vs. single-flag baseline;
  number of instances solved within budget; worst-case regression.
- Policy evolution log: which commits changed which policies, with
  before/after numbers.

This harness is itself substantial work — likely a few weeks of
setup plus ongoing operational care — and should be planned
alongside the driver, not after.

---

## Value proposition

On competition-level instances, a good portfolio driver can make a
10-100× difference on specific instance classes. The tradeoffs
between -adaptive and default branching, between binary harvest and
no harvest, between aggressive and conservative learning — each has
"right" and "wrong" instance classes. A single hardcoded flag set
cannot win on all of them. An analyzer-driven portfolio can.

The value is real and worth pursuing, but it is a direction, not a
single feature. Committing to Part B means committing to:

- The analyzer module.
- The driver module.
- The subprocess contract and timeout bookkeeping.
- The benchmark harness.
- Ongoing tuning work as we learn what heuristics actually help.

That is a multi-month project, not a multi-day one.

---

## Relationship to current priorities

Part B is the long-term direction for making the solver competitive
across instance classes. It should not be started until:

1. The current correctness fixes are stabilized in production (a
   clean pass on the full MC2025 suite, not just the few reproducers
   we have been testing).
2. The preprocessor has its second-round rules (binary harvest at
   minimum) so the analyzer has something meaningful to analyze.
3. A benchmark-harness skeleton exists to catch regressions as the
   driver evolves.

In the short term, the value from cleanups, per-rule preprocessing
flags, and small safe optimizations is higher — because it lays the
groundwork the driver will consume. The driver comes after that
foundation is solid.

---

## What to do next, operationally

If we decide to invest in Part B:

1. Land the solver entry-point refactor (step 1 above) as a standalone
   commit. This unlocks all further work. Low risk.
2. Land a single experimental preprocessing rule (binary harvest)
   behind a flag, default off. Gives the analyzer something to toggle.
3. Pause. Measure current solver on the MC2025 suite at default
   settings; use that as the baseline the driver must beat.
4. Then start on the analyzer + minimal driver (steps 4 + 5).

Each step is a checkpoint where we can honestly ask "is this working
out?" and pivot if not. The architecture supports this — we are not
committing to a monolithic build.
