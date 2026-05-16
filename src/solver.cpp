/*
 * solver.cpp
 *
 *  Created on: Aug 23, 2012
 *      Author: marc
 */
#include "solver.h"
#include "preprocessor.h"
#include "probe_preprocessor.h"
#include "canonical_key.h"
#include <deque>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <unordered_set>
#include <chrono>
#include <cstdint>
#include <functional>

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
	// Trivially-UNSAT short-circuit: createfromFile flags this when the
	// input CNF contains an empty clause. Check first, before the -noPP
	// early return, so the flag is honored under any solver config.
	if (parsed_trivially_unsat_) return false;

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

	// Run the standalone #SAT-sound preprocessor. It operates on its
	// own internal representation (no Solver/Instance state is shared)
	// and returns a simplified CNF + a list of root-forced units. We
	// then rebuild the solver's in-memory formula from the output,
	// discarding the pre-preprocess formula entirely. After rebuild,
	// the second unit-BCP + HardWireAndCompact absorbs the
	// preprocessor-produced units in the same code path that handles
	// original units — the solver starts from a clean-slate state
	// regardless of how the simplified CNF was obtained.
	if (succeeded
	    && (config_.perform_preprocess_subsumption
	        || config_.perform_preprocess_pure_duplicate
	        || config_.perform_preprocess_ssr)) {
		auto extracted = extractFormulaAsDimacs();
		PreprocessorConfig pcfg;
		pcfg.enable_subsumption    = config_.perform_preprocess_subsumption;
		pcfg.enable_pure_duplicate = config_.perform_preprocess_pure_duplicate;
		pcfg.enable_ssr            = config_.perform_preprocess_ssr;
		pcfg.time_budget_ms        = config_.preprocess_time_budget_ms;
		pcfg.verbose               = config_.preprocess_verbose;
		PreprocessorResult pre_out = preprocess(
		    num_variables(), extracted, pcfg);

		if (config_.preprocess_verbose || !config_.quiet) {
			unsigned pre_n = (unsigned)extracted.size();
			unsigned post_n = (unsigned)pre_out.clauses.size();
			std::cerr << "preprocess: "
			          << pre_n << " -> " << post_n
			          << " clauses (subsumed=" << pre_out.num_subsumed
			          << " pure_dup=" << pre_out.num_pure_dup
			          << " ssr=" << pre_out.num_ssr
			          << " forced=" << pre_out.num_bcp_units
			          << " passes=" << pre_out.passes
			          << " elapsed_ms=" << pre_out.elapsed_ms
			          << ")\n";
		}

		if (pre_out.unsat) return false;

		rebuildFromPreprocessedCNF(pre_out);

		// Re-run unit propagation + hardwire on the rebuilt formula to
		// absorb the preprocessor-produced units. This mirrors the
		// original unit-clause path so the solver's state after this
		// point is exactly what it would be if we had parsed a
		// freshly-simplified CNF file.
		if (!unit_clauses_.empty()) {
			for (auto lit : unit_clauses_)
				setLiteralIfFree(lit);
			if (!BCP(0)) return false;
			HardWireAndCompact();
		}
	}

	// Local-search probe-based preprocessing (opt-in). Runs after the
	// standard preprocessor; uses A internally for mop-up. Eliminated
	// variables are pinned via forced units so the downstream solver's
	// component-analysis count is correct without any solver-side
	// variable-set bookkeeping.
	if (succeeded && config_.perform_local_search_preprocess) {
		auto extracted = extractFormulaAsDimacs();
		LocalSearchPreprocessConfig lcfg;
		lcfg.max_probes = 1000;
		lcfg.max_size   = 4;
		lcfg.max_total  = 5000;
		lcfg.enable_r4  = !config_.lsp_no_r4;
		lcfg.budget_ms  = config_.preprocess_time_budget_ms;
		lcfg.verbose    = config_.preprocess_verbose;
		lcfg.preprocessor_cfg.enable_subsumption    = config_.perform_preprocess_subsumption;
		lcfg.preprocessor_cfg.enable_pure_duplicate = config_.perform_preprocess_pure_duplicate;
		lcfg.preprocessor_cfg.enable_ssr            = config_.perform_preprocess_ssr;
		lcfg.preprocessor_cfg.time_budget_ms        = config_.preprocess_time_budget_ms;
		lcfg.preprocessor_cfg.verbose               = false;

		LocalSearchPreprocessResult lsp_out =
		    runLocalSearchPreprocess(num_variables(), extracted, lcfg);

		if (config_.preprocess_verbose || !config_.quiet) {
			std::cerr << "lsp: passes=" << lsp_out.passes
			          << " probes=" << lsp_out.num_probes_run
			          << " units=" << lsp_out.num_units_added
			          << " subsume=" << lsp_out.num_subsumptions
			          << " ssr=" << lsp_out.num_ssr_strengthenings
			          << " elim=" << lsp_out.num_eliminations
			          << " elapsed_ms=" << lsp_out.elapsed_ms
			          << " unsat=" << lsp_out.unsat << "\n";
		}

		if (lsp_out.unsat) return false;

		// Convert back to a PreprocessorResult shape and rebuild.
		PreprocessorResult final_out;
		final_out.clauses      = lsp_out.clauses;
		final_out.forced_units = lsp_out.forced_units;
		final_out.num_vars     = num_variables();
		final_out.unsat        = false;

		rebuildFromPreprocessedCNF(final_out);
		if (!unit_clauses_.empty()) {
			for (auto lit : unit_clauses_)
				setLiteralIfFree(lit);
			if (!BCP(0)) return false;
			HardWireAndCompact();
		}
	}
	return succeeded;
}

// ---------------------------------------------------------------
// Extract the current formula (after HardWireAndCompact) as a list
// of DIMACS int vectors. Includes:
//   - all long clauses in literal_pool_ (size >= 3)
//   - all binary clauses (once each, deduped by endpoint ordering)
// Does NOT include unit clauses — they have already been propagated
// and compactVariables cleared unit_clauses_.
// ---------------------------------------------------------------
std::vector<std::vector<int>> Solver::extractFormulaAsDimacs() {
	std::vector<std::vector<int>> out;

	// Binaries. literal.binary_links_ stores each binary twice (once
	// per endpoint); emit only when we're at the lower-raw endpoint.
	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		const auto &blinks = literal(l).binary_links_;
		for (auto bt = blinks.begin(); *bt != SENTINEL_LIT; ++bt) {
			if (l.raw() < bt->raw())
				out.push_back({l.toInt(), bt->toInt()});
		}
	}

	// Long clauses (size >= 3).
	for (auto it = literal_pool_.begin(); it != literal_pool_.end(); ) {
		if (*it == SENTINEL_LIT) {
			if (it + 1 == literal_pool_.end()) break;
			it += ClauseHeader::overheadInLits() + 1;
			continue;
		}
		std::vector<int> lits;
		auto p = it;
		while (*p != SENTINEL_LIT) { lits.push_back(p->toInt()); ++p; }
		if (!lits.empty()) out.push_back(std::move(lits));
		it = p;
	}

	return out;
}

