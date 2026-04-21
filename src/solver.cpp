/*
 * solver.cpp
 *
 *  Created on: Aug 23, 2012
 *      Author: marc
 */
#include "solver.h"
#include <deque>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <random>
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

		// Preprocessing completeness guards. Two levels:
		//   1. Unit-propagation saturation: every clause has ≥ 2 active
		//      literals (cheap; catches missed units).
		//   2. Failed-literal saturation: no variable has a polarity
		//      whose BCP derives conflict (expensive but one-off here).
		verifyUnitPropagationSaturated("post-simplePreProcess");
		// Temporarily disabled while debugging: this call mutates clause
		// watch structure as a side effect of BCP during probing, which
		// appears to perturb search results on t1_011.
		// verifyNoFailedLiterals("post-simplePreProcess");

		// Diagnostic: dump the preprocessed formula as DIMACS and exit.
		// Used to isolate preprocessing bugs by feeding the dump to an
		// independent model counter (ganak) and comparing counts. Done
		// BEFORE allocating the guard variable so the dump reflects only
		// what preprocessing produced.
		if (!config_.dump_preprocessed_path.empty()) {
			std::ofstream out(config_.dump_preprocessed_path);
			if (!out) {
				cerr << "failed to open dump path: "
				     << config_.dump_preprocessed_path << endl;
				return;
			}
			// Count surviving variables and clauses.
			unsigned n_vars = num_variables();
			unsigned n_cls = 0;
			// Long clauses (stored in literal_pool_ up to original_lit_pool_size_).
			for (auto it = literal_pool_.begin();
			     it != literal_pool_.end(); it++) {
				if (*it == SENTINEL_LIT) {
					if (it + 1 == literal_pool_.end()) break;
					it += ClauseHeader::overheadInLits();
					ClauseOfs ofs = (ClauseOfs)(it + 1 - literal_pool_.begin());
					if (ofs >= (ClauseOfs)original_lit_pool_size_) break;
					// Count effective length (skip falsified lits).
					unsigned len = 0;
					bool sat = false;
					for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++) {
						if (literal_values_[*lt] == T_TRI) { sat = true; break; }
						if (literal_values_[*lt] == X_TRI) len++;
					}
					if (!sat && len >= 1) n_cls++;
					// Advance past the clause literals.
					auto end_it = beginOf(ofs);
					while (*end_it != SENTINEL_LIT) end_it++;
					it = end_it - 1;  // loop ++ moves to SENTINEL next
				}
			}
			// Binary clauses (from binary_links_). Deduplicate by
			// emitting only when (lit, other) has lit.raw() < other.raw().
			for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
				if (literal_values_[l] == T_TRI) continue;  // lit already
				                                            // true, binaries
				                                            // with this lit
				                                            // are satisfied
				const auto &bl = literal(l).binary_links_;
				for (auto bt = bl.begin(); *bt != SENTINEL_LIT; ++bt) {
					if (!(l.raw() < bt->raw())) continue;
					if (literal_values_[*bt] == T_TRI) continue;
					if (literal_values_[l] == F_TRI
					    && literal_values_[*bt] == F_TRI) continue;  // empty
					n_cls++;
				}
			}
			// Unit clauses (assigned literals at DL 0).
			for (auto lit : literal_stack_) {
				if (var(lit).decision_level == 0) n_cls++;
			}
			out << "c post-preprocessing dump from sharpsat-separator\n";
			out << "p cnf " << n_vars << " " << n_cls << "\n";
			// Emit long clauses.
			for (auto it = literal_pool_.begin();
			     it != literal_pool_.end(); it++) {
				if (*it == SENTINEL_LIT) {
					if (it + 1 == literal_pool_.end()) break;
					it += ClauseHeader::overheadInLits();
					ClauseOfs ofs = (ClauseOfs)(it + 1 - literal_pool_.begin());
					if (ofs >= (ClauseOfs)original_lit_pool_size_) break;
					bool sat = false;
					std::vector<int> lits;
					for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++) {
						if (literal_values_[*lt] == T_TRI) { sat = true; break; }
						if (literal_values_[*lt] == X_TRI) {
							int val = (int)lt->var();
							if (!lt->sign()) val = -val;
							lits.push_back(val);
						}
					}
					auto end_it = beginOf(ofs);
					while (*end_it != SENTINEL_LIT) end_it++;
					if (!sat && !lits.empty()) {
						for (int v : lits) out << v << " ";
						out << "0\n";
					}
					it = end_it - 1;
				}
			}
			// Emit binaries.
			for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
				if (literal_values_[l] == T_TRI) continue;
				const auto &bl = literal(l).binary_links_;
				for (auto bt = bl.begin(); *bt != SENTINEL_LIT; ++bt) {
					if (!(l.raw() < bt->raw())) continue;
					if (literal_values_[*bt] == T_TRI) continue;
					bool l_false  = (literal_values_[l]  == F_TRI);
					bool bt_false = (literal_values_[*bt] == F_TRI);
					if (l_false && bt_false) continue;  // empty clause
					if (!l_false) {
						int v = (int)l.var();
						if (!l.sign()) v = -v;
						out << v << " ";
					}
					if (!bt_false) {
						int v = (int)bt->var();
						if (!bt->sign()) v = -v;
						out << v << " ";
					}
					out << "0\n";
				}
			}
			// Emit units (literals assigned at DL 0 that were originally units).
			for (auto lit : literal_stack_) {
				if (var(lit).decision_level == 0) {
					int v = (int)lit.var();
					if (!lit.sign()) v = -v;
					out << v << " 0\n";
				}
			}
			out.close();
			cerr << "dumped preprocessed formula to "
			     << config_.dump_preprocessed_path
			     << " (" << n_vars << " vars, " << n_cls << " clauses)\n";
			return;
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

	if (config_.analyze_clause_pool) {
		analyzeOriginalClausePool();
		analyzeLearnedClausePool();
	}

	if (config_.analyze_dynamic_subsumption) {
		cout << "\n=== Dynamic-subsumption measurement ===" << endl;
		cout << "  branches sampled     : " << statistics_.dyn_sub_branches_sampled_ << endl;
		cout << "  affected clauses     : " << statistics_.dyn_sub_affected_clauses_ << endl;
		cout << "  shortened clauses    : " << statistics_.dyn_sub_shortened_clauses_ << endl;
		cout << "  subsumption events   : " << statistics_.dyn_sub_events_ << endl;
		cout << "    of which → binary : " << statistics_.dyn_sub_events_to_binary_ << endl;
		cout << "  SSR events           : " << statistics_.dyn_ssr_events_ << endl;
		cout << "    of which → binary : " << statistics_.dyn_ssr_events_to_binary_ << endl;
		if (statistics_.dyn_sub_branches_sampled_ > 0) {
			cout << "  sub events / branch  : "
			     << (double)statistics_.dyn_sub_events_ /
			        (double)statistics_.dyn_sub_branches_sampled_ << endl;
			cout << "  SSR events / branch  : "
			     << (double)statistics_.dyn_ssr_events_ /
			        (double)statistics_.dyn_sub_branches_sampled_ << endl;
		}
	}

	if (config_.perform_implicant_learning) {
		cout << "\n=== Implicant learning summary ===" << endl;
		cout << "  learned            : " << statistics_.num_implicants_learned_ << endl;
		cout << "  dropped (size cap) : " << statistics_.num_implicants_size_dropped_ << endl;
		cout << "  dropped (depth<min): " << statistics_.num_implicants_depth_dropped_ << endl;
		cout << "  dropped (trivial)  : " << statistics_.num_implicants_trivial_dropped_ << endl;
		cout << "  dropped (dedup)    : " << statistics_.num_implicants_dedup_dropped_ << endl;
		cout << "  quota reached      : " << (statistics_.num_implicants_quota_stop_ ? "yes" : "no") << endl;
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
	// Dedup: skip storing if we've likely seen this clause before.
	// Bloom filter — false positives just skip a learn (sound).
	Antecedent ante(NOT_A_CLAUSE);
	if (uip.size() >= 2 && !maybeDedupClause(uip)) {
		statistics_.num_learned_dedup_dropped_++;
		// Duplicate — don't store, but still force the asserting literal
		// with an unscoped NOT_A_CLAUSE antecedent. Correct: under the
		// current state the literal IS entailed (same reasoning as for
		// a freshly-learned clause); we just don't persist it.
	} else {
		ante = addScopedUIPConflictClause(uip);
	}
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

// Read-only analysis: how much duplication / subsumption is in the
// current learned-clause pool? Doesn't modify anything — just reports.
// O(N^2) worst case in the pool size; practical via length-bucketing +
// a 64-bit signature pre-filter. Intended for end-of-solve diagnostic
// before committing to a live subsumption pass.
void Solver::analyzeLearnedClausePool() {
	struct Rec {
		ClauseOfs ofs;
		std::vector<uint32_t> lits;  // sorted ascending
		uint64_t sig;                // bloom-style 64-bit hash of literals
		const std::set<ClauseOfs> *scope;  // nullptr = scope-empty
	};
	auto hash_lit = [](uint32_t r) {
		r ^= r >> 16;
		r *= 0x7feb352dU;
		r ^= r >> 15;
		return r;
	};
	auto hash_set = [](const std::vector<uint32_t> &v) {
		uint64_t h = 0xcbf29ce484222325ULL;
		for (uint32_t r : v) { h ^= r; h *= 0x100000001b3ULL; }
		return h;
	};

	std::vector<Rec> recs;
	recs.reserve(conflict_clauses_.size());
	std::map<unsigned, unsigned> len_hist;
	for (ClauseOfs cl_ofs : conflict_clauses_) {
		Rec r;
		r.ofs = cl_ofs;
		r.sig = 0;
		for (auto lt = beginOf(cl_ofs); *lt != SENTINEL_LIT; lt++) {
			r.lits.push_back(lt->raw());
			r.sig |= (1ULL << (hash_lit(lt->raw()) & 63));
		}
		std::sort(r.lits.begin(), r.lits.end());
		auto it = learned_clause_scope_.find(cl_ofs);
		r.scope = (it != learned_clause_scope_.end()) ? &it->second : nullptr;
		len_hist[r.lits.size()]++;
		recs.push_back(std::move(r));
	}

	std::cerr << "\n=== Clause pool analysis ===\n";
	std::cerr << "  total learned clauses   : " << recs.size() << "\n";
	std::cerr << "  length distribution     :";
	for (auto &p : len_hist)
		std::cerr << " " << p.first << ":" << p.second;
	std::cerr << "\n";

	// Exact duplicates (identical literal sets, ignoring scope).
	std::unordered_map<uint64_t, std::vector<size_t>> by_hash;
	for (size_t i = 0; i < recs.size(); i++)
		by_hash[hash_set(recs[i].lits)].push_back(i);
	unsigned dup_clauses = 0;                  // # clauses in a duplicate group
	unsigned dup_clauses_same_scope = 0;        // stronger condition
	for (auto &p : by_hash) {
		if (p.second.size() <= 1) continue;
		auto &g = p.second;
		// Verify literal sets really match (not just hash).
		std::sort(g.begin(), g.end(), [&](size_t a, size_t b) {
			return recs[a].lits < recs[b].lits;
		});
		for (size_t i = 0; i < g.size();) {
			size_t j = i + 1;
			while (j < g.size() && recs[g[j]].lits == recs[g[i]].lits) j++;
			unsigned blk = j - i;
			if (blk > 1) {
				dup_clauses += blk;
				// Within this literal-set block, count same-scope pairs.
				// Group by scope: nullptr or pointer equality on the
				// &set<ClauseOfs> — but two distinct scope entries can
				// still be equal sets. Do content comparison.
				// For the upper-bound reporting, count duplicates where
				// scope pointers or contents match:
				for (size_t a = i; a < j; a++) {
					for (size_t b = a + 1; b < j; b++) {
						bool same_scope =
							(recs[g[a]].scope == nullptr && recs[g[b]].scope == nullptr) ||
							(recs[g[a]].scope && recs[g[b]].scope &&
							 *recs[g[a]].scope == *recs[g[b]].scope);
						if (same_scope) dup_clauses_same_scope += 2;  // count both
					}
				}
			}
			i = j;
		}
	}
	std::cerr << "  duplicate literal sets  : " << dup_clauses
	          << " (" << (recs.empty() ? 0.0 :
	                      100.0 * (double)dup_clauses / (double)recs.size())
	          << "% of pool)\n";
	std::cerr << "    of which same scope  : " << dup_clauses_same_scope << "\n";

	// Subsumption: for each clause C, count clauses D (with |D|>|C|)
	// subsumed by C. Report how many clauses are subsumed by at least
	// one shorter clause (scope-ignoring upper bound).
	std::map<unsigned, std::vector<size_t>> by_len;
	for (size_t i = 0; i < recs.size(); i++)
		by_len[recs[i].lits.size()].push_back(i);
	std::unordered_set<size_t> subsumed;           // scope-ignoring
	std::unordered_set<size_t> subsumed_same_scope;
	for (auto &pc : by_len) {
		unsigned k_c = pc.first;
		for (auto &pd : by_len) {
			unsigned k_d = pd.first;
			if (k_d <= k_c) continue;
			for (size_t ci : pc.second) {
				const auto &a = recs[ci].lits;
				uint64_t sig_c = recs[ci].sig;
				for (size_t di : pd.second) {
					if ((sig_c & recs[di].sig) != sig_c) continue;
					const auto &b = recs[di].lits;
					size_t ia = 0, ib = 0;
					bool is_sub = true;
					while (ia < a.size()) {
						while (ib < b.size() && b[ib] < a[ia]) ib++;
						if (ib >= b.size() || b[ib] != a[ia]) { is_sub = false; break; }
						ia++; ib++;
					}
					if (is_sub) {
						subsumed.insert(di);
						bool scope_ok =
							(recs[ci].scope == nullptr) ||  // C is scope-empty: always valid
							(recs[di].scope && recs[ci].scope &&
							 std::includes(recs[di].scope->begin(), recs[di].scope->end(),
							               recs[ci].scope->begin(), recs[ci].scope->end()));
						if (scope_ok) subsumed_same_scope.insert(di);
					}
				}
			}
		}
	}
	std::cerr << "  subsumed by shorter     : " << subsumed.size()
	          << " (" << (recs.empty() ? 0.0 :
	                      100.0 * (double)subsumed.size() / (double)recs.size())
	          << "% of pool, scope-ignoring)\n";
	std::cerr << "    scope-safe subset     : " << subsumed_same_scope.size()
	          << " (" << (recs.empty() ? 0.0 :
	                      100.0 * (double)subsumed_same_scope.size() / (double)recs.size())
	          << "% of pool)\n";
}

// Invariant guard: verify the formula is unit-propagation saturated.
// See declaration in solver.h for the spec.
//
// Scans every long clause in literal_pool_ (both original and learned)
// and every binary clause in binary_links_. For each clause C:
//   - If C has any T_TRI literal: satisfied, OK.
//   - If C is removed: skip.
//   - Else count X_TRI literals. If exactly 1, this literal is a
//     forced unit that BCP missed. Fire.
//
// Design as a reusable guard: takes a context label so we can tell
// where in the pipeline the violation was detected. Zero allocation;
// O(L) where L = total literal occurrences.
void Solver::verifyUnitPropagationSaturated(const char *label) {
	unsigned n_violations = 0;
	LiteralID violating_free_lit;
	ClauseOfs violating_clause = NOT_A_CLAUSE;
	bool violating_is_binary = false;

	// Long clauses.
	for (auto it_lit = literal_pool_.begin();
	     it_lit != literal_pool_.end(); it_lit++) {
		if (*it_lit != SENTINEL_LIT) continue;
		if (it_lit + 1 == literal_pool_.end()) break;
		it_lit += ClauseHeader::overheadInLits();
		ClauseOfs ofs = (ClauseOfs)(it_lit + 1 - literal_pool_.begin());
		if (isClauseRemoved(ofs)) {
			// Advance past this clause's literals to the next SENTINEL.
			auto e = beginOf(ofs);
			while (*e != SENTINEL_LIT) e++;
			it_lit = e - 1;
			continue;
		}
		unsigned active = 0;
		bool satisfied = false;
		LiteralID lone_active;
		for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++) {
			TriValue v = literal_values_[*lt];
			if (v == T_TRI) { satisfied = true; break; }
			if (v == X_TRI) { active++; lone_active = *lt; }
		}
		// Advance past literals.
		auto e = beginOf(ofs);
		while (*e != SENTINEL_LIT) e++;
		it_lit = e - 1;
		if (satisfied) continue;
		if (active == 1) {
			if (n_violations == 0) {
				violating_free_lit = lone_active;
				violating_clause = ofs;
			}
			n_violations++;
		}
	}

	// Binary clauses (via binary_links_). Each binary is represented
	// twice (once per endpoint); dedup by l.raw() < bt->raw().
	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		// If l itself is true or false, every binary with l is
		// satisfied (if l true) or reduces to the other literal (if
		// l false).
		TriValue vl = literal_values_[l];
		const auto &bl = literal(l).binary_links_;
		for (auto bt = bl.begin(); *bt != SENTINEL_LIT; ++bt) {
			if (!(l.raw() < bt->raw())) continue;
			TriValue vb = literal_values_[*bt];
			if (vl == T_TRI || vb == T_TRI) continue;  // satisfied
			if (vl == F_TRI && vb == F_TRI) continue;  // empty — a
			                                            // different
			                                            // violation
			                                            // caught elsewhere
			unsigned active = 0;
			LiteralID lone;
			if (vl == X_TRI) { active++; lone = l; }
			if (vb == X_TRI) { active++; lone = *bt; }
			if (active == 1) {
				if (n_violations == 0) {
					violating_free_lit = lone;
					violating_clause = NOT_A_CLAUSE;
					violating_is_binary = true;
				}
				n_violations++;
			}
		}
	}

	if (n_violations > 0) {
		std::cerr << "\n*** UNIT_PROP_NOT_SATURATED (" << label << ") ***\n"
		          << "  " << n_violations << " clauses have exactly one"
		          << " active literal with all others falsified.\n"
		          << "  First violation: "
		          << (violating_is_binary ? "binary clause" : "long clause at ofs=")
		          << (violating_is_binary ? "" : std::to_string(violating_clause))
		          << " free_lit=" << violating_free_lit.toInt() << "\n"
		          << "  This literal should have been unit-propagated.\n";
		std::cerr.flush();
		std::abort();
	}
}

