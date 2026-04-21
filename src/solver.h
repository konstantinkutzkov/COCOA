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



#include "solver_config.h"

#include <sys/time.h>

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

	bool bcp();

	// Precomputed nested-dissection hierarchy (built once at solve start)
	NDHierarchy nd_hierarchy_;

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
//		score += (10*stack_.get_decision_level()) * literal(LiteralID(v, true)).activity_score_;
//		score += (10*stack_.get_decision_level()) * literal(LiteralID(v, false)).activity_score_;

		return score;
	}

	bool setLiteralIfFree(LiteralID lit,
			Antecedent ant = Antecedent(NOT_A_CLAUSE)) {
		if (literal_values_[lit] != X_TRI)
			return false;
		// Guard 4: polarity invariant. Entering this branch means
		// literal_values_[lit] == X_TRI; the opposite polarity must
		// also be X_TRI, otherwise the two views of the same variable
		// are desynchronised — a BCP state corruption bug.
		assert(literal_values_[lit.neg()] == X_TRI
		       && "literal_values_ polarity invariant: opposite must be X_TRI");
		var(lit).decision_level = stack_.get_decision_level();
		var(lit).ante = ant;
		literal_stack_.push_back(lit);
		if (ant.isAClause() && ant.asCl() != NOT_A_CLAUSE)
			getHeaderOf(ant.asCl()).increaseScore();
		literal_values_[lit] = T_TRI;
		literal_values_[lit.neg()] = F_TRI;
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
