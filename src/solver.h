/*
 * solver.h
 *
 *  Created on: Aug 23, 2012
 *      Author: marc
 */

#ifndef SOLVER_H_
#define SOLVER_H_


#include "statistics.h"
#include "instance.h"
#include "component_management.h"
#include "nd_hierarchy.h"
#include "canonical_key.h"



#include "solver_config.h"

#include <sys/time.h>
#include <tuple>
#include <unordered_set>

enum retStateT {
	EXIT, RESOLVED, PROCESS_COMPONENT, BACKTRACK
};

// Result of a one-shot BCP probe (see Solver::probeLiteral).
// vars_forced and delta_2clauses are undefined when success == false.
struct ProbeResult {
	bool success;
	int  vars_forced;
	int  delta_2clauses;
};



class StopWatch {
public:

  StopWatch();

  bool timeBoundBroken() {
    timeval actual_time;
    gettimeofday(&actual_time, NULL);
    return actual_time.tv_sec - start_time_.tv_sec > time_bound_;
  }

  bool start() {
    bool ret = gettimeofday(&last_interval_start_, NULL);
    start_time_ = stop_time_ = last_interval_start_;
    return !ret;
  }

  bool stop() {
    return gettimeofday(&stop_time_, NULL) == 0;
  }

  double getElapsedSeconds() {
    timeval r = getElapsedTime();
    return r.tv_sec + (double) r.tv_usec / 1000000;
  }

  bool interval_tick() {
    timeval actual_time;
    gettimeofday(&actual_time, NULL);
    if (actual_time.tv_sec - last_interval_start_.tv_sec
        > interval_length_.tv_sec) {
      gettimeofday(&last_interval_start_, NULL);
      return true;
    }
    return false;
  }

  void setTimeBound(long int seconds) {
    time_bound_ = seconds;
  }
  long int getTimeBound();

private:
  timeval start_time_;
  timeval stop_time_;

  long int time_bound_;

  timeval interval_length_;
  timeval last_interval_start_;

  // if we have started and then stopped the watch, this returns
  // the elapsed time
  // otherwise, time elapsed from start_time_ till now is returned
  timeval getElapsedTime();
};

class Solver: public Instance {
public:
	Solver() {
		stopwatch_.setTimeBound(config_.time_bound_seconds);
	}

	void solve(const string & file_name);

	// Queue equivalences (in input-CNF variable space, 1-indexed) to be
	// injected into the redundant-binary lane after simplePreProcess
	// finishes (i.e., after compactVariables / HardWireAndCompact). Each
	// tuple is (var_x, var_y, same_polarity). Translation from input
	// space to the post-compactification variable space is done by solve()
	// at injection time using compact_to_orig_; equivalences whose vars
	// were eliminated by preprocessing are silently dropped (the
	// elimination already encodes the equivalence's effect on the count).
	void setPendingRedundantEquivalences(
	    std::vector<std::tuple<unsigned, unsigned, bool>> equivs) {
		pending_redundant_equivs_ = std::move(equivs);
	}

	// Test-support entry point: load the CNF, initialize the decision
	// stack, apply the unit clauses, and run BCP. Returns true iff no
	// conflict is derived during the initial propagation. Does NOT run
	// the full preprocessing (failed-literal pre-pass, hard-wire compact).
	// Intended for unit tests that want to exercise probeLiteral /
	// commitFailedLiteral against a known baseline state.
	bool initForTesting(const std::string &file_name);

	// One-shot BCP probe with rollback. Sets `lit`, runs BCP. On failure
	// populates uip_clauses_ via recordAllUIPCauses. Rolls back the
	// literal stack to its entry state unconditionally. On success
	// reports the cascade size (including `lit`) and the signed change
	// in the count of active 2-clauses.
	//
	// probeLiteral does the full O(L) 2-clause scans before and after
	// BCP to compute delta_2clauses. Callers that only need pass/fail
	// (e.g. implicitBCP) should use probeLiteralPassFail instead, which
	// skips the scoring scans entirely.
	ProbeResult probeLiteral(LiteralID lit);

	// Lightweight probe: same BCP-with-rollback + UIP-recording behavior
	// as probeLiteral, but skips the vars_forced / delta_2clauses
	// bookkeeping. Returns true iff BCP succeeded. Used by implicitBCP
	// where only the failed-literal signal is consumed.
	bool probeLiteralPassFail(LiteralID lit);

	// Installs the UIP clauses from the most recent failed probe as
	// antecedents for the forced literals, then runs BCP. Returns false
	// iff the resulting BCP derives a new conflict (component UNSAT).
	// Precondition: uip_clauses_ was populated by a probeLiteral that
	// returned success == false.
	bool commitFailedLiteral();

	// Counts active 2-clauses in the current formula state. A clause is
	// an active 2-clause iff it is in scope, not satisfied, not removed,
	// and has exactly two non-falsified literals. Binary clauses are
	// counted via binary_links_; non-binary via a walk over literal_pool_.
	int count_active_2clauses();

	// Read-only access to the UIP clauses from the most recent failed
	// probe. Used by test harnesses to assert on learned-clause content.
	const std::vector<std::vector<LiteralID>> &uip_clauses_view() const {
		return uip_clauses_;
	}

