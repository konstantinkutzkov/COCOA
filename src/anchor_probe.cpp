/*
 * anchor_probe.cpp
 *
 * Pure helpers for the depth-bounded anchor probe. The recursive
 * enumeration itself lives in Solver (anchor_probe_run.cpp) because
 * it needs Solver internals (literal_stack_, BCP, comp_manager_,
 * canonical_key machinery). This file holds only:
 *   - status name lookup
 *   - score formula
 *   - JSON output
 */

#include "anchor_probe.h"

#include <cmath>
#include <ostream>

namespace anchor_probe {

const char *statusName(ProbeStatus s) {
  switch (s) {
    case ProbeStatus::OK:           return "OK";
    case ProbeStatus::UNSAT_AT_PIN: return "UNSAT_AT_PIN";
    case ProbeStatus::BCP_CLOSED:   return "BCP_CLOSED";
  }
  return "UNKNOWN";
}

double computeAmplificationScore(uint64_t mult, unsigned comp_size,
                                 double beta) {
  if (mult <= 1) return 0.0;
  double sz = (double)comp_size;
  double sz_term = (beta == 1.0) ? sz : std::pow(sz, beta);
  return (double)mult * sz_term;
}

static void writeBuckets(std::ostream &out, const SizeBuckets &b) {
  out << "{\"small_<25\":" << b.small
      << ",\"medium_25_100\":" << b.medium
      << ",\"large_100_400\":" << b.large
      << ",\"xlarge_400+\":" << b.xlarge << "}";
}

static void writePerPol(std::ostream &out, const PerPolarityResult &r) {
  out << "{\"var\":" << r.var
      << ",\"polarity\":\"" << (r.polarity ? "T" : "F") << "\""
      << ",\"status\":\"" << statusName(r.status) << "\""
      << ",\"depth_used\":" << r.depth_used
      << ",\"paths_explored\":" << r.paths_explored
      << ",\"total_keys\":" << r.total_keys
      << ",\"unique_keys\":" << r.unique_keys
      << ",\"repeated_keys\":" << r.repeated_keys
      << ",\"repeat_count\":" << r.repeat_count
      << ",\"repeated_keys_by_size\":";
  writeBuckets(out, r.repeated_keys_by_size);
  out << ",\"amplification_score\":" << r.amplification_score
      << ",\"wall_time_ms\":" << r.wall_time_ms
      << "}";
}

static void writePerVar(std::ostream &out, const PerVarSummary &s) {
  out << "{\"var\":" << s.var
      << ",\"score_min\":" << s.score_min
      << ",\"score_max\":" << s.score_max
      << ",\"lopsided\":" << (s.lopsided ? "true" : "false")
      << "}";
}

void writeJson(const std::vector<PerPolarityResult> &per_pol,
               const std::vector<PerVarSummary> &per_var,
               std::ostream &out) {
  out << "{\"per_polarity\":[";
  for (size_t i = 0; i < per_pol.size(); i++) {
    if (i) out << ",";
    writePerPol(out, per_pol[i]);
  }
  out << "],\"per_var\":[";
  for (size_t i = 0; i < per_var.size(); i++) {
    if (i) out << ",";
    writePerVar(out, per_var[i]);
  }
  out << "]}\n";
}

}  // namespace anchor_probe
