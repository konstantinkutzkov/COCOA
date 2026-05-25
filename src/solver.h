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
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <tuple>
#include <unordered_set>

// sat_check.h is C++11-clean (the CMS internals are pimpl'd into
// sat_check.cpp, which is the only TU compiled as C++17). Including
// the full header here lets std::unique_ptr<SatChecker> see the
// complete type for its destructor.
#include "sat_check.h"

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
    // Throttle the gettimeofday syscall: do the actual check once
    // every TIMEBOUND_POLL_MASK + 1 calls; otherwise return the
    // last cached result. Profile (commit c61cefc) showed the
    // syscall family (gettimeofday + __commpage_gettimeofday_internal
    // + mach_absolute_time) at ~5% of wall time on a hot t1_045 run,
    // because solveComponent + solveComponentImpl each check on
    // every recursive call.
    //
    // Granularity: up to 1023 recursive calls past the actual
    // deadline before we notice. At ~340 K decisions/s on t1_045
    // that's ~3 ms — negligible vs. user time bounds in seconds.
    // Once broken, cached value is true and stays true (elapsed
    // time only grows), so the timeout is sticky once seen.
    static constexpr unsigned TIMEBOUND_POLL_MASK = 1023;
    if ((time_bound_poll_counter_++ & TIMEBOUND_POLL_MASK) != 0) {
      return last_time_bound_result_;
    }
    timeval actual_time;
    gettimeofday(&actual_time, NULL);
    last_time_bound_result_ =
        (actual_time.tv_sec - start_time_.tv_sec > time_bound_);
    return last_time_bound_result_;
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

  // Throttling state for timeBoundBroken — see that method.
  unsigned time_bound_poll_counter_ = 0;
  bool     last_time_bound_result_  = false;

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

	// =====================================================================
	// Per-operation timing for cost breakdown investigation (2026-05-25).
	// Uses std::chrono::steady_clock for portable nanosecond timing.
	// =====================================================================
	enum OpKind {
		OP_ANALYZE,      // discoverComponentsOf call
		OP_CANONICAL,    // buildCanonicalKey call
		OP_L1_PEEK,      // ContentCache::l1_lookup
		OP_L2_PEEK,      // ContentCache::peek
		OP_PICK,         // pickBranchVariable
		OP_BCP,          // Solver::BCP
		OP_COUNT
	};
	mutable uint64_t op_count_[OP_COUNT] = {0};
	mutable uint64_t op_time_ns_[OP_COUNT] = {0};

	// RAII timer. Bracket an operation with: { OpTimer _t(this, OP_X); ... }
	struct OpTimer {
		const Solver *s;
		OpKind k;
		std::chrono::steady_clock::time_point t0;
		OpTimer(const Solver *sv, OpKind kind)
		    : s(sv), k(kind), t0(std::chrono::steady_clock::now()) {}
		~OpTimer() {
			auto dt = std::chrono::steady_clock::now() - t0;
			s->op_time_ns_[k] +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
			s->op_count_[k]++;
		}
	};

	// Print current op_stats to stderr. Called from periodic progress
	// emission AND from final stats line.
	void printOpStats(const char *tag) const {
		static const char *names[OP_COUNT] = {
			"ANALYZE", "CANONICAL", "L1_PEEK", "L2_PEEK", "PICK", "BCP"
		};
		std::cerr << "OP_STATS " << tag;
		for (int i = 0; i < OP_COUNT; i++) {
			uint64_t c = op_count_[i];
			uint64_t t_ns = op_time_ns_[i];
			double avg_ns = c > 0 ? double(t_ns) / c : 0;
			std::cerr << " " << names[i]
			          << ":c=" << c
			          << ":ms=" << (t_ns / 1000000.0)
			          << ":avg=" << avg_ns << "ns";
		}
		std::cerr << "\n";
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
	CanonicalKey _computeRootCanonicalKey() {
		Component &root = comp_manager_.superComponentOf(stack_.top());
		return buildCanonicalKey(
		    root, literal_pool_, literals_, literal_values_,
		    comp_manager_.getAnalyzer().clauseIdToOfs(),
		    removed_clauses_, original_lit_pool_size_);
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

	// Test-support: prepare the solver for key-building without
	// starting a full solve (parse file, preprocess, init component
	// manager). Does NOT allocate the guard variable — tests that
	// need only the canonical-key computation don't need it.
	void _prepareForKeyComputation(const std::string &file_name);
	// Test-support: same as above but skips simplePreProcess so free
	// vars (declared but unused) survive into canonical_key building.
	void _prepareForKeyComputationNoPP(const std::string &file_name);
	// Test-support: force a single literal to T_TRI (no BCP). Returns
	// false if the literal was already assigned.
	bool _forceLitForTest(int dimacs_lit) {
		LiteralID lit = (dimacs_lit > 0)
		    ? LiteralID((unsigned)dimacs_lit, true)
		    : LiteralID((unsigned)(-dimacs_lit), false);
		return setLiteralIfFree(lit);
	}

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

	// OPEN_WORK snapshot — captured once on first time-bound break.
	// Tracks the nested chain of in-progress sub-components: pushed at
	// every solveComponentImpl entry, popped on exit (RAII guard inside
	// solveComponentImpl). At timeout we walk this chain, count active
	// vars in each sub-component, and emit the size list. See
	// docs/portfolio_plan.md §6 for the rationale (per-variant ranking
	// metric for probe_flags).
	//
	// The chain is nested (inner ⊂ outer in terms of vars), so the
	// outermost element bounds the inner work; the innermost element is
	// the deepest in-progress sub-component (= how far the search has
	// descended). The script's ranker uses both.
	bool                       open_work_captured_   = false;
	double                     open_work_log2_bound_ = 0.0;  // = max of chain
	unsigned                   open_work_n_root_     = 0;
	unsigned                   open_work_n_open_     = 0;
	std::vector<unsigned>      open_work_sizes_;             // outer → inner
	std::vector<Component *>   current_comp_chain_;          // live during search
	void captureOpenWorkSnapshot();

	// Phase 1 SAT-check diagnostic state (see solver_config.h::sat_check_every).
	// cms_solver_ holds an incremental CMSat::SATSolver populated with the
	// post-preprocess original CNF once at solve() startup. Subsequent
	// sat_check_diagnose_() calls pass the current literal_stack_ as
	// assumptions; CMS retains learned clauses across calls. Allocated
	// only when sat_check_every > 0 to avoid the startup cost otherwise.
	std::unique_ptr<SatChecker>       cms_solver_;
	unsigned long long                sat_check_counter_  = 0;
	unsigned long long                sat_check_n_calls_  = 0;
	unsigned long long                sat_check_n_unsat_  = 0;
	unsigned long long                sat_check_n_sat_    = 0;
	unsigned long long                sat_check_n_undef_  = 0;
	double                            sat_check_total_us_ = 0.0;
	void sat_check_init_();
	void sat_check_diagnose_();

	// Derivative-cache probe (Phase 1/2/3). long_clause_hashes_[ofs]
	// holds a random uint64_t per long clause (lazy, learned-only —
	// originals use content-aware hashing via Instance::clause_content_hash_).
	//
	// deriv_cache_xors_seen_ accumulates the XOR fingerprint of every
	// sub-component stored in the L2 cache. Phase 1/2 used a
	// std::unordered_set<uint64_t> — exact but expensive at scale
	// (~16 B/entry; 730 M L2 stores ⇒ ~12 GB). Phase 3 switches to a
	// 1 GB Bloom filter (2^33 bits, k=4) — ~10× memory reduction at
	// peak L2 size with bounded false-positive cost (canonical-key
	// check at the end catches Bloom FPs; only wasted work is the
	// canonical-key build itself).
	class DerivCacheBloom {
	public:
		static constexpr size_t kBitsLog2 = 33;            // 2^33 bits = 1 GB
		static constexpr size_t kBits  = size_t(1) << kBitsLog2;
		static constexpr size_t kMask  = kBits - 1;
		static constexpr int    kK     = 4;

		DerivCacheBloom()
		    : bits_((kBits + 63) / 64, 0ULL), n_inserts_(0) {}

		void insert(uint64_t key) {
			uint64_t h1 = key * 0x9E3779B97F4A7C15ULL;
			uint64_t h2 = key * 0xBF58476D1CE4E5B9ULL;
			for (int k = 0; k < kK; k++) {
				size_t idx = (h1 + (uint64_t)k * h2) & kMask;
				bits_[idx >> 6] |= (uint64_t)1 << (idx & 63);
			}
			n_inserts_++;
		}

		bool may_contain(uint64_t key) const {
			uint64_t h1 = key * 0x9E3779B97F4A7C15ULL;
			uint64_t h2 = key * 0xBF58476D1CE4E5B9ULL;
			for (int k = 0; k < kK; k++) {
				size_t idx = (h1 + (uint64_t)k * h2) & kMask;
				if (!(bits_[idx >> 6] & ((uint64_t)1 << (idx & 63))))
					return false;
			}
			return true;
		}

		size_t inserts() const { return n_inserts_; }

	private:
		std::vector<uint64_t> bits_;
		size_t                n_inserts_;
	};

	std::unordered_map<ClauseOfs, uint64_t> long_clause_hashes_;
	std::unique_ptr<DerivCacheBloom>  deriv_cache_xors_seen_;
	unsigned long long                deriv_cache_counter_       = 0;  // throttle
	unsigned long long                deriv_cache_n_probes_      = 0;
	// Var-branching: hypothetical F|v=T or F|v=F via setLiteralIfFree + BCP.
	unsigned long long                deriv_cache_n_xor_hits_    = 0;
	unsigned long long                deriv_cache_n_real_hits_   = 0;
	// Clause-branching (removal-only): hypothetical F\{c} via XOR-out of
	// the clause's content hash. No BCP, no state mutation for the XOR
	// pre-filter step — only mark/unmark when running the canonical-key
	// follow-up after a positive pre-filter.
	unsigned long long                deriv_cache_n_cl_xor_hits_  = 0;
	unsigned long long                deriv_cache_n_cl_real_hits_ = 0;
	// Phase 3 bias: counters for hits actually USED to short-circuit a
	// branch (a subset of real_hits; some real hits are not the chosen
	// candidate because a higher-priority kind won).
	unsigned long long                deriv_cache_n_used_var_both_  = 0;
	unsigned long long                deriv_cache_n_used_cl_ie_     = 0;
	unsigned long long                deriv_cache_n_used_var_one_   = 0;
	// Histogram of used CLAUSE_IE by abstract_budget at the time of the
	// hit. High budget = close to root, big saved sub-tree.
	//   bucket 0: budget <  10
	//   bucket 1: 10 ≤ budget < 30
	//   bucket 2: 30 ≤ budget < 60
	//   bucket 3: 60 ≤ budget < 80
	//   bucket 4: budget ≥ 80
	static constexpr unsigned kDerivCacheBudgetBuckets = 5;
	unsigned long long deriv_cache_used_cl_ie_budget_hist_[kDerivCacheBudgetBuckets] = {0};
	// Last time the periodic DERIV_CACHE_BIAS log was emitted (elapsed
	// wall seconds via stopwatch_). -1 means never. Used to throttle
	// the periodic bias-stats log to once per 60s on long runs.
	double deriv_cache_last_log_s_ = -1.0;
	// Profiling: time (μs) spent in deriv_cache_bias_select_ overall,
	// in the clause-scan inner loop (XOR checks per candidate, before
	// the canonical-key build), and in canonical-key builds invoked
	// from bias (clause-IE path only).
	double deriv_cache_bias_total_us_      = 0.0;
	double deriv_cache_bias_scan_us_       = 0.0;
	double deriv_cache_bias_canonical_us_  = 0.0;
	double deriv_cache_bias_xorinit_us_    = 0.0;
	// Connectivity-gate scratch: union-find parent array indexed by var
	// ID (1-based; entry 0 unused). Resized once at solve start to
	// (num_variables + 2). Each bias call re-initializes parent[v] = v
	// for the active vars it visits and leaves other entries untouched
	// — we only follow find() chains rooted at vars we just initialised.
	std::vector<unsigned> deriv_cache_bias_dsu_parent_;
	// Diagnostic: debug map from formula_xor → canonical_key (hash, hash_hi)
	// for the LAST stored sub-formula with that xor. Populated when
	// config_.deriv_cache_dump_fp > 0. On a Bloom-positive / L2-miss
	// (FP), we look up here to get the colliding stored entry's
	// canonical_key and dump the case to stderr.
	// (hash, hash_hi, num_vars, n_in_clauses, num_clauses)
	struct DebugKeyTuple {
		uint64_t hash, hash_hi;
		unsigned num_vars, n_in_clauses, num_clauses;
	};
	std::unordered_map<uint64_t, DebugKeyTuple>
	    deriv_cache_debug_xor_to_key_;
	// Side debug map storing a stringified comp structure for each
	// xor (limited capacity). Lets collision dumps include BOTH the
	// prior formula and the current.
	std::unordered_map<uint64_t, std::string>
	    deriv_cache_debug_xor_to_struct_;
	unsigned long long deriv_cache_fp_dumped_ = 0;
	void deriv_cache_init_();
	uint64_t deriv_cache_component_xor_(Component &comp);
	void deriv_cache_record_store_(Component &comp,
	                               const CanonicalKey *key_for_debug = nullptr);
	void deriv_cache_probe_(Component &comp);

	// Phase 3 bias-driven branch selection. Probes top-K var candidates
	// and active original clauses; if a derivative is cached, returns a
	// classified choice the caller can short-circuit on. NONE means no
	// hit — fall through to normal branching. See deriv_cache_bias_select_
	// in solver.cpp for the priority rule.
	struct DerivCacheBranchChoice {
		enum Kind { NONE, VAR_BOTH, VAR_ONE, CLAUSE_IE };
		Kind kind = NONE;
		VariableIndex var = 0;
		bool cached_polarity = false;    // VAR_ONE: which polarity is cached
		ClauseOfs cl_ofs = 0;            // CLAUSE_IE
		mpz_class arm0_count;            // VAR_BOTH: T scaled; VAR_ONE: cached scaled; CLAUSE_IE: F\{c} scaled
		mpz_class arm1_count;            // VAR_BOTH: F scaled; others unused
	};
	DerivCacheBranchChoice deriv_cache_bias_select_(
	    Component &comp,
	    const std::vector<CutNode> &separator,
	    double abstract_budget);

	// Periodic PROGRESS line emission for trajectory analysis. Diagnostic:
	// when last_progress_emit_s_ + PROGRESS_TICK_S seconds have passed,
	// re-run captureOpenWorkSnapshot non-destructively and emit
	//   PROGRESS t=<elapsed> progress_bits=<x> open=<n> bound_log2=<x>
	// to stderr. Doesn't set open_work_captured_; the final timeout
	// snapshot still captures cleanly.
	double                     last_progress_emit_s_ = -1.0;  // -1 = never emitted

	// Cumulative log-sum-exp over every LEAF event of the abstract-budget
	// passed to that event. Monotonically increasing by construction.
	// Sums in 2^budget space to exactly 2^n_root when the search finishes,
	// because the budget-threading invariant guarantees that every
	// solveComponent invocation contributes its budget once to a leaf.
	// fraction_processed = 2^(closed_log_sum_ - n_root). Initialised
	// to -inf so the first event sets it to that subtree's budget exactly.
	double closed_log_sum_ = -std::numeric_limits<double>::infinity();

	// A solveComponent call with abstract_budget B is responsible for
	// resolving 2^B abstract leaves of the worst-case decision tree.
	// When the call terminates without further recursion (cache hit,
	// trivial closure, conflict), it issues exactly one credit of B
	// log-bits. When the call recurses (branch / decomposition), the
	// budget is split among children such that 2^B is conserved, and
	// the LEAVES of the recursion sum the credit to 2^B.
	//
	// Uses the stable log-sum-exp recurrence:
	//   new = max(old, B) + log2(1 + 2^(min(old,B) - max(old,B)))
	void noteResolved(double log_bits) {
		double dk = log_bits;
		double old = closed_log_sum_;
		if (old == -std::numeric_limits<double>::infinity()) {
			closed_log_sum_ = dk;
		} else if (dk > old) {
			closed_log_sum_ = dk + std::log2(1.0 + std::exp2(old - dk));
		} else {
			closed_log_sum_ = old + std::log2(1.0 + std::exp2(dk - old));
		}
	}

	ComponentManager comp_manager_ = ComponentManager(config_,
			statistics_, literal_values_);

	// Static (preprocessing-time) WL labels. Indexed by var ID
	// (1-based; entry 0 unused). Computed once after preprocessing
	// and used as a final-step refiner in buildCanonicalKey for vars
	// still in collision blocks after dynamic WL.
	std::vector<uint64_t> static_wl_labels_;

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
	// `precomputed_key` (optional): the caller already built the canonical
	// key for `comp` and L2-peeked the cache with a confirmed MISS. When
	// non-null, solveComponent's entry skips the redundant buildCanonicalKey
	// + L2 peek and uses *precomputed_key directly. Caller is responsible
	// for ensuring (a) the value is bit-identical to what buildCanonicalKey
	// would produce here, (b) the cache state hasn't changed since the
	// caller's peek (single-threaded), and (c) the L2 peek genuinely missed.
	// Currently used only at solveComponentImpl's post-consumption decompose
	// loop. See docs/precomputed_key_safety_analysis.md for the verification.
	mpz_class solveComponent(Component &comp,
	                         std::vector<CutNode> separator,
	                         bool separator_reset,
	                         int depth = 0,
	                         int nd_node = -1,  // hierarchy node, -1 = use root
	                         int reactive_metis_skip_until_depth = 0,
	                         double abstract_budget = 0.0,
	                         const CanonicalKey *precomputed_key = nullptr);

	// The body of solveComponent. The public solveComponent wraps this
	// with function-boundary memoization (canonical-key lookup at
	// entry, store on return). All recursive calls go through the
	// public wrapper to benefit from the cache uniformly.
	//
	// `abstract_budget` (log-bits) is the share of the worst-case
	// 2^n_root decision tree this call must resolve. Threaded through:
	// at a binary branch each child receives `parent - 1`; at a
	// decomposition each sub-comp i receives `parent + a_i - logsumexp(a)`
	// so the children's 2^budget sums equal the parent's 2^budget exactly.
	mpz_class solveComponentImpl(Component &comp,
	                             std::vector<CutNode> separator,
	                             bool separator_reset,
	                             int depth = 0,
	                             int nd_node = -1,
	                             int reactive_metis_skip_until_depth = 0,
	                             double abstract_budget = 0.0);
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
	// `child_abstract_budget` is the budget for the recursive
	// solveComponent call made by this branch (= parent budget - 1).
	// On a conflict-at-entry leaf (F_TRI / BCP failure) this budget is
	// credited directly via noteResolved, since no solveComponent call
	// is made to issue the credit for us.
	mpz_class branchOnLiteral(LiteralID lit,
	                           Component &comp,
	                           std::vector<CutNode> separator,
	                           bool separator_reset,
	                           int depth = 0,
	                           int nd_node = -1,
	                           bool from_separator = false,
	                           int reactive_metis_skip_until_depth = 0,
	                           double child_abstract_budget = 0.0);
	// Branch on a clause (removed vs removed+negated).
	mpz_class branchOnClause(ClauseOfs cl_ofs,
	                          Component &comp,
	                          std::vector<CutNode> separator,
	                          bool separator_reset,
	                          bool negate_literals,
	                          int depth = 0,
	                          int nd_node = -1,
	                          int reactive_metis_skip_until_depth = 0,
	                          double child_abstract_budget = 0.0);
	// Discover independent sub-components of the current formula state
	// restricted to `super_comp`. Returns owned components.
	std::vector<Component*> discoverComponentsOf(Component &super_comp,
	                                              mpz_class &trivial_factor);
	// Select next variable to branch on within comp (highest activity score).
	VariableIndex pickBranchVariable(Component &comp);

	// Stage C: result of the unified picker — either a VAR (id is a
	// variable index) or a CLAUSE (id is a clause offset).
	struct BranchTarget {
		enum Kind { VAR, CLAUSE };
		Kind kind = VAR;
		unsigned id = 0;
		float score = -1.0f;
	};
	// The unified (-unifiedPicker) branch-target picker.
	// Multiplicative scoring: base(x) · boost(x). See body in
	// solver_rec.cpp for the formula and rationale.
	BranchTarget pickBranchTarget(Component &comp,
	                              const std::vector<CutNode> &separator);

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
	// CNF to `path`. Idempotent; no side effects on the solver state.
	// Returns true on success, false on file-open error.
	bool dumpPreprocessedCnf(const std::string &path);

	// Emit a single sub-component as a standalone DIMACS CNF under the
	// current partial assignment. Used by -bruteForceCacheDumpDir to
	// capture offending sub-components on brute-force cache mismatch.
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

	// INV_T5: brute-force-verify a learned clause D is entailed by
	// F\removed_clauses_ at learn time. Env-gated SHARPSAT_VERIFY_LEARN_N.
	// Aborts on the first unsoundness found, dumping the offending model.
	void verifyLearnedClauseSound(const std::vector<LiteralID> &D,
	                               ClauseOfs cl_ofs,
	                               const char *call_site = "?");

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

	// Concrete-example dumper: enumerate originals-only models of `sub`,
	// then for each confined in-scope learned clause check whether it
	// EXCLUDES any of those models. Print such "culprit" clauses with
	// full learn-context (lits + current values + learn-time scope +
	// current removed_clauses_). Use only on small comps (n_active ≤ ~22).
	void dumpUnsoundLearnedClauses(class Component &sub);

	// Env-gated L2-store-time verifier (no-op unless SHARPSAT_VERIFY_*
	// env vars are set):
	//   SHARPSAT_VERIFY_STORE=N    — brute-force-verify comp's count under
	//                                originals matches the about-to-be-stored
	//                                `result`, for comps with n_active ≤ N.
	//   SHARPSAT_VERIFY_LEARNED=N  — additionally verify that originals-only
	//                                count and originals+in-scope-learned
	//                                count agree (INV_S2-style check).
	//   SHARPSAT_DUMP_UNSOUND=N    — on a LEARNED_UNSOUND fire, dump the
	//                                first N offending learned clauses with
	//                                full context.
	// Each diagnostic caps at 50 fires to keep run time tractable.
	void verifyL2Store(class Component &comp,
	                   const struct CanonicalKey &cached_key,
	                   const mpz_class &result,
	                   int depth);


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
		// by BCP" (range [0, log 2]). Only competes with freq/activity
		// to break ties.
		if (config_.cascade_score_weight > 0.0) {
			score += (float)(config_.cascade_score_weight
			                 * computeCascadeScore(v));
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

	// Newton-iterate τ from τ^(-a) + τ^(-b) = 1. Returns the unique
	// τ > 1 satisfying the equation. Precondition: a, b > 0.
	static double tauBranchingNumber(double a, double b);

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
		    && !learnedClauseInScopeOrSound(ant.asCl(), config_.sound_provenance)) {
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
		literal_stack_.push_back(lit);
		if (ant.isAClause() && ant.asCl() != NOT_A_CLAUSE)
			getHeaderOf(ant.asCl()).increaseScore();
		literal_values_[lit] = T_TRI;
		literal_values_[lit.neg()] = F_TRI;
		if (deriv_cache_hooks_enabled_) deriv_cache_track_lit_assign_(lit);
		// Bump the throttle counter for the mid-consumption decompose
		// gate, but only on BRANCHING DECISIONS (no antecedent).
		// BCP-forced lits don't bump it — on dense instances BCP can
		// cascade many lits per decision and would saturate the
		// counter at every recursion if counted.
		if (!ant.isAnt()) decisions_since_connectivity_check_++;
		return true;
	}

	// Set `lit` as a "branch-constraint" assignment at the current
	// decision level. Used by branchOnClause's negate arm for ¬l_2..¬l_k
	// (¬l_1 stays a regular decision via setLiteralIfFree). The
	// is_branch_constraint_[v] flag tells UIP to NOT treat this as a
	// decision (would re-create the multi-decision-DL UIP bug) and to
	// NOT try to resolve through any antecedent clause (none exists);
	// instead UIP adds the literal's negation to the learned clause
	// directly, capturing the structural ¬C constraint.
	//
	// NB: does NOT increment num_implications_ (not a real implication)
	// or decisions_since_connectivity_check_ (not a real decision); both
	// metrics treat branch-constraint as a third category.
	bool setLiteralAsBranchConstraint(LiteralID lit) {
		if (literal_values_[lit] != X_TRI)
			return false;
		assert(literal_values_[lit.neg()] == X_TRI
		       && "polarity invariant: opposite must be X_TRI");
		var(lit).decision_level = stack_.get_decision_level();
		var(lit).ante = Antecedent(NOT_A_CLAUSE);
		is_branch_constraint_[lit.var()] = true;
		literal_stack_.push_back(lit);
		literal_values_[lit] = T_TRI;
		literal_values_[lit.neg()] = F_TRI;
		if (deriv_cache_hooks_enabled_) deriv_cache_track_lit_assign_(lit);
		return true;
	}

	void setConflictState(LiteralID litA, LiteralID litB) {
		violated_clause.clear();
		violated_clause.push_back(litA);
		violated_clause.push_back(litB);
		last_violated_clause_ofs_ = NOT_A_CLAUSE;
	}
	void setConflictState(ClauseOfs cl_ofs) {
		getHeaderOf(cl_ofs).increaseScore();
		violated_clause.clear();
		for (auto it = beginOf(cl_ofs); *it != SENTINEL_LIT; it++)
			violated_clause.push_back(*it);
		last_violated_clause_ofs_ = cl_ofs;
	}

	// ClauseOfs of the long clause that triggered the most recent
	// conflict (NOT_A_CLAUSE for binary conflicts). Used by UIP analysis
	// to seed the provenance chain with the conflict-triggering clause.
	ClauseOfs last_violated_clause_ofs_ = NOT_A_CLAUSE;

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

	// Snapshot the most-recent UIP resolution chain into the provenance
	// map for `learned_ofs`. Assumes last_analysis_chain_ holds the
	// long-clause antecedents and last_violated_clause_ofs_ holds the
	// conflict-trigger clause (if long; NOT_A_CLAUSE for binary).
	// Binary partner antecedents (last_analysis_chain_binaries_) are
	// skipped — original binaries are unconditionally sound.
	void recordLearnedClauseProvenance(ClauseOfs learned_ofs) {
		std::vector<ClauseOfs> prov;
		prov.reserve(last_analysis_chain_.size() + 1);
		if (last_violated_clause_ofs_ != NOT_A_CLAUSE)
			prov.push_back(last_violated_clause_ofs_);
		for (ClauseOfs c : last_analysis_chain_) prov.push_back(c);
		learned_clause_provenance_[learned_ofs] = std::move(prov);
	}


	// Diagnostic: record every antecedent clause walked during the
	// MOST RECENT conflict analysis. Cleared at the start of
	// recordLastUIPCauses / recordAllUIPCauses; appended each time
	// the loop reads an antecedent (clause or binary partner-as-clause).
	// Read by verifyLearnedClauseSound (INV_T5) to print the
	// resolution chain when an unsound clause is detected.
	std::vector<ClauseOfs> last_analysis_chain_;
	std::vector<LiteralID> last_analysis_violated_clause_;
	// Also track binary antecedents (as the partner literal) — they're
	// not ClauseOfs so they go in a separate vector.
	std::vector<LiteralID> last_analysis_chain_binaries_;

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
