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

// Compute a per-variable centrality score from the ND-hierarchy.
// Score = (max_depth − depth_of(v's leaf in the ND-tree)).  Variables
// in shallow leaves (closer to the root) rank higher; variables in deep
// leaves rank lower.  Conceptually analogous to ganak's td_score (which
// derives from TD-centroid distances) — both give a static, per-variable
// structural ranking to anchor branching order across recursion paths.
//
// Indexed by COMPACT var IDs (same indexing as nd_hierarchy_.var_leaf).
// Activated by `config_.nd_centrality_weight > 0`.
void Solver::computeNdCentrality() {
  nd_centrality_score_.clear();
  if (!nd_hierarchy_.valid) return;
  const int n_nodes = (int)nd_hierarchy_.left_child.size();
  if (n_nodes == 0) return;

  // BFS from root to assign per-node depth.
  std::vector<int> depth(n_nodes, -1);
  const int root = nd_hierarchy_.root();
  if (root < 0 || root >= n_nodes) return;
  depth[root] = 0;
  std::queue<int> q;
  q.push(root);
  int max_depth = 0;
  while (!q.empty()) {
    int u = q.front(); q.pop();
    if (depth[u] > max_depth) max_depth = depth[u];
    int lc = nd_hierarchy_.left_child[u];
    int rc = nd_hierarchy_.right_child[u];
    if (lc >= 0 && lc < n_nodes && depth[lc] < 0) { depth[lc] = depth[u]+1; q.push(lc); }
    if (rc >= 0 && rc < n_nodes && depth[rc] < 0) { depth[rc] = depth[u]+1; q.push(rc); }
  }

  // Map leaf_id → tree_node_id (leaves have leaf_lo == leaf_hi and no children).
  int max_leaf = -1;
  for (int n = 0; n < n_nodes; ++n) {
    bool is_leaf = (nd_hierarchy_.left_child[n] < 0 && nd_hierarchy_.right_child[n] < 0);
    if (is_leaf && nd_hierarchy_.leaf_lo[n] >= 0) {
      if (nd_hierarchy_.leaf_lo[n] > max_leaf) max_leaf = nd_hierarchy_.leaf_lo[n];
    }
  }
  if (max_leaf < 0) return;
  std::vector<int> leaf_to_node(max_leaf + 1, -1);
  for (int n = 0; n < n_nodes; ++n) {
    bool is_leaf = (nd_hierarchy_.left_child[n] < 0 && nd_hierarchy_.right_child[n] < 0);
    if (is_leaf && nd_hierarchy_.leaf_lo[n] >= 0
        && nd_hierarchy_.leaf_lo[n] == nd_hierarchy_.leaf_hi[n]) {
      leaf_to_node[nd_hierarchy_.leaf_lo[n]] = n;
    }
  }

  // Per-variable centrality. Score = max_depth − depth_of(leaf_node(v)).
  const size_t n_v = nd_hierarchy_.var_leaf.size();
  nd_centrality_score_.assign(n_v, 0.0f);
  for (size_t v = 1; v < n_v; ++v) {
    int leaf_id = nd_hierarchy_.var_leaf[v];
    if (leaf_id < 0 || leaf_id > max_leaf) continue;
    int node = leaf_to_node[leaf_id];
    if (node < 0 || depth[node] < 0) continue;
    nd_centrality_score_[v] = (float)(max_depth - depth[node]);
  }
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
		computeNdCentrality();
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

	// Preprocessing-style decomposition: peel free vars and split the
	// root formula into connected components before main solve.
	mpz_class result;
	mpz_class trivial_factor = 1;
	std::vector<Component*> subcomps =
	    discoverComponentsOf(root, trivial_factor);
	if (config_.verbose || !config_.quiet) {
		unsigned root_active = 0;
		for (auto vt = root.varsBegin(); *vt != varsSENTINEL; vt++)
			if (isActive(LiteralID(*vt, true))) root_active++;
		std::cerr << "ROOT_DECOMP: root_active=" << root_active
		          << " subcomps=" << subcomps.size()
		          << " trivial_factor=" << trivial_factor;
		for (size_t i = 0; i < subcomps.size(); i++) {
			unsigned sub_active = 0;
			for (auto vt = subcomps[i]->varsBegin(); *vt != varsSENTINEL; vt++)
				if (isActive(LiteralID(*vt, true))) sub_active++;
			std::cerr << " sub" << i << "=" << sub_active;
		}
		std::cerr << "\n";
	}
	result = trivial_factor;
	for (Component *sub : subcomps) {
		mpz_class sub_count =
		    solveComponent(*sub, {}, true, 0, start_node);
		result *= sub_count;
		delete sub;
		if (result == 0) break;
	}
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
	// Progress accounting: capture this subtree's size at entry. Reset
	// the my_call_recursed_ flag for THIS call (saving the parent's
	// value to restore on exit). On exit: count this subtree as a LEAF
	// event iff our body did NOT recurse into solveComponent. Set the
	// flag to true on exit so the parent's solveComponent observes that
	// it DID recurse (= it's not a leaf, doesn't count).
	// Timeout return is NOT a backtrack — we abandoned, don't count.
	unsigned k_at_entry = comp.num_variables();
	bool parent_recursed_flag = my_call_recursed_;
	my_call_recursed_ = false;

	if (stopwatch_.timeBoundBroken()) {
		// Capture chain (outer levels) if not already captured.
		if (!open_work_captured_) {
			captureOpenWorkSnapshot();
			open_work_captured_ = true;
		}
		my_call_recursed_ = parent_recursed_flag;  // restore unchanged
		return 0;
	}
	// Diagnostics: accumulate component sizes seen at solveComponent entry.
	// Lets us compute avg comp size processed vs avg comp size that hits
	// the cache, telling apart "we cache big amortized chunks" from
	// "we cache trivial singletons".
	statistics_.sum_comp_vars_at_entry_ += comp.num_variables();
	statistics_.num_comp_entries_++;

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
		    original_lit_pool_size_, config_.wl_iterations,
		    static_wl_labels_.empty() ? nullptr : &static_wl_labels_);
		key_built = true;
		free_vars = (cached_key.num_vars > cached_key.n_in_clauses)
		    ? (cached_key.num_vars - cached_key.n_in_clauses) : 0;
		// Diagnostic: track distinct canonical keys (full 128-bit) vs total calls.
		static long long total_calls = 0;
		struct PairHash {
			size_t operator()(const std::pair<uint64_t,uint64_t>& p) const {
				return p.first ^ (p.second << 1);
			}
		};
		static std::unordered_set<std::pair<uint64_t,uint64_t>, PairHash> distinct_keys;
		total_calls++;
		distinct_keys.insert({cached_key.hash, cached_key.hash_hi});
		if (config_.log_branches && total_calls % 2000 == 0) {
			std::cerr << "CANON_DIAG calls=" << total_calls
			          << " distinct_keys=" << distinct_keys.size()
			          << " duplicate_ratio="
			          << (1.0 - (double)distinct_keys.size() / (double)total_calls)
			          << "\n";
		}
		mpz_class hit;
		if (comp_manager_.contentCache().peek(cached_key, hit)) {
			auto &cc = comp_manager_.contentCache();
			cc.stats_hits++;
			cc.stats_hit_vars_sum += cached_key.num_vars;
			if (cached_key.num_vars > cc.stats_max_hit_size)
				cc.stats_max_hit_size = cached_key.num_vars;
			cc.stats_hit_buckets[ContentCache::size_bucket(cached_key.num_vars)]++;
			mpz_class scaled = hit;
			for (unsigned i = 0; i < free_vars; ++i) scaled *= 2;
			// Cache hit = leaf event (didn't go through solveComponentImpl,
			// so didn't recurse). Always count.
			noteBacktrackedSubtree(k_at_entry);
			my_call_recursed_ = true;  // parent sees us as recursion
			return scaled;
		}
		comp_manager_.contentCache().stats_misses++;
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
	// LEAF event iff solveComponentImpl did NOT recurse — i.e., it
	// computed via BCP closure or base case without further branching.
	// Branching solveComponentImpl sets my_call_recursed_ to true via
	// its child solveComponent calls.
	if (!my_call_recursed_) {
		noteBacktrackedSubtree(k_at_entry);
	}
	my_call_recursed_ = true;  // parent sees us as recursion
	return result;
}

