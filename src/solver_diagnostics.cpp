/*
 * solver_diagnostics.cpp
 *
 * Bug-investigation and debugging support for the Solver class.
 * Functions here are NOT on any production hot path. They are
 * either:
 *   - opt-in invariant checks (verify* family),
 *   - opt-in dump-and-inspect tools (dump* family),
 *   - opt-in brute-force verifiers (bruteForce* family),
 *   - test helpers for canonical-key tests (_*ForTest, _prepare*).
 *
 * Behaviorally identical to the same code when it lived in solver.cpp;
 * relocated here so solver.cpp focuses on the main solve pipeline.
 */

#include "solver.h"
#include "canonical_key.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>



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
		bool skip = isClauseRemoved(ofs);
		// Out-of-scope learned clauses are NOT used by BCP (it skips them
		// via the learnedClauseInScope check). They may legitimately have
		// only one active literal under the current state without that
		// being a BCP-saturation violation.
		if (!skip && ofs >= (ClauseOfs)original_lit_pool_size_
		    && !learnedClauseInScope(ofs))
			skip = true;
		if (skip) {
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
		          << " free_lit=" << violating_free_lit.toInt() << "\n";
		if (!violating_is_binary) {
			std::cerr << "  is_learned=" << (violating_clause >= (ClauseOfs)original_lit_pool_size_ ? 1 : 0)
			          << " is_removed=" << (isClauseRemoved(violating_clause) ? 1 : 0)
			          << " in_scope=" << (learnedClauseInScope(violating_clause) ? 1 : 0)
			          << " removed_clauses_size=" << removed_clauses_.size() << "\n";
			std::cerr << "  clause literals (lit:val):";
			for (auto lt = beginOf(violating_clause); *lt != SENTINEL_LIT; lt++) {
				const char *tag =
				    (literal_values_[*lt] == T_TRI) ? "(T)" :
				    (literal_values_[*lt] == F_TRI) ? "(F)" : "(X)";
				std::cerr << " " << lt->toInt() << tag;
			}
			std::cerr << "\n";
		}
		std::cerr << "  This literal should have been unit-propagated.\n";
		std::cerr.flush();
		std::abort();
	}
}

