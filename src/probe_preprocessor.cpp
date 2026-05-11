/*
 * probe_preprocessor.cpp
 *
 * Implementation of the diff-and-lift harness — applyAssignment and
 * runProbe. No rules / no lift step yet.
 */

#include "probe_preprocessor.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <set>
#include <unordered_set>
#include <unordered_map>

ReducedFormula applyAssignment(
    unsigned n_vars,
    const std::vector<std::vector<int>> &clauses,
    const PartialAssignment &sigma) {

  ReducedFormula out;
  out.n_vars = n_vars;

  // sigma_lits: set of literals satisfied by σ (positive form).
  // sigma_neg : set of literal-values FALSIFIED by σ (= ¬σ_i).
  std::unordered_set<int> sigma_lits;
  std::unordered_set<int> sigma_neg;
  sigma_lits.reserve(sigma.size() * 2);
  sigma_neg.reserve(sigma.size() * 2);
  for (int l : sigma) {
    sigma_lits.insert(l);
    sigma_neg.insert(-l);
  }

  // σ's own literals seed forced_units.
  for (int l : sigma) out.forced_units.push_back(l);

  for (const auto &C : clauses) {
    bool satisfied = false;
    std::vector<int> reduced;
    reduced.reserve(C.size());
    for (int lit : C) {
      if (sigma_lits.count(lit)) { satisfied = true; break; }
      if (sigma_neg.count(lit)) continue;     // falsified literal — drop
      reduced.push_back(lit);
    }
    if (satisfied) continue;
    if (reduced.empty()) {
      out.unsat = true;
      // Keep scanning so we don't lose any other unit-detection state;
      // however the caller's contract is to short-circuit on unsat.
      continue;
    }
    if (reduced.size() == 1) {
      out.forced_units.push_back(reduced[0]);
      continue;
    }
    out.clauses.push_back(std::move(reduced));
  }
  return out;
}

ProbeDiff runProbe(
    unsigned n_vars,
    const std::vector<std::vector<int>> &clauses,
    const PartialAssignment &sigma,
    const PreprocessorConfig &cfg) {

  ProbeDiff d;

  ReducedFormula reduced = applyAssignment(n_vars, clauses, sigma);
  d.pre_clauses = reduced.clauses;
  d.pre_forced_units = reduced.forced_units;

  if (reduced.unsat) {
    d.unsat = true;
    d.post_clauses = d.pre_clauses;
    d.post_forced_units = d.pre_forced_units;
    return d;
  }

  // Build A's input: pre clauses (size >= 2) plus forced_units as
  // size-1 clauses. Preprocessor will then BCP-propagate the units
  // and apply the other rules.
  std::vector<std::vector<int>> a_input;
  a_input.reserve(reduced.clauses.size() + reduced.forced_units.size());
  for (const auto &C : reduced.clauses) a_input.push_back(C);
  for (int u : reduced.forced_units) a_input.push_back({u});

  PreprocessorResult R = preprocess(n_vars, a_input, cfg);

  d.post_clauses = R.clauses;
  d.post_forced_units = R.forced_units;
  d.unsat = R.unsat;
  d.num_subsumed  = R.num_subsumed;
  d.num_pure_dup  = R.num_pure_dup;
  d.num_ssr       = R.num_ssr;
  d.num_bcp_units = R.num_bcp_units;
  d.elapsed_ms    = R.elapsed_ms;

  return d;
}

// Sort a clause's literals ascending so it can be compared as a set
// against another sorted clause. Returns a copy.
static std::vector<int> sorted_clause(const std::vector<int> &c) {
  std::vector<int> out = c;
  std::sort(out.begin(), out.end());
  return out;
}

