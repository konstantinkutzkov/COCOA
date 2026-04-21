/*
 * statistics.h
 *
 *  Created on: Feb 13, 2013
 *      Author: mthurley
 */

#ifndef STATISTICS_H_
#define STATISTICS_H_

#include <string>
#include <cstdint>
#include <vector>

#include <gmpxx.h>

#include "structures.h"
// cacheable_component.h removed — using content-based cache

#include "primitive_types.h"

using namespace std;

class DataAndStatistics {
public:
  string input_file_;
  double time_elapsed_ = 0.0;
  uint64_t maximum_cache_size_bytes_ = 0;

  SOLVER_StateT exit_state_ = NO_STATE;
  // different variable counts
  // number of variables  and clauses before preprocessing
  unsigned long num_original_variables_ = 0;
  unsigned long num_original_clauses_ = 0;
  unsigned long num_original_binary_clauses_ = 0;
  unsigned long num_original_unit_clauses_ = 0;

  // number of variables remaining
  unsigned long num_variables_ = 0;
  // number of variables that actually occurs in clauses
  unsigned long num_used_variables_ = 0;
  unsigned long num_free_variables_ = 0;

  /// different clause counts

  // number of clauses after preprocessing
  unsigned long num_long_clauses_ = 0;
  unsigned long num_binary_clauses_ = 0;

  unsigned long num_long_conflict_clauses_ = 0;
  unsigned long num_binary_conflict_clauses_ = 0;

  unsigned long times_conflict_clauses_cleaned_ = 0;

  unsigned long num_unit_clauses_ = 0;

  /// number of all decisions made
  unsigned long num_decisions_ = 0;
  /// number of all implications derived
  unsigned long num_implications_ = 0;
  // number of all failed literal detections
  unsigned long num_failed_literals_detected_ = 0;
  unsigned long num_failed_literal_tests_ = 0;
  // number of all conflicts occurred
  unsigned long num_conflicts_ = 0;

  // number of clauses overall learned
  unsigned num_clauses_learned_ = 0;
  // Dedup drops across ALL learning sites (conflict-UIP + implicant).
  // See instance.h's ClauseSigFilter / maybeDedupClause.
  unsigned long num_learned_dedup_dropped_ = 0;

  // Implicant learning (Phase 4). All zero unless
  // config_.perform_implicant_learning is on.
  unsigned long num_implicants_learned_        = 0;
  unsigned long num_implicants_size_dropped_   = 0;  // size cap fired
  unsigned long num_implicants_trivial_dropped_= 0;  // equal to antecedent
  unsigned long num_implicants_dedup_dropped_  = 0;  // LRU hit
  unsigned long num_implicants_quota_stop_     = 0;  // total cap reached
  unsigned long num_implicants_fired_          = 0;  // used in BCP
  unsigned long num_implicants_fired_outside_  = 0;  // fired in a subtree other than learn-site
  unsigned long num_implicants_depth_dropped_  = 0;  // chain too shallow


  /* cache statistics */
  uint64_t num_cache_hits_ = 0;
  uint64_t num_cache_look_ups_ = 0;
  uint64_t sum_cache_hit_sizes_ = 0;

  uint64_t num_cached_components_ = 0;
  uint64_t sum_size_cached_components_ = 0;

  // the number of bytes occupied by all
  // components
  uint64_t sum_bytes_cached_components_ = 0;
  // the same number, summing over all components ever stored
  uint64_t overall_bytes_components_stored_ = 0;

  // the above numbers, but without any overhead,
  // counting only the pure data size of the components - without model counts
  uint64_t sum_bytes_pure_cached_component_data_ = 0;
  // the same number, summing over all components ever stored
  uint64_t overall_bytes_pure_stored_component_data_ = 0;


  uint64_t sys_overhead_sum_bytes_cached_components_ = 0;
    // the same number, summing over all components ever stored
  uint64_t sys_overhead_overall_bytes_components_stored_ = 0;

