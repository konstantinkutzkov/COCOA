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

// Global canon stats
CanonStats g_canon_stats;

// Reusable buffers to avoid heap allocation per call.
//
// PERFORMANCE NOTE — single-threaded only.
// These were originally `static thread_local`. Each access to a
// thread_local variable goes through `_tlv_get_addr` (~5-10 ns), and
// at 95M+ calls/instance the cumulative cost was measured at ~10% of
// total CPU on t1_049 via macOS `sample`. Plain `static` compiles to
// a direct load — same semantics for a single-threaded solver, no
// per-access runtime call.
//
// IMPORTANT: if the solver ever becomes multi-threaded (per-thread
// Counter as in Ganak's OuterCounter), these MUST be reverted to
// `thread_local` to avoid data races. Each thread would race on
// these shared buffers and produce wrong canonical keys silently.
static std::vector<unsigned> s_active_vars;
static std::vector<int> s_var_idx;
static std::vector<int> s_pos_count;
static std::vector<int> s_neg_count;
static std::vector<uint64_t> s_sig;
static std::vector<int> s_canonical_id;
static std::vector<std::pair<uint64_t, unsigned>> s_sig_pairs;

struct ClauseRef {
  ClauseOfs ofs;        // for long clauses
  unsigned var_a, var_b; // for binary clauses (var IDs)
  int lit_a, lit_b;     // for binary clauses (literal values)
  bool is_binary;
};
static std::vector<ClauseRef> s_clause_refs;  // see note above re: thread_local

// Per-variable flags packed into a single byte array
// Bit 0: is_singleton, Bit 1: flip
static std::vector<uint8_t> s_var_flags;  // see note above re: thread_local

