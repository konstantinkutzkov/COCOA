/*
 * solver_rec.cpp
 *
 * Recursive #SAT implementation based on the pseudocode agreed with the user.
 * Reuses BCP, component analyzer, content cache, and separator finding from
 * the existing iterative Solver. Does NOT use StackLevel or DecisionStack
 * beyond level 0 for component analyzer setup.
 *
 * Control flow:
 *   solveComponent(F, separator, reset):
 *     if BCP-conflict-state: return 0
 *     if reset == false and separator empty:
 *       decompose F into components; product of their solveComponent(c, [], true)
 *     if reset: separator = findSeparator(F); reset = false
 *     if separator non-empty:
 *       el = separator[0]; rest = separator[1:]
 *       if el is consumed (BCP): return solveComponent(F, rest, false)
 *       if el is variable: return A + B    where A,B are the two branches
 *       if el is clause:   return A - B    where A = F\{C}, B = F\{C}∧¬C
 *     // no separator: regular variable branch
 *     return A + B
 *
 * State management:
 *   - literal_stack_: save size before each recursive call, unSet + pop on return.
 *   - removed_clauses_: we only mark/unmark clauses we removed at this level.
 *   - content cache: check before computing, store after.
 */

#include "solver.h"

#include <sstream>
#include <unordered_map>

using namespace std;

// Signature of a component: sorted list of sorted clauses (each clause is
// a sorted vector of current literal ints over active variables).
static string componentSignature(
    const Component &comp,
    const vector<LiteralID> &literal_pool,
    const LiteralIndexedVector<Literal> &literals,
    const LiteralIndexedVector<TriValue> &literal_values,
    const vector<ClauseOfs> &clause_id_to_ofs,
    const unordered_map<ClauseOfs, unsigned> &removed_clauses,
    unsigned original_lit_pool_size) {
  vector<unsigned> active;
  for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++)
    if (literal_values[LiteralID(*it, true)] == X_TRI)
      active.push_back(*it);
  std::sort(active.begin(), active.end());

  vector<vector<int>> clauses;
  for (auto it = comp.clsBegin(); *it != clsSENTINEL; it++) {
    ClauseOfs ofs = clause_id_to_ofs[*it];
    if (removed_clauses.count(ofs)) continue;
    if (ofs >= original_lit_pool_size) continue;
    bool satisfied = false;
    vector<int> lits;
    for (auto lt = literal_pool.begin() + ofs; *lt != SENTINEL_LIT; lt++) {
      if (literal_values[*lt] == T_TRI) { satisfied = true; break; }
      if (literal_values[*lt] == X_TRI) lits.push_back(lt->toInt());
    }
    if (satisfied || lits.size() < 2) continue;
    std::sort(lits.begin(), lits.end());
    clauses.push_back(std::move(lits));
  }
  std::unordered_set<unsigned> act_set(active.begin(), active.end());
  for (unsigned v : active) {
    for (int sign = 0; sign <= 1; sign++) {
      LiteralID lit(v, sign == 0);
      unsigned orig_count = literals[lit].original_binary_link_count_;
      unsigned idx = 0;
      for (auto bt = literals[lit].binary_links_.begin();
           *bt != SENTINEL_LIT; bt++, idx++) {
        if (idx >= orig_count) break;
        unsigned other = bt->var();
        if (other <= v) continue;
        if (!act_set.count(other)) continue;
        if (literal_values[*bt] == T_TRI) continue;
        if (literal_values[lit] == T_TRI) continue;
        vector<int> lits = {lit.toInt(), bt->toInt()};
        std::sort(lits.begin(), lits.end());
        clauses.push_back(std::move(lits));
      }
    }
  }
  std::sort(clauses.begin(), clauses.end());
  std::ostringstream oss;
  oss << "V:";
  for (unsigned v : active) oss << v << ",";
  oss << " C:";
  for (const auto &c : clauses) {
    oss << "[";
    for (int l : c) oss << l << ",";
    oss << "]";
  }
  return oss.str();
}