	// Test-support entry points: after initForTesting (or a full solve
	// start), compute the canonical key for the super-component. Used
	// by test_canonical_key_invariance to verify the key is invariant
	// under stored-clause-literal permutations.
	CanonicalKey _computeRootCanonicalKey(bool compact = false) {
		Component &root = comp_manager_.superComponentOf(stack_.top());
		return buildCanonicalKey(
		    root, literal_pool_, literals_, literal_values_,
		    comp_manager_.getAnalyzer().clauseIdToOfs(),
		    removed_clauses_, original_lit_pool_size_, compact);
	}

	// Test-support: randomly permute the stored-literal order within
	// each original long clause in literal_pool_. Preserves SENTINEL_LIT
	// terminators. Watch-list references track clauses by ClauseOfs,
	// not literal position within the clause body, so this is safe as
	// a one-shot perturbation BEFORE search starts.
	void _permuteClauseLiteralsForTest(unsigned seed);

	// Test-support: inject a clause at the "learned" end of the formula
	// — long clauses go past original_lit_pool_size_, binaries go past
	// original_binary_link_count_. Used to verify the canonical-key
	// filters correctly exclude learned content. These methods are
	// intentionally ugly-named so they are not confused with production
	// learning paths.
	//
	// The test contract: for any injected learned clause (long or
	// binary) over currently-active variables, the canonical key of
	// the super-component must be IDENTICAL to the key computed
	// before the injection. If it differs, the filter is broken.
	void _injectLearnedLongClauseForTest(const std::vector<int> &dimacs_lits);
	void _injectLearnedBinaryForTest(int dimacs_a, int dimacs_b);

	// Test-support: is this variable currently unassigned (active)?
	bool _isActiveForTest(unsigned v) {
		return v >= 1 && v <= num_variables()
		    && literal_values_[LiteralID(v, true)] == X_TRI;
	}

	// Test-support: number of variables after preprocessing.
	unsigned _numVariablesForTest() { return num_variables(); }

	// Order-sensitivity probes (post-preprocess, pre-search). See
	// SolverConfiguration for the purpose. Each is idempotent and
	// operates on the current in-memory representation; applying
	// multiple probes composes (seeds are independent).

	// Shuffle stored-literal order within each long clause AND
	// maintain the watch invariant (remove ofs from old
	// lits[0]/lits[1]'s watch lists, add ofs to new lits[0]/lits[1]'s
	// watch lists). Safe for a subsequent full solve.
	void permuteClauseLiteralsSafe(unsigned seed);

	// Shuffle binary_links_[l] for every literal l, preserving the
	// trailing SENTINEL_LIT. BCP over binary_links_ is order-sensitive
	// in the sense of "which implied literal gets pushed first", so
	// this probe tests whether that ordering affects the final count.
	void permuteBinaryLinksOrder(unsigned seed);

	// Shuffle watch_list_[l] for every literal l, preserving the
	// leading SENTINEL_CL (position 0). The solver iterates via
	// .rbegin() so shuffling changes the clause traversal order
	// during BCP.
	void permuteWatchListsOrder(unsigned seed);
	void permuteWatchListsIndep(unsigned seed, uint32_t mask);

	// Shuffle occurrence_lists_[l] for every literal l. Component
	// analysis iterates occurrence_lists_ to find clauses containing
	// each variable; if that iteration order matters for correctness
	// (it shouldn't, for a sound counter), this probe will expose it.
	void permuteOccurrenceListsOrder(unsigned seed);

	// Canonicalize (sort) each order-sensitive structure. Each function
	// sorts its target by a deterministic key and preserves structural
	// invariants (SENTINEL_LIT/CL in the right positions, watches on
	// first-two-stored lits of each clause).
	void sortBinaryLinksOrder();
	void sortWatchListsOrder();
	void sortOccurrenceListsOrder();
	void sortClauseLiteralsSafe();

	// Rewrite literal_pool_ with original clauses in canonical (sorted)
	// order. Rebuilds watch_list_ and occurrence_lists_ against the new
	// ofs values. Call AFTER sortClauseLiteralsSafe so the comparison
	// keys are canonical. Only original clauses are affected — call
	// post-preprocess, before any learned clauses are added.
	void sortClausePoolOrder();

	// Test-support: prepare the solver for key-building without
	// starting a full solve (parse file, preprocess, init component
	// manager). Does NOT allocate the guard variable — tests that
	// need only the canonical-key computation don't need it.
	void _prepareForKeyComputation(const std::string &file_name);

	SolverConfiguration &config() {
		return config_;
	}

	DataAndStatistics &statistics() {
	        return statistics_;
	}
	void setTimeBound(long int i) {
		stopwatch_.setTimeBound(i);
	}

private:
	SolverConfiguration config_;

	DecisionStack stack_; // decision stack
	vector<LiteralID> literal_stack_;

	StopWatch stopwatch_;

	ComponentManager comp_manager_ = ComponentManager(config_,
			statistics_, literal_values_);

	// Static (preprocessing-time) WL labels. Indexed by var ID
	// (1-based; entry 0 unused). Computed once after preprocessing
	// and used as a final-step refiner in buildCanonicalKey for vars
	// still in collision blocks after dynamic WL.
	std::vector<uint64_t> static_wl_labels_;

