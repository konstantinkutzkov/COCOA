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
#include "sat_check.h"
#include <deque>
#include <cmath>
#include <fstream>
#include <iostream>
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
	is_branch_constraint_.assign(variables_.size(), false);

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
			LiteralID l0 = int_to_lit(c[0]);
			LiteralID l1 = int_to_lit(c[1]);
			literal(l0).addWatchLinkTo(ofs, l1);  // blocker = other watch
			literal(l1).addWatchLinkTo(ofs, l0);
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

	// TD-score load (from -tdScoreFile). Must happen after createfromFile
	// so num_variables() is known. Vector is indexed by VariableIndex
	// (1-based), so we size to num_variables()+1.
	if (!config_.td_score_file.empty()) {
		loadTdScoreFile(config_.td_score_file);
	}

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

		// Compute fixed branching order if requested. Must happen after
		// comp_manager_.initialize() (so clauseIdToOfs is populated).
		if (config_.picker_order != SolverConfiguration::NONE) {
			computeFixedBranchOrder();
		}
		comp_manager_.setRemovedClauses(&removed_clauses_);

		// Phase 1 SAT-check diagnostic: initialize the persistent CMS
		// solver with the post-preprocess original CNF if the flag is
		// on. Allocated lazily — bear no startup cost when -satCheckEvery=0.
		if (config_.sat_check_every > 0) {
			sat_check_init_();
		}

		// Derivative-cache init. Sets up content-aware hash tracking
		// arrays and enables the per-literal hooks. Required by both
		// the Phase 1/2 diagnostic probe (-derivCacheEvery > 0) and
		// the Phase 3 bias path (-derivCacheBias > 0). No startup
		// cost when both flags are 0.
		if (config_.deriv_cache_every > 0 || config_.deriv_cache_bias > 0) {
			deriv_cache_init_();
		}

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

		// Calibration mode: the countSATRec above (run under -t) WARMED the
		// cache. Now estimate this hash mode's cache effectiveness via
		// Monte-Carlo dives, print DIVE_STATS, and RETURN before printShort()
		// — so NO model count is emitted. These numbers feed only the
		// portfolio's hash-mode choice. See Solver::diveSample.
		if (config_.calibrate_dive > 0) {
			DiveStats ds = diveSample(config_.calibrate_dive, config_.calibrate_seed);
			const char *mode = (config_.cache_hash_mode == SolverConfiguration::IDENTITY)
			                       ? "identity" : "canonical";
			double hr   = ds.n_probes ? (double)ds.n_hits / (double)ds.n_probes : 0.0;
			double swhr = ds.weighted_probes > 0.0
			                  ? ds.weighted_hits / ds.weighted_probes : 0.0;
			double us_per_key = ds.n_probes ? ds.keybuild_us / (double)ds.n_probes : 0.0;
			stopwatch_.stop();
			cout << "DIVE_STATS"
			     << " mode=" << mode
			     << " dives=" << ds.n_dives
			     << " probes=" << ds.n_probes
			     << " hits=" << ds.n_hits
			     << " hit_rate=" << hr
			     << " size_weighted_hit_rate=" << swhr
			     << " us_per_key=" << us_per_key
			     << " cache_entries=" << comp_manager_.contentCache().size()
			     << "\n";
			cout.flush();
			return;
		}

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

	printOpStats("FINAL");
	printBcpPaths("FINAL");

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
	std::cerr << "SCC_UNSAT_STATS"
	          << " enabled=" << (config_.use_scc_unsat_prune ? 1 : 0)
	          << " pruned_lit=" << statistics_.num_scc_unsat_pruned_lit_
	          << " pruned_clause=" << statistics_.num_scc_unsat_pruned_clause_
	          << " total=" << (statistics_.num_scc_unsat_pruned_lit_
	                            + statistics_.num_scc_unsat_pruned_clause_)
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

	// Final Phase 3 stats (one-shot, regardless of 60s cadence).
	if (config_.deriv_cache_bias > 0 || config_.deriv_cache_every > 0) {
		std::cerr << "DERIV_CACHE_FINAL"
		          << " probes="        << deriv_cache_n_probes_
		          << " var_xor="       << deriv_cache_n_xor_hits_
		          << " var_real="      << deriv_cache_n_real_hits_
		          << " cl_xor="        << deriv_cache_n_cl_xor_hits_
		          << " cl_real="       << deriv_cache_n_cl_real_hits_
		          << " used_var_both=" << deriv_cache_n_used_var_both_
		          << " used_var_one="  << deriv_cache_n_used_var_one_
		          << " used_cl_ie="    << deriv_cache_n_used_cl_ie_
		          << " cache_size="    << (deriv_cache_xors_seen_
		                                   ? deriv_cache_xors_seen_->inserts() : 0)
		          << " bias_ms="       << (uint64_t)(deriv_cache_bias_total_us_/1000)
		          << " canonical_ms="  << (uint64_t)(deriv_cache_bias_canonical_us_/1000)
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
// Computes the true upper bound on remaining enumeration work by
// enumerating ALL unfinished subtrees at snapshot time:
//
//   (a) The deepest currently-in-progress sub-component (the leaf of
//       current_comp_chain_).
//   (b) For each StackLevel, the queued-but-not-yet-started sibling
//       components on the level's current branch — i.e., components
//       at comp_stack indices [remaining_components_ofs,
//       unprocessed_components_end - 1) (excluding the "current" one
//       at end-1, which is the super of the deeper level and is
//       already accounted for via (a) and the deeper-level walk).
//   (c) For each StackLevel where !isSecondBranch(), the OTHER-polarity
//       subtree that hasn't been explored yet. Its size is
//       super_component[k].var_list_size - 1 (one for the branching
//       variable, which is fixed at flipped value).
//
// All sizes use the COMPONENT VAR-LIST SIZE (fixed at decomposition
// time), NOT the current X_TRI count. The var-list size is the count
// the user's model wants: at the time the subtree was created, it had
// this many free variables; we use 2^size as the worst-case enumeration
// upper bound. Using current X_TRI would make the metric oscillate as
// deeper levels assign/unassign vars in the same var-list — spuriously
// suggesting progress or regress from trail churn that's already
// accounted for elsewhere in the open-list.
//
// log2(Σ 2^n_i) is then computed via log-sum-exp:
//     log2_bound = max(n_i) + log2(Σ 2^(n_i - max))
//
// progress_bits = n_root - log2_bound, the monotone odometer.
// =====================================================================

void Solver::captureOpenWorkSnapshot() {
	open_work_sizes_.clear();

	// Active-var count of a Component: count of vars in its var-list
	// that are STILL X_TRI right now. This is the "leaves under
	// current node" measure — the search-tree depth measured by
	// vars not yet assigned. For (a), this shrinks as deeper levels
	// assign more vars in this comp's var-list, and grows back upon
	// deeper backtrack.
	auto active_in_component = [&](Component *c) -> unsigned {
		unsigned n = 0;
		for (auto vt = c->varsBegin(); *vt != varsSENTINEL; vt++)
			if (isActive(LiteralID(*vt, true))) n++;
		return n;
	};

	// (a) Deepest currently-in-progress sub-component.
	if (!current_comp_chain_.empty() && current_comp_chain_.back()) {
		open_work_sizes_.push_back(
		    active_in_component(current_comp_chain_.back()));
	}

	// Walk the decision stack — (b) queued siblings + (c) other-polarity.
	for (unsigned k = 0; k < stack_.size(); k++) {
		StackLevel &sl = stack_[k];

		// (b) Queued siblings under the current branch at this level.
		// "Queued" = comp_stack indices [remaining_components_ofs,
		// unprocessed_components_end - 1). The "current" at end-1 is
		// the super of the deeper level (or, at the deepest level, is
		// already counted via (a)) — so always exclude it.
		unsigned q_begin = sl.remaining_components_ofs();
		unsigned q_end   = sl.unprocessed_components_end();
		if (q_end > q_begin) q_end--;  // exclude the "current" at end-1
		for (unsigned ci = q_begin; ci < q_end; ci++) {
			Component *c = comp_manager_.componentAt(ci);
			if (!c) continue;
			open_work_sizes_.push_back(active_in_component(c));
		}

		// (c) Other-polarity branch at this level (if not yet started).
		// Size = sl.active_at_push() - 1, where active_at_push is the
		// count of active vars in the parent component captured at the
		// moment this stack level was pushed (set by branchOnLiteral /
		// branchOnClause). Fixed thereafter, so the metric is invariant
		// under deeper descent / backtrack (vars assigned at deeper levels
		// don't change this count).
		if (!sl.isSecondBranch()) {
			unsigned a = sl.active_at_push();
			unsigned n = (a > 0) ? a - 1 : 0;
			open_work_sizes_.push_back(n);
		}
	}

	// log-sum-exp.
	if (open_work_sizes_.empty()) {
		open_work_log2_bound_ = 0.0;
	} else {
		unsigned max_n = 0;
		for (unsigned n : open_work_sizes_) if (n > max_n) max_n = n;
		double tail_sum = 0.0;
		for (unsigned n : open_work_sizes_) {
			tail_sum += std::exp2((double)((int)n - (int)max_n));
		}
		open_work_log2_bound_ = (double)max_n + std::log2(tail_sum);
	}
	open_work_n_open_ = static_cast<unsigned>(open_work_sizes_.size());

	// Override stack-walk bound with the leaf-credit-conservation value:
	//   remaining = 2^n_root - 2^closed_log_sum
	//   log2(remaining) = closed + log2(2^(n_root - closed) - 1)
	// closed_log_sum_ is the cumulative log-sum-exp over every LEAF
	// event's abstract budget. By the budget-distribution invariant
	// (binary branches split B→B-1,B-1; decompositions split B across
	// sub-comps via logsumexp), closed_log_sum_ grows monotonically
	// toward exactly n_root at finish, so `remaining` only shrinks
	// → bound monotone → progress_bits monotone. This replaces the
	// stack-walk bound, which was structurally non-monotone
	// (chain.back transitions caused spurious bound growth).
	if (closed_log_sum_ != -std::numeric_limits<double>::infinity()) {
		double n_root_d = (double)open_work_n_root_;
		double diff = n_root_d - closed_log_sum_;
		if (diff <= 0.0) {
			// closed >= n_root: everything accounted for; bound = 0.
			open_work_log2_bound_ = 0.0;
		} else if (diff > 60.0) {
			// closed << n_root: 2^diff - 1 ≈ 2^diff, log2 ≈ diff, so
			// bound = closed + diff = n_root. Avoid the subtraction.
			open_work_log2_bound_ = n_root_d;
		} else {
			// log2(2^n - 2^c) = c + log2(2^(n-c) - 1)
			open_work_log2_bound_ =
			    closed_log_sum_ + std::log2(std::exp2(diff) - 1.0);
		}
	}
}


bool Solver::BCP(unsigned start_at_stack_ofs) {
	OpTimer _t_bcp(this, OP_BCP);
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
		//
		// Gate on any_redundant_binaries_present_: when -arjun_light is
		// off (the common case) no equivalences are ever injected, the
		// lanes are globally empty, and the loop would do nothing useful
		// while still paying a `literal(unLit).redundant_binary_links_`
		// cache-line touch + .begin()/deref/compare on EVERY propagation.
		if (any_redundant_binaries_present_) {
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
		}
		//END Propagate Redundant Binaries
		for (auto itcl = literal(unLit).watch_list_.rbegin();
				itcl->ofs != SENTINEL_CL; itcl++) {
			bcp_long_visits_++;

			// BLOCKER FAST PATH: if the cached blocker literal is currently
			// true, the clause is satisfied — skip without touching the
			// clause body. Single literal_values_ read; saves the
			// beginOf(ofs)/isLitA/p_otherLit deref chain. Glucose/MiniSat-2
			// pattern. Soundness: blocker is a literal in the clause; if
			// true, clause is satisfied in any context.
			if (isSatisfied(itcl->blocker)) {
				bcp_path_B_++;
				continue;
			}

			bool isLitA = (*beginOf(itcl->ofs) == unLit);
			auto p_watchLit = beginOf(itcl->ofs) + 1 - isLitA;
			auto p_otherLit = beginOf(itcl->ofs) + isLitA;

			// Blocker miss but the real other watch is satisfied — refresh
			// the stale blocker so future visits hit the fast path.
			if (isSatisfied(*p_otherLit)) {
				itcl->blocker = *p_otherLit;
				bcp_path_B_++;
				continue;
			}
			if (isClauseRemoved(itcl->ofs)) {
				bcp_path_A_++;
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
			if (itcl->ofs >= (ClauseOfs)original_lit_pool_size_) {
				if (!learnedClauseInScopeOrSound(itcl->ofs, config_.sound_provenance)) {
					bcp_path_C1_sound_fail_++;
					continue;
				}
				if (!learnedClauseInComponent(itcl->ofs, current_sub_varset_)) {
					bcp_path_C2_comp_fail_++;
					continue;
				}
			}
			auto itL = beginOf(itcl->ofs) + 2;
			auto itL_start = itL;
			bcp_scan_invocations_++;
			while (isResolved(*itL))
				itL++;
			bcp_scan_lits_walked_ += (uint64_t)(itL - itL_start);
			// either we found a free or satisfied lit
			if (*itL != SENTINEL_LIT) {
				if ((itL - itL_start) <= 1) bcp_path_D_early_++;
				else bcp_path_E_late_++;
				// New entry's blocker = the OTHER watch (which we know is
				// X_TRI or T_TRI here — we passed the isSatisfied check).
				literal(*itL).addWatchLinkTo(itcl->ofs, *p_otherLit);
				swap(*itL, *p_watchLit);
				*itcl = literal(unLit).watch_list_.back();
				literal(unLit).watch_list_.pop_back();
			} else {
				if (setLiteralIfFree(*p_otherLit, Antecedent(itcl->ofs))) { // implication
					bcp_path_F_propagate_++;
					if (isLitA)
						swap(*p_otherLit, *p_watchLit);
					// *p_otherLit is now T_TRI; cache as blocker so a future
					// re-visit (after backtrack + re-descend) fast-paths.
					itcl->blocker = *p_otherLit;
				} else {
					bcp_path_G_conflict_++;
					if (config_.log_conflicts) {
						std::cerr << "CONFLICT_CL ofs=" << itcl->ofs
						          << " DL=" << stack_.get_decision_level()
						          << " decisions=";
						for (auto l : literal_stack_)
							if (!var(l).ante.isAnt()) std::cerr << l.toInt() << ",";
						std::cerr << "\n";
					}
					if (config_.verbose)
						cout << "  CONFLICT_CL=" << itcl->ofs
							 << " unLit=" << unLit.toInt()
							 << " removed=" << isClauseRemoved(itcl->ofs) << endl;
					setConflictState(itcl->ofs);
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
		    && !learnedClauseInScopeOrSound(cl_ofs, config_.sound_provenance)) {
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
	if (L < 1) {
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
		// INV_T5: verify the learned clause is entailed by F\removed_clauses_.
		// Env-gated. No-op when env var unset.
		if (ante.isAClause())
			verifyLearnedClauseSound(uip, ante.asCl(),
			                          "commitFailedLiteral");
		// Record provenance: the antecedent chain (long-clause
		// resolutions) plus the conflict-trigger clause. Binary
		// antecedents (chain_binaries_) are skipped — original
		// binaries are always sound; padded learned binaries appear
		// in chain_ via their ClauseOfs.
		if (ante.isAClause() && config_.sound_provenance)
			recordLearnedClauseProvenance(ante.asCl());
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
		    && !learnedClauseInScopeOrSound(ofs, config_.sound_provenance)) continue;
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
		VariableIndex best = 0;
		double best_score = -1.0;
		for (VariableIndex v : candidates) {
			if (cheap[v] > best_score) { best_score = cheap[v]; best = v; }
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
		// Branch-constraint vars are never droppable — they represent
		// structural ¬C-branch literals that MUST appear in the
		// learned clause to keep it sound for F.
		if (is_branch_constraint_[lit.var()]) return false;
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

	// Reset the antecedent-chain trace for INV_T5 diagnostics.
	last_analysis_chain_.clear();
	last_analysis_chain_binaries_.clear();
	last_analysis_violated_clause_ = violated_clause;  // snapshot

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

		// Branch-constraint dispatch: the variable was set by
		// branchOnClause's negate arm as a structural ¬C-conjunct
		// (not a regular decision, not a regular propagation). Don't
		// resolve through any antecedent — preserve curr_lit's negation
		// in the learned clause so the learned clause captures the
		// branching constraint and is sound for F. See
		// docs/branchonclause_branch_constraint_plan.md.
		if (is_branch_constraint_[curr_lit.var()]) {
			tmp_clause.push_back(curr_lit.neg());
			curr_lit = NOT_A_LIT;
			continue;
		}

		assert(hasAntecedent(curr_lit));

		//cout << "{" << curr_lit.toInt() << "}";
		if (getAntecedent(curr_lit).isAClause()) {
			ClauseOfs ante_cl = getAntecedent(curr_lit).asCl();
			last_analysis_chain_.push_back(ante_cl);
			// Invariant A (gated): if the antecedent is a learned clause,
			// it must be in scope under the CURRENT removed_clauses_.
			// At firing time the BCP scope check ensured in-scope; this
			// guard catches the case where removed_clauses_ has grown
			// (via clause branching) since the firing, making the
			// antecedent invalid for the current resolution step.
			if (config_.check_learn_invariants
			    && ante_cl >= (ClauseOfs)original_lit_pool_size_
			    && !learnedClauseInScopeOrSound(ante_cl, config_.sound_provenance)) {
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
			last_analysis_chain_binaries_.push_back(alit);
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

	// Reset the antecedent-chain trace for INV_T5 diagnostics.
	last_analysis_chain_.clear();

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

		// Branch-constraint dispatch: see recordLastUIPCauses above
		// and docs/branchonclause_branch_constraint_plan.md.
		if (is_branch_constraint_[curr_lit.var()]) {
			tmp_clause.push_back(curr_lit.neg());
			continue;
		}

		assert(hasAntecedent(curr_lit));

		if (getAntecedent(curr_lit).isAClause()) {
			last_analysis_chain_.push_back(getAntecedent(curr_lit).asCl());
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
	is_branch_constraint_.assign(variables_.size(), false);
	for (unsigned v = 1; v < variables_.size(); v++) {
		variables_[v].ante = Antecedent(NOT_A_CLAUSE);
		variables_[v].decision_level = INVALID_DL;
	}
}

// ============================================================================
// Phase 1 SAT-check diagnostic. See solver_config.h::sat_check_every.
//
// Builds one incremental CMSat::SATSolver populated with the post-preprocess
// original CNF (long + binary). On each diagnose call we pass the current
// literal_stack_ as assumptions, with a small conflict budget. The solver
// retains learned clauses across calls so successive checks on similar
// assignments may settle faster (incremental SAT).
//
// SOUNDNESS: This path is *diagnostic only* — it never modifies counts,
// returns, or any solver state outside its own stat counters. The
// search runs as if -satCheckEvery were not set.
//
// Restriction: caller (in solver_rec.cpp) only invokes this when
// `removed_clauses_.empty()`. Inside a clause-branch context, the
// effective formula has clauses removed that CMS still believes are
// in scope, so a "SAT" result is meaningless and "UNSAT" could be
// either a real UNSAT or a removed-clause false positive. Phase 2 (if
// we proceed) will need to handle scope, e.g. by gating on this same
// emptiness check.
// ============================================================================

void Solver::sat_check_init_() {
	if (cms_solver_) return;  // already initialized
	cms_solver_.reset(new SatChecker());

	// Build a vector<vector<int>> of clauses in DIMACS lit convention:
	//   positive int = positive lit, negative = negated lit, vars 1..N.
	// LiteralID::toInt() already produces that encoding (sign() = positive,
	// returns v; otherwise returns -v).
	std::vector<std::vector<int>> clauses;
	clauses.reserve(num_variables() * 4);

	// 1) Binary clauses via binary_links_. Each binary appears twice
	// (once per endpoint); emit only when partner var > current var.
	unsigned n_bin = 0;
	for (auto l = LiteralID(1, false); l != literals_.end_lit(); l.inc()) {
		const auto &blinks = literals_[l].binary_links_;
		for (auto bt = blinks.begin(); *bt != SENTINEL_LIT; ++bt) {
			if (bt->var() <= l.var()) continue;
			clauses.push_back({ l.toInt(), bt->toInt() });
			n_bin++;
		}
	}

	// 2) Long clauses from literal_pool_ (original clauses only for
	// Phase 1; learned clauses excluded — see comment block above).
	unsigned n_long = 0;
	ClauseOfs cl_ofs = (ClauseOfs)(1 + ClauseHeader::overheadInLits());
	while (cl_ofs < (ClauseOfs)literal_pool_.size()) {
		if (cl_ofs >= (ClauseOfs)original_lit_pool_size_) break;
		if (!isClauseRemoved(cl_ofs)) {
			std::vector<int> lits;
			for (auto lt = literal_pool_.begin() + cl_ofs;
			     *lt != SENTINEL_LIT; ++lt) {
				lits.push_back(lt->toInt());
			}
			clauses.push_back(std::move(lits));
			n_long++;
		}
		auto it = literal_pool_.begin() + cl_ofs;
		while (*it != SENTINEL_LIT) ++it;
		cl_ofs = (ClauseOfs)((it - literal_pool_.begin()) + 1
		                      + ClauseHeader::overheadInLits());
	}

	// 3) Unit clauses.
	for (LiteralID l : unit_clauses_) {
		clauses.push_back({ l.toInt() });
	}

	cms_solver_->init(num_variables(), clauses);

	std::cerr << "SAT_CHECK_INIT bin=" << n_bin
	          << " long=" << n_long
	          << " units=" << unit_clauses_.size()
	          << " vars=" << num_variables() << std::endl;
}

void Solver::sat_check_diagnose_() {
	if (!cms_solver_) return;

	std::vector<int> assumptions;
	assumptions.reserve(literal_stack_.size());
	for (LiteralID l : literal_stack_) {
		assumptions.push_back(l.toInt());
	}

	auto t0 = std::chrono::steady_clock::now();
	SatChecker::Result result =
	    cms_solver_->check(assumptions, config_.sat_check_max_confl);
	auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
	    std::chrono::steady_clock::now() - t0).count();

	sat_check_n_calls_++;
	sat_check_total_us_ += (double)dt;

	const char *res_str;
	if      (result == SatChecker::UNSAT) { res_str = "UNSAT"; sat_check_n_unsat_++; }
	else if (result == SatChecker::SAT)   { res_str = "SAT";   sat_check_n_sat_++;   }
	else                                   { res_str = "UNDEF"; sat_check_n_undef_++; }

	std::cerr << "SAT_CHECK dl=" << stack_.get_decision_level()
	          << " assumptions=" << assumptions.size()
	          << " result=" << res_str
	          << " elapsed_us=" << dt << std::endl;
}

// ============================================================================
// Derivative-cache probe (Phase 2: content-aware XOR pre-filter). See
// project_derivative_cache_idea memory for the full design.
//
// Phase 2 vs Phase 1: the XOR fingerprint now combines a per-original-clause
// CONTENT hash (XOR of lit_hashes_[l] for every active literal l in the
// clause) instead of a per-clause IDENTITY hash. Maintained incrementally
// by Instance::deriv_cache_track_lit_{assign,unassign}_ at every
// setLiteralIfFree / unSet. Two components with identical clause IDs but
// different sub-assignments now have DIFFERENT fingerprints, dramatically
// reducing the false-positive rate Phase 1 saw on dense instances like
// t1_059.
//
// Learned clauses stay on identity hashing via long_clause_hashes_ —
// they're not in occurrence_lists_ so the hook can't update them
// incrementally. This is a recall limitation, not a correctness issue
// (false negative: a true cache hit on learned-clause content may be
// missed). Most components are dominated by original clauses, so the
// learned-clause fallback is acceptable for Phase 2.
//
// SOUNDNESS: Phase 2 still does NOT modify search behavior. The probe
// runs hypothetical assignments via setLiteralIfFree + BCP + canonical_key
// + unSet, fully restoring state. The incremental hooks fire on every
// setLiteralIfFree / unSet (always — that's how the invariant is
// preserved), but they touch only the deriv_cache_* arrays which never
// feed back into the search.
// ============================================================================

void Solver::deriv_cache_init_() {
	long_clause_hashes_.clear();
	// Allocate the Bloom filter on demand (1 GB; allocating eagerly at
	// solver construction would penalize runs where bias is off).
	deriv_cache_xors_seen_.reset(new DerivCacheBloom());
	deriv_cache_counter_     = 0;
	deriv_cache_n_probes_    = 0;
	deriv_cache_n_xor_hits_  = 0;
	deriv_cache_n_real_hits_ = 0;
	// Build the content-aware tracking arrays from the current
	// post-preprocess literal_values_, then enable the incremental
	// hooks. After this returns, every setLiteralIfFree / unSet
	// keeps deriv_cache_clause_content_hash_ and
	// deriv_cache_clause_sat_count_ in sync.
	deriv_cache_track_init_();
}

// Lazy clause-hash assignment for LEARNED clauses (cl_ofs >=
// original_lit_pool_size_). Learned clauses are not in occurrence_lists_,
// so the content-aware incremental hooks can't update them; they fall
// back to identity hashing. Deterministic seed so multi-run comparisons
// are stable.
namespace {
std::mt19937_64 g_deriv_cache_learn_rng(0xbf58476d1ce4e5b9ULL);
}

static inline uint64_t get_or_make_hash(
    std::unordered_map<ClauseOfs, uint64_t> &hashes, ClauseOfs ofs) {
	auto it = hashes.find(ofs);
	if (it != hashes.end()) return it->second;
	uint64_t h = g_deriv_cache_learn_rng();
	hashes[ofs] = h;
	return h;
}

// XOR fingerprint of a sub-component's in-scope long clauses.
//   - Original (ofs < original_lit_pool_size_): use the content hash
//     maintained by Instance hooks. clause_sat_count_>0 ⇔ satisfied.
//   - Learned: identity hash via long_clause_hashes_ + isSatisfied
//     walk (the content hooks don't touch learned clauses).
// In-scope filter: removed clauses skipped via isClauseRemoved;
// learned clauses additionally require learnedClauseInScope.
uint64_t Solver::deriv_cache_component_xor_(Component &comp) {
	uint64_t xor_val = 0;
	for (auto cl_it = comp.clsBegin(); *cl_it != clsSENTINEL; ++cl_it) {
		ClauseOfs ofs = comp_manager_.clauseOfsOf(*cl_it);
		if (isClauseRemoved(ofs)) continue;
		if (ofs < (ClauseOfs)original_lit_pool_size_) {
			// Original clause: content-aware path.
			if (deriv_cache_clause_sat_count_[ofs] > 0) continue;
			xor_val ^= deriv_cache_clause_content_hash_[ofs];
		} else {
			// Learned clause: identity-hash fallback.
			if (!learnedClauseInScopeOrSound(ofs, config_.sound_provenance)) continue;
			if (isSatisfied(ofs)) continue;
			xor_val ^= get_or_make_hash(long_clause_hashes_, ofs);
		}
	}
	// Original BINARY clauses. Treated uniformly with long clauses:
	// per-binary clause_random baseline + lit_hash contributions,
	// maintained incrementally by the assign/unassign hooks.
	// Dedup by lit.raw() < partner.raw(); partner being X_TRI is
	// equivalent to "binary in-scope, unsatisfied" since both endpoints
	// X_TRI ⇒ sat_count[id] = 0. The component analyzer's connectivity
	// invariant guarantees both endpoints share the component.
	for (auto var_it = comp.varsBegin(); *var_it != varsSENTINEL; ++var_it) {
		const unsigned v = *var_it;
		if (literal_values_[LiteralID(v, true)] != X_TRI) continue;
		for (int pol = 0; pol < 2; ++pol) {
			LiteralID lit(v, pol == 0);
			const Literal &li = literals_[lit];
			const unsigned norig = li.original_binary_link_count_;
			for (unsigned i = 0; i < norig; ++i) {
				LiteralID partner = li.binary_links_[i];
				if (lit.raw() >= partner.raw()) continue;
				if (literal_values_[partner] != X_TRI) continue;
				xor_val ^= deriv_cache_binary_clause_content_hash_[
				             li.binary_link_ids_[i]];
			}
		}
	}
	return xor_val;
}

void Solver::deriv_cache_record_store_(Component &comp,
                                        const CanonicalKey *key_for_debug) {
	// Use the incrementally-maintained current_component_xor_. The
	// CompXorGuard in solveComponent ensures this matches comp's XOR
	// when this function is called from the store path (right after
	// solveComponentImpl returns; BCP fully unwound).
	const uint64_t xor_val = current_component_xor_;
	deriv_cache_xors_seen_->insert(xor_val);
	if (config_.deriv_cache_dump_fp > 0 && key_for_debug != nullptr) {
		// Build a structure string for THIS comp's in-scope original clauses.
		std::string structure;
		structure.reserve(256);
		{
			char buf[64];
			structure += "[";
			bool first_clause = true;
			for (auto cl_it = comp.clsBegin(); *cl_it != clsSENTINEL; ++cl_it) {
				ClauseOfs ofs = comp_manager_.clauseOfsOf(*cl_it);
				if (ofs >= (ClauseOfs)original_lit_pool_size_) continue;
				if (isClauseRemoved(ofs)) continue;
				if (deriv_cache_clause_sat_count_[ofs] > 0) continue;
				if (!first_clause) structure += ",";
				first_clause = false;
				snprintf(buf, sizeof(buf), "%u:(", ofs);
				structure += buf;
				bool first_lit = true;
				for (auto lt = literal_pool_.begin() + ofs;
				     *lt != SENTINEL_LIT; ++lt) {
					if (!isActive(*lt)) continue;
					if (!first_lit) structure += " ";
					first_lit = false;
					snprintf(buf, sizeof(buf), "%d", lt->toInt());
					structure += buf;
				}
				snprintf(buf, sizeof(buf), "):0x%lx", (unsigned long)deriv_cache_clause_content_hash_[ofs]);
				structure += buf;
			}
			structure += "]";
		}

		auto it = deriv_cache_debug_xor_to_key_.find(xor_val);
		if (it == deriv_cache_debug_xor_to_key_.end()) {
			deriv_cache_debug_xor_to_key_[xor_val] = DebugKeyTuple{
			    key_for_debug->hash, key_for_debug->hash_hi,
			    key_for_debug->num_vars, key_for_debug->n_in_clauses,
			    key_for_debug->num_clauses};
			if (deriv_cache_debug_xor_to_struct_.size() < 200000) {
				deriv_cache_debug_xor_to_struct_[xor_val] = structure;
			}
		} else if (it->second.hash != key_for_debug->hash
		           || it->second.hash_hi != key_for_debug->hash_hi) {
			static unsigned long long store_collisions = 0;
			store_collisions++;
			if (store_collisions <= config_.deriv_cache_dump_fp) {
				auto sit = deriv_cache_debug_xor_to_struct_.find(xor_val);
				const std::string &prior_struct =
				    (sit != deriv_cache_debug_xor_to_struct_.end()) ? sit->second
				                                                    : std::string("<not_captured>");
				std::cerr << "DERIV_CACHE_STORE_COLLISION #" << store_collisions
				          << " xor=0x" << std::hex << xor_val << std::dec
				          << " prior=(h=" << it->second.hash
				          << ",hi=" << it->second.hash_hi
				          << ",nvars=" << it->second.num_vars
				          << ",n_in_cl=" << it->second.n_in_clauses
				          << ",ncl=" << it->second.num_clauses << ")"
				          << " curr=(h=" << key_for_debug->hash
				          << ",hi=" << key_for_debug->hash_hi
				          << ",nvars=" << key_for_debug->num_vars
				          << ",n_in_cl=" << key_for_debug->n_in_clauses
				          << ",ncl=" << key_for_debug->num_clauses << ")"
				          << "\n  prior_in_scope=" << prior_struct
				          << "\n  curr_in_scope=" << structure
				          << std::endl;
			}
		}
	}
}

void Solver::deriv_cache_probe_(Component &comp) {
	deriv_cache_n_probes_++;

	// Snapshot the current component xor once. Reused by the clause-removal
	// probes below: F\{c}'s xor equals current_xor XOR-out the clause's
	// content hash (which is its sole contribution to the xor).
	// Uses the incrementally-maintained value — no walk needed.
	const uint64_t current_xor = current_component_xor_;

	// ============================================================
	// VARIABLE-BRANCHING probes: for each of the top-K active vars,
	// hypothetically assert F|v=T and F|v=F via setLiteralIfFree+BCP
	// and check the resulting xor.
	// ============================================================
	// Pick top-K active vars from `comp`. Simplest stable selection:
	// the first K active vars in component-iteration order. This is NOT
	// the picker's score order; it's deterministic and cheap. Phase 3
	// can integrate with the picker's actual top-K once we have hit-rate
	// data.
	std::vector<unsigned> candidates;
	candidates.reserve(config_.deriv_cache_top_k);
	for (auto v_it = comp.varsBegin();
	     *v_it != varsSENTINEL && candidates.size() < config_.deriv_cache_top_k;
	     ++v_it) {
		if (isActive(LiteralID(*v_it, true))) candidates.push_back(*v_it);
	}

	for (unsigned v : candidates) {
		// Skip if it was forced since we collected candidates.
		if (!isActive(LiteralID(v, true))) continue;

		for (int pol_int = 0; pol_int < 2; ++pol_int) {
			LiteralID lit(v, pol_int == 1);

			// Hypothetical assignment + BCP cascade (mirrors probeLiteralPassFail).
			unsigned sz = literal_stack_.size();
			if (!setLiteralIfFree(lit)) continue;
			bool ok = BCP(sz);

			if (ok) {
				// Post-BCP component_xor — incremental hooks already
				// updated current_component_xor_ via setLit + BCP.
				uint64_t deriv_xor = current_component_xor_;

				if (deriv_cache_xors_seen_->may_contain(deriv_xor)) {
					deriv_cache_n_xor_hits_++;

					// Pre-filter says maybe — do the full canonical-key check.
					CanonicalKey k = buildCanonicalKey(
					    comp, literal_pool_, literals_, literal_values_,
					    comp_manager_.getAnalyzer().clauseIdToOfs(),
					    removed_clauses_, original_lit_pool_size_,
					    config_.wl_iterations,
					    static_wl_labels_.empty() ? nullptr : &static_wl_labels_);
					mpz_class out;
					if (comp_manager_.contentCache().peek(k, out)) {
						deriv_cache_n_real_hits_++;
					}
				}
			}

			// Restore — unSet the BCP cascade.
			while (literal_stack_.size() > sz) {
				unSet(literal_stack_.back());
				literal_stack_.pop_back();
			}
		}
	}

	// ============================================================
	// CLAUSE-BRANCHING (removal-only) probes: for each of the top-K
	// active ORIGINAL clauses in `comp`, the F\{c} derivative xor is
	//   current_xor ^ deriv_cache_clause_content_hash_[c]
	// because the clause's only contribution to the component xor is
	// its content hash. No BCP, no temp state mutation needed for the
	// pre-filter step. On a positive pre-filter, mark the clause
	// removed (refcounted, reversible), compute the full canonical
	// key, peek the cache, then unmark.
	//
	// Restrictions:
	//   - Original clauses only (cl_ofs < original_lit_pool_size_).
	//     Learned clauses fall back to per-identity hashing, not
	//     content-aware — and removing a learned clause shouldn't
	//     change #SAT anyway (they're sound consequences of originals).
	//   - Already-removed clauses skipped (`isClauseRemoved`).
	//   - Satisfied clauses skipped (`sat_count_ > 0`): their xor
	//     contribution is already 0, so the F\{c} xor equals current_xor
	//     which is by construction not in xors_seen (this comp hasn't
	//     been stored yet — we're inside a cache miss).
	// ============================================================
	std::vector<ClauseOfs> cl_candidates;
	cl_candidates.reserve(config_.deriv_cache_top_k);
	for (auto cl_it = comp.clsBegin();
	     *cl_it != clsSENTINEL && cl_candidates.size() < config_.deriv_cache_top_k;
	     ++cl_it) {
		ClauseOfs ofs = comp_manager_.clauseOfsOf(*cl_it);
		if (ofs >= (ClauseOfs)original_lit_pool_size_) continue;  // learned
		if (isClauseRemoved(ofs)) continue;
		if (deriv_cache_clause_sat_count_[ofs] > 0) continue;     // satisfied
		cl_candidates.push_back(ofs);
	}

	for (ClauseOfs ofs : cl_candidates) {
		const uint64_t hyp_xor =
		    current_xor ^ deriv_cache_clause_content_hash_[ofs];
		if (deriv_cache_xors_seen_->may_contain(hyp_xor)) {
			deriv_cache_n_cl_xor_hits_++;

			// Temporary mark — keeps the same refcount semantics that
			// branchOnClause uses, so the canonical key path sees the
			// formula exactly as it would during a real F\{c} branch.
			markClauseRemoved(ofs);
			CanonicalKey k = buildCanonicalKey(
			    comp, literal_pool_, literals_, literal_values_,
			    comp_manager_.getAnalyzer().clauseIdToOfs(),
			    removed_clauses_, original_lit_pool_size_,
			    config_.wl_iterations,
			    static_wl_labels_.empty() ? nullptr : &static_wl_labels_);
			mpz_class out;
			if (comp_manager_.contentCache().peek(k, out)) {
				deriv_cache_n_cl_real_hits_++;
			}
			unmarkClauseRemoved(ofs);
		}
	}

	// Periodic summary log — every 1000 probes. Reports the var and
	// clause arms separately so we can see whether clause-removal
	// probes pull their weight.
	if (deriv_cache_n_probes_ % 1000 == 0) {
		std::cerr << "DERIV_CACHE_PROBE probes=" << deriv_cache_n_probes_
		          << " var_xor=" << deriv_cache_n_xor_hits_
		          << " var_real=" << deriv_cache_n_real_hits_
		          << " cl_xor=" << deriv_cache_n_cl_xor_hits_
		          << " cl_real=" << deriv_cache_n_cl_real_hits_
		          << " cache_size=" << deriv_cache_xors_seen_->inserts()
		          << std::endl;
	}
}

// ============================================================================
// Phase 3: cache-biased branch selection. Probes top-K var candidates and
// active original clauses; returns a classified hit the caller can act on.
//
// Priority (matches project_derivative_cache_idea):
//   1. VAR_BOTH  — both arms of some var are cached, the entire component
//                  resolves to (cached_T + cached_F) with no recursion.
//   2. CLAUSE_IE — F\{c} is cached for some clause c; IE gives us
//                  cached − #SAT(F\{c} ∧ ¬c), the recurse arm forces ≥2 lits.
//   3. VAR_ONE   — one arm of some var is cached; branch on v with the
//                  cached arm free, recurse on the other (1 forced lit).
//   4. NONE      — no hit; caller falls through to normal picker.
//
// Among multiple CLAUSE_IE hits, prefer the clause with the largest active
// literal count (biggest BCP juice on the ¬c recurse arm).
// ============================================================================

Solver::DerivCacheBranchChoice Solver::deriv_cache_bias_select_(
    Component &comp,
    const std::vector<CutNode> &separator,
    double abstract_budget) {
	// RAII timer accumulator for total time in this function across all
	// return paths. Caller-side: deriv_cache_bias_total_us_.
	struct TotalGuard {
		double &accum;
		std::chrono::steady_clock::time_point t0;
		TotalGuard(double &a)
		    : accum(a), t0(std::chrono::steady_clock::now()) {}
		~TotalGuard() {
			auto t1 = std::chrono::steady_clock::now();
			accum += std::chrono::duration<double, std::micro>(t1 - t0).count();
		}
	} _tt(deriv_cache_bias_total_us_);

	DerivCacheBranchChoice choice;  // kind = NONE by default

	// Cache-warmup gate. Early in the search the cache is empty;
	// probing yields no hits and is pure overhead. Bias the technique
	// at the search PHASE where neighbor lookups become productive.
	if (deriv_cache_xors_seen_->inserts() < config_.deriv_cache_bias_min_cache)
		return choice;

	// Connectivity gate. `comp` at solveComponentImpl entry is whatever
	// the analyzer identified at the PARENT level; BCP since then may
	// have severed it into multiple sub-components. Bias on a joint
	// state is unsound work — the per-sub-component cache entries
	// don't match a joint xor.
	//
	// Union-find on active vars directly indexed by var ID (using the
	// member scratch deriv_cache_bias_dsu_parent_, sized once at solve
	// start). For each active var v we set parent[v] = v before any
	// find() touches it, so stale entries from earlier calls are
	// invisible. Skip bias if > 1 root is found.
	{
		auto &par = deriv_cache_bias_dsu_parent_;
		if (par.size() < variables_.size() + 1)
			par.assign(variables_.size() + 2, 0);
		// Initialize parent[v]=v for each active var in comp. We also
		// pick `start_v` as the first active var — find() chains will
		// only originate from active vars we've initialized this call,
		// so stale entries can't reach untouched memory.
		unsigned start_v = 0;
		unsigned n_active = 0;
		for (auto v_it = comp.varsBegin(); *v_it != varsSENTINEL; ++v_it) {
			unsigned v = *v_it;
			if (!isActive(LiteralID(v, true))) continue;
			par[v] = v;
			n_active++;
			if (start_v == 0) start_v = v;
		}
		if (n_active <= 1) return choice;
		auto find = [&](unsigned x) -> unsigned {
			while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; }
			return x;
		};
		auto unite = [&](unsigned x, unsigned y) {
			unsigned rx = find(x), ry = find(y);
			if (rx != ry) par[rx] = ry;
		};
		// Long clauses (only the active ones in comp).
		for (auto cl_it = comp.clsBegin(); *cl_it != clsSENTINEL; ++cl_it) {
			ClauseOfs ofs = comp_manager_.clauseOfsOf(*cl_it);
			if (ofs >= (ClauseOfs)original_lit_pool_size_) continue;
			if (isClauseRemoved(ofs))                       continue;
			if (deriv_cache_clause_sat_count_[ofs] > 0)     continue;
			unsigned first = 0;
			for (auto lt = literal_pool_.begin() + ofs; *lt != SENTINEL_LIT; ++lt) {
				if (!isActive(*lt)) continue;
				unsigned v = lt->var();
				if (par[v] != v && find(v) != v && par[v] == 0) continue;  // not initialised
				if (first == 0) first = v;
				else            unite(first, v);
			}
		}
		// Binary clauses: walk binary_links_ for each active var.
		for (auto v_it = comp.varsBegin(); *v_it != varsSENTINEL; ++v_it) {
			unsigned v = *v_it;
			if (!isActive(LiteralID(v, true))) continue;
			for (int pol = 0; pol < 2; ++pol) {
				LiteralID lit(v, pol == 1);
				for (auto other : literal(lit).binary_links_) {
					if (other == SENTINEL_LIT) continue;
					if (literal_values_[other.neg()] == T_TRI) continue;
					unsigned ov = other.var();
					if (!isActive(LiteralID(ov, true))) continue;
					unite(v, ov);
				}
			}
		}
		// Count: are all initialised vars in the same root?
		unsigned root0 = find(start_v);
		for (auto v_it = comp.varsBegin(); *v_it != varsSENTINEL; ++v_it) {
			unsigned v = *v_it;
			if (!isActive(LiteralID(v, true))) continue;
			if (find(v) != root0) return choice;  // multi-component
		}
	}

	deriv_cache_n_probes_++;

	// Read the incrementally-maintained component XOR. With the
	// CompXorGuard + assign/unassign hook diffs in place, this is O(1)
	// (was previously a full-comp walk per probe call; landed
	// 2026-05-23 along with the dirty-clause incremental scheme).
	const uint64_t current_xor = current_component_xor_;
	(void)deriv_cache_bias_xorinit_us_;  // retained for counter compat; no-op

	// ============================================================
	// VAR probes: top-K active vars by picker score (scoreOf), plus
	// any VAR elements of the current separator. Skipped entirely
	// when -derivCacheBiasVar 0 (clause-only mode).
	// ============================================================
	struct VarHit {
		VariableIndex var = 0;
		unsigned mask = 0;          // bit 0 = T cached, bit 1 = F cached
		mpz_class scaled_t;
		mpz_class scaled_f;
	};
	std::vector<VarHit> var_hits;
	std::vector<unsigned> var_candidates;
	if (config_.deriv_cache_bias_var) {
		// Score-rank all active vars in comp, take top-K.
		std::vector<std::pair<float, unsigned>> scored;
		scored.reserve(64);
		for (auto v_it = comp.varsBegin(); *v_it != varsSENTINEL; ++v_it) {
			if (!isActive(LiteralID(*v_it, true))) continue;
			scored.emplace_back(scoreOf(*v_it), *v_it);
		}
		const unsigned k_target = config_.deriv_cache_top_k;
		if (scored.size() > k_target) {
			std::partial_sort(scored.begin(), scored.begin() + k_target,
			                  scored.end(),
			                  [](const std::pair<float, unsigned> &a,
			                     const std::pair<float, unsigned> &b) {
				                  return a.first > b.first;
			                  });
			scored.resize(k_target);
		}
		var_candidates.reserve(k_target + separator.size());
		for (const auto &p : scored) var_candidates.push_back(p.second);
		// Union with separator VAR elements (dedup).
		for (const auto &nd : separator) {
			if (nd.kind != CutNode::VAR) continue;
			unsigned sv = nd.id;
			if (!isActive(LiteralID(sv, true))) continue;
			bool already = false;
			for (unsigned u : var_candidates) {
				if (u == sv) { already = true; break; }
			}
			if (!already) var_candidates.push_back(sv);
		}
	}

	for (unsigned v : var_candidates) {
		if (!isActive(LiteralID(v, true))) continue;
		VarHit vh; vh.var = v;
		for (int pol_int = 0; pol_int < 2; ++pol_int) {
			LiteralID lit(v, pol_int == 1);
			unsigned sz = literal_stack_.size();
			if (!setLiteralIfFree(lit)) continue;
			bool ok = BCP(sz);
			if (ok) {
				// Post-BCP comp xor — hooks already updated current_component_xor_.
				uint64_t deriv_xor = current_component_xor_;
				if (deriv_cache_xors_seen_->may_contain(deriv_xor)) {
					deriv_cache_n_xor_hits_++;
					const auto ck_t0 = std::chrono::steady_clock::now();
					CanonicalKey k = buildCanonicalKey(
					    comp, literal_pool_, literals_, literal_values_,
					    comp_manager_.getAnalyzer().clauseIdToOfs(),
					    removed_clauses_, original_lit_pool_size_,
					    config_.wl_iterations,
					    static_wl_labels_.empty() ? nullptr : &static_wl_labels_);
					deriv_cache_bias_canonical_us_ +=
					    std::chrono::duration<double, std::micro>(
					        std::chrono::steady_clock::now() - ck_t0).count();
					mpz_class out;
					if (comp_manager_.contentCache().peek(k, out)) {
						deriv_cache_n_real_hits_++;
						// Scale by 2^free_vars (same convention as the
						// L2 cache wrapper in solveComponent).
						unsigned hyp_free_vars = (k.num_vars > k.n_in_clauses)
						    ? (k.num_vars - k.n_in_clauses) : 0;
						mpz_class scaled = out;
						for (unsigned i = 0; i < hyp_free_vars; ++i) scaled *= 2;

						// Brute-force verify the var shortcut. Current
						// state (post-setLit + BCP) represents F|v=POL.
						// SHARPSAT_BIAS_BRUTE=N gates this.
						{
							static int s_brute_n = []() {
								const char *e = std::getenv("SHARPSAT_BIAS_BRUTE");
								return e ? std::atoi(e) : 0;
							}();
							static long long s_dumped_var = 0;
							if (s_brute_n > 0 && s_dumped_var < 50) {
								unsigned n_active = 0;
								mpz_class brute = bruteForceCountSubcomp(
								    comp, (unsigned)s_brute_n, &n_active);
								if (brute >= 0 && scaled != brute) {
									std::cerr << "*** PHASE3_VAR_UNSOUND ***"
									          << " var=" << v
									          << " pol=" << pol_int
									          << " n_active=" << n_active
									          << " cached=" << scaled
									          << " brute="  << brute
									          << " diff="   << (scaled - brute)
									          << " key.num_vars=" << k.num_vars
									          << " key.n_in_cl=" << k.n_in_clauses
									          << " hyp_fv=" << hyp_free_vars
									          << "\n";
									s_dumped_var++;
								}
							}
						}

						if (pol_int == 1) {
							vh.mask |= 1; vh.scaled_t = std::move(scaled);
						} else {
							vh.mask |= 2; vh.scaled_f = std::move(scaled);
						}
					}
				}
			}
			// Rollback BCP cascade for the next probe / fall-through.
			while (literal_stack_.size() > sz) {
				unSet(literal_stack_.back());
				literal_stack_.pop_back();
			}
		}
		if (vh.mask != 0) var_hits.push_back(std::move(vh));
	}

	// Priority 1: VAR_BOTH — return first one found.
	for (auto &vh : var_hits) {
		if (vh.mask == 3) {
			choice.kind = DerivCacheBranchChoice::VAR_BOTH;
			choice.var  = vh.var;
			choice.arm0_count = std::move(vh.scaled_t);
			choice.arm1_count = std::move(vh.scaled_f);
			deriv_cache_n_used_var_both_++;
			return choice;
		}
	}

	// ============================================================
	// CLAUSE probes: collect candidates (active originals), sort by
	// static length descending so the LONGEST clauses are tried first
	// (longer ⇒ more lits forced on ¬c arm ⇒ bigger BCP cascade), and
	// BREAK on the first canonical-key-confirmed hit. Avoids paying
	// the canonical-key cost N times to find a "best" candidate; we
	// trust the length-descending order to surface the best one first.
	// ============================================================
	struct ClauseCand {
		ClauseOfs ofs;
		unsigned  length;       // static, from ClauseHeader
	};
	std::vector<ClauseCand> cl_candidates;
	cl_candidates.reserve(32);
	for (auto cl_it = comp.clsBegin(); *cl_it != clsSENTINEL; ++cl_it) {
		ClauseOfs ofs = comp_manager_.clauseOfsOf(*cl_it);
		if (ofs >= (ClauseOfs)original_lit_pool_size_) continue;
		if (isClauseRemoved(ofs)) continue;
		if (deriv_cache_clause_sat_count_[ofs] > 0) continue;
		cl_candidates.push_back({ofs, getHeaderOf(ofs).length()});
	}
	std::sort(cl_candidates.begin(), cl_candidates.end(),
	          [](const ClauseCand &a, const ClauseCand &b) {
		          return a.length > b.length;
	          });

	ClauseOfs hit_cl_ofs = 0;
	mpz_class hit_cl_scaled;
	for (const auto &cand : cl_candidates) {
		const ClauseOfs ofs = cand.ofs;
		const uint64_t hyp_xor =
		    current_xor ^ deriv_cache_clause_content_hash_[ofs];
		if (!deriv_cache_xors_seen_->may_contain(hyp_xor)) continue;
		deriv_cache_n_cl_xor_hits_++;

		markClauseRemoved(ofs);
		const auto ck_t0 = std::chrono::steady_clock::now();
		CanonicalKey k = buildCanonicalKey(
		    comp, literal_pool_, literals_, literal_values_,
		    comp_manager_.getAnalyzer().clauseIdToOfs(),
		    removed_clauses_, original_lit_pool_size_,
		    config_.wl_iterations,
		    static_wl_labels_.empty() ? nullptr : &static_wl_labels_);
		deriv_cache_bias_canonical_us_ +=
		    std::chrono::duration<double, std::micro>(
		        std::chrono::steady_clock::now() - ck_t0).count();
		mpz_class out;
		bool peek_hit = comp_manager_.contentCache().peek(k, out);

		// Brute-force verification of the CLAUSE_IE shortcut.
		// Env-gated: SHARPSAT_BIAS_BRUTE=N enables, N=max active vars.
		// While clause `ofs` is STILL marked removed, brute-force-count
		// the current sub-component (which is F\{ofs}) and compare to
		// the scaled cached value we're about to use. Any mismatch
		// indicates the shortcut is unsound for THIS context.
		if (peek_hit) {
			static int s_brute_n = []() {
				const char *e = std::getenv("SHARPSAT_BIAS_BRUTE");
				return e ? std::atoi(e) : 0;
			}();
			static long long s_dumped = 0;
			if (s_brute_n > 0 && s_dumped < 50) {
				unsigned n_active = 0;
				mpz_class brute = bruteForceCountSubcomp(
				    comp, (unsigned)s_brute_n, &n_active);
				if (brute >= 0) {
					unsigned hyp_fv = (k.num_vars > k.n_in_clauses)
					    ? (k.num_vars - k.n_in_clauses) : 0;
					mpz_class cached_scaled = out;
					for (unsigned i = 0; i < hyp_fv; ++i)
						cached_scaled *= 2;
					if (cached_scaled != brute) {
						std::cerr << "*** PHASE3_CLAUSE_IE_UNSOUND ***"
						          << " cl_ofs=" << ofs
						          << " n_active=" << n_active
						          << " cached=" << cached_scaled
						          << " brute="  << brute
						          << " diff="   << (cached_scaled - brute)
						          << " key.num_vars=" << k.num_vars
						          << " key.n_in_cl=" << k.n_in_clauses
						          << " hyp_fv=" << hyp_fv
						          << "\n";
						s_dumped++;
					}
				}
			}
		}

		unmarkClauseRemoved(ofs);

		if (!peek_hit) {
			// FP: hyp_xor matched in Bloom but canonical_key not in cache.
			// Dump a few examples for diagnosis.
			if (config_.deriv_cache_dump_fp > 0
			    && deriv_cache_fp_dumped_ < config_.deriv_cache_dump_fp) {
				auto it = deriv_cache_debug_xor_to_key_.find(hyp_xor);
				if (it != deriv_cache_debug_xor_to_key_.end()) {
					std::cerr << "DERIV_CACHE_FP "
					          << "hyp_xor=0x" << std::hex << hyp_xor << std::dec
					          << " query_key=(" << k.hash << "," << k.hash_hi << ")"
					          << " stored_key=(" << it->second.hash
					          << "," << it->second.hash_hi << ")"
					          << " query_num_vars=" << k.num_vars
					          << " query_n_in_clauses=" << k.n_in_clauses
					          << " stored_num_vars=" << it->second.num_vars
					          << " stored_n_in_clauses=" << it->second.n_in_clauses
					          << " cl_ofs=" << ofs
					          << std::endl;
					deriv_cache_fp_dumped_++;
				}
			}
			continue;
		}
		deriv_cache_n_cl_real_hits_++;

		hit_cl_ofs = ofs;
		unsigned hyp_free_vars = (k.num_vars > k.n_in_clauses)
		    ? (k.num_vars - k.n_in_clauses) : 0;
		hit_cl_scaled = out;
		for (unsigned i = 0; i < hyp_free_vars; ++i) hit_cl_scaled *= 2;
		break;  // First confirmed hit wins (length-descending order).
	}

	// Priority 2: CLAUSE_IE — first (longest) confirmed clause hit.
	if (hit_cl_ofs != 0) {
		choice.kind = DerivCacheBranchChoice::CLAUSE_IE;
		choice.cl_ofs = hit_cl_ofs;
		choice.arm0_count = std::move(hit_cl_scaled);
		deriv_cache_n_used_cl_ie_++;
		// Bucket the hit by abstract_budget so we can see whether
		// shortcuts happen near the root (big sub-tree skipped) or
		// deep (tiny sub-tree skipped).
		unsigned bucket;
		if      (abstract_budget < 10)  bucket = 0;
		else if (abstract_budget < 30)  bucket = 1;
		else if (abstract_budget < 60)  bucket = 2;
		else if (abstract_budget < 80)  bucket = 3;
		else                            bucket = 4;
		deriv_cache_used_cl_ie_budget_hist_[bucket]++;
		return choice;
	}

	// Priority 3: VAR_ONE — any var with one arm cached. Pick the
	// first (= highest priority in candidate iteration order).
	for (auto &vh : var_hits) {
		if (vh.mask == 1 || vh.mask == 2) {
			choice.kind = DerivCacheBranchChoice::VAR_ONE;
			choice.var  = vh.var;
			choice.cached_polarity = (vh.mask == 1);
			choice.arm0_count = (vh.mask == 1)
			    ? std::move(vh.scaled_t)
			    : std::move(vh.scaled_f);
			deriv_cache_n_used_var_one_++;
			return choice;
		}
	}

	// Priority 4: NONE. Caller falls through.
	// Periodic summary, throttled to once per 60s on long runs.
	const double now_s = stopwatch_.getElapsedSeconds();
	if (deriv_cache_last_log_s_ < 0.0 || now_s - deriv_cache_last_log_s_ >= 60.0) {
		deriv_cache_last_log_s_ = now_s;
		std::cerr << "DERIV_CACHE_BIAS t=" << now_s
		          << " probes=" << deriv_cache_n_probes_
		          << " cl_xor="        << deriv_cache_n_cl_xor_hits_
		          << " cl_real="       << deriv_cache_n_cl_real_hits_
		          << " used_cl_ie="    << deriv_cache_n_used_cl_ie_
		          << " cl_ie_bkt=["
		          << deriv_cache_used_cl_ie_budget_hist_[0] << ","
		          << deriv_cache_used_cl_ie_budget_hist_[1] << ","
		          << deriv_cache_used_cl_ie_budget_hist_[2] << ","
		          << deriv_cache_used_cl_ie_budget_hist_[3] << ","
		          << deriv_cache_used_cl_ie_budget_hist_[4] << "]"
		          << " var_xor="       << deriv_cache_n_xor_hits_
		          << " var_real="      << deriv_cache_n_real_hits_
		          << " used_var_both=" << deriv_cache_n_used_var_both_
		          << " used_var_one="  << deriv_cache_n_used_var_one_
		          << " cache_size="    << deriv_cache_xors_seen_->inserts()
		          << " bias_us="       << (uint64_t)deriv_cache_bias_total_us_
		          << " xorinit_us="    << (uint64_t)deriv_cache_bias_xorinit_us_
		          << " canonical_us="  << (uint64_t)deriv_cache_bias_canonical_us_
		          << std::endl;
	}
	return choice;
}

// ---------------------------------------------------------------------
// Compute the fixed branching order based on config_.picker_order.
// Called once at solve start, after preprocessing.
//
// DEGREE: vars sorted by their incidence count in the post-PP formula
//         (active binaries + long-clause occurrences), descending. The
//         picker then iterates a component's vars and returns the one
//         with the lowest order position.
// METIS / TD: placeholders — fall back to DEGREE for now.
// ---------------------------------------------------------------------
void Solver::computeFixedBranchOrder() {
	const unsigned n = num_variables();
	std::vector<unsigned> degree(n + 1, 0);

	// Binary clause contributions: literal(l).binary_links_ holds
	// partners for each polarity. Original binaries only (post-PP).
	for (unsigned v = 1; v <= n; v++) {
		for (int pol = 0; pol < 2; pol++) {
			const LiteralID l(v, pol == 1);
			const Literal &lit_rec = literal(l);
			const unsigned orig_count = lit_rec.original_binary_link_count_;
			degree[v] += orig_count;
		}
	}

	// Long clause contributions: iterate by ClauseID via the analyzer's
	// clause_id_to_ofs[] map, which gives the BODY offset (post-header)
	// of each original long clause.
	const auto& clause_id_to_ofs = comp_manager_.getAnalyzer().clauseIdToOfs();
	for (unsigned cid = 1; cid < clause_id_to_ofs.size(); cid++) {
		const ClauseOfs ofs = clause_id_to_ofs[cid];
		if (ofs == 0 || ofs >= original_lit_pool_size_) continue;
		for (auto lt = literal_pool_.begin() + ofs;
		     *lt != SENTINEL_LIT; ++lt) {
			const unsigned v = lt->var();
			if (v >= 1 && v <= n) degree[v]++;
		}
	}

	// METIS_DEG: compute separator vars from short-clause var-only METIS,
	// then prioritize them. Other modes: just degree-based.
	std::vector<bool> in_metis_sep(n + 1, false);
	if (config_.picker_order == SolverConfiguration::METIS_DEG) {
		// Build var-only METIS input restricted to clauses of length ≤ 3.
		// Length 2 (binaries) become direct var-var edges. Length 3 become
		// triangles (3 edges per clause via clique encoding in
		// NDHierarchy::build).
		std::vector<std::pair<unsigned, std::vector<unsigned>>> short_long;
		for (unsigned cid = 1; cid < clause_id_to_ofs.size(); cid++) {
			const ClauseOfs ofs = clause_id_to_ofs[cid];
			if (ofs == 0 || ofs >= original_lit_pool_size_) continue;
			std::vector<unsigned> vars;
			for (auto lt = literal_pool_.begin() + ofs;
			     *lt != SENTINEL_LIT; ++lt) {
				vars.push_back(lt->var());
			}
			if (vars.size() == 3) short_long.push_back({ofs, vars});
		}
		// Binaries: dedup with v < partner.
		std::vector<std::pair<unsigned, unsigned>> bin_pairs;
		for (unsigned v = 1; v <= n; v++) {
			for (int pol = 0; pol < 2; pol++) {
				const LiteralID l(v, pol == 1);
				const Literal &lr = literal(l);
				const unsigned orig = lr.original_binary_link_count_;
				unsigned idx = 0;
				for (auto bt = lr.binary_links_.begin();
				     *bt != SENTINEL_LIT; ++bt, ++idx) {
					if (idx >= orig) break;
					const unsigned other = bt->var();
					if (v < other) bin_pairs.push_back({v, other});
				}
			}
		}
		// Run METIS via local NDHierarchy.
		NDHierarchy nd_local;
		nd_local.build((int)n, short_long, bin_pairs, /*vars_only=*/true);
		// Collect every variable that appears in any separator.
		for (size_t i = 0; i < nd_local.separator.size(); i++) {
			for (const auto& cn : nd_local.separator[i]) {
				if (cn.kind == CutNode::VAR && cn.id <= n) {
					in_metis_sep[cn.id] = true;
				}
			}
		}
		if (!config_.quiet) {
			unsigned n_sep = 0;
			for (unsigned v = 1; v <= n; v++) if (in_metis_sep[v]) n_sep++;
			std::cout << "c o [picker_order METIS_DEG] sep vars from short-clause "
			          << "graph: " << n_sep << " (of " << n << " total)\n";
		}
	}

	// Build the order: list all candidate vars, sort by degree desc.
	fixed_order_vars_.clear();
	fixed_order_vars_.reserve(n);
	for (unsigned v = 1; v <= n; v++) {
		// Skip vars already assigned (e.g., by units / failed-lit test)
		if (literal_values_[LiteralID(v, true)] != X_TRI) continue;
		fixed_order_vars_.push_back(v);
	}
	// Sort: under METIS_DEG in-separator vars come first; within each
	// tier, degree desc (or asc for DEGREE_ASC).
	const bool ascending = (config_.picker_order == SolverConfiguration::DEGREE_ASC);
	const bool use_sep = (config_.picker_order == SolverConfiguration::METIS_DEG);
	std::sort(fixed_order_vars_.begin(), fixed_order_vars_.end(),
	          [ascending, use_sep,
	           &degree, &in_metis_sep](unsigned a, unsigned b) {
		// Under METIS_DEG, in-separator vars come before non-separator.
		if (use_sep) {
			const bool as = in_metis_sep[a];
			const bool bs = in_metis_sep[b];
			if (as != bs) return as;
		}
		// Within group: ascending or descending by degree.
		return ascending ? (degree[a] < degree[b]) : (degree[a] > degree[b]);
	});

	// Build reverse index.
	var_to_order_.assign(n + 1, UINT_MAX);
	for (size_t i = 0; i < fixed_order_vars_.size(); i++) {
		var_to_order_[fixed_order_vars_[i]] = (unsigned)i;
	}

	if (!config_.quiet) {
		cout << "c o [picker_order] computed fixed order over "
		     << fixed_order_vars_.size() << " vars"
		     << " (mode=" << (int)config_.picker_order << ")\n";
		if (fixed_order_vars_.size() > 0) {
			cout << "c o [picker_order] top 8 by degree:";
			for (size_t i = 0; i < 8 && i < fixed_order_vars_.size(); i++) {
				unsigned v = fixed_order_vars_[i];
				cout << " v" << v << "(d=" << degree[v] << ")";
			}
			cout << "\n";
		}
	}
}
