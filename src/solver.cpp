/*
 * solver.cpp
 *
 *  Created on: Aug 23, 2012
 *      Author: marc
 */
#include "solver.h"
#include <deque>
#include <cmath>
#include <limits>
#include <unordered_set>

#include <algorithm>


StopWatch::StopWatch() {
  interval_length_.tv_sec = 60;
  gettimeofday(&last_interval_start_, NULL);
  start_time_ = stop_time_ = last_interval_start_;
}

timeval StopWatch::getElapsedTime() {
  timeval r;
  timeval other_time = stop_time_;
  if (stop_time_.tv_sec == start_time_.tv_sec
      && stop_time_.tv_usec == start_time_.tv_usec)
    gettimeofday(&other_time, NULL);
  long int ad = 0;
  long int bd = 0;

  if (other_time.tv_usec < start_time_.tv_usec) {
    ad = 1;
    bd = 1000000;
  }
  r.tv_sec = other_time.tv_sec - ad - start_time_.tv_sec;
  r.tv_usec = other_time.tv_usec + bd - start_time_.tv_usec;
  return r;
}



bool Solver::simplePreProcess() {

	if (!config_.perform_pre_processing)
		return true;
	assert(literal_stack_.size() == 0);
	unsigned start_ofs = 0;
//BEGIN process unit clauses
	for (auto lit : unit_clauses_)
		setLiteralIfFree(lit);
//END process unit clauses
	bool succeeded = BCP(start_ofs);

	if (succeeded)
		succeeded &= prepFailedLiteralTest();

	if (succeeded)
		HardWireAndCompact();
	return succeeded;
}

bool Solver::prepFailedLiteralTest() {
	unsigned last_size;
	do {
		last_size = literal_stack_.size();
		for (unsigned v = 1; v < variables_.size(); v++)
			if (isActive(v)) {
				unsigned sz = literal_stack_.size();
				setLiteralIfFree(LiteralID(v, true));
				bool res = BCP(sz);
				while (literal_stack_.size() > sz) {
					unSet(literal_stack_.back());
					literal_stack_.pop_back();
				}

				if (!res) {
					sz = literal_stack_.size();
					setLiteralIfFree(LiteralID(v, false));
					if (!BCP(sz))
						return false;
				} else {

					sz = literal_stack_.size();
					setLiteralIfFree(LiteralID(v, false));
					bool resb = BCP(sz);
					while (literal_stack_.size() > sz) {
						unSet(literal_stack_.back());
						literal_stack_.pop_back();
					}
					if (!resb) {
						sz = literal_stack_.size();
						setLiteralIfFree(LiteralID(v, true));
						if (!BCP(sz))
							return false;
					}
				}
			}
	} while (literal_stack_.size() > last_size);

	return true;
}

void Solver::HardWireAndCompact() {
	compactClauses();
	compactVariables();
	literal_stack_.clear();

	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		literal(l).activity_score_ = literal(l).binary_links_.size() - 1;
		literal(l).activity_score_ += occurrence_lists_[l].size();
		literal(l).recordOriginalBinaryLinks();
	}

	statistics_.num_unit_clauses_ = unit_clauses_.size();

	statistics_.num_original_binary_clauses_ = statistics_.num_binary_clauses_;
	statistics_.num_original_unit_clauses_ = statistics_.num_unit_clauses_ =
			unit_clauses_.size();
	initStack(num_variables());
	original_lit_pool_size_ = literal_pool_.size();
}

