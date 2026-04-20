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

		last_ccl_deletion_time_ = last_ccl_cleanup_time_ =
				statistics_.getTime();

		violated_clause.reserve(num_variables());

		comp_manager_.initialize(literals_, literal_pool_);
		comp_manager_.setRemovedClauses(&removed_clauses_);
		comp_manager_.setFormulaRefs(&literals_, &literal_pool_, original_lit_pool_size_);
		// Note: verify_cache logic is implemented in solver_rec.cpp at the
		// decomposition site. The ContentCache's own verify_mode is NOT
		// toggled here because we want the solver's lookup to behave
		// normally (return hits); solver_rec then recomputes independently
		// and compares cached vs. recomputed counts.
		if (config_.verify_cache)
			cout << "c verify_cache mode ON (compare cached vs. recomputed on every hit; aborts on mismatch)" << endl;

		if (config_.use_recursive_solver) {
			statistics_.exit_state_ = countSATRec();
			// countSATRec sets final_solution_count internally
		} else {
			statistics_.exit_state_ = countSAT();
			statistics_.set_final_solution_count(stack_.top().getTotalModelCount());
		}
		statistics_.num_long_conflict_clauses_ = num_conflict_clauses();

	} else {
		statistics_.exit_state_ = SUCCESS;
		statistics_.set_final_solution_count(0.0);
		cout << endl << " FOUND UNSAT DURING PREPROCESSING " << endl;
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

SOLVER_StateT Solver::countSAT() {
	retStateT state = RESOLVED;

	while (true) {
		while (comp_manager_.findNextRemainingComponentOf(stack_.top())) {
			if (stopwatch_.timeBoundBroken())
				return TIMEOUT;

			// --- Very long clause: branch immediately without Dinic's ---
			if (config_.perform_clause_branching) {
				Component &curr_comp = comp_manager_.currentRemainingComponentOf(stack_.top());
				unsigned n_vars = curr_comp.num_variables();
				unsigned threshold = n_vars * 3 / 10;  // 30%
				if (threshold >= config_.clause_branch_min_length) {
					ClauseOfs best = NOT_A_CLAUSE;
					unsigned best_active = 0;
					for (auto it = curr_comp.clsBegin(); *it != clsSENTINEL; it++) {
						ClauseOfs ofs = comp_manager_.clauseOfsOf(*it);
						if (isClauseRemoved(ofs) || isSatisfied(ofs) || ofs >= original_lit_pool_size_)
							continue;
						unsigned active = 0;
						for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++)
							if (isActive(*lt))
								active++;
						if (active >= threshold && active > best_active) {
							best_active = active;
							best = ofs;
						}
					}
					if (best != NOT_A_CLAUSE) {
						decideClause(best);
						continue;
					}
				}
			}

			// --- Separator branching ---
			if (config_.perform_separator_branching) {
				Component &curr_comp = comp_manager_.currentRemainingComponentOf(stack_.top());

				// Try to use an active separator element
				if (separator_base_dl_ >= 0) {
					int idx = findMatchingSeparatorElement(curr_comp);
					if (idx >= 0) {
						separator_used_[idx] = true;
						if (separator_elements_[idx].kind == CutNode::CLAUSE) {
							decideClause(separator_elements_[idx].id);
							continue;
						} else {
							decideSeparatorVariable(separator_elements_[idx].id);
							goto do_bcp;
						}
					}
					// No matching element for this component —
					// if all elements are used AND we're past the
					// separator's decision levels, allow finding new
					// separators for sub-components.
					// Do NOT clear if we're still within the separator's
					// branching levels (backtracking could revisit them).
					bool all_used = true;
					for (size_t i = 0; i < separator_used_.size(); i++)
						if (!separator_used_[i]) { all_used = false; break; }
					if (all_used) {
						separator_stack_.push_back({
							separator_elements_, separator_used_, separator_base_dl_});
						clearSeparator();
					}
				}

				// Try to find a new separator for this component
				// Only allow if no stacked separator's elements are
				// still on the decision stack (prevents premature
				// inner separators during outer separator's branch 2)
				if (separator_base_dl_ < 0 && separator_stack_.empty()) {
					if (tryInstallSeparator(curr_comp)) {
						int idx = findMatchingSeparatorElement(curr_comp);
						if (idx >= 0) {
							separator_used_[idx] = true;
							if (separator_elements_[idx].kind == CutNode::CLAUSE) {
								decideClause(separator_elements_[idx].id);
								continue;
							} else {
								decideSeparatorVariable(separator_elements_[idx].id);
								goto do_bcp;
							}
						}
						clearSeparator();
					}
				}
			}

			// Try clause branching
			{
				ClauseOfs cl_branch = selectClauseForBranching();
				if (cl_branch != NOT_A_CLAUSE) {
					decideClause(cl_branch);
					continue;
				}
			}

			decideLiteral();
do_bcp:
			if (stopwatch_.timeBoundBroken())
				return TIMEOUT;
			if (stopwatch_.interval_tick())
				printOnlineStats();

			while (!bcp()) {
				state = resolveConflict();
				if (state == BACKTRACK)
					break;
			}
			if (state == BACKTRACK)
				break;
		}

		state = backtrack();
		if (state == EXIT)
			return SUCCESS;
		while (state != PROCESS_COMPONENT && !bcp()) {
			if (stack_.top().isClauseBranch() && stack_.top().isSecondBranch()) {
				// BCP failed in branch 2 of clause branch:
				// no conflict analysis, just mark UNSAT and backtrack
				if (config_.verbose) {
					cout << "  BCP_FAIL_CB2 dl=" << stack_.get_decision_level()
						 << " cl=" << stack_.top().clauseBranchOfs()
						 << " violated:";
					for (auto l : violated_clause)
						cout << " " << l.toInt();
					// dump watch lists for violated clause literals
					for (auto l : violated_clause) {
						cout << " | wl(" << l.toInt() << "):";
						for (auto w = literal(l).watch_list_.begin(); *w != SENTINEL_CL; w++)
							cout << " " << *w << (isClauseRemoved(*w) ? "R" : "");
					}
					cout << endl;
				}
				stack_.top().mark_branch_unsat();
				state = backtrack();
				if (state == EXIT)
					return SUCCESS;
			} else {
				state = resolveConflict();
				if (state == BACKTRACK) {
					state = backtrack();
					if (state == EXIT)
						return SUCCESS;
				}
			}
		}
	}
	return SUCCESS;
}