// Invariant guard: verify internal representational integrity after
// preprocessing (or any other boundary where state should be clean).
// See declaration in solver.h for the full contract.
void Solver::verifyStateIntegrity(const char *label) {
	auto fire = [&](const std::string &msg) {
		std::cerr << "\n*** STATE_INTEGRITY (" << label << "): "
		          << msg << " ***\n";
		std::cerr.flush();
		std::abort();
	};

	// Check 1: literal-values polarity.
	for (unsigned v = 1; v < variables_.size(); v++) {
		TriValue pos = literal_values_[LiteralID(v, true)];
		TriValue neg = literal_values_[LiteralID(v, false)];
		if (pos == T_TRI && neg != F_TRI) fire("var " + std::to_string(v)
		    + " has literal_values_[pos]=T but neg != F");
		if (pos == F_TRI && neg != T_TRI) fire("var " + std::to_string(v)
		    + " has literal_values_[pos]=F but neg != T");
		if (pos == X_TRI && neg != X_TRI) fire("var " + std::to_string(v)
		    + " has literal_values_[pos]=X but neg != X");
	}

	// Check 2: watch-list correctness for long clauses.
	// For every clause C in literal_pool_, the two literals at positions
	// 0 and 1 must have this clause in their watch_list_.
	std::unordered_set<uint64_t> expected_watches;  // (lit.raw(), ofs) pairs
	for (auto it = literal_pool_.begin(); it != literal_pool_.end(); it++) {
		if (*it != SENTINEL_LIT) continue;
		if (it + 1 == literal_pool_.end()) break;
		it += ClauseHeader::overheadInLits();
		ClauseOfs ofs = (ClauseOfs)(it + 1 - literal_pool_.begin());
		auto p0 = literal_pool_.begin() + ofs;
		auto p1 = p0 + 1;
		if (*p0 == SENTINEL_LIT) continue;  // empty clause; skip
		if (*p1 == SENTINEL_LIT) continue;  // length-1 stored clause;
		                                     // unexpected but skip
		expected_watches.insert(((uint64_t)p0->raw() << 32) | ofs);
		expected_watches.insert(((uint64_t)p1->raw() << 32) | ofs);
		// Advance past rest of clause literals.
		auto e = p0;
		while (*e != SENTINEL_LIT) e++;
		it = e - 1;
	}
	// Collect actual watches. Note: watch_list_ layout is
	// [SENTINEL_CL, ofs1, ofs2, ...] — sentinel at position 0, real
	// entries appended after. Solver iterates via .rbegin() for this
	// reason; we do the same.
	std::unordered_set<uint64_t> actual_watches;
	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		const auto &wl = literal(l).watch_list_;
		for (auto wt = wl.rbegin(); *wt != SENTINEL_CL; ++wt) {
			actual_watches.insert(((uint64_t)l.raw() << 32) | (uint64_t)*wt);
		}
	}
	if (expected_watches != actual_watches) {
		unsigned n_missing = 0, n_stale = 0;
		LiteralID first_missing_lit, first_stale_lit;
		ClauseOfs first_missing_ofs = 0, first_stale_ofs = 0;
		std::vector<int> first_missing_clause_lits, first_stale_clause_lits;
		auto dump_clause = [&](ClauseOfs ofs, std::vector<int> &out) {
			for (auto lt = literal_pool_.begin() + ofs;
			     *lt != SENTINEL_LIT; lt++) {
				out.push_back(lt->toInt());
			}
		};
		for (uint64_t e : expected_watches) {
			if (!actual_watches.count(e)) {
				if (n_missing == 0) {
					first_missing_lit.copyRaw((uint32_t)(e >> 32));
					first_missing_ofs = (ClauseOfs)(e & 0xFFFFFFFF);
					dump_clause(first_missing_ofs, first_missing_clause_lits);
				}
				n_missing++;
			}
		}
		for (uint64_t a : actual_watches) {
			if (!expected_watches.count(a)) {
				if (n_stale == 0) {
					first_stale_lit.copyRaw((uint32_t)(a >> 32));
					first_stale_ofs = (ClauseOfs)(a & 0xFFFFFFFF);
					dump_clause(first_stale_ofs, first_stale_clause_lits);
				}
				n_stale++;
			}
		}
		std::cerr << "\n*** STATE_INTEGRITY (" << label << "): watch-list mismatch ***\n"
		          << "  expected watches (lit, ofs) total: " << expected_watches.size() << "\n"
		          << "  actual   watches (lit, ofs) total: " << actual_watches.size() << "\n"
		          << "  missing (expected, not actual): " << n_missing << "\n"
		          << "  stale   (actual, not expected): " << n_stale << "\n";
		if (n_missing > 0) {
			std::cerr << "  first missing: lit=" << first_missing_lit.toInt()
			          << " clause ofs=" << first_missing_ofs
			          << " clause lits:";
			for (int l : first_missing_clause_lits) std::cerr << " " << l;
			std::cerr << "\n";
		}
		if (n_stale > 0) {
			std::cerr << "  first stale: lit=" << first_stale_lit.toInt()
			          << " clause ofs=" << first_stale_ofs
			          << " clause lits:";
			for (int l : first_stale_clause_lits) std::cerr << " " << l;
			std::cerr << "\n";
		}
		std::cerr.flush();
		std::abort();
	}

	// Check 3: binary-links symmetry.
	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		const auto &bl = literal(l).binary_links_;
		for (auto bt = bl.begin(); *bt != SENTINEL_LIT; ++bt) {
			// Does *bt's list contain l?
			const auto &back = literal(*bt).binary_links_;
			bool found = false;
			for (auto bt2 = back.begin(); *bt2 != SENTINEL_LIT; ++bt2) {
				if (*bt2 == l) { found = true; break; }
			}
			if (!found) {
				fire("binary_links_ asymmetry: lit " + std::to_string(l.toInt())
				     + " links to " + std::to_string(bt->toInt())
				     + " but reverse link missing");
			}
		}
	}

	// Check 4: occurrence-list consistency (original clauses only —
	// occurrence_lists_ is populated from long original clauses in
	// compactClauses; learned clauses are not tracked there).
	// For each clause C in literal_pool_ below original_lit_pool_size_,
	// every literal in C should have ofs in its occurrence_lists_[lit].
	for (auto it = literal_pool_.begin(); it != literal_pool_.end(); it++) {
		if (*it != SENTINEL_LIT) continue;
		if (it + 1 == literal_pool_.end()) break;
		it += ClauseHeader::overheadInLits();
		ClauseOfs ofs = (ClauseOfs)(it + 1 - literal_pool_.begin());
		if (ofs >= (ClauseOfs)original_lit_pool_size_) break;
		for (auto lt = literal_pool_.begin() + ofs; *lt != SENTINEL_LIT; lt++) {
			const auto &ol = occurrence_lists_[*lt];
			bool found = false;
			for (ClauseOfs o : ol) if (o == ofs) { found = true; break; }
			if (!found) {
				fire("occurrence_lists_ missing: lit " + std::to_string(lt->toInt())
				     + " appears in clause ofs=" + std::to_string(ofs)
				     + " but that clause isn't in occurrence_lists_[lit]");
			}
		}
		auto e = literal_pool_.begin() + ofs;
		while (*e != SENTINEL_LIT) e++;
		it = e - 1;
	}
	// Reverse direction: every ofs in occurrence_lists_[lit] should
	// contain lit.
	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		for (ClauseOfs ofs : occurrence_lists_[l]) {
			if (ofs >= (ClauseOfs)original_lit_pool_size_) continue;
			bool found = false;
			for (auto lt = literal_pool_.begin() + ofs;
			     *lt != SENTINEL_LIT; lt++) {
				if (*lt == l) { found = true; break; }
			}
			if (!found) {
				fire("occurrence_lists_ stale: lit " + std::to_string(l.toInt())
				     + " claims clause ofs=" + std::to_string(ofs)
				     + " contains it, but that clause doesn't have lit");
			}
		}
	}
}

