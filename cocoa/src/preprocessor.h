/*
 * preprocessor.h
 *
 * Standalone, #SAT-sound CNF preprocessor. Takes a CNF, applies a set
 * of model-set-preserving simplification rules to fixpoint, and emits
 * a simplified CNF + the list of literals forced to true at root.
 *
 * Soundness: every rule preserves the exact set of models of the
 * formula, so #SAT(F_in) == #SAT(F_out) conditioned on the forced
 * units being assigned. Rules implemented:
 *
 *   - BCP (unit propagation on a queue of forced literals).
 *   - Subsumption:            C ⊆ D  → delete D
 *   - Pure-duplicate resolution:
 *                             (ℓ ∨ K) ∧ (¬ℓ ∨ K) → (K)
 *     only when the residual K is bit-identical between the two.
 *   - Self-subsuming resolution (SSR):
 *                             resolve(C, D) = R,  R ⊆ D  →  replace D with R.
 *
 * Explicitly NOT implemented: bounded variable elimination (BVE). BVE
 * is sound for SAT but NOT for #SAT — it drops the eliminated
 * variable's freedom factor from the count.
 *
 * Module invariant: no dependency on Solver, Instance, literal_pool_,
 * watch lists, or any other solver-facing state. Operates exclusively
 * on its own internal data structures. The caller is responsible for
 * extracting the input CNF and rebuilding solver state from the output.
 */

#ifndef PREPROCESSOR_H_
#define PREPROCESSOR_H_

#include <vector>
#include <cstdint>

struct PreprocessorConfig {
  bool enable_subsumption     = true;
  bool enable_pure_duplicate  = true;
  bool enable_ssr             = true;
  // Wall-clock cap on the whole phase (milliseconds). When exceeded
  // mid-pass the current rule completes and the loop exits; the
  // partially-simplified formula is still sound.
  unsigned time_budget_ms     = 10000;
  bool verbose                = false;
};

struct PreprocessorResult {
  // Simplified CNF, each clause a sorted ascending vector of DIMACS
  // literal ints. Clauses of size >= 2 only — units are in
  // `forced_units` instead.
  std::vector<std::vector<int>> clauses;

  // Literals that must be true at root. Derived either from input
  // unit clauses or produced by rule applications that reduce a
  // clause to size 1.
  std::vector<int> forced_units;

  // Variable count — same as input, even if some variables no longer
  // appear in the output (they become trivially free when the search
  // later discovers them). The caller preserves the variable
  // numbering.
  unsigned num_vars = 0;

  // True iff the preprocessor derived a direct contradiction
  // (conflicting units or an empty resolvent). The caller should
  // treat the formula as UNSAT and return #SAT = 0.
  bool unsat = false;

  // Stats.
  unsigned long num_subsumed    = 0;
  unsigned long num_pure_dup    = 0;
  unsigned long num_ssr         = 0;
  unsigned long num_bcp_units   = 0;
  unsigned      passes          = 0;
  long          elapsed_ms      = 0;
};

// Run the preprocessor to fixpoint within the wall-clock budget.
// Soundness is preserved regardless of the budget outcome.
PreprocessorResult preprocess(
    unsigned n_vars,
    const std::vector<std::vector<int>> &clauses,
    const PreprocessorConfig &cfg = PreprocessorConfig());

#endif /* PREPROCESSOR_H_ */
