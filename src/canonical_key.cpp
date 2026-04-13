/*
 * canonical_key.cpp
 *
 * Implementation of buildCanonicalKey.
 *
 * Phase B Steps 1-5:
 *   1. Singleton anonymization
 *   2. Polarity normalization
 *   3. Variable list removed (implied by clauses)
 *   4. WL iteration 0: structural variable labeling
 *   5. WL iteration 1: neighbor-label refinement (collision blocks only)
 */

#include "canonical_key.h"
#include "component_types/component.h"
#include <functional>
#include <set>

// A clause descriptor from the perspective of a variable:
// (clause_length, num_pos_nonsingleton, num_neg_nonsingleton, num_singletons, my_polarity)
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

  // ---------------------------------------------------------------
  // Step 1: Collect active variables
  // ---------------------------------------------------------------
  std::vector<unsigned> active_vars;
  for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++) {
    if (literal_values[LiteralID(*it, true)] == X_TRI)
      active_vars.push_back(*it);
  }

  unsigned max_var = active_vars.empty() ? 0 : active_vars.back();
  std::vector<bool> var_in_comp(max_var + 1, false);
  for (unsigned v : active_vars)
    var_in_comp[v] = true;

  // ---------------------------------------------------------------
  // Step 2: Collect all clauses as raw literal lists
  // ---------------------------------------------------------------
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

  // ---------------------------------------------------------------
  // Step 3: Singleton detection and polarity normalization
  // ---------------------------------------------------------------
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

  // Build normalized clauses (singletons replaced, polarity flipped)
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

  // ---------------------------------------------------------------
  // Step 4: WL iteration 0 — compute structural signatures
  // ---------------------------------------------------------------
  std::unordered_map<unsigned, VarSignature> var_signatures;
  for (unsigned v : nonsingleton_vars)
    var_signatures[v] = VarSignature();

  for (size_t ci = 0; ci < norm_clauses.size(); ci++) {
    const auto &nc = norm_clauses[ci];

    int clause_len = nc.size();
    int num_pos = 0, num_neg = 0, num_sing = 0;
    for (int lit : nc) {
      if (lit == SINGLETON_MARKER) num_sing++;
      else if (lit > 0) num_pos++;
      else num_neg++;
    }

    const auto &raw = raw_clauses[ci];
    for (size_t li = 0; li < raw.size(); li++) {
      int orig_lit = raw[li];
      unsigned v = orig_lit > 0 ? (unsigned)orig_lit : (unsigned)(-orig_lit);
      if (is_singleton[v]) continue;

      int norm_lit = flip[v] ? -orig_lit : orig_lit;
      int my_polarity = norm_lit > 0 ? 1 : -1;

      ClauseDescriptor desc = {clause_len, num_pos, num_neg, num_sing, my_polarity};
      var_signatures[v].push_back(desc);
    }
  }

  for (auto &vs : var_signatures)
    std::sort(vs.second.begin(), vs.second.end());

  // Sort variables by signature, assign initial canonical IDs
  std::vector<std::pair<VarSignature, unsigned>> sig_var_pairs;
  for (unsigned v : nonsingleton_vars)
    sig_var_pairs.push_back({var_signatures[v], v});

  std::sort(sig_var_pairs.begin(), sig_var_pairs.end(),
    [](const std::pair<VarSignature, unsigned> &a,
       const std::pair<VarSignature, unsigned> &b) {
      return a.first < b.first;
    });

  // Assign initial canonical IDs and identify collision blocks
  std::unordered_map<unsigned, int> canonical_id;
  std::vector<std::vector<unsigned>> collision_blocks;
  {
    int next_id = 1;
    size_t i = 0;
    while (i < sig_var_pairs.size()) {
      size_t j = i + 1;
      while (j < sig_var_pairs.size() && sig_var_pairs[j].first == sig_var_pairs[i].first)
        j++;

      if (j - i > 1) {
        // Collision block: multiple variables with same signature
        std::vector<unsigned> block;
        for (size_t k = i; k < j; k++)
          block.push_back(sig_var_pairs[k].second);
        collision_blocks.push_back(block);
      }

      for (size_t k = i; k < j; k++)
        canonical_id[sig_var_pairs[k].second] = next_id++;
      i = j;
    }
  }

  // ---------------------------------------------------------------
  // Step 5: WL iteration 1 — refine collision block variables only
  // ---------------------------------------------------------------
  if (!collision_blocks.empty()) {
    // Collect which variables need refinement
    std::set<unsigned> needs_refinement;
    for (const auto &block : collision_blocks)
      for (unsigned v : block)
        needs_refinement.insert(v);

    // For each variable in a collision block, compute a refined signature:
    // (original_signature, sorted multiset of neighbor canonical IDs with edge polarity)
    // A neighbor is a non-singleton variable that co-occurs in at least one clause.
    using RefinedSig = std::pair<VarSignature, std::vector<std::pair<int, int>>>;
    // The second element: sorted list of (neighbor_canonical_id, my_polarity_in_shared_clause)

    std::unordered_map<unsigned, RefinedSig> refined_signatures;

    for (unsigned v : needs_refinement) {
      std::vector<std::pair<int, int>> neighbor_labels;

      // Scan all clauses to find neighbors of v
      for (size_t ci = 0; ci < raw_clauses.size(); ci++) {
        const auto &raw = raw_clauses[ci];

        // Check if v is in this clause
        bool v_in_clause = false;
        int v_polarity = 0;
        for (int lit : raw) {
          unsigned lv = lit > 0 ? (unsigned)lit : (unsigned)(-lit);
          if (lv == v) {
            v_in_clause = true;
            int norm_lit = flip[v] ? -lit : lit;
            v_polarity = norm_lit > 0 ? 1 : -1;
            break;
          }
        }
        if (!v_in_clause) continue;

        // Collect canonical IDs of non-singleton neighbors in this clause
        for (int lit : raw) {
          unsigned lv = lit > 0 ? (unsigned)lit : (unsigned)(-lit);
          if (lv == v || is_singleton[lv]) continue;
          int neighbor_cid = canonical_id[lv];
          neighbor_labels.push_back({neighbor_cid, v_polarity});
        }
      }

      std::sort(neighbor_labels.begin(), neighbor_labels.end());
      refined_signatures[v] = {var_signatures[v], neighbor_labels};
    }

    // Re-sort within each collision block using refined signatures
    // and re-assign canonical IDs
    for (const auto &block : collision_blocks) {
      // Build (refined_sig, original_var) pairs for this block
      std::vector<std::pair<RefinedSig, unsigned>> block_pairs;
      for (unsigned v : block)
        block_pairs.push_back({refined_signatures[v], v});

      std::sort(block_pairs.begin(), block_pairs.end(),
        [](const std::pair<RefinedSig, unsigned> &a,
           const std::pair<RefinedSig, unsigned> &b) {
          return a.first < b.first;
        });

      // Re-assign canonical IDs for this block, starting from the
      // lowest ID in the block
      int base_id = canonical_id[block[0]];
      for (const auto &bp : block_pairs) {
        // Find the minimum current ID in the block
        if (canonical_id[bp.second] < base_id)
          base_id = canonical_id[bp.second];
      }
      for (size_t k = 0; k < block_pairs.size(); k++)
        canonical_id[block_pairs[k].second] = base_id + (int)k;
    }
  }

  // ---------------------------------------------------------------
  // Step 6: Build final canonical key with renamed variables
  // ---------------------------------------------------------------
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
