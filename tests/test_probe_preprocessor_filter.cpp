/*
 * test_probe_preprocessor_filter.cpp
 *
 * Tests `applyUsefulLearnings` — the "must shrink" filter that
 * accepts only unit-cases, subsumptions, and SSR-strengthenings.
 */

#include <iostream>
#include <vector>
#include <algorithm>
#include <set>

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

static std::set<std::vector<int>> as_set(
    const std::vector<std::vector<int>> &v) {
  std::set<std::vector<int>> out;
  for (auto c : v) {
    std::sort(c.begin(), c.end());
    out.insert(std::move(c));
  }
  return out;
}

static void test_unit_kept() {
  std::cout << "[test_unit_kept]\n";
  std::vector<std::vector<int>> F = {{1, 2}, {-1, 3}};
  std::vector<int> units;
  std::vector<std::vector<int>> cands = {{2}};        // unit candidate
  UsefulApplyStats S = applyUsefulLearnings(F, units, cands);
  check(S.units_added == 1, "unit candidate accepted");
  check(units == std::vector<int>{2}, "unit added to forced_units");
  // F unchanged.
  check(as_set(F) == std::set<std::vector<int>>{{1, 2}, sorted_copy({-1, 3})},
        "F clauses unchanged after unit-only candidate");
}

static void test_subsumption_kept() {
  std::cout << "[test_subsumption_kept]\n";
  // F has D = (1, 2, 3). Candidate L = (1, 2) ⊂ D. L should replace D.
  std::vector<std::vector<int>> F = {{1, 2, 3}};
  std::vector<int> units;
  std::vector<std::vector<int>> cands = {{1, 2}};
  UsefulApplyStats S = applyUsefulLearnings(F, units, cands);
  check(S.subsumptions == 1, "subsumption accepted");
  check(as_set(F) == std::set<std::vector<int>>{{1, 2}}, "D replaced by L");
}

static void test_ssr_kept() {
  std::cout << "[test_ssr_kept]\n";
  // F has D = (1, 2, -3). Candidate L = (1, 3). Pick ℓ = 3 in L:
  // ¬ℓ = -3 ∈ D. L \ {3} = (1) ⊆ D \ {-3} = (1, 2). SSR fires:
  // D shrinks to (1, 2).
  std::vector<std::vector<int>> F = {{1, 2, -3}};
  std::vector<int> units;
  std::vector<std::vector<int>> cands = {{1, 3}};
  UsefulApplyStats S = applyUsefulLearnings(F, units, cands);
  check(S.ssr_strengthenings == 1, "SSR strengthening accepted");
  check(as_set(F) == std::set<std::vector<int>>{{1, 2}},
        "D strengthened to (1, 2)");
}

static void test_useless_discarded() {
  std::cout << "[test_useless_discarded]\n";
  // F = {(1, 2), (3, 4)}. Candidate L = (-1, -2, -3, -4). L doesn't
  // subsume anything; SSR doesn't fire on disjoint clauses. Discard.
  std::vector<std::vector<int>> F = {{1, 2}, {3, 4}};
  std::vector<int> units;
  std::vector<std::vector<int>> cands = {{-1, -2, -3, -4}};
  UsefulApplyStats S = applyUsefulLearnings(F, units, cands);
  check(S.discarded == 1, "useless candidate discarded");
  check(units.empty(), "no units added");
  check(as_set(F) == std::set<std::vector<int>>{
            sorted_copy({1, 2}), sorted_copy({3, 4})},
        "F unchanged");
}

static void test_user_ssr_under_sigma_example() {
  std::cout << "[test_user_ssr_under_sigma_example]\n";
  // The user's example: F = (¬a ∨ b ∨ c) ∧ (b ∨ ¬c ∨ d) ∧ (a ∨ b).
  // Lifted under σ={a=1}: candidate (¬a ∨ b ∨ d).
  // Under the strict filter, this candidate is LONGER than D = (b, ¬c, d).
  // It cannot subsume D (¬a ∉ D). Test SSR: ℓ = ¬a in L, ¬ℓ = a; is
  // a ∈ D? D = (b, ¬c, d). No. Try ℓ = b: ¬b ∈ D? No. Try ℓ = d:
  // ¬d ∈ D? No. So SSR doesn't fire either against (b, ¬c, d).
  // What about (a ∨ b)? L = (¬a, b, d), pick ℓ = ¬a, ¬ℓ = a, a ∈
  // (a, b)? Yes. L \ {¬a} = (b, d). D \ {a} = (b). (b, d) ⊆ (b)?
  // No (d not there). So SSR doesn't fire there either.
  // Conclusion: the user's canonical SSR-under-σ example is CORRECTLY
  // DISCARDED under the strict filter — confirms our analysis that
  // SSR-under-σ rarely shrinks F.
  std::vector<std::vector<int>> F = {sorted_copy({-1, 2, 3}),
                                     sorted_copy({2, -3, 4}),
                                     sorted_copy({1, 2})};
  std::vector<int> units;
  std::vector<std::vector<int>> cands = {sorted_copy({-1, 2, 4})};
  UsefulApplyStats S = applyUsefulLearnings(F, units, cands);
  check(S.discarded == 1,
        "(-a, b, d) lifted candidate is discarded (no shrinkage)");
  check(S.units_added == 0 && S.subsumptions == 0 &&
        S.ssr_strengthenings == 0,
        "no useful action triggered");
  check(F.size() == 3, "F unchanged");
}

int main() {
  test_unit_kept();
  test_subsumption_kept();
  test_ssr_kept();
  test_useless_discarded();
  test_user_ssr_under_sigma_example();

  if (g_failed) {
    std::cerr << "\n" << g_failed << " test(s) FAILED\n";
    return 1;
  }
  std::cout << "\nAll filter tests passed.\n";
  return 0;
}
