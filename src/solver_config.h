/*
 * basic_types.h
 *
 *  Created on: Jun 24, 2012
 *      Author: Marc Thurley
 */

#ifndef SOLVER_CONFIG_H_
#define SOLVER_CONFIG_H_

#include <string>
#include <vector>

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

  // When true (default), allow conflict-clause learning during the
  // consumption of a precomputed separator (from_separator=true call
  // sites). Per-sub-component membership filter (binary lane filter +
  // learnedClauseInComponent for size ≥ 3) gates the resulting clauses,
  // so a cross-cut learned clause cannot bridge components in BCP.
  // Set to false to restore the legacy suppression for A/B comparison.
  bool allow_learning_in_separator_branching = true;

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

  // Per-variable centrality score derived from the ND-hierarchy position.
  // Adds `nd_centrality_weight * nd_centrality_score[v]` to scoreOf(v).
  // The score is (max_depth - leaf_depth) so vars in shallow nodes (closer
  // to the root) rank higher. Default 0.0 = off (no behavior change).
  // Analogous in spirit to ganak's `td_weight * tdscore[v]` term.
  double nd_centrality_weight  = 0.0;

  // Mid-consumption decomposition: at every solveComponent entry
  // with a non-empty separator, run discoverComponentsOf to detect
  // BCP-induced disconnects. On instances with many recursive calls
  // and rare disconnects (e.g., t1_071) the per-call cost was found
  // to dominate; default OFF. CLI: -decomposeInSep.
  bool   decompose_in_separator = false;
  // Throttle: only attempt mid-consumption discoverComponentsOf
  // when at least this many BRANCHING DECISIONS have been made
  // since the last connectivity check. BCP-forced literals are NOT
  // counted (they cascade fast on dense instances and would
  // saturate the gate every recursion). Counter is reset when
  // discoverComponentsOf runs (mid-consumption gate or post-
  // consumption block). With k=5 the check fires roughly every
  // 5 branching decisions. CLI: -decomposeAfterK.
  unsigned decompose_after_k    = 6;

  // Reorder the carried separator at acceptance time so all CLAUSE
  // elements come first, then all VAR elements (within-kind order
  // preserved). The standard separator-consumption loop then branches
  // clauses first, then vars. Helps on small density-1 instances where
  // exhausting clauses before vars gives BCP a better residual.
  // Empirically a modest win on t1_021_k10_s1 (4.4s vs 5.0s baseline);
  // measured to lose on dense + reactive-METIS instances like t1_041
  // where the METIS-derived interleaved order is better, so apply per
  // instance class rather than as a global default. CLI: -sepClausesFirst.
  bool   separator_clauses_first = false;

  // The unified branch-target picker (-unifiedPicker). Scores every
  // active VAR + active long CLAUSE in the current sub-component and
  // branches on argmax. Multiplicative scoring: base(x) · boost(x),
  // where boost = 1 + α · exp(−λ · rel_k) on separator elements (rel_k
  // = active sep size / active vars in comp). Acts as the "soft
  // preference" picker, used as a graceful last-resort fallback when
  // no specific strategy is known to win. See pickBranchTarget() in
  // solver_rec.cpp for the full formula.
  bool   unified_picker         = false;
  // Sigmoid midpoint c used by clause scoring inside the picker.
  // Default 3 centers the sigmoid at the smallest length we'll
  // clause-branch on. CLI: -clauseLenMid.
  double clause_length_midpoint = 3.0;
  // Sigmoid steepness β. Larger β = sharper discrimination near
  // the midpoint. CLI: -clauseLenBeta.
  double clause_length_steepness = 1.0;

  // Picker hybrid weight on the adaptive Stage-0 cheap_score
  // (Σ 2^(-α·active_len(C))) over freq+activity. Var score becomes
  //   freq + 20·activity + cheap_score_weight · cheap_score(v) + sep_bonus
  // where α = stage0_length_decay (already auto-picked when
  // auto_stage0_length_decay is on). Default 0 disables (recovers
  // length-agnostic freq+activity scoring). CLI: -cheapScoreW.
  // Useful on BCP-dominated instances where vars in many short clauses
  // are the high-cascade branching targets that legacy freq+activity
  // misses but separator preference doesn't substitute for either.
  double cheap_score_weight = 0.0;

  // Discrete coeff^k BCP-cascade addend (with min aggregation over
  // polarities) on the variable scoring path (Solver::scoreOf, used by
  // both legacy pickBranchVariable and the additive unified picker).
  // When weight > 0: score_addend(v) = cascade_score_weight × min(
  //   walk(lit_pos, depth), walk(lit_neg, depth)) where walk is a
  // depth-bounded recursive walk on binary_links_, contributing 1 if
  // no forcing or coeff·Σ recurse(forced) otherwise (coeff=2 hardcoded).
  // CLI: -cascadeW (default 0 = disabled), -cascadeDepth (default 3).
  double cascade_score_weight = 0.0;
  int    cascade_score_depth  = 3;

  // Unified-picker scoring knobs (multiplicative form).
  //   base(v)  = picker_var_weight · max(picker_base_floor,
  //                                      freq + activity + cheapW·cheap + cascadeW·cascade)
  //   base(C)  = picker_clause_weight · sigmoid(β · (L − mid))
  //   boost(v) = 1 + picker_alpha_var · exp(−picker_lambda_var · rel_k) · 1[v ∈ sep]
  //   boost(C) = 1 + picker_alpha_clause · exp(−picker_lambda_clause · rel_k) · 1[C ∈ sep]
  // rel_k = (active sep elements) / (active vars in comp).
  // Type-pure boost lets sep clauses compete with sep vars: the much
  // larger picker_alpha_clause default compensates for the small sigmoid
  // base on clauses. CLI: -pickerAlphaVar/-pickerAlphaClause,
  //                  -pickerLambdaVar/-pickerLambdaClause,
  //                  -pickerVarW/-pickerClauseW.
  double picker_alpha_var      = 15.0;
  double picker_alpha_clause   = 110.0;
  double picker_lambda_var     = 5.0;
  double picker_lambda_clause  = 5.0;
  double picker_var_weight     = 1.0;
  double picker_clause_weight  = 1.5;
  double picker_base_floor     = 0.01;

  // When true, a NON-SEPARATOR branch (var pick, or clause pick on a
  // clause not in the carried separator) drops the ND node (passes
  // nd_node = -1 to branchOnLiteral / branchOnClause), but keeps the
  // `separator` vector carried — so descendants lose ND-hierarchy
  // descent below this point. Sep-clause picks (the consumption spine)
  // still carry nd_node forward. CLI: -pickerNonSepKillsNd.
  bool picker_non_sep_kills_nd = false;

  // When true, sub-components reached via decomposition (post-consumption
  // and mid-consumption decompose paths) get nd_node = -1 forwarded
  // unconditionally — no fresh ND-sep is installed at any depth below
  // the root. Tests whether deep seps add value or are pure overhead.
  // CLI: -pickerRootSepOnly.
  bool picker_root_sep_only = false;

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
  // If true, `stage0_length_decay` is picked automatically from cheap
  // structural statistics of the post-preprocess formula (mean active
  // clause length). Disabled when the user passes -adaptiveAlpha
  // explicitly — their value wins. Default on; harmless when
  // -adaptive is not in use.
  bool     auto_stage0_length_decay = true;
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

  // Anchor probe mode. When `probe_anchor_mode` is true, the solver
  // runs preprocessing + ND-build + static-WL-labels as normal, then
  // dispatches into the depth-bounded anchor probe (per
  // docs/anchor_probe_design.md) and exits without running the main
  // count. Probe candidates are listed in `probe_anchor_vars` as
  // ORIGINAL (pre-preprocess) DIMACS variable IDs; each is probed at
  // both polarities. Output is a JSON document written to
  // `probe_anchor_out` (default stdout).
  //
  // Score formula: sum_k mult(k) * |comp(k)|^beta where mult(k) is
  // the multiplicity of canonical key k in the per-candidate ephemeral
  // multiset and |comp(k)| is the active-var count of the
  // corresponding sub-component. Higher beta emphasises large-component
  // reuse.
  bool     probe_anchor_mode   = false;
  int      probe_anchor_depth  = 8;
  int      probe_anchor_budget = 256;
  double   probe_anchor_beta   = 1.0;
  std::vector<int> probe_anchor_vars;
  std::string probe_anchor_out;   // empty = stdout

  // Anchor trace: when non-empty, every branchOnLiteral call appends a
  // line to this CSV file with (call_id, depth, var, polarity, cascade,
  // active_post, bcp_ok, conflicts, l2_stores, l2_hits). Diagnostic for
  // comparing two solver trajectories side-by-side under different first
  // decisions (e.g. v242 vs v70 on t1_041). Cost when empty: zero (single
  // string-empty check at the trace site). Cost when on: ~1 fprintf per
  // branch decision.
  std::string anchor_trace_path;

  // Reactive-METIS input dump: when non-empty, every call to
  // computeRuntimeMetisSeparator writes its input graph (active_vars,
  // long_clauses, binary_pairs) to this file in a simple text format.
  // The companion replay tool (tools/metis_replay) reads the file and
  // calls METIS standalone on each record to characterise failure modes
  // (n<4, METIS error, degenerate output, accepted-trivial, accepted-good)
  // and per-input properties (connected components, edges, etc.).
  // Diagnostic only.
  std::string dump_reactive_metis_path;

  // Local-search probe-based preprocessing (opt-in).
  // When enabled, runs after the existing preprocessor (subsumption /
  // pure-dup / SSR) and applies the diff-and-lift schema described in
  // docs/probe_preprocessing_plan.md. Default OFF.
  bool perform_local_search_preprocess = false;
  unsigned lsp_max_probes = 1000;
  unsigned lsp_max_size   = 4;
  unsigned lsp_max_total  = 5000;
  bool     lsp_no_r4      = false;
  bool     lsp_verbose    = false;

  // ===============================================================
  // DIAGNOSTIC FLAGS — debug-only, NOT for benchmarking
  // ---------------------------------------------------------------
  // Everything below this banner up through the WL/cascade section
  // is opt-in instrumentation preserved for future bug investigations
  // (canonical-key correctness, order-dependent miscounts, learned-
  // clause scope, structural cache verification). Turning these on
  // is generally expensive and changes behavior beyond what a
  // portfolio harness should explore. Treat them as forensic tools.
  // ===============================================================

  // Learning invariant checks (opt-in, debug aid).
  //
  // When ON, asserts that each time conflict analysis or BCP touches
  // a learned clause's antecedent, the clause is in-scope under the
  // current removed_clauses_. This targets the t1_011 order-dependent
  // miscount investigation: hypothesis is that an antecedent recorded
  // when the clause was in-scope can later be used by 1-UIP analysis
  // at a deeper scope where the clause is out-of-scope, producing an
  // unsound UIP clause whose asserting literal is then force-set,
  // causing an undercount.
  //
  // Specific invariants enabled:
  //   A. recordLastUIPCauses walk: if curr_lit's antecedent is a
  //      learned clause, must be in-scope NOW.
  //   B. recordLastUIPCauses walk: each antecedent's other literal
  //      must be F_TRI right now (BCP invariant carried forward).
  //   C. setLiteralIfFree: if storing a learned-clause antecedent,
  //      that clause must be in-scope right now.
  //
  // Each fires with a structured diagnostic and aborts. Cost when
  // OFF: zero (single bool check at the top of each guard).
  bool check_learn_invariants = false;

  // Brute-force cache check: at every cache store and cache hit, if the
  // sub-component has <= N active variables, brute-force enumerate
  // 2^N assignments and verify the count matches what's being stored
  // / returned. On mismatch: dump the sub-component CNF + abort.
  // Targets the t1_011 wrong-count investigation: catches the smallest
  // miscounted sub-component in the search tree.
  // 0 = disabled. Default 18 once turned on; cost ~0.1s per check.
  unsigned brute_force_cache_check_n = 0;
  // Where to dump offending sub-components when the brute-force check
  // fires. Empty = don't dump (just abort with diagnostic to stderr).
  std::string brute_force_cache_dump_dir = "";

  // ===============================================================
  // END diagnostic flags
  // ===============================================================

  // Maximum WL refinement iterations to attempt in buildCanonicalKey.
  // The cascade always runs: iter 1, then iter 2..wl_iterations while
  // collisions remain, then static-label combine for any residual
  // collision-block vars, then raw-id fallback for vars still tied.
  // Default 1: just iter 1, then static + raw-id fallback. Measured
  // faster on super_d3_id8 than wl_iterations=2 because skipping iter 2
  // leaves coarser dynamic blocks that are then resolved by static labels
  // and raw-id fallback in a single pass, yielding fewer cache misses
  // downstream than a sharper-but-stricter dynamic key. Hyperparameter
  // candidate for portfolio tuning — see docs/portfolio_insights.md.
  int wl_iterations = 1;

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

  // (Order-sensitivity probes — perm_*_seed / sort_* fields and their
  // -perm* / -sort* CLI flags — were the runtime bisection harness for
  // the t1_011 order-dependent miscount investigation, fixed 2026-04-26.
  // The regression test now lives in test_canonical_key_invariance, which
  // uses the underscored helper Solver::_permuteClauseLiteralsForTest.)

  // (-learnTrace / learn_trace_path, -pathTrace* / path_trace_*,
  // -stopAtBranch / stop_at_branch*, -dumpCompDir / dump_comp_dir
  // family, and -dumpRecursionDir / dump_recursion_dir family were
  // removed in cleanup pass 2 — they were the t1_011-investigation
  // specific tracing infrastructure. -forceDecisions, -logBranches,
  // -logConflicts kept since they're general-purpose.)

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