void Solver::solve(const string &file_name) {
	stopwatch_.start();
	statistics_.input_file_ = file_name;

	createfromFile(file_name);
	initStack(num_variables());

	if (!config_.quiet) {
		cout << "Solving " << file_name << endl;
		statistics_.printShortFormulaInfo();
	}
	if (!config_.quiet)
		cout << endl << "Preprocessing .." << flush;
	bool notfoundUNSAT = simplePreProcess();
	if (!config_.quiet)
		cout << " DONE" << endl;

	if (notfoundUNSAT) {

		if (!config_.quiet) {
			statistics_.printShortFormulaInfo();
		}

		// Allocate the guard variable for scope-tracked binary UIP
		// learning. MUST be after simplePreProcess (HardWireAndCompact
		// would otherwise count d as a free variable and double the
		// count via num_free_variables_). See instance.h's guard_var_
		// comment for the full invariant.
		allocateGuardVariable();

		last_ccl_deletion_time_ = last_ccl_cleanup_time_ =
				statistics_.getTime();

		violated_clause.reserve(num_variables());

		comp_manager_.initialize(literals_, literal_pool_);
		comp_manager_.setRemovedClauses(&removed_clauses_);
		// Note: verify_cache logic is implemented in solver_rec.cpp at the
		// decomposition site. The ContentCache's own verify_mode is NOT
		// toggled here because we want the solver's lookup to behave
		// normally (return hits); solver_rec then recomputes independently
		// and compares cached vs. recomputed counts.
		if (config_.verify_cache)
			cout << "c verify_cache mode ON (compare cached vs. recomputed on every hit; aborts on mismatch)" << endl;

		statistics_.exit_state_ = countSATRec();
		// countSATRec sets final_solution_count internally
		statistics_.num_long_conflict_clauses_ = num_conflict_clauses();

	} else {
		statistics_.exit_state_ = SUCCESS;
		statistics_.set_final_solution_count(0.0);
		cout << endl << " FOUND UNSAT DURING PREPROCESSING " << endl;
	}

	// Guard 3: guard variable post-solve sanity — must still be assigned
	// to true at DL 0, never flipped to active. Violation means unSet
	// slipped through somewhere (Guard 2 should have caught it) or the
	// initial assignment was corrupted. Runtime check: the cost of a
	// silent undercount here is higher than a visible abort.
	if (guard_var_ != 0) {
		LiteralID d_pos(guard_var_, true);
		if (literal_values_[d_pos] != T_TRI
		    || variables_[guard_var_].decision_level != 0) {
			std::cerr << "*** GUARD_VAR_CORRUPTED at end of solve ***\n"
			          << "  literal_values_[d.pos]=" << (int)literal_values_[d_pos]
			          << " (expected T_TRI=" << (int)T_TRI << ")\n"
			          << "  decision_level=" << variables_[guard_var_].decision_level
			          << " (expected 0)\n";
			std::cerr.flush();
			std::abort();
		}
	}

	stopwatch_.stop();
	statistics_.time_elapsed_ = stopwatch_.getElapsedSeconds();

	comp_manager_.gatherStatistics();
	statistics_.writeToFile("data.out");
	if (!config_.quiet)
		statistics_.printShort();

	if (config_.verify_cache) {
		auto &cc = comp_manager_.contentCache();
		cout << "c verify_cache summary: forced_misses(hits_converted)=" << cc.stats_verify_checks
		     << " verified_matches=" << cc.stats_verify_ok
		     << " unique_stores=" << cc.stats_stores << endl;
	}

	if (config_.perform_implicant_learning) {
		cout << "\n=== Implicant learning summary ===" << endl;
		cout << "  learned           : " << statistics_.num_implicants_learned_ << endl;
		cout << "  dropped (size cap): " << statistics_.num_implicants_size_dropped_ << endl;
		cout << "  dropped (trivial) : " << statistics_.num_implicants_trivial_dropped_ << endl;
		cout << "  dropped (dedup)   : " << statistics_.num_implicants_dedup_dropped_ << endl;
		cout << "  quota reached     : " << (statistics_.num_implicants_quota_stop_ ? "yes" : "no") << endl;
	}

	if (config_.use_reactive_metis && reactive_metis_calls_ > 0) {
		cout << "\n=== Reactive-METIS summary ===" << endl;
		cout << "  calls            : " << reactive_metis_calls_ << endl;
		cout << "  failed (r.ok=0)  : " << reactive_metis_failed_ << endl;
		cout << "  gate1 rejected   : " << reactive_metis_gate1_rej_ << endl;
		cout << "  gate2 rejected   : " << reactive_metis_gate2_rej_ << endl;
		cout << "  accepted & used  : " << reactive_metis_accepted_ << endl;
		cout << "  total time (ms)  : "
		     << (reactive_metis_total_us_ / 1000.0) << endl;
		cout << "  mean time (us)   : "
		     << (reactive_metis_total_us_ / (double)reactive_metis_calls_) << endl;
		cout << "  max  time (us)   : " << reactive_metis_max_us_ << endl;
		cout << "  mean input vars  : "
		     << ((double)reactive_metis_sum_nvars_ / (double)reactive_metis_calls_) << endl;
		cout << "  mean sep size    : "
		     << ((double)reactive_metis_sum_sep_ / (double)reactive_metis_calls_) << endl;
		cout << "  overhead vs solve time : "
		     << (reactive_metis_total_us_ / 1e6 / std::max(1e-9, statistics_.time_elapsed_) * 100.0)
		     << " %" << endl;
		static const char *bucket_labels[kReactiveBuckets] = {
			"[0,16)", "[16,32)", "[32,64)", "[64,128)",
			"[128,256)", "[256,512)", "[512,inf)"};
		cout << "  bucket distribution (vars -> calls / mean us):" << endl;
		for (int i = 0; i < kReactiveBuckets; i++) {
			if (reactive_metis_bucket_count_[i] == 0) continue;
			cout << "    " << bucket_labels[i]
			     << " : " << reactive_metis_bucket_count_[i] << " calls, "
			     << (reactive_metis_bucket_total_us_[i]
			         / (double)reactive_metis_bucket_count_[i])
			     << " us mean" << endl;
		}
	}
}


bool Solver::bcp() {
// the asserted literal has been set, so we start
// bcp on all literals pushed at this decision level
	unsigned start_ofs = stack_.top().literal_stack_ofs();

//BEGIN process unit clauses
	for (auto lit : unit_clauses_)
		setLiteralIfFree(lit);
//END process unit clauses

	bool bSucceeded = BCP(start_ofs);

	if (config_.perform_failed_lit_test && bSucceeded && removed_clauses_.empty()
	    && !config_.perform_separator_branching) {
		bSucceeded = implicitBCP();
	}
	return bSucceeded;
}

