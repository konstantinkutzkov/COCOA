/*
 * canonical_key.cpp — minimal version for speed testing
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

  CanonicalKey key;

  // Hash long clauses — raw active literals, sorted within each clause
  int buf[256];
  for (auto it = comp.clsBegin(); *it != clsSENTINEL; it++) {
    ClauseOfs ofs = clause_id_to_ofs[*it];
    if (removed_clauses.count(ofs)) continue;

    unsigned len = 0;
    bool satisfied = false;
    for (auto lt = literal_pool.begin() + ofs; *lt != SENTINEL_LIT; lt++) {
      if (literal_values[*lt] == T_TRI) { satisfied = true; break; }
      if (literal_values[*lt] == X_TRI && len < 256)
        buf[len++] = lt->toInt();
    }
    if (satisfied || len < 2) continue;
    std::sort(buf, buf + len);
    key.addClause(buf, len);
  }

  // Hash original binary clauses
  unsigned max_var = 0;
  std::vector<unsigned> active_vars;
  for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++) {
    if (literal_values[LiteralID(*it, true)] == X_TRI) {
      active_vars.push_back(*it);
      if (*it > max_var) max_var = *it;
    }
  }

  std::vector<bool> var_in_comp(max_var + 1, false);
  for (unsigned v : active_vars) var_in_comp[v] = true;

  for (unsigned v : active_vars) {
    for (int sign = 0; sign <= 1; sign++) {
      LiteralID lit(v, sign == 0);
      unsigned orig_count = literals[lit].original_binary_link_count_;
      unsigned idx = 0;
      for (auto bt = literals[lit].binary_links_.begin();
           *bt != SENTINEL_LIT; bt++, idx++) {
        if (idx >= orig_count) break;
        unsigned other_var = bt->var();
        if (other_var <= v) continue;
        if (other_var > max_var || !var_in_comp[other_var]) continue;
        if (literal_values[*bt] == T_TRI) continue;
        if (literal_values[lit] == T_TRI) continue;
        int bin[2] = {std::min(lit.toInt(), bt->toInt()),
                      std::max(lit.toInt(), bt->toInt())};
        key.addClause(bin, 2);
      }
    }
  }

  key.finalize();
  return key;
}