	// Separator-as-bias mode: set of VAR ids the picker should boost.
	// Filled lazily via markSeparatorBias() from the separator-acceptance
	// gate when config_.separator_vars_as_bias is on; consulted in
	// scoreOf. Size is num_variables()+1 (1-based, entry 0 unused).
	std::vector<bool> sep_bias_active_;

	// Throttle counter for mid-consumption decompose checks. Bumped
	// only on BRANCHING DECISIONS (setLiteralIfFree call where
	// !ant.isAnt() — i.e. no antecedent, top-level decision).
	// BCP-forced literals are NOT counted: they cascade rapidly on
	// dense instances and would saturate the gate on every recursion.
	// Reset when discoverComponentsOf runs (mid-consumption gate or
	// post-consumption block). Consulted only when
	// config_.decompose_in_separator is on.
	unsigned long decisions_since_connectivity_check_ = 0;

	// Equivalences queued by setPendingRedundantEquivalences; consumed
	// by solve() after preprocessing and before search starts.
	std::vector<std::tuple<unsigned, unsigned, bool>> pending_redundant_equivs_;

	// Counters for the experimental -decomposeInSep / -cacheInSep paths,
	// printed at solve end via printMidSepStats().
public:
	unsigned long long mid_sep_decomp_attempts_ = 0;
	unsigned long long mid_sep_decomp_splits_   = 0;
private:

	// the last time conflict clauses have been deleted
	unsigned long last_ccl_deletion_time_ = 0;
	// the last time the conflict clause storage has been compacted
	unsigned long last_ccl_cleanup_time_ = 0;

	bool simplePreProcess();
	bool prepFailedLiteralTest();
	// we assert that the formula is consistent
	// and has not been found UNSAT yet
	// hard wires all assertions in the literal stack into the formula
	// removes all set variables and essentially reinitiallizes all
	// further data
	void HardWireAndCompact();

	// First-cut structural analyzer: picks Stage-0 α from cheap
	// formula statistics (mean active-clause length after
	// preprocessing). Runs only when config_.auto_stage0_length_decay
	// is true and -adaptive is enabled. The user can force a specific
	// α via -adaptiveAlpha; that flag turns the analyzer off.
	//
	// Mapping (first cut; to be refined from measurement):
	//   mean_len < 3.5  (dense 3-SAT-ish)  →  α = 0.5
	//   mean_len < 6.0  (medium)            →  α = 1.0
	//   mean_len ≥ 6.0  (sparse, long cls)  →  α = 2.0
	void analyzeAndSetHyperparameters();

	// Extract the current formula as a list of DIMACS int vectors,
	// one per clause. Includes long clauses (size >= 3) and binaries
	// (emitted once each). Does NOT include unit clauses —
	// HardWireAndCompact has already cleared unit_clauses_ by the
	// time this is called in simplePreProcess. Const because it
	// reads state only.
	std::vector<std::vector<int>> extractFormulaAsDimacs();

	// Reset all in-memory formula state and repopulate from the
	// preprocessor's output. After this call, literal_pool_,
	// watch_list_, binary_links_, occurrence_lists_, unit_clauses_,
	// literal_values_, literal_stack_, and per-variable DL/ante are
	// in a clean-slate state — identical to what createfromFile
	// would produce on a fresh DIMACS file containing the
	// preprocessed CNF. Call must be followed by the standard
	// unit-BCP + HardWireAndCompact to absorb any forced units.
	void rebuildFromPreprocessedCNF(const struct PreprocessorResult &pre_out);


	// Phase 2 / Tier 1 gating: decide whether a precomputed ND-hierarchy
	// separator is acceptable for the current sub-component. Rejects
	// separators that are too large (|sep|/n > separator_max_ratio) or
	// too imbalanced (min(L,R)/(L+R) < separator_min_balance), where L
	// and R are the counts of active component vars falling into the
	// left/right child subtrees of `nd_node`.
	//
	// Called after the component-filter step in solver_rec.cpp. Returns
	// true iff both gates pass (and therefore the separator should be
	// used). Returns true trivially when the separator is empty or when
	// nd_node is invalid / leaf (no balance signal available); the gate
	// is only meaningful for non-empty separators at internal nodes.
	bool hierarchySeparatorAcceptable(int nd_node,
	                                  Component &comp,
	                                  unsigned filtered_sep_size);

	// Separator size gate, shared between the precomputed-hierarchy path
	// and the reactive METIS path. Returns true iff sep_size is acceptable
	// for a sub-component with n_active active variables.
	// Policy: allowed_sep = min(0.3 * n_active, 20).
	static bool separatorSizeAcceptable(unsigned sep_size, unsigned n_active) {
		if (sep_size == 0 || n_active == 0) return true;
		unsigned allowed = std::min((unsigned)(0.3 * (double)n_active), 100u);  // DIAG: relax 20→100
		return sep_size <= allowed;
	}

