#include "solver.h"
#include "preprocessor_light.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace std;


int main(int argc, char *argv[]) {

  string input_file;
  Solver theSolver;
  bool arjun_light = false;


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
    cout << "\t -reactiveMetis\t enable runtime-METIS fallback at hierarchy-reject points (opt-in; measured to regress on dense sub-instances as of 2026-04-20)" << endl;
    cout << "\t -reactiveMetisMin n\t min active vars to trigger reactive METIS (default 15)" << endl;
    cout << "\t -reactiveMetisSkip k\t after a reactive-METIS failure, wait k decomposition levels before retrying (default 5)" << endl;
    cout << "\t -reactiveMetisBeta b\t Scheme F branching-var quality gate: require σ_sep_avg ≥ b·σ_top (default 0.5)" << endl;
    cout << "\t -localSearchPreprocess\t enable probe-based #SAT-sound preprocessing (diff-and-lift; opt-in). See docs/probe_preprocessing_plan.md" << endl;
    cout << "\t -lspNoR4\t disable definitional elimination (R4) inside the local-search pass" << endl;
    cout << "\t -checkLearnInvariants\t assert antecedent-in-scope at conflict-analysis and force-set time. Debug aid for t1_011-style order-dependent bugs. Aborts on violation." << endl;
    cout << "\t -bruteForceCacheCheck N\t at every cache store/hit, if sub-component has <=N active vars, brute-force verify. Aborts on mismatch. Try N=18." << endl;
    cout << "\t -bruteForceCacheDumpDir DIR\t dump offending sub-components here when -bruteForceCacheCheck mismatches." << endl;
    cout << "\t" << endl;

    return -1;
  }

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-noCC") == 0)
      theSolver.config().perform_component_caching = false;
    if (strcmp(argv[i], "-noSepLearn") == 0)
      theSolver.config().allow_learning_in_separator_branching = false;
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
    } else if (strcmp(argv[i], "-decomposeInSep") == 0) {
      theSolver.config().decompose_in_separator = true;
    } else if (strcmp(argv[i], "-decomposeAfterK") == 0) {
      if (i + 1 >= argc) { cout << "-decomposeAfterK needs an integer\n"; return -1; }
      theSolver.config().decompose_after_k = (unsigned)atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-sepClausesFirst") == 0) {
      theSolver.config().separator_clauses_first = true;
    } else if (strcmp(argv[i], "-unifiedPicker") == 0) {
      theSolver.config().unified_picker = true;
    } else if (strcmp(argv[i], "-clauseLenMid") == 0) {
      if (i + 1 >= argc) { cout << "-clauseLenMid needs a number\n"; return -1; }
      theSolver.config().clause_length_midpoint = atof(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-clauseLenBeta") == 0) {
      if (i + 1 >= argc) { cout << "-clauseLenBeta needs a number\n"; return -1; }
      theSolver.config().clause_length_steepness = atof(argv[i + 1]);
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
    } else if (strcmp(argv[i], "-pickerRootSepOnly") == 0) {
      theSolver.config().picker_root_sep_only = true;
    } else if (strcmp(argv[i], "-pickerSepLockstep") == 0) {
      theSolver.config().picker_sep_lockstep = true;
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
    } else if (strcmp(argv[i], "-reactiveMetis") == 0) {
      theSolver.config().use_reactive_metis = true;
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
    } else if (strcmp(argv[i], "-ndCentralityW") == 0) {
      if (i + 1 < argc) {
        theSolver.config().nd_centrality_weight = atof(argv[i + 1]);
        i++;
      }
    } else if (strcmp(argv[i], "-dumpPreprocessed") == 0) {
      if (argc <= i + 1) { cout << " -dumpPreprocessed needs a path\n"; return -1; }
      theSolver.config().dump_preprocessed_path = argv[i + 1];
      i++;
    } else if (strcmp(argv[i], "-dumpNDAndExit") == 0) {
      if (argc <= i + 1) { cout << " -dumpNDAndExit needs a path\n"; return -1; }
      theSolver.config().dump_nd_and_exit_path = argv[i + 1];
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
    } else if (strcmp(argv[i], "-adaptiveMin") == 0) {
      if (argc <= i + 1) {
        cout << " -adaptiveMin needs a numeric argument" << endl;
        return -1;
      }
      theSolver.config().adaptive_probing_min_vars = atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-localSearchPreprocess") == 0) {
      theSolver.config().perform_local_search_preprocess = true;
    } else if (strcmp(argv[i], "-lspNoR4") == 0) {
      theSolver.config().lsp_no_r4 = true;
    } else if (strcmp(argv[i], "-checkLearnInvariants") == 0) {
      theSolver.config().check_learn_invariants = true;
    } else if (strcmp(argv[i], "-bruteForceCacheCheck") == 0) {
      if (argc <= i + 1) { cout << "-bruteForceCacheCheck needs N\n"; return -1; }
      theSolver.config().brute_force_cache_check_n = (unsigned)atoi(argv[i + 1]); i++;
    } else if (strcmp(argv[i], "-bruteForceCacheDumpDir") == 0) {
      if (argc <= i + 1) { cout << "-bruteForceCacheDumpDir needs a path\n"; return -1; }
      theSolver.config().brute_force_cache_dump_dir = argv[i + 1]; i++;
    } else if (strcmp(argv[i], "-wlIter") == 0) {
      if (argc <= i + 1) { cout << "-wlIter needs an int\n"; return -1; }
      theSolver.config().wl_iterations = atoi(argv[i + 1]); i++;
    } else if (strcmp(argv[i], "-cs") == 0) {
      if (argc <= i + 1) {
        cout << " wrong parameters" << endl;
        return -1;
      }
      theSolver.statistics().maximum_cache_size_bytes_ = atol(argv[i + 1]) * (uint64_t) 1000000;
    } else if (strcmp(argv[i], "-arjunLight") == 0) {
      arjun_light = true;
    } else if (strcmp(argv[i], "-dumpReactiveMetisInputs") == 0) {
      if (argc <= i + 1) { cout << "-dumpReactiveMetisInputs needs a path\n"; return -1; }
      theSolver.config().dump_reactive_metis_path = argv[i + 1];
      i++;
    } else
      input_file = argv[i];
  }

  // Arjun-light: count-preserving CMS5 simplification on the input CNF.
  // Writes the simplified CNF to a temp file and routes the solver at it.
  // See preprocessor_light.h for the pass selection rationale.
  // Also extracts SCC equivalences to a sidecar and injects them into
  // the solver's redundant-binary lane (BCP-only, invisible to component
  // decomposition and the canonical-key cache).
  string simplified_path;
  string equiv_sidecar_path;
  if (arjun_light && !input_file.empty()) {
    char tmpl[] = "/tmp/sharpsat_arjunlight_XXXXXX.cnf";
    int fd = mkstemps(tmpl, 4);
    if (fd < 0) {
      cerr << "[arjun-light] mkstemps failed; skipping preprocessing\n";
    } else {
      close(fd);
      simplified_path = tmpl;
      equiv_sidecar_path = simplified_path + ".eqv";
      if (!PreprocessorLight::simplify(input_file, simplified_path,
                                       0,
                                       equiv_sidecar_path)) {
        cerr << "[arjun-light] simplification failed; using original CNF\n";
        unlink(simplified_path.c_str());
        unlink(equiv_sidecar_path.c_str());
        simplified_path.clear();
        equiv_sidecar_path.clear();
      } else {
        cout << "c o [arjun-light] simplified CNF written to "
             << simplified_path << "\n";
        input_file = simplified_path;

        // Load equivalences from sidecar and queue them with the solver.
        std::ifstream eqv_in(equiv_sidecar_path);
        if (eqv_in) {
          std::vector<std::tuple<unsigned, unsigned, bool>> equivs;
          std::string line;
          unsigned expected = 0;
          bool have_count = false;
          while (std::getline(eqv_in, line)) {
            if (line.empty() || line[0] == 'c') continue;
            std::istringstream ss(line);
            if (!have_count) { ss >> expected; have_count = true; continue; }
            unsigned v1 = 0, v2 = 0;
            char pol = '+';
            if (ss >> v1 >> v2 >> pol)
              equivs.emplace_back(v1, v2, pol == '+');
          }
          cout << "c o [arjun-light] sidecar reported " << equivs.size()
               << " equivalences (expected " << expected << ")\n";
          theSolver.setPendingRedundantEquivalences(std::move(equivs));
        }
      }
    }
  }

  theSolver.solve(input_file);

  if (!simplified_path.empty()) {
    unlink(simplified_path.c_str());
    unlink(equiv_sidecar_path.c_str());
  }
  return 0;
}