// Emit the current post-preprocess in-memory formula as DIMACS CNF.
// Iteration order for binaries reflects the current binary_links_
// layout (so a post-permBinaryLinks dump captures the permuted order
// in the file). Long clauses emit in literal_pool_ layout order; the
// first two stored literals come out first, preserving the watched
// positions across re-parse.
bool Solver::dumpPreprocessedCnf(const std::string &path) {
	std::ofstream out(path);
	if (!out) {
		std::cerr << "failed to open dump path: " << path << std::endl;
		return false;
	}
	// Count surviving variables and clauses.
	unsigned n_vars = num_variables();
	unsigned n_cls = 0;
	for (auto it = literal_pool_.begin(); it != literal_pool_.end(); it++) {
		if (*it != SENTINEL_LIT) continue;
		if (it + 1 == literal_pool_.end()) break;
		it += ClauseHeader::overheadInLits();
		ClauseOfs ofs = (ClauseOfs)(it + 1 - literal_pool_.begin());
		if (ofs >= (ClauseOfs)original_lit_pool_size_) break;
		unsigned len = 0;
		bool sat = false;
		for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; lt++) {
			if (literal_values_[*lt] == T_TRI) { sat = true; break; }
			if (literal_values_[*lt] == X_TRI) len++;
		}
		if (!sat && len >= 1) n_cls++;
		auto end_it = beginOf(ofs);
		while (*end_it != SENTINEL_LIT) end_it++;
		it = end_it - 1;
	}
	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		if (literal_values_[l] == T_TRI) continue;
		const auto &bl = literal(l).binary_links_;
		for (auto bt = bl.begin(); *bt != SENTINEL_LIT; ++bt) {
			if (!(l.raw() < bt->raw())) continue;
			if (literal_values_[*bt] == T_TRI) continue;
			if (literal_values_[l] == F_TRI
			    && literal_values_[*bt] == F_TRI) continue;
			n_cls++;
		}
	}
	for (auto lit : literal_stack_) {
		if (var(lit).decision_level == 0) n_cls++;
	}

	out << "c post-preprocessing dump from sharpsat-separator\n";
	out << "p cnf " << n_vars << " " << n_cls << "\n";
	for (auto it = literal_pool_.begin(); it != literal_pool_.end(); it++) {
		if (*it != SENTINEL_LIT) continue;
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
	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		if (literal_values_[l] == T_TRI) continue;
		const auto &bl = literal(l).binary_links_;
		for (auto bt = bl.begin(); *bt != SENTINEL_LIT; ++bt) {
			if (!(l.raw() < bt->raw())) continue;
			if (literal_values_[*bt] == T_TRI) continue;
			bool l_false  = (literal_values_[l]  == F_TRI);
			bool bt_false = (literal_values_[*bt] == F_TRI);
			if (l_false && bt_false) continue;
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
	for (auto lit : literal_stack_) {
		if (var(lit).decision_level == 0) {
			int v = (int)lit.var();
			if (!lit.sign()) v = -v;
			out << v << " 0\n";
		}
	}
	out.close();
	std::cerr << "dumped preprocessed formula to " << path
	          << " (" << n_vars << " vars, " << n_cls << " clauses)\n";
	return true;
}


void Solver::dumpBinariesAfterPreprocess(const std::string &path) {
	std::ofstream out(path);
	if (!out) return;
	unsigned count = 0;
	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		const auto &bl = literal(l).binary_links_;
		for (auto bt = bl.begin(); *bt != SENTINEL_LIT; bt++) {
			if (!(l.raw() < bt->raw())) continue;  // dedup
			out << l.toInt() << " " << bt->toInt() << "\n";
			count++;
		}
	}
	std::cerr << "dumped " << count << " binaries to " << path << "\n";
}