CanonicalKey buildCanonicalKey(
    Component &comp,
    const std::vector<LiteralID> &literal_pool,
    const LiteralIndexedVector<Literal> &literals,
    const LiteralIndexedVector<TriValue> &literal_values,
    const std::vector<ClauseOfs> &clause_id_to_ofs,
    const std::unordered_map<ClauseOfs, unsigned> &removed_clauses,
    unsigned original_lit_pool_size,
    bool compact,
    bool no_anonymization,
    int wl_iterations,
    const std::vector<uint64_t> *static_wl_labels) {

  // Collect active variables
  s_active_vars.clear();
  for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++)
    if (literal_values[LiteralID(*it, true)] == X_TRI)
      s_active_vars.push_back(*it);

  if (s_active_vars.empty()) {
    CanonicalKey k;
    k.compact = compact;
    return k;
  }

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

  // Long clauses. Invariant guard: every ClauseID in the component's
  // clsBegin() list must refer to an original clause — learned clauses
  // never get ClauseIDs assigned (AltComponentAnalyzer::initialize
  // runs once on the original pool before search starts, and no later
  // code adds to clause_id_to_ofs_). If we see a ClauseID whose ofs
  // sits past the original-pool boundary, either (a) the pool was
  // restructured post-init without re-stamping original_lit_pool_size_,
  // or (b) learned clauses were added to clause_id_to_ofs_. Either
  // case corrupts the "key includes only non-redundant originals"
  // invariant, so abort loudly with diagnostics.
  for (auto it = comp.clsBegin(); *it != clsSENTINEL; it++) {
    ClauseOfs ofs = clause_id_to_ofs[*it];
    if (ofs >= original_lit_pool_size) {
      std::cerr << "\n*** CANONICAL_KEY_LEARNED_CLAUSE_ID "
                   "(ClauseID references learned pool region) ***\n"
                << "  clause_id=" << *it
                << " ofs=" << ofs
                << " original_lit_pool_size=" << original_lit_pool_size
                << "\n";
      std::cerr.flush();
      std::abort();
    }
    if (removed_clauses.count(ofs)) continue;

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

  // Sort signatures → assign canonical IDs.
  // When no_anonymization is set, bypass signature ranking entirely:
  // use the raw active-vars index (i+1) as the "canonical" ID and
  // clear all flip/singleton flags so polarity/singleton-collapse
  // don't kick in either. This makes the key dependent on the
  // absolute structure (modulo a stable per-component renumbering),
  // ruling out any bug introduced by the anonymization pass.
  s_canonical_id.assign(n_vars, 0);
  if (no_anonymization) {
    for (unsigned i = 0; i < n_vars; i++) {
      s_canonical_id[i] = i + 1;
      s_var_flags[i] = 0;  // no singleton, no flip
    }
    s_sig_pairs.clear();
    for (unsigned i = 0; i < n_vars; i++)
      s_sig_pairs.push_back({0, i});
  } else {
    s_sig_pairs.clear();
    for (unsigned i = 0; i < n_vars; i++)
      if (!(s_var_flags[i] & 1))
        s_sig_pairs.push_back({s_sig[i], i});

    std::sort(s_sig_pairs.begin(), s_sig_pairs.end());

    for (unsigned i = 0; i < s_sig_pairs.size(); i++)
      s_canonical_id[s_sig_pairs[i].second] = i + 1;

    // Diagnostic: scan adjacent equal-signature runs for collision blocks.
    g_canon_stats.n_calls++;
    unsigned anchored = 0;
    unsigned coll_vars = 0;
    unsigned orient_amb = 0;
    unsigned max_block_in_call = 1;
    bool had_any_collision = false;
    size_t i = 0;
    while (i < s_sig_pairs.size()) {
      size_t j = i + 1;
      while (j < s_sig_pairs.size() && s_sig_pairs[j].first == s_sig_pairs[i].first)
        j++;
      unsigned block_size = (unsigned)(j - i);
      if (block_size == 1) {
        anchored++;
      } else {
        had_any_collision = true;
        coll_vars += block_size;
        if (block_size > max_block_in_call) max_block_in_call = block_size;
        for (size_t k = i; k < j; k++) {
          unsigned idx = s_sig_pairs[k].second;
          if (s_pos_count[idx] == s_neg_count[idx]) orient_amb++;
        }
      }
      i = j;
    }
    g_canon_stats.sum_anchored += anchored;
    g_canon_stats.sum_collision_block_vars += coll_vars;
    g_canon_stats.sum_orientation_ambiguous_in_blocks += orient_amb;
    if (had_any_collision) g_canon_stats.calls_with_any_collision++;
    if (max_block_in_call > g_canon_stats.max_block_size)
      g_canon_stats.max_block_size = max_block_in_call;
    // bucket = floor(log2(max_block_in_call)), clamped to 15
    {
      unsigned bucket = 0, x = max_block_in_call;
      while (x > 1 && bucket < 15) { x >>= 1; bucket++; }
      g_canon_stats.max_block_buckets[bucket]++;
    }

    // WL refinement loop.
    //
    // We maintain a per-var "block label" (uint64) that equals across
    // vars in the same WL block. Iter 1's block labels are s_sig[].
    // Each iter k >= 2 hashes neighbors' block labels (NOT sequential
    // canonical IDs — using sequential IDs would make every var its
    // own "block" because IDs are unique post-iter-1) into sig_k, then
    // updates each var's block label to a hash of (old_label, sig_k).
    // Two vars stay in the same block iff their hashes coincide.
    //
    // After all iters, we re-sort by (final_block_label, idx) and
    // reassign canonical_id sequentially so the canon_buf keys carry
    // the refined ordering.
    auto mix64 = [](uint64_t x) {
      x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
      x ^= x >> 33;
      return x;
    };
    std::vector<uint64_t> block_label(n_vars, 0);
    for (unsigned i = 0; i < n_vars; i++) block_label[i] = s_sig[i];
    bool collision_now = had_any_collision;
    unsigned final_anchored = anchored;
    unsigned final_coll_vars = coll_vars;
    unsigned final_max_block = max_block_in_call;
    for (int iter = 2; iter <= wl_iterations && collision_now; iter++) {
      std::vector<uint64_t> sig_k(n_vars, 0);
      for (const auto &ref : s_clause_refs) {
        uint64_t clause_h = 0;
        if (!ref.is_binary) {
          for (auto lt = literal_pool.begin() + ref.ofs; *lt != SENTINEL_LIT; lt++) {
            if (literal_values[*lt] != X_TRI) continue;
            int idx = s_var_idx[lt->var()];
            if (idx < 0 || (s_var_flags[idx] & 1)) continue;
            clause_h += mix64(block_label[idx]);
          }
          for (auto lt = literal_pool.begin() + ref.ofs; *lt != SENTINEL_LIT; lt++) {
            if (literal_values[*lt] != X_TRI) continue;
            int idx = s_var_idx[lt->var()];
            if (idx < 0 || (s_var_flags[idx] & 1)) continue;
            sig_k[idx] += clause_h - mix64(block_label[idx]);
          }
        } else {
          int ia = s_var_idx[ref.var_a];
          int ib = s_var_idx[ref.var_b];
          uint64_t ha = (s_var_flags[ia] & 1) ? 0 : mix64(block_label[ia]);
          uint64_t hb = (s_var_flags[ib] & 1) ? 0 : mix64(block_label[ib]);
          clause_h = ha + hb;
          if (!(s_var_flags[ia] & 1)) sig_k[ia] += clause_h - ha;
          if (!(s_var_flags[ib] & 1)) sig_k[ib] += clause_h - hb;
        }
      }
      // Refine block_label: combine old label with sig_k.
      for (unsigned i = 0; i < n_vars; i++) {
        if (s_var_flags[i] & 1) continue;
        block_label[i] = mix64(block_label[i] ^ (sig_k[i] * 0x9E3779B97F4A7C15ULL));
      }
      // Recount blocks by sorting block_labels.
      std::vector<std::pair<uint64_t, unsigned>> order;
      order.reserve(s_sig_pairs.size());
      for (auto &p : s_sig_pairs)
        order.push_back({block_label[p.second], p.second});
      std::sort(order.begin(), order.end());
      unsigned anch_k = 0, coll_k = 0, max_k = 1;
      bool any_k = false;
      size_t a = 0;
      while (a < order.size()) {
        size_t b = a + 1;
        while (b < order.size() && order[b].first == order[a].first) b++;
        unsigned bs = (unsigned)(b - a);
        if (bs == 1) anch_k++;
        else { any_k = true; coll_k += bs; if (bs > max_k) max_k = bs; }
        a = b;
      }
      collision_now = any_k;
      final_anchored = anch_k;
      final_coll_vars = coll_k;
      final_max_block = max_k;
    }
    // Step 3: combine with static (preprocessing-time) WL labels for
    // vars still in collision blocks. Refines those blocks using
    // global structural info that pure dynamic WL doesn't see.
    // Anchored vars are untouched (their labels are already unique).
    // Fires whenever collisions remain — independent of wl_iterations.
    if (collision_now && static_wl_labels != nullptr) {
      // Identify which vars are currently in collision blocks
      // (block_size > 1 under current block_label).
      std::unordered_map<uint64_t, int> count;
      for (auto &p : s_sig_pairs) count[block_label[p.second]]++;
      for (auto &p : s_sig_pairs) {
        unsigned i = p.second;
        if (count[block_label[i]] > 1) {
          unsigned raw_var = s_active_vars[i];
          uint64_t sl = (raw_var < static_wl_labels->size())
                          ? (*static_wl_labels)[raw_var] : 0;
          block_label[i] = mix64(block_label[i] ^ (sl * 0x9E3779B97F4A7C15ULL));
        }
      }
      // Recount blocks after step 3.
      std::vector<std::pair<uint64_t, unsigned>> reord;
      reord.reserve(s_sig_pairs.size());
      for (auto &p : s_sig_pairs) reord.push_back({block_label[p.second], p.second});
      std::sort(reord.begin(), reord.end());
      unsigned anch3 = 0, coll3 = 0, max3 = 1;
      bool any3 = false;
      size_t pa = 0;
      while (pa < reord.size()) {
        size_t pb = pa + 1;
        while (pb < reord.size() && reord[pb].first == reord[pa].first) pb++;
        unsigned bs = (unsigned)(pb - pa);
        if (bs == 1) anch3++;
        else { any3 = true; coll3 += bs; if (bs > max3) max3 = bs; }
        pa = pb;
      }
      collision_now = any3;
      final_anchored = anch3;
      final_coll_vars = coll3;
      final_max_block = max3;
    }
    // Step 4: final canonical-ID assignment with identity fallback for
    // residual collision-block vars.
    //   - Anchored vars (final block size == 1): sequential 1..k_anchored
    //     in (block_label, idx) order, with the polarity flip applied.
    //   - Residual vars (final block size > 1): canonical_id =
    //     RESIDUAL_OFFSET + raw_var_id, marked with flag bit 4 so pass 2
    //     emits them with their ORIGINAL polarity (no flip).
    // Fires whenever iter 1 left any collision, since the post-iter-1
    // canonical IDs from the heuristic var_idx tie-break are unsafe.
    if (had_any_collision) {
      std::unordered_map<uint64_t, int> blk_count;
      for (auto &p : s_sig_pairs) blk_count[block_label[p.second]]++;
      // Collect anchored vars; sort by (block_label, idx); assign 1..k.
      std::vector<std::pair<uint64_t, unsigned>> anchored_sorted;
      anchored_sorted.reserve(s_sig_pairs.size());
      for (auto &p : s_sig_pairs)
        if (blk_count[block_label[p.second]] == 1)
          anchored_sorted.push_back({block_label[p.second], p.second});
      std::sort(anchored_sorted.begin(), anchored_sorted.end(),
        [](const std::pair<uint64_t, unsigned> &x,
           const std::pair<uint64_t, unsigned> &y) {
          if (x.first != y.first) return x.first < y.first;
          return x.second < y.second;
        });
      for (size_t k = 0; k < anchored_sorted.size(); k++)
        s_canonical_id[anchored_sorted[k].second] = (int)(k + 1);
      // Residual vars: raw_var_id + offset, no flip.
      const int RESIDUAL_OFFSET = (int)max_var + 1;
      for (auto &p : s_sig_pairs) {
        unsigned i = p.second;
        if (blk_count[block_label[i]] > 1) {
          s_canonical_id[i] = RESIDUAL_OFFSET + (int)s_active_vars[i];
          s_var_flags[i] |= 4;  // mark residual: pass 2 will skip flip
        }
      }
      g_canon_stats.sum_anchored_iter2 += final_anchored;
      g_canon_stats.sum_collision_block_vars_iter2 += final_coll_vars;
      if (collision_now) g_canon_stats.calls_with_any_collision_iter2++;
      if (final_max_block > g_canon_stats.max_block_size_iter2)
        g_canon_stats.max_block_size_iter2 = final_max_block;
      unsigned bucket_f = 0, xf = final_max_block;
      while (xf > 1 && bucket_f < 15) { xf >>= 1; bucket_f++; }
      g_canon_stats.max_block_buckets_iter2[bucket_f]++;
    }
  }

  // Pass 2: Compute component hash using canonical IDs. In compact
  // mode we also compute a second independent FNV to fill hash_hi
  // (different offset basis + different multiplier), and we skip
  // materializing the canonical clause multiset. In strict mode we
  // still populate `canonical_clauses` for the deep equality check.
  uint64_t component_hash = 0;
  uint64_t component_hash_hi = 0;
  unsigned total_clauses = 0;
  std::vector<std::vector<int>> canonical_clauses;
  if (!compact) canonical_clauses.reserve(s_clause_refs.size());

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
          bool is_residual = (s_var_flags[idx] & 4);
          bool norm_pos = is_residual
                             ? (lt->toInt() > 0)
                             : ((lt->toInt() > 0) != bool(s_var_flags[idx] & 2));
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
          bool is_residual = (s_var_flags[idx] & 4);
          bool norm_pos = is_residual
                             ? (orig_lit > 0)
                             : ((orig_lit > 0) != bool(s_var_flags[idx] & 2));
          canon = norm_pos ? cid : -cid;
        }
        if (canon_len < 256) canon_buf[canon_len++] = canon;
      }
    }

    std::sort(canon_buf, canon_buf + canon_len);

    uint64_t ch_lo = 14695981039346656037ULL;
    uint64_t ch_hi = 0x9E3779B97F4A7C15ULL;
    for (unsigned i = 0; i < canon_len; i++) {
      uint64_t w = (uint64_t)(unsigned)canon_buf[i];
      ch_lo ^= w; ch_lo *= 1099511628211ULL;
      ch_hi ^= w; ch_hi *= 0xBF58476D1CE4E5B9ULL;
    }
    ch_lo ^= canon_len * 0x9E3779B97F4A7C15ULL;
    ch_hi ^= canon_len * 0xC6A4A7935BD1E995ULL;

    component_hash += ch_lo;
    component_hash_hi += ch_hi;
    total_clauses++;

    if (!compact) {
      // Store the canonical clause (sorted) for structural equality check.
      canonical_clauses.emplace_back(canon_buf, canon_buf + canon_len);
    }
  }

  // Sort clauses lexicographically so the multiset has a canonical order.
  if (!compact) std::sort(canonical_clauses.begin(), canonical_clauses.end());

  // --- Sanity-check guards: debug-only -----------------------------
  // These structural invariants on the canonical form are redundant
  // with the external regression tests (test_canonical_key_invariance,
  // test_canonical_key_learned) and are O(Σ|clause|) per build. With
  // ~100k key builds on t1_011 they add up. Keep them under NDEBUG so
  // debug builds still catch any regression while Release pays nothing.
  // Also gated on strict mode — compact mode never fills
  // canonical_clauses, so there is nothing to inspect.