	// Phase 3 / Tier 2: adaptive branching via probe-scored τ minimization.
	// Returns the chosen branching variable, or 0 if no branching is
	// possible. When `out_unsat` is set to true on return, the component
	// is UNSAT (model count = 0). When `out_unsat == false && return == 0`,
	// the component is already fully assigned (model count = 1).
	//
	// Implementation (see docs/adaptive_branching_plan.md Phase 3):
	//   - outer loop: gather active vars in comp, Stage 0 cheap filter
	//     (clause-length-weighted occurrence sum with exponential decay),
	//     take top-K by cheap score.
	//   - probe each top-K candidate in both polarities. On failed literal
	//     (one polarity conflicts), commit the UIP(s) and restart the
	//     outer loop against the simplified formula. On both polarities
	//     failing, return UNSAT.
	//   - once no failed literals are found in a pass, pick the candidate
	//     minimizing the branching number τ from τ^(-a) + τ^(-b) = 1.
	VariableIndex pickBranchVariableAdaptive(Component &comp, bool &out_unsat);

	// Extract a runtime snapshot of `comp` in the form expected by
	// computeRuntimeMetisSeparator: active variables, active long
	// clauses (each with its current unassigned literals' vars), and
	// active binary clauses (deduplicated pairs). Used for the
	// reactive-METIS measurement path.
	void buildMetisInputFromComponent(
	    Component &comp,
	    std::vector<unsigned> &active_vars,
	    std::vector<std::pair<unsigned, std::vector<unsigned>>> &long_clauses,
	    std::vector<std::pair<unsigned, unsigned>> &binary_pairs);

	// Stage 0 cheap-score helper: fills `cheap[v]` for every active
	// variable in `comp` using the clause-length-weighted sum
	// Σ 2^(-α · active_len(C)). Fills `candidates` with unassigned
	// vars of comp. No probing, no side effects on the formula state.
	void stage0_cheap_scores(Component &comp,
	                         double alpha,
	                         std::vector<double> &cheap,
	                         std::vector<VariableIndex> &candidates);

	// Recursive implementation (defined in solver_rec.cpp).
	// Entry point: returns exit state; final count is stored in statistics_.
	SOLVER_StateT countSATRec();
	// Workhorse: compute #SAT of the current formula state restricted
	// to component `comp`, given the remaining separator elements.
	//
	// `reactive_metis_skip_until_depth` implements the failure throttle
	// for reactive METIS: reactive METIS is only attempted when
	// `depth >= reactive_metis_skip_until_depth`. On a reactive-METIS
	// failure this value is advanced to `depth + skip_k` for the rest
	// of this subtree; on success it is left unchanged. The value is
	// propagated verbatim through separator consumption and into child
	// sub-components. Only a failed reactive call inside the current
	// subtree raises it.
	mpz_class solveComponent(Component &comp,
	                         std::vector<CutNode> separator,
	                         bool separator_reset,
	                         int depth = 0,
	                         int nd_node = -1,  // hierarchy node, -1 = use root
	                         int reactive_metis_skip_until_depth = 0);

	// The body of solveComponent. The public solveComponent wraps this
	// with function-boundary memoization (canonical-key lookup at
	// entry, store on return). All recursive calls go through the
	// public wrapper to benefit from the cache uniformly.
	mpz_class solveComponentImpl(Component &comp,
	                             std::vector<CutNode> separator,
	                             bool separator_reset,
	                             int depth = 0,
	                             int nd_node = -1,
	                             int reactive_metis_skip_until_depth = 0);
	// Branch on a literal, run BCP, recurse, then restore state.
	//
	// `from_separator` distinguishes the two call sites:
	//  - true  : consuming a variable element of the precomputed
	//            separator. Clause learning MUST be disabled here —
	//            a learned clause can add incidence-graph edges that
	//            violate the separator's structural invariant (the
	//            separator must separate F, no exceptions; BCP + clause
	//            removal can only shrink connectivity, but learning
	//            adds it). If we learn during separator branching, a
	//            future `mapToChild` may return -1 because the learned
	//            clause now connects variables across subtree boundaries.
	//  - false : regular variable branching on the no-separator path.
	//            Learning is allowed and safe (scoped by
	//            removed_clauses_ for clause-branch contexts).
	mpz_class branchOnLiteral(LiteralID lit,
	                           Component &comp,
	                           std::vector<CutNode> separator,
	                           bool separator_reset,
	                           int depth = 0,
	                           int nd_node = -1,
	                           bool from_separator = false,
	                           int reactive_metis_skip_until_depth = 0);
	// Branch on a clause (removed vs removed+negated).
	mpz_class branchOnClause(ClauseOfs cl_ofs,
	                          Component &comp,
	                          std::vector<CutNode> separator,
	                          bool separator_reset,
	                          bool negate_literals,
	                          int depth = 0,
	                          int nd_node = -1,
	                          int reactive_metis_skip_until_depth = 0);
	// Discover independent sub-components of the current formula state
	// restricted to `super_comp`. Returns owned components.
	std::vector<Component*> discoverComponentsOf(Component &super_comp,
	                                              mpz_class &trivial_factor);
	// Select next variable to branch on within comp (highest activity score).
	VariableIndex pickBranchVariable(Component &comp);

	// Stage C: result of the unified picker — either a VAR (id is a
	// variable index) or a CLAUSE (id is a clause offset).
	struct BranchTarget {
		enum Kind { VAR, CLAUSE, SEPARATOR };
		Kind kind = VAR;
		unsigned id = 0;
		float score = -1.0f;
	};
	// Score every active VAR + every long active CLAUSE in `comp`,
	// return the argmax. CLAUSE candidates are gated on the existing
	// `clause_branch_min_length` so we don't clause-branch on
	// binaries (which collapse to var-branching anyway). Clauses in
	// `sep_clauses` get the separator-bias bonus on their score.
	BranchTarget pickBranchTarget(Component &comp,
	                              const std::vector<CutNode> &sep_clauses,
	                              int nd_node = -1);