// Helper: walk literal_pool_ once and emit learned clauses whose
// literals all fall within the given active-variable set (so the
// dump is a self-contained CNF on that variable set). Skip clauses
// with any literal on an outside variable.
static void emitLearnedClausesInSubComp(
    const std::vector<LiteralID> &literal_pool,
    unsigned original_lit_pool_size,
    const LiteralIndexedVector<TriValue> &literal_values,
    const std::unordered_map<ClauseOfs, unsigned> &removed_clauses,
    const std::unordered_map<unsigned, int> &var_map,
    std::vector<std::vector<int>> &out_clauses) {
	auto it = literal_pool.begin();
	while (it != literal_pool.end()) {
		if (*it != SENTINEL_LIT) { it++; continue; }
		if (it + 1 == literal_pool.end()) break;
		it += ClauseHeader::overheadInLits();
		ClauseOfs ofs = (ClauseOfs)(it + 1 - literal_pool.begin());
		if (ofs < (ClauseOfs)original_lit_pool_size) {
			// original: advance past and skip
			auto e = literal_pool.begin() + ofs;
			while (*e != SENTINEL_LIT) e++;
			it = e;
			continue;
		}
		// learned clause
		if (!removed_clauses.count(ofs)) {
			bool all_in = true;
			bool satisfied = false;
			std::vector<int> clause;
			for (auto lt = literal_pool.begin() + ofs;
			     *lt != SENTINEL_LIT; lt++) {
				if (literal_values[*lt] == T_TRI) { satisfied = true; break; }
				if (literal_values[*lt] == F_TRI) continue;  // drop
				auto vi = var_map.find(lt->var());
				if (vi == var_map.end()) { all_in = false; break; }
				int v = vi->second;
				clause.push_back(lt->sign() ? v : -v);
			}
			if (!satisfied && all_in && clause.size() >= 1)
				out_clauses.push_back(clause);
		}
		auto e = literal_pool.begin() + ofs;
		while (*e != SENTINEL_LIT) e++;
		it = e;
	}
}

