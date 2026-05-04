#include <fstream>

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

#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using namespace std;

// Walk antecedent chain backward from l_star, collecting decision
// literals (ante == NOT_A_CLAUSE, DL > 0) visited during the walk.
// Returns empty vector if the collected set would exceed max_size
// (bail-early signal: "too big, skip learning").
std::vector<LiteralID> Solver::deriveDecisionImplicant(
    LiteralID l_star, unsigned max_size, unsigned *out_chain_depth)
{
  // Guard: the walk starts from a currently-true literal. If l_star
  // isn't true, the caller invoked us with a stale/unassigned literal.
  assert(literal_values_[l_star] == T_TRI
         && "deriveDecisionImplicant: l_star must be currently true");

  unsigned chain_depth = 0;
  std::vector<LiteralID> impl;
  std::unordered_set<unsigned> seen;
  std::queue<LiteralID> frontier;
  frontier.push(l_star);
  seen.insert(l_star.var());

  while (!frontier.empty()) {
    LiteralID l = frontier.front();
    frontier.pop();
    // Guard A: every frontier literal must be currently true. The walk
    // traces currently-true literals back through their antecedents; a
    // currently-false literal in the frontier means the push site got
    // the polarity wrong (the binary-antecedent bug we already caught
    // looked exactly like this: wrong polarity produced F_TRI literals
    // in the frontier, leading to unsound implicants and undercounts).
    assert(literal_values_[l] == T_TRI
           && "frontier literal must be currently true");

    int dl = var(l).decision_level;
    if (dl <= 0) continue;  // baseline (DL 0) or not-yet-assigned — skip
    Antecedent ante = var(l).ante;
    if (!ante.isAnt()) {
      // Decision. Collect in the implicant frontier.
      if (impl.size() >= max_size) return {};  // too big — bail
      // Guard B: decision literals we collect into the implicant must
      // be currently true, otherwise the resulting clause would have
      // flipped polarities.
      assert(literal_values_[l] == T_TRI
             && "implicant decision literal must be currently true");
      impl.push_back(l);
      continue;
    }
    // Forced literal — count this expansion step.
    chain_depth++;
    if (ante.isAClause()) {
      ClauseOfs ofs = ante.asCl();
      for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++) {
        if (lt->var() == l.var()) continue;
        if (!seen.insert(lt->var()).second) continue;
        // Stored clause contains `lt` (literal). For this clause to
        // have forced `l`, `lt` is falsified → its negation is the
        // currently-true trigger to walk back through.
        // Guard: BCP invariant — non-forced literals of the antecedent
        // clause must be falsified at firing time (and still now, since
        // we haven't backtracked since the firing).
        assert(literal_values_[*lt] == F_TRI
               && "antecedent clause's non-forced literal must be F_TRI");
        frontier.push(lt->neg());
      }
    } else {
      // Binary antecedent: ante.asLit() returns the falsified partner
      // literal (see BCP at solver.cpp:285 — setLiteralIfFree(*bt,
      // Antecedent(unLit)) where unLit is the currently-falsified
      // literal, the "other" half of the binary clause). To walk back
      // we need the currently-TRUE literal: that's ante.asLit().neg().
      LiteralID other_false = ante.asLit();
      assert(literal_values_[other_false] == F_TRI
             && "binary antecedent's stored literal must be F_TRI");
      LiteralID other_true = other_false.neg();
      if (seen.insert(other_true.var()).second) frontier.push(other_true);
    }
  }
  if (out_chain_depth) *out_chain_depth = chain_depth;
  return impl;
}

void Solver::maybeLearnImplicants(unsigned bcp_start_ofs)
{
  if (!config_.perform_implicant_learning) return;
  if (statistics_.num_implicants_learned_
        >= config_.implicant_max_total) {
    // Hard cap reached — count the event once (not every call) is fine:
    // stats_implicants_quota_stop_ tracks how many literals we skipped.
    return;
  }

  // Iterate newly-forced literals from this BCP.
  for (unsigned i = bcp_start_ofs + 1; i < literal_stack_.size(); i++) {
    LiteralID l = literal_stack_[i];
    // Must have an antecedent — pure decisions (not forced) are not
    // mine-able as implicants (their "implicant" is themselves).
    if (!var(l).ante.isAnt()) continue;

    // Fast depth-filter: chain_depth is maintained incrementally in
    // setLiteralIfFree. If it's below min_chain_depth, we know the
    // walk would fail the filter anyway — skip the walk entirely.
    // This is the big win vs. the previous "walk everything, then
    // filter" approach: on bench_D with min_chain=8 we performed
    // ~15M walks to keep ~440 clauses; with this fast-skip we walk
    // only the ~440.
    if ((unsigned)var(l).chain_depth < config_.implicant_min_chain_depth) {
      statistics_.num_implicants_depth_dropped_++;
      continue;
    }

    unsigned chain_depth = 0;
    auto impl = deriveDecisionImplicant(l, config_.implicant_max_size, &chain_depth);
    if (impl.empty()) {
      statistics_.num_implicants_size_dropped_++;
      continue;
    }
    // Debug sanity: the cached depth should match the walk's count.
    // If they disagree, chain-depth maintenance is broken somewhere
    // (missed update in setLiteralIfFree / unSet).
    assert(chain_depth == (unsigned)var(l).chain_depth
           && "cached chain_depth must match walk-computed depth");
    // Skip size==1 (singleton implicants): a single decision that forces
    // `l` means we'd learn a binary (¬d v l). This IS valuable — the
    // guard-padding mechanism handles it. Keep.

    // Non-trivial filter: if the derived clause's literal set equals
    // the antecedent clause's literal set, we'd be re-storing an
    // existing clause. Cheap check: compare sizes + content. The
    // derived clause has `impl.size() + 1` literals; antecedent has
    // whatever it has. If sizes match AND every impl literal appears
    // (negated) in the antecedent, it's trivial.
    bool trivial = false;
    if (var(l).ante.isAClause()) {
      ClauseOfs ofs = var(l).ante.asCl();
      unsigned ante_len = 0;
      for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++) ante_len++;
      if (ante_len == impl.size() + 1) {
        // Potential trivial case. Verify each impl literal's negation
        // appears in the antecedent.
        std::unordered_set<uint32_t> ante_raws;
        for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++)
          ante_raws.insert(lt->raw());
        bool all_match = true;
        for (auto d : impl) {
          if (!ante_raws.count(d.neg().raw())) { all_match = false; break; }
        }
        if (all_match && ante_raws.count(l.raw())) trivial = true;
      }
    }
    if (trivial) {
      statistics_.num_implicants_trivial_dropped_++;
      continue;
    }

    // Build the learned clause: ¬d_1 ∨ ... ∨ ¬d_k ∨ l. First literal
    // must be l (addScopedUIPConflictClause / addClause assume front
    // is the UIP-asserting literal in the watching scheme).
    std::vector<LiteralID> clause;
    clause.reserve(impl.size() + 1);
    clause.push_back(l);
    for (auto d : impl) clause.push_back(d.neg());

    // Guard C: at store time the clause must satisfy the BCP invariant
    // for a conflict-driven learned clause: clause[0] is the asserting
    // literal (currently TRUE, since l* was just forced by BCP), and
    // every other literal is currently FALSE (because each d_i is a
    // currently-true decision, so ¬d_i is currently false). If any
    // "other" literal is TRUE, the polarity was flipped somewhere in
    // the walk — this is exactly the class of bug we hit with binary
    // antecedents before the fix. Runtime undercount ensues.
    //
    // Runtime (not just debug) check: implicant learning is opt-in via
    // -implicantLearn, so paying a small O(k) check per learned clause
    // is acceptable. The silent failure mode (undercount) is too bad
    // to miss in Release.
    if (literal_values_[clause[0]] != T_TRI) {
      std::cerr << "\n*** UNSOUND_IMPLICANT: asserting literal not true ***\n"
                << "  clause[0]=" << clause[0].toInt()
                << " value=" << (int)literal_values_[clause[0]]
                << " (expected T_TRI=" << (int)T_TRI << ")\n";
      std::cerr.flush();
      std::abort();
    }
    for (size_t ci = 1; ci < clause.size(); ci++) {
      if (literal_values_[clause[ci]] != F_TRI) {
        std::cerr << "\n*** UNSOUND_IMPLICANT: non-asserting literal not false ***\n"
                  << "  clause[" << ci << "]=" << clause[ci].toInt()
                  << " value=" << (int)literal_values_[clause[ci]]
                  << " (expected F_TRI=" << (int)F_TRI << ")\n"
                  << "  full clause:";
        for (auto lx : clause) std::cerr << " " << lx.toInt();
        std::cerr << "\n";
        std::cerr.flush();
        std::abort();
      }
    }

    // Dedup via shared Bloom filter (Instance::learned_clause_sig_ via
    // maybeDedupClause). Replaces the old 4096-entry LRU that was
    // letting ~50% of duplicates through on long solves.
    if (!maybeDedupClause(clause)) {
      statistics_.num_implicants_dedup_dropped_++;
      statistics_.num_learned_dedup_dropped_++;
      continue;
    }

    // Learn via the scoped-UIP machinery: automatic scope tagging,
    // guard-padding for size==2, correct behaviour under clause
    // branching. Dry-run mode skips the actual store so we can
    // measure walk+filter overhead in isolation.
    if (!config_.implicant_dry_run) {
      const int L = config_.learn_level;
      Antecedent a = addScopedUIPConflictClause(
          clause,
          /*pad_binary=*/  L >= 4,
          /*record_scope=*/L >= 3);
      if (a.isAnt() && a.isAClause())
        logLearnTrace(a.asCl(), clause);
    }
    statistics_.num_implicants_learned_++;
    if (statistics_.num_implicants_learned_
          >= config_.implicant_max_total) {
      statistics_.num_implicants_quota_stop_ = 1;
      break;
    }
  }
}

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
	// Build precomputed ND hierarchy if separator branching is enabled.
	if (config_.perform_separator_branching && !nd_hierarchy_.valid) {
		// Collect long clauses: (clause_ofs, list of variable IDs).
		// We drop clauses that are already satisfied/removed from the
		// metis input to keep the graph snug, but MUST include every
		// active clause that carries connectivity.
		vector<pair<unsigned, vector<unsigned>>> clause_list;
		Component &root_comp = comp_manager_.superComponentOf(stack_.top());
		for (auto it = root_comp.clsBegin(); *it != varsSENTINEL; it++) {
			ClauseOfs ofs = comp_manager_.clauseOfsOf(*it);
			if (ofs >= original_lit_pool_size_) continue;
			vector<unsigned> vars;
			for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++)
				vars.push_back(lt->var());
			clause_list.push_back({ofs, vars});
		}
		// Binary clauses: collected as (var_a, var_b) pairs. Each binary
		// is physically stored once per endpoint in binary_links_; we
		// deduplicate here by only emitting when the lower-raw endpoint
		// owns the pair. The METIS input graph will render these as
		// direct var-var edges (see nd_hierarchy.cpp).
		vector<pair<unsigned, unsigned>> binary_pairs;
		for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
			const auto &blinks = literal(l).binary_links_;
			for (auto bt = blinks.begin(); *bt != SENTINEL_LIT; ++bt) {
				if (l.raw() < bt->raw()) {
					// Each binary clause connects two variables; we only
					// care about variable connectivity for METIS.
					binary_pairs.push_back({(unsigned)l.var(),
					                         (unsigned)bt->var()});
				}
			}
		}
		// Exclude the guard variable from the hierarchy input: it has no
		// clauses, so it contributes an isolated vertex that shifts
		// METIS's balance and produces a structurally different
		// decomposition. The guard's var_id is past n_vars; mapToChild
		// already skips vars with id >= var_leaf.size().
		int build_n_vars = (guard_var_ > 0)
		                     ? (int)(guard_var_ - 1)
		                     : (int)num_variables();
		nd_hierarchy_.build(build_n_vars, clause_list, binary_pairs);
	}

	// Diagnostic: dump ND-hierarchy state and exit before search.
	if (!config_.dump_nd_and_exit_path.empty()) {
		std::ofstream out(config_.dump_nd_and_exit_path);
		if (out) {
			out << "# ND hierarchy dump\n";
			out << "valid " << nd_hierarchy_.valid << "\n";
			out << "n_nodes " << nd_hierarchy_.n_nodes << "\n";
			out << "npes " << nd_hierarchy_.npes << "\n";
			out << "root " << nd_hierarchy_.root() << "\n";
			for (int i = 0; i < nd_hierarchy_.n_nodes; i++) {
				out << "node " << i
				    << " lc=" << nd_hierarchy_.left_child[i]
				    << " rc=" << nd_hierarchy_.right_child[i]
				    << " leaf_lo=" << nd_hierarchy_.leaf_lo[i]
				    << " leaf_hi=" << nd_hierarchy_.leaf_hi[i]
				    << " sep_size=" << nd_hierarchy_.separator[i].size()
				    << " sep=[";
				for (size_t j = 0; j < nd_hierarchy_.separator[i].size(); ++j) {
					const auto &cn = nd_hierarchy_.separator[i][j];
					if (j) out << ",";
					out << (cn.kind == CutNode::VAR ? "V" : "C") << cn.id;
				}
				out << "]\n";
			}
			// Compact var IDs → leaf
			for (size_t v = 1; v < nd_hierarchy_.var_leaf.size(); v++) {
				unsigned orig = (v < compact_to_orig_.size() && !compact_to_orig_.empty())
				                ? compact_to_orig_[v] : (unsigned)v;
				out << "var_leaf compact=" << v
				    << " orig=" << orig
				    << " leaf=" << nd_hierarchy_.var_leaf[v]
				    << "\n";
			}
			// Clause leaves
			for (auto &p : nd_hierarchy_.clause_leaf) {
				out << "clause_leaf ofs=" << p.first << " leaf=" << p.second << "\n";
			}
			// Every active binary in binary_links_ (compacted)
			for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
				const auto &bl = literal(l).binary_links_;
				for (auto bt = bl.begin(); *bt != SENTINEL_LIT; ++bt) {
					if (!(l.raw() < bt->raw())) continue;  // dedup
					unsigned a_v = l.var(), b_v = bt->var();
					unsigned a_orig = (a_v < compact_to_orig_.size() && !compact_to_orig_.empty())
					                    ? compact_to_orig_[a_v] : a_v;
					unsigned b_orig = (b_v < compact_to_orig_.size() && !compact_to_orig_.empty())
					                    ? compact_to_orig_[b_v] : b_v;
					int a_leaf = (a_v < nd_hierarchy_.var_leaf.size())
					               ? nd_hierarchy_.var_leaf[a_v] : -1;
					int b_leaf = (b_v < nd_hierarchy_.var_leaf.size())
					               ? nd_hierarchy_.var_leaf[b_v] : -1;
					out << "binary compact=" << l.toInt() << "," << bt->toInt()
					    << " orig=" << (l.sign() ? (int)a_orig : -(int)a_orig)
					    << "," << (bt->sign() ? (int)b_orig : -(int)b_orig)
					    << " leaves=" << a_leaf << "," << b_leaf << "\n";
				}
			}
			// Every active original long clause in the root comp
			Component &rc = comp_manager_.superComponentOf(stack_.top());
			for (auto it = rc.clsBegin(); *it != varsSENTINEL; it++) {
				ClauseOfs ofs = comp_manager_.clauseOfsOf(*it);
				if (ofs >= original_lit_pool_size_) continue;
				out << "clause ofs=" << ofs << " lits=";
				for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++) {
					unsigned v = lt->var();
					unsigned orig = (v < compact_to_orig_.size() && !compact_to_orig_.empty())
					                  ? compact_to_orig_[v] : v;
					int signed_orig = lt->sign() ? (int)orig : -(int)orig;
					int leaf = (v < nd_hierarchy_.var_leaf.size())
					             ? nd_hierarchy_.var_leaf[v] : -1;
					out << signed_orig << "(leaf=" << leaf << "),";
				}
				int cleaf = nd_hierarchy_.clause_leaf.count(ofs)
				              ? nd_hierarchy_.clause_leaf.at(ofs) : -1;
				out << " clause_leaf=" << cleaf << "\n";
			}
			std::cerr << "ND hierarchy dumped to " << config_.dump_nd_and_exit_path << "\n";
		}
		std::exit(0);
	}

	Component &root = comp_manager_.superComponentOf(stack_.top());
	int start_node = nd_hierarchy_.valid ? nd_hierarchy_.root() : -1;
	mpz_class result = solveComponent(root, {}, true, 0, start_node);
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

