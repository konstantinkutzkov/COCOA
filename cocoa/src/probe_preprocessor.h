/*
 * probe_preprocessor.h
 *
 * Diff-and-lift harness for probe-based #SAT-sound preprocessing.
 *
 * Design (see docs/probe_preprocessing_plan.md):
 *
 *   For a partial assignment σ and a CNF F, compute F|σ, run the
 *   existing preprocessor A on F|σ, and expose the diff (pre vs post)
 *   so a caller can lift A's discoveries back to global learnings on F
 *   by prefixing them with ¬σ.
 *
 *   This file contains ONLY the harness (clone / apply σ / run A /
 *   diff). The lifting logic and the rules R1..R5 will be added in
 *   subsequent commits. No rules fire from this header alone.
 */

#ifndef PROBE_PREPROCESSOR_H_
#define PROBE_PREPROCESSOR_H_

#include <vector>
#include <cstdint>

#include "preprocessor.h"

// ---------------------------------------------------------------
// Partial assignment.
//
// A list of DIMACS literals (positive = true, negative = false). Each
// variable appears at most once. The harness does not validate this
// invariant; callers must.
// ---------------------------------------------------------------
using PartialAssignment = std::vector<int>;

// ---------------------------------------------------------------
// F|σ — the formula F reduced under the partial assignment σ.
//
// Construction: for each clause C in F:
//   - if any literal of C is satisfied by σ → drop the clause
//   - else: drop literals of C falsified by σ
//     - if the result is empty → unsat = true
//     - if the result has size 1 → add to forced_units
//     - else (size >= 2) → keep in clauses
//
// `forced_units` includes both σ's own literals and any units derived
// purely from σ-application (clauses reduced to size 1).
// ---------------------------------------------------------------
struct ReducedFormula {
  unsigned n_vars = 0;
  std::vector<std::vector<int>> clauses;   // size >= 2 each
  std::vector<int> forced_units;           // size 1 (post-σ)
  bool unsat = false;
};

ReducedFormula applyAssignment(
    unsigned n_vars,
    const std::vector<std::vector<int>> &clauses,
    const PartialAssignment &sigma);

// ---------------------------------------------------------------
// ProbeDiff — input to A and output of A on F|σ.
//
// Both `pre` and `post` carry size>=2 clauses + forced_units. `pre`
// is the immediate result of applyAssignment(F, σ). `post` is the
// PreprocessorResult from running A on `pre`. The fields are kept
// separate (not pre-merged) because the diff between them is exactly
// what the lift step (added in a later commit) needs to inspect.
//
// `unsat` is true iff EITHER the σ-application itself derived ⊥ OR A
// did. In both cases, the global lift is the disjunctive nogood ¬σ.
// ---------------------------------------------------------------
struct ProbeDiff {
  // pre = F|σ as produced by applyAssignment.
  std::vector<std::vector<int>> pre_clauses;
  std::vector<int> pre_forced_units;

  // post = A(F|σ) — comes from preprocessor::preprocess on `pre`.
  std::vector<std::vector<int>> post_clauses;
  std::vector<int> post_forced_units;

  bool unsat = false;

  // Forwarded stats (counts of operations A performed).
  unsigned long num_subsumed   = 0;
  unsigned long num_pure_dup   = 0;
  unsigned long num_ssr        = 0;
  unsigned long num_bcp_units  = 0;
  long          elapsed_ms     = 0;
};

// ---------------------------------------------------------------
// runProbe — the full harness step for a single σ.
//
//   1. Compute F|σ via applyAssignment.
//   2. If F|σ is already UNSAT → short-circuit (post = pre, unsat = true).
//   3. Otherwise run A on F|σ (with σ-derived units folded in as size-1
//      clauses on input).
//   4. Return both pre and post sides for caller-side diffing.
//
// The harness does NOT yet emit lifted clauses on F. That is the job
// of a future commit (the lift step).
// ---------------------------------------------------------------
ProbeDiff runProbe(
    unsigned n_vars,
    const std::vector<std::vector<int>> &clauses,
    const PartialAssignment &sigma,
    const PreprocessorConfig &cfg = PreprocessorConfig());