// Side-table for verify mode: key.hash -> first-occurrence signature.
// Used to confirm whether canonical-key collisions produce different
// component contents.
static std::unordered_map<uint64_t, string> g_first_sig;

// Dump the current state of a Component to cerr in a readable form:
// active variables, active long clauses (with current literal values),
// and binary clauses between active variables (original-only). Used by
// verify_cache mode when a key-count mismatch is detected.
static void dumpComponentState(
    const Component &comp,
    const vector<LiteralID> &literal_pool,
    const LiteralIndexedVector<Literal> &literals,
    const LiteralIndexedVector<TriValue> &literal_values,
    const vector<ClauseOfs> &clause_id_to_ofs,
    const unordered_map<ClauseOfs, unsigned> &removed_clauses,
    unsigned original_lit_pool_size,
    const char *tag) {
  cerr << "---- " << tag << " ----\n";
  vector<unsigned> active;
  for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++)
    if (literal_values[LiteralID(*it, true)] == X_TRI)
      active.push_back(*it);
  cerr << "  active_vars(" << active.size() << "):";
  for (unsigned v : active) cerr << " " << v;
  cerr << "\n";

  // Long clauses from component (original + learned)
  unsigned n_long = 0, n_learned = 0, n_removed = 0, n_sat_filtered = 0;
  for (auto it = comp.clsBegin(); *it != clsSENTINEL; it++) {
    ClauseOfs ofs = clause_id_to_ofs[*it];
    if (removed_clauses.count(ofs)) { n_removed++; continue; }
    bool satisfied = false;
    vector<int> lits_all;       // including resolved (F_TRI) as-is
    vector<int> lits_active;    // only X_TRI
    for (auto lt = literal_pool.begin() + ofs; *lt != SENTINEL_LIT; lt++) {
      lits_all.push_back(lt->toInt());
      if (literal_values[*lt] == T_TRI) { satisfied = true; break; }
      if (literal_values[*lt] == X_TRI) lits_active.push_back(lt->toInt());
    }
    if (satisfied) { n_sat_filtered++; continue; }
    bool is_learned = (ofs >= original_lit_pool_size);
    if (lits_active.size() < 2) {
      cerr << "  [SKIPPED len<2] C[id=" << *it << ",ofs=" << ofs
           << (is_learned ? " LEARNED" : "") << "]: active:";
      for (int l : lits_active) cerr << " " << l;
      cerr << " | all:";
      for (int l : lits_all) cerr << " " << l;
      cerr << "\n";
      continue;
    }
    cerr << "  C[id=" << *it << ",ofs=" << ofs
         << (is_learned ? " LEARNED" : "") << "]:";
    for (int l : lits_active) cerr << " " << l;
    if (lits_active.size() != lits_all.size()) {
      cerr << "  (full:";
      for (int l : lits_all) cerr << " " << l;
      cerr << ")";
    }
    cerr << "\n";
    if (is_learned) n_learned++;
    n_long++;
  }
  cerr << "  (n_removed_in_comp=" << n_removed << " n_satisfied_in_comp="
       << n_sat_filtered << " n_learned=" << n_learned << ")\n";
  // Binary clauses
  unsigned n_bin = 0;
  std::unordered_set<unsigned> act_set(active.begin(), active.end());
  for (unsigned v : active) {
    for (int sign = 0; sign <= 1; sign++) {
      LiteralID lit(v, sign == 0);
      unsigned orig_count = literals[lit].original_binary_link_count_;
      unsigned idx = 0;
      for (auto bt = literals[lit].binary_links_.begin();
           *bt != SENTINEL_LIT; bt++, idx++) {
        if (idx >= orig_count) break;
        unsigned other = bt->var();
        if (other <= v) continue;
        if (!act_set.count(other)) continue;
        if (literal_values[*bt] == T_TRI) continue;
        if (literal_values[lit] == T_TRI) continue;
        cerr << "  B: " << lit.toInt() << " " << bt->toInt() << "\n";
        n_bin++;
      }
    }
  }
  cerr << "  n_long=" << n_long << " n_bin=" << n_bin << "\n";
}