// Emit a single sub-component as a standalone DIMACS CNF under the
// current partial assignment. Used by -bruteForceCacheDumpDir to
// capture offending sub-components when a brute-force cache check
// fires a mismatch.
bool Solver::dumpSubComponentCnf(Component &comp, const std::string &path,
                                  unsigned *out_nvars) {
	// Collect active vars in component
	std::vector<unsigned> vars;
	for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++) {
		if (isActive(LiteralID(*it, true))) vars.push_back(*it);
	}
	std::sort(vars.begin(), vars.end());
	if (out_nvars) *out_nvars = (unsigned)vars.size();
	if (vars.empty()) return false;

	// Compact var remap 1..N
	std::unordered_map<unsigned, int> var_map;
	for (size_t i = 0; i < vars.size(); i++) var_map[vars[i]] = (int)(i + 1);
	unsigned nv = (unsigned)vars.size();

	std::vector<std::vector<int>> out_clauses;

	// Long clauses from comp via clause_id_to_ofs
	const auto &cmap = comp_manager_.getAnalyzer().clauseIdToOfs();
	for (auto it = comp.clsBegin(); *it != clsSENTINEL; it++) {
		ClauseOfs ofs = cmap[*it];
		if (ofs >= (ClauseOfs)original_lit_pool_size_) continue;  // skip learned
		if (removed_clauses_.count(ofs)) continue;
		std::vector<int> clause;
		bool satisfied = false;
		for (auto lt = literal_pool_.begin() + ofs; *lt != SENTINEL_LIT; lt++) {
			if (literal_values_[*lt] == T_TRI) { satisfied = true; break; }
			if (literal_values_[*lt] == X_TRI) {
				auto vi = var_map.find(lt->var());
				if (vi == var_map.end()) continue;
				int v = vi->second;
				clause.push_back(lt->sign() ? v : -v);
			}
		}
		if (!satisfied && clause.size() >= 2) out_clauses.push_back(clause);
	}

	// Binary clauses among active component vars. Each binary emitted once.
	for (unsigned v : vars) {
		for (int s = 0; s <= 1; s++) {
			LiteralID lit(v, s == 1);
			if (literal_values_[lit] != X_TRI) continue;
			unsigned orig_count = literals_[lit].original_binary_link_count_;
			unsigned idx = 0;
			for (auto bt = literals_[lit].binary_links_.begin();
			     *bt != SENTINEL_LIT; bt++, idx++) {
				if (idx >= orig_count) break;  // skip learned binaries
				unsigned other_v = bt->var();
				if (other_v <= v) continue;  // dedup each binary once
				auto vi = var_map.find(other_v);
				if (vi == var_map.end()) continue;
				if (literal_values_[*bt] == T_TRI) continue;  // satisfied
				if (literal_values_[*bt] == F_TRI) continue;  // degenerate unit
				int a = lit.sign() ? var_map[v] : -var_map[v];
				int b = bt->sign() ? vi->second : -vi->second;
				out_clauses.push_back({a, b});
			}
		}
	}

	// Include learned clauses whose literals are all within this
	// sub-component.
	size_t n_original_clauses = out_clauses.size();
	std::vector<std::string> raw_learned_lines;
	{
		auto it2 = literal_pool_.begin();
		while (it2 != literal_pool_.end()) {
			if (*it2 != SENTINEL_LIT) { it2++; continue; }
			if (it2 + 1 == literal_pool_.end()) break;
			it2 += ClauseHeader::overheadInLits();
			ClauseOfs ofs = (ClauseOfs)(it2 + 1 - literal_pool_.begin());
			if (ofs < (ClauseOfs)original_lit_pool_size_) {
				auto e = literal_pool_.begin() + ofs;
				while (*e != SENTINEL_LIT) e++;
				it2 = e;
				continue;
			}
			if (!removed_clauses_.count(ofs)) {
				bool all_in = true;
				bool sat = false;
				std::string raw = "c raw-learned ofs=" + std::to_string(ofs) + ":";
				std::string projected = " projected:";
				for (auto lt = literal_pool_.begin() + ofs;
				     *lt != SENTINEL_LIT; lt++) {
					int v = lt->sign() ? (int)lt->var() : -(int)lt->var();
					const char *tag =
					    (literal_values_[*lt] == T_TRI) ? "(T)" :
					    (literal_values_[*lt] == F_TRI) ? "(F)" : "";
					raw += " ";
					raw += std::to_string(v);
					raw += tag;
					if (literal_values_[*lt] == T_TRI) sat = true;
					else if (literal_values_[*lt] == X_TRI) {
						auto vi = var_map.find(lt->var());
						if (vi == var_map.end()) all_in = false;
						else {
							projected += " ";
							projected += std::to_string(lt->sign() ? vi->second : -vi->second);
						}
					}
				}
				if (!sat && all_in) raw += projected;
				raw_learned_lines.push_back(raw);
			}
			auto e = literal_pool_.begin() + ofs;
			while (*e != SENTINEL_LIT) e++;
			it2 = e;
		}
	}
	emitLearnedClausesInSubComp(literal_pool_, original_lit_pool_size_,
	                            literal_values_, removed_clauses_,
	                            var_map, out_clauses);
	size_t n_learned = out_clauses.size() - n_original_clauses;

	std::ofstream out(path);
	if (!out) return false;
	out << "c sub-component dump from sharpsat-separator\n";
	out << "c  original clauses: " << n_original_clauses
	    << "  learned clauses (confined to this comp): " << n_learned << "\n";
	out << "c var mapping (local -> original):\n";
	for (size_t i = 0; i < vars.size(); i++)
		out << "c   " << (i + 1) << " -> " << vars[i] << "\n";
	out << "c raw learned clauses (original vars, annotated with T/F of assigned lits):\n";
	for (const auto &line : raw_learned_lines) out << line << "\n";
	out << "p cnf " << nv << " " << out_clauses.size() << "\n";
	for (auto &c : out_clauses) {
		for (int l : c) out << l << " ";
		out << "0\n";
	}
	return true;
}