// ---------------------------------------------------------------
// LiftedLearnings — the global clauses to ADD to F as a result of a
// single probe σ.
//
// Each entry in `new_clauses` is a complete clause in DIMACS form,
// sorted ascending, ready to be appended to F. By the soundness
// theorem (§4 of docs/probe_preprocessing_plan.md), each such clause
// holds in every model of F, so adding it preserves #SAT(F).
//
// The lift rules (§3):
//   - For every unit ℓ in post.forced_units that is NOT in
//     pre.forced_units: add (¬σ ∨ ℓ).
//   - For every clause C in post.clauses that is NOT in pre.clauses:
//     add (¬σ ∨ C).
//   - If post.unsat: add ¬σ as a single clause (overrides all of the
//     above — F|σ has no models, so the disjunctive nogood is the
//     strongest learning).
//
// Subsumption / pure-duplicate operations on pre's clauses do not
// produce new entries directly; instead, any resolvent A places into
// post.clauses appears as "new in post" and is lifted via the second
// rule.
// ---------------------------------------------------------------
struct LiftedLearnings {
  std::vector<std::vector<int>> new_clauses;
};

LiftedLearnings liftProbe(const ProbeDiff &d, const PartialAssignment &sigma);

// ---------------------------------------------------------------
// Filter / apply step — only keep learnings that strictly shrink F.
//
// Given a candidate lifted clause L and the current formula F, three
// useful actions are possible (each strictly reduces total literal
// count of F):
//
//   (a) L is a unit: add to forced_units. (Shrinks via BCP.)
//
//   (b) L subsumes some D ∈ F (L ⊆ D as literal sets, |L| < |D|):
//       replace D with L. The replacement is sound because L holds
//       in every model of F (by our soundness theorem) and L ⊆ D
//       means D is implied by L. Net: |D| - |L| literals saved.
//
//   (c) L self-subsumes some D ∈ F via a literal ℓ:
//       ∃ ℓ ∈ L with ¬ℓ ∈ D, and L \ {ℓ} ⊆ D \ {¬ℓ}. Replace D
//       with D \ {¬ℓ}. Sound by case analysis: any model M of F
//       either has ℓ true (then ¬ℓ in D contributes nothing, so M
//       satisfies D iff M satisfies D \ {¬ℓ}) or ℓ false (then L
//       being satisfied by M forces some literal of L \ {ℓ} ⊆
//       D \ {¬ℓ} to be true). Net: 1 literal saved.
//
// Anything that does not match (a), (b), or (c) is DISCARDED — it
// would only enlarge F.
//
// `applyUsefulLearnings` mutates `clauses` and `forced_units` in
// place. Returns the count of applied operations (units + subs +
// ssr) — caller uses this to decide if any rule fired.
// ---------------------------------------------------------------
struct UsefulApplyStats {
  unsigned units_added       = 0;
  unsigned subsumptions      = 0;
  unsigned ssr_strengthenings = 0;
  unsigned discarded         = 0;
};

UsefulApplyStats applyUsefulLearnings(
    std::vector<std::vector<int>> &clauses,
    std::vector<int> &forced_units,
    const std::vector<std::vector<int>> &candidates);


