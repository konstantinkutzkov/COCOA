/*
 * anchor_probe.h
 *
 * Depth-bounded anchor probe — the Mechanism-3 (cache amplification)
 * probe targeting per-anchor-candidate ranking. Specified in
 * docs/anchor_probe_design.md.
 *
 * For a candidate variable v and polarity val, this probe:
 *   1. Pins v = val and runs BCP.
 *   2. Decomposes the residual into sub-components.
 *   3. Recursively branches both polarities of a chosen variable in
 *      each sub-component, to bounded depth K, with a path budget B.
 *   4. At each sub-component visited, builds its canonical key and
 *      adds it to a per-candidate ephemeral key-multiset.
 *   5. Aggregates into a score = Sum_k mult(k) * |comp(k)|^beta where
 *      mult(k) is the multiplicity (>= 1) of canonical key k and
 *      |comp(k)| is the active-var count of the corresponding
 *      sub-component.
 *
 * The probe does NOT consult or write the L1/L2 component cache. It
 * builds its own ephemeral hashmap and discards it on return. There
 * is no shared state across candidates — Q3 in the design doc.
 *
 * Output: per-(var, polarity) record + per-var summary, JSON.
 */

#ifndef ANCHOR_PROBE_H_
#define ANCHOR_PROBE_H_

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

#include "canonical_key.h"

namespace anchor_probe {

// Per-key entry in the ephemeral multiset.
//   multiplicity: number of times this canonical key was visited.
//   max_size:     largest active-var count seen for this key (under
//                 canonicalisation, identical keys correspond to the
//                 same canonical sub-formula, so size is a property of
//                 the key — but during enumeration we record max
//                 defensively in case of any drift).
struct KeyEntry {
  uint64_t multiplicity = 0;
  unsigned max_size = 0;
};

using KeyMap = std::unordered_map<CanonicalKey, KeyEntry, CanonicalKeyHash>;

// State threaded through the recursive enumerator.
//   keymap:           the ephemeral multiset built up across all
//                     sub-components visited during this candidate's
//                     probe (per-candidate; never shared).
//   budget_remaining: shared path budget across the whole candidate's
//                     enumeration. Each branch decision consumes one.
//   max_depth_used:   max recursion depth at which a key was added
//                     (for the "depth_used" output field).

struct EnumState {
  KeyMap keymap;
  int    budget_remaining = 0;
  int    max_depth_used   = 0;
};

// Bucketing of repeated-key sub-component sizes mirrors the existing
// L2_HIT_HIST buckets in solver.cpp end-of-run logging.
struct SizeBuckets {
  uint64_t small      = 0;   // |comp| <  25
  uint64_t medium     = 0;   //         < 100
  uint64_t large      = 0;   //         < 400
  uint64_t xlarge     = 0;   //         >= 400
};

enum class ProbeStatus {
  OK,             // probed successfully (may have explored 0 paths if
                  // no active vars remain after BCP).
  UNSAT_AT_PIN,   // pinning the anchor + BCP derived a conflict.
  BCP_CLOSED,     // BCP after pinning closed the formula entirely
                  // (every var assigned, no enumeration to do).
};

const char *statusName(ProbeStatus s);

struct PerPolarityResult {
  int       var = 0;
  bool      polarity = false;     // true = pinned v = T
  ProbeStatus status = ProbeStatus::OK;
  int       depth_used = 0;
  uint64_t  paths_explored = 0;
  uint64_t  total_keys = 0;       // total sub-components visited
  uint64_t  unique_keys = 0;      // distinct canonical keys seen
  uint64_t  repeated_keys = 0;    // distinct keys with multiplicity > 1
  uint64_t  repeat_count = 0;     // sum_k (mult(k) - 1) over repeated keys
  SizeBuckets repeated_keys_by_size;  // size bucket of repeated keys
  double    amplification_score = 0.0;
  double    wall_time_ms = 0.0;
};

struct PerVarSummary {
  int     var = 0;
  double  score_min = 0.0;        // min over polarities
  double  score_max = 0.0;
  bool    lopsided = false;       // ratio (max/min) above threshold
};

// Aggregate score formula:
//   score = sum over keys k with mult(k) > 1 of:
//             mult(k) * |comp(k)|^beta
// beta default 1.0. Higher beta emphasises large-component reuse.
//
// |comp(k)| is the *largest* active-var count seen for canonical-key k
// across all visits — under canonicalisation, all instances of the
// same key correspond to the same canonical sub-formula, so the size
// is a property of the key, not of the visit.
double computeAmplificationScore(uint64_t mult, unsigned comp_size,
                                 double beta);

// JSON serialisation. Writes one record per (var, polarity), then a
// per-var summary block.
void writeJson(const std::vector<PerPolarityResult> &per_pol,
               const std::vector<PerVarSummary> &per_var,
               std::ostream &out);

}  // namespace anchor_probe

#endif  // ANCHOR_PROBE_H_