#ifndef NDEBUG
  if (!compact) {
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
    for (size_t i = 1; i < cl.size(); i++) {
      if (cl[i-1] > cl[i])
        fire_guard("canonical clause literals not sorted ascending", &cl);
    }
    for (int l : cl) {
      if (l != 0 && (std::abs(l) < 1 || std::abs(l) > n_nonsing))
        fire_guard("canonical literal ID out of valid range", &cl);
    }
    for (size_t i = 1; i < cl.size(); i++) {
      if (cl[i] == 0 && cl[i-1] == 0) continue;
      if (std::abs(cl[i]) == std::abs(cl[i-1]))
        fire_guard("canonical clause contains a variable twice", &cl);
    }
  }
  for (size_t i = 1; i < canonical_clauses.size(); i++) {
    if (canonical_clauses[i] < canonical_clauses[i-1])
      fire_guard("canonical_clauses not sorted lexicographically");
  }
  }  // if (!compact)
#endif
  // -----------------------------------------------------------------

  // Clean up var_idx entries (reset only what we set)
  for (unsigned v : s_active_vars) s_var_idx[v] = -1;

  // Count active vars that appear in at least one canonical clause.
  // The remaining (num_vars - n_in_clauses) are free vars at this
  // level — they contribute a 2^free factor that is applied OUTSIDE
  // the cached value, so the cache stores the structural count only.
  unsigned n_in_clauses = 0;
  for (unsigned i = 0; i < n_vars; i++)
    if (s_pos_count[i] + s_neg_count[i] > 0)
      n_in_clauses++;

  CanonicalKey key;
  key.hash = component_hash;
  key.hash_hi = component_hash_hi;
  key.num_vars = n_vars;
  key.n_in_clauses = n_in_clauses;
  key.num_clauses = total_clauses;
  key.compact = compact;
  if (!compact) key.clauses = std::move(canonical_clauses);
  return key;
}