// Test-support: permute stored-literal order within each original
// clause. Preserves clause boundaries (SENTINEL_LIT) and watch-list
// references (watches are keyed on ClauseOfs, not literal position).
// Intended to run once BEFORE search starts, to test whether downstream
// code is invariant under stored-literal order.
void Solver::_permuteClauseLiteralsForTest(unsigned seed) {
	std::mt19937 rng(seed);
	auto it = literal_pool_.begin();
	while (it != literal_pool_.end()) {
		if (*it != SENTINEL_LIT) { it++; continue; }
		if (it + 1 == literal_pool_.end()) break;
		it += ClauseHeader::overheadInLits();
		ClauseOfs ofs = (ClauseOfs)(it + 1 - literal_pool_.begin());
		if (ofs >= (ClauseOfs)original_lit_pool_size_) break;
		auto start = literal_pool_.begin() + ofs;
		auto end = start;
		while (*end != SENTINEL_LIT) end++;
		std::shuffle(start, end, rng);
		it = end;
	}
}

// Test-support: minimal preparation sequence (parse + preprocess +
// component-manager init) sufficient for canonical-key computation.
// Does NOT allocate the guard variable and does NOT start the search.
void Solver::_prepareForKeyComputation(const std::string &file_name) {
	createfromFile(file_name);
	initStack(num_variables());
	simplePreProcess();
	comp_manager_.initialize(literals_, literal_pool_);
	comp_manager_.setRemovedClauses(&removed_clauses_);
}

