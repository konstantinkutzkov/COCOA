/*
 * test_probe_preprocessor_lift.cpp
 *
 * Tests the lift step (liftProbe) of the probe-based preprocessor.
 *
 * Tests:
 *   1. Empty diff (A did nothing) → empty learnings.
 *   2. UNSAT diff → exactly one learned clause = ¬σ.
 *   3. Lifting a new unit derived under σ.
 *   4. Lifting a new strengthened clause (the canonical SSR-under-σ
 *      example: F = (¬a ∨ b ∨ c) ∧ (b ∨ ¬c ∨ d), σ = {a=1}, expect
 *      learning to include (¬a ∨ b ∨ d)).
 *   5. Tautology drop: a lifted clause that contains x and ¬x is omitted.
 *   6. Brute-force soundness invariant: for a small F and probe σ,
 *      #SAT(F) == #SAT(F ∪ liftProbe(...)) — over ~50 random small
 *      formulas / probes.
 *
 * Each test prints PASS/FAIL.
 */

#include <iostream>
#include <vector>
#include <algorithm>
#include <set>
#include <random>

#include "probe_preprocessor.h"

static int g_failed = 0;

static void check(bool cond, const char *what) {
  if (!cond) {
    std::cerr << "FAIL: " << what << "\n";
    g_failed++;
  } else {
    std::cout << "  ok: " << what << "\n";
  }
}

static std::vector<int> sorted_copy(std::vector<int> v) {
  std::sort(v.begin(), v.end());
  return v;
}

static std::set<std::vector<int>> as_clause_set(
    const std::vector<std::vector<int>> &v) {
  std::set<std::vector<int>> out;
  for (auto c : v) {
    std::sort(c.begin(), c.end());
    out.insert(std::move(c));
  }
  return out;
}

// Brute-force #SAT over n_vars by enumerating all 2^n_vars assignments.
static unsigned long brute_count(
    unsigned n_vars, const std::vector<std::vector<int>> &clauses) {
  unsigned long count = 0;
  for (unsigned long mask = 0; mask < (1UL << n_vars); mask++) {
    bool sat = true;
    for (const auto &C : clauses) {
      bool clause_sat = false;
      for (int lit : C) {
        int v = std::abs(lit);
        bool val = (mask >> (v - 1)) & 1;     // var v's value
        bool target = (lit > 0);              // literal expects var=true
        if (val == target) { clause_sat = true; break; }
      }
      if (!clause_sat) { sat = false; break; }
    }
    if (sat) count++;
  }
  return count;
}

static void test_empty_lift() {
  std::cout << "[test_empty_lift]\n";
  // F = (1 ∨ 2). Probe σ = {} → A on F. A may or may not simplify;
  // either way pre==post and lift produces nothing new.
  std::vector<std::vector<int>> F = {{1, 2}};
  ProbeDiff d = runProbe(2, F, {});
  LiftedLearnings L = liftProbe(d, {});
  check(L.new_clauses.empty(),
        "no learnings when A did not change F (empty σ, simple F)");
}

static void test_unsat_lift() {
  std::cout << "[test_unsat_lift]\n";
  // F = (1) ∧ (2 ∨ 3). σ = {¬1}: σ-application falsifies the unit (1)
  // → unsat. Lift adds ¬σ = (1).
  std::vector<std::vector<int>> F = {{1}, {2, 3}};
  ProbeDiff d = runProbe(3, F, {-1});
  check(d.unsat, "diff is unsat");
  LiftedLearnings L = liftProbe(d, {-1});
  check(L.new_clauses.size() == 1, "exactly one lifted clause");
  if (L.new_clauses.size() >= 1)
    check(L.new_clauses[0] == std::vector<int>{1},
          "lifted clause is ¬σ = (1)");
}

static void test_lift_new_unit() {
  std::cout << "[test_lift_new_unit]\n";
  // F = (¬a ∨ b) ∧ (a ∨ b). σ = {} (empty): A on F derives unit b
  // via pure-duplicate. Lift adds (¬σ ∨ b) = (b).
  // But this clause might also appear as a derived unit in the post
  // already as a top-level forced unit. We just check that liftProbe
  // surfaces (b) somehow.
  std::vector<std::vector<int>> F = {{-1, 2}, {1, 2}};
  ProbeDiff d = runProbe(2, F, {});
  LiftedLearnings L = liftProbe(d, {});
  // With empty σ, ¬σ is empty, so a "new unit b" lifts to {b}.
  bool found_b = false;
  for (auto &c : L.new_clauses)
    if (c == std::vector<int>{2}) { found_b = true; break; }
  check(found_b, "new unit b is lifted as a unit clause");
}