// Brute-force #SAT count over the sub-component's currently-active
// variables. Active = X_TRI under the current trail. Active clauses
// = original (not learned) clauses that are not removed and not
// satisfied by the trail. Original binary clauses included.
//
// Returns -1 (mpz_class) when |active vars| > n_max (too big to brute
// force). Returns 0 if any active clause has all literals F_TRI
// (already-falsified clause means UNSAT under the trail).
//
// Learned clauses are EXCLUDED. Rationale: a sound learned clause is
// a logical consequence of the original formula, so adding it
// doesn't change #SAT — brute force without it gives the same
// answer as with. If the solver uses a learned clause UNSOUNDLY
// (somehow restricting the model space wrongly), the comparison vs
// brute force will catch the resulting count mismatch.
mpz_class Solver::bruteForceCountSubcomp(Component &sub,
                                          unsigned n_max,
                                          unsigned *out_active_n) {
	mpz_class neg_one = -1;

	// Collect active vars in order.
	std::vector<unsigned> avars;
	for (auto vt = sub.varsBegin(); *vt != varsSENTINEL; vt++)
		if (isActive(LiteralID(*vt, true))) avars.push_back(*vt);
	if (out_active_n) *out_active_n = (unsigned)avars.size();
	if (avars.size() > n_max) return neg_one;

	// Map original var ID -> bit position.
	std::unordered_map<unsigned, unsigned> var_to_bit;
	var_to_bit.reserve(avars.size() * 2);
	for (unsigned i = 0; i < avars.size(); i++) var_to_bit[avars[i]] = i;

	// Collect active long clauses (originals, not removed, not satisfied),
	// each as a list of (bit, target) pairs over its currently-active lits.
	struct ClauseLits { std::vector<std::pair<unsigned,bool>> lits; };
	std::vector<ClauseLits> clauses;
	clauses.reserve(64);
	const auto &cmap = comp_manager_.getAnalyzer().clauseIdToOfs();
	for (auto it = sub.clsBegin(); *it != clsSENTINEL; it++) {
		ClauseOfs ofs = cmap[*it];
		if (ofs >= (ClauseOfs)original_lit_pool_size_) continue;
		if (removed_clauses_.count(ofs)) continue;
		bool satisfied = false;
		ClauseLits cl;
		for (auto lt = literal_pool_.begin() + ofs; *lt != SENTINEL_LIT; lt++) {
			if (literal_values_[*lt] == T_TRI) { satisfied = true; break; }
			if (literal_values_[*lt] != X_TRI) continue;     // F_TRI -> drop lit
			auto vi = var_to_bit.find(lt->var());
			if (vi == var_to_bit.end()) continue;
			cl.lits.push_back({vi->second, lt->sign()});
		}
		if (satisfied) continue;
		if (cl.lits.empty()) return 0;     // unsatisfiable already
		clauses.push_back(std::move(cl));
	}

	// Original binary clauses among active vars.
	for (unsigned v : avars) {
		for (int s = 0; s <= 1; s++) {
			LiteralID lit(v, s == 1);
			if (literal_values_[lit] != X_TRI) continue;
			unsigned orig_count = literals_[lit].original_binary_link_count_;
			unsigned idx = 0;
			for (auto bt = literals_[lit].binary_links_.begin();
			     *bt != SENTINEL_LIT; bt++, idx++) {
				if (idx >= orig_count) break;
				unsigned other_v = bt->var();
				if (other_v <= v) continue;
				if (literal_values_[*bt] == T_TRI) continue;
				if (literal_values_[*bt] == F_TRI) continue;
				auto vi = var_to_bit.find(other_v);
				if (vi == var_to_bit.end()) continue;
				ClauseLits cl;
				cl.lits.push_back({var_to_bit[v], lit.sign()});
				cl.lits.push_back({vi->second, bt->sign()});
				clauses.push_back(std::move(cl));
			}
		}
	}

	// Enumerate.
	mpz_class count = 0;
	const unsigned long total = 1UL << avars.size();
	for (unsigned long m = 0; m < total; m++) {
		bool sat = true;
		for (const auto &cl : clauses) {
			bool csat = false;
			for (const auto &lp : cl.lits) {
				bool val = (m >> lp.first) & 1UL;
				if (val == lp.second) { csat = true; break; }
			}
			if (!csat) { sat = false; break; }
		}
		if (sat) ++count;
	}
	return count;
}

