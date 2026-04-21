/*
 * canonical_key.cpp
 *
 * One-pass canonical key computation using tabulation hashing.
 *
 * 1. Scan clauses once: classify type, detect singletons, accumulate
 *    variable signatures via dictionary lookup
 * 2. Sort N signature values → assign canonical IDs
 * 3. Scan clauses again: compute component hash using canonical IDs
 *
 * No heap allocation per component. Reuses stack-allocated arrays.
 */

#include "canonical_key.h"
#include "component_types/component.h"

// Global clause type dictionary
ClauseTypeDictionary g_clause_type_dict;

// Reusable buffers to avoid heap allocation per call
static thread_local std::vector<unsigned> s_active_vars;
static thread_local std::vector<int> s_var_idx;
static thread_local std::vector<int> s_pos_count;
static thread_local std::vector<int> s_neg_count;
static thread_local std::vector<uint64_t> s_sig;
static thread_local std::vector<int> s_canonical_id;
static thread_local std::vector<std::pair<uint64_t, unsigned>> s_sig_pairs;

struct ClauseRef {
  ClauseOfs ofs;        // for long clauses
  unsigned var_a, var_b; // for binary clauses (var IDs)
  int lit_a, lit_b;     // for binary clauses (literal values)
  bool is_binary;
};
static thread_local std::vector<ClauseRef> s_clause_refs;

// Per-variable flags packed into a single byte array
// Bit 0: is_singleton, Bit 1: flip
static thread_local std::vector<uint8_t> s_var_flags;