void Solver::decideLiteral() {
	// establish another decision stack level
	stack_.push_back(
			StackLevel(stack_.top().currentRemainingComponent(),
					literal_stack_.size(),
					comp_manager_.component_stack_size()));
	float max_score = -1;
	float score;
	unsigned max_score_var = 0;
	for (auto it =
			comp_manager_.superComponentOf(stack_.top()).varsBegin();
			*it != varsSENTINEL; it++) {
		score = scoreOf(*it);
		if (score > max_score) {
			max_score = score;
			max_score_var = *it;
		}
	}
	// this assert should always hold,
	// if not then there is a bug in the logic of countSAT();
	assert(max_score_var != 0);

	LiteralID theLit(max_score_var,
			literal(LiteralID(max_score_var, true)).activity_score_
					> literal(LiteralID(max_score_var, false)).activity_score_);

	setLiteralIfFree(theLit);
	statistics_.num_decisions_++;

	if (statistics_.num_decisions_ % 128 == 0)
//    if (statistics_.num_conflicts_ % 128 == 0)
     decayActivities();
       // decayActivitiesOf(comp_manager_.superComponentOf(stack_.top()));
	assert(
			stack_.top().remaining_components_ofs() <= comp_manager_.component_stack_size());
}

void Solver::decideClause(ClauseOfs cl_ofs) {
	assert(!isClauseRemoved(cl_ofs) && "Cannot branch on already removed clause");
	assert(!isSatisfied(cl_ofs) && "Cannot branch on satisfied clause");
	if (config_.verbose) {
		unsigned active_vars = 0;
		for (unsigned v = 1; v < variables_.size(); v++)
			if (isActive(LiteralID(v, true))) active_vars++;
		cout << "CLAUSE_BRANCH dl=" << stack_.get_decision_level()
			 << " cl_ofs=" << cl_ofs << " lits:";
		for (auto it = beginOf(cl_ofs); *it != SENTINEL_LIT; it++)
			cout << " " << (isActive(*it) ? "" : "!") << it->toInt();
		cout << " | active_vars=" << active_vars
			 << " lit_stack=" << literal_stack_.size()
			 << " removed=" << removed_clauses_.size() << endl;
	}
	stack_.push_back(
			StackLevel(stack_.top().currentRemainingComponent(),
					literal_stack_.size(),
					comp_manager_.component_stack_size(),
					true /* clause_branch */,
					cl_ofs));

	// Mark clause as removed (BCP and component analyzer will skip it)
	markClauseRemoved(cl_ofs);
	assert(isClauseRemoved(cl_ofs));

	statistics_.num_decisions_++;
	// Branch 1: clause removed, no literal assignments, no BCP needed
}