// True iff `sub` ⊆ `sup` (literal-set inclusion). Both must be sorted
// ascending and deduplicated.
static bool clause_subset(const std::vector<int> &sub,
                          const std::vector<int> &sup) {
  size_t i = 0, j = 0;
  while (i < sub.size() && j < sup.size()) {
    if (sub[i] == sup[j]) { i++; j++; }
    else if (sub[i] > sup[j]) { j++; }
    else return false;       // sub[i] not found in sup
  }
  return i == sub.size();
}

UsefulApplyStats applyUsefulLearnings(
    std::vector<std::vector<int>> &clauses,
    std::vector<int> &forced_units,
    const std::vector<std::vector<int>> &candidates) {

  UsefulApplyStats S;

  // ---- Working copies of F: each clause stored sorted ascending. ----
  std::vector<std::vector<int>> repl;
  repl.reserve(clauses.size());
  for (const auto &c : clauses) repl.push_back(sorted_clause(c));
  std::vector<bool> alive(repl.size(), true);

  std::set<int> forced_unit_set(forced_units.begin(), forced_units.end());

  // ---- Inverted index: literal → list of clause indices that contain it.
  // Indices into `repl`. We maintain it incrementally as clauses are
  // replaced or shortened — a literal removed from a clause is removed
  // from its index entry; a literal added is added. The index never
  // contains stale entries for `alive == false` clauses (we sweep
  // when removing).
  std::unordered_map<int, std::vector<size_t>> idx;
  auto add_to_idx = [&](size_t cidx, int lit) {
    idx[lit].push_back(cidx);
  };
  auto remove_from_idx = [&](size_t cidx, int lit) {
    auto it = idx.find(lit);
    if (it == idx.end()) return;
    auto &vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), cidx), vec.end());
  };

  for (size_t i = 0; i < repl.size(); i++)
    for (int lit : repl[i]) add_to_idx(i, lit);

  // Helper: replace clause i with new contents (assumed sorted, dedup'd,
  // size >= 2). Maintains index.
  auto replace_clause = [&](size_t i,
                            const std::vector<int> &old_lits,
                            const std::vector<int> &new_lits) {
    for (int l : old_lits) remove_from_idx(i, l);
    repl[i] = new_lits;
    for (int l : new_lits) add_to_idx(i, l);
  };
  auto kill_clause = [&](size_t i) {
    for (int l : repl[i]) remove_from_idx(i, l);
    alive[i] = false;
    repl[i].clear();
  };

  // Pick the literal of L with the fewest occurrences in F's index —
  // iterating its (small) clause list is the cheapest anchor.
  auto rarest_literal_in = [&](const std::vector<int> &L) -> int {
    int best = L.front();
    size_t best_count = SIZE_MAX;
    for (int l : L) {
      auto it = idx.find(l);
      size_t c = (it == idx.end()) ? 0 : it->second.size();
      if (c < best_count) { best_count = c; best = l; }
    }
    return best;
  };

  for (const auto &cand_in : candidates) {
    if (cand_in.empty()) { S.discarded++; continue; }

    std::vector<int> L = sorted_clause(cand_in);
    L.erase(std::unique(L.begin(), L.end()), L.end());

    // Tautology drop.
    bool taut = false;
    for (size_t i = 1; i < L.size(); i++)
      if (L[i] == -L[i-1]) { taut = true; break; }
    if (taut) { S.discarded++; continue; }

    // (a) unit case ---------------------------------------------------
    if (L.size() == 1) {
      int u = L[0];
      if (!forced_unit_set.count(u)) {
        forced_units.push_back(u);
        forced_unit_set.insert(u);
        S.units_added++;
      } else {
        S.discarded++;
      }
      continue;
    }

    // (b) subsumption -------------------------------------------------
    // Need D ∈ F with L ⊆ D and |L| < |D|. D must contain every literal
    // of L, including the rarest one. Iterate index[rarest] only.
    bool applied = false;
    {
      int anchor = rarest_literal_in(L);
      auto it = idx.find(anchor);
      if (it != idx.end()) {
        // Snapshot the candidate clause list (we may mutate idx during
        // the loop on success).
        std::vector<size_t> cand_clauses = it->second;
        for (size_t i : cand_clauses) {
          if (!alive[i]) continue;
          const auto &D = repl[i];
          if (D.size() <= L.size()) continue;
          if (clause_subset(L, D)) {
            replace_clause(i, D, L);
            S.subsumptions++;
            applied = true;
            break;
          }
        }
      }
    }
    if (applied) continue;

    // (c) SSR via L ---------------------------------------------------
    // For each ℓ ∈ L: D must contain ¬ℓ. Iterate index[¬ℓ] only.
    for (int ell : L) {
      if (applied) break;
      auto it = idx.find(-ell);
      if (it == idx.end()) continue;
      // Snapshot — mutation may follow on success.
      std::vector<size_t> cand_clauses = it->second;
      for (size_t i : cand_clauses) {
        if (!alive[i]) continue;
        auto &D = repl[i];
        if (D.size() <= 1) continue;
        // Build L \ {ℓ} and D \ {¬ℓ}.
        std::vector<int> L_minus;
        L_minus.reserve(L.size() - 1);
        for (int x : L) if (x != ell) L_minus.push_back(x);
        std::vector<int> D_minus;
        D_minus.reserve(D.size() - 1);
        for (int x : D) if (x != -ell) D_minus.push_back(x);
        if (!clause_subset(L_minus, D_minus)) continue;
        // Strengthen D → D_minus.
        if (D_minus.empty()) {
          // Empty clause — UNSAT signal.
          forced_units.push_back(0);    // sentinel
          kill_clause(i);
          S.ssr_strengthenings++;
          applied = true;
          break;
        }
        if (D_minus.size() == 1) {
          int u = D_minus[0];
          if (!forced_unit_set.count(u)) {
            forced_units.push_back(u);
            forced_unit_set.insert(u);
          }
          kill_clause(i);
          S.ssr_strengthenings++;
          applied = true;
          break;
        }
        replace_clause(i, D, D_minus);
        S.ssr_strengthenings++;
        applied = true;
        break;
      }
    }
    if (applied) continue;

    // No useful action — discard.
    S.discarded++;
  }

  // Commit: drop dead clauses and any size<2 leftovers.
  std::vector<std::vector<int>> committed;
  committed.reserve(repl.size());
  for (size_t i = 0; i < repl.size(); i++) {
    if (!alive[i]) continue;
    if (repl[i].size() < 2) continue;
    committed.push_back(std::move(repl[i]));
  }
  clauses = std::move(committed);

  return S;
}

