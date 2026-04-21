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
    cout << "\t -cb [n]\t enable clause branching (min clause length n, default 8)" << endl;
    cout << "\t -sep [n]\t enable separator branching (min active vars n, default 15)" << endl;
    cout << "\t -adaptive\t use Phase-3 adaptive (τ-based) branching on the no-separator path" << endl;
    cout << "\t -adaptiveMin n\t components with fewer than n active vars skip probing (default 12)" << endl;
    cout << "\t -reactiveMetis\t enable runtime-METIS fallback at hierarchy-reject points (opt-in; measured to regress on dense sub-instances as of 2026-04-20)" << endl;
    cout << "\t -reactiveMetisMin n\t min active vars to trigger reactive METIS (default 15)" << endl;
    cout << "\t -reactiveMetisSkip k\t after a reactive-METIS failure, wait k decomposition levels before retrying (default 5)" << endl;
    cout << "\t -reactiveMetisBeta b\t Scheme F branching-var quality gate: require σ_sep_avg ≥ b·σ_top (default 0.5)" << endl;
    cout << "\t -implicantLearn\t enable implicant learning (scoped clauses from BCP traces, opt-in)" << endl;
    cout << "\t -implicantMaxSize n\t max decision literals in a learned implicant (default 4)" << endl;
    cout << "\t -implicantMaxTotal n\t cap on total implicants learned per solve (default 100000)" << endl;
    cout << "\t" << endl;

    return -1;
  }

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-noCC") == 0)
      theSolver.config().perform_component_caching = false;
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
    } else if (strcmp(argv[i], "-rec") == 0) {
      // Accepted for backward compatibility; recursive is now the only solver.
    } else if (strcmp(argv[i], "-adaptive") == 0) {
      theSolver.config().perform_adaptive_branching = true;
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
    } else if (strcmp(argv[i], "-adaptiveMin") == 0) {
      if (argc <= i + 1) {
        cout << " -adaptiveMin needs a numeric argument" << endl;
        return -1;
      }
      theSolver.config().adaptive_probing_min_vars = atoi(argv[i + 1]);
      i++;
    } else if (strcmp(argv[i], "-verifyCache") == 0) {
      theSolver.config().verify_cache = true;
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