// Wrapper: function-boundary memoization.
//
// Caching is a property of #SAT(active residual), independent of how the
// search arrived here. We look up at entry, run the body on miss, store
// on return. The cache stores STRUCTURAL counts (excluding free-var
// factor); we divide by 2^free_vars before storing and multiply on
// retrieval. The body (`solveComponentImpl`) is the previous
// solveComponent implementation; its internal cache lookups in the
// post-consumption decompose-block are now redundant and are removed
// in this commit.
mpz_class Solver::solveComponent(Component &comp,
                                  vector<CutNode> separator,
                                  bool separator_reset,
                                  int depth,
                                  int nd_node,
                                  int reactive_metis_skip_until_depth) {
	if (stopwatch_.timeBoundBroken())
		return 0;

	bool can_cache = config_.perform_component_caching
	                 && comp.num_variables() >= 3;
	CanonicalKey cached_key;
	unsigned free_vars = 0;
	bool key_built = false;
	if (can_cache) {
		const auto &rm = removed_clauses_;
		cached_key = buildCanonicalKey(
		    comp, literal_pool_, literals_, literal_values_,
		    comp_manager_.getAnalyzer().clauseIdToOfs(), rm,
		    original_lit_pool_size_, config_.canonical_compact,
		    config_.no_anonymization, config_.wl_iterations,
		    static_wl_labels_.empty() ? nullptr : &static_wl_labels_);
		key_built = true;
		free_vars = (cached_key.num_vars > cached_key.n_in_clauses)
		    ? (cached_key.num_vars - cached_key.n_in_clauses) : 0;
		mpz_class hit;
		if (comp_manager_.contentCache().peek(cached_key, hit)) {
			mpz_class scaled = hit;
			for (unsigned i = 0; i < free_vars; ++i) scaled *= 2;
			return scaled;
		}
	}

	mpz_class result = solveComponentImpl(
	    comp, std::move(separator), separator_reset, depth, nd_node,
	    reactive_metis_skip_until_depth);

	if (can_cache && key_built) {
		mpz_class structural = result;
		for (unsigned i = 0; i < free_vars; ++i)
			mpz_fdiv_q_2exp(structural.get_mpz_t(),
			                structural.get_mpz_t(), 1);
		comp_manager_.contentCache().store(cached_key, structural);
	}
	return result;
}

