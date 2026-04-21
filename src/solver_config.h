/*
 * basic_types.h
 *
 *  Created on: Jun 24, 2012
 *      Author: Marc Thurley
 */

#ifndef SOLVER_CONFIG_H_
#define SOLVER_CONFIG_H_


struct SolverConfiguration {

  bool perform_non_chron_back_track = true;

  // TODO component caching cannot be deactivated for now!
  bool perform_component_caching = true;
  bool perform_failed_lit_test = true;
  bool perform_pre_processing = true;

  unsigned long time_bound_seconds = 100000;

  bool verbose = false;

  // quiet = true will override verbose;
  bool quiet = false;

  // Clause branching: branch on long clauses using
  // #SAT(F) = #SAT(F\{C}) - #SAT(F\{C} ∧ ¬C)
  bool perform_clause_branching = false;
  unsigned clause_branch_min_length = 8;

  // Separator branching: branch on elements of a precomputed METIS
  // nested-dissection separator until the separator is exhausted in the
  // current sub-component, then recurse on disconnected sub-components.
  bool perform_separator_branching = false;
  unsigned separator_min_active_vars = 15;

  // Phase 2 / Tier 1 gating: reject a precomputed ND-hierarchy separator
  // for the current sub-component if it is too large or too imbalanced
  // relative to the component's active variables. When rejected, we fall
  // through to reactive METIS (if enabled) or to variable branching.
  //
  //   separator_max_ratio   = max ( |filtered_sep| / |active vars in comp| )
  //   separator_min_balance = min ( min(L,R) / (L+R) )
  //
  // L and R count active component vars mapped to the left/right child
  // subtrees of the current hierarchy node.
  double separator_max_ratio   = 0.20;
  double separator_min_balance = 0.30;

  // Phase 3 / Tier 2: adaptive branching via probe-scored τ minimization.
  // When enabled, replaces `pickBranchVariable` on the no-separator path
  // inside `solveComponent`.
  //
  //   adaptive_top_k        = number of candidates to probe after Stage 0.
  //   stage0_length_decay   = α in cheap_score(v) = Σ 2^(−α · active_len(C)).
  //   epsilon_2clauses      = ε in s(v,σ) = vars_forced + ε · clamp(Δ_2clauses).
  //                           Δ is clamped to [−K_Δ, +K_Δ] with K_Δ = ⌊1/ε⌋−1
  //                           so |ε·Δ| < 1 ≤ vars_forced (protects Newton).
  bool perform_adaptive_branching = false;
  unsigned adaptive_top_k        = 20;
  // α = 2 (base 4): binaries contribute 0.0625, ternaries 0.0156, so a
  // var in one binary scores ~4× a var in one ternary. This sharpens
  // σ as a proxy for BCP-cascade potential, where binaries actually
  // trigger BCP immediately and longer clauses rarely do anything on
  // a single assignment.
  double   stage0_length_decay   = 2.0;
  double   epsilon_2clauses      = 0.1;
  // Components smaller than this many active variables use a
  // Stage-0-only (cheap clause-length-weighted) picker — no probing,
  // no Newton-τ, no failed-literal commits. The probing machinery only
  // pays off on medium-to-large components; on hierarchy leaves and
  // post-filter intermediate nodes of well-decomposed instances
  // (e.g. t1_071) probing is pure overhead.
  //
  // Default 60 is the empirically-measured elbow on t1_071 — above it
  // probing never fires on any decision and performance matches the
  // no-adaptive baseline. Lower the threshold with -adaptiveMin n to
  // experiment with probing on dense instances (e.g. t1_049).
  unsigned adaptive_probing_min_vars = 60;

  // Verify cache keys: lookup() always misses, and store() compares the
  // newly computed count against any previously stored count for the
  // same key. A mismatch indicates a semantic bug in the canonical key.
  bool verify_cache = false;

