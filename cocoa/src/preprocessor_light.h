#ifndef PREPROCESSOR_LIGHT_H_
#define PREPROCESSOR_LIGHT_H_

#include <string>

// Drives CryptoMiniSat5's simplifier with a strategy restricted to
// passes that are provably *raw* model-count preserving:
//   - backbone (units forced by every model)
//   - SCC + binary-equivalence replacement (x ↔ ±y substitution)
//   - subsumption family (sub-impl, sub-cls-with-bin, sub-str-cls-with-bin,
//                         occ-backw-sub, occ-backw-sub-str)
//   - vivification / distillation (distill-cls, distill-bins,
//                                  distill-cls-onlyrem, full-probe,
//                                  intree-probe)
//   - cleanup (clean-cls, must-renumber)
//
// Explicitly NOT used: BVE (set_bve(0)), BVA (set_no_bva()), autarky,
// BCE, SBVA, gate-based variable elimination. These change the *raw*
// model count, so they are disabled (COCOA counts raw models).
//
// On success, writes the simplified CNF in DIMACS form to output_path
// and returns true. The simplified CNF's model count equals the input
// CNF's model count by construction.
//
// On UNSAT, writes "p cnf 0 1\n0\n" (one empty clause) and returns true.
//
// Returns false only on I/O or parse failure.
class PreprocessorLight {
 public:
  // Run no-SCC simplification and write the simplified CNF to output_path.
  // If equiv_sidecar_path is non-empty, then ALSO run a second CMS5 pass
  // with SCC detection enabled, read the equivalences via
  // get_all_binary_xors(), and write them to equiv_sidecar_path in the
  // format:
  //     c arjun-light equivalences (1-indexed vars, '+' = same polarity)
  //     <n>
  //     <var1> <var2> <+|->
  //     ...
  // The vars in the sidecar are in the SAME variable space as
  // output_path (i.e., the simplified CNF's vars after must-renumber).
  static bool simplify(const std::string& input_path,
                       const std::string& output_path,
                       unsigned verbosity = 0,
                       const std::string& equiv_sidecar_path = "");
};

#endif  // PREPROCESSOR_LIGHT_H_