mpz_class Solver::solveComponentImpl(Component &comp,
                                  vector<CutNode> separator,
                                  bool separator_reset,
                                  int depth,
                                  int nd_node,
                                  int reactive_metis_skip_until_depth) {
	if (stopwatch_.timeBoundBroken())
		return 0;

	// Top-branch CNF dump: at every solveComponent entry where
	// depth <= dump_recursion_max_depth, snapshot the SUPER-comp
	// formula state and write to disk. Manifest at log.txt records
	// (id, depth, nvars, path). Diagnostic tool: re-run ganak/our-solver
	// on each dump to find the smallest sub-formula where counts
	// diverge.
	if (!config_.dump_recursion_dir.empty()
	    && (unsigned)depth <= config_.dump_recursion_max_depth) {
		static long long rec_dump_id = 0;
		long long my_id = rec_dump_id++;
		std::string path = config_.dump_recursion_dir
		                 + "/super_d" + std::to_string(depth)
		                 + "_id" + std::to_string(my_id) + ".cnf";
		unsigned dump_n = 0;
		if (dumpSubComponentCnf(comp, path, &dump_n)) {
			std::ofstream log(config_.dump_recursion_dir + "/log.txt",
			                  std::ios::app);
			log << "id=" << my_id
			    << " depth=" << depth
			    << " nvars=" << dump_n
			    << " removed_clauses_size=" << removed_clauses_.size()
			    << " trail_size=" << literal_stack_.size()
			    << " path=" << path << "\n";
		}
	}

	static int rec_depth = 0;
	static long long call_count = 0;
	call_count++;
	struct DG { int &d; DG(int &x):d(x){d++;} ~DG(){d--;} } guard(rec_depth);
	if (call_count % 100000 == 0) {
		cerr << "calls=" << call_count << " depth=" << rec_depth << endl;
	}

	// Per-component BCP filter mask is updated only when decomposition
	// produces a strictly smaller sub-component than its parent — see
	// the SubVarsetGuard wrapping `solveComponent(*sub, ...)` below in
	// the decomposition loop. solveComponent's entry is INTENTIONALLY
	// no-op here: branching paths (separator/clause/lit) recurse with
	// the same comp.varsBegin, so the filter shouldn't change.

	// (Diagnostic guard `verifyUnitPropagationSaturated` was tested at
	// every solveComponent entry on /tmp/t1_011_rev.cnf, 2026-04-22.
	// It DID fire — BCP saturation is not maintained on learned clauses
	// after chronological backtracking when their pos-1 watcher is
	// already F_TRI. However, the count produced by the solver is
	// identical with and without acting on those firings: the analyzer
	// treats the unforced var as having two polarities, BCP catches the
	// bad polarity via watch on the asserting lit, and the sum is
	// correct. So the unsaturated state is an efficiency loss only,
	// not a soundness defect. Guard removed to avoid per-call cost.)

	// Mid-consumption decomposition. When `separator` is non-empty
	// (we're consuming an ancestor's separator), check whether BCP
	// has actually disconnected the residual into ≥ 2 connected
	// sub-components. If so, recurse on each sub-comp independently
	// with its own filtered or recomputed separator. Always-on as of
	// stage A; the previous opt-in flag was redundant since this
	// path is sound regardless and the cost is bounded by the
	// already-amortized BCP work.
	// Mid-consumption decompose: fire when explicitly enabled OR
	// when the unified picker is on. Under -unifiedPicker the
	// consumption block is bypassed and `separator` stays non-empty
	// as an immutable hint, so the post-consumption decompose-block
	// (which gates on `separator.empty()`) never fires. Without
	// this path, decomposition is never detected and the search
	// explodes on instances where the formula disconnects via BCP.
	// Throttled by decompose_after_k decisions in either case.
	if ((config_.decompose_in_separator || config_.unified_picker)
	    && !separator.empty() && !separator_reset
	    && decisions_since_connectivity_check_ >= config_.decompose_after_k) {
		mid_sep_decomp_attempts_++;
		decisions_since_connectivity_check_ = 0;
		mpz_class trivial_factor = 1;
		vector<Component*> subcomps = discoverComponentsOf(comp, trivial_factor);
		// Real split = ≥ 2 connected components. Peeled-only (1 sub +
		// trivial_factor > 1) defers to the existing post-consumption
		// decompose-block; the same canonical residual will be
		// reached when the separator naturally exhausts.
		if (subcomps.size() <= 1) {
			for (Component *s : subcomps) delete s;
			// Fall through to existing flow.
		} else {
			mid_sep_decomp_splits_++;
			mpz_class result = trivial_factor;
			if (config_.verbose) {
				std::cerr << "  MIDSEP_DECOMP depth=" << depth
				          << " subs=" << subcomps.size()
				          << " trivial=" << trivial_factor
				          << " parent_sep=" << separator.size() << std::endl;
			}
			for (size_t i = 0; i < subcomps.size(); ++i) {
				Component *sub = subcomps[i];
				// Build var/clause membership sets for filtering.
				std::unordered_set<unsigned> sub_vars;
				for (auto it = sub->varsBegin(); *it != varsSENTINEL; ++it)
					sub_vars.insert(*it);
				std::unordered_set<unsigned> sub_clauses;
				for (auto it = sub->clsBegin(); *it != clsSENTINEL; ++it) {
					ClauseOfs ofs = comp_manager_.clauseOfsOf(*it);
					sub_clauses.insert(ofs);
				}
				// Step 3a: filter parent's separator to this sub-comp.
				vector<CutNode> filtered;
				filtered.reserve(separator.size());
				for (const auto &nd : separator) {
					if (nd.kind == CutNode::VAR) {
						if (sub_vars.count(nd.id)) filtered.push_back(nd);
					} else {
						if (sub_clauses.count(nd.id)) filtered.push_back(nd);
					}
				}
				// Step 3b: evaluate filtered separator quality.
				vector<CutNode> chosen;
				if (!filtered.empty()
				    && hierarchySeparatorAcceptable(nd_node, *sub,
				                                    (unsigned)filtered.size())) {
					chosen = std::move(filtered);
				}
				// Step 3d: rejected/empty → reactive METIS (Gate 1 only).
				if (chosen.empty() && config_.use_reactive_metis) {
					std::vector<unsigned> mv;
					std::vector<std::pair<unsigned, std::vector<unsigned>>> mc;
					std::vector<std::pair<unsigned, unsigned>> mp;
					buildMetisInputFromComponent(*sub, mv, mc, mp);
					RuntimeSeparatorResult r = computeRuntimeMetisSeparator(mv, mc, mp);
					reactive_metis_calls_++;
					if (!r.ok) reactive_metis_failed_++;
					if (r.ok) {
						unsigned n_quick = 0;
						for (auto it = sub->varsBegin();
						     *it != varsSENTINEL; ++it)
							if (isActive(LiteralID(*it, true))) n_quick++;
						bool size_ok = separatorSizeAcceptable(
						    (unsigned)r.separator.size(), n_quick);
						unsigned Ltot = r.left_vars + r.right_vars;
						double balance = (Ltot > 0)
						    ? (double)std::min(r.left_vars, r.right_vars)
						      / (double)Ltot
						    : 0.0;
						if (size_ok
						    && balance >= config_.separator_min_balance) {
							chosen = std::move(r.separator);
							reactive_metis_accepted_++;
						} else {
							reactive_metis_gate1_rej_++;
						}
					}
				}
				// Step 3e: recurse. Two paths depending on whether the
				// parent's separator still applies to this sub-comp:
				//   - filtered/chosen non-empty → parent's separator
				//     still has elements relevant here. Inherit it
				//     (separator_reset=false) and keep parent's nd_node.
				//   - chosen is empty (parent's separator was exhausted
				//     for this sub-comp, AND reactive METIS didn't
				//     produce one) → sub-comp's structural cut is at
				//     the child ND-node, not the parent. Recurse with
				//     separator_reset=true and child_nd_node so the
				//     sub-comp's solveComponent fires fresh acceptance
				//     and looks up THIS sub's nd_node separator.
				int sub_nd_node;
				bool sub_reset;
				vector<CutNode> sub_sep;
				if (!chosen.empty()) {
					sub_nd_node = nd_node;
					sub_reset   = false;
					sub_sep     = std::move(chosen);
				} else {
					int mt = -1;
					if (nd_node >= 0 && nd_hierarchy_.valid) {
						std::vector<unsigned> sub_active_vars;
						for (auto it = sub->varsBegin();
						     *it != varsSENTINEL; ++it)
							if (isActive(LiteralID(*it, true)))
								sub_active_vars.push_back(*it);
						mt = nd_hierarchy_.mapToChild(nd_node, sub_active_vars);
						if (mt == -2) mt = -1;
					}
					sub_nd_node = mt;
					sub_reset   = true;
					// sub_sep stays empty; acceptance at sub_nd_node
					// will populate it.
				}
				mpz_class sub_count = solveComponent(
				    *sub, std::move(sub_sep), sub_reset,
				    depth + 1, sub_nd_node,
				    reactive_metis_skip_until_depth);
				result *= sub_count;
				delete sub;
				if (result == 0) {
					// Free remaining sub-comps before returning.
					for (size_t j = i + 1; j < subcomps.size(); ++j)
						delete subcomps[j];
					return 0;
				}
			}
			return result;
		}
	}

	// Decompose step: factor the super-component into its connected components.
	// Enter this path in two situations:
	//   (a) separator exhausted without a reset — the classical path after a
	//       sequence of separator-branch consumptions.
	//   (b) we're at a pass-through ND-hierarchy node. A pass-through tells us
	//       this region was disconnected at build time and has no separator to
	//       branch on. Falling through to variable branching would burn a
	//       decision level before `discoverComponentsOf` catches the
	//       disconnection in the sub-call — wasteful given we already know.
	bool at_passthrough = (nd_node >= 0 && nd_hierarchy_.valid
	                       && nd_hierarchy_.isPassthrough(nd_node));
	// "Separator exhausted" gate. Under baseline (and `-sepVarBias`)
	// the consumption loop pops elements until the carried `separator`
	// is empty, so `separator.empty()` is the right signal. Under
	// `-unifiedPicker` we don't pop; instead an element is "consumed"
	// when its var becomes inactive (set on trail) or its clause is
	// removed/satisfied. So the gate is generalized: fire when no
	// element of the carried separator is still active. Both modes
	// reduce to the same condition when the picker is off.
	bool sep_exhausted = true;
	for (const auto &nd : separator) {
		if (nd.kind == CutNode::VAR) {
			if (isActive(LiteralID(nd.id, true))) { sep_exhausted = false; break; }
		} else {
			if (!isClauseRemoved(nd.id) && !isSatisfied(nd.id)) {
				sep_exhausted = false; break;
			}
		}
	}
	if ((!separator_reset && sep_exhausted) || at_passthrough) {
		// Consume the reset flag so the branches below don't re-enter the
		// separator-lookup block for this same node.
		separator_reset = false;
		// Reset the mid-consumption throttle: we're about to do a
		// fresh connectivity check now anyway, so future calls should
		// re-accumulate from 0.
		decisions_since_connectivity_check_ = 0;
		mpz_class trivial_factor = 1;
		vector<Component*> subcomps = discoverComponentsOf(comp, trivial_factor);
		// Fast path: no real decomposition (1 sub identical to comp,
		// no peeled free vars). Recursing here would lose the parent's
		// hierarchy lineage — child_nd_node would be -1 for sub-comps
		// that span both children of nd_node — so under -unifiedPicker
		// the deeper acceptance rounds wouldn't find any precomputed
		// separator to mark in sep_bias_active_. Instead, free the
		// sub-comp (it aliases comp) and fall through to the picker
		// at the current level: same nd_node, same accepted separator
		// already marked, residual just slightly smaller from BCP.
		if (subcomps.size() == 1 && trivial_factor == 1) {
			delete subcomps[0];
			// Don't return; let control continue past this block to
			// the picker / consumption / variable-branching below.
		} else {
		mpz_class result = trivial_factor;
		// "Did decomposition really shrink/split the formula?" If a single
		// sub-component came back with no isolated peeling, the sub has
		// the same vars as the parent and the per-component BCP filter
		// state should not change. Skip the SubVarsetGuard work entirely
		// in that hot path — the recursive solveComponent inherits the
		// caller's mask. Only when we actually decompose (multiple subs
		// OR isolated vars peeled) do we update the filter.
		bool decomposed = (subcomps.size() > 1) || (trivial_factor != 1);
		for (Component *sub : subcomps) {
			// Map sub-component to ND hierarchy child node.
			// mapToChild's return convention:
			//   >= 0 : valid child to descend to.
			//   -1   : legitimate "no child" (parent is a leaf, or no
			//          active vars with known leaves). Treat as "no
			//          hierarchy available for this sub-component".
			//   -2   : invariant violation — sub-comp vars span both
			//          children. Must not happen when learning is
			//          correctly disabled during separator branching.
			int child_nd_node = -1;
			if (nd_node >= 0 && nd_hierarchy_.valid) {
				vector<unsigned> sub_vars;
				for (auto it = sub->varsBegin(); *it != varsSENTINEL; it++)
					if (isActive(LiteralID(*it, true)))
						sub_vars.push_back(*it);
				int mt = nd_hierarchy_.mapToChild(nd_node, sub_vars);
				// With -decomposeInSep, a sub-comp can legitimately span
				// both children because mid-consumption decomposition
				// kept the parent's nd_node for sub-comps that contained
				// unbranched bridging elements. In that context -2 is
				// not an invariant violation — it just means we cannot
				// descend to a single child here. Treat as -1 (no
				// hierarchy descent) and let the recursive solveComponent
				// fall back to reactive METIS / plain branching. When
				// the flag is off, the original abort discipline holds.
				// Under -decomposeInSep / -sepVarBias / -unifiedPicker,
				// the precomputed separator no longer strictly cuts
				// the residual. A sub-comp may legitimately span both
				// L and R children of nd_node; treat -2 as "no clean
				// child mapping" and descend without hierarchy guidance.
				// (Already true under any of those flags now, after
				// the unified-picker fix that wires mid-consumption
				// decompose under it.)
				if (mt == -2
				    && (config_.decompose_in_separator
				        || config_.separator_vars_as_bias
				        || config_.unified_picker)) {
					mt = -1;
				}
				if (mt == -2) {
					int lc = nd_hierarchy_.left_child[nd_node];
					int rc = nd_hierarchy_.right_child[nd_node];
					int L_lo = nd_hierarchy_.leaf_lo[lc], L_hi = nd_hierarchy_.leaf_hi[lc];
					int R_lo = nd_hierarchy_.leaf_lo[rc], R_hi = nd_hierarchy_.leaf_hi[rc];
					auto side = [&](int leaf) -> char {
						if (leaf < 0) return 'X';
						if (leaf >= L_lo && leaf <= L_hi) return 'L';
						if (leaf >= R_lo && leaf <= R_hi) return 'R';
						return '?';
					};
					std::cerr << "\n*** SEPARATOR_INVARIANT_VIOLATED ***\n"
					          << "  nd_node=" << nd_node
					          << " sub_vars.size()=" << sub_vars.size()
					          << " removed_clauses=" << removed_clauses_.size()
					          << "\n  child subtrees:"
					          << " left=[" << L_lo << ".." << L_hi << "]"
					          << " right=[" << R_lo << ".." << R_hi << "]\n";

					// Build set of active sub_vars for fast membership
					std::unordered_set<unsigned> sub_var_set(sub_vars.begin(), sub_vars.end());

					// Dump ALL clauses whose active lits in this sub-comp bridge L and R.
					// We iterate the sub-comp's clause IDs (original) + conflict_clauses_ (learned).
					auto emit_clause_if_bridging = [&](ClauseOfs ofs, bool is_learned) {
						int nL = 0, nR = 0, nX = 0;
						std::vector<std::pair<int,int>> active_lits;  // (signed_compact_lit, leaf)
						for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++) {
							unsigned var = lt->var();
							if (!sub_var_set.count(var)) continue;  // not in this sub-comp
							if (!isActive(*lt)) continue;  // lit resolved
							int leaf = (var < nd_hierarchy_.var_leaf.size())
							             ? nd_hierarchy_.var_leaf[var] : -1;
							active_lits.push_back({lt->toInt(), leaf});
							char s = side(leaf);
							if (s == 'L') nL++;
							else if (s == 'R') nR++;
							else nX++;
						}
						if (nL > 0 && nR > 0) {
							std::cerr << "  BRIDGE_CLAUSE ofs=" << ofs
							          << " kind=" << (is_learned ? "LEARNED" : "ORIGINAL")
							          << " nL=" << nL << " nR=" << nR << " nX=" << nX
							          << " lits=";
							for (auto &p : active_lits) {
								int leaf = p.second;
								char s = side(leaf);
								std::cerr << p.first << "(" << s << "/" << leaf << "),";
							}
							std::cerr << "\n";
						}
					};

					// Walk ALL clauses in the sub-component that the component
					// analyzer sees — both original and learned.
					Component &c = const_cast<Component&>(comp);
					for (auto ci = c.clsBegin(); *ci != clsSENTINEL; ci++) {
						ClauseOfs ofs = comp_manager_.clauseOfsOf(*ci);
						bool is_learned = (ofs >= original_lit_pool_size_);
						emit_clause_if_bridging(ofs, is_learned);
					}
					// Also dump any bridging binaries in binary_links_
					std::cerr << "  (binary_links_ bridges):\n";
					int bin_bridges = 0;
					for (unsigned v : sub_vars) {
						for (int sign = 0; sign <= 1; sign++) {
							LiteralID l(v, sign == 0);
							const auto &bl = literal(l).binary_links_;
							for (auto bt = bl.begin(); *bt != SENTINEL_LIT; ++bt) {
								if (!(l.raw() < bt->raw())) continue;  // dedup
								unsigned other = bt->var();
								if (!sub_var_set.count(other)) continue;
								int la = (v < nd_hierarchy_.var_leaf.size())
								           ? nd_hierarchy_.var_leaf[v] : -1;
								int lb = (other < nd_hierarchy_.var_leaf.size())
								           ? nd_hierarchy_.var_leaf[other] : -1;
								char sa = side(la), sb = side(lb);
								if ((sa == 'L' && sb == 'R') || (sa == 'R' && sb == 'L')) {
									std::cerr << "    BIN_BRIDGE lits=" << l.toInt()
									          << "," << bt->toInt()
									          << " leaves=" << la << "(" << sa << ")"
									          << "," << lb << "(" << sb << ")\n";
									bin_bridges++;
									if (bin_bridges >= 20) break;
								}
							}
							if (bin_bridges >= 20) break;
						}
						if (bin_bridges >= 20) break;
					}
					std::cerr << "  total binary bridges (first 20 shown)=" << bin_bridges << "\n";
					std::cerr.flush();
					std::abort();
				}
				child_nd_node = mt;  // >= 0 or -1 (both valid/legitimate)
			}

			// Content cache lookup
			mpz_class sub_count;
			static const unordered_map<ClauseOfs, unsigned> empty_removed;
			const auto &rm = removed_clauses_;

			// L1 fast-path: identity-based lookup. The 128-bit hash
			// was computed at component-construction time in
			// makeComponentFromState, so this is two field reads +
			// one unordered_map probe (128-bit equality).
			IdKey id_key;
			if (config_.perform_component_caching
			    && sub->num_variables() >= 3
			    && !config_.verify_cache
			    && sub->hasL1Hash()) {
				id_key.hash_lo = sub->l1HashLo();
				id_key.hash_hi = sub->l1HashHi();
				if (comp_manager_.contentCache().l1_lookup(id_key, sub_count)) {
					// Brute-force check at L1-hit time.
					if (config_.brute_force_cache_check_n > 0) {
						unsigned n_active = 0;
						mpz_class brute = bruteForceCountSubcomp(
						    *sub, config_.brute_force_cache_check_n, &n_active);
						if (brute >= 0 && brute != sub_count) {
							std::cerr << "\n*** BRUTE_FORCE_L1_HIT_MISMATCH ***\n"
							          << "  n_active=" << n_active
							          << "  brute_count=" << brute
							          << "  l1_returned=" << sub_count
							          << "  diff=" << (sub_count - brute)
							          << "  depth=" << depth << "\n";
							if (!config_.brute_force_cache_dump_dir.empty()) {
								static long long bf_l1_id = 0;
								std::string path = config_.brute_force_cache_dump_dir
								                 + "/l1_mismatch_"
								                 + std::to_string(bf_l1_id++) + ".cnf";
								unsigned dump_n = 0;
								if (dumpSubComponentCnf(*sub, path, &dump_n))
									std::cerr << "  dumped to: " << path << "\n";
							}
							std::cerr.flush();
							std::abort();
						}
					}
					result *= sub_count;
					delete sub;
					continue;
				}
			}

			CanonicalKey key = buildCanonicalKey(
				*sub, literal_pool_, literals_, literal_values_,
				comp_manager_.getAnalyzer().clauseIdToOfs(), rm,
				original_lit_pool_size_, config_.canonical_compact,
				config_.no_anonymization, config_.wl_iterations,
				static_wl_labels_.empty() ? nullptr : &static_wl_labels_);
			bool hit = (config_.perform_component_caching &&
			            sub->num_variables() >= 3 &&
			            comp_manager_.contentCache().peek(key, sub_count));
			if (hit && !config_.verify_cache) {
				// L2 hit: canonicalized form matches a previously-cached
				// sub-component. Populate L1 so future visits with this
				// same ID-set skip the canonical build.
				// Brute-force check at L2-hit time.
				if (config_.brute_force_cache_check_n > 0) {
					unsigned n_active = 0;
					mpz_class brute = bruteForceCountSubcomp(
					    *sub, config_.brute_force_cache_check_n, &n_active);
					if (brute >= 0 && brute != sub_count) {
						std::cerr << "\n*** BRUTE_FORCE_L2_HIT_MISMATCH ***\n"
						          << "  n_active=" << n_active
						          << "  brute_count=" << brute
						          << "  l2_returned=" << sub_count
						          << "  diff=" << (sub_count - brute)
						          << "  depth=" << depth << "\n";
						if (!config_.brute_force_cache_dump_dir.empty()) {
							static long long bf_l2_id = 0;
							std::string path = config_.brute_force_cache_dump_dir
							                 + "/l2_mismatch_"
							                 + std::to_string(bf_l2_id++) + ".cnf";
							unsigned dump_n = 0;
							if (dumpSubComponentCnf(*sub, path, &dump_n))
								std::cerr << "  dumped to: " << path << "\n";
						}
						std::cerr.flush();
						std::abort();
					}
				}
				comp_manager_.contentCache().stats_hits++;
				if (sub->num_variables() >= 3) {
					comp_manager_.contentCache().l1_store(id_key, sub_count);
				}
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
				mpz_class recomputed = solveComponent(*sub, {}, true, depth + 1, child_nd_node,
				                                       reactive_metis_skip_until_depth);
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
			// Snapshot H's decision-only path when sub-component matches the
			// user-specified target var set (path_trace_comp_vars).
			if (config_.path_trace_ofs != 0 &&
			    !config_.path_trace_comp_vars.empty() &&
			    !config_.learn_trace_path.empty()) {
				extern bool g_path_recording;
				if (g_path_recording) {
					std::vector<unsigned> avars;
					for (auto it2 = sub->varsBegin(); *it2 != varsSENTINEL; it2++)
						if (isActive(LiteralID(*it2, true))) avars.push_back(*it2);
					std::vector<unsigned> target;
					const std::string &s = config_.path_trace_comp_vars;
					size_t start = 0;
					while (start < s.size()) {
						size_t c = s.find(',', start);
						if (c == std::string::npos) c = s.size();
						target.push_back((unsigned)atoi(s.substr(start, c - start).c_str()));
						start = c + 1;
					}
					std::sort(avars.begin(), avars.end());
					std::sort(target.begin(), target.end());
					if (avars == target) {
						std::ofstream tr(config_.learn_trace_path, std::ios::app);
						tr << "H_SNAPSHOT: depth=" << (depth + 1)
						   << " nvars=" << avars.size() << "\n";
						tr << "  decisions (DL ordered):\n";
						for (auto l : literal_stack_) {
							if (!var(l).ante.isAnt())
								tr << "    DL=" << var(l).decision_level
								   << " " << l.toInt() << "\n";
						}
						tr << "  full stack length=" << literal_stack_.size() << "\n";
						tr << "=== PATH RECORDING END ===\n";
						g_path_recording = false;
					}
				}
			}
			// Per-component BCP filter mask. Only fires when decomposition
			// actually split the formula (`decomposed` set above). A learned
			// clause whose vars escape *sub's varsBegin would propagate
			// across the new sub-component boundary and contaminate the
			// cached count. Diff against the parent (current_sub_var_list_)
			// is O(|parent ∖ child| + |child ∖ parent|), no full-bitmap reset.
			//
			// Branching paths inside solveComponent(*sub) won't push their
			// own guard — they recurse with the same comp.varsBegin, so the
			// mask we set here remains active throughout the sub's solve.
			struct SubVarsetGuard {
				Solver &slv;
				std::vector<unsigned> saved_parent_list;
				bool was_empty = false;
				bool active;
				SubVarsetGuard(Solver &s, Component &c, bool a) : slv(s), active(a) {
					if (!active) return;
					if (s.current_sub_varset_.empty()) {
						was_empty = true;
						s.current_sub_varset_.assign(s.num_variables() + 2, 0);
					}
					saved_parent_list = std::move(s.current_sub_var_list_);
					std::vector<unsigned> child_list;
					child_list.reserve(64);
					for (auto it = c.varsBegin(); *it != varsSENTINEL; it++)
						child_list.push_back(*it);
					std::sort(child_list.begin(), child_list.end());
					auto pi = saved_parent_list.begin(), pe = saved_parent_list.end();
					auto ci = child_list.begin(),       ce = child_list.end();
					while (pi != pe || ci != ce) {
						if (pi == pe || (ci != ce && *ci < *pi)) {
							if (*ci < s.current_sub_varset_.size())
								s.current_sub_varset_[*ci] = 1;
							ci++;
						} else if (ci == ce || *pi < *ci) {
							if (*pi < s.current_sub_varset_.size())
								s.current_sub_varset_[*pi] = 0;
							pi++;
						} else { pi++; ci++; }
					}
					s.current_sub_var_list_ = std::move(child_list);
				}
				~SubVarsetGuard() {
					if (!active) return;
					auto pi = saved_parent_list.begin(), pe = saved_parent_list.end();
					auto ci = slv.current_sub_var_list_.begin(),
					     ce = slv.current_sub_var_list_.end();
					while (pi != pe || ci != ce) {
						if (pi == pe || (ci != ce && *ci < *pi)) {
							if (*ci < slv.current_sub_varset_.size())
								slv.current_sub_varset_[*ci] = 0;
							ci++;
						} else if (ci == ce || *pi < *ci) {
							if (*pi < slv.current_sub_varset_.size())
								slv.current_sub_varset_[*pi] = 1;
							pi++;
						} else { pi++; ci++; }
					}
					slv.current_sub_var_list_ = std::move(saved_parent_list);
					if (was_empty) {
						slv.current_sub_varset_.clear();
						slv.current_sub_var_list_.clear();
					}
				}
			} sub_filter(*this, *sub, decomposed);
			sub_count = solveComponent(*sub, {}, true, depth + 1, child_nd_node,
			                           reactive_metis_skip_until_depth);

			// Diagnostic: if -dumpCompDir is set, dump this sub-component's
			// sub-problem as a DIMACS CNF and log our computed count. A
			// post-process script can then run ganak on each dumped file
			// to find the smallest sub-problem where our count is wrong —
			// a minimal in-tree reproducer of any correctness bug.
			if (!config_.dump_comp_dir.empty()) {
				static long long comp_dump_id = 0;
				unsigned comp_active_nvars = 0;
				// Pre-count to avoid dumping if outside window.
				for (auto it = sub->varsBegin(); *it != varsSENTINEL; it++)
					if (isActive(LiteralID(*it, true))) comp_active_nvars++;
				if (comp_active_nvars >= config_.dump_comp_min_vars
				    && comp_active_nvars <= config_.dump_comp_max_vars) {
					long long my_id = comp_dump_id++;
					std::string path = config_.dump_comp_dir + "/comp_"
					                 + std::to_string(my_id) + ".cnf";
					unsigned real_nv = 0;
					if (dumpSubComponentCnf(*sub, path, &real_nv)) {
						std::ofstream log(config_.dump_comp_dir + "/log.txt",
						                  std::ios::app);
						log << "id=" << my_id
						    << " depth=" << (depth + 1)
						    << " nvars=" << real_nv
						    << " count=" << sub_count
						    << " path=" << path << "\n";
					}
				}
			}

			// Brute-force cache check at STORE time. If sub-component is
			// small enough (<= N), enumerate and verify the count. On
			// mismatch: dump CNF + abort with diagnostic.
			if (config_.brute_force_cache_check_n > 0
			    && config_.perform_component_caching
			    && sub->num_variables() >= 3) {
				unsigned n_active = 0;
				mpz_class brute = bruteForceCountSubcomp(
				    *sub, config_.brute_force_cache_check_n, &n_active);
				if (brute >= 0 && brute != sub_count) {
					std::cerr << "\n*** BRUTE_FORCE_CACHE_STORE_MISMATCH ***\n"
					          << "  n_active=" << n_active
					          << "  brute_count=" << brute
					          << "  sub_count=" << sub_count
					          << "  diff=" << (sub_count - brute)
					          << "  depth=" << (depth + 1)
					          << "  removed_clauses=" << removed_clauses_.size()
					          << "\n";
					if (!config_.brute_force_cache_dump_dir.empty()) {
						static long long bf_dump_id = 0;
						std::string path = config_.brute_force_cache_dump_dir
						                 + "/store_mismatch_"
						                 + std::to_string(bf_dump_id++) + ".cnf";
						unsigned dump_n = 0;
						if (dumpSubComponentCnf(*sub, path, &dump_n)) {
							std::cerr << "  dumped to: " << path
							          << " (nvars=" << dump_n << ")\n";
						}
					}
					std::cerr.flush();
					std::abort();
				}
				// Invariant S2: if any in-scope learned clause confined to
				// this sub-component is unsoundly restricting models, the
				// "with learned" count would be SMALLER than the
				// "without learned" count. Sound learned clauses don't
				// change #SAT.
				if (brute >= 0) {
					unsigned n_learn = 0;
					mpz_class brute_with_learned =
					    bruteForceCountSubcompWithLearned(
					        *sub, config_.brute_force_cache_check_n,
					        nullptr, &n_learn);
					if (brute_with_learned >= 0
					    && brute_with_learned != brute) {
						std::cerr << "\n*** INV_S2_LEARNED_CLAUSE_UNSOUND ***\n"
						          << "  n_active=" << n_active
						          << "  brute (originals only)=" << brute
						          << "  brute (originals + in-scope learned)="
						          << brute_with_learned
						          << "  diff=" << (brute - brute_with_learned)
						          << "  n_learned_clauses_checked=" << n_learn
						          << "  depth=" << (depth + 1)
						          << "  removed_clauses=" << removed_clauses_.size()
						          << "\n";
						if (!config_.brute_force_cache_dump_dir.empty()) {
							static long long s2_dump_id = 0;
							std::string path = config_.brute_force_cache_dump_dir
							                 + "/s2_unsound_learned_"
							                 + std::to_string(s2_dump_id++) + ".cnf";
							unsigned dump_n = 0;
							if (dumpSubComponentCnf(*sub, path, &dump_n))
								std::cerr << "  dumped to: " << path
								          << " (nvars=" << dump_n << ")\n";
						}
						std::cerr.flush();
						std::abort();
					}
				}
			}
			if (config_.perform_component_caching && sub->num_variables() >= 3) {
				if (config_.verify_cache && g_first_sig.find(key.hash) == g_first_sig.end()) {
					g_first_sig[key.hash] = pre_sig;
				}
				comp_manager_.contentCache().store(key, sub_count);
				if (!config_.verify_cache) {
					// Populate L1 so future visits to the same ID-set skip
					// the canonical build. Skipped under verify_cache because
					// that mode force-recomputes everything.
					comp_manager_.contentCache().l1_store(id_key, sub_count);
				}
			}
			result *= sub_count;
			delete sub;
			if (result == 0) break;
		}
		return result;
		}  // close the else (real decomposition path)
	}

	// If requested, try to find a separator for this component.
	if (separator_reset) {
		separator_reset = false;
		if (config_.perform_separator_branching &&
		    comp.num_variables() >= config_.separator_min_active_vars) {
			// Try precomputed ND hierarchy first
			if (nd_node >= 0 && nd_hierarchy_.valid) {
				vector<unsigned> active_ids;
				for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++)
					if (isActive(LiteralID(*it, true)))
						active_ids.push_back(*it);
				separator = nd_hierarchy_.lookupSeparator(active_ids, nd_node);
				// Filter: the precomputed separator lists elements from the
				// static hierarchy node. Only keep those actually present in
				// the current sub-component — branching on a variable outside
				// the component would double-count (A+B = 2*#SAT when the
				// variable doesn't constrain the component).
				if (!separator.empty()) {
					std::unordered_set<unsigned> comp_vars;
					for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++)
						comp_vars.insert(*it);
					std::unordered_set<unsigned> comp_clauses;
					for (auto it = comp.clsBegin(); *it != clsSENTINEL; it++) {
						ClauseOfs ofs = comp_manager_.clauseOfsOf(*it);
						comp_clauses.insert(ofs);
					}
					std::vector<CutNode> filtered;
					filtered.reserve(separator.size());
					for (const auto &nd : separator) {
						if (nd.kind == CutNode::VAR) {
							if (comp_vars.count(nd.id))
								filtered.push_back(nd);
						} else {
							if (comp_clauses.count(nd.id))
								filtered.push_back(nd);
						}
					}
					if (config_.verbose && filtered.empty() && !separator.empty()) {
						// Separator vars at this node are assigned to
						// the left child's leaf range by construction, so
						// a stored but entirely-filtered separator means
						// the current sub-component lives only in the
						// right child's subtree. That's exactly the
						// extreme-imbalance case the balance gate is for
						// (L=0, R=n → balance=0). Report it as a single
						// rejection reason — the action (fall through to
						// variable branching) is the same either way.
						std::cerr << "  TIER1_REJECT nd=" << nd_node
						          << " reason=filter_empty stored_sep="
						          << separator.size() << std::endl;
					}
					separator = std::move(filtered);
				}
				// Phase 2 / Tier 1 gating: reject too-large or too-imbalanced
				// hierarchy separators for this sub-component. When rejected,
				// `separator` is cleared and we fall through to the reactive
				// Dinic's fallback (if enabled) or plain variable branching.
				if (!separator.empty() &&
				    !hierarchySeparatorAcceptable(nd_node, comp,
				                                  (unsigned)separator.size())) {
					separator.clear();
				}
				// Legacy -sepVarBias path: strip VARs to bias bitmap,
				// keep CLAUSEs in separator. NOT applied under
				// -unifiedPicker — the picker reads VAR membership
				// directly from `separator`, so we keep both kinds
				// in the carried vector.
				if (config_.separator_vars_as_bias
				    && !config_.unified_picker
				    && !separator.empty()) {
					std::vector<CutNode> vars_only, clauses_only;
					vars_only.reserve(separator.size());
					clauses_only.reserve(separator.size());
					for (const auto &nd : separator) {
						if (nd.kind == CutNode::VAR) vars_only.push_back(nd);
						else clauses_only.push_back(nd);
					}
					if (!vars_only.empty()) markSeparatorBias(vars_only);
					separator = std::move(clauses_only);
				}
				// Verbose logging: accepted precomputed separator —
				// record size, component size, L/R distribution.
				if (config_.verbose && !separator.empty() && nd_node >= 0
				    && nd_hierarchy_.valid) {
					unsigned n_act = 0, L_dbg = 0, R_dbg = 0;
					int lc = nd_hierarchy_.left_child[nd_node];
					int rc = nd_hierarchy_.right_child[nd_node];
					if (lc >= 0 && rc >= 0) {
						int L_lo = nd_hierarchy_.leaf_lo[lc];
						int L_hi = nd_hierarchy_.leaf_hi[lc];
						int R_lo = nd_hierarchy_.leaf_lo[rc];
						int R_hi = nd_hierarchy_.leaf_hi[rc];
						for (auto it = comp.varsBegin(); *it != varsSENTINEL; ++it) {
							if (!isActive(LiteralID(*it, true))) continue;
							n_act++;
							if ((size_t)*it >= nd_hierarchy_.var_leaf.size()) continue;
							int leaf = nd_hierarchy_.var_leaf[*it];
							if (leaf < 0) continue;
							if (leaf >= L_lo && leaf <= L_hi) L_dbg++;
							else if (leaf >= R_lo && leaf <= R_hi) R_dbg++;
						}
						std::cerr << "  SEP_USE kind=precomp depth=" << depth
						          << " sep=" << separator.size()
						          << " n_active=" << n_act
						          << " L=" << L_dbg
						          << " R=" << R_dbg << std::endl;
					}
				}
			}
			// Reactive METIS fallback: when the precomputed hierarchy's
			// separator is unavailable or was rejected by the Phase-2
			// gate, compute a fresh METIS separator on the current
			// sub-component and USE it. Sub-components with fewer
			// than `reactive_metis_min_vars` active variables are
			// skipped (too small for METIS to bisect usefully) and
			// fall through to variable branching.
			//
			// Failure throttle: after a reactive-METIS failure at some
			// depth d we advance `reactive_metis_skip_until_depth` to
			// d + config_.reactive_metis_skip_k, so this subtree does
			// not retry METIS until BCP + variable branching at k more
			// decomposition levels has had a chance to simplify the
			// formula. Success does NOT raise the skip — a succeeding
			// subtree continues to attempt METIS on subsequent
			// fallbacks, matching the pre-throttle behaviour.
			if (separator.empty() && config_.use_reactive_metis
			    && depth >= reactive_metis_skip_until_depth) {
				// Cheap threshold check first: count active vars WITHOUT
				// building the full METIS input. buildMetisInput iterates
				// every clause and binary link too, which is wasteful for
				// sub-components that will be rejected here anyway.
				unsigned n_active_quick = 0;
				for (auto it = comp.varsBegin(); *it != varsSENTINEL; ++it)
					if (isActive(LiteralID(*it, true))) n_active_quick++;
				if (n_active_quick >= config_.reactive_metis_min_vars) {
					std::vector<unsigned> mv;
					std::vector<std::pair<unsigned, std::vector<unsigned>>> mc;
					std::vector<std::pair<unsigned, unsigned>> mp;
					buildMetisInputFromComponent(comp, mv, mc, mp);
					RuntimeSeparatorResult r = computeRuntimeMetisSeparator(mv, mc, mp);
					// Accumulate measurement stats regardless of outcome.
					reactive_metis_calls_++;
					reactive_metis_total_us_ += r.metis_elapsed_us;
					if (r.metis_elapsed_us > reactive_metis_max_us_)
						reactive_metis_max_us_ = r.metis_elapsed_us;
					reactive_metis_sum_nvars_ += mv.size();
					reactive_metis_sum_sep_   += r.separator.size();
					if (!r.ok) reactive_metis_failed_++;
					int bucket = 0;
					size_t n = mv.size();
					if      (n >= 512) bucket = 6;
					else if (n >= 256) bucket = 5;
					else if (n >= 128) bucket = 4;
					else if (n >= 64)  bucket = 3;
					else if (n >= 32)  bucket = 2;
					else if (n >= 16)  bucket = 1;
					reactive_metis_bucket_count_[bucket]++;
					reactive_metis_bucket_total_us_[bucket] += r.metis_elapsed_us;

					// Scheme F: dual-gate the reactive separator before
					// accepting. Even when METIS returns a valid
					// bisection, the separator may be a poor branching
					// choice on two axes:
					//   Gate 1 (structural): the bisection itself must
					//     be reasonable — sep size modest relative to n,
					//     and the two sides not lopsided. Reuses the
					//     Phase-2 thresholds already applied to
					//     precomputed separators.
					//   Gate 2 (branching-variable quality): the
					//     separator's variables must be "good" branching
					//     targets by the Stage-0 σ proxy (BCP cascade
					//     potential). Rejects the bench_A pathology
					//     where METIS chose a 1-var separator whose
					//     one variable is σ-weak. Skipped when the σ
					//     signal across the component is too uniform
					//     to discriminate.
					bool accept = r.ok;
					if (accept) {
						// Gate 1: structural bisection quality. Uses the
						// same size gate as precomputed-separator path
						// (separatorSizeAcceptable: min(0.5*n, 32)) so
						// both paths agree on what "reasonable" means.
						// Balance gate is retained as-is.
						bool size_ok = separatorSizeAcceptable(
						    (unsigned)r.separator.size(),
						    n_active_quick);
						unsigned Ltot = r.left_vars + r.right_vars;
						double g1_balance = (Ltot > 0)
						    ? (double)std::min(r.left_vars, r.right_vars)
						      / (double)Ltot
						    : 0.0;
						if (!size_ok
						    || g1_balance < config_.separator_min_balance) {
							accept = false;
							reactive_metis_gate1_rej_++;
							if (config_.verbose) {
								unsigned allowed = std::min(
								    (unsigned)(0.3 * (double)n_active_quick),
								    20u);
								std::cerr << "  SEP_REJECT_REACTIVE gate=1"
								          << " depth=" << depth
								          << " sep=" << r.separator.size()
								          << " n=" << n_active_quick
								          << " allowed=" << allowed
								          << " balance=" << g1_balance << std::endl;
							}
						}
					}
					if (accept && config_.reactive_metis_sigma_beta > 0.0) {
						// Gate 2: branching-variable quality via σ.
						std::vector<double> sigma_all;
						std::vector<VariableIndex> cand_all;
						stage0_cheap_scores(comp, config_.stage0_length_decay,
						                     sigma_all, cand_all);
						if (!cand_all.empty()) {
							std::vector<double> sorted_sigma;
							sorted_sigma.reserve(cand_all.size());
							for (VariableIndex v : cand_all)
								sorted_sigma.push_back(sigma_all[v]);
							std::sort(sorted_sigma.begin(), sorted_sigma.end());
							double sigma_top    = sorted_sigma.back();
							double sigma_median = sorted_sigma[sorted_sigma.size() / 2];
							bool signal_strong = (sigma_median > 0.0 &&
							    sigma_top / sigma_median
							    >= config_.reactive_metis_sigma_signal_threshold);
							if (signal_strong) {
								double sigma_sep_sum = 0.0;
								int var_count = 0;
								for (const auto &cn : r.separator) {
									if (cn.kind == CutNode::VAR
									    && (size_t)cn.id < sigma_all.size()) {
										sigma_sep_sum += sigma_all[cn.id];
										var_count++;
									}
								}
								if (var_count > 0) {
									double sigma_sep_avg = sigma_sep_sum / var_count;
									if (sigma_sep_avg
									    < config_.reactive_metis_sigma_beta * sigma_top) {
										accept = false;
										reactive_metis_gate2_rej_++;
										if (config_.verbose) {
											std::cerr << "  SEP_REJECT_REACTIVE gate=2"
											          << " depth=" << depth
											          << " sep_avg=" << sigma_sep_avg
											          << " top=" << sigma_top
											          << " median=" << sigma_median
											          << " beta="
											          << config_.reactive_metis_sigma_beta
											          << std::endl;
										}
									}
								}
							}
						}
					}

					if (accept) {
						reactive_metis_accepted_++;
						if (config_.verbose) {
							std::cerr << "  SEP_USE kind=reactive depth=" << depth
							          << " sep=" << r.separator.size()
							          << " n_active=" << n_active_quick
							          << " L=" << r.left_vars
							          << " R=" << r.right_vars << std::endl;
						}
						if (config_.separator_vars_as_bias
						    && !config_.unified_picker) {
							// Legacy -sepVarBias path: strip VARs to bias.
							std::vector<CutNode> vars_only, clauses_only;
							vars_only.reserve(r.separator.size());
							clauses_only.reserve(r.separator.size());
							for (const auto &nd : r.separator) {
								if (nd.kind == CutNode::VAR)
									vars_only.push_back(nd);
								else
									clauses_only.push_back(nd);
							}
							if (!vars_only.empty()) markSeparatorBias(vars_only);
							separator = std::move(clauses_only);
							nd_node = -1;
						} else {
							// Default + -unifiedPicker: keep all elements
							// in the carried separator.
							separator = std::move(r.separator);
							nd_node = -1;
						}
					} else {
						// Either METIS failed, or Scheme F gates rejected.
						// In both cases throttle: don't retry reactive METIS
						// for this subtree until k more decomposition
						// levels have passed.
						reactive_metis_skip_until_depth =
							depth + (int)config_.reactive_metis_skip_k;
					}
				}
			}
		}
	}

	// Stage C: unified picker. Score active VARs and CLAUSEs at this
	// decision; branch on the highest-scoring target. Replaces both
	// the separator-consumption loop below and the variable picker
	// further down. Forced-decision overrides and adaptive branching
	// keep their existing paths and take precedence when configured.
	if (config_.unified_picker
	    && !config_.perform_adaptive_branching
	    && config_.forced_decisions.empty()) {
		BranchTarget tgt = pickBranchTarget(comp, separator);
		if (tgt.score < 0.0f) {
			return 1;
		}
		if (tgt.kind == BranchTarget::VAR) {
			VariableIndex v = tgt.id;
			LiteralID lit_t(v, true), lit_f(v, false);
			bool t_first = literal(lit_t).activity_score_
			               > literal(lit_f).activity_score_;
			// Pass `separator` forward unchanged: in unified-picker
			// mode it's an immutable hint that drives the clause-bias
			// score on subsequent picks; we do not consume from it.
			// from_separator=false: learning is enabled for var
			// branches under the unified picker (we no longer rely
			// on strict L/R descent that could be broken by learned
			// clauses; mapToChild==-2 is already softened to -1).
			mpz_class A = branchOnLiteral(t_first ? lit_t : lit_f, comp,
			                              separator, false, depth, nd_node,
			                              /*from_separator=*/false,
			                              reactive_metis_skip_until_depth);
			mpz_class B = branchOnLiteral(t_first ? lit_f : lit_t, comp,
			                              separator, false, depth, nd_node,
			                              /*from_separator=*/false,
			                              reactive_metis_skip_until_depth);
			return A + B;
		} else {
			ClauseOfs ofs = (ClauseOfs)tgt.id;
			mpz_class A = branchOnClause(ofs, comp, separator, false,
			                              false, depth, nd_node,
			                              reactive_metis_skip_until_depth);
			mpz_class B = branchOnClause(ofs, comp, separator, false,
			                              true, depth, nd_node,
			                              reactive_metis_skip_until_depth);
			return A - B;
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
				return solveComponent(comp, rest, false, depth, nd_node,
				                      reactive_metis_skip_until_depth);
			}
			// Branch: A = v=true, B = v=false. This branch consumes a
			// separator element — learning must be suppressed inside.
			LiteralID lit_t(el.id, true), lit_f(el.id, false);
			bool t_first = literal(lit_t).activity_score_ >
			               literal(lit_f).activity_score_;
			mpz_class A = branchOnLiteral(t_first ? lit_t : lit_f,
			                              comp, rest, false, depth, nd_node,
			                              /*from_separator=*/true,
			                              reactive_metis_skip_until_depth);
			mpz_class B = branchOnLiteral(t_first ? lit_f : lit_t,
			                              comp, rest, false, depth, nd_node,
			                              /*from_separator=*/true,
			                              reactive_metis_skip_until_depth);
			return A + B;
		} else {
			// Clause element
			if (isClauseRemoved(el.id) || isSatisfied(el.id)) {
				return solveComponent(comp, rest, false, depth, nd_node,
				                      reactive_metis_skip_until_depth);
			}
			mpz_class A = branchOnClause(el.id, comp, rest, false, false, depth, nd_node,
			                              reactive_metis_skip_until_depth);
			mpz_class B = branchOnClause(el.id, comp, rest, false, true, depth, nd_node,
			                              reactive_metis_skip_until_depth);
			return A - B;
		}
	}

	// No separator — variable branching within comp.
	// Phase 3: when `perform_adaptive_branching` is enabled, use the τ-based
	// adaptive picker which also commits failed literals as unconditional
	// progress (see pickBranchVariableAdaptive). Otherwise fall back to the
	// legacy activity-score picker.
	VariableIndex v = 0;
	bool forced_polarity_pos = false;
	bool used_forced = false;

	// Scripted decision override. Use the next forced literal IFF the
	// literal's variable is active AND is a member of the current comp.
	if (!config_.forced_decisions.empty()) {
		static size_t forced_idx = 0;
		while (forced_idx < config_.forced_decisions.size()) {
			int fl = config_.forced_decisions[forced_idx];
			VariableIndex fv = (VariableIndex)std::abs(fl);
			if (!isActive(LiteralID(fv, true))) { forced_idx++; continue; }
			bool in_comp = false;
			for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++)
				if (*it == fv) { in_comp = true; break; }
			if (!in_comp) break;  // forced var not in this comp — fall through
			v = fv;
			forced_polarity_pos = (fl > 0);
			used_forced = true;
			forced_idx++;
			break;
		}
	}

	if (!used_forced) {
		if (config_.perform_adaptive_branching) {
			bool comp_unsat = false;
			v = pickBranchVariableAdaptive(comp, comp_unsat);
			if (comp_unsat) return 0;
		} else {
			v = pickBranchVariable(comp);
		}
	}
	if (v == 0) {
		return 1;
	}
	LiteralID lit_t(v, true), lit_f(v, false);
	bool t_first;
	if (used_forced) {
		t_first = forced_polarity_pos;
	} else {
		t_first = literal(lit_t).activity_score_ >
		          literal(lit_f).activity_score_;
	}
	// Non-separator variable branching: learning IS allowed here.
	mpz_class A = branchOnLiteral(t_first ? lit_t : lit_f, comp, {}, false, depth, -1,
	                              /*from_separator=*/false,
	                              reactive_metis_skip_until_depth);
	mpz_class B = branchOnLiteral(t_first ? lit_f : lit_t, comp, {}, false, depth, -1,
	                              /*from_separator=*/false,
	                              reactive_metis_skip_until_depth);
	return A + B;
}

