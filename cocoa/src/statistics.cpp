/*
 * statistics.cpp
 *
 *  Created on: Feb 13, 2013
 *      Author: mthurley
 */

#include "statistics.h"

#include <iostream>
#include <fstream>

using namespace std;

void DataAndStatistics::print_final_solution_count() {
  cout << final_solution_count_.get_str();
}

void DataAndStatistics::writeToFile(const string & file_name) {
  ofstream out(file_name, ios_base::app);
  unsigned pos = input_file_.find_last_of("/\\");
  out << "<tr>" << endl;
  out << "<td>" << input_file_.substr(pos + 1) << "</td>" << endl;
  out << "<td>" << num_original_variables_ << "</td>" << endl;
  out << "<td>" << num_original_clauses_ << "</td>" << endl;
  out << "<td>" << num_decisions_ << "</td>" << endl;
  out << "<td>" << time_elapsed_ << "</td>" << endl;

  string s = final_solution_count_.get_str();
  if (final_solution_count_ == 0)
    s = "UNSAT";
  out << "<td>" << s << "</td>" << endl;
  out << "</tr>" << endl;
}

void DataAndStatistics::printShort() {
  if (exit_state_ == TIMEOUT) {
    cout << endl << " TIMEOUT !" << endl;
    cout << "decisions \t\t\t\t" << num_decisions_ << endl;
    cout << "cache (stores / hits) \t\t\t" << num_cached_components_ << "/"
         << num_cache_hits_ << endl;
    cout << "time: " << time_elapsed_ << "s" << endl;
    return;
  }
  cout << endl << endl;
  cout << "variables (total / active / free)\t" << num_variables_ << "/"
      << num_used_variables_ << "/" << num_variables_ - num_used_variables_
      << endl;
  cout << "clauses (removed) \t\t\t" << num_original_clauses_ << " ("
      << num_original_clauses_ - num_clauses() << ")" << endl;
  cout << "decisions \t\t\t\t" << num_decisions_ << endl;
  cout << "conflicts \t\t\t\t" << num_conflicts_ << endl;
  cout << "conflict clauses (all/bin/unit) \t";
  cout << num_conflict_clauses();
  cout << "/" << num_binary_conflict_clauses_ << "/" << num_unit_clauses_
      << endl << endl;
  cout << "failed literals found by implicit BCP \t "
      << num_failed_literals_detected_ << endl;


  cout << "implicit BCP miss rate \t " << implicitBCP_miss_rate() * 100 << "%";
  cout << endl;
  // LIVE cache size — the counter byte-bounded eviction actually enforces
  // (cur_bytes_), not the legacy sum_bytes_cached_components_ which is no longer
  // maintained and printed 0. Reported in bytes + MB, with the eviction count.
  cout << "bytes cache size (live)\t" << cache_live_bytes_ << "\t("
      << (cache_live_bytes_ / (1024.0 * 1024.0)) << " MB), evictions "
      << cache_evictions_ << endl;


  cout << "cache (stores / hits) \t\t\t" << num_cached_components_ << "/"
      << num_cache_hits_ << endl;
  cout << "cache miss rate " << cache_miss_rate() * 100 << "%" << endl;
  cout << " avg. variable count (stores / hits) \t" << getAvgComponentSize()
      << "/" << getAvgCacheHitSize() << endl << endl;
  cout << "\n# solutions " << endl;

  print_final_solution_count();

  cout << "\n# END" << endl << endl;
  cout << "time: " << time_elapsed_ << "s\n\n";
}