bool Solver::BCP(unsigned start_at_stack_ofs) {
	for (unsigned int i = start_at_stack_ofs; i < literal_stack_.size(); i++) {
		LiteralID unLit = literal_stack_[i].neg();
		//BEGIN Propagate Bin Clauses
		for (auto bt = literal(unLit).binary_links_.begin();
				*bt != SENTINEL_LIT; bt++) {
			if (isResolved(*bt)) {
				setConflictState(unLit, *bt);
				return false;
			}
			setLiteralIfFree(*bt, Antecedent(unLit));
		}
		//END Propagate Bin Clauses
		for (auto itcl = literal(unLit).watch_list_.rbegin();
				*itcl != SENTINEL_CL; itcl++) {
			bool isLitA = (*beginOf(*itcl) == unLit);
			auto p_watchLit = beginOf(*itcl) + 1 - isLitA;
			auto p_otherLit = beginOf(*itcl) + isLitA;

			if (isSatisfied(*p_otherLit) || isClauseRemoved(*itcl))
				continue;
			// Scope check for learned clauses: if a learned clause was
			// derived when some clauses C were removed, it's only sound
			// in contexts where all of C are still removed. Otherwise
			// skip — treat the clause as absent for BCP purposes.
			// (Non-learned clauses have no scope entry and are always OK.)
			if (*itcl >= (ClauseOfs)original_lit_pool_size_
			    && !learnedClauseInScope(*itcl))
				continue;
			auto itL = beginOf(*itcl) + 2;
			while (isResolved(*itL))
				itL++;
			// either we found a free or satisfied lit
			if (*itL != SENTINEL_LIT) {
				literal(*itL).addWatchLinkTo(*itcl);
				swap(*itL, *p_watchLit);
				*itcl = literal(unLit).watch_list_.back();
				literal(unLit).watch_list_.pop_back();
			} else {
				// or p_unLit stays resolved
				// and we have hence no free literal left
				// for p_otherLit remain poss: Active or Resolved
				if (setLiteralIfFree(*p_otherLit, Antecedent(*itcl))) { // implication
					if (isLitA)
						swap(*p_otherLit, *p_watchLit);
				} else {
					if (config_.verbose)
						cout << "  CONFLICT_CL=" << *itcl
							 << " unLit=" << unLit.toInt()
							 << " removed=" << isClauseRemoved(*itcl) << endl;
					setConflictState(*itcl);
					return false;
				}
			}
		}
	}
	return true;
}

// IBCP (implicit BCP / failed literal testing)
// Disabled when separator branching is active — learned clauses can
// create cross-component connections that invalidate the decomposition.
bool Solver::implicitBCP() {
	static vector<LiteralID> test_lits(num_variables());
	static LiteralIndexedVector<unsigned char> viewed_lits(num_variables() + 1,
			0);

	unsigned stack_ofs = stack_.top().literal_stack_ofs();
	unsigned num_curr_lits = 0;
	while (stack_ofs < literal_stack_.size()) {
		test_lits.clear();
		for (auto it = literal_stack_.begin() + stack_ofs;
				it != literal_stack_.end(); it++) {
			for (auto cl_ofs : occurrence_lists_[it->neg()])
				if (!isSatisfied(cl_ofs) && !isClauseRemoved(cl_ofs)) {
					for (auto lt = beginOf(cl_ofs); *lt != SENTINEL_LIT; lt++)
						if (isActive(*lt) && !viewed_lits[lt->neg()]) {
							test_lits.push_back(lt->neg());
							viewed_lits[lt->neg()] = true;

						}
				}
		}
		num_curr_lits = literal_stack_.size() - stack_ofs;
		stack_ofs = literal_stack_.size();
		for (auto jt = test_lits.begin(); jt != test_lits.end(); jt++)
			viewed_lits[*jt] = false;

		vector<float> scores;
		scores.clear();
		for (auto jt = test_lits.begin(); jt != test_lits.end(); jt++) {
			scores.push_back(literal(*jt).activity_score_);
		}
		sort(scores.begin(), scores.end());
		num_curr_lits = 10 + num_curr_lits / 20;
		float threshold = 0.0;
		if (scores.size() > num_curr_lits) {
			threshold = scores[scores.size() - num_curr_lits];
		}

		statistics_.num_failed_literal_tests_ += test_lits.size();

		for (auto lit : test_lits) {
			if (!isActive(lit)) continue;
			if (threshold > literal(lit).activity_score_) continue;

			if (probeLiteralPassFail(lit)) continue;

			statistics_.num_failed_literals_detected_++;
			if (config_.verbose) {
				cout << "  IBCP_FAILED lit=" << lit.toInt()
					 << " removed_cls=" << removed_clauses_.size()
					 << " learned:";
				for (auto &cl : uip_clauses_) {
					cout << " [";
					for (auto l : cl) cout << l.toInt() << " ";
					cout << "]";
				}
				cout << endl;
			}
			if (!commitFailedLiteral())
				return false;
		}
	}

	return true;
}

// ----------------------------------------------------------------------------
// Phase 1 primitives (see docs/phase1_implementation_contract.md)
// ----------------------------------------------------------------------------