mpz_class Solver::branchOnLiteral(LiteralID lit,
                                   Component &comp,
                                   vector<CutNode> separator,
                                   bool separator_reset, int depth, int nd_node,
                                   bool from_separator,
                                   int reactive_metis_skip_until_depth) {
	unsigned lit_save = literal_stack_.size();
	// Invariants T1+T2 (gated): snapshot trail state for restore-check.
	std::size_t snap_removed_size = 0;
	std::size_t snap_lscope_size  = 0;
	if (config_.check_learn_invariants) {
		snap_removed_size = removed_clauses_.size();
		snap_lscope_size  = learned_clause_scope_.size();
	}
	statistics_.num_decisions_++;
	if (statistics_.num_decisions_ % 128 == 0)
		decayActivities();

	// Check if literal is already assigned
	if (!isActive(lit)) {
		if (literal_values_[lit] == F_TRI) return 0;
		return solveComponent(comp, separator, separator_reset, depth, nd_node,
		                      reactive_metis_skip_until_depth);
	}

	// Push a placeholder StackLevel BEFORE setLiteralIfFree so:
	//   - setLiteralIfFree records decision_level = this level (not 0)
	//   - recordLastUIPCauses can identify "current dl" on conflict
	// We pop at the end regardless of outcome.
	stack_.push_back(StackLevel(1, lit_save,
	                            comp_manager_.component_stack_size()));

	static long long bol_call = 0;
	bol_call++;
	long long my_id = bol_call;
	if (config_.log_branches) {
		// Emit the decision path from root to this branch. Each lit is
		// a decision (ante-less) on the current literal_stack_.
		auto to_orig_lit = [&](LiteralID l) -> int {
			unsigned v = l.var();
			int orig = (v < compact_to_orig_.size() && !compact_to_orig_.empty())
			             ? (int)compact_to_orig_[v] : (int)v;
			return l.sign() ? orig : -orig;
		};
		std::cerr << "BRANCH_ENTER id=" << my_id
		          << " lit=" << lit.toInt()
		          << " lit_orig=" << to_orig_lit(lit)
		          << " DL=" << stack_.get_decision_level()
		          << " path=";
		for (auto l : literal_stack_)
			if (!var(l).ante.isAnt())
				std::cerr << l.toInt() << ",";
		std::cerr << " path_orig=";
		for (auto l : literal_stack_)
			if (!var(l).ante.isAnt())
				std::cerr << to_orig_lit(l) << ",";
		std::cerr << "\n";
	}

	setLiteralIfFree(lit);

	bool bcp_ok = BCP(lit_save);

	// Stop-and-dump point: after BCP at the requested branch id, write
	// the current formula state as CNF and exit. The two-file pair of
	// such dumps at the same id from two runs forms a smaller reproducer.
	if (config_.stop_at_branch == my_id && !config_.stop_at_branch_path.empty()) {
		std::cerr << "STOP_AT_BRANCH id=" << my_id
		          << " bcp_ok=" << bcp_ok << "\n";
		dumpPreprocessedCnf(config_.stop_at_branch_path);
		std::exit(0);
	}

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
		//
		// IMPORTANT: learning is suppressed when this branch is part of
		// consuming a precomputed separator element (from_separator=true).
		// A learned clause adds a hyperedge to the incidence graph, which
		// can connect variables across the hierarchy's left/right subtrees
		// and violate the separator's structural invariant — breaking
		// mapToChild's ability to descend. During separator branching the
		// formula must evolve only via BCP + clause removal (which can
		// only shrink connectivity, never grow it).
		recordLastUIPCauses();
		// Learning ladder (see solver_config.h::learn_level):
		//   level 0 → don't learn at all
		//   level 1 → learn but skip dedup
		//   level 2 → + dedup (no scope)
		//   level 3 → + scope (no binary padding)
		//   level 4 → + binary padding
		//   level 5 → + minimization (handled in minimizeAndStoreUIPClause)
		const int L = config_.learn_level;
		if (!from_separator
		    && config_.perform_conflict_clause_learning
		    && L >= 1
		    && !uip_clauses_.empty() && uip_clauses_.back().size() >= 2
		    && uip_clauses_.back().front() == lit.neg()) {
			// Dedup via Bloom filter (shared with commitFailedLiteral
			// and implicant learning). FP = skip learning = sound.
			bool go = (L <= 1) || maybeDedupClause(uip_clauses_.back());
			if (go) {
				Antecedent a = addScopedUIPConflictClause(
				    uip_clauses_.back(),
				    /*pad_binary=*/  L >= 4,
				    /*record_scope=*/L >= 3);
				if (a.isAnt() && a.isAClause())
					logLearnTrace(a.asCl(), uip_clauses_.back());
			} else {
				statistics_.num_learned_dedup_dropped_++;
			}
		}
		result = 0;
	} else {
		// Phase 4: mine implicants from the BCP cascade. Opt-in; no-op
		// by default. Suppressed during separator branching (same
		// discipline as regular clause learning at line 951-955).
		if (!from_separator) {
			maybeLearnImplicants(lit_save);
		}
		// Dynamic-subsumption measurement. Read-only; opt-in; sampled.
		// Also suppressed during separator branching — measurement
		// during that phase would count events in a state where we
		// explicitly forbid learning, so it wouldn't be actionable.
		if (!from_separator && config_.analyze_dynamic_subsumption) {
			analyzeDynamicSubsumption(lit_save);
		}
		result = solveComponent(comp, separator, separator_reset, depth, nd_node,
		                        reactive_metis_skip_until_depth);
	}

	if (config_.log_branches) {
		// Emit the decision path at exit for matching across runs.
		std::cerr << "BRANCH_EXIT id=" << my_id
		          << " lit=" << lit.toInt()
		          << " result=" << result
		          << " path=";
		for (auto l : literal_stack_)
			if (!var(l).ante.isAnt())
				std::cerr << l.toInt() << ",";
		std::cerr << "\n";
	}
	while (literal_stack_.size() > lit_save) {
		unSet(literal_stack_.back());
		literal_stack_.pop_back();
	}
	// Invariant T1: literal_stack_ size restored to entry value.
	// Invariant T2: removed_clauses_ size restored, learned_clause_scope_
	//               only-grew (we may have learned new scoped clauses).
	if (config_.check_learn_invariants) {
		if (literal_stack_.size() != lit_save) {
			std::cerr << "\n*** INV_T1_TRAIL_NOT_RESTORED (branchOnLiteral) ***\n"
			          << "  lit=" << lit.toInt()
			          << "  lit_save=" << lit_save
			          << "  current literal_stack_.size()=" << literal_stack_.size()
			          << "  diff=" << ((long)literal_stack_.size() - (long)lit_save)
			          << "\n";
			std::cerr.flush();
			std::abort();
		}
		if (removed_clauses_.size() != snap_removed_size) {
			std::cerr << "\n*** INV_T2_REMOVED_CLAUSES_LEAK (branchOnLiteral) ***\n"
			          << "  lit=" << lit.toInt()
			          << "  snap_removed_size=" << snap_removed_size
			          << "  current removed_clauses_.size()=" << removed_clauses_.size()
			          << "\n";
			std::cerr.flush();
			std::abort();
		}
		if (learned_clause_scope_.size() < snap_lscope_size) {
			std::cerr << "\n*** INV_T2b_LEARNED_SCOPE_SHRANK (branchOnLiteral) ***\n"
			          << "  lit=" << lit.toInt()
			          << "  snap=" << snap_lscope_size
			          << "  current=" << learned_clause_scope_.size() << "\n";
			std::cerr.flush();
			std::abort();
		}
		// Invariant T3: literal_values_ <-> literal_stack_ desync.
		// Walk the trail; for each literal on the stack, both polarities
		// of literal_values_ must reflect "set". For each X_TRI literal,
		// neither polarity should claim T_TRI/F_TRI. Sampled by depth %
		// 8 to keep cost bounded.
		if ((unsigned)depth % 8 == 0) {
			// Scan all variables 1..num_variables(). For each, classify
			// via literal_values_; check trail-consistency.
			for (unsigned v = 1; v <= num_variables(); v++) {
				LiteralID p(v, true), n(v, false);
				TriValue vp = literal_values_[p], vn = literal_values_[n];
				bool both_x = (vp == X_TRI && vn == X_TRI);
				bool t_polar = (vp == T_TRI && vn == F_TRI);
				bool f_polar = (vp == F_TRI && vn == T_TRI);
				if (!both_x && !t_polar && !f_polar) {
					std::cerr << "\n*** INV_T3_LITVALUES_INCONSISTENT ***\n"
					          << "  var=" << v
					          << "  +v_value=" << (int)vp
					          << "  -v_value=" << (int)vn << "\n";
					std::cerr.flush();
					std::abort();
				}
			}
		}
	}
	stack_.pop_back();
	return result;
}