	// Multiplicative-mode picker (S = base · boost). Regime A only:
	// γ=0, no per-call cascade-cost overhead. See solver_rec.cpp.
	BranchTarget pickBranchTargetMultiplicative(
	    Component &comp,
	    const std::vector<CutNode> &separator);

	// Rate-based unified picker. Returns SEPARATOR / VAR / CLAUSE
	// per the smallest exponential growth rate. See solver_rec.cpp.
	BranchTarget pickBranchTargetRate(
	    Component &comp,
	    const std::vector<CutNode> &separator,
	    int nd_node);

	// Phase 4: implicant learning helpers (see solver_config.h for design).
	// Walks the antecedent chain backward from `l_star` and collects the
	// decision literals that appeared along the way (DL > 0). Returns an
	// empty vector if the collected set would exceed max_size.
	// `out_chain_depth` receives the number of antecedent-expansion
	// steps performed during the walk (= number of non-decision literals
	// popped from the frontier and expanded via their antecedent). A
	// depth of 1 means `l_star`'s ante was expanded but no recursion
	// occurred; higher depths mean longer BCP chains.
	std::vector<LiteralID> deriveDecisionImplicant(
	    LiteralID l_star, unsigned max_size, unsigned *out_chain_depth = nullptr);
	// Mine implicants for literals forced during the BCP call that
	// started at literal-stack position `bcp_start_ofs`. Applies
	// size / non-trivial / dedup filters and stops at the total cap.
	void maybeLearnImplicants(unsigned bcp_start_ofs);

	// Diagnostic: scan conflict_clauses_ at end of solve and report
	// duplication + subsumption statistics. Read-only.
	void analyzeLearnedClausePool();
	// Diagnostic: scan the ORIGINAL formula (clauses in literal_pool_
	// below original_lit_pool_size_) and report how much subsumption /
	// SSR simplification would be available. Read-only. Intended to
	// answer: would a preprocessing pass on F itself be worth writing?
	void analyzeOriginalClausePool();

	// Invariant guard (reusable): verify the formula is
	// unit-propagation saturated — every clause has ≥ 2 active
	// (X_TRI) literals, or is already satisfied (≥ 1 T_TRI literal),
	// or is flagged as removed. If any active clause has exactly
	// one active literal with all others falsified, preprocessing
	// left a forced literal un-propagated; that literal should have
	// been pushed to literal_stack_ and BCP'd to completion.
	//
	// Runtime check: aborts with diagnostic if the invariant is
	// violated. Intended to be called immediately after preprocessing
	// and optionally at each branching boundary to catch silent
	// state corruption.
	void verifyUnitPropagationSaturated(const char *label);

	// Invariant guard: verify the solver's in-memory representational
	// state is consistent with the formula. Checks:
	//   - literal_values_[lit] and literal_values_[lit.neg()] are
	//     polar opposites (both X_TRI, or one T/other F).
	//   - For each clause in literal_pool_, the two currently-stored
	//     front literals are in their respective watch lists.
	//   - binary_links_ is symmetric: if l2 is in binary_links_[l1]
	//     then l1 is in binary_links_[l2].
	//   - occurrence_lists_[lit] references only clauses that actually
	//     contain lit (original clauses only; learned clauses are not
	//     tracked by occurrence_lists_).
	//
	// O(total literal occurrences) per call — affordable as a one-off
	// check immediately after preprocessing, not per branch. Aborts
	// with diagnostic if any invariant is violated.
	void verifyStateIntegrity(const char *label);

	// Dump all binaries in binary_links_ after preprocessing to a file,
	// one per line as DIMACS. Purpose: post-process with an external SAT
	// solver to check if each is entailed by the original formula F.
	// Spurious binaries (not entailed by F) indicate preprocessing bugs
	// (e.g., cleanClause creating binaries from satisfied clauses).
	void dumpBinariesAfterPreprocess(const std::string &path);

	// Invariant guard: verify the solver is at a true clean slate after
	// preprocessing. Every per-variable/per-literal scratch field, every
	// search-scoped collection, and every counter that search will
	// accumulate into must be at its default-constructed value — no
	// residue from the first BCP or the failed-literal probing phase.
	//
	// Motivation (2026-04-22 t1_011 investigation): the in-memory solve
	// on the original CNF produces an undercount, but a DIMACS dump of
	// the post-preprocess state fed back to a fresh solver produces the
	// correct count. Something that isn't preserved by the DIMACS dump
	// differs between the two entry points. This guard + the
	// resetPostPreprocessScratch() reset below rule out "residue from
	// preprocessing" as the cause.
	void verifyPostPreprocessCleanSlate(const char *label);

	// Emit the current post-preprocess in-memory formula as a DIMACS
	// CNF to `path`. Called from solve() after preprocessing and after
	// any order-probe perturbations, so a -permBinaryLinks / -permClauseLits
	// run produces a self-contained CNF reproducer of the perturbed
	// state. Idempotent; no side effects on the solver state. Returns
	// true on success, false on file-open error.
	bool dumpPreprocessedCnf(const std::string &path);

