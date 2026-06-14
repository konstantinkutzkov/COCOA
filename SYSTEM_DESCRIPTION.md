# COCOA — System Description (MC 2026, Track 1: Exact Model Counting)

**Solver:** COCOA (COmponent COunting with Anonymization)
**Track:** Track 1 — exact, unweighted model counting (`c t mc`)
**Author / contact:** Konstantin Kutzkov — kutzkov@googlemail.com
**Repository:** https://github.com/konstantinkutzkov/COCOA

> *Draft to accompany the solver submission; to be sent to mcw@modelcounting.org.*

## 1. Overview

COCOA is a **two-engine portfolio** for exact propositional model counting. Its
primary engine is a search-based exact counter derived from sharpSAT, extended
with a **canonical, anonymized component cache** and **METIS nested-dissection
separators**; its fallback is the **GANAK** exact counter. A lightweight
structural analysis routes each instance, and a forecaster-driven funnel selects
the most promising configuration, handing off to GANAK when the primary engine
is predicted not to finish in budget.

The design thesis: many large industrial instances decompose, under a good
variable order, into a very large number of **isomorphic** sub-components. A
component cache keyed on a *canonical, variable-anonymized* form of each residual
sub-formula collapses those repeats into a single computation, which a
variable-identity (hash) cache cannot. METIS nested dissection supplies the
separators that expose this decomposition.

## 2. Architecture

**Step 1 — structural analysis.** METIS computes a nested-dissection tree; a cost
model (`nd_cost`) summarizes it (separator ratio, tree depth/`max_path`, leaf
sizes, an `nd_cost` band low/mid/high) and Arjun reports whether substantial
definability is present. These features *order* the configuration set; they do
not prune it.

**Step 2 — configuration funnel.** A covering set of 8 COCOA configurations
(separator static/reactive/none × variable/clause branching × picker
legacy/adaptive/unified × BCP-cascade on/off × cache capacity) is raced. Each
runs a short window; an ETA forecaster on `closed_bits` (the log-space search
progress) narrows the set 8 → 4 → 2 → 1 over three rounds, after which the single
leader runs to the time budget. Any configuration that *finishes* short-circuits
and wins immediately.

**Step 3 — monitoring & GANAK handoff.** While the leader runs, an
escape-history decision tree judges, per recheck, whether it is *progressing fast
enough to finish in the remaining budget*. On sustained "bail" verdicts (a walled
or progressing-but-doomed leader), COCOA is terminated (freeing memory) and GANAK
is given the remaining budget. GANAK's output is in the competition format
natively; COCOA's count is re-emitted in that format by `run.sh`.

## 3. Key techniques

- **Canonical anonymized component caching.** Residual components are cached
  under a canonical key computed by Weisfeiler–Leman color refinement over the
  component's variable-incidence structure, so isomorphic-up-to-renaming
  components share one cached count.
- **METIS nested-dissection separators** expose the decomposition the cache
  exploits.
- **Soundness of caching with clause learning.** A globally-learned clause
  confined to a component can prune that component's count; cached under a
  learned-blind canonical key it would be transported unsoundly to isomorphic
  components in other contexts. COCOA makes learning + canonical caching sound
  via `-cachePurge 1` (purge cache entries whose subtree consumed a non-local
  learned-clause firing, on branch-arm failure) refined by `-provLocalTaint`
  (a σ-aware provenance check that exempts the ~97% of learned-clause firings
  that are provably locally entailed, keeping the soundness cost ≈ 1.2× worst
  case and ≈ 0 typically). These flags are enabled for every COCOA configuration.
- **Escape-history / pct-ETA handoff forecaster.** Keep/bail decisions use the
  log-space (`closed_bits`) progress rate and plateau-escape history, with an
  honest pct-space ETA (`2^remaining` evaluated in log space) so a slow but
  positive creep is not mistaken for an imminent finish.

## 4. Components and dependencies

- **COCOA primary engine** — sharpSAT-derived exact counter (this work), with the
  canonical anonymized cache and METIS integration.
- **GANAK** — Sharma, Roy, Soos, Meel, *GANAK: A Scalable Probabilistic Exact
  Model Counter*, IJCAI 2019 (run deterministically, `--prob 0`).
- **Arjun / CryptoMiniSat / CaDiCaL / CadiBack / SBVA** — preprocessing and SAT
  back-ends (Soos, Biere, et al.), built statically and linked.
- **METIS / GKlib** — Karypis et al., nested-dissection ordering.

All components are built from source by `build.sh` (GKlib → METIS → GANAK with
static SAT dependencies → COCOA).

## 5. Build and run

- `./build.sh` — builds the full stack with no parameters. Requires a C/C++
  toolchain plus `cmake make git python3 libgmp-dev zlib1g-dev`; the GANAK step
  downloads its SAT dependencies via CMake FetchContent on the first build
  (network required during build).
- `./run.sh <instance.cnf>` — runs the portfolio under a 3600 s budget and writes
  the MC2026 solution lines (`s SATISFIABLE` / `c s type mc` /
  `c s log10-estimate …` / `c s exact arb int N`) to stdout; the full solver
  trace goes to stderr. Non-`mc` type lines are rejected with a format error.

**Resource use:** within the 32 GB limit — the configuration funnel admits new
candidates only up to a 20 GB resident-set gate, and GANAK's 26 GB cache runs
only after the COCOA leader is terminated at handoff (so the two never coexist).