mpz_class Solver::solveComponentImpl(Component &comp,
                                  vector<CutNode> separator,
                                  bool separator_reset,
                                  int depth,
                                  int nd_node,
                                  int reactive_metis_skip_until_depth) {
	// Push this comp onto the chain of in-progress sub-components for
	// the OPEN_WORK snapshot. RAII guard ensures we pop on every exit
	// path (normal return, timeout return, etc.).
	struct ChainGuard {
		std::vector<Component *> &chain;
		ChainGuard(std::vector<Component *> &c, Component *p) : chain(c) {
			chain.push_back(p);
		}
		~ChainGuard() { chain.pop_back(); }
	} _chain_guard(current_comp_chain_, &comp);

	if (stopwatch_.timeBoundBroken()) {
		// First detection wins — captures the deepest stack snapshot
		// available, before unwinding starts dropping frames.
		if (!open_work_captured_) {
			captureOpenWorkSnapshot();
			open_work_captured_ = true;
		}
		return 0;
	}

	// Periodic PROGRESS emit (diagnostic; cheap when off-tick).
	// Tick interval: SHARPSAT_PROGRESS_INTERVAL seconds (default 2.0).
	// Only emitted when env SHARPSAT_PROGRESS=1.
	{
		static const bool progress_enabled = []() {
			const char *e = std::getenv("SHARPSAT_PROGRESS");
			return e && e[0] == '1';
		}();
		static const double progress_interval = []() {
			const char *e = std::getenv("SHARPSAT_PROGRESS_INTERVAL");
			return (e && *e) ? std::atof(e) : 2.0;
		}();
		if (progress_enabled) {
			double now = stopwatch_.getElapsedSeconds();
			if (last_progress_emit_s_ < 0.0 || now - last_progress_emit_s_ >= progress_interval) {
				captureOpenWorkSnapshot();  // overwrites fields; OK — final snapshot will rewrite
				double pb = (double)open_work_n_root_ - open_work_log2_bound_;
				// closed_bits: monotone log_sum_exp over every cache store
				// since solve start. fraction_done = 2^(closed - n_root).
				double closed = (closed_log_sum_ == -std::numeric_limits<double>::infinity())
				              ? 0.0 : closed_log_sum_;
				std::cerr << "PROGRESS t=" << now
				          << " progress_bits=" << pb
				          << " closed_bits=" << closed
				          << " open=" << open_work_n_open_
				          << " bound_log2=" << open_work_log2_bound_
				          << " decisions=" << statistics_.num_decisions_
				          << " l2_hits=" << comp_manager_.contentCache().stats_hits
				          << std::endl;
				last_progress_emit_s_ = now;
			}
		}
	}

	if (config_.log_branches && depth == 0) {
		static int g_root_calls = 0;
		g_root_calls++;
		unsigned act = 0, total = 0;
		for (auto vt = comp.varsBegin(); *vt != varsSENTINEL; vt++) {
			total++;
			if (isActive(LiteralID(*vt, true))) act++;
		}
		std::cerr << "ROOT_ENTRY call=" << g_root_calls
		          << " sep_size=" << separator.size()
		          << " sep_reset=" << separator_reset
		          << " comp_total_vars=" << total
		          << " comp_active=" << act
		          << " trail=" << literal_stack_.size() << "\n";
	}

	static int rec_depth = 0;
	static long long call_count = 0;
	call_count++;
	struct DG { int &d; DG(int &x):d(x){d++;} ~DG(){d--;} } guard(rec_depth);
	if (call_count % 100000 == 0) {
		cerr << "calls=" << call_count << " depth=" << rec_depth
		     << " decisions=" << statistics_.num_decisions_
		     << " conflicts=" << statistics_.num_conflicts_
		     << " stores=" << comp_manager_.contentCache().stats_stores
		     << " hits=" << comp_manager_.contentCache().stats_hits
		     << " l1_hits=" << comp_manager_.contentCache().stats_l1_hits
		     << endl;
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
				int sub_nd_eff = config_.picker_root_sep_only ? -1 : sub_nd_node;
				mpz_class sub_count = solveComponent(
				    *sub, std::move(sub_sep), sub_reset,
				    depth + 1, sub_nd_eff,
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
	// "Separator exhausted" gate. Under the default consumption path,
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
		// Diagnostic: count decomposition events by outcome.
		static long long g_decomp_calls = 0, g_decomp_split = 0;
		static long long g_decomp_trivial = 0;
		g_decomp_calls++;
		if (subcomps.size() > 1) g_decomp_split++;
		else g_decomp_trivial++;
		if (config_.log_branches && g_decomp_calls % 1000 == 0) {
			std::cerr << "DECOMP_DIAG post_calls=" << g_decomp_calls
			          << " post_split=" << g_decomp_split
			          << " post_trivial=" << g_decomp_trivial
			          << " mid_attempts=" << mid_sep_decomp_attempts_
			          << " mid_splits=" << mid_sep_decomp_splits_
			          << "\n";
		}
		// Fast path: no real decomposition (1 sub identical to comp,
		// no peeled free vars). Recursing here would lose the parent's
		// hierarchy lineage — child_nd_node would be -1 for sub-comps
		// that span both children of nd_node — so the deeper acceptance
		// rounds wouldn't find any precomputed separator. Instead, free
		// the sub-comp (it aliases comp) and fall through to the picker
		// at the current level: same nd_node, same accepted separator,
		// residual just slightly smaller from BCP.
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
				// Under -decomposeInSep / -unifiedPicker, the precomputed
				// separator no longer strictly cuts the residual. A
				// sub-comp may legitimately span both L and R children
				// of nd_node; treat -2 as "no clean child mapping" and
				// descend without hierarchy guidance.
				if (mt == -2
				    && (config_.decompose_in_separator
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
				original_lit_pool_size_, config_.wl_iterations,
				static_wl_labels_.empty() ? nullptr : &static_wl_labels_);
			bool hit = (config_.perform_component_caching &&
			            sub->num_variables() >= 3 &&
			            comp_manager_.contentCache().peek(key, sub_count));
			if (hit) {
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
			// Miss — recurse on sub-component
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
			int sub_nd = config_.picker_root_sep_only ? -1 : child_nd_node;
			sub_count = solveComponent(*sub, {}, true, depth + 1, sub_nd,
			                           reactive_metis_skip_until_depth);


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
				comp_manager_.contentCache().store(key, sub_count);
				// Populate L1 so future visits to the same ID-set skip
				// the canonical build.
				comp_manager_.contentCache().l1_store(id_key, sub_count);
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
				if (config_.separator_clauses_first
				    && !separator.empty()) {
					// Reorder: all CLAUSEs first, then all VARs (within-kind
					// order preserved). Consumption loop then branches
					// clauses first, vars after. Helps on small density-1
					// (sees t1_021_k10_s1 4.4s vs 5.0s no-flag).
					std::vector<CutNode> reordered;
					reordered.reserve(separator.size());
					for (const auto &nd : separator)
						if (nd.kind == CutNode::CLAUSE) reordered.push_back(nd);
					for (const auto &nd : separator)
						if (nd.kind == CutNode::VAR)    reordered.push_back(nd);
					separator = std::move(reordered);
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
						if (config_.separator_clauses_first) {
							// Reorder: all CLAUSEs first, then all VARs.
							std::vector<CutNode> reordered;
							reordered.reserve(r.separator.size());
							for (const auto &nd : r.separator)
								if (nd.kind == CutNode::CLAUSE) reordered.push_back(nd);
							for (const auto &nd : r.separator)
								if (nd.kind == CutNode::VAR)    reordered.push_back(nd);
							separator = std::move(reordered);
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
		// Empty-component sentinel: score stays at -1.0f if no candidate
		// was found (the picker's `best` is default-initialized that way).
		if (tgt.score < 0.0f) return 1;
		if (tgt.kind == BranchTarget::VAR) {
			VariableIndex v = tgt.id;
			LiteralID lit_t(v, true), lit_f(v, false);
			bool t_first = literal(lit_t).activity_score_
			               > literal(lit_f).activity_score_;
			// Sep VAR pick = consuming a planned separator element →
			// from_separator=true (suppress learning, matches legacy
			// Stage-2). Non-sep VAR = freely branched, learning enabled.
			bool v_in_sep = false;
			for (const auto &nd : separator) {
				if (nd.kind == CutNode::VAR && nd.id == (unsigned)v) {
					v_in_sep = true;
					break;
				}
			}
			// kills_nd: drop ND descent for non-sep var picks. Sep var
			// picks keep nd_node alive (consumption spine).
			int nd_fwd = (config_.picker_non_sep_kills_nd && !v_in_sep)
			                 ? -1 : nd_node;
			mpz_class A = branchOnLiteral(t_first ? lit_t : lit_f, comp,
			                              separator, false, depth, nd_fwd,
			                              v_in_sep,
			                              reactive_metis_skip_until_depth);
			mpz_class B = branchOnLiteral(t_first ? lit_f : lit_t, comp,
			                              separator, false, depth, nd_fwd,
			                              v_in_sep,
			                              reactive_metis_skip_until_depth);
			return A + B;
		} else {
			ClauseOfs ofs = (ClauseOfs)tgt.id;
			// Sep-clause picks are the consumption spine — keep
			// nd_node alive so descendants can install fresh seps.
			// Non-sep clause picks are off-spine, treat like vars.
			bool is_sep_clause = false;
			for (const auto &nd : separator) {
				if (nd.kind == CutNode::CLAUSE && nd.id == (unsigned)ofs) {
					is_sep_clause = true;
					break;
				}
			}
			int nd_fwd = (config_.picker_non_sep_kills_nd && !is_sep_clause)
			                 ? -1 : nd_node;
			mpz_class A = branchOnClause(ofs, comp, separator, false,
			                              false, depth, nd_fwd,
			                              reactive_metis_skip_until_depth);
			mpz_class B = branchOnClause(ofs, comp, separator, false,
			                              true, depth, nd_fwd,
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
	// Snapshot active-var count of the parent component (the one being
	// decomposed at this level) for the OPEN_WORK polarity-2 estimate.
	{
		unsigned active = 0;
		for (auto vt = comp.varsBegin(); *vt != varsSENTINEL; vt++)
			if (isActive(LiteralID(*vt, true))) active++;
		stack_.back().set_active_at_push(active);
	}

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
		// State hash: simple FNV over (full literal_stack_ in order) +
		// (sorted active clause IDs that aren't satisfied/removed).
		uint64_t h = 14695981039346656037ULL;
		for (auto l : literal_stack_) {
			h ^= (uint64_t)l.toInt();
			h *= 1099511628211ULL;
		}
		std::vector<unsigned> active_cls;
		for (unsigned i = 0; i < removed_clauses_.size() + 1000 && i < 50000; i++) {
			(void)i;
		}
		// Iterate active orig clauses via instance stats — quick proxy:
		// hash the count of currently-active vars (sufficient with trail
		// hash for verifying same state across solvers).
		unsigned act_v = 0;
		for (unsigned v = 1; v <= num_variables(); v++)
			if (isActive(LiteralID(v, true))) act_v++;
		h ^= (uint64_t)act_v;
		h *= 1099511628211ULL;
		std::cerr << " state_hash=0x" << std::hex << h << std::dec
		          << " active=" << act_v
		          << " trail=" << literal_stack_.size();
		std::cerr << "\n";
	}

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
		// Learning ladder (see solver_config.h::learn_level):
		//   level 0 → don't learn at all
		//   level 1 → learn but skip dedup
		//   level 2 → + dedup (no scope)
		//   level 3 → + scope (no binary padding)
		//   level 4 → + binary padding
		//   level 5 → + minimization (handled in minimizeAndStoreUIPClause)
		const int L = config_.learn_level;
		// Suppression-gate. The legacy invariant was "do not learn during
		// separator consumption" because a learned clause might bridge
		// the hierarchy's children. With the per-sub-component membership
		// filter in place (binary lane + long-clause learnedClauseInComponent),
		// any such bridge clause is dismissed at the next descent — so the
		// suppression is no longer load-bearing for soundness. Default is
		// to allow; set config_.allow_learning_in_separator_branching =
		// false to restore the legacy behaviour for A/B measurement.
		bool sep_gate_ok = !from_separator
		                || config_.allow_learning_in_separator_branching;
		if (sep_gate_ok
		    && L >= 1
		    && !uip_clauses_.empty() && uip_clauses_.back().size() >= 2
		    && uip_clauses_.back().front() == lit.neg()) {
			// Dedup via Bloom filter (shared with commitFailedLiteral).
			// FP = skip learning = sound.
			bool go = (L <= 1) || maybeDedupClause(uip_clauses_.back());
			if (go) {
				addScopedUIPConflictClause(
				    uip_clauses_.back(),
				    /*pad_binary=*/  L >= 4,
				    /*record_scope=*/L >= 3);
			} else {
				statistics_.num_learned_dedup_dropped_++;
			}
		}
		result = 0;
	} else {
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
	// Snapshot active-var count of parent (for OPEN_WORK metric).
	{
		unsigned active = 0;
		for (auto vt = comp.varsBegin(); *vt != varsSENTINEL; vt++)
			if (isActive(LiteralID(*vt, true))) active++;
		stack_.back().set_active_at_push(active);
	}

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
	unsigned active_count = 0;
	for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++) {
		if (!isActive(LiteralID(*it, true))) continue;
		active_count++;
		float s = scoreOf(*it);
		if (s > best_score) {
			best_score = s;
			best = *it;
		}
	}
	if (config_.log_branches && best != 0) {
		static long long g_leg_call = 0;
		g_leg_call++;
		float fq = (float)comp_manager_.scoreOf(best);
		float ap = literal(LiteralID(best, true)).activity_score_;
		float an = literal(LiteralID(best, false)).activity_score_;
		std::cerr << "PICK_CALL_LEG id=" << g_leg_call
		          << " active=" << active_count
		          << " v=" << best
		          << " freq=" << fq
		          << " ap=" << ap
		          << " an=" << an
		          << " score=" << best_score
		          << "\n";
	}
	return best;
}

// The unified (-unifiedPicker) branch-target picker.
//
//   base(v)  = picker_var_weight · max(ε,
//                  freq + 10·a+ + 10·a- + cheapW·cheap + cascadeW·cascade(v))
//   base(C)  = picker_clause_weight · sigmoid(β·(L − mid))
//   boost(v) = 1 + picker_alpha_var    · exp(−picker_lambda_var    · rel_k) · 1[v ∈ sep]
//   boost(C) = 1 + picker_alpha_clause · exp(−picker_lambda_clause · rel_k) · 1[C ∈ sep]
//   S(x)     = base(x) · boost(x)
//   rel_k    = (active sep elements) / N_active_vars
//
// Multiplicative ("soft preference") scoring: a sep element gets its base
// score scaled by (1 + boost), so the picker prefers sep candidates whose
// base is comparable to non-sep alternatives, but lets a strong non-sep
// candidate win. Empirically more robust than additive (which timed out
// catastrophically on t1_071) and than the rate framework. See
// docs/history/unified_picker_redesign.md for the comparison results.
//
// Type-pure: var-base never sums clause terms, vice versa. Cross-type
// competition only via picker_var_weight / picker_clause_weight ratio.
Solver::BranchTarget Solver::pickBranchTarget(
    Component &comp,
    const std::vector<CutNode> &separator) {
	BranchTarget best;

	// Build sep sets from the carried `separator` (vars + clauses).
	std::unordered_set<unsigned> sep_var_set;
	std::unordered_set<unsigned> sep_clause_set;
	for (const auto &nd : separator) {
		if (nd.kind == CutNode::VAR) sep_var_set.insert(nd.id);
		else                         sep_clause_set.insert(nd.id);
	}

	// Count active sep elements (numerator of rel_k).
	unsigned n_sep_active = 0;
	for (unsigned vid : sep_var_set)
		if (isActive(LiteralID(vid, true))) n_sep_active++;
	for (unsigned ofs : sep_clause_set)
		if (!isClauseRemoved((ClauseOfs)ofs)
		    && !isSatisfied((ClauseOfs)ofs)) n_sep_active++;

	// Count active vars in the comp (denominator of rel_k).
	unsigned n_active_vars = 0;
	for (auto it = comp.varsBegin(); *it != varsSENTINEL; ++it)
		if (isActive(LiteralID(*it, true))) n_active_vars++;

	// Separator-shortness gate (constant across all sep elements at
	// this node; type-specific α and λ produce two boost values).
	const double rel_k = (n_active_vars > 0)
	    ? (double)n_sep_active / (double)n_active_vars
	    : 0.0;
	const double alpha_var     = config_.picker_alpha_var;
	const double lambda_var    = config_.picker_lambda_var;
	const double alpha_clause  = config_.picker_alpha_clause;
	const double lambda_clause = config_.picker_lambda_clause;
	const float boost_sep_value_var =
	    (float)(alpha_var * std::exp(-lambda_var * rel_k));
	const float boost_sep_value_clause =
	    (float)(alpha_clause * std::exp(-lambda_clause * rel_k));

	const double varW      = config_.picker_var_weight;
	const double clauseW   = config_.picker_clause_weight;
	const double cheap_w   = config_.cheap_score_weight;
	const double cascade_w = config_.cascade_score_weight;
	// Tiny floor on var base score so a sep boost still has a non-zero
	// multiplicand for vars with no freq/activity signal. Hardcoded
	// because no production tuning touched the previous knob; if you
	// need to tune this, edit the constant.
	const float  eps       = 0.01f;

	// Optional cheap-score precomputation (matches additive path).
	std::vector<double> cheap_scores;
	if (cheap_w > 0.0) {
		std::vector<VariableIndex> cheap_candidates;
		stage0_cheap_scores(comp, config_.stage0_length_decay,
		                    cheap_scores, cheap_candidates);
	}

	// Diagnostic: dump the first N picks where the separator has been
	// (mostly) exhausted — i.e., post-branching state. n_sep_active
	// counts active sep elements; we only dump when it's 0, so cascade
	// scores reflect the residual after separator consumption.
	static long long g_dump_pick_count = 0;
	const long long DUMP_PICK_LIMIT = 5;
	const size_t TOP_K_DUMP = 8;
	bool do_dump = config_.log_branches
	               && g_dump_pick_count < DUMP_PICK_LIMIT
	               && n_sep_active == 0;
	struct CandEntry { char kind; unsigned id; float raw; float base;
	                    float boost; float score; int in_sep; unsigned len; };
	std::vector<CandEntry> entries;
	if (do_dump) entries.reserve(64);

	// Phase 2a: score VARs.
	for (auto it = comp.varsBegin(); *it != varsSENTINEL; ++it) {
		if (!isActive(LiteralID(*it, true))) continue;
		float raw = (float)comp_manager_.scoreOf(*it);
		raw += 10.0f * literal(LiteralID(*it, true)).activity_score_;
		raw += 10.0f * literal(LiteralID(*it, false)).activity_score_;
		if (cheap_w > 0.0 && *it < cheap_scores.size()) {
			raw += (float)(cheap_w * cheap_scores[*it]);
		}
		if (cascade_w > 0.0) {
			raw += (float)(cascade_w * computeBcpGainScore(*it));
		}
		if (raw < eps) raw = eps;
		float base = (float)varW * raw;
		bool is_sep = sep_var_set.count(*it) > 0;
		float boost = 1.0f + (is_sep ? boost_sep_value_var : 0.0f);
		float s = base * boost;
		if (do_dump) entries.push_back({'V', *it, raw, base, boost,
		                                 s, is_sep ? 1 : 0, 0});
		if (s > best.score) {
			best.kind  = BranchTarget::VAR;
			best.id    = *it;
			best.score = s;
		}
	}

	// Phase 2b: score CLAUSEs (originals, length ≥ clause_branch_min_length).
	if (config_.perform_clause_branching) {
		const double beta = config_.clause_length_steepness;
		const double mid  = config_.clause_length_midpoint;
		for (auto ct = comp.clsBegin(); *ct != clsSENTINEL; ++ct) {
			ClauseOfs ofs = comp_manager_.clauseOfsOf(*ct);
			if (ofs >= (ClauseOfs)original_lit_pool_size_) continue;
			if (isClauseRemoved(ofs) || isSatisfied(ofs)) continue;
			unsigned active_len = 0;
			for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; ++lt)
				if (literal_values_[*lt] == X_TRI) active_len++;
			if (active_len < config_.clause_branch_min_length) continue;
			double sig = 1.0 /
			    (1.0 + std::exp(-beta * ((double)active_len - mid)));
			float base = (float)(clauseW * sig);
			if (base < eps) base = eps;
			bool is_sep = sep_clause_set.count((unsigned)ofs) > 0;
			float boost = 1.0f + (is_sep ? boost_sep_value_clause : 0.0f);
			float s = base * boost;
			if (do_dump) entries.push_back({'C', (unsigned)ofs, (float)sig,
			                                 base, boost, s,
			                                 is_sep ? 1 : 0, active_len});
			if (s > best.score) {
				best.kind  = BranchTarget::CLAUSE;
				best.id    = (unsigned)ofs;
				best.score = s;
			}
		}
	}

	if (do_dump) {
		g_dump_pick_count++;
		std::sort(entries.begin(), entries.end(),
		          [](const CandEntry &a, const CandEntry &b)
		          { return a.score > b.score; });
		std::cerr << "===== PICK_DUMP #" << g_dump_pick_count
		          << " rel_k=" << rel_k
		          << " sep_var_active=" << sep_var_set.size()
		          << " sep_clause_active=" << sep_clause_set.size()
		          << " boost_var=" << boost_sep_value_var
		          << " boost_clause=" << boost_sep_value_clause
		          << " (top " << std::min(TOP_K_DUMP, entries.size())
		          << " of " << entries.size() << ") =====\n";
		for (size_t i = 0; i < std::min(TOP_K_DUMP, entries.size()); ++i) {
			const auto &e = entries[i];
			std::cerr << "  " << (i+1) << ". kind=" << e.kind
			          << " id=" << e.id
			          << " in_sep=" << e.in_sep
			          << (e.kind == 'C' ? " len=" : "")
			          << (e.kind == 'C' ? std::to_string(e.len) : std::string(""))
			          << " raw=" << e.raw
			          << " base=" << e.base
			          << " boost=" << e.boost
			          << " score=" << e.score << "\n";
		}
	}

	if (config_.log_branches) {
		bool in_sep = (best.kind == BranchTarget::VAR
		                ? sep_var_set.count(best.id) > 0
		                : sep_clause_set.count(best.id) > 0);
		float bs = (best.kind == BranchTarget::VAR
		             ? boost_sep_value_var
		             : boost_sep_value_clause);
		std::cerr << "MULT_PICK kind="
		          << (best.kind == BranchTarget::VAR ? "VAR" : "CLAUSE")
		          << " id=" << best.id
		          << " in_sep=" << (in_sep ? 1 : 0)
		          << " rel_k=" << rel_k
		          << " boost_sep=" << bs
		          << " score=" << best.score
		          << "\n";
	}
	return best;
}