ClauseOfs Solver::selectClauseForBranching() {
	if (!config_.perform_clause_branching)
		return NOT_A_CLAUSE;

	Component &comp = comp_manager_.currentRemainingComponentOf(stack_.top());
	ClauseOfs best = NOT_A_CLAUSE;
	unsigned best_len = 0;

	for (auto it = comp.clsBegin(); *it != clsSENTINEL; it++) {
		ClauseOfs ofs = comp_manager_.clauseOfsOf(*it);
		if (isClauseRemoved(ofs))
			continue;
		// Only branch on original clauses, not conflict clauses
		if (ofs >= original_lit_pool_size_)
			continue;
		// Count active (unassigned) literals
		unsigned active = 0;
		for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++)
			if (isActive(*lt))
				active++;
		if (active > best_len && active >= config_.clause_branch_min_length) {
			best_len = active;
			best = ofs;
		}
	}
	return best;
}

FormulaInfo Solver::buildFormulaInfo(Component &comp) {
	FormulaInfo info;

	for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++) {
		if (isActive(LiteralID(*it, true)))
			info.active_vars.push_back(*it);
	}

	for (auto it = comp.clsBegin(); *it != clsSENTINEL; it++) {
		ClauseOfs ofs = comp_manager_.clauseOfsOf(*it);
		if (isClauseRemoved(ofs))
			continue;
		if (isSatisfied(ofs))
			continue;
		if (ofs >= original_lit_pool_size_)
			continue;

		std::vector<unsigned> vars_in_clause;
		for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++) {
			if (isActive(*lt))
				vars_in_clause.push_back(lt->var());
		}
		if (vars_in_clause.size() < 2)
			continue;

		info.active_clause_ids.push_back(ofs);
		info.clause_variables.push_back(std::move(vars_in_clause));
	}

	// Add binary clauses between active variables in the component
	unsigned max_var = info.active_vars.empty() ? 0 : info.active_vars.back();
	std::vector<bool> var_in_comp(max_var + 1, false);
	for (unsigned v : info.active_vars)
		var_in_comp[v] = true;

	for (unsigned v : info.active_vars) {
		for (int sign = 0; sign <= 1; sign++) {
			LiteralID lit(v, sign == 0);
			unsigned orig_count = literal(lit).original_binary_link_count_;
			unsigned idx = 0;
			for (auto bt = literal(lit).binary_links_.begin();
				 *bt != SENTINEL_LIT; bt++, idx++) {
				if (idx >= orig_count) break;  // skip learned binary clauses
				unsigned other_var = bt->var();
				if (other_var <= v) continue;
				if (other_var > max_var || !var_in_comp[other_var]) continue;
				if (isSatisfied(*bt)) continue;
				if (isSatisfied(lit)) continue;

				info.active_clause_ids.push_back(0);  // dummy ID for binary
				info.clause_variables.push_back({v, other_var});
			}
		}
	}

	info.buildIndex();
	return info;
}

std::vector<CutNode> Solver::sortSeparatorElements(const std::vector<CutNode> &sep) {
	std::vector<CutNode> clause_nodes, var_nodes;
	for (const auto &node : sep) {
		if (node.kind == CutNode::CLAUSE)
			clause_nodes.push_back(node);
		else
			var_nodes.push_back(node);
	}
	std::sort(var_nodes.begin(), var_nodes.end(),
		[this](const CutNode &a, const CutNode &b) {
			return scoreOf(a.id) > scoreOf(b.id);
		});
	std::vector<CutNode> result;
	for (const auto &n : clause_nodes) result.push_back(n);
	for (const auto &n : var_nodes) result.push_back(n);
	return result;
}