// Stricter version: enumerate as above but also require each model to
// satisfy every IN-SCOPE LEARNED clause confined to this sub-component.
// If a learned clause is sound for the current scope, this should
// return the same count as bruteForceCountSubcomp (since sound learned
// clauses are entailed by originals and don't change #SAT). If the
// counts differ, an unsound learned clause is restricting the model
// space.
mpz_class Solver::bruteForceCountSubcompWithLearned(Component &sub,
                                                     unsigned n_max,
                                                     unsigned *out_active_n,
                                                     unsigned *out_n_learned_checked) {
	mpz_class neg_one = -1;

	std::vector<unsigned> avars;
	for (auto vt = sub.varsBegin(); *vt != varsSENTINEL; vt++)
		if (isActive(LiteralID(*vt, true))) avars.push_back(*vt);
	if (out_active_n) *out_active_n = (unsigned)avars.size();
	if (avars.size() > n_max) return neg_one;

	std::unordered_map<unsigned, unsigned> var_to_bit;
	var_to_bit.reserve(avars.size() * 2);
	for (unsigned i = 0; i < avars.size(); i++) var_to_bit[avars[i]] = i;
	std::set<unsigned> avar_set(avars.begin(), avars.end());

	struct ClauseLits { std::vector<std::pair<unsigned,bool>> lits; };
	std::vector<ClauseLits> clauses;
	clauses.reserve(64);

	// Active originals + binaries (same as the simpler version).
	const auto &cmap = comp_manager_.getAnalyzer().clauseIdToOfs();
	for (auto it = sub.clsBegin(); *it != clsSENTINEL; it++) {
		ClauseOfs ofs = cmap[*it];
		if (ofs >= (ClauseOfs)original_lit_pool_size_) continue;
		if (removed_clauses_.count(ofs)) continue;
		bool satisfied = false;
		ClauseLits cl;
		for (auto lt = literal_pool_.begin() + ofs; *lt != SENTINEL_LIT; lt++) {
			if (literal_values_[*lt] == T_TRI) { satisfied = true; break; }
			if (literal_values_[*lt] != X_TRI) continue;
			auto vi = var_to_bit.find(lt->var());
			if (vi == var_to_bit.end()) continue;
			cl.lits.push_back({vi->second, lt->sign()});
		}
		if (satisfied) continue;
		if (cl.lits.empty()) return 0;
		clauses.push_back(std::move(cl));
	}
	for (unsigned v : avars) {
		for (int s = 0; s <= 1; s++) {
			LiteralID lit(v, s == 1);
			if (literal_values_[lit] != X_TRI) continue;
			unsigned orig_count = literals_[lit].original_binary_link_count_;
			unsigned idx = 0;
			for (auto bt = literals_[lit].binary_links_.begin();
			     *bt != SENTINEL_LIT; bt++, idx++) {
				if (idx >= orig_count) break;
				unsigned other_v = bt->var();
				if (other_v <= v) continue;
				if (literal_values_[*bt] == T_TRI) continue;
				if (literal_values_[*bt] == F_TRI) continue;
				auto vi = var_to_bit.find(other_v);
				if (vi == var_to_bit.end()) continue;
				ClauseLits cl;
				cl.lits.push_back({var_to_bit[v], lit.sign()});
				cl.lits.push_back({vi->second, bt->sign()});
				clauses.push_back(std::move(cl));
			}
		}
	}

	// IN-SCOPE LEARNED clauses confined to this sub-component.
	// Walk the literal pool past original_lit_pool_size_; for each
	// learned clause: check (a) currently in-scope, (b) all active
	// vars belong to this sub-component (fully confined), (c) not
	// satisfied. If yes, project to active lits and add.
	unsigned n_learned_added = 0;
	for (auto it = literal_pool_.begin() + original_lit_pool_size_;
	     it != literal_pool_.end(); ) {
		if (*it == SENTINEL_LIT) {
			if (it + 1 == literal_pool_.end()) break;
			it += ClauseHeader::overheadInLits() + 1;
			continue;
		}
		ClauseOfs ofs = (ClauseOfs)(it - literal_pool_.begin());
		// Walk the clause to gather lits.
		auto end_it = it;
		while (*end_it != SENTINEL_LIT) end_it++;
		// Check in-scope.
		if (!learnedClauseInScope(ofs)) { it = end_it; continue; }
		// Check confinement and build active lit list.
		bool satisfied = false;
		bool confined = true;
		ClauseLits cl;
		for (auto lt = it; lt != end_it; lt++) {
			if (literal_values_[*lt] == T_TRI) { satisfied = true; break; }
			if (literal_values_[*lt] != X_TRI) continue;     // F_TRI -> drop lit
			if (!avar_set.count(lt->var())) { confined = false; break; }
			auto vi = var_to_bit.find(lt->var());
			if (vi == var_to_bit.end()) { confined = false; break; }
			cl.lits.push_back({vi->second, lt->sign()});
		}
		it = end_it;
		if (satisfied || !confined) continue;
		if (cl.lits.empty()) return 0;
		clauses.push_back(std::move(cl));
		n_learned_added++;
	}
	if (out_n_learned_checked) *out_n_learned_checked = n_learned_added;

	mpz_class count = 0;
	const unsigned long total = 1UL << avars.size();
	for (unsigned long m = 0; m < total; m++) {
		bool sat = true;
		for (const auto &cl : clauses) {
			bool csat = false;
			for (const auto &lp : cl.lits) {
				bool val = (m >> lp.first) & 1UL;
				if (val == lp.second) { csat = true; break; }
			}
			if (!csat) { sat = false; break; }
		}
		if (sat) ++count;
	}
	return count;
}