SOLVER_StateT Solver::countSATRec() {
	// The initial "super component" is what initStack put at component_stack_[1].
	Component &root = comp_manager_.superComponentOf(stack_.top());
	mpz_class result = solveComponent(root, {}, true);
	statistics_.num_long_conflict_clauses_ = num_conflict_clauses();
	// If solveComponent returned because of the time bound, the count is
	// incomplete (any sub-call that hit timeout returned 0, poisoning the
	// product). Don't claim success.
	if (stopwatch_.timeBoundBroken()) {
		statistics_.set_final_solution_count(0);
		return TIMEOUT;
	}
	statistics_.set_final_solution_count(result);
	return SUCCESS;
}

mpz_class Solver::solveComponent(Component &comp,
                                  vector<CutNode> separator,
                                  bool separator_reset) {
	if (stopwatch_.timeBoundBroken())
		return 0;

	static int rec_depth = 0;
	static long long call_count = 0;
	call_count++;
	struct DG { int &d; DG(int &x):d(x){d++;} ~DG(){d--;} } guard(rec_depth);
	if (call_count % 100000 == 0) {
		cerr << "calls=" << call_count << " depth=" << rec_depth << endl;
	}

	// Decompose step: separator exhausted without a reset — factor into components.
	if (!separator_reset && separator.empty()) {
		mpz_class trivial_factor = 1;
		vector<Component*> subcomps = discoverComponentsOf(comp, trivial_factor);
		mpz_class result = trivial_factor;
		for (Component *sub : subcomps) {
			// Content cache lookup
			mpz_class sub_count;
			static const unordered_map<ClauseOfs, unsigned> empty_removed;
			const auto &rm = removed_clauses_;
			CanonicalKey key = buildCanonicalKey(
				*sub, literal_pool_, literals_, literal_values_,
				comp_manager_.getAnalyzer().clauseIdToOfs(), rm,
				original_lit_pool_size_);
			bool hit = (config_.perform_component_caching &&
			            sub->num_variables() >= 3 &&
			            comp_manager_.contentCache().peek(key, sub_count));
			if (hit && !config_.verify_cache) {
				// Normal path: return cached count without recomputing.
				comp_manager_.contentCache().stats_hits++;
				result *= sub_count;
				delete sub;
				continue;
			}
			if (hit && config_.verify_cache) {
				// Compare-on-hit: we have `sub_count` = cached value. Force
				// a fresh recomputation (via recursion) and compare.
				mpz_class cached = sub_count;
				string cur_sig = componentSignature(*sub, literal_pool_, literals_,
				    literal_values_, comp_manager_.getAnalyzer().clauseIdToOfs(),
				    rm, original_lit_pool_size_);
				mpz_class recomputed = solveComponent(*sub, {}, true);
				comp_manager_.contentCache().stats_verify_checks++;
				if (cached != recomputed) {
					auto it = g_first_sig.find(key.hash);
					cerr << "\n*** CACHE_HIT_MISMATCH ***\n"
					     << "  current_sig   = " << cur_sig << "\n";
					if (it != g_first_sig.end()) {
						cerr << "  first_sig     = " << it->second << "\n";
						cerr << "  signatures "
						     << (it->second == cur_sig ? "MATCH (true collision in key-only algo)"
						                                : "DIFFER (canonical key collision)")
						     << "\n";
					} else {
						cerr << "  first_sig     = <not recorded>\n";
					}
					cerr << "  key.hash        = 0x" << std::hex << key.hash << std::dec << "\n"
					     << "  key.num_vars    = " << key.num_vars << "\n"
					     << "  key.num_clauses = " << key.num_clauses << "\n"
					     << "  cached    = " << cached << "\n"
					     << "  recomputed= " << recomputed << "\n"
					     << "  diff      = " << (recomputed - cached) << "\n"
					     << "  removed_clauses (current): " << removed_clauses_.size() << "\n"
					     << "  literal_stack_ size: " << literal_stack_.size() << "\n";
					cerr << "  literal_stack_:";
					for (auto lit : literal_stack_) cerr << " " << lit.toInt();
					cerr << "\n  removed_clauses (content):\n";
					for (auto &p : removed_clauses_) {
						cerr << "    ofs=" << p.first << " cnt=" << p.second << ":";
						for (auto lt = literal_pool_.begin() + p.first; *lt != SENTINEL_LIT; lt++) {
							cerr << " " << lt->toInt();
							const char *tag = (literal_values_[*lt] == T_TRI ? "(T)"
							               : (literal_values_[*lt] == F_TRI ? "(F)" : ""));
							cerr << tag;
						}
						cerr << "\n";
					}
					dumpComponentState(*sub, literal_pool_, literals_, literal_values_,
					                   comp_manager_.getAnalyzer().clauseIdToOfs(), rm,
					                   original_lit_pool_size_,
					                   "SUB-COMPONENT AT CACHE-HIT");
					cerr.flush();
					std::abort();
				}
				comp_manager_.contentCache().stats_verify_ok++;
				result *= cached;
				delete sub;
				continue;
			}
			// Miss — recurse on sub-component
			string pre_sig;
			if (config_.verify_cache && config_.perform_component_caching && sub->num_variables() >= 3) {
				pre_sig = componentSignature(*sub, literal_pool_, literals_,
				    literal_values_, comp_manager_.getAnalyzer().clauseIdToOfs(),
				    rm, original_lit_pool_size_);
			}
			sub_count = solveComponent(*sub, {}, true);
			if (config_.perform_component_caching && sub->num_variables() >= 3) {
				if (config_.verify_cache && g_first_sig.find(key.hash) == g_first_sig.end()) {
					g_first_sig[key.hash] = pre_sig;
				}
				comp_manager_.contentCache().store(key, sub_count);
			}
			result *= sub_count;
			delete sub;
			if (result == 0) break;
		}
		return result;
	}

	// If requested, try to find a separator for this component.
	if (separator_reset) {
		separator_reset = false;
		if (config_.perform_separator_branching &&
		    comp.num_variables() >= config_.separator_min_active_vars) {
			separator = findSeparatorFor(comp);
		}
	}

	// Consume one separator element, then recurse with rest.
	if (!separator.empty()) {
		CutNode el = separator.front();
		vector<CutNode> rest(separator.begin() + 1, separator.end());

		if (el.kind == CutNode::VAR) {
			// Variable element
			if (!isActive(LiteralID(el.id, true))) {
				// Consumed by BCP — skip
				return solveComponent(comp, rest, false);
			}
			// Branch: A = v=true, B = v=false
			LiteralID lit_t(el.id, true), lit_f(el.id, false);
			bool t_first = literal(lit_t).activity_score_ >
			               literal(lit_f).activity_score_;
			mpz_class A = branchOnLiteral(t_first ? lit_t : lit_f,
			                              comp, rest, false);
			mpz_class B = branchOnLiteral(t_first ? lit_f : lit_t,
			                              comp, rest, false);
			return A + B;
		} else {
			// Clause element
			if (isClauseRemoved(el.id) || isSatisfied(el.id)) {
				return solveComponent(comp, rest, false);
			}
			mpz_class A = branchOnClause(el.id, comp, rest, false, false);
			mpz_class B = branchOnClause(el.id, comp, rest, false, true);
			return A - B;
		}
	}

	// No separator — regular variable branching within comp.
	VariableIndex v = pickBranchVariable(comp);
	if (v == 0) {
		// No active variable in comp — this shouldn't happen if comp was
		// non-trivial at entry, but handle defensively.
		return 1;
	}
	LiteralID lit_t(v, true), lit_f(v, false);
	bool t_first = literal(lit_t).activity_score_ >
	               literal(lit_f).activity_score_;
	mpz_class A = branchOnLiteral(t_first ? lit_t : lit_f, comp, {}, false);
	mpz_class B = branchOnLiteral(t_first ? lit_f : lit_t, comp, {}, false);
	return A + B;
}