bool Solver::tryInstallSeparator(Component &comp) {
	if (comp.num_variables() < config_.separator_min_active_vars)
		return false;

	separator_cache_.verbose = config_.verbose;

	FormulaInfo info = buildFormulaInfo(comp);

	if (info.active_vars.size() < config_.separator_min_active_vars)
		return false;

	KMVSketch sketch = separator_cache_.computeSketch(info);

	// Stage 1: Check negative cache
	if (separator_cache_.isLikelyNegative(sketch)) {
		if (config_.verbose)
			cout << "SEP_NEG_HIT" << endl;
		return false;
	}

	// Stage 2: Try to transfer/repair a cached separator
	std::vector<CutNode> cached_sep;
	std::vector<int> cached_sizes;
	if (separator_cache_.findAndTransfer(info, sketch, cached_sep, cached_sizes)) {
		separator_elements_ = sortSeparatorElements(cached_sep);
		separator_used_.assign(separator_elements_.size(), false);
		separator_base_dl_ = stack_.get_decision_level();
		if (config_.verbose) {
			cout << "SEPARATOR_CACHED dl=" << separator_base_dl_
				 << " size=" << separator_elements_.size() << endl;
		}
		return true;
	}

	// Stage 3: Separator discovery.
	// Large components (≥ 20 vars): use METIS for well-balanced separators.
	// METIS has higher per-call overhead but produces much better balance,
	// which pays off exponentially at early recursion levels.
	// Small components: use Dinic's-based weighted separator (fast, adequate).
	SeparatorCandidate candidate;
	bool found = false;

	if (prefer_metis_separator_) {
		found = find_metis_separator(
			info, candidate, config_.separator_min_second_comp);
	}

	if (!found) {
		found = find_weighted_separator(
			info, candidate,
			config_.separator_tries,
			config_.verify_cache ? 42 : (int)statistics_.num_decisions_,
			0,   // min_balance (auto: n_vars/4)
			30,  // max_iterations
			30); // walks_per_iteration
	}

	if (found && config_.verbose) {
		int nv = 0, nc = 0;
		for (const auto &nd : candidate.separator)
			if (nd.kind == CutNode::VAR) nv++; else nc++;
		cout << "SEPARATOR_FOUND vars=" << info.active_vars.size()
			 << " clauses=" << info.active_clause_ids.size()
			 << " sep=" << candidate.separator.size()
			 << " (" << nv << "V+" << nc << "C)"
			 << " sides=";
		for (int s : candidate.component_var_sizes)
			cout << s << "/";
		cout << endl;
	}

	if (!found) {
		separator_cache_.insertNegative(sketch);
		return false;
	}

	// Verify separator actually disconnects the graph
	std::set<CutNode> removed_set(candidate.separator.begin(), candidate.separator.end());
	auto comps = components_after_removing(info, removed_set);
	assert(comps.size() >= 2 && "Separator must disconnect the graph");

	// Partition A = largest component vars, B = union of rest
	std::set<unsigned> partition_a, partition_b;
	if (!comps.empty()) {
		// Sort by variable count descending
		std::sort(comps.begin(), comps.end(),
			[](const std::vector<CutNode> &a, const std::vector<CutNode> &b) {
				return component_variable_count(a) > component_variable_count(b);
			});
		for (const auto &nd : comps[0])
			if (nd.kind == CutNode::VAR)
				partition_a.insert(nd.id);
		for (size_t i = 1; i < comps.size(); i++)
			for (const auto &nd : comps[i])
				if (nd.kind == CutNode::VAR)
					partition_b.insert(nd.id);
	}

	// Insert into cache
	separator_cache_.insert(info, candidate.separator, partition_a, partition_b,
		candidate.component_var_sizes, sketch);

	// Install separator
	separator_elements_ = sortSeparatorElements(candidate.separator);
	separator_used_.assign(separator_elements_.size(), false);
	separator_base_dl_ = stack_.get_decision_level();

	if (config_.verbose) {
		cout << "SEPARATOR dl=" << separator_base_dl_
			 << " size=" << separator_elements_.size() << " elems:";
		for (const auto &e : separator_elements_)
			cout << " " << (e.kind == CutNode::CLAUSE ? "C" : "V") << e.id;
		cout << " (cache: " << separator_cache_.entries.size() << " entries)" << endl;
	}

	return true;
}

int Solver::findMatchingSeparatorElement(Component &comp) {
	for (int i = 0; i < (int)separator_elements_.size(); i++) {
		if (separator_used_[i])
			continue;

		const CutNode &node = separator_elements_[i];

		if (node.kind == CutNode::VAR) {
			if (!isActive(LiteralID(node.id, true)))
				continue;
			bool found = false;
			for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++) {
				if (*it == node.id) { found = true; break; }
			}
			if (!found) continue;
			return i;
		} else {
			ClauseOfs ofs = node.id;
			if (isClauseRemoved(ofs) || isSatisfied(ofs))
				continue;
			bool found = false;
			for (auto it = comp.clsBegin(); *it != clsSENTINEL; it++) {
				if (comp_manager_.clauseOfsOf(*it) == ofs) { found = true; break; }
			}
			if (!found) continue;
			return i;
		}
	}
	return -1;
}