  // Implicant learning: when BCP forces a literal l* after branching,
  // walk the antecedent chain backward to collect the decision literals
  // that participated in the derivation. If the resulting set is small
  // enough, learn the clause (¬d_1 v ... v ¬d_k v l*) as a scoped
  // conflict clause — sound by resolution, potentially reusable in
  // distant subtrees when the same decision pattern recurs.
  //
  // Filters (compounding, cheap):
  //   - size cap: implicant_max_size (reject longer sets).
  //   - non-trivial: skip if the derived clause would equal the
  //     antecedent clause itself (no new information).
  //   - dedup: LRU hash of recently-learned implicant signatures.
  //
  // Hard total cap: once implicant_max_total learned, disable for the
  // rest of the solve.
  bool     perform_implicant_learning = false;
  unsigned implicant_max_size         = 4;
  unsigned implicant_max_total        = 100000;

  // Reactive METIS: when the precomputed hierarchy separator is
  // unavailable or rejected by the Phase-2 gate, compute a fresh
  // METIS separator on the current sub-component's incidence graph
  // and USE it. Applies only to sub-components with at least
  // `reactive_metis_min_vars` active variables — below that threshold
  // we fall through to the variable-branching path (adaptive Stage 0
  // picker or legacy activity-score picker).
  //
  // Default OFF. A naive per-fallback invocation was measured to be a
  // 22-93x slowdown on shrunken t1_049 sub-instances, because the
  // call count compounds across recursion levels (165k+ calls on an
  // 80-var instance). The infrastructure is kept for future work on
  // a *strategic* version — invoked sparsely after enough simplification
  // that METIS is likely to find a useful separator — or with an
  // incremental graph that avoids rebuilding the incidence snapshot on
  // every call. As shipped, reactive METIS is an opt-in experiment.
  //
  // Stats (call count, total/mean/max us, failure rate, bucket
  // distribution) are always accumulated when use_reactive_metis is
  // on and printed at end-of-solve if any call was made.
  bool     use_reactive_metis       = false;
  unsigned reactive_metis_min_vars  = 15;
  // Failure throttle: when reactive METIS is attempted at decomposition
  // depth d and fails to produce a usable separator (r.ok = false), do
  // not retry reactive METIS in this subtree until we reach depth
  // d + reactive_metis_skip_k. Intuition: BCP and variable branching
  // at intermediate levels simplify the formula; after k more levels
  // METIS is more likely to find a useful separator. Success does NOT
  // set the skip — future reactive calls in the successful subtree
  // proceed exactly as before (nothing changes from the no-throttle
  // behaviour on sparse instances where the precomputed hierarchy is
  // always good enough).
  unsigned reactive_metis_skip_k    = 5;
  // Scheme F — branching-variable quality gate for reactive METIS
  // separators. Even when a reactive separator is structurally good
  // (passes the Phase-2 ratio + balance gates applied to its sides),
  // its variables may be poor branching targets: METIS's objective is
  // graph-theoretic balance, not BCP-cascade potential. A variable
  // with low σ triggers tiny cascades, leading to bloated search trees
  // (measured: 22× slowdown on bench_A with 1-var reactive separators
  // whose single variable was σ-weak).
  //
  // Gate 2: require σ_sep_avg ≥ β · σ_top, where σ is the Stage-0
  // cheap-score over every active variable in the component, σ_top is
  // its max, and σ_sep_avg is the mean σ of the separator's VAR
  // elements (CLAUSE elements are excluded from the average).
  //
  // Safeguard for uniform-σ formulas: if σ_top / σ_median <
  // reactive_metis_sigma_signal_threshold, Gate 2 is skipped entirely
  // (the σ signal is too weak to discriminate; trust Gate 1 alone).
  //
  //   β = 0       : Gate 2 disabled (today's behaviour before Scheme F).
  //   β = 1       : demand separator elements average at least σ_top —
  //                 very strict, often falls back to Stage 0.
  //   β = 0.5     : separator's elements score at least half the top —
  //                 reasonable starting point.
  double   reactive_metis_sigma_beta               = 0.5;
  double   reactive_metis_sigma_signal_threshold   = 2.0;
};

#endif /* SOLVER_CONFIG_H_ */