// Invariant guard: verify there are no failed literals. See declaration
// in solver.h for the spec. Uses probeLiteralPassFail which performs
// a clean save-assign-BCP-rollback cycle.
void Solver::verifyNoFailedLiterals(const char *label) {
	unsigned n_failed = 0;
	LiteralID first_failed;
	for (unsigned v = 1; v < variables_.size(); v++) {
		if (literal_values_[LiteralID(v, true)] != X_TRI) continue;  // already assigned
		// Try v=true.
		if (!probeLiteralPassFail(LiteralID(v, true))) {
			if (n_failed == 0) first_failed = LiteralID(v, true);
			n_failed++;
			continue;  // don't try v=false too; one is enough
		}
		// Try v=false.
		if (!probeLiteralPassFail(LiteralID(v, false))) {
			if (n_failed == 0) first_failed = LiteralID(v, false);
			n_failed++;
		}
	}
	if (n_failed > 0) {
		std::cerr << "\n*** FAILED_LITERAL_NOT_SATURATED (" << label << ") ***\n"
		          << "  " << n_failed << " variables have a polarity whose"
		          << " BCP derives conflict; the opposite literal is entailed"
		          << " and should have been forced during preprocessing.\n"
		          << "  First failed literal: " << first_failed.toInt() << "\n";
		std::cerr.flush();
		std::abort();
	}
}