mpz_class Solver::branchOnClause(ClauseOfs cl_ofs,
                                  Component &comp,
                                  vector<CutNode> separator,
                                  bool separator_reset,
                                  bool negate_literals, int depth, int nd_node,
                                  int reactive_metis_skip_until_depth) {
	unsigned lit_save = literal_stack_.size();
	std::size_t snap_removed_size = 0;
	std::size_t snap_lscope_size  = 0;
	if (config_.check_learn_invariants) {
		snap_removed_size = removed_clauses_.size();
		snap_lscope_size  = learned_clause_scope_.size();
	}
	statistics_.num_decisions_++;

	if (config_.log_branches) {
		std::cerr << "BRANCH_CLAUSE_ENTER ofs=" << cl_ofs
		          << " negate=" << (negate_literals ? 1 : 0)
		          << " DL=" << stack_.get_decision_level()
		          << " depth=" << depth
		          << "\n";
	}

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

	// Invariant T4 (the one targeted at the wrongly-justified-literal
	// failure mode): once C is added to removed_clauses_, every
	// literal currently on the trail must have an antecedent that
	// is STILL in scope. If a literal ℓ was forced earlier via a
	// learned clause D whose scope did NOT include C, then under
	// F\{C} the antecedent D may no longer be a logical consequence,
	// which means ℓ may not be forced under F\{C}. The literal sits
	// on the trail as if it were assigned, but it's wrongly so.
	// Subsequent counting under this branch undercounts by however
	// many models F\{C} has with ℓ unassigned (or assigned the other
	// polarity).
	if (config_.check_learn_invariants) {
		for (auto trail_lit : literal_stack_) {
			Antecedent ant = var(trail_lit).ante;
			if (!ant.isAnt() || !ant.isAClause()) continue;
			ClauseOfs ant_ofs = ant.asCl();
			if (ant_ofs < (ClauseOfs)original_lit_pool_size_) continue;  // original
			if (!learnedClauseInScope(ant_ofs)) {
				std::cerr << "\n*** INV_T4_TRAIL_LIT_LOST_JUSTIFICATION ***\n"
				          << "  trail_lit=" << trail_lit.toInt()
				          << "  forcing_ante_cl=" << ant_ofs << " (learned)\n"
				          << "  removed_clauses_ (after adding C):\n";
				std::cerr << "    cl_ofs (just-added C) = " << cl_ofs << "\n";
				std::cerr << "    current removed set size = " << removed_clauses_.size() << "\n";
				auto it_scope = learned_clause_scope_.find(ant_ofs);
				if (it_scope != learned_clause_scope_.end()) {
					std::cerr << "    learned clause's recorded scope size = " << it_scope->second.size() << "\n";
					std::cerr << "    scope members:";
					for (auto x : it_scope->second) std::cerr << " " << x;
					std::cerr << "\n";
				} else {
					std::cerr << "    learned clause has NO scope entry"
					          << " (treated as scope = {})\n";
				}
				std::cerr.flush();
				std::abort();
			}
		}
	}

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
			result = solveComponent(comp, separator, separator_reset, depth, nd_node,
			                        reactive_metis_skip_until_depth);
		}
	}

	while (literal_stack_.size() > lit_save) {
		unSet(literal_stack_.back());
		literal_stack_.pop_back();
	}
	unmarkClauseRemoved(cl_ofs);
	if (config_.check_learn_invariants) {
		if (literal_stack_.size() != lit_save) {
			std::cerr << "\n*** INV_T1_TRAIL_NOT_RESTORED (branchOnClause) ***\n"
			          << "  cl_ofs=" << cl_ofs
			          << "  lit_save=" << lit_save
			          << "  current=" << literal_stack_.size() << "\n";
			std::cerr.flush();
			std::abort();
		}
		if (removed_clauses_.size() != snap_removed_size) {
			std::cerr << "\n*** INV_T2_REMOVED_CLAUSES_LEAK (branchOnClause) ***\n"
			          << "  cl_ofs=" << cl_ofs
			          << "  snap=" << snap_removed_size
			          << "  current=" << removed_clauses_.size() << "\n";
			std::cerr.flush();
			std::abort();
		}
		if (learned_clause_scope_.size() < snap_lscope_size) {
			std::cerr << "\n*** INV_T2b_LEARNED_SCOPE_SHRANK (branchOnClause) ***\n"
			          << "  cl_ofs=" << cl_ofs
			          << "  snap=" << snap_lscope_size
			          << "  current=" << learned_clause_scope_.size() << "\n";
			std::cerr.flush();
			std::abort();
		}
	}
	stack_.pop_back();
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
	ana.resetIsolatedPeeledCount();

	// Count active vars in super_comp BEFORE decomposition.
	unsigned super_active = 0;
	for (auto vt = super_comp.varsBegin(); *vt != varsSENTINEL; vt++)
		if (isActive(LiteralID(*vt, true))) super_active++;

	// Note: tried Invariant I1 here (assert every globally-X_TRI var
	// is in super_comp.varsBegin()), but it has legitimate
	// false-positives: at deep recursion levels, super_comp is one of
	// several sibling sub-components, and vars in OTHER siblings are
	// X_TRI but not in THIS super_comp's list. The check would need
	// to be aware of the full sibling set, which isn't accessible
	// here. Removed; needs a more sophisticated invariant or a
	// different angle of attack.

	for (auto vt = super_comp.varsBegin(); *vt != varsSENTINEL; vt++) {
		if (ana.isUnseenAndActive(*vt)) {
			if (ana.exploreRemainingCompOf(*vt)) {
				Component *new_comp = ana.makeComponentFromArcheType();
				result.push_back(new_comp);
			}
			// trivial components are counted via includeSolution(2) on tmp
		}
	}

	// INVARIANT: every active var of super_comp must land in EXACTLY
	// one of: a non-trivial sub-component, or the isolated-peel counter.
	// If a var is silently dropped, we lose a factor of 2 in the count —
	// a concrete undercount bug. Check it here.
	unsigned total_sub_active = 0;
	for (Component *sub : result) {
		for (auto vt = sub->varsBegin(); *vt != varsSENTINEL; vt++)
			if (isActive(LiteralID(*vt, true))) total_sub_active++;
	}
	unsigned isolated = ana.isolatedPeeledCount();
	if (total_sub_active + isolated != super_active) {
		std::cerr << "\n*** COMPONENT_ACCOUNTING_VIOLATED ***\n"
		          << "  super_active   = " << super_active << "\n"
		          << "  sum sub_active = " << total_sub_active
		          << "  (across " << result.size() << " sub-comps)\n"
		          << "  isolated peel  = " << isolated << "\n"
		          << "  missing        = "
		          << ((long)super_active - (long)total_sub_active - (long)isolated)
		          << "\n";
		std::cerr.flush();
		std::abort();
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

Solver::BranchTarget Solver::pickBranchTarget(Component &comp,
                                              const std::vector<CutNode> &separator) {
	BranchTarget best;

	// Build per-call sep sets from the carried `separator` (single
	// source of truth under the unified picker — VARs and CLAUSEs
	// together). Cheap because `separator` is small (typically a few
	// elements).
	std::unordered_set<unsigned> sep_var_set;
	std::unordered_set<unsigned> sep_clause_set;
	for (const auto &nd : separator) {
		if (nd.kind == CutNode::VAR) sep_var_set.insert(nd.id);
		else                         sep_clause_set.insert(nd.id);
	}

	// k = active separator elements in this comp.
	unsigned k = 0;
	for (unsigned vid : sep_var_set)
		if (isActive(LiteralID(vid, true))) k++;
	for (unsigned ofs : sep_clause_set)
		if (!isClauseRemoved((ClauseOfs)ofs)
		    && !isSatisfied((ClauseOfs)ofs)) k++;

	// Optional cross-instance normalization: divide k by (N+M)^p, where
	// (N+M) is the active incidence-graph size of this comp (active vars +
	// active original clauses). p=0 disables (recovers raw k). When p>0,
	// large separators on big formulas decay less than the same-sized
	// separator on a small formula — making the bonus shape comparable
	// across instance scales.
	const double p_norm = config_.separator_size_norm_p;
	double k_eff = (double)k;
	if (p_norm > 0.0 && k > 0) {
		unsigned n_active_vars = 0;
		for (auto it = comp.varsBegin(); *it != varsSENTINEL; ++it)
			if (isActive(LiteralID(*it, true))) n_active_vars++;
		unsigned n_active_clauses = 0;
		for (auto ct = comp.clsBegin(); *ct != clsSENTINEL; ++ct) {
			ClauseOfs ofs = comp_manager_.clauseOfsOf(*ct);
			if (ofs >= (ClauseOfs)original_lit_pool_size_) continue;
			if (isClauseRemoved(ofs) || isSatisfied(ofs)) continue;
			n_active_clauses++;
		}
		unsigned size_nm = n_active_vars + n_active_clauses;
		if (size_nm > 0) {
			k_eff = (double)k / std::pow((double)size_nm, p_norm);
		}
	}

	// Dynamic separator-importance multiplier m = a^(-k_eff). Singletons
	// keep ~1× sepW; longer separators decay. a=1.0 disables (m=1).
	const double a = config_.separator_importance_base;
	const double m = (a > 1.0) ? std::pow(a, -k_eff) : 1.0;
	const float sep_bonus_m = (float)(config_.separator_bias_weight * m);

	// Optional length-weighted cheap_score addend on var scoring,
	// reusing the adaptive picker's BCP-cascade proxy. When weight > 0,
	// vars in many short clauses get an extra positive contribution —
	// designed for BCP-dominated instances where length-agnostic freq
	// misses the high-cascade targets.
	const double cheap_w = config_.cheap_score_weight;
	std::vector<double> cheap_scores;
	if (cheap_w > 0.0) {
		std::vector<VariableIndex> cheap_candidates;  // unused result
		stage0_cheap_scores(comp, config_.stage0_length_decay,
		                    cheap_scores, cheap_candidates);
	}

	// Phase 2a: score VARs. The bonus is applied to vars in the
	// carried separator.
	for (auto it = comp.varsBegin(); *it != varsSENTINEL; ++it) {
		if (!isActive(LiteralID(*it, true))) continue;
		float s = scoreOf(*it);  // freq + activity (no static bias under unified picker)
		if (cheap_w > 0.0 && *it < cheap_scores.size()) {
			s += (float)(cheap_w * cheap_scores[*it]);
		}
		if (sep_var_set.count(*it)) {
			s += sep_bonus_m;
		}
		if (s > best.score) {
			best.kind  = BranchTarget::VAR;
			best.id    = *it;
			best.score = s;
		}
	}

	// Phase 2b: score CLAUSEs (originals only, length above the
	// clause-branching threshold so we don't pick binaries which
	// collapse to var-branching anyway).
	if (config_.perform_clause_branching) {
		const double beta = config_.clause_length_steepness;
		const double mid  = config_.clause_length_midpoint;
		const double cw   = config_.clause_score_weight;
		for (auto ct = comp.clsBegin(); *ct != clsSENTINEL; ++ct) {
			ClauseOfs ofs = comp_manager_.clauseOfsOf(*ct);
			if (ofs >= (ClauseOfs)original_lit_pool_size_) continue;
			if (isClauseRemoved(ofs) || isSatisfied(ofs)) continue;
			unsigned active_len = 0;
			for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; ++lt)
				if (literal_values_[*lt] == X_TRI) active_len++;
			if (active_len < config_.clause_branch_min_length) continue;
			// Sigmoid: 1/(1+exp(-β·(L-c))). Increasing in L —
			// longer clauses are better branching targets (more
			// lits pinned in the negated branch ⇒ deeper BCP cascade).
			double sig = 1.0 / (1.0 + std::exp(-beta * ((double)active_len - mid)));
			float s = (float)(cw * sig);
			if (sep_clause_set.count((unsigned)ofs)) {
				s += sep_bonus_m;
			}
			if (s > best.score) {
				best.kind  = BranchTarget::CLAUSE;
				best.id    = (unsigned)ofs;
				best.score = s;
			}
		}
	}

	return best;
}

