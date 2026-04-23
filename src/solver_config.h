/*
 * basic_types.h
 *
 *  Created on: Jun 24, 2012
 *      Author: Marc Thurley
 */

#ifndef SOLVER_CONFIG_H_
#define SOLVER_CONFIG_H_

#include <string>

struct SolverConfiguration {

  bool perform_non_chron_back_track = true;

  // Disable UIP conflict-clause learning. When off, BCP conflicts still
  // trigger the analyze / backjump path, and the asserting literal is
  // still forced; we just do not persist the learned clause in the
  // clause pool, and do not force the literal via a clause antecedent
  // (NOT_A_CLAUSE is used). Pure diagnostic — used to test whether a
  // count discrepancy is driven by the learned-clause machinery.
  bool perform_conflict_clause_learning = true;

  // Learning-feature ladder (for bug hunting). Removing features from
  // the top of the stack one at a time lets us pinpoint which learning
  // feature is unsound. Enable conditions (a feature runs iff the
  // current level is >= the threshold):
  //
  //   5 (default) = full: minimization + padding + scope + dedup + learn.
  //   4           = drop minimization.
  //   3           = drop binary-UIP padding (size<3 UIPs dropped on the
  //                 spot; scope tracking still records for size>=3).
  //   2           = drop scope tracking (learned clauses are unscoped —
  //                 sound only when removed_clauses_ stays empty, i.e.
  //                 no -cb / no -sep).
  //   1           = drop bloom-filter dedup (always attempt to learn).
  //   0           = no learning at all (same as perform_conflict_clause_
  //                 learning=false; the asserting literal is still
  //                 forced with NOT_A_CLAUSE antecedent, but no clause
  //                 is persisted).
  //
  // Activity updates remain on throughout — they influence branching
  // heuristics, not correctness.
  //
  // Default is 4 (no minimization): the naive minimization historically
  // implemented in minimizeAndStoreUIPClause checked `seen[var()]`, a
  // stale flag that did not reflect drops made during the same pass —
  // producing unsound learned clauses on some formulas (observed on
  // t1_011 as a pool-order-dependent 2^17 undercount). Level 5 still
  // exists but uses a rewritten iterative-fixed-point implementation
  // with live in_clause[] state and structural guards; enable
  // explicitly via -learnLevel 5 once the rewrite is known good.
  int learn_level = 4;

  // When true, run the extra end-of-minimization resolution-replay
  // verification (opt-in, expensive). For each literal dropped during
  // minimization we stored the antecedent clause used; the replay
  // re-derives the final clause by resolution and asserts it matches.
  // Catches any further bugs in the minimization machinery; too
  // expensive for default use.
  bool verify_learn = false;

  // Preprocessing-time simplification rules (run once, at startup,
  // after BCP + failed-literal test). Each is sound for #SAT (preserves
  // the set of models, not merely satisfiability).
  //
  //   subsumption:         C ⊆ D  → delete D
  //   pure-duplicate:      (ℓ ∨ C) ∧ (¬ℓ ∨ C) → C   (C identical)
  //   ssr (self-subsuming resolution):
  //                        (C) ∧ (D) with resolvent R ⊆ D → replace D with R
  //
  // Note: general bounded variable elimination (BVE) is NOT sound for
  // #SAT and is intentionally excluded.
  bool perform_preprocess_subsumption = true;
  bool perform_preprocess_pure_duplicate = true;
  bool perform_preprocess_ssr = true;
  // Hard wall-clock cap on the whole preprocessing-rules phase (ms).
  // When the budget is exceeded mid-pass we stop after the current
  // rule completes; the formula is still sound, just less simplified.
  unsigned preprocess_time_budget_ms = 10000;
  // Verbose per-pass logging of how many clauses were deleted /
  // shortened, and elapsed time.
  bool preprocess_verbose = false;

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
  // Chain-depth filter: require the derivation to have at least this
  // many antecedent-expansion steps ("forced-literal pops" during the
  // walk) before we consider the implicant worth storing. A clause
  // with chain_depth==1 equals its own antecedent (trivial filter
  // catches it). chain_depth==2 compresses 2 BCP hops to 1 clause.
  // chain_depth==3+ compresses 3+ hops. Higher threshold = fewer but
  // higher-value stored clauses, less BCP pool bloat.
  unsigned implicant_min_chain_depth  = 2;
  // Diagnostic: do the walk + filters but skip the clause store. Lets
  // us measure the cost of the learning machinery itself vs the cost
  // of BCP over an expanded clause pool.
  bool     implicant_dry_run          = false;

  // Diagnostic: at end of solve, scan the learned-clause pool and
  // report duplication + subsumption statistics. Read-only — does
  // not modify the pool. Used to decide whether implementing a live
  // subsumption pass would actually help.
  bool     analyze_clause_pool        = false;

  // Diagnostic: during search, after each branchOnLiteral's BCP
  // settles, look at the clauses AFFECTED by the branching (those
  // containing a newly-falsified literal) and count how many have
  // shortened effective form that would now subsume another clause.
  // Sampled (every Nth branch) to keep cost bounded. Read-only.
  //
  // Measures the actually-interesting question: as the solver runs,
  // do branches create subsumption opportunities that weren't in the
  // static formula? The static-pool analyzer cannot answer this.
  bool     analyze_dynamic_subsumption       = false;
  unsigned analyze_dynamic_subsumption_every = 100;  // sample every Nth branch

