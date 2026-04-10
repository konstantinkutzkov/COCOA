/*
 * canonical_key.cpp
 *
 * Implementation of buildCanonicalKey.
 *
 * Phase A: content-based key with variable list, clause content,
 * and binary clauses. No WL-based canonicalization yet.
 *
 * TODO Phase B: add singleton anonymization, polarity normalization,
 * and WL-based variable renaming.
 */

#include "canonical_key.h"
#include "component_types/component.h"

CanonicalKey buildCanonicalKey(
    Component &comp,
    const std::vector<LiteralID> &literal_pool,
    const LiteralIndexedVector<Literal> &literals,
    const LiteralIndexedVector<TriValue> &literal_values,
    const std::vector<ClauseOfs> &clause_id_to_ofs,
    const std::unordered_map<ClauseOfs, unsigned> &removed_clauses,
    unsigned original_lit_pool_size) {

  // Collect active variables
  std::vector<unsigned> active_vars;
  for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++) {
    if (literal_values[LiteralID(*it, true)] == X_TRI)
      active_vars.push_back(*it);
  }

  unsigned max_var = active_vars.empty() ? 0 : active_vars.back();
  std::vector<bool> var_in_comp(max_var + 1, false);
  for (unsigned v : active_vars)
    var_in_comp[v] = true;

  CanonicalKey key;

  // Include variable list in the key (distinguishes components with
  // same clauses but different free variables)
  std::vector<int> var_entry;
  var_entry.push_back(-999999);  // marker to separate from clauses
  for (unsigned v : active_vars)
    var_entry.push_back((int)v);
  key.clauses.push_back(std::move(var_entry));

  // Collect long clauses (not removed, not satisfied)
  for (auto it = comp.clsBegin(); *it != clsSENTINEL; it++) {
    ClauseOfs ofs = clause_id_to_ofs[*it];
    if (removed_clauses.count(ofs))
      continue;

    std::vector<int> lits;
    bool satisfied = false;
    for (auto lt = literal_pool.begin() + ofs; *lt != SENTINEL_LIT; lt++) {
      if (literal_values[*lt] == T_TRI) { satisfied = true; break; }
      if (literal_values[*lt] == X_TRI)
        lits.push_back(lt->toInt());
    }
    if (satisfied || lits.size() < 2)
      continue;

    std::sort(lits.begin(), lits.end(), litLess);
    key.clauses.push_back(std::move(lits));
  }

  // Collect binary clauses between active variables
  for (unsigned v : active_vars) {
    for (int sign = 0; sign <= 1; sign++) {
      LiteralID lit(v, sign == 0);
      for (auto bt = literals[lit].binary_links_.begin();
           *bt != SENTINEL_LIT; bt++) {
        unsigned other_var = bt->var();
        if (other_var <= v) continue;
        if (other_var > max_var || !var_in_comp[other_var]) continue;
        if (literal_values[*bt] == T_TRI) continue;
        if (literal_values[lit] == T_TRI) continue;

        std::vector<int> bin_cl = {lit.toInt(), bt->toInt()};
        std::sort(bin_cl.begin(), bin_cl.end(), litLess);
        key.clauses.push_back(std::move(bin_cl));
      }
    }
  }

  // Sort all clauses lexicographically
  std::sort(key.clauses.begin(), key.clauses.end());

  // Compute hash
  key.computeHash();

  return key;
}