void Solver::decideSeparatorVariable(VariableIndex var) {
	assert(isActive(LiteralID(var, true)) && "Separator variable must be active");
	if (config_.verbose)
		cout << "SEP_VAR_BRANCH dl=" << stack_.get_decision_level()
			 << " var=" << var << endl;
	stack_.push_back(
		StackLevel(stack_.top().currentRemainingComponent(),
				literal_stack_.size(),
				comp_manager_.component_stack_size()));

	LiteralID theLit(var,
		literal(LiteralID(var, true)).activity_score_
			> literal(LiteralID(var, false)).activity_score_);

	setLiteralIfFree(theLit);
	statistics_.num_decisions_++;

	if (statistics_.num_decisions_ % 128 == 0)
		decayActivities();
}

void Solver::clearSeparator() {
	separator_elements_.clear();
	separator_used_.clear();
	separator_base_dl_ = -1;
}

retStateT Solver::backtrack() {
	assert(
			stack_.top().remaining_components_ofs() <= comp_manager_.component_stack_size());
	do {
		// Clear separator if backtracking past the level where it was found
		if (separator_base_dl_ >= 0 && stack_.get_decision_level() <= separator_base_dl_) {
			clearSeparator();
			// Restore parent separator from stack if available
			while (!separator_stack_.empty() &&
				   stack_.get_decision_level() <= separator_stack_.back().base_dl) {
				// This stacked separator is also past — pop it too
				separator_stack_.pop_back();
			}
			if (!separator_stack_.empty()) {
				auto &parent = separator_stack_.back();
				separator_elements_ = parent.elements;
				// Reset used flags — we're in a different branch now,
				// elements need to be re-evaluated for the new formula state
				separator_used_.assign(separator_elements_.size(), false);
				separator_base_dl_ = parent.base_dl;
				separator_stack_.pop_back();
			}
		}

		if (stack_.top().branch_found_unsat())
			comp_manager_.removeAllCachePollutionsOf(stack_.top());
		else if (stack_.top().anotherCompProcessible())
			return PROCESS_COMPONENT;

		if (!stack_.top().isSecondBranch()) {
			if (stack_.top().isClauseBranch()) {
				// Clause branch: switch to branch 2
				// Branch 2: clause stays removed + negate all its literals
				ClauseOfs cl_ofs = stack_.top().clauseBranchOfs();
				stack_.top().changeBranch();
				reactivateTOS();
				if (config_.verbose) {
					cout << "  BRANCH2 cl=" << cl_ofs << " pool:";
					for (auto it = beginOf(cl_ofs); *it != SENTINEL_LIT; it++)
						cout << " " << it->toInt() << (isSatisfied(*it) ? "S" : isActive(*it) ? "" : "R");
					cout << endl;
				}
				// Assign negation of each literal in the removed clause
				for (auto it = beginOf(cl_ofs); *it != SENTINEL_LIT; it++) {
					if (isSatisfied(*it)) {
						// Literal already true at lower level — can't negate
						if (config_.verbose)
							cout << "  BRANCH2_UNSAT: lit " << it->toInt() << " satisfied at dl=" << var(*it).decision_level << endl;
						stack_.top().mark_branch_unsat();
						break;
					}
					if (isActive(*it))
						setLiteralIfFree(it->neg());
					// If already false (resolved), consistent — skip
				}
				return RESOLVED;
			}
			LiteralID aLit = TOS_decLit();
			assert(stack_.get_decision_level() > 0);
			stack_.top().changeBranch();
			reactivateTOS();
			setLiteralIfFree(aLit.neg(), NOT_A_CLAUSE);
			return RESOLVED;
		}
		// OTHERWISE:  backtrack further
		// If clause branch, restore the removed clause
		if (stack_.top().isClauseBranch()) {
			if (config_.verbose) {
				unsigned av = 0;
				for (unsigned v = 1; v < variables_.size(); v++)
					if (isActive(LiteralID(v, true))) av++;
				cout << "CLAUSE_DONE dl=" << stack_.get_decision_level()
					 << " cl=" << stack_.top().clauseBranchOfs()
					 << " total=" << stack_.top().getTotalModelCount()
					 << " b0=" << stack_.top().branchModelCount(0)
					 << " b1=" << stack_.top().branchModelCount(1)
					 << " u0=" << stack_.top().branchFoundUnsat(0)
					 << " u1=" << stack_.top().branchFoundUnsat(1)
					 << " | active_vars=" << av
					 << " lit_stack=" << literal_stack_.size()
					 << " removed=" << removed_clauses_.size() << endl;
			}
			unmarkClauseRemoved(stack_.top().clauseBranchOfs());
		}

		assert(stack_.top().getTotalModelCount() >= 0);
		comp_manager_.cacheModelCountOf(stack_.top().super_component(),
				stack_.top().getTotalModelCount());

		if (stack_.get_decision_level() <= 0)
			break;
		reactivateTOS();

		assert(stack_.size()>=2);
		(stack_.end() - 2)->includeSolution(stack_.top().getTotalModelCount());
		stack_.pop_back();
		// step to the next component not yet processed
		stack_.top().nextUnprocessedComponent();

		assert(
				stack_.top().remaining_components_ofs() < comp_manager_.component_stack_size()+1);

	} while (stack_.get_decision_level() >= 0);
	return EXIT;
}