int Solver::count_active_2clauses() {
	int count = 0;

	// 1) Binary clauses: each active literal's active neighbors in
	//    binary_links_ counts one endpoint of an active binary clause.
	//    Each active binary is counted twice (once per endpoint), so we
	//    divide by two at the end.
	unsigned bin_endpoint_sum = 0;
	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		if (literal_values_[l] != X_TRI) continue;  // not active
		const auto &blinks = literals_[l].binary_links_;
		for (auto bt = blinks.begin(); *bt != SENTINEL_LIT; ++bt) {
			if (literal_values_[*bt] == X_TRI) bin_endpoint_sum++;
		}
	}
	count += (int)(bin_endpoint_sum / 2);

	// 2) Non-binary clauses: walk literal_pool_ linearly. The pool starts
	//    with a lone SENTINEL_LIT at position 0 (see Instance::createfromFile),
	//    so the first real clause's literals start at
	//    1 + ClauseHeader::overheadInLits(). Each clause ends at its
	//    SENTINEL_LIT, followed by the next clause's header slots.
	ClauseOfs cl_ofs = (ClauseOfs)(1 + ClauseHeader::overheadInLits());
	while (cl_ofs < (ClauseOfs)literal_pool_.size()) {
		bool in_scope = true;
		if (cl_ofs >= (ClauseOfs)original_lit_pool_size_
		    && !learnedClauseInScope(cl_ofs)) {
			in_scope = false;
		}
		if (isClauseRemoved(cl_ofs)) in_scope = false;

		bool satisfied = false;
		int nonfalsified = 0;
		auto it = literal_pool_.begin() + cl_ofs;
		for (; *it != SENTINEL_LIT; ++it) {
			TriValue v = literal_values_[*it];
			if (v == T_TRI) satisfied = true;
			if (v != F_TRI) nonfalsified++;
		}
		// it now points at SENTINEL_LIT. The next clause's header begins
		// immediately after the sentinel.
		cl_ofs = (ClauseOfs)((it - literal_pool_.begin()) + 1
		                      + ClauseHeader::overheadInLits());

		if (in_scope && !satisfied && nonfalsified == 2) count++;
	}

	return count;
}

bool Solver::probeLiteralPassFail(LiteralID lit) {
	const unsigned sz = literal_stack_.size();
	stack_.startFailedLitTest();
	setLiteralIfFree(lit);
	assert(!hasAntecedent(lit));

	const bool bSucceeded = BCP(sz);
	if (!bSucceeded) recordAllUIPCauses();

	stack_.stopFailedLitTest();
	while (literal_stack_.size() > sz) {
		unSet(literal_stack_.back());
		literal_stack_.pop_back();
	}
	assert(literal_stack_.size() == sz);
	return bSucceeded;
}

ProbeResult Solver::probeLiteral(LiteralID lit) {
	ProbeResult res{true, 0, 0};
	const int baseline_2c = count_active_2clauses();
	const unsigned sz = literal_stack_.size();

	stack_.startFailedLitTest();
	setLiteralIfFree(lit);
	assert(!hasAntecedent(lit));

	const bool bSucceeded = BCP(sz);
	if (!bSucceeded) {
		recordAllUIPCauses();
		res.success = false;
	} else {
		const int post_2c = count_active_2clauses();
		res.vars_forced    = (int)(literal_stack_.size() - sz);
		res.delta_2clauses = post_2c - baseline_2c;
	}

	stack_.stopFailedLitTest();
	while (literal_stack_.size() > sz) {
		unSet(literal_stack_.back());
		literal_stack_.pop_back();
	}
	assert(literal_stack_.size() == sz);

	return res;
}

bool Solver::commitFailedLiteral() {
	const unsigned sz = literal_stack_.size();
	// recordAllUIPCauses can emit an empty UIP clause when the conflict
	// is fully determined by literals at lower decision levels than the
	// probe's (e.g. inside a deep clause-branch scope where prior learned
	// clauses already make the formula UNSAT at the probe point). In that
	// case there is no literal to force — the component is UNSAT.
	//
	// Scope-aware learning: if we're at the root scope (removed_clauses_
	// empty) we use addUIPConflictClause (unscoped); if we're at a deeper
	// scope we use addScopedUIPConflictClause so the learned clause's
	// scope is recorded and learnedClauseInScope() gives the right
	// answer in sibling/ancestor contexts. addScopedUIPConflictClause
	// only learns non-binary (size >= 3) clauses — smaller UIPs are
	// forced via setLiteralIfFree with NOT_A_CLAUSE antecedent, which is
	// sound (the literal is an entailed unit of F at this scope, and is
	// unSet on backtrack).
	if (uip_clauses_.empty() || uip_clauses_.front().empty()) {
		return false;
	}
	// Scope-safe learning across the whole call path. We always use
	// addScopedUIPConflictClause:
	//   - size ≥ 3: the learned clause is added with its current scope
	//     (empty at root, {C, ...} inside clause branches). BCP's
	//     learnedClauseInScope check then keeps it sound across sibling
	//     branches.
	//   - size < 3: addScoped returns NOT_A_CLAUSE without learning, so
	//     we do NOT touch the global unit_clauses_ / binary_links_.
	//     Those have no scope tracking and no BCP scope guard, so a
	//     binary UIP derived under removed_clauses_ = {C} would
	//     propagate in the sibling ¬C branch and could be unsound
	//     there — causing a silent undercount (observed on t1_071).
	//     The forced literal is still committed (sound under the
	//     current state); we just don't persist the unit/binary form.
	//
	// Commit only the 1-UIP (uip_clauses_.front()); committing the full
	// chain changed counts on t1_071 and was never shown to help.
	auto &uip = const_cast<std::vector<LiteralID>&>(uip_clauses_.front());
	Antecedent ante = addScopedUIPConflictClause(uip);
	setLiteralIfFree(uip.front(), ante);
	return BCP(sz);
}