// ---------------------------------------------------------------
// R4 (definitional elimination) — cross-probe rule.
//
// For each variable `a`, runs two probes σ = {a=0} and σ = {a=1}.
// If both branches are SAT and there exists a variable `b ≠ a` such
// that A forces b = v₀ under {a=0} and b = v₁ under {a=1} with
// v₀ ≠ v₁, we substitute b ← a (when (v₀, v₁) = (0, 1)) or b ← ¬a
// (when (1, 0)) throughout F and remove b from the variable set.
//
// Soundness: §5 of docs/probe_preprocessing_plan.md — every model M
// of F has b = v_{M(a)}, so projecting M to V \ {b} is a bijection
// onto models of the substituted formula.
//
// Cases:
//   - Both branches force b to the SAME value → NOT R4; instead the
//     diff-and-lift schema's BCP-unit-lift adds (b = v) to F. R4
//     ignores this case (it would produce a redundant elimination).
//   - One branch UNSAT → NOT R4; instead the lift adds the surviving
//     branch's σ as a unit (already handled by liftProbe). R4 returns
//     this as a special signal so the driver loop can re-run.
//   - Both branches UNSAT → F itself is unsat; R4Result.unsat = true.
//
// `eliminated_vars` lists each var b that was substituted away. The
// caller must not include these in any future #SAT enumeration over
// F'; their model multiplicity is captured implicitly by the
// definitional substitution (no factor of 2 to multiply back).
// ---------------------------------------------------------------
struct R4Result {
  unsigned n_vars = 0;                              // unchanged (var IDs preserved)
  std::vector<std::vector<int>> clauses;            // after substitutions, size >= 2
  std::vector<int> forced_units;                    // units added during R4
  std::vector<int> eliminated_vars;                 // vars dropped from active set
  bool unsat = false;
  // Stats
  unsigned long num_eliminations = 0;
  unsigned long num_units_added  = 0;
  unsigned long num_probes_run   = 0;
};

R4Result runR4(
    unsigned n_vars,
    const std::vector<std::vector<int>> &clauses,
    const PreprocessorConfig &cfg = PreprocessorConfig());

// ---------------------------------------------------------------
// Local-search probe-based preprocessing — the top-level driver.
//
// Runs a fixpoint loop of:
//   1. Sample up to `max_probes` short partial assignments σ
//      (depth 1, high-degree vars in short clauses first, random
//      polarity).
//   2. For each σ: runProbe + liftProbe → append new clauses to F
//      (subject to `max_total` and `max_size` caps).
//   3. If `enable_r4`: run R4 once. Eliminated variables are pinned
//      to false via a `(¬b)` forced unit so the downstream solver's
//      count does NOT include a phantom factor-of-2 for the now-
//      absent variable.
//   4. Mop up with the existing preprocessor A.
//   5. Repeat until no rule fires or the wall-clock budget elapses.
//
// Soundness contract: #SAT(F) = #SAT(output.clauses + output.forced_units)
// over the variables NOT in `output.eliminated_vars` (those vars are
// pinned to a fixed value by the appended units, so they contribute
// factor 1 to the count instead of factor 2).
// ---------------------------------------------------------------
struct LocalSearchPreprocessConfig {
  unsigned max_probes = 1000;       // total probes attempted
  unsigned max_size   = 4;          // max σ length (depth)
  unsigned max_total  = 5000;       // max clauses learned per call
  bool enable_r4      = true;
  unsigned budget_ms  = 10000;      // wall-clock cap
  bool verbose        = false;
  PreprocessorConfig preprocessor_cfg;
  unsigned seed       = 0;
};

struct LocalSearchPreprocessResult {
  unsigned n_vars = 0;
  std::vector<std::vector<int>> clauses;
  std::vector<int> forced_units;
  std::vector<int> eliminated_vars;
  bool unsat = false;
  // Stats.
  unsigned long num_probes_run        = 0;
  unsigned long num_units_added       = 0;     // case (a): forced units
  unsigned long num_subsumptions      = 0;     // case (b): D replaced by shorter L
  unsigned long num_ssr_strengthenings= 0;     // case (c): D shortened by 1 literal
  unsigned long num_eliminations      = 0;     // R4
  unsigned      passes                = 0;
  long          elapsed_ms            = 0;
  // Aggregate (sum of cases a + b + c) — kept for backward compatibility
  // and quick reporting; per-case fields above are authoritative.
  unsigned long num_clauses_added     = 0;
};

LocalSearchPreprocessResult runLocalSearchPreprocess(
    unsigned n_vars,
    const std::vector<std::vector<int>> &clauses,
    const LocalSearchPreprocessConfig &cfg = LocalSearchPreprocessConfig());

#endif /* PROBE_PREPROCESSOR_H_ */