// ---------------------------------------------------------------
// Rebuild the solver's in-memory formula from the preprocessor's
// output, discarding everything about the pre-preprocess formula.
// After this call, the state is bit-identical to what
// createfromFile would produce on a CNF containing
//   pre_out.forced_units + pre_out.clauses
// over the same number of variables.
// ---------------------------------------------------------------
void Solver::rebuildFromPreprocessedCNF(const PreprocessorResult &pre_out) {
	// 1. Reset per-variable state (antecedent, DL, value).
	for (unsigned v = 0; v < variables_.size(); v++) {
		variables_[v].ante = Antecedent(NOT_A_CLAUSE);
		variables_[v].decision_level = INVALID_DL;
	}
	literal_values_.clear();
	literal_values_.resize(literals_.end_lit().raw(), X_TRI);
	literal_stack_.clear();

	// 2. Reset per-literal structures: watch lists, binary links,
	// occurrence lists.
	for (auto l = LiteralID(0, false); l != literals_.end_lit(); l.inc()) {
		literal(l).resetWatchList();
		literal(l).binary_links_ = std::vector<LiteralID>(1, SENTINEL_LIT);
		literal(l).original_binary_link_count_ = 0;
	}
	occurrence_lists_.clear();
	occurrence_lists_.resize(variables_.size());
	unit_clauses_.clear();

	// 3. Reset the literal pool.
	literal_pool_.clear();
	literal_pool_.push_back(SENTINEL_LIT);

	// 4. Populate forced units into unit_clauses_ (to be absorbed
	// by the caller's follow-up BCP + HardWireAndCompact).
	auto int_to_lit = [](int l) -> LiteralID {
		return (l > 0) ? LiteralID((unsigned)l, true)
		               : LiteralID((unsigned)(-l), false);
	};
	for (int u : pre_out.forced_units)
		unit_clauses_.push_back(int_to_lit(u));

	// 5. Populate clauses. size==2 → binary_links_; size>=3 → pool.
	unsigned n_long = 0, n_bin = 0;
	for (const auto &c : pre_out.clauses) {
		if (c.size() < 2) {
			// Preprocessor guarantee violated — should never happen.
			std::cerr << "\n*** REBUILD_FROM_PREPROCESSED_BAD_SIZE ***\n"
			          << "  clause size=" << c.size() << "\n";
			std::cerr.flush();
			std::abort();
		}
		if (c.size() == 2) {
			LiteralID a = int_to_lit(c[0]);
			LiteralID b = int_to_lit(c[1]);
			if (!literal(a).hasBinaryLinkTo(b)) {
				literal(a).addBinLinkTo(b);
				literal(b).addBinLinkTo(a);
				n_bin++;
			}
		} else {
			for (unsigned i = 0; i < ClauseHeader::overheadInLits(); i++)
				literal_pool_.push_back(LiteralID(0, false));
			ClauseOfs ofs = (ClauseOfs)literal_pool_.size();
			for (int l : c) {
				LiteralID lit = int_to_lit(l);
				literal_pool_.push_back(lit);
				occurrence_lists_[lit].push_back(ofs);
			}
			literal_pool_.push_back(SENTINEL_LIT);
			literal(int_to_lit(c[0])).addWatchLinkTo(ofs);
			literal(int_to_lit(c[1])).addWatchLinkTo(ofs);
			n_long++;
		}
	}

	for (auto l = LiteralID(0, false); l != literals_.end_lit(); l.inc())
		literal(l).recordOriginalBinaryLinks();

	original_lit_pool_size_ = literal_pool_.size();

	// 6. Statistics. Mirrors HardWireAndCompact's bookkeeping — the
	// caller's subsequent HardWireAndCompact call will re-stamp these.
	statistics_.num_long_clauses_            = n_long;
	statistics_.num_binary_clauses_          = n_bin;
	statistics_.num_original_binary_clauses_ = n_bin;
	statistics_.num_unit_clauses_            = unit_clauses_.size();
	statistics_.num_original_unit_clauses_   = unit_clauses_.size();
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

// ---------------------------------------------------------------
// Structural analyzer: picks Stage-0 α from formula density.
// Runs post-preprocess, before comp_manager_.initialize() and
// before search. When auto_stage0_length_decay is off or
// perform_adaptive_branching is off, this is a no-op.
// ---------------------------------------------------------------
void Solver::analyzeAndSetHyperparameters() {
	if (!config_.perform_adaptive_branching) return;
	if (!config_.auto_stage0_length_decay)  return;

	// Count active original clauses + sum their lengths.
	// Binaries are counted via binary_links_ (original_binary_link_count_).
	// Long clauses are walked from literal_pool_; only those with
	// offset < original_lit_pool_size_ count (learned clauses don't
	// exist at this point, but the filter is cheap insurance).
	unsigned long n_clauses = 0;
	unsigned long sum_len = 0;

	// Binaries (dedup: each stored twice, once per endpoint).
	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		unsigned orig = literal(l).original_binary_link_count_;
		unsigned idx = 0;
		for (auto bt = literal(l).binary_links_.begin();
		     *bt != SENTINEL_LIT; ++bt, ++idx) {
			if (idx >= orig) break;
			if (l.raw() < bt->raw()) {  // dedup
				n_clauses++;
				sum_len += 2;
			}
		}
	}

	// Long clauses.
	for (auto it = literal_pool_.begin(); it != literal_pool_.end(); ) {
		if (*it == SENTINEL_LIT) {
			if (it + 1 == literal_pool_.end()) break;
			it += ClauseHeader::overheadInLits() + 1;
			continue;
		}
		ClauseOfs ofs = (ClauseOfs)(it - literal_pool_.begin());
		if (ofs >= (ClauseOfs)original_lit_pool_size_) break;
		unsigned len = 0;
		auto p = it;
		while (*p != SENTINEL_LIT) { len++; ++p; }
		if (len >= 2) {
			n_clauses++;
			sum_len += len;
		}
		it = p;
	}

	if (n_clauses == 0) return;  // degenerate; leave α as-is

	double mean_len = (double)sum_len / (double)n_clauses;

	double chosen_alpha;
	const char *bucket;
	if (mean_len < 3.5) {
		chosen_alpha = 0.5;
		bucket = "dense (mean_len<3.5)";
	} else if (mean_len < 6.0) {
		chosen_alpha = 1.0;
		bucket = "medium (3.5≤mean_len<6.0)";
	} else {
		chosen_alpha = 2.0;
		bucket = "sparse (mean_len≥6.0)";
	}

	config_.stage0_length_decay = chosen_alpha;

	if (!config_.quiet) {
		std::cerr << "analyzer: " << n_clauses << " active clauses, "
		          << "mean_len=" << mean_len << " → " << bucket
		          << " → stage0_length_decay=" << chosen_alpha << "\n";
	}
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

	// Full clean-slate reset of all search-scoped state. After this
	// point, every field the search phase will accumulate into is at
	// its default-constructed value. verifyPostPreprocessCleanSlate
	// (called in solve()) enforces the invariant.
	resetPostPreprocessScratch();
}

