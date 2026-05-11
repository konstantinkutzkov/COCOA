#include "solver.h"

#include <iostream>
#include <string>

using namespace std;


int main(int argc, char *argv[]) {

  string input_file;
  Solver theSolver;


  if (argc <= 1) {
    cout << "Usage: sharpSAT [options] [CNF_File]" << endl;
    cout << "Options: " << endl;
    cout << "\t -noPP  \t turn off preprocessing" << endl;
    cout << "\t -q     \t quiet mode" << endl;
    cout << "\t -t [s] \t set time bound to s seconds" << endl;
    cout << "\t -noCC  \t turn off component caching" << endl;
    cout << "\t -cs [n]\t set max cache size to n MB" << endl;
    cout << "\t -noIBCP\t turn off implicit BCP" << endl;
    cout << "\t -learnLevel n\t learning feature ladder (default 4: no minimization; 5=full w/ rewritten minimize, 3=no bin-pad, 2=no scope, 1=no dedup, 0=no learn)" << endl;
    cout << "\t -verifyLearn\t replay the resolution chain after each minimization for an end-to-end sanity check (opt-in, slow)" << endl;
    cout << "\t -noSubsumption\t disable preprocess subsumption" << endl;
    cout << "\t -noPureDup\t disable preprocess pure-duplicate resolution" << endl;
    cout << "\t -noSSR\t\t disable preprocess self-subsuming resolution" << endl;
    cout << "\t -preprocessBudget ms\t wall-clock cap for the simplification phase (default 10000)" << endl;
    cout << "\t -preprocessVerbose\t log per-pass stats for the simplification phase" << endl;
    cout << "\t -cb [n]\t enable clause branching (min clause length n, default 8)" << endl;
    cout << "\t -sep [n]\t enable separator branching (min active vars n, default 15)" << endl;
    cout << "\t -adaptive\t use Phase-3 adaptive (τ-based) branching on the no-separator path" << endl;
    cout << "\t -adaptiveMin n\t components with fewer than n active vars skip probing (default 12)" << endl;
    cout << "\t -adaptiveAlpha f\t Stage-0 length-decay α (default auto-picked from formula density; passing this fixes it)" << endl;
    cout << "\t -noAutoAlpha\t disable analyzer-chosen α; use the current stage0_length_decay as-is" << endl;
    cout << "\t -reactiveMetis\t enable runtime-METIS fallback at hierarchy-reject points (opt-in; measured to regress on dense sub-instances as of 2026-04-20)" << endl;
    cout << "\t -reactiveMetisMin n\t min active vars to trigger reactive METIS (default 15)" << endl;
    cout << "\t -reactiveMetisSkip k\t after a reactive-METIS failure, wait k decomposition levels before retrying (default 5)" << endl;
    cout << "\t -reactiveMetisBeta b\t Scheme F branching-var quality gate: require σ_sep_avg ≥ b·σ_top (default 0.5)" << endl;
    cout << "\t -implicantLearn\t enable implicant learning (scoped clauses from BCP traces, opt-in)" << endl;
    cout << "\t -implicantMaxSize n\t max decision literals in a learned implicant (default 4)" << endl;
    cout << "\t -implicantMaxTotal n\t cap on total implicants learned per solve (default 100000)" << endl;
    cout << "\t -l2Strict\t L2 cache uses strict canonical keys (128-bit hash + full clause multiset for structural equality). Default is compact (128-bit hash only). Debug aid." << endl;
    cout << "\t -localSearchPreprocess\t enable probe-based #SAT-sound preprocessing (diff-and-lift; opt-in). See docs/probe_preprocessing_plan.md" << endl;
    cout << "\t -lspMaxProbes n\t max probes per local-search pass (default 1000)" << endl;
    cout << "\t -lspMaxSize n\t  max σ length for probes (default 4)" << endl;
    cout << "\t -lspMaxTotal n\t max clauses learned per local-search invocation (default 5000)" << endl;
    cout << "\t -lspNoR4\t disable definitional elimination (R4) inside the local-search pass" << endl;
    cout << "\t -lspVerbose\t per-pass stats from the local-search pass" << endl;
    cout << "\t -checkLearnInvariants\t assert antecedent-in-scope at conflict-analysis and force-set time. Debug aid for t1_011-style order-dependent bugs. Aborts on violation." << endl;
    cout << "\t -permWatchIndep S\t per-literal independent watch-list shuffle, seed S. Companion to -permWatchSelect for bisection." << endl;
    cout << "\t -permWatchSelect M\t hex/dec mask: permute literal l only if (l.raw() & M) != 0. Default ~0 (all). Use with -permWatchIndep." << endl;
    cout << "\t -bruteForceCacheCheck N\t at every cache store/hit, if sub-component has <=N active vars, brute-force verify. Aborts on mismatch. Try N=18." << endl;
    cout << "\t -bruteForceCacheDumpDir DIR\t dump offending sub-components here when -bruteForceCacheCheck mismatches." << endl;
    cout << "\t -dumpRecursionDir DIR\t dump SUPER-comp formula at each solveComponent entry where depth<=K. Manifest at DIR/log.txt." << endl;
    cout << "\t -dumpRecursionMaxDepth K\t cap on dumping (default 0 = root only)." << endl;
    cout << "\t" << endl;

    return -1;
  }

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-noCC") == 0)
      theSolver.config().perform_component_caching = false;
    if (strcmp(argv[i], "-noLearn") == 0)
      theSolver.config().perform_conflict_clause_learning = false;
    if (strcmp(argv[i], "-noIBCP") == 0)
      theSolver.config().perform_failed_lit_test = false;
    if (strcmp(argv[i], "-noPP") == 0)
      theSolver.config().perform_pre_processing = false;
    else if (strcmp(argv[i], "-q") == 0)
      theSolver.config().quiet = true;
    else if (strcmp(argv[i], "-v") == 0)
      theSolver.config().verbose = true;
    else if (strcmp(argv[i], "-t") == 0) {
      if (argc <= i + 1) {
        cout << " wrong parameters" << endl;
        return -1;
      }
      theSolver.config().time_bound_seconds = atol(argv[i + 1]);
      theSolver.setTimeBound(theSolver.config().time_bound_seconds);
      if (theSolver.config().verbose)
        cout << "time bound set to" << theSolver.config().time_bound_seconds << "s\n";
     } else if (strcmp(argv[i], "-cb") == 0) {
      theSolver.config().perform_clause_branching = true;
      if (i + 1 < argc && isdigit(argv[i+1][0])) {
        theSolver.config().clause_branch_min_length = atoi(argv[i + 1]);
        i++;
      }
    } else if (strcmp(argv[i], "-sep") == 0) {
      theSolver.config().perform_separator_branching = true;
      theSolver.config().perform_clause_branching = true;  // separator uses clause branching
      if (i + 1 < argc && isdigit(argv[i+1][0])) {
        theSolver.config().separator_min_active_vars = atoi(argv[i + 1]);
        i++;
      }
    } else if (strcmp(argv[i], "-sepBiasW") == 0) {
      if (i + 1 >= argc) { cout << "-sepBiasW needs a number\n"; return -1; }
      theSolver.config().separator_bias_weight = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-decomposeInSep") == 0) {
      theSolver.config().decompose_in_separator = true;
    } else if (strcmp(argv[i], "-decomposeAfterK") == 0) {
      if (i + 1 >= argc) { cout << "-decomposeAfterK needs an integer\n"; return -1; }
      theSolver.config().decompose_after_k = (unsigned)atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-sepVarBias") == 0) {
      theSolver.config().separator_vars_as_bias = true;
    } else if (strcmp(argv[i], "-unifiedPicker") == 0) {
      theSolver.config().unified_picker = true;
    } else if (strcmp(argv[i], "-clauseScoreW") == 0) {
      if (i + 1 >= argc) { cout << "-clauseScoreW needs a number\n"; return -1; }
      theSolver.config().clause_score_weight = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-clauseLenMid") == 0) {
      if (i + 1 >= argc) { cout << "-clauseLenMid needs a number\n"; return -1; }
      theSolver.config().clause_length_midpoint = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-clauseLenBeta") == 0) {
      if (i + 1 >= argc) { cout << "-clauseLenBeta needs a number\n"; return -1; }
      theSolver.config().clause_length_steepness = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-sepImpA") == 0) {
      if (i + 1 >= argc) { cout << "-sepImpA needs a number\n"; return -1; }
      theSolver.config().separator_importance_base = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-sepSizeNormP") == 0) {
      if (i + 1 >= argc) { cout << "-sepSizeNormP needs a number in [0,1]\n"; return -1; }
      theSolver.config().separator_size_norm_p = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-cheapScoreW") == 0) {
      if (i + 1 >= argc) { cout << "-cheapScoreW needs a number\n"; return -1; }
      theSolver.config().cheap_score_weight = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-cascadeW") == 0) {
      if (i + 1 >= argc) { cout << "-cascadeW needs a number\n"; return -1; }
      theSolver.config().cascade_score_weight = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-cascadeDepth") == 0) {
      if (i + 1 >= argc) { cout << "-cascadeDepth needs an integer\n"; return -1; }
      theSolver.config().cascade_score_depth = atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-pickerMode") == 0) {
      if (i + 1 >= argc) { cout << "-pickerMode needs additive|multiplicative\n"; return -1; }
      if (strcmp(argv[i+1], "additive") == 0) {
        theSolver.config().unified_picker_mode =
          SolverConfiguration::UnifiedPickerMode::ADDITIVE;
      } else if (strcmp(argv[i+1], "multiplicative") == 0) {
        theSolver.config().unified_picker_mode =
          SolverConfiguration::UnifiedPickerMode::MULTIPLICATIVE;
      } else {
        cout << "-pickerMode must be 'additive' or 'multiplicative'\n"; return -1;
      }
      i++;
    } else if (strcmp(argv[i], "-pickerAlphaVar") == 0) {
      if (i + 1 >= argc) { cout << "-pickerAlphaVar needs a number\n"; return -1; }
      theSolver.config().picker_alpha_var = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-pickerAlphaClause") == 0) {
      if (i + 1 >= argc) { cout << "-pickerAlphaClause needs a number\n"; return -1; }
      theSolver.config().picker_alpha_clause = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-pickerLambdaVar") == 0) {
      if (i + 1 >= argc) { cout << "-pickerLambdaVar needs a number\n"; return -1; }
      theSolver.config().picker_lambda_var = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-pickerLambdaClause") == 0) {
      if (i + 1 >= argc) { cout << "-pickerLambdaClause needs a number\n"; return -1; }
      theSolver.config().picker_lambda_clause = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-pickerGamma") == 0) {
      if (i + 1 >= argc) { cout << "-pickerGamma needs a number\n"; return -1; }
      theSolver.config().picker_gamma = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-pickerVarW") == 0) {
      if (i + 1 >= argc) { cout << "-pickerVarW needs a number\n"; return -1; }
      theSolver.config().picker_var_weight = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-pickerClauseW") == 0) {
      if (i + 1 >= argc) { cout << "-pickerClauseW needs a number\n"; return -1; }
      theSolver.config().picker_clause_weight = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-pickerNonSepKillsNd") == 0) {
      theSolver.config().picker_non_sep_kills_nd = true;
    } else if (strcmp(argv[i], "-pickerRateFramework") == 0) {
      theSolver.config().picker_rate_framework = true;
    } else if (strcmp(argv[i], "-pickerRhoExp") == 0) {
      if (i + 1 >= argc) { cout << "-pickerRhoExp needs a number\n"; return -1; }
      theSolver.config().picker_rho_exp = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-pickerFrontBonus") == 0) {
      if (i + 1 >= argc) { cout << "-pickerFrontBonus needs a number\n"; return -1; }
      theSolver.config().picker_front_bonus = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-pickerNoCascadeGain") == 0) {
      theSolver.config().picker_no_cascade_gain = true;
    } else if (strcmp(argv[i], "-pickerRootSepOnly") == 0) {
      theSolver.config().picker_root_sep_only = true;
    } else if (strcmp(argv[i], "-rec") == 0) {
      // Accepted for backward compatibility; recursive is now the only solver.
    } else if (strcmp(argv[i], "-learnLevel") == 0) {
      if (i + 1 >= argc) { cout << "-learnLevel needs a number 0..5\n"; return -1; }
      int lvl = atoi(argv[i + 1]);
      if (lvl < 0) lvl = 0;
      if (lvl > 5) lvl = 5;
      theSolver.config().learn_level = lvl;
      i++;
    } else if (strcmp(argv[i], "-verifyLearn") == 0) {
      theSolver.config().verify_learn = true;
    } else if (strcmp(argv[i], "-noSubsumption") == 0) {
      theSolver.config().perform_preprocess_subsumption = false;
    } else if (strcmp(argv[i], "-noPureDup") == 0) {
      theSolver.config().perform_preprocess_pure_duplicate = false;
    } else if (strcmp(argv[i], "-noSSR") == 0) {
      theSolver.config().perform_preprocess_ssr = false;
    } else if (strcmp(argv[i], "-preprocessBudget") == 0) {
      if (i + 1 >= argc) { cout << "-preprocessBudget needs ms\n"; return -1; }
      theSolver.config().preprocess_time_budget_ms = atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-preprocessVerbose") == 0) {
      theSolver.config().preprocess_verbose = true;
    } else if (strcmp(argv[i], "-adaptive") == 0) {
      theSolver.config().perform_adaptive_branching = true;
    } else if (strcmp(argv[i], "-adaptiveAlpha") == 0) {
      if (i + 1 >= argc) { cout << "-adaptiveAlpha needs a number\n"; return -1; }
      theSolver.config().stage0_length_decay = atof(argv[i + 1]);
      theSolver.config().auto_stage0_length_decay = false;  // user override
      i++;
    } else if (strcmp(argv[i], "-noAutoAlpha") == 0) {
      theSolver.config().auto_stage0_length_decay = false;
    } else if (strcmp(argv[i], "-reactiveMetis") == 0) {
      theSolver.config().use_reactive_metis = true;
    } else if (strcmp(argv[i], "-noReactiveMetis") == 0) {
      theSolver.config().use_reactive_metis = false;
    } else if (strcmp(argv[i], "-reactiveMetisMin") == 0) {
      if (i + 1 < argc && isdigit(argv[i+1][0])) {
        theSolver.config().reactive_metis_min_vars = atoi(argv[i + 1]);
        i++;
      }
    } else if (strcmp(argv[i], "-reactiveMetisSkip") == 0) {
      if (i + 1 < argc && isdigit(argv[i+1][0])) {
        theSolver.config().reactive_metis_skip_k = atoi(argv[i + 1]);
        i++;
      }
    } else if (strcmp(argv[i], "-reactiveMetisBeta") == 0) {
      if (i + 1 < argc) {
        theSolver.config().reactive_metis_sigma_beta = atof(argv[i + 1]);
        i++;
      }
    } else if (strcmp(argv[i], "-implicantLearn") == 0) {
      theSolver.config().perform_implicant_learning = true;
    } else if (strcmp(argv[i], "-implicantMaxSize") == 0) {
      if (i + 1 < argc && isdigit(argv[i+1][0])) {
        theSolver.config().implicant_max_size = atoi(argv[i + 1]);
        i++;
      }
    } else if (strcmp(argv[i], "-implicantMaxTotal") == 0) {
      if (i + 1 < argc && isdigit(argv[i+1][0])) {
        theSolver.config().implicant_max_total = atoi(argv[i + 1]);
        i++;
      }
    } else if (strcmp(argv[i], "-implicantDryRun") == 0) {
      theSolver.config().implicant_dry_run = true;
    } else if (strcmp(argv[i], "-analyzeClausePool") == 0) {
      theSolver.config().analyze_clause_pool = true;
    } else if (strcmp(argv[i], "-dumpPreprocessed") == 0) {
      if (argc <= i + 1) { cout << " -dumpPreprocessed needs a path\n"; return -1; }
      theSolver.config().dump_preprocessed_path = argv[i + 1];
      i++;
    } else if (strcmp(argv[i], "-dumpNDAndExit") == 0) {
      if (argc <= i + 1) { cout << " -dumpNDAndExit needs a path\n"; return -1; }
      theSolver.config().dump_nd_and_exit_path = argv[i + 1];
      i++;
    } else if (strcmp(argv[i], "-permClauseLits") == 0) {
      if (argc <= i + 1) { cout << " -permClauseLits needs a seed (uint)\n"; return -1; }
      theSolver.config().perm_clause_lits_seed = (unsigned)atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-permBinaryLinks") == 0) {
      if (argc <= i + 1) { cout << " -permBinaryLinks needs a seed (uint)\n"; return -1; }
      theSolver.config().perm_binary_links_seed = (unsigned)atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-permWatchLists") == 0) {
      if (argc <= i + 1) { cout << " -permWatchLists needs a seed (uint)\n"; return -1; }
      theSolver.config().perm_watch_lists_seed = (unsigned)atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-permOccLists") == 0) {
      if (argc <= i + 1) { cout << " -permOccLists needs a seed (uint)\n"; return -1; }
      theSolver.config().perm_occ_lists_seed = (unsigned)atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-sortBinaryLinks") == 0) {
      theSolver.config().sort_binary_links = true;
    } else if (strcmp(argv[i], "-sortWatchLists") == 0) {
      theSolver.config().sort_watch_lists = true;
    } else if (strcmp(argv[i], "-sortOccLists") == 0) {
      theSolver.config().sort_occ_lists = true;
    } else if (strcmp(argv[i], "-sortClauseLits") == 0) {
      theSolver.config().sort_clause_lits = true;
    } else if (strcmp(argv[i], "-sortClausePool") == 0) {
      theSolver.config().sort_clause_pool = true;
    } else if (strcmp(argv[i], "-learnTrace") == 0) {
      if (argc <= i + 1) { cout << " -learnTrace needs a path\n"; return -1; }
      theSolver.config().learn_trace_path = argv[i + 1];
      i++;
    } else if (strcmp(argv[i], "-pathTraceOfs") == 0) {
      if (argc <= i + 1) { cout << " -pathTraceOfs needs a clause ofs\n"; return -1; }
      theSolver.config().path_trace_ofs = (unsigned)atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-pathTraceCompVars") == 0) {
      if (argc <= i + 1) { cout << " -pathTraceCompVars needs a csv var list\n"; return -1; }
      theSolver.config().path_trace_comp_vars = argv[i + 1];
      i++;
    } else if (strcmp(argv[i], "-logBranches") == 0) {
      theSolver.config().log_branches = true;
    } else if (strcmp(argv[i], "-logConflicts") == 0) {
      theSolver.config().log_conflicts = true;
    } else if (strcmp(argv[i], "-forceDecisions") == 0) {
      if (argc <= i + 1) { cout << " -forceDecisions needs comma-sep literals\n"; return -1; }
      std::string s = argv[i + 1];
      size_t start = 0;
      while (start < s.size()) {
        size_t c = s.find(',', start);
        if (c == std::string::npos) c = s.size();
        std::string tok = s.substr(start, c - start);
        if (!tok.empty()) theSolver.config().forced_decisions.push_back(atoi(tok.c_str()));
        start = c + 1;
      }
      i++;
    } else if (strcmp(argv[i], "-stopAtBranch") == 0) {
      if (argc <= i + 2) { cout << " -stopAtBranch needs N and path\n"; return -1; }
      theSolver.config().stop_at_branch = atoll(argv[i + 1]);
      theSolver.config().stop_at_branch_path = argv[i + 2];
      i += 2;
    } else if (strcmp(argv[i], "-dumpCompDir") == 0) {
      if (argc <= i + 1) { cout << " -dumpCompDir needs a path\n"; return -1; }
      theSolver.config().dump_comp_dir = argv[i + 1];
      i++;
    } else if (strcmp(argv[i], "-dumpCompMinVars") == 0) {
      if (argc <= i + 1) { cout << " -dumpCompMinVars needs a number\n"; return -1; }
      theSolver.config().dump_comp_min_vars = (unsigned)atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-dumpCompMaxVars") == 0) {
      if (argc <= i + 1) { cout << " -dumpCompMaxVars needs a number\n"; return -1; }
      theSolver.config().dump_comp_max_vars = (unsigned)atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-analyzeDynamic") == 0) {
      theSolver.config().analyze_dynamic_subsumption = true;
    } else if (strcmp(argv[i], "-analyzeDynamicEvery") == 0) {
      if (i + 1 < argc && isdigit(argv[i+1][0])) {
        theSolver.config().analyze_dynamic_subsumption_every = atoi(argv[i + 1]);
        i++;
      }
    } else if (strcmp(argv[i], "-implicantMinChain") == 0) {
      if (i + 1 < argc && isdigit(argv[i+1][0])) {
        theSolver.config().implicant_min_chain_depth = atoi(argv[i + 1]);
        i++;
      }
    } else if (strcmp(argv[i], "-adaptiveMin") == 0) {
      if (argc <= i + 1) {
        cout << " -adaptiveMin needs a numeric argument" << endl;
        return -1;
      }
      theSolver.config().adaptive_probing_min_vars = atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-verifyCache") == 0) {
      theSolver.config().verify_cache = true;
    } else if (strcmp(argv[i], "-l2Strict") == 0) {
      theSolver.config().canonical_compact = false;
    } else if (strcmp(argv[i], "-l2Compact") == 0) {
      theSolver.config().canonical_compact = true;
    } else if (strcmp(argv[i], "-localSearchPreprocess") == 0) {
      theSolver.config().perform_local_search_preprocess = true;
    } else if (strcmp(argv[i], "-lspMaxProbes") == 0) {
      if (i + 1 >= argc) { cout << "-lspMaxProbes needs a number\n"; return -1; }
      theSolver.config().lsp_max_probes = (unsigned)atoi(argv[i + 1]); i++;
    } else if (strcmp(argv[i], "-lspMaxSize") == 0) {
      if (i + 1 >= argc) { cout << "-lspMaxSize needs a number\n"; return -1; }
      theSolver.config().lsp_max_size = (unsigned)atoi(argv[i + 1]); i++;
    } else if (strcmp(argv[i], "-lspMaxTotal") == 0) {
      if (i + 1 >= argc) { cout << "-lspMaxTotal needs a number\n"; return -1; }
      theSolver.config().lsp_max_total = (unsigned)atoi(argv[i + 1]); i++;
    } else if (strcmp(argv[i], "-lspNoR4") == 0) {
      theSolver.config().lsp_no_r4 = true;
    } else if (strcmp(argv[i], "-lspVerbose") == 0) {
      theSolver.config().lsp_verbose = true;
    } else if (strcmp(argv[i], "-checkLearnInvariants") == 0) {
      theSolver.config().check_learn_invariants = true;
    } else if (strcmp(argv[i], "-permWatchIndep") == 0) {
      if (argc <= i + 1) { cout << "-permWatchIndep needs a seed\n"; return -1; }
      theSolver.config().perm_watch_indep_seed = (unsigned)atoi(argv[i + 1]); i++;
    } else if (strcmp(argv[i], "-permWatchSelect") == 0) {
      if (argc <= i + 1) { cout << "-permWatchSelect needs a mask\n"; return -1; }
      theSolver.config().perm_watch_indep_mask = (unsigned)strtoul(argv[i + 1], nullptr, 0); i++;
    } else if (strcmp(argv[i], "-bruteForceCacheCheck") == 0) {
      if (argc <= i + 1) { cout << "-bruteForceCacheCheck needs N\n"; return -1; }
      theSolver.config().brute_force_cache_check_n = (unsigned)atoi(argv[i + 1]); i++;
    } else if (strcmp(argv[i], "-bruteForceCacheDumpDir") == 0) {
      if (argc <= i + 1) { cout << "-bruteForceCacheDumpDir needs a path\n"; return -1; }
      theSolver.config().brute_force_cache_dump_dir = argv[i + 1]; i++;
    } else if (strcmp(argv[i], "-dumpRecursionDir") == 0) {
      if (argc <= i + 1) { cout << "-dumpRecursionDir needs a path\n"; return -1; }
      theSolver.config().dump_recursion_dir = argv[i + 1]; i++;
    } else if (strcmp(argv[i], "-dumpRecursionMaxDepth") == 0) {
      if (argc <= i + 1) { cout << "-dumpRecursionMaxDepth needs K\n"; return -1; }
      theSolver.config().dump_recursion_max_depth = (unsigned)atoi(argv[i + 1]); i++;
    } else if (strcmp(argv[i], "-noAnonymization") == 0) {
      theSolver.config().no_anonymization = true;
    } else if (strcmp(argv[i], "-canonStats") == 0) {
      theSolver.config().print_canon_stats = true;
    } else if (strcmp(argv[i], "-wlIter") == 0) {
      if (argc <= i + 1) { cout << "-wlIter needs an int\n"; return -1; }
      theSolver.config().wl_iterations = atoi(argv[i + 1]); i++;
    } else if (strcmp(argv[i], "-cs") == 0) {
      if (argc <= i + 1) {
        cout << " wrong parameters" << endl;
        return -1;
      }
      theSolver.statistics().maximum_cache_size_bytes_ = atol(argv[i + 1]) * (uint64_t) 1000000;
    } else
      input_file = argv[i];
  }

  theSolver.solve(input_file);
  return 0;
}