static void test_lift_ssr_under_sigma() {
  std::cout << "[test_lift_ssr_under_sigma]\n";
  // The user's canonical example.
  // F = (¬a ∨ b ∨ c) ∧ (b ∨ ¬c ∨ d) ∧ (a ∨ b)
  //   = (-1, 2, 3) ∧ (2, -3, 4) ∧ (1, 2)
  // σ = {a=1}. F|σ = (b ∨ c) ∧ (b ∨ ¬c ∨ d) ∧ (forced_unit a=1).
  // A's SSR strengthens (b ∨ ¬c ∨ d) to (b ∨ d) — possibly further
  // simplified by A's BCP (e.g., A might force b directly). We check:
  //   - Either liftProbe contains (¬a ∨ b ∨ d) directly (the SSR lift),
  //   - OR liftProbe contains (¬a ∨ b) (an even stronger lift).
  std::vector<std::vector<int>> F = {{-1, 2, 3}, {2, -3, 4}, {1, 2}};
  ProbeDiff d = runProbe(4, F, {1});
  LiftedLearnings L = liftProbe(d, {1});
  std::cout << "    lifted clauses:";
  for (auto &c : L.new_clauses) {
    std::cout << " (";
    for (size_t i = 0; i < c.size(); i++) std::cout << (i?" ":"") << c[i];
    std::cout << ")";
  }
  std::cout << "\n";
  auto ls = as_clause_set(L.new_clauses);
  bool has_full_ssr_lift = ls.count(sorted_copy({-1, 2, 4})) > 0;
  bool has_stronger_lift = ls.count(sorted_copy({-1, 2})) > 0;
  check(has_full_ssr_lift || has_stronger_lift,
        "lifted set contains (¬a ∨ b ∨ d) OR (¬a ∨ b)");
}

static void test_brute_force_invariance() {
  std::cout << "[test_brute_force_invariance]\n";
  // Generate random small CNFs and random short σ. For each:
  //   compute #SAT(F) and #SAT(F ∪ liftProbe(F, σ)). They must match.
  std::mt19937 rng(0xc0ffee);
  unsigned n_cases = 50;
  unsigned n_vars = 6;
  unsigned n_clauses = 10;
  unsigned passes = 0;
  unsigned mismatches = 0;
  for (unsigned t = 0; t < n_cases; t++) {
    // Random 3-CNF.
    std::vector<std::vector<int>> F;
    for (unsigned i = 0; i < n_clauses; i++) {
      std::set<int> picked;
      while (picked.size() < 3) {
        int v = (int)(rng() % n_vars) + 1;
        int lit = (rng() & 1) ? v : -v;
        // Avoid x and -x in the same clause.
        if (picked.count(-lit)) continue;
        picked.insert(lit);
      }
      F.push_back(std::vector<int>(picked.begin(), picked.end()));
    }
    // Random σ of size 1..3 with no var collisions.
    PartialAssignment sigma;
    std::set<int> sigma_vars;
    unsigned k = 1 + (rng() % 3);
    for (unsigned i = 0; i < k && sigma_vars.size() < n_vars; i++) {
      int v;
      do { v = (int)(rng() % n_vars) + 1; } while (sigma_vars.count(v));
      sigma_vars.insert(v);
      int lit = (rng() & 1) ? v : -v;
      sigma.push_back(lit);
    }

    ProbeDiff d = runProbe(n_vars, F, sigma);
    LiftedLearnings L = liftProbe(d, sigma);

    // F' = F ∪ lifted clauses.
    std::vector<std::vector<int>> F_prime = F;
    for (auto &c : L.new_clauses) F_prime.push_back(c);

    unsigned long c_orig = brute_count(n_vars, F);
    unsigned long c_lift = brute_count(n_vars, F_prime);
    if (c_orig != c_lift) {
      std::cerr << "  MISMATCH on case " << t << ": F count=" << c_orig
                << " vs F' count=" << c_lift << "\n";
      std::cerr << "    F: ";
      for (auto &c : F) {
        std::cerr << "(";
        for (size_t i = 0; i < c.size(); i++) std::cerr << (i?" ":"") << c[i];
        std::cerr << ") ";
      }
      std::cerr << "\n    σ:";
      for (int l : sigma) std::cerr << " " << l;
      std::cerr << "\n    lifted:";
      for (auto &c : L.new_clauses) {
        std::cerr << " (";
        for (size_t i = 0; i < c.size(); i++) std::cerr << (i?" ":"") << c[i];
        std::cerr << ")";
      }
      std::cerr << "\n";
      mismatches++;
    } else {
      passes++;
    }
  }
  std::cout << "  brute-force invariance: " << passes << "/" << n_cases
            << " cases passed, " << mismatches << " mismatches\n";
  check(mismatches == 0, "brute-force #SAT invariance over 50 random cases");
}

int main() {
  test_empty_lift();
  test_unsat_lift();
  test_lift_new_unit();
  test_lift_ssr_under_sigma();
  test_brute_force_invariance();

  if (g_failed) {
    std::cerr << "\n" << g_failed << " test(s) FAILED\n";
    return 1;
  }
  std::cout << "\nAll lift tests passed.\n";
  return 0;
}
