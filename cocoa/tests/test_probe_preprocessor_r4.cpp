/*
 * test_probe_preprocessor_r4.cpp
 *
 * Tests R4 (definitional elimination) — the cross-probe rule that
 * substitutes b ← a (or b ← ¬a) when F entails b ≡ a (or b ≡ ¬a).
 *
 * Tests:
 *   1. Direct equivalence b ↔ a → R4 fires, b eliminated, count match.
 *   2. Inverted equivalence b ↔ ¬a → R4 fires, b eliminated, count match.
 *   3. Both branches force same value (b=v in both) → NOT R4 territory;
 *      verify R4 does not eliminate b in this case (the diff-and-lift
 *      schema would handle it as a unit, but R4 itself ignores).
 *   4. Independent variables → R4 does nothing.
 *   5. Brute-force invariance: random small CNFs; check that
 *      #SAT(F) over n_vars == #SAT(F') over (n_vars \ eliminated).
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

// Brute-force #SAT over a specific set of active variables.
// `active_vars` is a sorted list of var IDs (1-indexed). We enumerate
// 2^|active_vars| assignments; vars NOT in active_vars are treated as
// the value 0 (their literals contribute nothing) — but in practice
// no clause should reference them after R4 has done its work.
static unsigned long brute_count_active(
    const std::vector<int> &active_vars,
    const std::vector<std::vector<int>> &clauses,
    const std::vector<int> &forced_units) {
  size_t k = active_vars.size();
  // Build a map from var_id → bit-position in `mask`.
  unsigned max_var = 0;
  for (int v : active_vars) if ((unsigned)v > max_var) max_var = v;
  for (const auto &C : clauses)
    for (int lit : C) if ((unsigned)std::abs(lit) > max_var)
      max_var = std::abs(lit);
  for (int u : forced_units) if ((unsigned)std::abs(u) > max_var)
    max_var = std::abs(u);
  std::vector<int> bit_of(max_var + 1, -1);
  for (size_t i = 0; i < k; i++) bit_of[active_vars[i]] = (int)i;

  unsigned long count = 0;
  for (unsigned long mask = 0; mask < (1UL << k); mask++) {
    bool sat = true;
    auto val_of = [&](int v) -> bool {
      int b = bit_of[v];
      // Vars not in active_vars must not appear in clauses; they
      // would otherwise be free, but for our test that's never the
      // case after R4.
      if (b < 0) return false;
      return ((mask >> b) & 1) != 0;
    };
    // Forced units must hold.
    for (int u : forced_units) {
      int v = std::abs(u);
      bool target = (u > 0);
      if (val_of(v) != target) { sat = false; break; }
    }
    if (!sat) continue;
    for (const auto &C : clauses) {
      bool clause_sat = false;
      for (int lit : C) {
        int v = std::abs(lit);
        bool target = (lit > 0);
        if (val_of(v) == target) { clause_sat = true; break; }
      }
      if (!clause_sat) { sat = false; break; }
    }
    if (sat) count++;
  }
  return count;
}

static void test_r4_direct_equivalence() {
  std::cout << "[test_r4_direct_equivalence]\n";
  // F = (¬a ∨ b) ∧ (a ∨ ¬b). Models: (0,0), (1,1) → #SAT = 2.
  // R4 should fire with v₀=0, v₁=1 (b ≡ a) and substitute b → a.
  std::vector<std::vector<int>> F = {{-1, 2}, {1, -2}};
  unsigned n = 2;
  R4Result R = runR4(n, F);
  check(!R.unsat, "F is satisfiable");
  check(R.num_eliminations >= 1, "at least one elimination fired");
  check(std::find(R.eliminated_vars.begin(), R.eliminated_vars.end(), 2)
            != R.eliminated_vars.end(),
        "variable 2 (b) was eliminated");
  // Active vars = {1, 2, ..., n} \ eliminated.
  std::set<int> elim(R.eliminated_vars.begin(), R.eliminated_vars.end());
  std::vector<int> actives;
  for (unsigned v = 1; v <= n; v++) if (!elim.count(v)) actives.push_back(v);
  unsigned long c_orig = brute_count_active({1, 2}, F, {});
  unsigned long c_new  = brute_count_active(actives, R.clauses, R.forced_units);
  std::cout << "    #SAT(F)={1,2}=" << c_orig
            << "  #SAT(F') over " << actives.size() << " active vars = "
            << c_new << "\n";
  check(c_orig == c_new, "#SAT(F) == #SAT(F') after R4");
}

static void test_r4_inverted_equivalence() {
  std::cout << "[test_r4_inverted_equivalence]\n";
  // F = (¬a ∨ ¬b) ∧ (a ∨ b). Models: (0,1), (1,0) → #SAT = 2.
  // F|{a=0}: forces b=1. F|{a=1}: forces b=0. (v0,v1)=(1,0) → b ≡ ¬a.
  std::vector<std::vector<int>> F = {{-1, -2}, {1, 2}};
  unsigned n = 2;
  R4Result R = runR4(n, F);
  check(!R.unsat, "F is satisfiable");
  check(R.num_eliminations >= 1, "at least one elimination fired");
  std::set<int> elim(R.eliminated_vars.begin(), R.eliminated_vars.end());
  std::vector<int> actives;
  for (unsigned v = 1; v <= n; v++) if (!elim.count(v)) actives.push_back(v);
  unsigned long c_orig = brute_count_active({1, 2}, F, {});
  unsigned long c_new  = brute_count_active(actives, R.clauses, R.forced_units);
  std::cout << "    #SAT(F)=" << c_orig << "  #SAT(F')=" << c_new << "\n";
  check(c_orig == c_new, "counts match");
}

static void test_r4_no_fire_same_value() {
  std::cout << "[test_r4_no_fire_same_value]\n";
  // F = (¬a ∨ b) ∧ (a ∨ b). Models where b=1 always (since (¬a∨b) and
  // (a∨b) together force b=1). So both branches force b=1. R4 case
  // (v₀=v₁=1) → R4 SHOULD NOT eliminate b (this is the "add unit"
  // case which the diff-and-lift schema handles).
  std::vector<std::vector<int>> F = {{-1, 2}, {1, 2}};
  R4Result R = runR4(2, F);
  check(R.num_eliminations == 0,
        "R4 does not eliminate when both branches force same value");
}

static void test_r4_independent_vars() {
  std::cout << "[test_r4_independent_vars]\n";
  // F = (a ∨ b) ∧ (¬c ∨ d). a and c are independent. R4 should not
  // find any (a, b, b≠a, v0≠v1) trigger.
  std::vector<std::vector<int>> F = {{1, 2}, {-3, 4}};
  R4Result R = runR4(4, F);
  check(R.num_eliminations == 0,
        "R4 does nothing on independent variable pairs");
}

static void test_r4_brute_force_invariance() {
  std::cout << "[test_r4_brute_force_invariance]\n";
  // Generate random small CNFs. For each, run R4 and verify
  // #SAT(F over n_vars) == #SAT(F' over (n_vars \ eliminated)).
  std::mt19937 rng(0xb0a710ad);
  unsigned n_cases = 30;
  unsigned n_vars = 6;
  unsigned n_clauses = 8;
  unsigned passes = 0;
  unsigned mismatches = 0;
  unsigned eliminations_total = 0;
  for (unsigned t = 0; t < n_cases; t++) {
    std::vector<std::vector<int>> F;
    for (unsigned i = 0; i < n_clauses; i++) {
      std::set<int> picked;
      while (picked.size() < 3) {
        int v = (int)(rng() % n_vars) + 1;
        int lit = (rng() & 1) ? v : -v;
        if (picked.count(-lit)) continue;
        picked.insert(lit);
      }
      F.push_back(std::vector<int>(picked.begin(), picked.end()));
    }
    R4Result R = runR4(n_vars, F);
    eliminations_total += R.num_eliminations;

    // Active = {1..n_vars} \ eliminated.
    std::set<int> elim(R.eliminated_vars.begin(), R.eliminated_vars.end());
    std::vector<int> all_vars, actives;
    for (unsigned v = 1; v <= n_vars; v++) {
      all_vars.push_back(v);
      if (!elim.count(v)) actives.push_back(v);
    }

    unsigned long c_orig = brute_count_active(all_vars, F, {});
    unsigned long c_new  = R.unsat ? 0 :
        brute_count_active(actives, R.clauses, R.forced_units);

    if (c_orig != c_new) {
      std::cerr << "  MISMATCH on case " << t
                << ": F count=" << c_orig << "  F' count=" << c_new
                << "  eliminated=" << R.eliminated_vars.size()
                << "  unsat=" << R.unsat << "\n";
      std::cerr << "    F: ";
      for (auto &c : F) {
        std::cerr << "(";
        for (size_t i = 0; i < c.size(); i++) std::cerr << (i?" ":"") << c[i];
        std::cerr << ") ";
      }
      std::cerr << "\n    F': ";
      for (auto &c : R.clauses) {
        std::cerr << "(";
        for (size_t i = 0; i < c.size(); i++) std::cerr << (i?" ":"") << c[i];
        std::cerr << ") ";
      }
      std::cerr << "\n    forced_units:";
      for (int u : R.forced_units) std::cerr << " " << u;
      std::cerr << "\n    eliminated:";
      for (int v : R.eliminated_vars) std::cerr << " " << v;
      std::cerr << "\n";
      mismatches++;
    } else {
      passes++;
    }
  }
  std::cout << "  R4 invariance: " << passes << "/" << n_cases
            << " passed, " << mismatches << " mismatches, "
            << eliminations_total << " total eliminations across cases\n";
  check(mismatches == 0, "brute-force #SAT invariance after R4 over 30 cases");
}

int main() {
  test_r4_direct_equivalence();
  test_r4_inverted_equivalence();
  test_r4_no_fire_same_value();
  test_r4_independent_vars();
  test_r4_brute_force_invariance();

  if (g_failed) {
    std::cerr << "\n" << g_failed << " test(s) FAILED\n";
    return 1;
  }
  std::cout << "\nAll R4 tests passed.\n";
  return 0;
}
