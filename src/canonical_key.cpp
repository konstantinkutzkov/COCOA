/*
 * canonical_key.cpp
 *
 * Implementation of buildCanonicalKey.
 *
 * Phase B Steps 1-6:
 *   1. Singleton anonymization
 *   2. Polarity normalization
 *   3. Variable list removed (implied by clauses)
 *   4. WL iteration 0: structural variable labeling
 *   5. WL iteration 1: neighbor-label refinement (collision blocks only)
 *   6. Collision block handling: lexicographic minimum over
 *      within-block permutations and flip masks
 */

#include "canonical_key.h"
#include "component_types/component.h"
#include <functional>
#include <set>

using ClauseDescriptor = std::vector<int>;
using VarSignature = std::vector<ClauseDescriptor>;

// Maximum collision block size for exact enumeration
// Blocks larger than this fall back to deterministic tie-breaking
static const unsigned MAX_ENUM_BLOCK = 4;

// Build a canonical clause multiset from norm_clauses using the given
// canonical_id and flip maps. Returns a sorted vector of sorted clauses.
static std::vector<std::vector<int>> buildClauseMultiset(
    const std::vector<std::vector<int>> &norm_clauses,
    const std::unordered_map<unsigned, int> &canonical_id,
    const std::vector<bool> &extra_flip,
    unsigned max_var) {

  std::vector<std::vector<int>> result;
  for (const auto &nc : norm_clauses) {
    std::vector<int> canonical_lits;
    for (int lit : nc) {
      if (lit == SINGLETON_MARKER) {
        canonical_lits.push_back(SINGLETON_MARKER);
      } else {
        unsigned v = lit > 0 ? (unsigned)lit : (unsigned)(-lit);
        int cid = canonical_id.at(v);
        int canon_lit = lit > 0 ? cid : -cid;
        if (v < extra_flip.size() && extra_flip[v])
          canon_lit = -canon_lit;
        canonical_lits.push_back(canon_lit);
      }
    }
    std::sort(canonical_lits.begin(), canonical_lits.end(), litLess);
    result.push_back(std::move(canonical_lits));
  }
  std::sort(result.begin(), result.end());
  return result;
}

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

  // Build normalized clauses
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

  std::vector<unsigned> nonsingleton_vars;
  for (unsigned v : active_vars)
    if (!is_singleton[v])
      nonsingleton_vars.push_back(v);

  // ---------------------------------------------------------------
  // Step 4: WL iteration 0 — structural signatures
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
  // Step 5: WL iteration 1 — refine collision blocks
  // ---------------------------------------------------------------
  if (!collision_blocks.empty()) {
    std::set<unsigned> needs_refinement;
    for (const auto &block : collision_blocks)
      for (unsigned v : block)
        needs_refinement.insert(v);

    using RefinedSig = std::pair<VarSignature, std::vector<std::pair<int, int>>>;
    std::unordered_map<unsigned, RefinedSig> refined_signatures;

    for (unsigned v : needs_refinement) {
      std::vector<std::pair<int, int>> neighbor_labels;

      for (size_t ci = 0; ci < raw_clauses.size(); ci++) {
        const auto &raw = raw_clauses[ci];

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

        for (int lit : raw) {
          unsigned lv = lit > 0 ? (unsigned)lit : (unsigned)(-lit);
          if (lv == v || is_singleton[lv]) continue;
          neighbor_labels.push_back({canonical_id[lv], v_polarity});
        }
      }

      std::sort(neighbor_labels.begin(), neighbor_labels.end());
      refined_signatures[v] = {var_signatures[v], neighbor_labels};
    }

    // Re-identify collision blocks after refinement
    collision_blocks.clear();
    for (const auto &block_orig : collision_blocks) { /* already cleared */ }

    // Rebuild collision blocks from scratch using refined signatures
    // Group all refined variables by their refined signature
    std::vector<std::pair<RefinedSig, unsigned>> refined_pairs;
    for (unsigned v : needs_refinement)
      refined_pairs.push_back({refined_signatures[v], v});

    std::sort(refined_pairs.begin(), refined_pairs.end(),
      [](const std::pair<RefinedSig, unsigned> &a,
         const std::pair<RefinedSig, unsigned> &b) {
        return a.first < b.first;
      });

    // Re-assign IDs and find remaining collision blocks
    {
      size_t i = 0;
      while (i < refined_pairs.size()) {
        size_t j = i + 1;
        while (j < refined_pairs.size() && refined_pairs[j].first == refined_pairs[i].first)
          j++;

        // Find base ID for this group
        int base_id = canonical_id[refined_pairs[i].second];
        for (size_t k = i; k < j; k++)
          if (canonical_id[refined_pairs[k].second] < base_id)
            base_id = canonical_id[refined_pairs[k].second];

        if (j - i > 1) {
          std::vector<unsigned> block;
          for (size_t k = i; k < j; k++)
            block.push_back(refined_pairs[k].second);
          collision_blocks.push_back(block);
        }

        for (size_t k = i; k < j; k++)
          canonical_id[refined_pairs[k].second] = base_id + (int)(k - i);
        i = j;
      }
    }
  }

  // ---------------------------------------------------------------
  // Step 6: Collision block enumeration
  // ---------------------------------------------------------------
  // For small collision blocks, enumerate all within-block permutations
  // and flip masks for orientation-ambiguous variables.
  // Take the lexicographic minimum clause multiset as the canonical key.

  // Check if any collision blocks remain and are small enough to enumerate
  bool need_enumeration = false;
  for (const auto &block : collision_blocks)
    if (block.size() >= 2 && block.size() <= MAX_ENUM_BLOCK)
      need_enumeration = true;

  if (need_enumeration) {
    // Identify orientation-ambiguous variables in collision blocks
    // (pos_count == neg_count after initial normalization flip)
    std::vector<bool> is_ambiguous(max_var + 1, false);
    for (const auto &block : collision_blocks) {
      for (unsigned v : block) {
        // After flip, the effective counts are swapped if flip[v]
        int eff_pos = flip[v] ? neg_count[v] : pos_count[v];
        int eff_neg = flip[v] ? pos_count[v] : neg_count[v];
        if (eff_pos == eff_neg)
          is_ambiguous[v] = true;
      }
    }

    // Build the baseline clause multiset (no extra flips)
    std::vector<bool> no_extra_flip(max_var + 1, false);
    std::vector<std::vector<int>> best = buildClauseMultiset(
        norm_clauses, canonical_id, no_extra_flip, max_var);

    // For each collision block, enumerate permutations and flips
    // We process all blocks jointly: enumerate the Cartesian product
    // of all block permutations × flip masks

    // Collect all enumerable blocks
    struct EnumBlock {
      std::vector<unsigned> vars;    // original var IDs in this block
      std::vector<int> base_ids;     // their current canonical IDs
      std::vector<bool> ambiguous;   // which are orientation-ambiguous
    };
    std::vector<EnumBlock> enum_blocks;
    for (const auto &block : collision_blocks) {
      if (block.size() < 2 || block.size() > MAX_ENUM_BLOCK) continue;
      EnumBlock eb;
      eb.vars = block;
      for (unsigned v : block) {
        eb.base_ids.push_back(canonical_id[v]);
        eb.ambiguous.push_back(is_ambiguous[v]);
      }
      std::sort(eb.base_ids.begin(), eb.base_ids.end());
      enum_blocks.push_back(std::move(eb));
    }

    // Generate all permutations for each block and try them
    // For simplicity, enumerate one block at a time (independent blocks)
    for (const auto &eb : enum_blocks) {
      unsigned n = eb.vars.size();
      unsigned n_ambiguous = 0;
      for (bool a : eb.ambiguous) if (a) n_ambiguous++;

      // Generate all permutations of the block
      std::vector<unsigned> perm_indices(n);
      for (unsigned i = 0; i < n; i++) perm_indices[i] = i;

      do {
        // For each permutation, try all flip masks on ambiguous vars
        unsigned n_flip_combos = 1u << n_ambiguous;
        for (unsigned flip_mask = 0; flip_mask < n_flip_combos; flip_mask++) {

          // Apply this permutation + flip combination
          std::unordered_map<unsigned, int> trial_id = canonical_id;
          std::vector<bool> trial_flip(max_var + 1, false);

          unsigned ambig_idx = 0;
          for (unsigned i = 0; i < n; i++) {
            unsigned var = eb.vars[i];
            trial_id[var] = eb.base_ids[perm_indices[i]];
            if (eb.ambiguous[i]) {
              if (flip_mask & (1u << ambig_idx))
                trial_flip[var] = true;
              ambig_idx++;
            }
          }

          auto candidate = buildClauseMultiset(
              norm_clauses, trial_id, trial_flip, max_var);

          if (candidate < best) {
            best = candidate;
            // Update canonical_id and remember flips for the best
            for (unsigned i = 0; i < n; i++)
              canonical_id[eb.vars[i]] = trial_id[eb.vars[i]];
            // Store the flip in norm_clauses for final output
            for (unsigned i = 0; i < n; i++) {
              if (trial_flip[eb.vars[i]]) {
                // Apply additional flip to norm_clauses
                for (auto &nc : norm_clauses) {
                  for (int &lit : nc) {
                    if (lit == SINGLETON_MARKER) continue;
                    unsigned v = lit > 0 ? (unsigned)lit : (unsigned)(-lit);
                    if (v == eb.vars[i])
                      lit = -lit;
                  }
                }
              }
            }
          }
        }
      } while (std::next_permutation(perm_indices.begin(), perm_indices.end()));
    }

    // Use the best clause multiset directly as the key
    CanonicalKey key;
    key.clauses = std::move(best);
    key.computeHash();
    return key;
  }

  // ---------------------------------------------------------------
  // No enumeration needed: build key directly
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