// Scan the ORIGINAL formula's clauses (everything in literal_pool_ below
// original_lit_pool_size_) for subsumption + SSR opportunities. Read-only
// — does NOT modify any state. Intended to answer: would writing a
// preprocessing pass be worthwhile? Reports:
//   - total original clauses and length distribution
//   - duplicates (identical literal sets)
//   - subsumption pairs (C ⊆ D as literal sets; D is subsumable)
//   - SSR opportunities (pairs where SSR would shorten a clause)
//   - among SSR: how many would produce a binary clause (high-value)
void Solver::analyzeOriginalClausePool() {
	struct Rec {
		ClauseOfs ofs;
		std::vector<uint32_t> lits;  // sorted ascending
		uint64_t sig;                // bloom-style 64-bit hash of literals
	};
	auto hash_lit = [](uint32_t r) {
		r ^= r >> 16;
		r *= 0x7feb352dU;
		r ^= r >> 15;
		return r;
	};
	auto hash_set = [](const std::vector<uint32_t> &v) {
		uint64_t h = 0xcbf29ce484222325ULL;
		for (uint32_t r : v) { h ^= r; h *= 0x100000001b3ULL; }
		return h;
	};

	// Walk literal_pool_ forward, collecting original-clause offsets.
	// Clause layout: [SENTINEL] [header x overheadInLits] [lit_1 .. lit_k]
	// [SENTINEL] [header x overheadInLits] [lit_1 .. lit_k] [SENTINEL] ...
	// Following the same iteration pattern as compactVariables().
	std::vector<Rec> recs;
	std::map<unsigned, unsigned> len_hist;
	for (auto it_lit = literal_pool_.begin();
	     it_lit != literal_pool_.end(); it_lit++) {
		if (*it_lit == SENTINEL_LIT) {
			if (it_lit + 1 == literal_pool_.end()) break;
			it_lit += ClauseHeader::overheadInLits();
			ClauseOfs ofs = (ClauseOfs)(it_lit + 1 - literal_pool_.begin());
			// Only original clauses — stop once we cross into learned territory.
			if (ofs >= (ClauseOfs)original_lit_pool_size_) break;
			Rec r;
			r.ofs = ofs;
			r.sig = 0;
			for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++) {
				r.lits.push_back(lt->raw());
				r.sig |= (1ULL << (hash_lit(lt->raw()) & 63));
			}
			std::sort(r.lits.begin(), r.lits.end());
			// Capture length, do the iterator advance, THEN move. This
			// ordering matters: std::move(r) leaves r.lits empty, so any
			// subsequent r.lits.size() would return 0 and the advance
			// `(size_t)0 - 1` would underflow to SIZE_MAX (this bug
			// actually surfaced during initial implementation, producing
			// phantom length-0 records for every real clause). By doing
			// the advance before the move, we cannot re-introduce the bug.
			const size_t clause_len = r.lits.size();
			len_hist[clause_len]++;
			// Guard: the iterator must never advance backward. A backward
			// advance means we've hit the use-after-move bug or some
			// other corruption of clause_len. Runtime check (not debug
			// assert) because a silent backward walk produces corrupt
			// records that then feed into the subsumption/SSR analysis.
			if (clause_len == 0) {
				std::cerr << "*** ANALYZER_INVARIANT: clause_len == 0 at ofs="
				          << ofs << " — pool iteration bug\n";
				std::cerr.flush();
				std::abort();
			}
			const ptrdiff_t advance = (ptrdiff_t)clause_len - 1;
			if (advance < 0) {
				std::cerr << "*** ANALYZER_INVARIANT: negative advance (="
				          << advance << ") at ofs=" << ofs << "\n";
				std::cerr.flush();
				std::abort();
			}
			it_lit += advance;
			recs.push_back(std::move(r));
		}
	}

	std::cerr << "\n=== Original-formula pool analysis ===\n";
	std::cerr << "  total original clauses  : " << recs.size() << "\n";
	std::cerr << "  length distribution     :";
	for (auto &p : len_hist)
		std::cerr << " " << p.first << ":" << p.second;
	std::cerr << "\n";

	// Duplicates.
	std::unordered_map<uint64_t, std::vector<size_t>> by_hash;
	for (size_t i = 0; i < recs.size(); i++)
		by_hash[hash_set(recs[i].lits)].push_back(i);
	unsigned dup_clauses = 0;
	for (auto &p : by_hash) {
		if (p.second.size() <= 1) continue;
		auto &g = p.second;
		std::sort(g.begin(), g.end(), [&](size_t a, size_t b) {
			return recs[a].lits < recs[b].lits;
		});
		for (size_t i = 0; i < g.size();) {
			size_t j = i + 1;
			while (j < g.size() && recs[g[j]].lits == recs[g[i]].lits) j++;
			if (j - i > 1) dup_clauses += (j - i);
			i = j;
		}
	}
	std::cerr << "  duplicate literal sets  : " << dup_clauses
	          << " (" << (recs.empty() ? 0.0 :
	                      100.0 * (double)dup_clauses / (double)recs.size())
	          << "% of pool)\n";

	// Subsumption by shorter.
	std::map<unsigned, std::vector<size_t>> by_len;
	for (size_t i = 0; i < recs.size(); i++)
		by_len[recs[i].lits.size()].push_back(i);
	std::unordered_set<size_t> subsumed;
	for (auto &pc : by_len) {
		unsigned k_c = pc.first;
		for (auto &pd : by_len) {
			unsigned k_d = pd.first;
			if (k_d <= k_c) continue;
			for (size_t ci : pc.second) {
				const auto &a = recs[ci].lits;
				uint64_t sig_c = recs[ci].sig;
				for (size_t di : pd.second) {
					if ((sig_c & recs[di].sig) != sig_c) continue;
					const auto &b = recs[di].lits;
					size_t ia = 0, ib = 0;
					bool is_sub = true;
					while (ia < a.size()) {
						while (ib < b.size() && b[ib] < a[ia]) ib++;
						if (ib >= b.size() || b[ib] != a[ia]) { is_sub = false; break; }
						ia++; ib++;
					}
					if (is_sub) subsumed.insert(di);
				}
			}
		}
	}
	std::cerr << "  subsumable (C ⊆ D)      : " << subsumed.size()
	          << " (" << (recs.empty() ? 0.0 :
	                      100.0 * (double)subsumed.size() / (double)recs.size())
	          << "% of pool)\n";

	// SSR opportunities: for clause C of length k_c, does there exist
	// a clause D such that (literals(D) \ {l}) ⊆ (literals(C) \ {¬l})
	// for some l where l ∈ D and ¬l ∈ C? If so, SSR would shorten C
	// by stripping ¬l.
	// Method: for each pair (C, D) with |D| ≤ |C|, check if there's
	// exactly one literal in D whose negation is in C, and every other
	// D literal appears in C. That one literal can be eliminated from C.
	// We count each shortenable C once (by its ClauseOfs set).
	std::unordered_set<size_t> ssr_shortenable;
	std::unordered_set<size_t> ssr_to_binary;  // produces size-2 clause
	for (size_t ci = 0; ci < recs.size(); ci++) {
		const auto &c = recs[ci].lits;
		if (c.size() < 3) continue;  // size-2 SSR → unit, outside our scope
		uint64_t sig_c = recs[ci].sig;
		for (size_t di = 0; di < recs.size(); di++) {
			if (di == ci) continue;
			const auto &d = recs[di].lits;
			if (d.size() > c.size()) continue;  // need |D| ≤ |C|
			// For SSR: d ⊆ c except one literal l ∈ d whose negation ¬l is in c.
			// Equivalently: |d ∩ c| == |d| - 1, and the missing d-lit's negation is in c.
			uint32_t mismatch_lit = 0;
			int mismatches = 0;
			size_t ic = 0, id = 0;
			while (id < d.size() && mismatches <= 1) {
				while (ic < c.size() && c[ic] < d[id]) ic++;
				if (ic < c.size() && c[ic] == d[id]) {
					ic++; id++;
				} else {
					mismatch_lit = d[id];
					mismatches++;
					id++;
				}
			}
			if (mismatches != 1) continue;
			// Check ¬mismatch_lit is in c. LiteralID::raw() flips bit 0 for neg.
			uint32_t neg_lit = mismatch_lit ^ 1;
			bool in_c = std::binary_search(c.begin(), c.end(), neg_lit);
			if (!in_c) continue;
			// Bit-sig safety: neg_lit should fall within c's bit signature.
			if (((1ULL << (hash_lit(neg_lit) & 63)) & sig_c) == 0) continue;
			// C can be shortened to size c.size() - 1 via SSR with D.
			ssr_shortenable.insert(ci);
			if (c.size() - 1 == 2) ssr_to_binary.insert(ci);
			break;  // only need to know if any D works; move on
		}
	}
	std::cerr << "  SSR-shortenable         : " << ssr_shortenable.size()
	          << " (" << (recs.empty() ? 0.0 :
	                      100.0 * (double)ssr_shortenable.size() / (double)recs.size())
	          << "% of pool)\n";
	std::cerr << "    of which → binary    : " << ssr_to_binary.size() << "\n";
}