LiftedLearnings liftProbe(const ProbeDiff &d, const PartialAssignment &sigma) {
  LiftedLearnings L;

  // ¬σ literals (each is the negation of a σ literal). Used as the
  // prefix for every lifted clause and as the standalone nogood when
  // post.unsat is true.
  std::vector<int> neg_sigma;
  neg_sigma.reserve(sigma.size());
  for (int l : sigma) neg_sigma.push_back(-l);
  std::sort(neg_sigma.begin(), neg_sigma.end());

  if (d.unsat) {
    // Disjunctive nogood: F ∧ σ is unsatisfiable (either σ-application
    // derived ⊥ directly, or A did). Therefore F ⇒ ¬σ. Add ¬σ as a
    // single clause. (If σ is empty, this is the empty clause — F
    // itself is unsat and the caller should treat that as a global
    // signal.)
    L.new_clauses.push_back(neg_sigma);
    return L;
  }

  // Build sorted-clause sets for pre to detect "what's new in post".
  std::set<std::vector<int>> pre_clause_set;
  for (const auto &c : d.pre_clauses)
    pre_clause_set.insert(sorted_clause(c));
  std::set<int> pre_unit_set(d.pre_forced_units.begin(),
                              d.pre_forced_units.end());

  // For each unit in post not in pre: lift to (¬σ ∨ ℓ).
  for (int u : d.post_forced_units) {
    if (pre_unit_set.count(u)) continue;
    std::vector<int> lifted = neg_sigma;
    lifted.push_back(u);
    std::sort(lifted.begin(), lifted.end());
    // Drop the lifted clause if it's a tautology (contains both x and -x).
    bool taut = false;
    for (size_t i = 1; i < lifted.size(); i++)
      if (lifted[i] == -lifted[i-1]) { taut = true; break; }
    if (taut) continue;
    // Drop duplicate literals (sigma can collide with the unit only
    // pathologically; defensive).
    lifted.erase(std::unique(lifted.begin(), lifted.end()), lifted.end());
    L.new_clauses.push_back(std::move(lifted));
  }

  // For each clause in post not in pre: lift to (¬σ ∨ C).
  for (const auto &c : d.post_clauses) {
    auto sc = sorted_clause(c);
    if (pre_clause_set.count(sc)) continue;
    std::vector<int> lifted;
    lifted.reserve(neg_sigma.size() + sc.size());
    lifted.insert(lifted.end(), neg_sigma.begin(), neg_sigma.end());
    lifted.insert(lifted.end(), sc.begin(), sc.end());
    std::sort(lifted.begin(), lifted.end());
    lifted.erase(std::unique(lifted.begin(), lifted.end()), lifted.end());
    bool taut = false;
    for (size_t i = 1; i < lifted.size(); i++)
      if (lifted[i] == -lifted[i-1]) { taut = true; break; }
    if (taut) continue;
    L.new_clauses.push_back(std::move(lifted));
  }

  return L;
}

