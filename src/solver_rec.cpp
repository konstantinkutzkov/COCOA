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

// LRU-style dedup for recently-learned implicant signatures. A signature
// is a 64-bit hash of the sorted variable IDs + polarity bits of the
// clause literals. Fixed-capacity ring: when full, oldest entry is evicted.
namespace {
constexpr size_t kImplicantDedupCap = 4096;
struct ImplicantDedup {
  std::unordered_set<uint64_t> set;
  std::deque<uint64_t> order;
  bool insert(uint64_t sig) {
    if (set.count(sig)) return false;
    if (set.size() >= kImplicantDedupCap) {
      uint64_t oldest = order.front();
      order.pop_front();
      set.erase(oldest);
    }
    set.insert(sig);
    order.push_back(sig);
    return true;
  }
  void clear() { set.clear(); order.clear(); }
};
thread_local ImplicantDedup g_impl_dedup;

static uint64_t implicantSignature(const std::vector<LiteralID> &clause) {
  std::vector<uint32_t> raws;
  raws.reserve(clause.size());
  for (auto l : clause) raws.push_back(l.raw());
  std::sort(raws.begin(), raws.end());
  // FNV-1a
  uint64_t h = 0xcbf29ce484222325ULL;
  for (uint32_t r : raws) {
    h ^= (uint64_t)r;
    h *= 0x100000001b3ULL;
  }
  return h;
}
}  // namespace

// Walk antecedent chain backward from l_star, collecting decision
// literals (ante == NOT_A_CLAUSE, DL > 0) visited during the walk.
// Returns empty vector if the collected set would exceed max_size
// (bail-early signal: "too big, skip learning").
std::vector<LiteralID> Solver::deriveDecisionImplicant(
    LiteralID l_star, unsigned max_size)
{
  // Guard: the walk starts from a currently-true literal. If l_star
  // isn't true, the caller invoked us with a stale/unassigned literal.
  assert(literal_values_[l_star] == T_TRI
         && "deriveDecisionImplicant: l_star must be currently true");

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

    auto impl = deriveDecisionImplicant(l, config_.implicant_max_size);
    if (impl.empty()) {
      statistics_.num_implicants_size_dropped_++;
      continue;
    }
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

    // Dedup via signature.
    uint64_t sig = implicantSignature(clause);
    if (!g_impl_dedup.insert(sig)) {
      statistics_.num_implicants_dedup_dropped_++;
      continue;
    }

    // Learn via the scoped-UIP machinery: automatic scope tagging,
    // guard-padding for size==2, correct behaviour under clause
    // branching.
    addScopedUIPConflictClause(clause);
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

mpz_class Solver::solveComponent(Component &comp,
                                  vector<CutNode> separator,
                                  bool separator_reset,
                                  int depth,
                                  int nd_node,
                                  int reactive_metis_skip_until_depth) {
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
				if (mt == -2) {
					std::cerr << "\n*** SEPARATOR_INVARIANT_VIOLATED ***\n"
					          << "  nd_node=" << nd_node
					          << " sub_vars.size()=" << sub_vars.size()
					          << " removed_clauses=" << removed_clauses_.size()
					          << "\n  sub_vars leaves:";
					for (unsigned v : sub_vars) {
						int l = ((size_t)v < nd_hierarchy_.var_leaf.size())
						        ? nd_hierarchy_.var_leaf[v] : -1;
						std::cerr << " " << v << "(leaf=" << l << ")";
					}
					std::cerr << "\n  child subtrees:"
					          << " left=[" << nd_hierarchy_.leaf_lo[nd_hierarchy_.left_child[nd_node]]
					          << ".." << nd_hierarchy_.leaf_hi[nd_hierarchy_.left_child[nd_node]] << "]"
					          << " right=[" << nd_hierarchy_.leaf_lo[nd_hierarchy_.right_child[nd_node]]
					          << ".." << nd_hierarchy_.leaf_hi[nd_hierarchy_.right_child[nd_node]] << "]"
					          << std::endl;
					std::cerr.flush();
					std::abort();
				}
				child_nd_node = mt;  // >= 0 or -1 (both valid/legitimate)
			}

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
			sub_count = solveComponent(*sub, {}, true, depth + 1, child_nd_node,
			                           reactive_metis_skip_until_depth);
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
						// Gate 1: structural bisection quality.
						double g1_ratio = (double)r.separator.size()
						                   / (double)n_active_quick;
						unsigned Ltot = r.left_vars + r.right_vars;
						double g1_balance = (Ltot > 0)
						    ? (double)std::min(r.left_vars, r.right_vars)
						      / (double)Ltot
						    : 0.0;
						if (g1_ratio > config_.separator_max_ratio
						    || g1_balance < config_.separator_min_balance) {
							accept = false;
							reactive_metis_gate1_rej_++;
							if (config_.verbose) {
								std::cerr << "  SEP_REJECT_REACTIVE gate=1"
								          << " depth=" << depth
								          << " sep=" << r.separator.size()
								          << " n=" << n_active_quick
								          << " ratio=" << g1_ratio
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
						separator = std::move(r.separator);
						nd_node = -1;
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
	VariableIndex v;
	if (config_.perform_adaptive_branching) {
		bool comp_unsat = false;
		v = pickBranchVariableAdaptive(comp, comp_unsat);
		if (comp_unsat) return 0;
	} else {
		v = pickBranchVariable(comp);
	}
	if (v == 0) {
		return 1;
	}
	LiteralID lit_t(v, true), lit_f(v, false);
	bool t_first = literal(lit_t).activity_score_ >
	               literal(lit_f).activity_score_;
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
		// size>=2 gate: addScopedUIPConflictClause handles size==2 via
		// guard-padding; size<2 (empty / unit) is still dropped inside.
		if (!from_separator
		    && !uip_clauses_.empty() && uip_clauses_.back().size() >= 2
		    && uip_clauses_.back().front() == lit.neg()) {
			addScopedUIPConflictClause(uip_clauses_.back());
		}
		result = 0;
	} else {
		// Phase 4: mine implicants from the BCP cascade. Opt-in; no-op
		// by default. Suppressed during separator branching (same
		// discipline as regular clause learning at line 951-955).
		if (!from_separator) {
			maybeLearnImplicants(lit_save);
		}
		result = solveComponent(comp, separator, separator_reset, depth, nd_node,
		                        reactive_metis_skip_until_depth);
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
                                  bool negate_literals, int depth, int nd_node,
                                  int reactive_metis_skip_until_depth) {
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
			result = solveComponent(comp, separator, separator_reset, depth, nd_node,
			                        reactive_metis_skip_until_depth);
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

