// Arjun integration for projected #SAT.
//
// Calls Arjun's standalone_minimize_indep() to extract:
//   - a simplified CNF (BVE + distillation + backbone + ...)
//   - an independent support set I
//   - a multiplier_weight (for vars Arjun eliminated as genuinely-free)
//
// Writes the simplified CNF to output_path in DIMACS form and a sidecar
// to sidecar_path with the indep set + multiplier. The sidecar format:
//
//     c arjun sidecar
//     indep <n_indep>
//     <var1>
//     <var2>
//     ...
//     multiplier <mpz-decimal>
//
// Main.cpp consumes the sidecar after the solver has loaded the
// simplified CNF and calls Solver::setArjunResult(indep, multiplier).
// See docs/scc_unsat_prune_plan.md → Phase D notes.

#ifndef PREPROCESSOR_ARJUN_H_
#define PREPROCESSOR_ARJUN_H_

#include <string>

class PreprocessorArjun {
 public:
  // Runs Arjun on input_path. Writes simplified CNF to output_path
  // (DIMACS, 1-indexed) and sidecar to sidecar_path.
  //
  // verbosity: 0=quiet, >0=arjun's verb level.
  //
  // all_indep: when true, asks Arjun NOT to shrink the independent
  // support (treats every var as part of I). Useful when we just want
  // Arjun's simplification without the projection-counting reduction.
  // Default false = let Arjun extract a minimal I.
  //
  // Returns true on success; false on I/O failure or Arjun error.
  static bool simplify(const std::string& input_path,
                       const std::string& output_path,
                       const std::string& sidecar_path,
                       unsigned verbosity = 0,
                       bool all_indep = false);
};

#endif  // PREPROCESSOR_ARJUN_H_