// Substitute every appearance of variable `from_var` in `clauses` by
// the literal `to_lit`. (`to_lit > 0` means "+from_var → +to_lit and
// -from_var → -to_lit"; `to_lit < 0` means "+from_var → to_lit and
// -from_var → -to_lit".)
//
// Each rewritten clause is sorted, deduplicated, and dropped if
// tautological. Unit clauses go to `forced_units`. Empty clauses set
// `unsat`.
static void substituteVariable(
    int from_var,
    int to_lit,
    std::vector<std::vector<int>> &clauses,
    std::vector<int> &forced_units,
    bool &unsat) {

  std::vector<std::vector<int>> out;
  out.reserve(clauses.size());

  for (auto &C : clauses) {
    std::vector<int> rewritten;
    rewritten.reserve(C.size());
    for (int lit : C) {
      if (std::abs(lit) == from_var) {
        // +from_var maps to +to_lit ; -from_var maps to -to_lit.
        rewritten.push_back(lit > 0 ? to_lit : -to_lit);
      } else {
        rewritten.push_back(lit);
      }
    }
    std::sort(rewritten.begin(), rewritten.end());
    rewritten.erase(std::unique(rewritten.begin(), rewritten.end()),
                    rewritten.end());
    bool taut = false;
    for (size_t i = 1; i < rewritten.size(); i++)
      if (rewritten[i] == -rewritten[i-1]) { taut = true; break; }
    if (taut) continue;
    if (rewritten.empty()) {
      unsat = true;
      continue;
    }
    if (rewritten.size() == 1) {
      forced_units.push_back(rewritten[0]);
      continue;
    }
    out.push_back(std::move(rewritten));
  }
  clauses = std::move(out);
}

// Build the set of forced units from a ProbeDiff (combines pre and
// post — these are the literals known to be forced under σ at the end
// of A's processing).
static std::set<int> probe_forced_units(const ProbeDiff &d) {
  std::set<int> s;
  for (int u : d.pre_forced_units)  s.insert(u);
  for (int u : d.post_forced_units) s.insert(u);
  return s;
}