bool Solver::hierarchySeparatorAcceptable(int nd_node,
                                          Component &comp,
                                          unsigned filtered_sep_size) {
	// Nothing to gate on.
	if (filtered_sep_size == 0) return true;
	if (nd_node < 0 || !nd_hierarchy_.valid) return true;

	// Ratio gate: |filtered_sep| / |active vars in comp|.
	unsigned n_active = comp.num_variables();
	if (n_active == 0) return true;
	double ratio = (double)filtered_sep_size / (double)n_active;
	if (ratio > config_.separator_max_ratio) {
		if (config_.verbose) {
			std::cout << "  TIER1_REJECT nd=" << nd_node
			          << " reason=ratio sep=" << filtered_sep_size
			          << " n=" << n_active
			          << " ratio=" << ratio
			          << " max=" << config_.separator_max_ratio << std::endl;
		}
		return false;
	}

	// Balance gate: partition active component vars by the child subtree
	// they belong to, using the ND-hierarchy leaf ranges.
	int lc = nd_hierarchy_.left_child[nd_node];
	int rc = nd_hierarchy_.right_child[nd_node];
	if (lc < 0 || rc < 0) return true;  // leaf node has no balance signal

	int L_lo = nd_hierarchy_.leaf_lo[lc];
	int L_hi = nd_hierarchy_.leaf_hi[lc];
	int R_lo = nd_hierarchy_.leaf_lo[rc];
	int R_hi = nd_hierarchy_.leaf_hi[rc];

	int L = 0, R = 0;
	for (auto it = comp.varsBegin(); *it != varsSENTINEL; ++it) {
		if (!isActive(LiteralID(*it, true))) continue;
		if ((size_t)*it >= nd_hierarchy_.var_leaf.size()) continue;
		int leaf = nd_hierarchy_.var_leaf[*it];
		if (leaf < 0) continue;
		if (leaf >= L_lo && leaf <= L_hi) L++;
		else if (leaf >= R_lo && leaf <= R_hi) R++;
	}
	int total = L + R;
	double balance = (total > 0) ? (double)std::min(L, R) / (double)total : 0.0;
	if (balance < config_.separator_min_balance) {
		if (config_.verbose) {
			std::cout << "  TIER1_REJECT nd=" << nd_node
			          << " reason=balance L=" << L << " R=" << R
			          << " balance=" << balance
			          << " min=" << config_.separator_min_balance << std::endl;
		}
		return false;
	}
	return true;
}

// ----------------------------------------------------------------------------
// Phase 3: adaptive branching (Tier 2)
// ----------------------------------------------------------------------------

// Solve τ^(-a) + τ^(-b) = 1 for τ via Newton's method.
// Precondition: a, b > 0. Returns a value in (1, 2].
static double newton_branching_number(double a, double b) {
	// Degenerate safety (callers should ensure a,b > 0).
	if (a <= 0.0 || b <= 0.0) return 2.0;
	// Start with the τ that would satisfy the larger exponent alone:
	// τ^(-max(a,b)) = 1/2 ⇒ τ = 2^(1/max(a,b)). Always > actual root.
	const double m = std::max(a, b);
	double tau = std::pow(2.0, 1.0 / m);
	for (int iter = 0; iter < 25; ++iter) {
		double pa = std::pow(tau, -a);
		double pb = std::pow(tau, -b);
		double f  = pa + pb - 1.0;
		double fp = -(a * pa + b * pb) / tau;
		if (fp == 0.0) break;
		double step = f / fp;
		tau -= step;
		if (tau < 1.0 + 1e-12) tau = 1.0 + 1e-12;
		if (std::abs(step) < 1e-12) break;
	}
	return tau;
}