mpz_class Solver::branchOnLiteral(LiteralID lit,
                                   Component &comp,
                                   vector<CutNode> separator,
                                   bool separator_reset) {
	unsigned lit_save = literal_stack_.size();
	statistics_.num_decisions_++;
	if (statistics_.num_decisions_ % 128 == 0)
		decayActivities();

	// Check if literal is already assigned
	if (!isActive(lit)) {
		if (literal_values_[lit] == F_TRI) return 0;
		return solveComponent(comp, separator, separator_reset);
	}

	// Push a placeholder StackLevel BEFORE setLiteralIfFree so:
	//   - setLiteralIfFree records decision_level = this level (not 0)
	//   - recordLastUIPCauses can identify "current dl" on conflict
	// We pop at the end regardless of outcome.
	stack_.push_back(StackLevel(1, lit_save,
	                            comp_manager_.component_stack_size()));

	setLiteralIfFree(lit);

	bool bcp_ok = BCP(lit_save);

	mpz_class result;
	if (!bcp_ok) {
		statistics_.num_conflicts_++;
		// Scoped conflict clause learning.
		// The learned clause D is derived from the current removed set R.
		// BCP will only use D in contexts where scope(D) = R ⊆ current
		// removed set — keeping D sound across sibling branches and
		// compatible with content caching (for in-scope contexts the
		// cached count #SAT(F∖R ∧ D) = #SAT(F∖R) because F∖R ⊨ D).
		//
		// Order matters: conflict analysis walks literal_stack_ backward
		// via antecedents, so it must run BEFORE we truncate the stack.
		recordLastUIPCauses();
		if (!uip_clauses_.empty() && uip_clauses_.back().size() >= 3
		    && uip_clauses_.back().front() == lit.neg()) {
			addScopedUIPConflictClause(uip_clauses_.back());
		}
		result = 0;
	} else {
		result = solveComponent(comp, separator, separator_reset);
	}

	while (literal_stack_.size() > lit_save) {
		unSet(literal_stack_.back());
		literal_stack_.pop_back();
	}
	stack_.pop_back();
	return result;
}