// Active-variable scan: which vars actually appear in any clause or
// forced unit?
static std::vector<int> active_vars_in(
    unsigned n_vars,
    const std::vector<std::vector<int>> &clauses,
    const std::vector<int> &forced_units) {
  std::vector<bool> seen(n_vars + 1, false);
  for (const auto &C : clauses)
    for (int lit : C) seen[std::abs(lit)] = true;
  for (int u : forced_units) seen[std::abs(u)] = true;
  std::vector<int> out;
  for (unsigned v = 1; v <= n_vars; v++) if (seen[v]) out.push_back(v);
  return out;
}

// Variable degree (#occurrences across all clauses).
static unsigned var_degree(
    int v, const std::vector<std::vector<int>> &clauses) {
  unsigned d = 0;
  for (const auto &C : clauses)
    for (int lit : C) if (std::abs(lit) == v) { d++; break; }
  return d;
}

R4Result runR4(
    unsigned n_vars,
    const std::vector<std::vector<int>> &clauses,
    const PreprocessorConfig &cfg) {

  R4Result R;
  R.n_vars = n_vars;
  R.clauses = clauses;

  std::set<int> eliminated_set;

  bool changed = true;
  while (changed) {
    changed = false;

    // Iterate over currently-active variables in highest-degree-first
    // order (preferring high-degree per the doc's heuristic).
    auto actives = active_vars_in(n_vars, R.clauses, R.forced_units);
    // Skip already-eliminated.
    actives.erase(
        std::remove_if(actives.begin(), actives.end(),
                       [&](int v) { return eliminated_set.count(v) > 0; }),
        actives.end());
    // Sort by degree descending.
    std::sort(actives.begin(), actives.end(),
              [&](int x, int y) {
                return var_degree(x, R.clauses) > var_degree(y, R.clauses);
              });

    for (int a : actives) {
      // Two probes: σ = {a=false} (lit -a) and σ = {a=true} (lit +a).
      ProbeDiff d0 = runProbe(n_vars, R.clauses, {-a}, cfg);
      ProbeDiff d1 = runProbe(n_vars, R.clauses, {+a}, cfg);
      R.num_probes_run += 2;

      if (d0.unsat && d1.unsat) {
        R.unsat = true;
        return R;
      }
      if (d0.unsat) {
        // a=false is impossible → force a=true.
        R.forced_units.push_back(+a);
        R.num_units_added++;
        // Apply the unit immediately by treating it as σ = {+a} on F:
        ProbeDiff applied = runProbe(n_vars, R.clauses, {+a}, cfg);
        if (applied.unsat) { R.unsat = true; return R; }
        R.clauses = applied.post_clauses;
        for (int u : applied.post_forced_units)
          if (std::abs(u) != a) R.forced_units.push_back(u);
        changed = true;
        break;
      }
      if (d1.unsat) {
        R.forced_units.push_back(-a);
        R.num_units_added++;
        ProbeDiff applied = runProbe(n_vars, R.clauses, {-a}, cfg);
        if (applied.unsat) { R.unsat = true; return R; }
        R.clauses = applied.post_clauses;
        for (int u : applied.post_forced_units)
          if (std::abs(u) != a) R.forced_units.push_back(u);
        changed = true;
        break;
      }

      // Both branches SAT. Look for a candidate b with v0 ≠ v1.
      auto u0 = probe_forced_units(d0);
      auto u1 = probe_forced_units(d1);

      int chosen_b = 0;
      int to_lit   = 0;     // what b is substituted with (a literal)

      for (int b : actives) {
        if (b == a) continue;
        // Check b's status in both branches.
        // v0: 1 if (+b) ∈ u0, 0 if (-b) ∈ u0, undefined otherwise.
        int v0 = (u0.count(+b) ? 1 : (u0.count(-b) ? 0 : -1));
        int v1 = (u1.count(+b) ? 1 : (u1.count(-b) ? 0 : -1));
        if (v0 < 0 || v1 < 0) continue;
        if (v0 == v1) continue;        // case is handled by lift, not R4
        // (v0, v1) = (0, 1): b=0 when a=0, b=1 when a=1 → b ≡ a
        // (v0, v1) = (1, 0): b=1 when a=0, b=0 when a=1 → b ≡ ¬a
        chosen_b = b;
        to_lit   = (v0 == 0 && v1 == 1) ? +a : -a;
        break;
      }
      if (chosen_b == 0) continue;     // try next variable a

      // Substitute b ← to_lit throughout F.
      bool unsat_after = false;
      substituteVariable(chosen_b, to_lit, R.clauses, R.forced_units, unsat_after);
      if (unsat_after) {
        R.unsat = true;
        return R;
      }
      eliminated_set.insert(chosen_b);
      R.num_eliminations++;
      changed = true;
      break;
    }
  }

  R.eliminated_vars.assign(eliminated_set.begin(), eliminated_set.end());
  std::sort(R.eliminated_vars.begin(), R.eliminated_vars.end());
  return R;
}