  // Diagnostic: after preprocessing, dump the resulting formula as a
  // DIMACS CNF to the given path and exit (without solving). Used to
  // verify preprocessing soundness by feeding the dump to an
  // independent counter (ganak) and comparing.
  std::string dump_preprocessed_path;

  // Diagnostic: after building the ND-hierarchy (just before the
  // first call to solveComponent), dump a static snapshot to this
  // path and exit. Contents: leaf-range per internal tree node,
  // var_leaf[v] for every compacted var, and the compacted->original
  // mapping so external analysis scripts can reason in original
  // variable space. Used to test whether the root partition is
  // self-consistent w.r.t. the current active formula before any
  // learning has happened.
  std::string dump_nd_and_exit_path;

  // Order-sensitivity probes. Post-preprocess, before search, randomly
  // permute a specific ordering dimension of the in-memory representation
  // and solve. A sound counter is order-invariant; if the count flips
  // when one of these knobs is toggled, the bug is keyed on that
  // ordering dimension.
  //
  //   perm_clause_lits_seed   : shuffle stored-literal order within each
  //                             long clause in literal_pool_. Watch list
  //                             entries for old lits[0]/lits[1] are
  //                             removed and re-added for new lits[0]/
  //                             lits[1] to keep the BCP invariant intact.
  //   perm_binary_links_seed  : shuffle binary_links_[l] for every
  //                             literal l, preserving the trailing
  //                             SENTINEL_LIT.
  //   perm_watch_lists_seed   : shuffle watch_list_[l] for every literal
  //                             l, preserving the leading SENTINEL_CL.
  //   perm_occ_lists_seed     : shuffle occurrence_lists_[l] for every
  //                             literal l.
  //
  // 0 = no permutation (default). Any non-zero seed enables that knob.
  // Multiple knobs can be active simultaneously.
  unsigned perm_clause_lits_seed   = 0;
  unsigned perm_binary_links_seed  = 0;
  unsigned perm_watch_lists_seed   = 0;
  unsigned perm_occ_lists_seed     = 0;

  // Canonicalize (sort) each order-sensitive structure. When the bug
  // depends on iteration order, toggling one of these ON should make
  // the wrong and correct files produce the same count. The toggle
  // that fixes the bug pinpoints which ordering dimension matters.
  bool sort_binary_links   = false;
  bool sort_watch_lists    = false;
  bool sort_occ_lists      = false;
  bool sort_clause_lits    = false;
  // Rewrite literal_pool_ with original clauses in canonical (sorted)
  // order. Required for FULL canonicalization: different line orders in
  // the input file produce different clause ofs values, which propagate
  // into watch_list_ and occurrence_list_ contents even after those are
  // sorted. Only this rewrite normalizes the ofs values themselves.
  bool sort_clause_pool    = false;

  // Dump each sub-component visited during search to <dump_comp_dir>/comp_<id>.cnf
  // and log (id, depth, num_vars, num_clauses, our_count) to
  // <dump_comp_dir>/log.txt at solveComponent return. A post-process script
  // can run ganak on each CNF and find the smallest sub-component whose
  // count our solver gets wrong. Empty string disables.
  //
  // Only dumps sub-components with num_vars in [min, max] to keep volume
  // bounded. Defaults catch small-to-medium components.
  // When non-empty, append a line per learned clause to this path at
  // learn time. Each line contains:
  //   LEARN ofs=<ofs> size=<n> DL=<dl> scope_size=<k>
  //         clause: <lit lit lit ...>
  //         stack (DL: lit ante):
  //           <for each lit on literal_stack_>
  //         scope: <ofs1 ofs2 ...>
  // Used to trace the provenance of a specific learned clause when the
  // solver's in-memory solve produces an undercount.
  std::string learn_trace_path;

  // G-to-H path capture. Recording turns ON when the clause at this ofs
  // is learned (G), and turns OFF when solveComponent is entered with
  // active vars equal to this set (H). Only events between G and H are
  // written. This is the unique tree-path between the two nodes.
  unsigned    path_trace_ofs = 0;   // 0 = disabled
  std::string path_trace_comp_vars; // CSV ints, e.g. "61,183,184,..."

  // Forced decision sequence. Each entry is a DIMACS literal (signed int).
  // When non-empty, the solver's variable branching path uses these
  // decisions in order instead of the activity-score heuristic. After
  // the list is exhausted (or the next forced var isn't active in the
  // current component), falls back to the heuristic.
  std::vector<int> forced_decisions;

  // Log each branchOnLiteral entry + exit with its returned count.
  // Purpose: by diffing the traces of two runs on different line
  // orderings, find the first branch where sub-counts diverge.
  bool         log_branches        = false;

  // At every BCP conflict, log the current literal_stack_'s DECISION
  // literals (ante-less ones). Post-process: for each conflict, add
  // those decisions as units to F and run a SAT solver. If SAT, that
  // conflict is SPURIOUS — the solver wrongly declared UNSAT.
  bool         log_conflicts       = false;
  // When > 0: at the Nth branchOnLiteral entry (after BCP), dump the
  // current in-memory formula state as a DIMACS CNF and exit 0. This
  // extracts a smaller reproducer at the divergence point found by
  // comparing logs.
  long long    stop_at_branch      = 0;
  std::string  stop_at_branch_path;

  std::string dump_comp_dir;
  unsigned    dump_comp_min_vars = 5;
  unsigned    dump_comp_max_vars = 80;

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