mpz_class Solver::branchOnClause(ClauseOfs cl_ofs,
                                  Component &comp,
                                  vector<CutNode> separator,
                                  bool separator_reset,
                                  bool negate_literals) {
	unsigned lit_save = literal_stack_.size();
	statistics_.num_decisions_++;

	// Push a StackLevel so the literals set below (by clause removal or
	// clause negation) get decision_level >= 1 rather than 0.
	//
	// Why this matters: the CDCL conflict analyzer (recordLastUIPCauses)
	// silently IGNORES DL=0 literals — treating them as globally-fixed
	// unit-propagated assumptions. That's correct for original unit
	// clauses but WRONG for the negated-clause literals we set in
	// branch 2 of clause branching: those are temporary and may not hold
	// in sibling/ancestor contexts. By putting them at DL>=1, conflict
	// analysis handles them correctly (their negations end up in the
	// learned clause when they contribute to a conflict).
	stack_.push_back(StackLevel(1, lit_save,
	                            comp_manager_.component_stack_size()));

	// Mark clause as removed
	markClauseRemoved(cl_ofs);

	bool conflict = false;
	if (negate_literals) {
		for (auto it = beginOf(cl_ofs); *it != SENTINEL_LIT; it++) {
			if (isSatisfied(*it)) {
				conflict = true;
				break;
			}
			if (isActive(*it)) {
				setLiteralIfFree(it->neg());
			}
		}
	}

	mpz_class result;
	if (conflict) {
		result = 0;
	} else {
		bool bcp_ok = BCP(lit_save);
		if (!bcp_ok) {
			statistics_.num_conflicts_++;
			// We deliberately DON'T do clause learning here. Branch 2 of a
			// clause branch can assign multiple literals (the negations of
			// the clause's literals), each marked as a "decision literal".
			// Standard CDCL conflict analysis assumes one decision per
			// decision level, so it fails on this multi-decision setup.
			result = 0;
		} else {
			result = solveComponent(comp, separator, separator_reset);
		}
	}

	while (literal_stack_.size() > lit_save) {
		unSet(literal_stack_.back());
		literal_stack_.pop_back();
	}
	unmarkClauseRemoved(cl_ofs);
	stack_.pop_back();
	return result;
}