void Solver::solve(const string &file_name) {
	stopwatch_.start();
	statistics_.input_file_ = file_name;

	createfromFile(file_name);
	initStack(num_variables());

	// Reactive-METIS input dump (diagnostic; off unless flag set).
	// Must be set BEFORE search begins so all reactive METIS calls are
	// captured.
	if (!config_.dump_reactive_metis_path.empty())
		setReactiveMetisDumpPath(config_.dump_reactive_metis_path);

	// Root-level forced literals: enqueue as input units so
	// simplePreProcess's existing BCP loop propagates them identically to
	// the original CNF's unit clauses. setLiteralIfFree + BCP is the same
	// primitive branching uses; we're just triggering it at root, before
	// any separator-consumption or var-branching code runs. Replaces the
	// old in-search forced-decision behavior, which on -sep / -cb
	// instances fired well after separator consumption and did not match
	// the documented "first decision" anchor methodology.
	for (int fl : config_.forced_decisions) {
		if (fl == 0) continue;
		unit_clauses_.push_back(
		    LiteralID((VariableIndex)std::abs(fl), fl > 0));
	}

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

		// Preprocessing completeness + state integrity guards.
		//   1. Unit-propagation saturation: every clause has ≥ 2 active
		//      literals (cheap; catches missed units).
		//   2. State integrity: the solver's in-memory representation
		//      is internally consistent (polarity, watches, binaries,
		//      occurrences).
		//   3. Failed-literal saturation: no variable has a polarity
		//      whose BCP derives conflict. DISABLED because BCP probing
		//      mutates watches and hides the t1_011 bug.
		verifyUnitPropagationSaturated("post-simplePreProcess");
		verifyStateIntegrity("post-simplePreProcess");
		verifyPostPreprocessCleanSlate("post-simplePreProcess");

		// Static WL labels for the canonical-key cascade. Computed
		// once on the post-preprocessing global formula. Used in
		// buildCanonicalKey to refine residual collision blocks
		// after dynamic WL settles. Cheap (~O(num_clauses) per iter,
		// 1 iter by default) and amortized across all later cache builds.
		// Always computed: the cascade fires whenever dynamic WL leaves
		// collisions, regardless of the wl_iterations cap on dynamic WL.
		static_wl_labels_ = computeStaticWLLabels(
		    num_variables(),
		    literal_pool_,
		    literals_,
		    literal_values_,
		    original_lit_pool_size_,
		    /*n_iters=*/1);

		// Diagnostic: dump all binaries from binary_links_ after preprocessing
		// so we can externally verify each is F-entailed. Disabled unless
		// the user provides -dumpBinaries <path>.
		{
			const char *bin_path = std::getenv("SHARPSAT_DUMP_BINARIES");
			if (bin_path) dumpBinariesAfterPreprocess(bin_path);
		}
		// verifyNoFailedLiterals("post-simplePreProcess");

		// Emit CNF reproducer of the post-preprocess in-memory formula.
		if (!config_.dump_preprocessed_path.empty()) {
			if (!dumpPreprocessedCnf(config_.dump_preprocessed_path))
				return;
			// Fall through: continue solving in memory.
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

		// Structural analyzer: pick hyperparameters (currently Stage-0 α)
		// from the post-preprocess formula. No-op unless -adaptive is on
		// and the user hasn't pinned α via -adaptiveAlpha.
		analyzeAndSetHyperparameters();

		// Inject redundant equivalences queued via
		// setPendingRedundantEquivalences. Translate from input-CNF var
		// space (what the equivalences were computed in) to the
		// post-compactification space using compact_to_orig_.
		// Equivalences whose vars were eliminated by preprocessing are
		// silently dropped — the elimination already captures the
		// equivalence's effect on the count.
		if (!pending_redundant_equivs_.empty()) {
			std::vector<unsigned> input_to_compact;
			if (!compact_to_orig_.empty()) {
				unsigned max_orig = 0;
				for (unsigned c = 1; c < compact_to_orig_.size(); c++)
					max_orig = std::max(max_orig, compact_to_orig_[c]);
				input_to_compact.assign(max_orig + 1, 0);
				for (unsigned c = 1; c < compact_to_orig_.size(); c++)
					input_to_compact[compact_to_orig_[c]] = c;
			}
			auto translate = [&](unsigned v_input) -> unsigned {
				if (input_to_compact.empty()) return v_input;
				if (v_input >= input_to_compact.size()) return 0;
				return input_to_compact[v_input];
			};
			unsigned injected = 0, dropped = 0;
			for (const auto &eq : pending_redundant_equivs_) {
				unsigned vx = translate(std::get<0>(eq));
				unsigned vy = translate(std::get<1>(eq));
				bool same_pol = std::get<2>(eq);
				if (vx == 0 || vy == 0 || vx == vy) { dropped++; continue; }
				if (addRedundantBinaryEquivalence(vx, vy, same_pol))
					injected++;
				else
					dropped++;
			}
			if (!config_.quiet)
				cout << "c o [arjun-light] injected " << injected
				     << " redundant equivalences (dropped " << dropped << ")\n";
			pending_redundant_equivs_.clear();
		}

		comp_manager_.initialize(literals_, literal_pool_, original_lit_pool_size_);
		comp_manager_.setRemovedClauses(&removed_clauses_);

		// Snapshot the root active-var count for the OPEN_WORK metric
		// (denominator of the progress measure: progress_bits =
		// n_root - log2(remaining_worst)).
		{
			Component &root = comp_manager_.superComponentOf(stack_.top());
			open_work_n_root_ = 0;
			for (auto vt = root.varsBegin(); *vt != varsSENTINEL; vt++)
				if (isActive(LiteralID(*vt, true))) open_work_n_root_++;
		}

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

	// OPEN_WORK: per-variant progress metric for probe_flags / portfolio
	// routing. On finish, n_open_comps=0 and progress_bits=n_root.
	// On timeout, n_open_comps>0 and progress_bits = n_root - log2(Σ 2^n_i)
	// over the open components captured at first time-bound break.
	{
		double progress_bits = open_work_captured_
		    ? (static_cast<double>(open_work_n_root_) - open_work_log2_bound_)
		    : static_cast<double>(open_work_n_root_);
		std::cerr << "OPEN_WORK"
		          << " n_root=" << open_work_n_root_
		          << " n_open_comps=" << (open_work_captured_ ? open_work_n_open_ : 0u)
		          << " bound_log2=" << (open_work_captured_ ? open_work_log2_bound_ : 0.0)
		          << " progress_bits=" << progress_bits
		          << " sizes=";
		for (size_t i = 0; i < open_work_sizes_.size(); i++) {
			if (i) std::cerr << ",";
			std::cerr << open_work_sizes_[i];
		}
		std::cerr << std::endl;
	}

	std::cerr << "MIDSEP_STATS"
	          << " decomp_attempts=" << mid_sep_decomp_attempts_
	          << " decomp_splits=" << mid_sep_decomp_splits_
	          << std::endl;
	std::cerr << "LEARN_FILTER_STATS"
	          << " learned_clauses=" << statistics_.num_clauses_learned_
	          << " dedup_dropped=" << statistics_.num_learned_dedup_dropped_
	          << " binary_filter_fires=" << statistics_.num_learned_binary_filtered_
	          << std::endl;
	double avg_comp = statistics_.num_comp_entries_
	                    ? (double)statistics_.sum_comp_vars_at_entry_ / statistics_.num_comp_entries_
	                    : 0.0;
	double avg_cascade = statistics_.num_decisions_
	                       ? (double)statistics_.num_implications_ / statistics_.num_decisions_
	                       : 0.0;
	{
		auto &cc2 = comp_manager_.contentCache();
		double avg_hit_size  = cc2.stats_hits  ? (double)cc2.stats_hit_vars_sum   / cc2.stats_hits   : 0.0;
		double avg_store_size= cc2.stats_stores? (double)cc2.stats_store_vars_sum / cc2.stats_stores : 0.0;
		std::cerr << "DIAG_STATS"
		          << " num_decisions=" << statistics_.num_decisions_
		          << " avg_bcp/dec=" << avg_cascade
		          << " avg_comp_at_entry=" << avg_comp
		          << " avg_L2_hit_size=" << avg_hit_size
		          << " max_L2_hit_size=" << cc2.stats_max_hit_size
		          << " avg_L2_store_size=" << avg_store_size
		          << " max_L2_store_size=" << cc2.stats_max_store_size
		          << std::endl;
		std::cerr << "L2_HIT_HIST"
		          << " [0-25)=" << cc2.stats_hit_buckets[0]
		          << " [25-50)=" << cc2.stats_hit_buckets[1]
		          << " [50-100)=" << cc2.stats_hit_buckets[2]
		          << " [100-200)=" << cc2.stats_hit_buckets[3]
		          << " [200-400)=" << cc2.stats_hit_buckets[4]
		          << " [400+)=" << cc2.stats_hit_buckets[5]
		          << std::endl;
		std::cerr << "L2_STORE_HIST"
		          << " [0-25)=" << cc2.stats_store_buckets[0]
		          << " [25-50)=" << cc2.stats_store_buckets[1]
		          << " [50-100)=" << cc2.stats_store_buckets[2]
		          << " [100-200)=" << cc2.stats_store_buckets[3]
		          << " [200-400)=" << cc2.stats_store_buckets[4]
		          << " [400+)=" << cc2.stats_store_buckets[5]
		          << std::endl;
	}
	{
		auto &cc = comp_manager_.contentCache();
		std::cerr << "FULL_CACHE_STATS"
		          << " l2_stores=" << cc.stats_stores
		          << " l2_hits=" << cc.stats_hits
		          << " l1_stores=" << cc.stats_l1_stores
		          << " l1_hits=" << cc.stats_l1_hits
		          << " l1_misses=" << cc.stats_l1_misses
		          << " total_lookups=" << (cc.stats_l1_hits + cc.stats_l1_misses)
		          << " total_hits=" << (cc.stats_hits + cc.stats_l1_hits)
		          << std::endl;
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


// =====================================================================
// OPEN_WORK snapshot — captured on first time-bound break.
//
// Walks the open components at the moment of timeout. "Open" =
// sub-components on the component stack that are not yet processed;
// this corresponds to the [remaining_components_ofs, unprocessed_end)
// range at each StackLevel. To avoid double-counting, we skip the
// `currentRemainingComponent` of each non-deepest frame, since that
// comp is being processed by the deeper frame (whose own open work
// captures it).
//
// For each open Component, count current-active vars; sum 2^n via
// log-sum-exp to stay in float range. Output is `log2(Σ 2^n_i)`,
// the worst-case bound on remaining enumeration cost.
// =====================================================================

void Solver::captureOpenWorkSnapshot() {
	open_work_sizes_.clear();
	unsigned max_n = 0;
	for (Component *comp : current_comp_chain_) {
		if (!comp) continue;
		unsigned n = 0;
		for (auto vt = comp->varsBegin(); *vt != varsSENTINEL; vt++) {
			if (isActive(LiteralID(*vt, true))) n++;
		}
		open_work_sizes_.push_back(n);
		if (n > max_n) max_n = n;
	}
	// The chain is nested: inner's vars ⊆ outer's vars. The outermost
	// (= max in the chain) is the worst-case bound on remaining work
	// for this entire branch of the search; the innermost is the
	// deepest in-progress comp. We report max as bound_log2 (the
	// conservative single-number bound) and the full chain so the
	// script can rank by innermost (= depth-of-progress) too.
	open_work_log2_bound_ = static_cast<double>(max_n);
	open_work_n_open_ = static_cast<unsigned>(open_work_sizes_.size());
}


bool Solver::BCP(unsigned start_at_stack_ofs) {
	for (unsigned int i = start_at_stack_ofs; i < literal_stack_.size(); i++) {
		LiteralID unLit = literal_stack_[i].neg();
		//BEGIN Propagate Bin Clauses
		// For LEARNED binaries (idx >= original_binary_link_count_) we
		// apply the component-membership filter, mirroring the long-clause
		// gate at learnedClauseInComponent: skip the binary when the
		// other endpoint's var is outside current_sub_varset_ AND still
		// active (X_TRI). Same rule as long clauses — assigned outside
		// vars are constants and don't bridge; only active outside vars
		// pose a cross-sub hazard. Original binaries (idx < orig_count)
		// are unfiltered: they're structural and always sound to fire.
		{
			unsigned orig_count = literal(unLit).original_binary_link_count_;
			unsigned idx = 0;
			for (auto bt = literal(unLit).binary_links_.begin();
					*bt != SENTINEL_LIT; bt++, idx++) {
				if (idx >= orig_count && !current_sub_varset_.empty()) {
					unsigned v = bt->var();
					bool in_mask = (v < current_sub_varset_.size()
					                && current_sub_varset_[v]);
					if (!in_mask
					    && literal_values_[LiteralID(v, true)] == X_TRI) {
						statistics_.num_learned_binary_filtered_++;
						continue;
					}
				}
				if (isResolved(*bt)) {
					if (config_.log_conflicts) {
						std::cerr << "CONFLICT_BIN clause=[" << unLit.toInt()
						          << "," << bt->toInt() << "] DL="
						          << stack_.get_decision_level() << " decisions=";
						for (auto l : literal_stack_)
							if (!var(l).ante.isAnt()) std::cerr << l.toInt() << ",";
						std::cerr << "\n";
					}
					setConflictState(unLit, *bt);
					return false;
				}
				setLiteralIfFree(*bt, Antecedent(unLit));
			}
		}
		//END Propagate Bin Clauses
		//BEGIN Propagate Redundant Binaries
		// Redundant lane: global lemmas implied by F (e.g. SCC
		// equivalences extracted by preprocessing). Sound to propagate
		// everywhere since they hold in every model of F. Not walked by
		// the component analyzer or canonical-key, so they never bridge
		// components in the cache-key sense. Same membership filter as
		// above: never propagate when the other endpoint is outside the
		// current sub-component AND active.
		for (auto bt = literal(unLit).redundant_binary_links_.begin();
				*bt != SENTINEL_LIT; bt++) {
			if (!current_sub_varset_.empty()) {
				unsigned v = bt->var();
				bool in_mask = (v < current_sub_varset_.size()
				                && current_sub_varset_[v]);
				if (!in_mask
				    && literal_values_[LiteralID(v, true)] == X_TRI) {
					statistics_.num_learned_binary_filtered_++;
					continue;
				}
			}
			if (isResolved(*bt)) {
				setConflictState(unLit, *bt);
				return false;
			}
			setLiteralIfFree(*bt, Antecedent(unLit));
		}
		//END Propagate Redundant Binaries
		for (auto itcl = literal(unLit).watch_list_.rbegin();
				*itcl != SENTINEL_CL; itcl++) {
			bool isLitA = (*beginOf(*itcl) == unLit);
			auto p_watchLit = beginOf(*itcl) + 1 - isLitA;
			auto p_otherLit = beginOf(*itcl) + isLitA;

			if (isSatisfied(*p_otherLit) || isClauseRemoved(*itcl)) {
				continue;
			}
			// Scope check for learned clauses: if a learned clause was
			// derived when some clauses C were removed, it's only sound
			// in contexts where all of C are still removed. Otherwise
			// skip — treat the clause as absent for BCP purposes.
			// (Non-learned clauses have no scope entry and are always OK.)
			//
			// Component-membership check: a learned clause D may only fire
			// during the solve of sub-component S iff vars(D) ⊆ S.varsBegin.
			// Without this gate, a learned clause whose vars span multiple
			// sub-components (after dynamic re-decomposition split previously-
			// connected vars) can propagate across the boundary, contaminating
			// the cached count of S with constraints not entailed by S alone.
			// Mask is empty at root → no filter; populated by SubVarsetGuard
			// at every solveComponent entry.
			if (*itcl >= (ClauseOfs)original_lit_pool_size_) {
				if (!learnedClauseInScope(*itcl)) continue;
				if (!learnedClauseInComponent(*itcl, current_sub_varset_)) continue;
			}
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
					if (config_.log_conflicts) {
						std::cerr << "CONFLICT_CL ofs=" << *itcl
						          << " DL=" << stack_.get_decision_level()
						          << " decisions=";
						for (auto l : literal_stack_)
							if (!var(l).ante.isAnt()) std::cerr << l.toInt() << ",";
						std::cerr << "\n";
					}
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
	const int L = config_.learn_level;
	if (!config_.perform_conflict_clause_learning || L < 1) {
		// Learning disabled for diagnosis: skip storing, still force the
		// asserting literal with NOT_A_CLAUSE antecedent (same
		// reasoning as the dedup-duplicate path below — sound).
	} else if (L >= 2 && uip.size() >= 2 && !maybeDedupClause(uip)) {
		statistics_.num_learned_dedup_dropped_++;
		// Duplicate — don't store, but still force the asserting literal
		// with an unscoped NOT_A_CLAUSE antecedent. Correct: under the
		// current state the literal IS entailed (same reasoning as for
		// a freshly-learned clause); we just don't persist it.
	} else {
		ante = addScopedUIPConflictClause(
		    uip,
		    /*pad_binary=*/  L >= 4,
		    /*record_scope=*/L >= 3);
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

	// Size gate: shared with reactive METIS via separatorSizeAcceptable.
	unsigned n_active = comp.num_variables();
	if (n_active == 0) return true;
	if (!separatorSizeAcceptable(filtered_sep_size, n_active)) {
		if (config_.verbose) {
			unsigned allowed = std::min((unsigned)(0.3 * (double)n_active), 20u);
			std::cout << "  TIER1_REJECT nd=" << nd_node
			          << " reason=size sep=" << filtered_sep_size
			          << " n=" << n_active
			          << " allowed=" << allowed << std::endl;
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

// Public wrapper exposing newton_branching_number as a Solver static method
// so callers in other translation units (e.g. solver_rec.cpp) can access it.
double Solver::tauBranchingNumber(double a, double b) {
	return newton_branching_number(a, b);
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

// Discrete coeff^k BCP-cascade addend, aggregated by min over the two
// polarities. For each polarity, a depth-bounded recursive walk over
// `binary_links_[¬lit]` returns:
//   - 1 if no forcing on the polarity (no active partner);
//   - coeff × Σ walk(forced_partner) otherwise.
// In a chain of k forced lits this evaluates to coeff^k; a fan of k
// forced at one level evaluates to coeff·k. The min aggregation matches
// #SAT counting semantics: both branches are traversed and tree depth is
// dominated by the weaker (less-cascading) side, so we score by that
// worst-case branch. This is empirically the best combination among
// {min, sum, τ-formula} — see commit message / benchmark log for
// experimental support on t1_021_k15_s1.
//
// TODO (long-term): re-express ALL scoring components (freq, activity,
// sep_bias, this cascade addend) in common log-rate (per-var work) units
// so they compose without per-instance weight tuning.
float Solver::computeCascadeScore(VariableIndex v) {
	if (!isActive(LiteralID(v, true))) return 0.0f;
	const int depth = config_.cascade_score_depth;
	const float coeff = 2.0f;
	std::unordered_set<unsigned> visited;
	visited.reserve(64);

	std::function<float(LiteralID, int)> walk =
	    [&](LiteralID lit, int d) -> float {
		if (d == 0) return 1.0f;
		if (!visited.insert(lit.raw()).second) return 1.0f;
		LiteralID complement = lit.neg();
		float forced_sum = 0.0f;
		bool any_forced = false;
		const auto &blinks = literal(complement).binary_links_;
		for (auto bt = blinks.begin(); *bt != SENTINEL_LIT; ++bt) {
			if (literal_values_[*bt] != X_TRI) continue;
			any_forced = true;
			forced_sum += walk(*bt, d - 1);
		}
		if (!any_forced) return 1.0f;
		return coeff * forced_sum;
	};

	float pos = walk(LiteralID(v, true), depth);
	visited.clear();
	float neg = walk(LiteralID(v, false), depth);
	return std::min(pos, neg);
}

// Gain-based BCP cascade estimate.
// gain(v=lit, d) = (# lits forced at this level) + 0.33·(# 3-clauses → binaries)
//                  + Σ_{forced lits fl} gain(fl, d-1)
// Hypothesis: setting `lit` true makes complement(lit) false. Clauses
// containing complement(lit) lose one literal:
//   - len 2 (binary, or len-3 with 1 already-false): become unit → forces remaining lit
//   - len 3 (3-clause with 0 false, all unassigned): becomes binary → 0.33
//   - len 4+: ignored
// Returns sum over the two polarities (v=true, v=false).
float Solver::computeBcpGainScore(VariableIndex v) {
	if (!isActive(LiteralID(v, true))) return 0.0f;
	const int max_depth = config_.cascade_score_depth;
	std::unordered_set<unsigned> visited;
	visited.reserve(64);

	std::function<float(LiteralID, int)> gain =
	    [&](LiteralID hypothetical_lit, int d) -> float {
		if (d == 0) return 0.0f;
		if (!visited.insert(hypothetical_lit.raw()).second) return 0.0f;
		LiteralID complement = hypothetical_lit.neg();
		float g = 0.0f;
		std::vector<LiteralID> forced_lits;

		// Binary chain: clauses where complement is one literal.
		// Setting hypothetical_lit true → complement is false → other lit
		// is forced.
		const auto &blinks = literal(complement).binary_links_;
		for (auto bt = blinks.begin(); *bt != SENTINEL_LIT; ++bt) {
			if (literal_values_[*bt] != X_TRI) continue;
			forced_lits.push_back(*bt);
		}
		g += (float)forced_lits.size();

		// Long-clause effects: iterate complement's occurrence list.
		const auto &occ = occurrence_lists_[complement];
		for (ClauseOfs ofs : occ) {
			if (isClauseRemoved(ofs)) continue;
			if (isSatisfied(ofs)) continue;
			// Count unassigned and track them (cap at 3 — we only care
			// about active_len ∈ {2, 3}; longer clauses are ignored).
			LiteralID u0 = SENTINEL_LIT, u1 = SENTINEL_LIT, u2 = SENTINEL_LIT;
			int n_unassigned = 0;
			for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; ++lt) {
				if (literal_values_[*lt] == X_TRI) {
					if (n_unassigned == 0) u0 = *lt;
					else if (n_unassigned == 1) u1 = *lt;
					else if (n_unassigned == 2) u2 = *lt;
					n_unassigned++;
					if (n_unassigned > 3) break;  // length 4+, ignore
				}
			}
			if (n_unassigned == 2) {
				// After hypothesis, becomes unit → forces the OTHER
				// unassigned literal.
				LiteralID other = (u0 == complement) ? u1 : u0;
				if (literal_values_[other] == X_TRI)
					forced_lits.push_back(other);
				g += 1.0f;
			} else if (n_unassigned == 3) {
				// One of u0/u1/u2 is the complement; the others form a
				// new binary. Just count, no recursion.
				g += 0.33f;
			}
			// n_unassigned > 3: ignore.
		}

		// Recurse on forced lits (newly-derived units → cascade further).
		if (d > 1) {
			for (LiteralID fl : forced_lits)
				g += gain(fl, d - 1);
		}
		return g;
	};

	float pos = gain(LiteralID(v, true), max_depth);
	visited.clear();
	float neg = gain(LiteralID(v, false), max_depth);
	return pos + neg;   // SUM, not min — both branches contribute.
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
	// Learned clauses are EXCLUDED from the METIS incidence graph: they
	// are dynamic artifacts of the search path so far and add edges that
	// don't reflect the formula's intrinsic structure. Including them
	// can produce a separator that's worse than the originals-only graph
	// would have yielded — METIS sees artificial cross-region connectivity.
	// Also, the post-decomposition learned-clause filter (in BCP, gated
	// by current_sub_varset_) drops bridge learned clauses whose vars
	// span the new sub-components, so even if METIS were to consider
	// them, BCP would never apply them within the resulting children.
	for (auto ct = comp.clsBegin(); *ct != clsSENTINEL; ++ct) {
		ClauseOfs ofs = comp_manager_.clauseOfsOf(*ct);
		if (isClauseRemoved(ofs) || isSatisfied(ofs)) continue;
		if (ofs >= (ClauseOfs)original_lit_pool_size_) continue;  // skip learned
		std::vector<unsigned> vars;
		for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; ++lt) {
			if (literal_values_[*lt] == X_TRI) vars.push_back(lt->var());
		}
		if (vars.size() >= 3) {
			long_clauses.push_back({(unsigned)ofs, std::move(vars)});
		} else if (vars.size() == 2) {
			// Pseudo-binary: a length-3+ clause that BCP has trimmed
			// to two active literals. The constraint is live and BCP
			// will propagate through it, AND the component analyzer
			// (alt_component_analyzer::searchClause) treats the
			// remaining two vars as connected via this clause. We
			// must include it as a METIS edge for the same reason —
			// otherwise METIS's view of connectivity diverges from
			// the analyzer's, and METIS receives spuriously-
			// disconnected inputs (diagnosed 2026-05-15: 100% of
			// reactive-METIS calls on t1_041/v176 had cc>1 by METIS
			// view but cc=1 by analyzer view, because pseudo-binaries
			// were skipped here).
			binary_pairs.push_back({vars[0], vars[1]});
		}
		// Size-1 (BCP-derived unit) is BCP's responsibility; not an edge.
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

// ---------------------------------------------------------------
// UIP clause minimization (+ invariant guards)
// ---------------------------------------------------------------
// Takes the raw 1-UIP clause (uipLit + tmp_clause) and optionally
// shortens it by resolving out literals whose antecedent clauses only
// reference variables already in the current surviving clause. This
// is a classical 1-UIP minimization step; the subtle bit is that the
// soundness criterion must be checked against the CURRENT surviving
// clause, not against a stale flag.
//
// Historical bug (fixed here): the previous implementation checked
// `seen[var()]`, which was set when a variable was visited during the
// 1-UIP walk and never cleared when a subsequent minimization step
// dropped that variable's literal. A drop decision could therefore
// rely on a literal that itself was (or would be) dropped — producing
// a clause that is NOT entailed by F, silently pruning models.
//
// Current implementation: maintain a live `in_clause[v]` bitmap that
// is flipped off every time a literal is dropped. Iterate the drop
// pass to fixed point. Each drop's soundness is verified at the
// moment of drop against the CURRENT in_clause state.
//
// Structural guards below are always on (Release). An opt-in
// resolution-replay guard (config_.verify_learn) additionally
// reconstructs the final clause from the recorded drop chain and
// asserts bit-equality.
void Solver::minimizeAndStoreUIPClause(LiteralID uipLit,
		vector<LiteralID> & tmp_clause, bool /*seen_stale*/[]) {
	static deque<LiteralID> clause;
	clause.clear();
	assertion_level_ = 0;

	// learn_level >= 5 attempts minimization. NOTE: the iterative
	// implementation below maintains live in_clause[] state and each
	// drop is locally sound resolution, yet on t1_011's shrunken
	// reproducer (latest_correct.cnf) it still produces a 2^17
	// undercount. A subtler unsoundness remains — possibly cascade
	// through learned-clause antecedents. Until the correct derivation
	// is identified, default is learn_level=4 (no minimization).
	const bool do_minimize = (config_.learn_level >= 5);

	// --- Guard 1 (pre): every tmp_clause literal must be F_TRI at
	// learn time (BCP-consistent 1-UIP candidate). ----------
	for (auto lit : tmp_clause) {
		if (existsUnitClauseOf(lit.var())) continue;
		if (literal_values_[lit] != F_TRI) {
			std::cerr << "\n*** LEARN_CANDIDATE_NOT_FALSIFIED ***\n"
			          << "  tmp_clause lit=" << lit.toInt()
			          << " value=" << (int)literal_values_[lit]
			          << " (expected F_TRI=" << (int)F_TRI << ")\n";
			std::cerr.flush();
			std::abort();
		}
	}

	// --- in_clause[v]: live "is variable v in the current surviving
	// clause?" bitmap. Reset per call via .assign, reused thread-local
	// to avoid heap alloc per conflict. -------------------------
	static std::vector<bool> in_clause;
	in_clause.assign(num_variables() + 2, false);
	for (auto lit : tmp_clause) {
		if (!existsUnitClauseOf(lit.var()))
			in_clause[lit.var()] = true;
	}
	// NOTE: intentionally NOT including uipLit in in_clause. The old
	// implementation didn't, and experimentation showed that including
	// it reintroduces the t1_011 undercount. Hypothesis: at the moment
	// minimization runs, uipLit.var() is also current-DL, so its
	// polarity is the one forced by THIS conflict's BCP chain. Using
	// it to justify dropping lower-DL lits produces a resolvent that
	// depends on the current-DL decision — which, after backjumping,
	// changes polarity. The clause would then no longer be entailed.
	// Keep the conservative behaviour: only lower-DL vars in tmp_clause
	// can justify drops.

	// --- track which tmp_clause positions survive + why they were
	// dropped (for the resolution-replay verifier). --------------
	std::vector<bool> survives(tmp_clause.size(), true);
	// Pre-filter: variables that exist as DL-0 units are always safe
	// to drop (their value is globally fixed, the clause is never
	// needed to pin them down).
	for (unsigned i = 0; i < tmp_clause.size(); i++) {
		if (existsUnitClauseOf(tmp_clause[i].var()))
			survives[i] = false;
	}

	// Drop-chain record for optional replay: (position in tmp_clause,
	// antecedent). Ordered in the sequence drops happened.
	struct DropRec { unsigned pos; Antecedent ante; };
	std::vector<DropRec> drop_log;

	// --- helper: would dropping `lit` be sound against the CURRENT
	// in_clause state? lit must have an antecedent (else can't drop).
	auto ante_all_in_clause = [&](LiteralID lit) -> bool {
		if (!hasAntecedent(lit)) return false;
		if (getAntecedent(lit).isAClause()) {
			for (auto it = beginOf(getAntecedent(lit).asCl()) + 1;
					*it != SENTINEL_CL; it++) {
				if (it->var() == lit.var()) continue;
				if (!in_clause[it->var()]) return false;
			}
			return true;
		} else {
			LiteralID a = getAntecedent(lit).asLit();
			if (a.var() == lit.var()) return false;
			return in_clause[a.var()];
		}
	};

	// --- Iterative minimization to fixed point. Drop a literal only
	// when its antecedent's OTHER literals are all in the CURRENT
	// in_clause. At drop time, remove the literal's variable from
	// in_clause so subsequent iterations see the updated state. ----
	if (do_minimize) {
		bool changed = true;
		while (changed) {
			changed = false;
			for (unsigned i = 0; i < tmp_clause.size(); i++) {
				if (!survives[i]) continue;
				LiteralID lit = tmp_clause[i];

				// --- Guard 2 (per-drop precondition): at this moment
				// the lit must be in in_clause (it survives). -------
				assert(in_clause[lit.var()]
				       && "in_clause must agree with survives[]");

				if (ante_all_in_clause(lit)) {
					survives[i] = false;
					in_clause[lit.var()] = false;
					drop_log.push_back({i, getAntecedent(lit)});
					changed = true;
				}
			}
		}
	}

	// --- Guard 3 (post): structural invariants of the final clause.
	// We do this before assembling `clause` so the final composition
	// step has a clean input to work with.
	{
		unsigned n_per_var_check_cap = num_variables() + 2;
		static std::vector<bool> seen_var;
		seen_var.assign(n_per_var_check_cap, false);

		auto check_lit = [&](LiteralID lit) {
			if (lit.var() >= n_per_var_check_cap) {
				std::cerr << "\n*** LEARN_VAR_OUT_OF_RANGE ***\n"
				          << "  lit=" << lit.toInt() << "\n";
				std::abort();
			}
			if (seen_var[lit.var()]) {
				std::cerr << "\n*** LEARN_DUPLICATE_VAR ***\n"
				          << "  var=" << lit.var() << " appears twice\n";
				std::abort();
			}
			seen_var[lit.var()] = true;
			if (literal_values_[lit] == T_TRI) {
				std::cerr << "\n*** LEARN_SATISFIED_LIT ***\n"
				          << "  lit=" << lit.toInt()
				          << " is T_TRI in learned clause\n";
				std::abort();
			}
		};

		if (uipLit.var()) {
			if (var(uipLit).decision_level != stack_.get_decision_level()) {
				std::cerr << "\n*** LEARN_UIP_WRONG_DL ***\n"
				          << "  uipLit=" << uipLit.toInt()
				          << " DL=" << var(uipLit).decision_level
				          << " current DL=" << stack_.get_decision_level() << "\n";
				std::abort();
			}
			check_lit(uipLit);
		}

		int current_dl = stack_.get_decision_level();
		for (unsigned i = 0; i < tmp_clause.size(); i++) {
			if (!survives[i]) continue;
			LiteralID lit = tmp_clause[i];
			check_lit(lit);
			if (var(lit).decision_level >= current_dl) {
				std::cerr << "\n*** LEARN_NONUIP_AT_CURRENT_DL ***\n"
				          << "  lit=" << lit.toInt()
				          << " DL=" << var(lit).decision_level
				          << " current DL=" << current_dl << "\n";
				std::abort();
			}
			if (!in_clause[lit.var()]) {
				std::cerr << "\n*** LEARN_IN_CLAUSE_INCONSISTENT ***\n"
				          << "  surviving lit=" << lit.toInt()
				          << " not marked in in_clause\n";
				std::abort();
			}
		}
	}

	// --- Guard 4 (optional, opt-in via -verifyLearn): replay the
	// resolution chain. Start from the original tmp_clause ∪ {uipLit}
	// as a set-of-variables, then for each recorded drop verify the
	// antecedent's other-vars are in the current set, apply the drop
	// (remove this var), and at the end assert the set equals the
	// final clause's var set. This catches any divergence between the
	// iterative fix and a direct resolution replay. ----------------
	if (do_minimize && config_.verify_learn && !drop_log.empty()) {
		std::vector<bool> replay_set(num_variables() + 2, false);
		for (auto lit : tmp_clause)
			if (!existsUnitClauseOf(lit.var()))
				replay_set[lit.var()] = true;
		if (uipLit.var()) replay_set[uipLit.var()] = true;

		for (const auto &dr : drop_log) {
			LiteralID lit = tmp_clause[dr.pos];
			Antecedent a = dr.ante;
			// Verify antecedent's other vars are in replay_set.
			bool ok = true;
			if (a.isAClause()) {
				for (auto it = beginOf(a.asCl()) + 1;
						*it != SENTINEL_CL; it++) {
					if (it->var() == lit.var()) continue;
					if (!replay_set[it->var()]) { ok = false; break; }
				}
			} else {
				ok = (a.asLit().var() != lit.var()
				      && replay_set[a.asLit().var()]);
			}
			if (!ok) {
				std::cerr << "\n*** VERIFY_LEARN_REPLAY_FAILED ***\n"
				          << "  drop of lit=" << lit.toInt()
				          << " did not satisfy resolution precondition"
				          << " at replay time\n";
				std::abort();
			}
			replay_set[lit.var()] = false;
		}
		// Final set must equal { surviving tmp_clause vars } ∪ {uipLit.var}.
		std::vector<bool> expected(num_variables() + 2, false);
		for (unsigned i = 0; i < tmp_clause.size(); i++)
			if (survives[i]) expected[tmp_clause[i].var()] = true;
		if (uipLit.var()) expected[uipLit.var()] = true;
		for (unsigned v = 0; v < replay_set.size(); v++) {
			if (replay_set[v] != expected[v]) {
				std::cerr << "\n*** VERIFY_LEARN_REPLAY_MISMATCH ***\n"
				          << "  replay and surviving disagree at var=" << v << "\n";
				std::abort();
			}
		}
	}

	// --- assemble the final clause: highest-DL lit at front (after
	// uipLit) for the watcher scheme.
	for (unsigned i = 0; i < tmp_clause.size(); i++) {
		if (!survives[i]) continue;
		LiteralID lit = tmp_clause[i];
		if (var(lit).decision_level >= assertion_level_) {
			assertion_level_ = var(lit).decision_level;
			clause.push_front(lit);
		} else {
			clause.push_back(lit);
		}
	}
	if (uipLit.var() != 0) {
		assert(var(uipLit).decision_level == stack_.get_decision_level());
		clause.push_front(uipLit);
	}
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
			ClauseOfs ante_cl = getAntecedent(curr_lit).asCl();
			// Invariant A (gated): if the antecedent is a learned clause,
			// it must be in scope under the CURRENT removed_clauses_.
			// At firing time the BCP scope check ensured in-scope; this
			// guard catches the case where removed_clauses_ has grown
			// (via clause branching) since the firing, making the
			// antecedent invalid for the current resolution step.
			if (config_.check_learn_invariants
			    && ante_cl >= (ClauseOfs)original_lit_pool_size_
			    && !learnedClauseInScope(ante_cl)) {
				std::cerr << "\n*** INV_A_ANTECEDENT_OUT_OF_SCOPE_AT_ANALYSIS ***\n"
				          << "  curr_lit=" << curr_lit.toInt()
				          << "  ante_cl=" << ante_cl
				          << "  curr_lit_DL=" << var(curr_lit).decision_level
				          << "  current_DL=" << DL
				          << "  current_removed=" << removed_clauses_.size()
				          << "\n";
				std::cerr.flush();
				std::abort();
			}
			updateActivities(ante_cl);
			assert(curr_lit == *beginOf(ante_cl));

			for (auto it = beginOf(ante_cl) + 1;
					*it != SENTINEL_CL; it++) {
				// Invariant B (gated): the antecedent's other literals
				// must be F_TRI right now. They were F_TRI at firing
				// time; if any is no longer F_TRI, the trail's
				// monotonicity invariant has been violated.
				if (config_.check_learn_invariants
				    && it->var() != curr_lit.var()
				    && literal_values_[*it] != F_TRI) {
					std::cerr << "\n*** INV_B_ANTECEDENT_OTHER_LIT_NOT_FALSE ***\n"
					          << "  curr_lit=" << curr_lit.toInt()
					          << "  ante_cl=" << ante_cl
					          << "  other_lit=" << it->toInt()
					          << "  other_lit_value=" << (int)literal_values_[*it]
					          << " (expected F_TRI=" << (int)F_TRI << ")\n";
					std::cerr.flush();
					std::abort();
				}
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
			// Invariant B (binary case): the falsified partner literal
			// must be F_TRI right now.
			if (config_.check_learn_invariants
			    && literal_values_[alit] != F_TRI) {
				std::cerr << "\n*** INV_B_BIN_ANTECEDENT_NOT_FALSE ***\n"
				          << "  curr_lit=" << curr_lit.toInt()
				          << "  partner=" << alit.toInt()
				          << "  partner_value=" << (int)literal_values_[alit]
				          << " (expected F_TRI=" << (int)F_TRI << ")\n";
				std::cerr.flush();
				std::abort();
			}
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

// Explicit clean-slate reset for everything the search phase will
// accumulate into. Called at the tail of HardWireAndCompact. See
// verifyPostPreprocessCleanSlate for the invariant this establishes.
void Solver::resetPostPreprocessScratch() {
	// Instance-level search-scoped containers.
	conflict_clauses_.clear();
	unit_clauses_.clear();
	removed_clauses_.clear();
	learned_clause_scope_.clear();
	// Bloom filter: force-re-init to zeros (re-initializing the member
	// in place). Using a fresh default-constructed filter is the
	// cheapest way to reset the bitset without exposing a reset method.
	learned_clause_sig_ = ClauseSigFilter();

	// Solver-level search-scoped state.
	uip_clauses_.clear();
	violated_clause.clear();
	assertion_level_ = 0;
	last_ccl_deletion_time_ = 0;
	last_ccl_cleanup_time_ = 0;

	// Per-variable residue. compactVariables default-constructs every
	// Variable (ante = Antecedent(), decision_level = INVALID_DL) so
	// this is defensive: belt-and-suspenders in case a future refactor
	// changes that.
	for (unsigned v = 1; v < variables_.size(); v++) {
		variables_[v].ante = Antecedent(NOT_A_CLAUSE);
		variables_[v].decision_level = INVALID_DL;
	}
}