	// Emit a single sub-component as a standalone DIMACS CNF under the
	// current partial assignment. Variables are remapped to the 1..N range
	// in index-sorted order so the file is self-contained. Falsified
	// literals are dropped; satisfied clauses are omitted. Learned and
	// removed clauses are omitted. Binaries among active component
	// variables are emitted once.
	//
	// Purpose: during search, the solver can dump each solveComponent's
	// sub-problem to a file and log the returned count. A post-process
	// script can then run ganak on each CNF and find the SMALLEST
	// sub-component where our count and ganak disagree — a minimal
	// in-tree bug reproducer.
	bool dumpSubComponentCnf(class Component &comp, const std::string &path,
	                         unsigned *out_nvars = nullptr);

	// Brute-force #SAT counter for a sub-component over its currently-
	// active variables. Iterates over 2^|active_vars| assignments and
	// counts those satisfying every active original clause + binary
	// (learned clauses excluded — they're sound consequences of
	// originals so they don't change #SAT). Returns -1 if the
	// sub-component is too big to brute-force (above N_max).
	mpz_class bruteForceCountSubcomp(class Component &sub,
	                                  unsigned n_max,
	                                  unsigned *out_active_n = nullptr);

	// Stricter brute-force: enumerate as above but ALSO require each
	// model to satisfy every in-scope learned clause confined to this
	// sub-component (i.e., learned clauses whose vars are all active
	// in `sub` and whose `learnedClauseInScope` returns true now).
	// Returns the count over (originals AND in-scope-confined learned).
	// Returns -1 if too big. If `out_n_learned_checked` provided,
	// outputs the number of learned clauses that participated.
	mpz_class bruteForceCountSubcompWithLearned(class Component &sub,
	                                             unsigned n_max,
	                                             unsigned *out_active_n = nullptr,
	                                             unsigned *out_n_learned_checked = nullptr);

	// Append one LEARN record to config_.learn_trace_path. Called at the
	// learn site after addScopedUIPConflictClause / addUIPConflictClause
	// has produced a non-zero ClauseOfs. Records the new clause's ofs,
	// size, content, the current decision stack (each lit with its DL
	// and antecedent type), and the current clause-branching scope.
	void logLearnTrace(ClauseOfs cl_ofs, const std::vector<LiteralID> &clause);

	// Explicit, one-shot reset of every search-scoped field. Idempotent.
	// Called from HardWireAndCompact; anything it clears is cheap
	// (empty containers) or a primitive zero. See
	// verifyPostPreprocessCleanSlate for the invariant it enforces.
	void resetPostPreprocessScratch();

	// Invariant guard: verify there are no "failed literals" remaining.
	// A failed literal is a literal l such that setting l and running
	// BCP derives a conflict. In that case ¬l is entailed and should
	// have been forced during preprocessing. If this guard finds any
	// failed literal, preprocessing's prepFailedLiteralTest was
	// incomplete.
	//
	// Cost: O(n × BCP) per call. Too expensive for per-branch use;
	// intended as a one-off check right after preprocessing. Uses
	// the existing probeLiteralPassFail primitive which has clean
	// state rollback.
	void verifyNoFailedLiterals(const char *label);

	// Diagnostic: after branchOnLiteral's BCP settles, scan the clauses
	// AFFECTED by the branching (those containing a newly-falsified
	// literal) and count how many now subsume some other clause in
	// their effective form. Read-only; sampled by
	// config_.analyze_dynamic_subsumption_every. Answers the dynamic
	// question the static analyzers cannot: does branching create
	// subsumption opportunities the stored formula doesn't show?
	void analyzeDynamicSubsumption(unsigned bcp_start_ofs);

	bool bcp();

	// Precomputed nested-dissection hierarchy (built once at solve start)
	NDHierarchy nd_hierarchy_;

	// Per-variable centrality score derived from the ND-hierarchy
	// (computed once after hierarchy build). nd_centrality_score_[v] is
	// (max_depth − depth_of(leaf_containing_v)) so vars near the root
	// rank higher. Indexed by COMPACT var IDs (same as var_leaf).
	std::vector<float> nd_centrality_score_;
	void computeNdCentrality();

	// Reactive-METIS measurement accumulators. Populated when
	// config_.measure_reactive_metis is on; printed at end-of-solve.
	unsigned long long reactive_metis_calls_   = 0;
	double             reactive_metis_total_us_ = 0.0;
	double             reactive_metis_max_us_   = 0.0;
	unsigned long long reactive_metis_sum_nvars_ = 0;
	unsigned long long reactive_metis_sum_sep_   = 0;
	unsigned long long reactive_metis_failed_    = 0;  // returned no sep
	unsigned long long reactive_metis_accepted_ = 0;  // passed both gates, used
	unsigned long long reactive_metis_gate1_rej_ = 0; // Gate 1 rejected
	unsigned long long reactive_metis_gate2_rej_ = 0; // Gate 2 rejected
	// Bucketed histograms (by input var count) for a sense of scaling.
	// Buckets: [0,16), [16,32), [32,64), [64,128), [128,256), [256,512), [512,inf)
	static constexpr int kReactiveBuckets = 7;
	unsigned long long reactive_metis_bucket_count_[kReactiveBuckets] = {0};
	double             reactive_metis_bucket_total_us_[kReactiveBuckets] = {0};