void Solver::verifyPostPreprocessCleanSlate(const char *label) {
	auto fire = [&](const std::string &msg) {
		std::cerr << "\n*** CLEAN_SLATE (" << label << "): "
		          << msg << " ***\n";
		std::cerr.flush();
		std::abort();
	};

	if (!conflict_clauses_.empty())
		fire("conflict_clauses_ not empty (size="
		     + std::to_string(conflict_clauses_.size()) + ")");
	if (!unit_clauses_.empty())
		fire("unit_clauses_ not empty (size="
		     + std::to_string(unit_clauses_.size()) + ")");
	if (!removed_clauses_.empty())
		fire("removed_clauses_ not empty (size="
		     + std::to_string(removed_clauses_.size()) + ")");
	if (!learned_clause_scope_.empty())
		fire("learned_clause_scope_ not empty (size="
		     + std::to_string(learned_clause_scope_.size()) + ")");

	if (!uip_clauses_.empty())
		fire("uip_clauses_ not empty (size="
		     + std::to_string(uip_clauses_.size()) + ")");
	if (!violated_clause.empty())
		fire("violated_clause not empty (size="
		     + std::to_string(violated_clause.size()) + ")");
	if (assertion_level_ != 0)
		fire("assertion_level_ != 0 (= "
		     + std::to_string(assertion_level_) + ")");
	if (last_ccl_deletion_time_ != 0)
		fire("last_ccl_deletion_time_ != 0");
	if (last_ccl_cleanup_time_ != 0)
		fire("last_ccl_cleanup_time_ != 0");

	// literal_stack_ must be empty (initStack cleared it).
	if (!literal_stack_.empty())
		fire("literal_stack_ not empty (size="
		     + std::to_string(literal_stack_.size()) + ")");

	// Per-variable: every Variable at its default-constructed state.
	// v=0 is unused (1-based indexing).
	for (unsigned v = 1; v < variables_.size(); v++) {
		if (variables_[v].ante.isAnt())
			fire("var " + std::to_string(v) + " has non-default ante");
		if (variables_[v].decision_level != INVALID_DL)
			fire("var " + std::to_string(v)
			     + " has decision_level="
			     + std::to_string(variables_[v].decision_level)
			     + " (expected INVALID_DL)");
		if (variables_[v].chain_depth != 0)
			fire("var " + std::to_string(v)
			     + " has chain_depth="
			     + std::to_string((int)variables_[v].chain_depth));
	}

	// Invariant R5: at search start, every literal's binary_links_
	// must contain ONLY original binaries — no learned binaries
	// should have leaked in. Size = original_binary_link_count_ + 1
	// (the +1 is for the trailing SENTINEL_LIT terminator).
	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		const auto &bl = literal(l).binary_links_;
		unsigned expected = literal(l).original_binary_link_count_ + 1;
		if (bl.size() != expected) {
			fire("INV_R5: lit " + std::to_string(l.toInt())
			     + " binary_links_.size()=" + std::to_string(bl.size())
			     + " expected=" + std::to_string(expected)
			     + " (original_binary_link_count_="
			     + std::to_string(literal(l).original_binary_link_count_) + ")");
		}
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
	comp_manager_.initialize(literals_, literal_pool_, original_lit_pool_size_);
	comp_manager_.setRemovedClauses(&removed_clauses_);
}

// Test-support: inject a "learned" long clause. Writes directly into
// literal_pool_ past original_lit_pool_size_ using the same layout
// addClause would produce, and registers watches on the first two
// literals. Does not assign a ClauseID (learned clauses never get one).
void Solver::_injectLearnedLongClauseForTest(const std::vector<int> &dimacs_lits) {
	if (dimacs_lits.size() < 3) {
		std::cerr << "_injectLearnedLongClauseForTest: size must be >= 3\n";
		std::abort();
	}
	auto int_to_lit = [](int l) {
		return (l > 0) ? LiteralID((unsigned)l, true)
		               : LiteralID((unsigned)(-l), false);
	};
	for (unsigned i = 0; i < ClauseHeader::overheadInLits(); i++)
		literal_pool_.push_back(LiteralID(0, false));
	ClauseOfs ofs = (ClauseOfs)literal_pool_.size();
	if (ofs < (ClauseOfs)original_lit_pool_size_) {
		std::cerr << "_injectLearnedLongClauseForTest: new clause at ofs="
		          << ofs << " but boundary=" << original_lit_pool_size_
		          << " — pool invariant violated\n";
		std::abort();
	}
	for (int l : dimacs_lits)
		literal_pool_.push_back(int_to_lit(l));
	literal_pool_.push_back(SENTINEL_LIT);
	literal(int_to_lit(dimacs_lits[0])).addWatchLinkTo(ofs);
	literal(int_to_lit(dimacs_lits[1])).addWatchLinkTo(ofs);
}

// Test-support: inject a "learned" binary. Appends past
// original_binary_link_count_ in both endpoints' binary_links_.
void Solver::_injectLearnedBinaryForTest(int dimacs_a, int dimacs_b) {
	auto int_to_lit = [](int l) {
		return (l > 0) ? LiteralID((unsigned)l, true)
		               : LiteralID((unsigned)(-l), false);
	};
	LiteralID a = int_to_lit(dimacs_a);
	LiteralID b = int_to_lit(dimacs_b);
	// addBinLinkTo appends past the sentinel unconditionally (it
	// doesn't distinguish original vs learned). Since
	// original_binary_link_count_ was fixed at preprocessing time,
	// anything added now automatically sits in the "learned" region.
	literal(a).addBinLinkTo(b);
	literal(b).addBinLinkTo(a);
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