retStateT Solver::resolveConflict() {
	recordLastUIPCauses();

	if (statistics_.num_clauses_learned_ - last_ccl_deletion_time_
			> statistics_.clause_deletion_interval()) {
		deleteConflictClauses();
		last_ccl_deletion_time_ = statistics_.num_clauses_learned_;
	}

	if (statistics_.num_clauses_learned_ - last_ccl_cleanup_time_ > 100000) {
		compactConflictLiteralPool();
		last_ccl_cleanup_time_ = statistics_.num_clauses_learned_;
	}

	statistics_.num_conflicts_++;

	assert(
			stack_.top().remaining_components_ofs() <= comp_manager_.component_stack_size());

	assert(uip_clauses_.size() == 1);

	stack_.top().mark_branch_unsat();
	//BEGIN Backtracking
	// maybe the other branch had some solutions
	if (stack_.top().isSecondBranch()) {
		return BACKTRACK;
	}

	Antecedent ant(NOT_A_CLAUSE);
	// this has to be checked since using implicit BCP
	// and checking literals there not exhaustively
	// we cannot guarantee that uip_clauses_.back().front() == TOS_decLit().neg()
	// this is because we might have checked a literal
	// during implict BCP which has been a failed literal
	// due only to assignments made at lower decision levels
	if (uip_clauses_.back().front() == TOS_decLit().neg()) {
		assert(TOS_decLit().neg() == uip_clauses_.back()[0]);
		var(TOS_decLit().neg()).ante = addUIPConflictClause(
				uip_clauses_.back());
		ant = var(TOS_decLit()).ante;
	}
	assert(stack_.get_decision_level() > 0);
	assert(stack_.top().branch_found_unsat());

	// we do not have to remove pollutions here,
	// since conflicts only arise directly before
	// remaining components are stored
	// hence
	assert(
			stack_.top().remaining_components_ofs() == comp_manager_.component_stack_size());

	stack_.top().changeBranch();
	LiteralID lit = TOS_decLit();
	reactivateTOS();
	setLiteralIfFree(lit.neg(), ant);
//END Backtracking
	return RESOLVED;
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

void Solver::printOnlineStats() {
	if (config_.quiet)
		return;

	cout << endl;
	cout << "time elapsed: " << stopwatch_.getElapsedSeconds() << "s" << endl;
	if(config_.verbose) {
	  cout << "conflict clauses (all / bin / unit) \t";
	  cout << num_conflict_clauses();
	  cout << "/" << statistics_.num_binary_conflict_clauses_ << "/"
	      << unit_clauses_.size() << endl;
	  cout << "failed literals found by implicit BCP \t "
	      << statistics_.num_failed_literals_detected_ << endl;
	  ;

	  cout << "implicit BCP miss rate \t "
	      << statistics_.implicitBCP_miss_rate() * 100 << "%";
	  cout << endl;

	  comp_manager_.gatherStatistics();

	  cout << "cache size " << statistics_.cache_MB_memory_usage()	<< "MB" << endl;
	  cout << "components (stored / hits) \t\t"
	      << statistics_.cached_component_count() << "/"
	      << statistics_.cache_hits() << endl;
	  cout << "avg. variable count (stored / hits) \t"
	      << statistics_.getAvgComponentSize() << "/"
	      << statistics_.getAvgCacheHitSize();
	  cout << endl;
	  cout << "cache miss rate " << statistics_.cache_miss_rate() * 100 << "%"
	      << endl;
	}
}