	 void decayActivitiesOf(Component & comp) {
	   for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++) {
	          literal(LiteralID(*it,true)).activity_score_ *=0.5;
	          literal(LiteralID(*it,false)).activity_score_ *=0.5;
	       }
	}
	///  this method performs Failed literal tests online
	bool implicitBCP();

	// this is the actual BCP algorithm
	// starts propagating all literal in literal_stack_
	// beginingg at offset start_at_stack_ofs
	bool BCP(unsigned start_at_stack_ofs);

	/////////////////////////////////////////////
	//  BEGIN small helper functions
	/////////////////////////////////////////////

	float scoreOf(VariableIndex v) {
		float score = comp_manager_.scoreOf(v);
		score += 10.0 * literal(LiteralID(v, true)).activity_score_;
		score += 10.0 * literal(LiteralID(v, false)).activity_score_;
		if (config_.nd_centrality_weight > 0.0
		    && (size_t)v < nd_centrality_score_.size()) {
			score += (float)(config_.nd_centrality_weight)
			       * nd_centrality_score_[v];
		}
//		score += (10*stack_.get_decision_level()) * literal(LiteralID(v, true)).activity_score_;
//		score += (10*stack_.get_decision_level()) * literal(LiteralID(v, false)).activity_score_;

		// τ-based BCP-cascade addend. When cascade_score_weight > 0,
		// augment freq+activity with log(2 / τ(1 + k_+, 1 + k_-))
		// — the branching-number-derived proxy for "branches saved
		// by BCP" (range [0, log 2]). Bounded magnitude so it never
		// overpowers separator_bias_weight (1000) when sep is active;
		// only competes with freq/activity to break ties on non-sep vars.
		if (config_.cascade_score_weight > 0.0) {
			score += (float)(config_.cascade_score_weight
			                 * computeCascadeScore(v));
		}

		// Separator-as-bias mode: VARs the ND/reactive separator gate has
		// accepted are treated as preferred branch targets, mirroring
		// ganak's td_score addend (counter.cpp:1289). The bias persists
		// across recursive solveComponent calls — once a var is marked, it
		// keeps the boost for the remainder of the search.
		// Static separator-bias bonus, used only outside the unified
		// picker. Under -unifiedPicker the picker applies a
		// dynamic-magnitude bonus (scaled by a^(-k)) itself, so we
		// must not double-count here.
		if (config_.separator_vars_as_bias
		    && !config_.unified_picker
		    && v < sep_bias_active_.size()
		    && sep_bias_active_[v]) {
			score += (float)config_.separator_bias_weight;
		}
		return score;
	}

	// Discrete coeff^k BCP-cascade addend with min aggregation over
	// polarities. See solver.cpp for details.
	float computeCascadeScore(VariableIndex v);
	// Gain-based BCP cascade estimate: sum over the two polarities of
	// hypothetical(v=lit) → (#literals BCP would force) + 0.33·(#3-clauses
	// shortened to binaries). Walks forced literals up to
	// cascade_score_depth levels via binary_links_ + occurrence_lists_.
	float computeBcpGainScore(VariableIndex v);
	// Polarity-split version of the same gain walker: returns
	// (pos_gain, neg_gain) so the caller can feed them to Newton's
	// τ-branching-number computation.
	std::pair<float, float> computeBcpGainPolarities(VariableIndex v);

	// Newton-iterate τ from τ^(-a) + τ^(-b) = 1. Returns the unique
	// τ > 1 satisfying the equation. Precondition: a, b > 0.
	static double tauBranchingNumber(double a, double b);

	// Mark VAR elements of an accepted separator as preferred branch
	// targets. Lazy-resizes sep_bias_active_ on first call.
	void markSeparatorBias(const std::vector<CutNode> &sep) {
		if (sep_bias_active_.size() <= num_variables())
			sep_bias_active_.assign(num_variables() + 1, false);
		for (const auto &nd : sep)
			if (nd.kind == CutNode::VAR && nd.id < sep_bias_active_.size())
				sep_bias_active_[nd.id] = true;
	}

	bool setLiteralIfFree(LiteralID lit,
			Antecedent ant = Antecedent(NOT_A_CLAUSE)) {
		if (literal_values_[lit] != X_TRI)
			return false;
		if (config_.log_conflicts) {
			const char *atype = "dec";
			std::string ante_desc;
			if (ant.isAnt()) {
				if (ant.isAClause()) {
					atype = "cl";
					ante_desc = "ofs=" + std::to_string(ant.asCl());
				} else {
					atype = "bin";
					ante_desc = "partner=" + std::to_string(ant.asLit().toInt());
				}
			}
			std::cerr << "FORCE lit=" << lit.toInt()
			          << " ante=" << atype
			          << " " << ante_desc
			          << " DL=" << stack_.get_decision_level()
			          << " decisions=";
			for (auto l : literal_stack_)
				if (!var(l).ante.isAnt()) std::cerr << l.toInt() << ",";
			std::cerr << "\n";
		}
		// Guard 4: polarity invariant. Entering this branch means
		// literal_values_[lit] == X_TRI; the opposite polarity must
		// also be X_TRI, otherwise the two views of the same variable
		// are desynchronised — a BCP state corruption bug.
		assert(literal_values_[lit.neg()] == X_TRI
		       && "literal_values_ polarity invariant: opposite must be X_TRI");
		// Invariant C (gated): if the antecedent is a learned clause,
		// it must be in scope under the current removed_clauses_ at
		// the moment we record it. BCP's pre-firing scope check at
		// solver.cpp:766-768 should already have ensured this; this
		// guard is a sanity check that BCP itself isn't recording an
		// out-of-scope antecedent. See solver_config.h::check_learn_invariants.
		if (config_.check_learn_invariants
		    && ant.isAClause() && ant.asCl() != NOT_A_CLAUSE
		    && ant.asCl() >= (ClauseOfs)original_lit_pool_size_
		    && !learnedClauseInScope(ant.asCl())) {
			std::cerr << "\n*** INV_C_ANTECEDENT_OUT_OF_SCOPE_AT_FIRING ***\n"
			          << "  lit=" << lit.toInt()
			          << "  ante_cl=" << ant.asCl()
			          << "  current_removed=" << removed_clauses_.size()
			          << "  DL=" << stack_.get_decision_level() << "\n";
			std::cerr.flush();
			std::abort();
		}
		var(lit).decision_level = stack_.get_decision_level();
		var(lit).ante = ant;
		// Track BCP propagations vs decisions. ant.isAnt() == true means
		// this literal was forced by BCP (not chosen as a decision).
		if (ant.isAnt()) statistics_.num_implications_++;
		// Chain-depth caching for fast implicant-filter lookup. Only
		// computed when implicant learning is enabled — costs ~O(k)
		// per forcing (k = antecedent clause length) but saves the
		// O(chain_size) walk for every literal whose depth is below
		// the min_chain_depth threshold. Decisions have depth 0.
		if (config_.perform_implicant_learning) {
			uint8_t cd = 0;
			if (ant.isAnt()) {
				if (ant.isAClause()) {
					ClauseOfs ofs = ant.asCl();
					for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++) {
						if (lt->var() == lit.var()) continue;
						uint8_t lt_cd = variables_[lt->var()].chain_depth;
						if (lt_cd > cd) cd = lt_cd;
					}
				} else {
					// Binary antecedent: ante.asLit() is the falsified
					// partner; its variable's chain_depth is stored.
					cd = variables_[ant.asLit().var()].chain_depth;
				}
				if (cd < 255) cd++;  // saturate at 255 (max of uint8_t)
			}
			var(lit).chain_depth = cd;
		}
		literal_stack_.push_back(lit);
		if (ant.isAClause() && ant.asCl() != NOT_A_CLAUSE)
			getHeaderOf(ant.asCl()).increaseScore();
		literal_values_[lit] = T_TRI;
		literal_values_[lit.neg()] = F_TRI;
		// Bump the throttle counter for the mid-consumption decompose
		// gate, but only on BRANCHING DECISIONS (no antecedent).
		// BCP-forced lits don't bump it — on dense instances BCP can
		// cascade many lits per decision and would saturate the
		// counter at every recursion if counted.
		if (!ant.isAnt()) decisions_since_connectivity_check_++;
		return true;
	}

	void setConflictState(LiteralID litA, LiteralID litB) {
		violated_clause.clear();
		violated_clause.push_back(litA);
		violated_clause.push_back(litB);
	}
	void setConflictState(ClauseOfs cl_ofs) {
		getHeaderOf(cl_ofs).increaseScore();
		violated_clause.clear();
		for (auto it = beginOf(cl_ofs); *it != SENTINEL_LIT; it++)
			violated_clause.push_back(*it);
	}

	void initStack(unsigned int resSize) {
		stack_.clear();
		stack_.reserve(resSize);
		literal_stack_.clear();
		literal_stack_.reserve(resSize);
		// initialize the stack to contain at least level zero
		stack_.push_back(StackLevel(1, 0, 2));
		stack_.back().changeBranch();
	}

	/////////////////////////////////////////////
	//  BEGIN conflict analysis
	/////////////////////////////////////////////

	// if the state name is CONFLICT,
	// then violated_clause contains the clause determining the conflict;
	vector<LiteralID> violated_clause;
	// this is an array of all the clauses found
	// during the most recent conflict analysis
	// it might contain more than 2 clauses
	// but always will have:
	//      uip_clauses_.front() the 1UIP clause found
	//      uip_clauses_.back() the lastUIP clause found
	//  possible clauses in between will be other UIP clauses
	vector<vector<LiteralID> > uip_clauses_;

	// the assertion level of uip_clauses_.back()
	// or (if the decision variable did not have an antecedent
	// before) then assertionLevel_ == DL;
	int assertion_level_ = 0;

	// build conflict clauses from most recent conflict
	// as stored in state_.violated_clause
	// solver state must be CONFLICT to work;
	// this first method record only the last UIP clause
	// so as to create clause that asserts the current decision
	// literal
	void recordLastUIPCauses();
	void recordAllUIPCauses();

	void minimizeAndStoreUIPClause(LiteralID uipLit,
			vector<LiteralID> & tmp_clause, bool seen[]);
	void storeUIPClause(LiteralID uipLit, vector<LiteralID> & tmp_clause);
	int getAssertionLevel() const {
		return assertion_level_;
	}

	/////////////////////////////////////////////
	//  END conflict analysis
	/////////////////////////////////////////////
};

#endif /* SOLVER_H_ */