// ---------------------------------------------------------------
// runLocalSearchPreprocess — top-level driver.
// ---------------------------------------------------------------

// Score function for variable selection: higher score = pick first.
// We prefer variables that occur in many short clauses, since those
// tend to chain BCP cascades and expose SSR-under-σ opportunities.
static unsigned probeScore(int v, const std::vector<std::vector<int>> &F) {
  unsigned score = 0;
  for (const auto &C : F) {
    bool has_v = false;
    for (int lit : C) if (std::abs(lit) == v) { has_v = true; break; }
    if (!has_v) continue;
    // Weight: shorter clauses contribute more. binaries: 8x; ternaries: 4x;
    // length L: max(1, 16/L).
    unsigned w = 16u / std::max((unsigned)C.size(), 1u);
    if (w == 0) w = 1;
    score += w;
  }
  return score;
}

LocalSearchPreprocessResult runLocalSearchPreprocess(
    unsigned n_vars,
    const std::vector<std::vector<int>> &clauses_in,
    const LocalSearchPreprocessConfig &cfg) {

  using clock = std::chrono::steady_clock;
  auto t_start = clock::now();
  auto deadline = t_start + std::chrono::milliseconds(cfg.budget_ms);

  LocalSearchPreprocessResult Out;
  Out.n_vars = n_vars;
  Out.clauses = clauses_in;

  std::set<int> eliminated_set;
  std::set<std::vector<int>> learned_set;     // dedup across passes

  std::mt19937 rng(cfg.seed ? cfg.seed : 0xCAFEBABEu);

  // Helper: run A on (Out.clauses + forced_units-as-size-1) and absorb.
  auto mopUp = [&]() -> bool {
    std::vector<std::vector<int>> a_input = Out.clauses;
    for (int u : Out.forced_units) a_input.push_back({u});
    PreprocessorResult R = preprocess(n_vars, a_input, cfg.preprocessor_cfg);
    if (R.unsat) { Out.unsat = true; return false; }
    Out.clauses = R.clauses;
    Out.forced_units = R.forced_units;
    return true;
  };

  // Initial mop-up: ensure F is in canonical form before we start.
  if (!mopUp()) return Out;

  unsigned passes = 0;
  while (clock::now() < deadline) {
    passes++;
    bool fired = false;

    // ------ Probe-sampling step (R1, R2, R3-as-unit, R5-as-nogood) ------
    if (Out.num_probes_run < cfg.max_probes
        && Out.num_clauses_added < cfg.max_total) {

      // Build candidate variables ranked by score, descending.
      std::vector<int> candidates;
      for (unsigned v = 1; v <= n_vars; v++) {
        if (eliminated_set.count(v)) continue;
        // Skip vars already forced (no point probing).
        bool forced = false;
        for (int u : Out.forced_units)
          if ((unsigned)std::abs(u) == v) { forced = true; break; }
        if (forced) continue;
        if (probeScore(v, Out.clauses) == 0) continue;
        candidates.push_back(v);
      }
      std::sort(candidates.begin(), candidates.end(),
                [&](int x, int y) {
                  return probeScore(x, Out.clauses) > probeScore(y, Out.clauses);
                });

      for (int v : candidates) {
        if (clock::now() >= deadline) break;
        if (Out.num_probes_run >= cfg.max_probes) break;
        if (Out.num_clauses_added >= cfg.max_total) break;
        // Random polarity.
        int polarity = (rng() & 1) ? +1 : -1;
        PartialAssignment sigma{polarity * v};

        // Skip σ literals that overlap an eliminated var (paranoia).
        if (eliminated_set.count(v)) continue;

        ProbeDiff d = runProbe(n_vars, Out.clauses, sigma, cfg.preprocessor_cfg);
        Out.num_probes_run++;

        LiftedLearnings L = liftProbe(d, sigma);

        // Empty-clause => σ globally unsatisfiable. liftProbe puts an
        // empty vector in new_clauses for this case; check before
        // running the filter.
        bool any_empty = false;
        for (auto &c : L.new_clauses) if (c.empty()) { any_empty = true; break; }
        if (any_empty) {
          Out.unsat = true;
          Out.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              clock::now() - t_start).count();
          return Out;
        }

        // Apply the "must shrink" filter — only keeps unit-cases,
        // subsumptions, and SSR-strengthenings. Other candidates are
        // discarded.
        UsefulApplyStats S = applyUsefulLearnings(
            Out.clauses, Out.forced_units, L.new_clauses);
        unsigned applied =
            S.units_added + S.subsumptions + S.ssr_strengthenings;
        if (applied > 0) {
          Out.num_units_added         += S.units_added;
          Out.num_subsumptions        += S.subsumptions;
          Out.num_ssr_strengthenings  += S.ssr_strengthenings;
          Out.num_clauses_added       += applied;
          fired = true;
        }
      }
    }

    // Mop-up after probe-sampling, before R4 (R4's probes benefit from
    // having all the recently-learned clauses already absorbed).
    if (fired) {
      if (!mopUp()) {
        Out.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now() - t_start).count();
        return Out;
      }
    }

    // ------ R4 (definitional elimination), if enabled ------
    if (cfg.enable_r4 && clock::now() < deadline) {
      R4Result R = runR4(n_vars, Out.clauses, cfg.preprocessor_cfg);
      if (R.unsat) {
        Out.unsat = true;
        Out.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now() - t_start).count();
        return Out;
      }
      if (R.num_eliminations > 0) {
        Out.clauses = R.clauses;
        for (int u : R.forced_units) Out.forced_units.push_back(u);
        for (int b : R.eliminated_vars) {
          // Pin eliminated var b to false. The choice of polarity is
          // arbitrary — b no longer appears in any clause, so any value
          // satisfies. The pinning removes the factor-of-2 freedom from
          // the count.
          if (eliminated_set.insert(b).second) {
            Out.forced_units.push_back(-b);
            Out.eliminated_vars.push_back(b);
          }
        }
        Out.num_eliminations += R.num_eliminations;
        fired = true;
      }
    }

    // Final mop-up after R4.
    if (fired) {
      if (!mopUp()) {
        Out.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now() - t_start).count();
        return Out;
      }
    } else {
      // Nothing fired; we've reached fixpoint. Exit the outer loop.
      break;
    }
  }

  Out.passes = passes;
  std::sort(Out.eliminated_vars.begin(), Out.eliminated_vars.end());
  Out.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      clock::now() - t_start).count();

  if (cfg.verbose) {
    std::cerr << "lsp: passes=" << Out.passes
              << " probes=" << Out.num_probes_run
              << " units=" << Out.num_units_added
              << " subsume=" << Out.num_subsumptions
              << " ssr=" << Out.num_ssr_strengthenings
              << " elim=" << Out.num_eliminations
              << " elapsed_ms=" << Out.elapsed_ms << "\n";
  }
  return Out;
}