vector<CutNode> Solver::findSeparatorFor(Component &comp) {
	vector<CutNode> result;
	if (tryInstallSeparator(comp)) {
		// tryInstallSeparator writes to separator_elements_ and sets
		// separator_base_dl_. Move the elements out and reset globals.
		result = std::move(separator_elements_);
		separator_elements_.clear();
		separator_used_.clear();
		separator_base_dl_ = -1;
	}
	return result;
}

vector<Component*> Solver::discoverComponentsOf(Component &super_comp,
                                                 mpz_class &trivial_factor) {
	vector<Component*> result;
	// Use the component analyzer directly. It needs a StackLevel for its
	// setupAnalysisContext call to accumulate trivial component factor (via
	// includeSolution(2) on the StackLevel). We use a temporary StackLevel.
	// After discovery, we read its model count to get the trivial factor.
	AltComponentAnalyzer &ana = comp_manager_.getAnalyzer();

	unsigned saved_comp_stack_size = comp_manager_.component_stack_size();

	// Create a temporary StackLevel for the analyzer. Its super_component_
	// field is not used by the analyzer itself (we pass super_comp directly);
	// the field is just for includeSolution(2) accumulation on trivial comps.
	StackLevel tmp(1, literal_stack_.size(), saved_comp_stack_size);
	tmp.changeBranch();  // neutral branch; we read branch_model_count_[1]
	// Ensure initial model count is 1 so includeSolution multiplies cleanly.
	tmp.includeSolution(1);

	ana.setupAnalysisContext(tmp, super_comp);

	for (auto vt = super_comp.varsBegin(); *vt != varsSENTINEL; vt++) {
		if (ana.isUnseenAndActive(*vt)) {
			if (ana.exploreRemainingCompOf(*vt)) {
				Component *new_comp = ana.makeComponentFromArcheType();
				result.push_back(new_comp);
			}
			// trivial components are counted via includeSolution(2) on tmp
		}
	}

	trivial_factor = tmp.getTotalModelCount();
	if (trivial_factor < 0) trivial_factor = 1;  // safety

	// Don't leave newly-created components on comp_manager_.component_stack_;
	// they were pushed by makeComponentFromArcheType. We own them now.
	// Truncate component_stack_ back to its saved size, but skip deleting
	// components we've moved to `result` (they're referenced by their
	// indices in component_stack_). Actually, makeComponentFromArcheType
	// does NOT push to component_stack_ - it just constructs a Component*.
	// So no cleanup needed.
	(void)saved_comp_stack_size;

	return result;
}

VariableIndex Solver::pickBranchVariable(Component &comp) {
	VariableIndex best = 0;
	float best_score = -1.0f;
	for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++) {
		if (!isActive(LiteralID(*it, true))) continue;
		float s = scoreOf(*it);
		if (s > best_score) {
			best_score = s;
			best = *it;
		}
	}
	return best;
}