// Dynamic subsumption measurement. Called at the end of
// branchOnLiteral's successful BCP. Sampled to keep cost bounded.
//
// Method (per affected-clauses-only approach):
//   1. Identify clauses AFFECTED by this branching: those containing
//      a literal whose negation was just assigned true.
//   2. For each affected clause C, compute its effective literal set
//      (stored literals minus currently-falsified ones; skip C if it
//      became satisfied).
//   3. For each affected C whose effective length shrank this BCP,
//      check if effective(C) subsumes some other clause D (looking
//      only at D that share a literal with effective(C), bounded by
//      the rarest-literal occurrence trick).
//   4. Count events. Do NOT modify state.
//
// Guards baked in (informed by prior bug classes):
//   - Captured sizes stored in const locals before any structural op.
//   - Explicit F_TRI/T_TRI/X_TRI branching (no implicit assumptions).
//   - Thread-local buffers clear()ed each call — no heap churn.
//   - Bounds checks on occurrence_lists_ index.
void Solver::analyzeDynamicSubsumption(unsigned bcp_start_ofs) {
	// Sample gate: only run every Nth branch.
	const unsigned every = std::max(1u, config_.analyze_dynamic_subsumption_every);
	static thread_local unsigned sample_counter = 0;
	sample_counter++;
	if (sample_counter % every != 0) return;

	// Guard: bcp_start_ofs must be within the literal stack.
	if (bcp_start_ofs >= literal_stack_.size()) return;  // nothing forced

	statistics_.dyn_sub_branches_sampled_++;

	// --- Collect newly-falsified literal RAWs ---
	// literal_stack_[i] is the TRUE literal just pushed; its negation is
	// the newly-falsified one. Deduplicate by variable (a single variable
	// can't be pushed twice in one BCP).
	static thread_local std::vector<uint32_t> falsified_raws;
	falsified_raws.clear();
	falsified_raws.reserve(literal_stack_.size() - bcp_start_ofs);
	for (unsigned i = bcp_start_ofs; i < literal_stack_.size(); i++) {
		LiteralID t = literal_stack_[i];
		LiteralID f = t.neg();
		falsified_raws.push_back(f.raw());
	}

	// --- Collect unique affected clauses ---
	// Each newly-falsified literal occurs in some subset of clauses
	// (via occurrence_lists_). A single clause may contain multiple
	// newly-falsified literals → visit it only once.
	static thread_local std::unordered_set<ClauseOfs> affected_seen;
	affected_seen.clear();
	for (uint32_t raw : falsified_raws) {
		LiteralID lit_false;
		lit_false.copyRaw(raw);
		// Guard: bounds on occurrence_lists_ access.
		if (lit_false.var() >= variables_.size()) continue;
		const auto &occ = occurrence_lists_[lit_false];
		for (ClauseOfs C_ofs : occ) {
			affected_seen.insert(C_ofs);
		}
	}
	statistics_.dyn_sub_affected_clauses_ += affected_seen.size();

	// --- For each affected C, compute effective literals. Track shrinkage.
	// Scope of subsumption check: among other original clauses only (we
	// stay in the stored-clause world; learned clauses don't have
	// occurrence_lists_ entries).
	static thread_local std::vector<uint32_t> eff_C;
	static thread_local std::vector<uint32_t> eff_D;

	// For each affected C, look for a D it subsumes.
	for (ClauseOfs C_ofs : affected_seen) {
		// Compute effective(C). Also measure stored length to detect
		// "actually shortened" clauses.
		eff_C.clear();
		unsigned stored_len = 0;
		bool C_satisfied = false;
		for (auto lt = beginOf(C_ofs); *lt != SENTINEL_LIT; lt++) {
			stored_len++;
			TriValue v = literal_values_[*lt];
			// Guard: literal_values must be one of the three defined
			// tri-states. Anything else is memory corruption.
			assert((v == T_TRI || v == F_TRI || v == X_TRI)
			       && "literal_values_ must be tri-state");
			if (v == T_TRI) { C_satisfied = true; break; }
			if (v == X_TRI) eff_C.push_back(lt->raw());
			// v == F_TRI: literal is falsified — excluded from effective form.
		}
		if (C_satisfied) continue;      // clause already satisfied; skip
		if (eff_C.empty()) continue;    // empty effective clause means
		                                // conflict; BCP would have caught it
		if (eff_C.size() == stored_len) continue;  // not actually shortened

		statistics_.dyn_sub_shortened_clauses_++;
		// Capture size BEFORE any further ops (guard against use-after-
		// move class of bugs).
		const size_t eff_C_len = eff_C.size();
		std::sort(eff_C.begin(), eff_C.end());

		// Compute a bit-signature of eff_C for cheap bitwise pre-filter.
		uint64_t sig_C = 0;
		for (uint32_t r : eff_C) {
			uint32_t h = r;
			h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15;
			sig_C |= (1ULL << (h & 63));
		}

		// Pick the literal in eff_C with smallest occurrence list — this
		// bounds the number of candidate D we examine. Iterate candidates.
		LiteralID rarest_lit;
		size_t rarest_count = (size_t)-1;
		for (uint32_t r : eff_C) {
			LiteralID l;
			l.copyRaw(r);
			size_t cnt = occurrence_lists_[l].size();
			if (cnt < rarest_count) {
				rarest_count = cnt;
				rarest_lit = l;
			}
		}
		// Guard: rarest_count must be at least 1 (contains C itself).
		if (rarest_count == 0 || rarest_count == (size_t)-1) continue;

		bool found_subsumption = false;
		for (ClauseOfs D_ofs : occurrence_lists_[rarest_lit]) {
			if (D_ofs == C_ofs) continue;                    // skip self
			if (isClauseRemoved(D_ofs)) continue;             // skip removed
			// Compute effective(D).
			eff_D.clear();
			bool D_sat = false;
			for (auto lt = beginOf(D_ofs); *lt != SENTINEL_LIT; lt++) {
				TriValue v = literal_values_[*lt];
				assert((v == T_TRI || v == F_TRI || v == X_TRI)
				       && "literal_values_ must be tri-state");
				if (v == T_TRI) { D_sat = true; break; }
				if (v == X_TRI) eff_D.push_back(lt->raw());
			}
			if (D_sat) continue;
			if (eff_D.size() < eff_C_len) continue;  // can't be superset
			std::sort(eff_D.begin(), eff_D.end());

			// Bit-signature pre-filter.
			uint64_t sig_D = 0;
			for (uint32_t r : eff_D) {
				uint32_t h = r;
				h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15;
				sig_D |= (1ULL << (h & 63));
			}
			if ((sig_C & sig_D) != sig_C) continue;

			// Subset check: is eff_C ⊆ eff_D?
			size_t i = 0, j = 0;
			bool is_sub = true;
			while (i < eff_C_len) {
				while (j < eff_D.size() && eff_D[j] < eff_C[i]) j++;
				if (j >= eff_D.size() || eff_D[j] != eff_C[i]) {
					is_sub = false;
					break;
				}
				i++; j++;
			}
			if (is_sub) {
				found_subsumption = true;
				break;  // one is enough for "this C creates a subsumption event"
			}
		}
		if (found_subsumption) {
			statistics_.dyn_sub_events_++;
			if (eff_C_len == 2) {
				statistics_.dyn_sub_events_to_binary_++;
			}
		}

		// --- SSR check (opt-in extension, same infrastructure) ---
		// For each literal l in effective(C), look for clause D with
		// ¬l in its effective form such that (effective(C) \ {l}) ⊆
		// (effective(D) \ {¬l}). If so, SSR would let us shorten D
		// by stripping ¬l.
		//
		// Guards from prior bug lessons:
		//   - Captured eff_C_len in a const local earlier (we reuse it
		//     here unchanged).
		//   - No mutation of eff_C during the loop; eff_D is reused
		//     cleanly (clear()) per candidate.
		//   - Tri-state assertion on every literal_values_ read.
		//   - Bounds-checked occurrence_lists_ access via LiteralID.var().
		bool found_ssr = false;
		unsigned ssr_result_len = 0;
		for (size_t li = 0; li < eff_C_len && !found_ssr; li++) {
			LiteralID lit_in_C;
			lit_in_C.copyRaw(eff_C[li]);
			LiteralID neg_l = lit_in_C.neg();
			if (neg_l.var() >= variables_.size()) continue;
			const auto &occ_neg = occurrence_lists_[neg_l];
			for (ClauseOfs D_ofs : occ_neg) {
				if (D_ofs == C_ofs) continue;
				if (isClauseRemoved(D_ofs)) continue;
				// Compute effective(D) excluding ¬l. The stored ¬l
				// literal must currently be X_TRI (otherwise D is
				// satisfied or ¬l is already falsified — both
				// uninteresting). We filter by skipping ¬l during
				// the effective-scan.
				eff_D.clear();
				bool D_sat = false;
				bool neg_l_in_D_and_free = false;
				for (auto lt = beginOf(D_ofs); *lt != SENTINEL_LIT; lt++) {
					TriValue v = literal_values_[*lt];
					assert((v == T_TRI || v == F_TRI || v == X_TRI));
					if (v == T_TRI) { D_sat = true; break; }
					if (v == X_TRI) {
						if (*lt == neg_l) {
							neg_l_in_D_and_free = true;
							// skip — we're computing effective(D) \ {¬l}
						} else {
							eff_D.push_back(lt->raw());
						}
					}
				}
				if (D_sat || !neg_l_in_D_and_free) continue;
				// Size check: need |eff_C \ {l}| = eff_C_len - 1 ≤ |eff_D \ {¬l}|.
				const size_t need_len = eff_C_len - 1;
				if (eff_D.size() < need_len) continue;
				std::sort(eff_D.begin(), eff_D.end());

				// Subset check: eff_C \ {l} ⊆ eff_D \ {¬l}.
				// eff_C is already sorted. We walk both, skipping
				// eff_C[li] in the comparison.
				size_t i = 0, j = 0;
				bool is_sub = true;
				while (i < eff_C_len) {
					if (i == li) { i++; continue; }  // skip l itself
					while (j < eff_D.size() && eff_D[j] < eff_C[i]) j++;
					if (j >= eff_D.size() || eff_D[j] != eff_C[i]) {
						is_sub = false;
						break;
					}
					i++; j++;
				}
				if (is_sub) {
					found_ssr = true;
					// Shortened D would have eff_D.size() literals.
					ssr_result_len = (unsigned)eff_D.size();
					break;
				}
			}
		}
		if (found_ssr) {
			statistics_.dyn_ssr_events_++;
			if (ssr_result_len == 2) {
				statistics_.dyn_ssr_events_to_binary_++;
			}
		}
	}
}