// Compute Stage 0 cheap scores for every active variable in `comp`.
// Writes `cheap[v]` for each active v and fills `candidates` with the
// unassigned component vars. Cost: O(|comp active vars| + L_comp).
void Solver::stage0_cheap_scores(Component &comp,
                                 double alpha,
                                 std::vector<double> &cheap,
                                 std::vector<VariableIndex> &candidates) {
	candidates.clear();
	for (auto it = comp.varsBegin(); *it != varsSENTINEL; ++it) {
		if (literal_values_[LiteralID(*it, true)] == X_TRI)
			candidates.push_back(*it);
	}
	cheap.assign(num_variables() + 1, 0.0);
	if (candidates.empty()) return;

	const double binary_weight = std::pow(2.0, -2.0 * alpha);
	for (VariableIndex v : candidates) {
		for (int pol = 0; pol < 2; ++pol) {
			LiteralID l(v, pol == 1);
			const auto &blinks = literals_[l].binary_links_;
			for (auto bt = blinks.begin(); *bt != SENTINEL_LIT; ++bt) {
				if (literal_values_[*bt] == X_TRI)
					cheap[v] += binary_weight;
			}
		}
	}

	for (auto ct = comp.clsBegin(); *ct != clsSENTINEL; ++ct) {
		ClauseOfs ofs = comp_manager_.clauseOfsOf(*ct);
		if (isClauseRemoved(ofs) || isSatisfied(ofs)) continue;
		if (ofs >= (ClauseOfs)original_lit_pool_size_
		    && !learnedClauseInScope(ofs)) continue;
		unsigned active_len = 0;
		for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; ++lt) {
			if (literal_values_[*lt] == X_TRI) active_len++;
		}
		if (active_len < 2) continue;
		double w = std::pow(2.0, -alpha * (double)active_len);
		for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; ++lt) {
			if (literal_values_[*lt] == X_TRI)
				cheap[lt->var()] += w;
		}
	}
}

VariableIndex Solver::pickBranchVariableAdaptive(Component &comp, bool &out_unsat) {
	out_unsat = false;

	const double alpha   = config_.stage0_length_decay;
	const double epsilon = config_.epsilon_2clauses;
	const size_t K       = config_.adaptive_top_k;
	const unsigned min_probe_vars = config_.adaptive_probing_min_vars;
	// Clamp bound for Δ_2clauses so |ε·Δ| < 1 (keeps scores > 0).
	const int K_delta = std::max(1,
	                   (int)std::floor(1.0 / std::max(epsilon, 1e-9)) - 1);

	std::vector<VariableIndex> candidates;
	std::vector<double> cheap;

	// Compute Stage 0 scores + active candidate list up front. This lets
	// the threshold check use the ACTIVE variable count (not the
	// component's stored var count, which may be much larger after BCP
	// has assigned many of its variables).
	stage0_cheap_scores(comp, alpha, cheap, candidates);
	if (candidates.empty()) return 0;

	// ------ Fast path: small active component → Stage 0 argmax ------
	//
	// On well-decomposed instances the "no separator" path at hierarchy
	// leaves usually has a tiny active component. K=20 × 2 probes per
	// decision is disproportionate there. Use the argmax of the cheap
	// score instead — same asymptotic cost as the legacy
	// `pickBranchVariable`.
	if (candidates.size() < min_probe_vars) {
		VariableIndex best = candidates[0];
		double best_score = cheap[best];
		for (VariableIndex v : candidates) {
			if (cheap[v] > best_score) {
				best_score = cheap[v];
				best = v;
			}
		}
		return best;
	}

	// ------ Slow path: full adaptive with top-K probing + τ aggregation ------
	struct ScoredCand {
		VariableIndex v;
		int vars_forced_T, vars_forced_F;
		int delta_2c_T,    delta_2c_F;
	};
	std::vector<ScoredCand> scored;

	while (true) {
		stage0_cheap_scores(comp, alpha, cheap, candidates);
		if (candidates.empty()) return 0;

		// Sort candidates by cheap score descending, keep top-K.
		std::sort(candidates.begin(), candidates.end(),
		          [&cheap](VariableIndex a, VariableIndex b) {
		              return cheap[a] > cheap[b];
		          });
		if (candidates.size() > K) candidates.resize(K);

		// ------ Probe each top-K candidate; commit failed literals ------
		scored.clear();
		bool found_failed = false;
		for (VariableIndex v : candidates) {
			if (literal_values_[LiteralID(v, true)] != X_TRI) continue;

			ProbeResult prT = probeLiteral(LiteralID(v, true));
			ProbeResult prF = probeLiteral(LiteralID(v, false));

			if (!prT.success && !prF.success) {
				out_unsat = true;
				return 0;
			}
			if (!prT.success) {
				if (!commitFailedLiteral()) { out_unsat = true; return 0; }
				found_failed = true;
				break;
			}
			if (!prF.success) {
				if (!commitFailedLiteral()) { out_unsat = true; return 0; }
				found_failed = true;
				break;
			}

			scored.push_back({v,
			                   prT.vars_forced, prF.vars_forced,
			                   prT.delta_2clauses, prF.delta_2clauses});
		}

		if (!found_failed) break;  // outer loop settled — proceed to τ selection
	}

	// ------ Pick argmin τ ------
	if (scored.empty()) return 0;  // defensive

	auto clamp_delta = [K_delta](int d) {
		return std::max(-K_delta, std::min(K_delta, d));
	};

	double best_tau = std::numeric_limits<double>::infinity();
	VariableIndex best_v = 0;
	for (const auto &c : scored) {
		double a = (double)c.vars_forced_T + epsilon * (double)clamp_delta(c.delta_2c_T);
		double b = (double)c.vars_forced_F + epsilon * (double)clamp_delta(c.delta_2c_F);
		double tau = newton_branching_number(a, b);
		if (tau < best_tau) {
			best_tau = tau;
			best_v = c.v;
		}
	}
	if (config_.verbose) {
		std::cout << "  TIER2_PICK v=" << best_v
		          << " tau=" << best_tau
		          << " scored=" << scored.size() << std::endl;
	}
	return best_v;
}

