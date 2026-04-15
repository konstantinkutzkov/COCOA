/*
 * basic_types.h
 *
 *  Created on: Jun 24, 2012
 *      Author: Marc Thurley
 */

#ifndef SOLVER_CONFIG_H_
#define SOLVER_CONFIG_H_


struct SolverConfiguration {

  bool perform_non_chron_back_track = true;

  // TODO component caching cannot be deactivated for now!
  bool perform_component_caching = true;
  bool perform_failed_lit_test = true;
  bool perform_pre_processing = true;

  unsigned long time_bound_seconds = 100000;

  bool verbose = false;

  // quiet = true will override verbose;
  bool quiet = false;

  // Clause branching: branch on long clauses using
  // #SAT(F) = #SAT(F\{C}) - #SAT(F\{C} ∧ ¬C)
  bool perform_clause_branching = false;
  unsigned clause_branch_min_length = 8;

  // Separator branching: use minimum vertex cut in the incidence graph
  // to select variables and clauses for branching
  bool perform_separator_branching = false;
  unsigned separator_min_active_vars = 15;
  unsigned separator_max_size = 30;
  unsigned separator_tries = 12;
  unsigned separator_min_second_comp = 12;

  // Use the recursive #SAT implementation (solver_rec.cpp) instead of
  // the iterative countSAT loop.
  bool use_recursive_solver = false;

  // Verify cache keys: lookup() always misses, and store() compares the
  // newly computed count against any previously stored count for the
  // same key. A mismatch indicates a semantic bug in the canonical key.
  bool verify_cache = false;
};

#endif /* SOLVER_CONFIG_H_ */