CanonicalKey buildCanonicalKey(
    Component &comp,
    const std::vector<LiteralID> &literal_pool,
    const LiteralIndexedVector<Literal> &literals,
    const LiteralIndexedVector<TriValue> &literal_values,
    const std::vector<ClauseOfs> &clause_id_to_ofs,
    const std::unordered_map<ClauseOfs, unsigned> &removed_clauses,
    unsigned original_lit_pool_size) {

  // Collect active variables
  s_active_vars.clear();
  for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++)
    if (literal_values[LiteralID(*it, true)] == X_TRI)
      s_active_vars.push_back(*it);

  if (s_active_vars.empty()) return CanonicalKey();

  unsigned n_vars = s_active_vars.size();
  unsigned max_var = s_active_vars.back();

  // Resize and clear reusable buffers
  // var_idx: only clear entries we set (tracked via active_vars)
  if (s_var_idx.size() <= max_var) s_var_idx.resize(max_var + 1, -1);

  for (unsigned i = 0; i < n_vars; i++)
    s_var_idx[s_active_vars[i]] = i;

  // Per-variable arrays sized to n_vars
  s_pos_count.assign(n_vars, 0);
  s_neg_count.assign(n_vars, 0);

  s_clause_refs.clear();

  // Long clauses
  for (auto it = comp.clsBegin(); *it != clsSENTINEL; it++) {
    ClauseOfs ofs = clause_id_to_ofs[*it];
    if (removed_clauses.count(ofs)) continue;
    if (ofs >= original_lit_pool_size) continue;

    bool satisfied = false;
    unsigned active_count = 0;
    for (auto lt = literal_pool.begin() + ofs; *lt != SENTINEL_LIT; lt++) {
      if (literal_values[*lt] == T_TRI) { satisfied = true; break; }
      if (literal_values[*lt] == X_TRI) active_count++;
    }
    if (satisfied || active_count < 2) continue;

    ClauseRef ref;
    ref.ofs = ofs;
    ref.is_binary = false;
    s_clause_refs.push_back(ref);

    for (auto lt = literal_pool.begin() + ofs; *lt != SENTINEL_LIT; lt++) {
      if (literal_values[*lt] == X_TRI) {
        int idx = s_var_idx[lt->var()];
        if (idx >= 0) {
          if (lt->toInt() > 0) s_pos_count[idx]++;
          else s_neg_count[idx]++;
        }
      }
    }
  }

  // Binary clauses — use var_idx >= 0 to check component membership
  for (unsigned v : s_active_vars) {
    for (int sign = 0; sign <= 1; sign++) {
      LiteralID lit(v, sign == 0);
      unsigned orig_count = literals[lit].original_binary_link_count_;
      unsigned idx = 0;
      for (auto bt = literals[lit].binary_links_.begin();
           *bt != SENTINEL_LIT; bt++, idx++) {
        if (idx >= orig_count) break;
        unsigned other_var = bt->var();
        if (other_var <= v) continue;
        if (other_var > max_var || s_var_idx[other_var] < 0) continue;
        if (literal_values[*bt] == T_TRI) continue;
        if (literal_values[lit] == T_TRI) continue;

        int vi = s_var_idx[v];
        int oi = s_var_idx[other_var];

        if (lit.toInt() > 0) s_pos_count[vi]++;
        else s_neg_count[vi]++;
        if (bt->toInt() > 0) s_pos_count[oi]++;
        else s_neg_count[oi]++;

        ClauseRef ref;
        ref.is_binary = true;
        ref.var_a = v;
        ref.var_b = other_var;
        ref.lit_a = lit.toInt();
        ref.lit_b = bt->toInt();
        s_clause_refs.push_back(ref);
      }
    }
  }

  // Determine singletons and polarity flips (packed into flags byte)
  s_var_flags.assign(n_vars, 0);
  for (unsigned i = 0; i < n_vars; i++) {
    if (s_pos_count[i] + s_neg_count[i] == 1)
      s_var_flags[i] = 1;  // singleton
    else if (s_neg_count[i] > s_pos_count[i])
      s_var_flags[i] = 2;  // flip
  }

  // Pass 1b: Compute variable signatures via tabulation
  s_sig.assign(n_vars, 0);

  for (const auto &ref : s_clause_refs) {
    unsigned len = 0, np = 0, nn = 0, ns = 0;

    if (!ref.is_binary) {
      for (auto lt = literal_pool.begin() + ref.ofs; *lt != SENTINEL_LIT; lt++) {
        if (literal_values[*lt] != X_TRI) continue;
        int idx = s_var_idx[lt->var()];
        if (idx < 0) continue;
        len++;
        if (s_var_flags[idx] & 1) ns++;
        else {
          bool norm_pos = (lt->toInt() > 0) != bool(s_var_flags[idx] & 2);
          if (norm_pos) np++;
          else nn++;
        }
      }
    } else {
      int ia = s_var_idx[ref.var_a];
      int ib = s_var_idx[ref.var_b];
      len = 2;
      if (s_var_flags[ia] & 1) ns++;
      else {
        bool norm_pos = (ref.lit_a > 0) != bool(s_var_flags[ia] & 2);
        if (norm_pos) np++;
        else nn++;
      }
      if (s_var_flags[ib] & 1) ns++;
      else {
        bool norm_pos = (ref.lit_b > 0) != bool(s_var_flags[ib] & 2);
        if (norm_pos) np++;
        else nn++;
      }
    }

    ClauseType type = {len, np, nn, ns};
    auto hashes = g_clause_type_dict.lookup(type);

    // Accumulate signatures for non-singleton variables
    if (!ref.is_binary) {
      for (auto lt = literal_pool.begin() + ref.ofs; *lt != SENTINEL_LIT; lt++) {
        if (literal_values[*lt] != X_TRI) continue;
        int idx = s_var_idx[lt->var()];
        if (idx < 0 || (s_var_flags[idx] & 1)) continue;
        bool norm_pos = (lt->toInt() > 0) != bool(s_var_flags[idx] & 2);
        s_sig[idx] += norm_pos ? hashes.first : hashes.second;
      }
    } else {
      int ia = s_var_idx[ref.var_a];
      int ib = s_var_idx[ref.var_b];
      if (!(s_var_flags[ia] & 1)) {
        bool norm_pos = (ref.lit_a > 0) != bool(s_var_flags[ia] & 2);
        s_sig[ia] += norm_pos ? hashes.first : hashes.second;
      }
      if (!(s_var_flags[ib] & 1)) {
        bool norm_pos = (ref.lit_b > 0) != bool(s_var_flags[ib] & 2);
        s_sig[ib] += norm_pos ? hashes.first : hashes.second;
      }
    }
  }

  // Sort signatures → assign canonical IDs
  s_sig_pairs.clear();
  for (unsigned i = 0; i < n_vars; i++)
    if (!(s_var_flags[i] & 1))
      s_sig_pairs.push_back({s_sig[i], i});

  std::sort(s_sig_pairs.begin(), s_sig_pairs.end());

  s_canonical_id.assign(n_vars, 0);
  for (unsigned i = 0; i < s_sig_pairs.size(); i++)
    s_canonical_id[s_sig_pairs[i].second] = i + 1;

  // Pass 2: Compute component hash using canonical IDs AND build the
  // normalized clause multiset for structural equality.
  uint64_t component_hash = 0;
  unsigned total_clauses = 0;
  std::vector<std::vector<int>> canonical_clauses;
  canonical_clauses.reserve(s_clause_refs.size());

  for (const auto &ref : s_clause_refs) {
    int canon_buf[256];
    unsigned canon_len = 0;

    if (!ref.is_binary) {
      for (auto lt = literal_pool.begin() + ref.ofs; *lt != SENTINEL_LIT; lt++) {
        if (literal_values[*lt] != X_TRI) continue;
        int idx = s_var_idx[lt->var()];
        if (idx < 0) continue;
        int canon;
        if (s_var_flags[idx] & 1) {
          canon = 0;
        } else {
          int cid = s_canonical_id[idx];
          bool norm_pos = (lt->toInt() > 0) != bool(s_var_flags[idx] & 2);
          canon = norm_pos ? cid : -cid;
        }
        if (canon_len < 256) canon_buf[canon_len++] = canon;
      }
    } else {
      int ia = s_var_idx[ref.var_a];
      int ib = s_var_idx[ref.var_b];
      for (int idx : {ia, ib}) {
        int canon;
        int orig_lit = (idx == ia) ? ref.lit_a : ref.lit_b;
        if (s_var_flags[idx] & 1) {
          canon = 0;
        } else {
          int cid = s_canonical_id[idx];
          bool norm_pos = (orig_lit > 0) != bool(s_var_flags[idx] & 2);
          canon = norm_pos ? cid : -cid;
        }
        if (canon_len < 256) canon_buf[canon_len++] = canon;
      }
    }

    std::sort(canon_buf, canon_buf + canon_len);

    uint64_t ch = 14695981039346656037ULL;
    for (unsigned i = 0; i < canon_len; i++) {
      ch ^= (uint64_t)(unsigned)canon_buf[i];
      ch *= 1099511628211ULL;
    }
    ch ^= canon_len * 0x9E3779B97F4A7C15ULL;

    component_hash += ch;
    total_clauses++;

    // Store the canonical clause (sorted) for structural equality check.
    canonical_clauses.emplace_back(canon_buf, canon_buf + canon_len);
  }

  // Sort clauses lexicographically so the multiset has a canonical order.
  std::sort(canonical_clauses.begin(), canonical_clauses.end());

  // --- Sanity-check guards (runtime, always on) ------------------
  // Cheap representational invariants of the canonical form. A
  // violation here indicates a key-collision vulnerability: the
  // canonical form isn't canonical, so equivalent components would
  // get different keys, or non-equivalent components would get the
  // same key. We abort with diagnostic rather than debug-assert so
  // they fire in Release builds.
  const int n_nonsing = (int)s_sig_pairs.size();

  auto fire_guard = [](const char *msg,
                       const std::vector<int> *cl = nullptr) {
    std::cerr << "\n*** CANONICAL_KEY_INVARIANT: " << msg << " ***\n";
    if (cl) {
      std::cerr << "  offending clause:";
      for (int l : *cl) std::cerr << " " << l;
      std::cerr << "\n";
    }
    std::cerr.flush();
    std::abort();
  };

  for (const auto &cl : canonical_clauses) {
    // Literals within a clause must be sorted ascending.
    for (size_t i = 1; i < cl.size(); i++) {
      if (cl[i-1] > cl[i])
        fire_guard("canonical clause literals not sorted ascending", &cl);
    }
    // Canonical IDs must be 0 (singleton) or in [-n_nonsing, n_nonsing].
    for (int l : cl) {
      if (l != 0 && (std::abs(l) < 1 || std::abs(l) > n_nonsing))
        fire_guard("canonical literal ID out of valid range", &cl);
    }
    // A variable must appear at most once in a canonical clause
    // (singletons all canonicalise to 0 so multiple 0s are allowed).
    for (size_t i = 1; i < cl.size(); i++) {
      if (cl[i] == 0 && cl[i-1] == 0) continue;
      if (std::abs(cl[i]) == std::abs(cl[i-1]))
        fire_guard("canonical clause contains a variable twice", &cl);
    }
  }

  // The clause multiset must be sorted lexicographically.
  for (size_t i = 1; i < canonical_clauses.size(); i++) {
    if (canonical_clauses[i] < canonical_clauses[i-1])
      fire_guard("canonical_clauses not sorted lexicographically");
  }
  // -----------------------------------------------------------------

  // Clean up var_idx entries (reset only what we set)
  for (unsigned v : s_active_vars) s_var_idx[v] = -1;

  CanonicalKey key;
  key.hash = component_hash;
  key.num_vars = n_vars;
  key.num_clauses = total_clauses;
  key.clauses = std::move(canonical_clauses);
  return key;
}