void Solver::buildMetisInputFromComponent(
    Component &comp,
    std::vector<unsigned> &active_vars,
    std::vector<std::pair<unsigned, std::vector<unsigned>>> &long_clauses,
    std::vector<std::pair<unsigned, unsigned>> &binary_pairs)
{
	active_vars.clear();
	long_clauses.clear();
	binary_pairs.clear();

	// Active variables of the component.
	std::unordered_set<unsigned> in_comp;
	for (auto it = comp.varsBegin(); *it != varsSENTINEL; ++it) {
		if (literal_values_[LiteralID(*it, true)] == X_TRI) {
			active_vars.push_back(*it);
			in_comp.insert(*it);
		}
	}
	if (active_vars.size() < 4) return;

	// Active long (non-binary) clauses with their live literals' vars.
	for (auto ct = comp.clsBegin(); *ct != clsSENTINEL; ++ct) {
		ClauseOfs ofs = comp_manager_.clauseOfsOf(*ct);
		if (isClauseRemoved(ofs) || isSatisfied(ofs)) continue;
		if (ofs >= (ClauseOfs)original_lit_pool_size_
		    && !learnedClauseInScope(ofs)) continue;
		std::vector<unsigned> vars;
		for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; ++lt) {
			if (literal_values_[*lt] == X_TRI) vars.push_back(lt->var());
		}
		if (vars.size() >= 3) {
			long_clauses.push_back({(unsigned)ofs, std::move(vars)});
		}
		// Size-2 entries (after BCP trimmed a length-3+ clause to 2)
		// are pseudo-binaries; we could add them as binary_pairs, but
		// the inclusion below via binary_links_ won't catch them
		// because they are not stored there. Skipping them is
		// conservative — they will still be part of BCP, just not part
		// of the METIS connectivity. Acceptable for the one-shot
		// runtime separator.
	}

	// Active binary clauses where both endpoints are in the component.
	for (VariableIndex v : active_vars) {
		for (int pol = 0; pol < 2; ++pol) {
			LiteralID l(v, pol == 1);
			const auto &blinks = literals_[l].binary_links_;
			for (auto bt = blinks.begin(); *bt != SENTINEL_LIT; ++bt) {
				if (literal_values_[*bt] != X_TRI) continue;
				if (l.raw() >= bt->raw()) continue;  // dedup
				if (in_comp.count(bt->var())) {
					binary_pairs.push_back({(unsigned)v, (unsigned)bt->var()});
				}
			}
		}
	}
}

bool Solver::initForTesting(const std::string &file_name) {
	createfromFile(file_name);
	initStack(num_variables());

	// Detect immediately conflicting unit clauses (x AND ¬x).
	for (auto lit : unit_clauses_) {
		if (literal_values_[lit] == F_TRI) return false;
	}
	for (auto lit : unit_clauses_) {
		setLiteralIfFree(lit);
	}
	return BCP(0);
}

///////////////////////////////////////////////////////////////////////////////////////////////
// BEGIN module conflictAnalyzer
///////////////////////////////////////////////////////////////////////////////////////////////

void Solver::minimizeAndStoreUIPClause(LiteralID uipLit,
		vector<LiteralID> & tmp_clause, bool seen[]) {
	static deque<LiteralID> clause;
	clause.clear();
	assertion_level_ = 0;
	for (auto lit : tmp_clause) {
		if (existsUnitClauseOf(lit.var()))
			continue;
		bool resolve_out = false;
		if (hasAntecedent(lit)) {
			resolve_out = true;
			if (getAntecedent(lit).isAClause()) {
				for (auto it = beginOf(getAntecedent(lit).asCl()) + 1;
						*it != SENTINEL_CL; it++)
					if (!seen[it->var()]) {
						resolve_out = false;
						break;
					}
			} else if (!seen[getAntecedent(lit).asLit().var()]) {
				resolve_out = false;
			}
		}

		if (!resolve_out) {
			// uipLit should be the sole literal of this Decision Level
			if (var(lit).decision_level >= assertion_level_) {
				assertion_level_ = var(lit).decision_level;
				clause.push_front(lit);
			} else
				clause.push_back(lit);
		}
	}

	if(uipLit.var())
	 assert(var(uipLit).decision_level == stack_.get_decision_level());

	//assert(uipLit.var() != 0);
	if (uipLit.var() != 0)
		clause.push_front(uipLit);
	uip_clauses_.push_back(vector<LiteralID>(clause.begin(), clause.end()));
}