  uint64_t cache_infrastructure_bytes_memory_usage_ = 0;


  uint64_t overall_num_cache_stores_ = 0;
  /*end statistics */

  bool cache_full(){
    return cache_bytes_memory_usage() >= maximum_cache_size_bytes_;
  }

  uint64_t cache_bytes_memory_usage(){
    return cache_infrastructure_bytes_memory_usage_
           + sum_bytes_cached_components_;
  }

  uint64_t overall_cache_bytes_memory_stored(){
      return cache_infrastructure_bytes_memory_usage_
             + overall_bytes_components_stored_;
    }

  // Old cache statistics methods removed — using content-based cache
  unsigned long cache_MB_memory_usage() {
      return cache_bytes_memory_usage() / 1000000;
  }
  mpz_class final_solution_count_ = 0;

  double implicitBCP_miss_rate() {
      if(num_failed_literal_tests_ == 0) return 0.0;
      return (num_failed_literal_tests_ - num_failed_literals_detected_) / (double) num_failed_literal_tests_;
  }
  unsigned long num_clauses() {
    return num_long_clauses_ + num_binary_clauses_ + num_unit_clauses_;
  }
  unsigned long num_conflict_clauses() {
    return num_long_conflict_clauses_ + num_binary_conflict_clauses_;
  }

  unsigned long clause_deletion_interval() {
    return 10000 + 10 * times_conflict_clauses_cleaned_;
  }

  void set_final_solution_count(const mpz_class &count) {
    // set final_solution_count_ = count * 2^(num_variables_ - num_used_variables_)
    mpz_mul_2exp(final_solution_count_.get_mpz_t (),count.get_mpz_t (), num_variables_ - num_used_variables_);
  }

  const mpz_class &final_solution_count() const {
    return final_solution_count_;
  }

  void incorporateConflictClauseData(const vector<LiteralID> &clause) {
    if (clause.size() == 1)
      num_unit_clauses_++;
    else if (clause.size() == 2)
      num_binary_conflict_clauses_++;
    num_long_conflict_clauses_++;
  }
  void incorporateClauseData(const vector<LiteralID> &clause) {
    if (clause.size() == 1)
      num_unit_clauses_++;
    else if (clause.size() == 2)
      num_binary_clauses_++;
    else
      num_long_clauses_++;
  }

  void print_final_solution_count();
  void writeToFile(const string & file_name);

  void printShort();

  void printShortFormulaInfo() {
    cout << "variables (all/used/free): \t";
    cout << num_variables_ << "/" << num_used_variables_ << "/";
    cout << num_variables_ - num_used_variables_ << endl;

    cout << "clauses (all/long/binary/unit): ";
    cout << num_clauses() << "/" << num_long_clauses_;
    cout << "/" << num_binary_clauses_ << "/" << num_unit_clauses_ << endl;
  }
  unsigned getTime() {
    return num_decisions_;
  }

  double avgCachedSize() {
    if (num_cache_hits_ == 0)
      return 0.0;
    return (double) sum_size_cached_components_
        / (double) num_cached_components_;
  }

  double avgCacheHitSize() {
    if (num_cache_hits_ == 0)
      return 0.0;
    return (double) sum_cache_hit_sizes_ / (double) num_cache_hits_;
  }

  long double getAvgComponentSize() {
    return sum_size_cached_components_ / (long double) num_cached_components_;
  }

  unsigned long cached_component_count() {
    return num_cached_components_;
  }

  unsigned long cache_hits() {
    return num_cache_hits_;
  }

  double cache_miss_rate() {
    if(num_cache_look_ups_ == 0) return 0.0;
    return (num_cache_look_ups_ - num_cache_hits_)
        / (double) num_cache_look_ups_;
  }

  long double getAvgCacheHitSize() {
    if(num_cache_hits_ == 0) return 0.0;
    return sum_cache_hit_sizes_ / (long double) num_cache_hits_;
  }
};

#endif /* STATISTICS_H_ */
