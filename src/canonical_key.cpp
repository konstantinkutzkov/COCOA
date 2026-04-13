/*
 * canonical_key.cpp
 *
 * Implementation of buildCanonicalKey.
 *
 * Phase B Steps 1-4:
 *   1. Singleton anonymization
 *   2. Polarity normalization
 *   3. Variable list removed (implied by clauses)
 *   4. WL iteration 0: structural variable labeling and canonical renaming
 */

#include "canonical_key.h"
#include "component_types/component.h"
#include <functional>

// A clause descriptor from the perspective of a variable:
// (clause_length, num_pos_nonsingleton, num_neg_nonsingleton, num_singletons, my_polarity)
// where my_polarity is +1 or -1 after normalization
using ClauseDescriptor = std::vector<int>;

// A variable's structural signature: sorted list of clause descriptors
using VarSignature = std::vector<ClauseDescriptor>;

CanonicalKey buildCanonicalKey(
    Component &comp,
    const std::vector<LiteralID> &literal_pool,
    const LiteralIndexedVector<Literal> &literals,
    const LiteralIndexedVector<TriValue> &literal_values,
    const std::vector<ClauseOfs> &clause_id_to_ofs,
    const std::unordered_map<ClauseOfs, unsigned> &removed_clauses,
    unsigned original_lit_pool_size) {

  // Step 1: Collect active variables
  std::vector<unsigned> active_vars;
  for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++) {
    if (literal_values[LiteralID(*it, true)] == X_TRI)
      active_vars.push_back(*it);
  }

  unsigned max_var = active_vars.empty() ? 0 : active_vars.back();
  std::vector<bool> var_in_comp(max_var + 1, false);
  for (unsigned v : active_vars)
    var_in_comp[v] = true;

  // Step 2: Collect all clauses as raw literal lists
  std::vector<std::vector<int>> raw_clauses;

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

    raw_clauses.push_back(std::move(lits));
  }

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

        raw_clauses.push_back({lit.toInt(), bt->toInt()});
      }
    }
  }

  // Step 3: Count occurrences for singleton detection and polarity
  std::unordered_map<unsigned, int> pos_count, neg_count;
  for (unsigned v : active_vars) {
    pos_count[v] = 0;
    neg_count[v] = 0;
  }

  for (const auto &cl : raw_clauses) {
    for (int lit : cl) {
      unsigned v = lit > 0 ? (unsigned)lit : (unsigned)(-lit);
      if (lit > 0) pos_count[v]++;
      else neg_count[v]++;
    }
  }

  std::vector<bool> is_singleton(max_var + 1, false);
  for (unsigned v : active_vars)
    if (pos_count[v] + neg_count[v] == 1)
      is_singleton[v] = true;

  std::vector<bool> flip(max_var + 1, false);
  for (unsigned v : active_vars)
    if (!is_singleton[v] && neg_count[v] > pos_count[v])
      flip[v] = true;

  // Step 4: WL iteration 0 — compute structural signatures
  // First, build normalized clauses (singletons replaced, polarity flipped)
  // so we can compute clause descriptors
  std::vector<std::vector<int>> norm_clauses;
  for (const auto &cl : raw_clauses) {
    std::vector<int> norm;
    for (int lit : cl) {
      unsigned v = lit > 0 ? (unsigned)lit : (unsigned)(-lit);
      if (is_singleton[v])
        norm.push_back(SINGLETON_MARKER);
      else
        norm.push_back(flip[v] ? -lit : lit);
    }
    norm_clauses.push_back(std::move(norm));
  }

  // Collect non-singleton variables
  std::vector<unsigned> nonsingleton_vars;
  for (unsigned v : active_vars)
    if (!is_singleton[v])
      nonsingleton_vars.push_back(v);

  // For each non-singleton variable, compute its signature:
  // sorted list of clause descriptors
  std::unordered_map<unsigned, VarSignature> var_signatures;
  for (unsigned v : nonsingleton_vars)
    var_signatures[v] = VarSignature();

  for (size_t ci = 0; ci < norm_clauses.size(); ci++) {
    const auto &nc = norm_clauses[ci];

    // Compute clause-level stats
    int clause_len = nc.size();
    int num_pos = 0, num_neg = 0, num_sing = 0;
    for (int lit : nc) {
      if (lit == SINGLETON_MARKER) num_sing++;
      else if (lit > 0) num_pos++;
      else num_neg++;
    }

    // For each non-singleton variable in this clause, record descriptor
    const auto &raw = raw_clauses[ci];
    for (size_t li = 0; li < raw.size(); li++) {
      int orig_lit = raw[li];
      unsigned v = orig_lit > 0 ? (unsigned)orig_lit : (unsigned)(-orig_lit);
      if (is_singleton[v]) continue;

      // Polarity of v in this clause after normalization
      int norm_lit = flip[v] ? -orig_lit : orig_lit;
      int my_polarity = norm_lit > 0 ? 1 : -1;

      ClauseDescriptor desc = {clause_len, num_pos, num_neg, num_sing, my_polarity};
      var_signatures[v].push_back(desc);
    }
  }

  // Sort each variable's signature (multiset of descriptors)
  for (auto &vs : var_signatures)
    std::sort(vs.second.begin(), vs.second.end());

  // Assign canonical IDs: sort variables by signature, assign 1, 2, 3...
  std::vector<std::pair<VarSignature, unsigned>> sig_var_pairs;
  for (unsigned v : nonsingleton_vars)
    sig_var_pairs.push_back({var_signatures[v], v});

  std::sort(sig_var_pairs.begin(), sig_var_pairs.end(),
    [](const std::pair<VarSignature, unsigned> &a,
       const std::pair<VarSignature, unsigned> &b) {
      return a.first < b.first;
    });

  // Map original variable ID -> canonical ID
  std::unordered_map<unsigned, int> canonical_id;
  int next_id = 1;
  for (const auto &sv : sig_var_pairs)
    canonical_id[sv.second] = next_id++;

  // Step 5: Build final canonical key with renamed variables
  CanonicalKey key;

  for (const auto &nc : norm_clauses) {
    std::vector<int> canonical_lits;
    for (int lit : nc) {
      if (lit == SINGLETON_MARKER) {
        canonical_lits.push_back(SINGLETON_MARKER);
      } else {
        unsigned v = lit > 0 ? (unsigned)lit : (unsigned)(-lit);
        int cid = canonical_id[v];
        canonical_lits.push_back(lit > 0 ? cid : -cid);
      }
    }
    std::sort(canonical_lits.begin(), canonical_lits.end(), litLess);
    key.clauses.push_back(std::move(canonical_lits));
  }

  std::sort(key.clauses.begin(), key.clauses.end());
  key.computeHash();

  return key;
}