void Solver::recordLastUIPCauses() {
// note:
// variables of lower dl: if seen we dont work with them anymore
// variables of this dl: if seen we incorporate their
// antecedent and set to unseen
	bool seen[num_variables() + 1];
	memset(seen, false, sizeof(bool) * (num_variables() + 1));

	static vector<LiteralID> tmp_clause;
	tmp_clause.clear();

	assertion_level_ = 0;
	uip_clauses_.clear();

	unsigned lit_stack_ofs = literal_stack_.size();
	int DL = stack_.get_decision_level();
	unsigned lits_at_current_dl = 0;

	for (auto l : violated_clause) {
		if (var(l).decision_level == 0 || existsUnitClauseOf(l.var()))
			continue;
		if (var(l).decision_level < DL)
			tmp_clause.push_back(l);
		else
			lits_at_current_dl++;
		literal(l).increaseActivity();
		seen[l.var()] = true;
	}

	LiteralID curr_lit;
	while (lits_at_current_dl) {
		assert(lit_stack_ofs != 0);
		curr_lit = literal_stack_[--lit_stack_ofs];

		if (!seen[curr_lit.var()])
			continue;

		seen[curr_lit.var()] = false;

		if (lits_at_current_dl-- == 1) {
			// perform UIP stuff
			if (!hasAntecedent(curr_lit)) {
				// this should be the decision literal when in first branch
				// or it is a literal decided to explore in failed literal testing
				break;
			}
		}

		assert(hasAntecedent(curr_lit));

		//cout << "{" << curr_lit.toInt() << "}";
		if (getAntecedent(curr_lit).isAClause()) {
			updateActivities(getAntecedent(curr_lit).asCl());
			assert(curr_lit == *beginOf(getAntecedent(curr_lit).asCl()));

			for (auto it = beginOf(getAntecedent(curr_lit).asCl()) + 1;
					*it != SENTINEL_CL; it++) {
				if (seen[it->var()] || (var(*it).decision_level == 0)
						|| existsUnitClauseOf(it->var()))
					continue;
				if (var(*it).decision_level < DL)
					tmp_clause.push_back(*it);
				else
					lits_at_current_dl++;
				seen[it->var()] = true;
			}
		} else {
			LiteralID alit = getAntecedent(curr_lit).asLit();
			literal(alit).increaseActivity();
			literal(curr_lit).increaseActivity();
			if (!seen[alit.var()] && !(var(alit).decision_level == 0)
					&& !existsUnitClauseOf(alit.var())) {
				if (var(alit).decision_level < DL)
					tmp_clause.push_back(alit);
				else
					lits_at_current_dl++;
				seen[alit.var()] = true;
			}
		}
		curr_lit = NOT_A_LIT;
	}

	minimizeAndStoreUIPClause(curr_lit.neg(), tmp_clause, seen);
}

void Solver::recordAllUIPCauses() {
// note:
// variables of lower dl: if seen we dont work with them anymore
// variables of this dl: if seen we incorporate their
// antecedent and set to unseen
	bool seen[num_variables() + 1];
	memset(seen, false, sizeof(bool) * (num_variables() + 1));

	static vector<LiteralID> tmp_clause;
	tmp_clause.clear();

	assertion_level_ = 0;
	uip_clauses_.clear();

	unsigned lit_stack_ofs = literal_stack_.size();
	int DL = stack_.get_decision_level();
	unsigned lits_at_current_dl = 0;

	for (auto l : violated_clause) {
		if (var(l).decision_level == 0 || existsUnitClauseOf(l.var()))
			continue;
		if (var(l).decision_level < DL)
			tmp_clause.push_back(l);
		else
			lits_at_current_dl++;
		literal(l).increaseActivity();
		seen[l.var()] = true;
	}
	unsigned n = 0;
	LiteralID curr_lit;
	while (lits_at_current_dl) {
		assert(lit_stack_ofs != 0);
		curr_lit = literal_stack_[--lit_stack_ofs];

		if (!seen[curr_lit.var()])
			continue;

		seen[curr_lit.var()] = false;

		if (lits_at_current_dl-- == 1) {
			n++;
			if (!hasAntecedent(curr_lit)) {
				// this should be the decision literal when in first branch
				// or it is a literal decided to explore in failed literal testing
				//assert(stack_.TOS_decLit() == curr_lit);
				break;
			}
			// perform UIP stuff
			minimizeAndStoreUIPClause(curr_lit.neg(), tmp_clause, seen);
		}

		assert(hasAntecedent(curr_lit));

		if (getAntecedent(curr_lit).isAClause()) {
			updateActivities(getAntecedent(curr_lit).asCl());
			assert(curr_lit == *beginOf(getAntecedent(curr_lit).asCl()));

			for (auto it = beginOf(getAntecedent(curr_lit).asCl()) + 1;
					*it != SENTINEL_CL; it++) {
				if (seen[it->var()] || (var(*it).decision_level == 0)
						|| existsUnitClauseOf(it->var()))
					continue;
				if (var(*it).decision_level < DL)
					tmp_clause.push_back(*it);
				else
					lits_at_current_dl++;
				seen[it->var()] = true;
			}
		} else {
			LiteralID alit = getAntecedent(curr_lit).asLit();
			literal(alit).increaseActivity();
			literal(curr_lit).increaseActivity();
			if (!seen[alit.var()] && !(var(alit).decision_level == 0)
					&& !existsUnitClauseOf(alit.var())) {
				if (var(alit).decision_level < DL)
					tmp_clause.push_back(alit);
				else
					lits_at_current_dl++;
				seen[alit.var()] = true;
			}
		}
	}
	if (!hasAntecedent(curr_lit)) {
		minimizeAndStoreUIPClause(curr_lit.neg(), tmp_clause, seen);
	}
//	if (var(curr_lit).decision_level > assertion_level_)
//		assertion_level_ = var(curr_lit).decision_level;
}

