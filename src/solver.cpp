/*
 * solver.cpp
 *
 *  Created on: Aug 23, 2012
 *      Author: marc
 */
#include "solver.h"
#include <deque>

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



void Solver::print(vector<LiteralID> &vec) {
	for (auto l : vec)
		cout << l.toInt() << " ";
	cout << endl;
}

void Solver::print(vector<unsigned> &vec) {
	for (auto l : vec)
		cout << l << " ";
	cout << endl;
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

		statistics_.exit_state_ = countSAT();

		statistics_.set_final_solution_count(stack_.top().getTotalModelCount());
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
}

SOLVER_StateT Solver::countSAT() {
	retStateT state = RESOLVED;

	while (true) {
		while (comp_manager_.findNextRemainingComponentOf(stack_.top())) {

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
				}

				// Try to find a new separator for this component
				if (separator_base_dl_ < 0) {
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

	// Stage 3: Full mincut discovery
	SeparatorCandidate candidate;
	bool found = find_best_separator(
		info, candidate,
		config_.separator_tries,
		statistics_.num_decisions_,
		config_.separator_min_second_comp,
		config_.separator_max_size);

	if (!found) {
		separator_cache_.insertNegative(sketch);
		return false;
	}

	// Compute partition for cache storage (for future repair)
	std::set<CutNode> removed_set(candidate.separator.begin(), candidate.separator.end());
	auto comps = components_after_removing(info, removed_set);

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
		if (separator_base_dl_ >= 0 && stack_.get_decision_level() <= separator_base_dl_)
			clearSeparator();

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
					 << " b0=" << stack_.top().branch_model_count_[0]
					 << " b1=" << stack_.top().branch_model_count_[1]
					 << " u0=" << stack_.top().branch_found_unsat_[0]
					 << " u1=" << stack_.top().branch_found_unsat_[1]
					 << " | active_vars=" << av
					 << " lit_stack=" << literal_stack_.size()
					 << " removed=" << removed_clauses_.size() << endl;
			}
			unmarkClauseRemoved(stack_.top().clauseBranchOfs());
		}

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

	// DEBUG
	if (uip_clauses_.back().size() == 0)
		cout << " EMPTY CLAUSE FOUND" << endl;
	// END DEBUG

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
//	// RRR
//	else if(var(uip_clauses_.back().front()).decision_level
//			< stack_.get_decision_level()
//			&& assertion_level_ <  stack_.get_decision_level()){
//         stack_.top().set_both_branches_unsat();
//         return BACKTRACK;
//	}
//
//
//	// RRR
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

	if (config_.perform_failed_lit_test && bSucceeded && removed_clauses_.empty()) {
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

//bool Solver::implicitBCP() {
//  static vector<LiteralID> test_lits(num_variables());
//  static LiteralIndexedVector<unsigned char> viewed_lits(num_variables() + 1,
//      0);
//
//  unsigned stack_ofs = stack_.top().literal_stack_ofs();
//  while (stack_ofs < literal_stack_.size()) {
//    test_lits.clear();
//    for (auto it = literal_stack_.begin() + stack_ofs;
//        it != literal_stack_.end(); it++) {
//      for (auto cl_ofs : occurrence_lists_[it->neg()])
//        if (!isSatisfied(cl_ofs)) {
//          for (auto lt = beginOf(cl_ofs); *lt != SENTINEL_LIT; lt++)
//            if (isActive(*lt) && !viewed_lits[lt->neg()]) {
//              test_lits.push_back(lt->neg());
//              viewed_lits[lt->neg()] = true;
//
//            }
//        }
//    }
//
//    stack_ofs = literal_stack_.size();
//    for (auto jt = test_lits.begin(); jt != test_lits.end(); jt++)
//      viewed_lits[*jt] = false;
//
//    statistics_.num_failed_literal_tests_ += test_lits.size();
//
//    for (auto lit : test_lits)
//      if (isActive(lit)) {
//        unsigned sz = literal_stack_.size();
//        // we increase the decLev artificially
//        // s.t. after the tentative BCP call, we can learn a conflict clause
//        // relative to the assignment of *jt
//        stack_.startFailedLitTest();
//        setLiteralIfFree(lit);
//
//        assert(!hasAntecedent(lit));
//
//        bool bSucceeded = BCP(sz);
//        if (!bSucceeded)
//          recordAllUIPCauses();
//
//        stack_.stopFailedLitTest();
//
//        while (literal_stack_.size() > sz) {
//          unSet(literal_stack_.back());
//          literal_stack_.pop_back();
//        }
//
//        if (!bSucceeded) {
//        	statistics_.num_failed_literals_detected_++;
//          sz = literal_stack_.size();
//          for (auto it = uip_clauses_.rbegin(); it != uip_clauses_.rend();
//              it++) {
//            setLiteralIfFree(it->front(), addUIPConflictClause(*it));
//          }
//          if (!BCP(sz))
//            return false;
//        }
//      }
//  }
//  return true;
//}

// this is IBCP 30.08
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

		for (auto lit : test_lits)
			if (isActive(lit) && threshold <= literal(lit).activity_score_) {
				unsigned sz = literal_stack_.size();
				// we increase the decLev artificially
				// s.t. after the tentative BCP call, we can learn a conflict clause
				// relative to the assignment of *jt
				stack_.startFailedLitTest();
				setLiteralIfFree(lit);

				assert(!hasAntecedent(lit));

				bool bSucceeded = BCP(sz);
				if (!bSucceeded)
					recordAllUIPCauses();

				stack_.stopFailedLitTest();

				while (literal_stack_.size() > sz) {
					unSet(literal_stack_.back());
					literal_stack_.pop_back();
				}

				if (!bSucceeded) {
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
					sz = literal_stack_.size();
					for (auto it = uip_clauses_.rbegin();
							it != uip_clauses_.rend(); it++) {
						if (it->size() == 0)
							cout << "EMPTY CLAUSE FOUND" << endl;
						setLiteralIfFree(it->front(),
								addUIPConflictClause(*it));
					}
					if (!BCP(sz))
						return false;
				}
			}
	}

	// BEGIN TEST
//	float max_score = -1;
//	float score;
//	unsigned max_score_var = 0;
//	for (auto it =
//			component_analyzer_.superComponentOf(stack_.top()).varsBegin();
//			*it != varsSENTINEL; it++)
//		if (isActive(*it)) {
//			score = scoreOf(*it);
//			if (score > max_score) {
//				max_score = score;
//				max_score_var = *it;
//			}
//		}
//	LiteralID theLit(max_score_var,
//			literal(LiteralID(max_score_var, true)).activity_score_
//					> literal(LiteralID(max_score_var, false)).activity_score_);
//	if (!fail_test(theLit.neg())) {
//		cout << ".";
//
//		statistics_.num_failed_literals_detected_++;
//		unsigned sz = literal_stack_.size();
//		for (auto it = uip_clauses_.rbegin(); it != uip_clauses_.rend(); it++) {
//			setLiteralIfFree(it->front(), addUIPConflictClause(*it));
//		}
//		if (!BCP(sz))
//			return false;
//
//	}
	// END
	return true;
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
				//assert(stack_.TOS_decLit() == curr_lit);
//				cout << "R" << curr_lit.toInt() << "S"
//				     << var(curr_lit).ante.isAnt() << " "  << endl;
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

//	cout << "T" << curr_lit.toInt() << "U "
//     << var(curr_lit).decision_level << ", " << stack_.get_decision_level() << endl;
//	cout << "V"  << var(curr_lit).ante.isAnt() << " "  << endl;
	minimizeAndStoreUIPClause(curr_lit.neg(), tmp_clause, seen);

//	if (var(curr_lit).decision_level > assertion_level_)
//		assertion_level_ = var(curr_lit).decision_level;
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

