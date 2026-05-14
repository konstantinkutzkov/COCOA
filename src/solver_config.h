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

  // Bonus added to scoreOf(v) for separator-membership when
  // -sepVarBias is on. CLI: -sepBiasW <float>. Default chosen
  // empirically to dominate the freq term but stay below typical
  // VSIDS activity scaling.
  double separator_bias_weight = 1000.0;

  // Checkpoint 1 toward score-driven branching: at separator-acceptance,
  // strip VAR elements from the consumed separator and mark them in
  // `sep_bias_active_`. CLAUSE elements stay in the separator and go
  // through the existing clause-branching consumption path. VARs are
  // chosen by `pickBranchVariable` via the bias bonus, not by forced
  // enumeration. Reduces decision-count by removing 2-decisions-per-
  // VAR-element-per-recursion. Clause-branching is unchanged.
  bool   separator_vars_as_bias = false;

  // Stage C: unified picker that scores active VARs and CLAUSEs at
  // every decision and picks the highest-scoring target. Replaces the
  // separator-consumption loop and the standalone variable picker.
  // VAR score = scoreOf(v) (existing, with sep-bias bonus when
  // separator_vars_as_bias is on). CLAUSE score = length-decay
  // 2^(-α·|C|) * clause_score_weight (+ sep-bias bonus if the clause
  // is in the current frame's separator hint). Branching on a CLAUSE
  // uses branchOnClause (internal learning-suppression). Branching
  // on a VAR uses branchOnLiteral (learning enabled — we no longer
  // descend the ND-hierarchy strictly, so the L/R structural
  // invariant doesn't constrain learning).
  bool   unified_picker         = false;
  // Magnitude scaler on the CLAUSE length term. Without it, the
  // bare sigmoid lives in [0, 1] and every VAR (whose score is
  // freq+activity, typically tens to hundreds) beats every CLAUSE
  // on raw scale. Lifting clause scores to a comparable range is
  // what makes the unified picker a real choice. CLI: -clauseScoreW.
  double clause_score_weight    = 100.0;
  // Sigmoid midpoint c in clauseScoreW · 1/(1+exp(-β·(|C|-c))).
  // Default 3 centers the sigmoid at the smallest length we'll
  // clause-branch on. CLI: -clauseLenMid.
  double clause_length_midpoint = 3.0;
  // Sigmoid steepness β. Larger β = sharper discrimination near
  // the midpoint. CLI: -clauseLenBeta.
  double clause_length_steepness = 1.0;
  // Dynamic separator-importance base `a`: separator-bias bonus is
  // multiplied by a^(-k) where k = currently-active separator
  // elements (VARs in bias bitmap + CLAUSEs in sep hint, both
  // intersected with this comp's active state). Singletons score
  // ~1× sepW; longer separators decay quickly. Default 1.5 sits
  // between linear and exponential. a=1.0 disables the dynamic
  // multiplier (recovers the flat-bonus behavior). CLI: -sepImpA.
  double separator_importance_base = 1.5;

  // Cross-instance normalization for the dynamic decay. The picker scores
  // separator elements with bonus = sepBiasW * a^(-k / (N+M)^p), where
  // (N+M) is the active incidence-graph size of the current sub-comp
  // (active vars + active original clauses) and p in [0, 1] controls how
  // aggressively k is shrunk relative to comp size. p=0 disables (recovers
  // raw a^(-k) behaviour). p=0.5 treats k as a fraction of sqrt(N+M).
  // CLI: -sepSizeNormP.
  double separator_size_norm_p = 0.0;

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

  // Unified picker mode selector and its multiplicative-form parameters.
  // ADDITIVE (default): legacy additive score combination
  //   S(x) = base(x) + sep_bonus_m·1[x∈sep] + cascade_addend
  // MULTIPLICATIVE: type-pure base AND type-pure boost — each kind
  // (variable / clause) has its own scoring AND boost shape:
  //   base(v) = picker_var_weight · max(picker_base_floor,
  //                                     freq + activity + cheapW·cheap)
  //   base(C) = picker_clause_weight · sigmoid(β · (L − mid))
  //   boost_var(v)    = 1 + picker_alpha_var · exp(−picker_lambda_var · rel_k)
  //                       · 1[v ∈ sep_var_set]
  //                     + picker_gamma · max(0, depth(v)/depth_med − 1)
  //                                                   (Regime B only)
  //   boost_clause(C) = 1 + picker_alpha_clause · exp(−picker_lambda_clause · rel_k)
  //                       · 1[C ∈ sep_clause_set]
  //   S(x) = base(x) · boost(x)
  // where rel_k = (active sep elements) / N_active_vars. Type-pure boost
  // lets sep clauses compete for the top score against sep vars even
  // when the sigmoid base is small. Concretely, with the defaults
  // below at root rel_k≈0.18 on k10:
  //   sep var multiplier:    1+15·exp(-0.875)≈ 7.25
  //   sep clause multiplier: 1+110·exp(-0.875)≈ 46.9
  //   sep var score:    5·7.25  ≈ 36
  //   sep clause base:  1.5·0.5 = 0.75; score 0.75·46.9 ≈ 35
  // → roughly tied; tune as needed. Non-sep length-3 clauses score
  // 1.5·0.5 = 0.75 < typical var ≈ 5, so they're suppressed.
  // Regime A (default): picker_gamma = 0 → cascade boost off, no
  // per-call cascade-cost overhead. Regime B requires sampled-median
  // + lazy candidate evaluation; not enabled in this iteration.
  // CLI: -pickerMode {additive|multiplicative}, -pickerAlphaVar,
  //      -pickerAlphaClause, -pickerLambdaVar, -pickerLambdaClause,
  //      -pickerGamma, -pickerVarW, -pickerClauseW.
  enum class UnifiedPickerMode { ADDITIVE, MULTIPLICATIVE };
  UnifiedPickerMode unified_picker_mode = UnifiedPickerMode::ADDITIVE;
  // Per-type α gain factors (separate for vars and clauses).
  double picker_alpha_var      = 15.0;
  double picker_alpha_clause   = 110.0;
  // Per-type λ decay rates (separate for vars and clauses).
  double picker_lambda_var     = 5.0;
  double picker_lambda_clause  = 5.0;
  double picker_gamma          = 0.0;
  double picker_var_weight     = 1.0;
  // Lowered to 1.5 so non-separator clauses don't compete with vars
  // (length-3 clause base = 1.5·0.5 = 0.75 < typical var ≈ 5).
  // Sep clauses still win because picker_alpha_clause is much larger
  // than picker_alpha_var, compensating for the small sigmoid base.
  double picker_clause_weight  = 1.5;
  double picker_base_floor     = 0.01;

  // When true, a NON-SEPARATOR branch (var pick, or clause pick on a
  // clause not in the carried separator) drops the ND node (passes
  // nd_node = -1 to branchOnLiteral / branchOnClause), but keeps the
  // `separator` vector carried — so descendants lose ND-hierarchy
  // descent below this point (matching legacy Stage-3 behaviour for
  // var picks; symmetric extension for non-sep clauses). Sep-clause
  // picks (the consumption spine) still carry nd_node forward. CLI:
  // -pickerNonSepKillsNd.
  bool picker_non_sep_kills_nd = false;

  // Exponent on the ρ factor in the rate-framework score:
  //   score(v) = ρ(v)^picker_rho_exp · τ_var(v)
  // ρ for sep elements ∈ [1, √2]; for non-sep ρ = 2. Larger w_ρ amplifies
  // the discount on sep elements. CLI: -pickerRhoExp, default 1.0.
  double picker_rho_exp = 1.0;

  // When true, sub-components reached via decomposition (post-consumption
  // and mid-consumption decompose paths) get nd_node = -1 forwarded
  // unconditionally — no fresh ND-sep is installed at any depth below
  // the root. Tests whether deep seps add value or are pure overhead.
  // CLI: -pickerRootSepOnly.
  bool picker_root_sep_only = false;

  // When true, the rate-framework picker skips the BCP cascade gain
  // contribution to (a, b) — uses only `1 + ε·activity` for τ inputs.
  // Diagnostic to test whether cascade gain biases picks badly on
  // binary-rich instances. CLI: -pickerNoCascadeGain.
  bool picker_no_cascade_gain = false;

  // Front-of-separator bonus: the FIRST currently-active element in the
  // carried separator vector (METIS-order) gets its rate divided by this
  // factor in the rate framework. > 1 means the front element is
  // preferred (effective rate smaller); = 1 disables the bonus. Tunes
  // how strictly the picker respects the planned consumption order:
  // larger values make the picker stick to METIS order; smaller values
  // allow more cascade-driven deviations. CLI: -pickerFrontBonus.
  double picker_front_bonus = 2.0;

  // Rate-based unified picker (per-var-growth-rate framework):
  //   var rate     = τ(pos_gain + ε·act_pos, neg_gain + ε·act_neg)
  //                  via Newton's method on τ^(-a) + τ^(-b) = 1
  //   clause rate  = τ(σ(L − m), L)
  //   separator rate = 2^(α + β),  α=sep size/n_active, β=larger child fraction
  //   sep elements get rate divided by (1 + α_sep · exp(−λ_sep · rel_k))
  //   pick = argmin over candidates (smallest exponential growth rate)
  // When SEPARATOR wins, picker returns BranchTarget::SEPARATOR; the
  // caller falls through to legacy Stage-2 sep-consumption. CLI:
  // -pickerRateFramework. Multiplicative-mode only.
  bool picker_rate_framework = false;

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

  // Verify cache keys: lookup() always misses, and store() compares the
  // newly computed count against any previously stored count for the
  // same key. A mismatch indicates a semantic bug in the canonical key.
  bool verify_cache = false;

  // L2 canonical-cache key mode.
  //   true  (default): compact — identity is the 128-bit canonical
  //                    hash alone. No clause multiset stored. Relies on
  //                    hash width for collision safety (~10^-20).
  //   false           : strict — also stores and compares the full
  //                    normalized clause multiset. Debug aid: lets us
  //                    tell a canonical-hash collision from a
  //                    canonicalization fault when a miscount is seen.
  //                    Intended to be removed once compact is proven.
  bool canonical_compact = true;

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

  // At every entry to solveComponent where depth <= dump_recursion_max_depth,
  // dump the super-component's formula state as a self-contained DIMACS
  // CNF to dump_recursion_dir/super_<id>.cnf. Plus a manifest at
  // dump_recursion_dir/log.txt with (id, depth, nvars, path) per dump.
  // Diagnostic for top-branch debugging: re-run ganak/our-solver on each
  // dump to find the smallest sub-formula where counts diverge.
  // dump_recursion_dir empty = disabled.
  std::string dump_recursion_dir = "";
  unsigned dump_recursion_max_depth = 0;

  // Diagnostic: disable the canonical-key anonymization pass
  // (signature ranking, polarity flip, singleton collapse). Use
  // raw per-component active-var indices instead. Two structurally
  // identical sub-components with different var orderings will then
  // hash differently — cache hits drop sharply, but if the bug
  // disappears, the anonymization pass is the carrier.
  bool no_anonymization = false;

  // Print canonical-key stats (CANON_STATS, CANON_MAX_BLOCK_HISTOGRAM,
  // CANON_STATS_ITER2) to stderr at end of solve. Off by default — these
  // are useful for portfolio/profiling work but noisy in production.
  bool print_canon_stats = false;

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
  // Independent per-literal watch-list permutation (separate from
  // perm_watch_lists_seed which uses a SHARED rng across literals).
  // perm_watch_indep_seed != 0 enables; mask filters which literals
  // get permuted (literal l permuted iff (l.raw() & perm_watch_indep_mask) != 0).
  // Use mask = ~0u to permute all literals; smaller masks for bisection.
  unsigned perm_watch_indep_seed   = 0;
  unsigned perm_watch_indep_mask   = 0xFFFFFFFFu;
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