// ---------------------------------------------------------------
// Static (preprocessing-time) WL label computation.
//
// One-time pass over the post-preprocessing global formula. Uses the
// same dynamic-WL primitives as buildCanonicalKey: each var's seed
// label is a sum of clause-type-dictionary hashes for the original
// long + binary clauses it participates in, then n_iters of neighbor
// aggregation. Result: one uint64 per var index (1..num_vars; entry 0
// unused). Called once and stored on the Solver for the rest of the run.
// ---------------------------------------------------------------
std::vector<uint64_t> computeStaticWLLabels(
    unsigned num_vars,
    const std::vector<LiteralID> &literal_pool,
    const LiteralIndexedVector<Literal> &literals,
    const LiteralIndexedVector<TriValue> &literal_values,
    unsigned original_lit_pool_size,
    int n_iters) {
  std::vector<uint64_t> labels(num_vars + 1, 0);

  // Build clause-ref list (long + binary) over ALL non-satisfied
  // active original clauses in the global formula.
  struct CRef {
    bool is_binary;
    ClauseOfs ofs;
    unsigned va, vb;
    int la, lb;
  };
  std::vector<CRef> refs;

  // Long original clauses.
  for (auto it = literal_pool.begin(); it != literal_pool.end(); it++) {
    if (*it != SENTINEL_LIT) continue;
    if (it + 1 == literal_pool.end()) break;
    it += ClauseHeader::overheadInLits();
    ClauseOfs ofs = (ClauseOfs)(it + 1 - literal_pool.begin());
    if (ofs >= (ClauseOfs)original_lit_pool_size) break;
    bool sat = false;
    unsigned active = 0;
    for (auto lt = literal_pool.begin() + ofs; *lt != SENTINEL_LIT; lt++) {
      if (literal_values[*lt] == T_TRI) { sat = true; break; }
      if (literal_values[*lt] == X_TRI) active++;
    }
    if (!sat && active >= 2) {
      CRef r; r.is_binary = false; r.ofs = ofs;
      r.va = r.vb = 0; r.la = r.lb = 0;
      refs.push_back(r);
    }
    auto end_it = literal_pool.begin() + ofs;
    while (*end_it != SENTINEL_LIT) end_it++;
    it = end_it - 1;
  }

  // Binary clauses (each undirected pair once).
  for (unsigned v = 1; v <= num_vars; v++) {
    for (int sign = 0; sign <= 1; sign++) {
      LiteralID lit(v, sign == 0);
      if (literal_values[lit] == T_TRI) continue;
      const auto &bl = literals[lit].binary_links_;
      unsigned orig_count = literals[lit].original_binary_link_count_;
      unsigned k = 0;
      for (auto bt = bl.begin(); *bt != SENTINEL_LIT; ++bt, ++k) {
        if (k >= orig_count) break;
        if (literal_values[*bt] == T_TRI) continue;
        unsigned other = bt->var();
        if (other <= v) continue;  // each pair once
        CRef r; r.is_binary = true;
        r.va = v; r.vb = other;
        r.la = lit.toInt(); r.lb = bt->toInt();
        r.ofs = 0;
        refs.push_back(r);
      }
    }
  }

  // Per-var pos/neg counts and clause types (matching dynamic-WL).
  std::vector<int> pos_count(num_vars + 1, 0);
  std::vector<int> neg_count(num_vars + 1, 0);
  for (const auto &r : refs) {
    if (!r.is_binary) {
      for (auto lt = literal_pool.begin() + r.ofs; *lt != SENTINEL_LIT; lt++) {
        if (literal_values[*lt] != X_TRI) continue;
        if (lt->toInt() > 0) pos_count[lt->var()]++;
        else neg_count[lt->var()]++;
      }
    } else {
      if (r.la > 0) pos_count[r.va]++; else neg_count[r.va]++;
      if (r.lb > 0) pos_count[r.vb]++; else neg_count[r.vb]++;
    }
  }
  std::vector<bool> is_singleton(num_vars + 1, false);
  std::vector<bool> is_flip(num_vars + 1, false);
  for (unsigned v = 1; v <= num_vars; v++) {
    int p = pos_count[v], n = neg_count[v];
    if (p + n == 1) is_singleton[v] = true;
    else if (n > p) is_flip[v] = true;
  }

  // Iter 1: clause-type signature.
  for (const auto &r : refs) {
    unsigned len = 0, np = 0, nn = 0, ns = 0;
    auto contrib = [&](unsigned var, int orig_lit) {
      if (literal_values[LiteralID(var, true)] != X_TRI) return;
      len++;
      if (is_singleton[var]) { ns++; return; }
      bool norm_pos = (orig_lit > 0) != is_flip[var];
      if (norm_pos) np++; else nn++;
    };
    if (!r.is_binary) {
      for (auto lt = literal_pool.begin() + r.ofs; *lt != SENTINEL_LIT; lt++)
        contrib(lt->var(), lt->toInt());
    } else {
      contrib(r.va, r.la);
      contrib(r.vb, r.lb);
    }
    ClauseType ct = {len, np, nn, ns};
    auto hh = g_clause_type_dict.lookup(ct);
    auto add_for_var = [&](unsigned var, int orig_lit) {
      if (is_singleton[var]) return;
      if (literal_values[LiteralID(var, true)] != X_TRI) return;
      bool norm_pos = (orig_lit > 0) != is_flip[var];
      labels[var] += norm_pos ? hh.first : hh.second;
    };
    if (!r.is_binary) {
      for (auto lt = literal_pool.begin() + r.ofs; *lt != SENTINEL_LIT; lt++)
        add_for_var(lt->var(), lt->toInt());
    } else {
      add_for_var(r.va, r.la);
      add_for_var(r.vb, r.lb);
    }
  }

  // Iters 2..n_iters: neighbor aggregation.
  auto mix64 = [](uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
  };
  for (int it = 2; it <= n_iters; it++) {
    std::vector<uint64_t> sig(num_vars + 1, 0);
    for (const auto &r : refs) {
      uint64_t clause_h = 0;
      auto add_clause_h = [&](unsigned var) {
        if (is_singleton[var]) return;
        if (literal_values[LiteralID(var, true)] != X_TRI) return;
        clause_h += mix64(labels[var]);
      };
      auto add_sig = [&](unsigned var) {
        if (is_singleton[var]) return;
        if (literal_values[LiteralID(var, true)] != X_TRI) return;
        sig[var] += clause_h - mix64(labels[var]);
      };
      if (!r.is_binary) {
        for (auto lt = literal_pool.begin() + r.ofs; *lt != SENTINEL_LIT; lt++)
          add_clause_h(lt->var());
        for (auto lt = literal_pool.begin() + r.ofs; *lt != SENTINEL_LIT; lt++)
          add_sig(lt->var());
      } else {
        add_clause_h(r.va);
        add_clause_h(r.vb);
        add_sig(r.va);
        add_sig(r.vb);
      }
    }
    for (unsigned v = 1; v <= num_vars; v++) {
      if (is_singleton[v]) continue;
      labels[v] = mix64(labels[v] ^ (sig[v] * 0x9E3779B97F4A7C15ULL));
    }
  }

  return labels;
}
