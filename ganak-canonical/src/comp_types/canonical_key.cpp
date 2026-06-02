/******************************************
Copyright (C) 2026 Authors of GANAK fork (canonical-key port)
                   Original code Copyright (C) 2023 GANAK Authors,
                   ported from sharpsat-separator/src/canonical_key.cpp

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
***********************************************/

/*
 * canonical_key.cpp
 *
 * One-pass canonical key computation using tabulation hashing.
 *
 * 1. Scan clauses once: classify type, detect singletons, accumulate
 *    variable signatures via dictionary lookup
 * 2. Sort N signature values -> assign canonical IDs
 * 3. Scan clauses again: compute component hash using canonical IDs
 *
 * No heap allocation per component. Reuses stack-allocated arrays.
 *
 * GANAK PORT (canonical-key port, Commit 4). Differences from the
 * sharpsat-separator original:
 *   - Long clauses are read from ctx.canon_lit_pool, a FROZEN-at-restart-init
 *     SENTINEL_LIT-delimited literal snapshot built in CompAnalyzer::initialize
 *     (the analogue of the original's literal_pool and of ganak's own
 *     long_clauses_data). comp.cls_begin() yields clause IDs (1..max_clid), not
 *     offsets (comp_analyzer.hpp:184), so each ID is resolved to its snapshot
 *     offset via ctx.clid_to_lit_off, then walked SENTINEL_LIT-terminated.
 *   - Binary clauses come from a FROZEN irred-binary snapshot
 *     (ctx.canon_bin_pool, keyed by lit.raw() via ctx.lit_to_bin_off), built in
 *     CompAnalyzer::initialize from the same watches[] ganak's own unif_occ_bin
 *     uses — so the binary view is frozen-at-init like the long-clause one,
 *     immune to mid-restart vivify adding irred binaries. Replaces the original's
 *     binary_links_ / original_binary_link_count_ walk.
 *   - removed_clauses and clause_id_to_ofs are dropped entirely (ganak has no
 *     clause branching; its analyzer already excludes learned clauses).
 *   - literal_values -> *ctx.values; LiteralID::toInt()>0 -> Lit::sign();
 *     LiteralID::var() -> Lit::var(); varsSENTINEL/clsSENTINEL -> `sentinel`.
 *
 * WHY A FROZEN SNAPSHOT (not the live allocator):
 * ganak caches on a frozen-at-restart-init view of clause structure — its
 * analyzer copies literals into long_clauses_data at init and never re-reads
 * the live clauses during a restart, and its identity caches key on clause IDs.
 * Mid-restart consolidate (rewrites long_irred_cls offsets in place) and vivify
 * (shrinks/removes clauses and COMPACTS+RESIZES long_irred_cls) both mutate the
 * live clause storage. Reading clause content live (the earlier
 * clid_to_pos -> long_irred_cls -> alloc->ptr design) diverged from that frozen
 * view and was unsound under vivify. Reading the frozen snapshot stays
 * bit-consistent with ganak's component detection and is immune to both. It is
 * sound because vivify/consolidate are model-count-preserving and everything
 * re-freezes at the next restart.
 */

#include "canonical_key.hpp"

#include <vector>
#include <algorithm>

namespace GanakInt {

// Global clause type dictionary
ClauseTypeDictionary g_clause_type_dict;

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
// IMPORTANT: --cachehash canonical is rejected at configure time when
// ganak's parallel counting is requested (--threads > 1), so a single
// Counter owns these buffers. If that restriction is ever lifted (per-thread
// Counter as in ganak's OuterCounter), these MUST be reverted to
// `thread_local` to avoid data races — each thread would race on these
// shared buffers and produce wrong canonical keys silently.
static std::vector<unsigned> s_active_vars;
static std::vector<int> s_var_idx;
static std::vector<int> s_pos_count;
static std::vector<int> s_neg_count;
static std::vector<uint64_t> s_sig;
static std::vector<int> s_canonical_id;
static std::vector<std::pair<uint64_t, unsigned>> s_sig_pairs;

struct ClauseRef {
  uint32_t ofs;          // long clauses: offset into ctx.canon_lit_pool
  unsigned var_a, var_b; // for binary clauses (var IDs)
  int lit_a, lit_b;      // for binary clauses (signed literal values; sign read)
  bool is_binary;
};
static std::vector<ClauseRef> s_clause_refs;  // see note above re: thread_local

// Per-variable flags packed into a single byte array
// Bit 0: is_singleton, Bit 1: flip, Bit 2: in-I (projection)
static std::vector<uint8_t> s_var_flags;  // see note above re: thread_local

// WL refinement scratch buffers. Same single-threaded restriction as
// the other s_* buffers above.
//
// s_labels_collision is a sorted vector of (block_label, var_idx)
// pairs covering all non-singleton vars. It serves three purposes:
//   (1) collision detection at the end of each WL iteration
//       (consecutive equal labels => collision remains),
//   (2) the static-WL refinement step's "which vars are in
//       multi-var blocks" question (groups of size > 1 in the
//       sorted vector),
//   (3) the final anchored-ID assignment, which needs singletons
//       in (block_label, var_idx) order to assign sequential IDs.
static std::vector<uint64_t> s_block_label;
static std::vector<uint64_t> s_sig_k;
static std::vector<std::pair<uint64_t, unsigned>> s_labels_collision;
// Per-clause scratch for the WL inner loop's contribution gather.
// Reused across all clauses within a single WL iteration so that the
// long-clause case can walk the clause ONCE per clause instead of
// twice. Each entry is (sig_pair_var_idx, mix64(block_label[idx])).
static std::vector<std::pair<int, uint64_t>> s_wl_clause_contribs;

CanonicalKey build_canonical_key(
    const Comp& comp,
    const CanonicalKeyContext& ctx,
    uint64_t /*hash_seed*/) {

  const LiteralIndexedVector<TriValue>& values = *ctx.values;
  // Frozen-at-init literal snapshot (built in CompAnalyzer::initialize). We read
  // clause literals from here, NOT live from the allocator, so the key sees the
  // same frozen formula ganak's own analyzer uses — immune to mid-restart
  // consolidate (offset rewrites) and vivify (clause shrink/removal/compaction).
  // Each clause's literals are a SENTINEL_LIT-terminated run; ref.ofs / the
  // clid_to_lit_off lookup give the run's start offset.
  const std::vector<Lit>& pool = *ctx.canon_lit_pool;

  // Collect active variables
  // Reset s_labels_collision so the post-WL "is it built?" check via
  // .size() == s_sig_pairs.size() can't be fooled by a prior call's
  // residue (e.g. same n_vars but different block_label contents).
  s_labels_collision.clear();

  s_active_vars.clear();
  for (auto it = comp.vars_begin(); *it != sentinel; it++)
    if (values[Lit(*it, true)] == X_TRI)
      s_active_vars.push_back(*it);

  if (s_active_vars.empty()) {
    CanonicalKey k;
    return k;
  }

  unsigned n_vars = s_active_vars.size();
  unsigned max_var = s_active_vars.back();

  // The canon-id encoding uses RESIDUAL_OFFSET = 1<<30 + raw_var_id and the
  // I-flag bit 1<<28 (pass 2). Both assume raw var ids fit below 2^28: above
  // that, the I-flag can alias with a residual id (an I/Y false merge -> wrong
  // projected count) and RESIDUAL_OFFSET + raw_var_id overflows signed int.
  // Unreachable on any real instance (would need >268M vars), but guard so the
  // failure is loud (aborts) rather than a silent miscount. release_assert
  // fires in Release builds too.
  release_assert(max_var < (1u << 28) &&
      "canonical-key var id exceeds 2^28; encoding would alias/overflow");

  // Resize and clear reusable buffers
  // var_idx: only clear entries we set (tracked via active_vars)
  if (s_var_idx.size() <= max_var) s_var_idx.resize(max_var + 1, -1);

  for (unsigned i = 0; i < n_vars; i++)
    s_var_idx[s_active_vars[i]] = i;

  // Per-variable arrays sized to n_vars
  s_pos_count.assign(n_vars, 0);
  s_neg_count.assign(n_vars, 0);

  s_clause_refs.clear();

  // Long clauses. comp.cls_begin() yields clause IDs (1..max_clid), NOT
  // ClauseOfs — see comp_analyzer.hpp:184. We resolve each ID to its literals
  // in the FROZEN snapshot via ctx.clid_to_lit_off (a frozen-at-init offset
  // into ctx.canon_lit_pool). Learned clauses never enter the Comp clause list
  // (the analyzer builds over long_irred_cls only), so no learned-clause guard
  // is needed.
  for (auto it = comp.cls_begin(); *it != sentinel; it++) {
    const uint32_t off = (*ctx.clid_to_lit_off)[*it];

    bool satisfied = false;
    unsigned active_count = 0;
    for (auto lt = pool.begin() + off; *lt != SENTINEL_LIT; lt++) {
      if (values[*lt] == T_TRI) { satisfied = true; break; }
      if (values[*lt] == X_TRI) active_count++;
    }
    if (satisfied || active_count < 2) continue;

    ClauseRef ref;
    ref.ofs = off;
    ref.is_binary = false;
    s_clause_refs.push_back(ref);

    for (auto lt = pool.begin() + off; *lt != SENTINEL_LIT; lt++) {
      if (values[*lt] == X_TRI) {
        int idx = s_var_idx[lt->var()];
        if (idx >= 0) {
          if (lt->sign()) s_pos_count[idx]++;
          else s_neg_count[idx]++;
        }
      }
    }
  }

  // Binary clauses — use var_idx >= 0 to check component membership. Read the
  // partner literals from the FROZEN binary snapshot (ctx.canon_bin_pool, keyed
  // by lit.raw() via ctx.lit_to_bin_off), NOT live watches[], so the binary view
  // is frozen-at-restart-init like the long-clause snapshot — mid-restart vivify
  // adding an irred binary cannot change the key. The snapshot already holds
  // only irredundant binaries (filtered when built in CompAnalyzer::initialize).
  const std::vector<Lit>& binpool = *ctx.canon_bin_pool;
  for (unsigned v : s_active_vars) {
    for (int sgn = 0; sgn <= 1; sgn++) {
      Lit lit(v, sgn == 0);  // sgn==0 -> positive literal (matches original)
      for (auto bt = binpool.begin() + (*ctx.lit_to_bin_off)[lit.raw()];
           *bt != SENTINEL_LIT; bt++) {
        const Lit other = *bt;
        const unsigned other_var = other.var();
        if (other_var <= v) continue;
        if (other_var > max_var || s_var_idx[other_var] < 0) continue;
        if (values[other] == T_TRI) continue;
        if (values[lit] == T_TRI) continue;

        int vi = s_var_idx[v];
        int oi = s_var_idx[other_var];

        if (lit.sign()) s_pos_count[vi]++;
        else s_neg_count[vi]++;
        if (other.sign()) s_pos_count[oi]++;
        else s_neg_count[oi]++;

        ClauseRef ref;
        ref.is_binary = true;
        ref.var_a = v;
        ref.var_b = other_var;
        ref.lit_a = lit.val();
        ref.lit_b = other.val();
        s_clause_refs.push_back(ref);
      }
    }
  }

  // Determine singletons and polarity flips (packed into flags byte).
  // Plus the I-bit (bit 2 = 0x04) when projection is active — this is
  // the "pre-0 iteration" labeling: two structurally-identical
  // components with different I-membership distributions will get
  // distinct initial labels here, which propagates through the WL
  // refinement to distinct final canonical keys.
  //
  // INVARIANT: the I-bit (is_indep) must enter the canonical key at WL
  // ITERATION 0, by pre-seeding the per-var signature with INDEP_SEED for
  // I-vars (the s_sig seed loop just below). Moving this seeding to any
  // later iteration is UNSOUND: it would let pre-iteration-0 collisions
  // propagate and collapse sub-components with different I/Y distributions
  // to the same canonical key, producing wrong projected counts.
  // Reference: precomputed_key_safety_analysis.md §3, and
  // canonical_key.cpp:216-240 in the sharpsat-separator source.
  s_var_flags.assign(n_vars, 0);
  for (unsigned i = 0; i < n_vars; i++) {
    const bool in_i = ctx.is_indep && (*ctx.is_indep)[s_active_vars[i]];
    // The singleton collapse is NEVER applied to I-vars. A Y-singleton can
    // always be assigned to satisfy its one clause "for free" (existential),
    // so its identity and polarity don't affect the count — only the clause
    // length — hence it collapses to canon=0. An I-var's assignment is
    // COUNTED: both values matter and the falsifying one is a real constraint,
    // so collapsing an I-singleton would merge components with different
    // projected counts (e.g. {I-sing, Y-sing} vs {Y-sing, Y-sing}). I-vars
    // therefore always get a full canonical ID; only Y-vars keep the singleton
    // optimization. (Projection-soundness fix; the marker alternative was
    // rejected as needing an unproven equivalence argument.)
    if (!in_i && s_pos_count[i] + s_neg_count[i] == 1)
      s_var_flags[i] |= 1;  // singleton (Y-vars only)
    else if (s_neg_count[i] > s_pos_count[i])
      s_var_flags[i] |= 2;  // flip
    if (in_i)
      s_var_flags[i] |= 4;  // in I
  }

  // Pass 1b: Compute variable signatures via tabulation.
  // When the I-bit is active (s_var_flags[i] & 4), pre-seed the
  // signature with INDEP_SEED so I-vars and non-I-vars accumulate
  // distinguishable signatures even if their clause-hash contributions
  // are identical. Without this seed, two structurally-equivalent
  // components with different I-distributions would collide on the
  // canonical_key — wrong for projected counting.
  //
  // When is_indep is null, no var has bit 4 set, so the seed loop is a
  // no-op and the signature is identical to the non-projected case.
  const uint64_t INDEP_SEED = 0xC3F2E1D0B0A09080ULL;
  s_sig.assign(n_vars, 0);
  for (unsigned i = 0; i < n_vars; i++) {
    if (s_var_flags[i] & 4) s_sig[i] = INDEP_SEED;
  }

  for (const auto &ref : s_clause_refs) {
    unsigned len = 0, np = 0, nn = 0, ns = 0;

    if (!ref.is_binary) {
      for (auto lt = pool.begin() + ref.ofs; *lt != SENTINEL_LIT; lt++) {
        if (values[*lt] != X_TRI) continue;
        int idx = s_var_idx[lt->var()];
        if (idx < 0) continue;
        len++;
        if (s_var_flags[idx] & 1) ns++;
        else {
          bool norm_pos = lt->sign() != bool(s_var_flags[idx] & 2);
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
      for (auto lt = pool.begin() + ref.ofs; *lt != SENTINEL_LIT; lt++) {
        if (values[*lt] != X_TRI) continue;
        int idx = s_var_idx[lt->var()];
        if (idx < 0 || (s_var_flags[idx] & 1)) continue;
        bool norm_pos = lt->sign() != bool(s_var_flags[idx] & 2);
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

  // Sort signatures -> assign canonical IDs.
  //
  // Exclusions from canonical_id assignment:
  //   - Singletons (flag & 1): only appear once, hashed as canon=0.
  //   - Free vars (s_sig[i] == 0): active vars that don't appear in any
  //     in-scope clause. Their canonical_ids are never read (clauses
  //     don't reference them), and including them in s_sig_pairs would
  //     shift the IDs of in-clause vars depending on how many free vars
  //     happen to be active — breaking canonical_key's invariance over
  //     free-var counts.
  s_canonical_id.assign(n_vars, 0);
  {
    s_sig_pairs.clear();
    for (unsigned i = 0; i < n_vars; i++)
      if (!(s_var_flags[i] & 1) && s_sig[i] != 0)
        s_sig_pairs.push_back({s_sig[i], i});

    std::sort(s_sig_pairs.begin(), s_sig_pairs.end());

    for (unsigned i = 0; i < s_sig_pairs.size(); i++)
      s_canonical_id[s_sig_pairs[i].second] = i + 1;

    // Scan adjacent equal-signature runs for collision blocks. The
    // counters here feed WL-refinement control flow below (collision_now
    // gates the WL loop and the static-WL combine step).
    bool had_any_collision = false;
    size_t ri = 0;
    while (ri < s_sig_pairs.size()) {
      size_t rj = ri + 1;
      while (rj < s_sig_pairs.size() && s_sig_pairs[rj].first == s_sig_pairs[ri].first)
        rj++;
      if (rj - ri > 1) had_any_collision = true;
      ri = rj;
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
    s_block_label.assign(s_sig.begin(), s_sig.begin() + n_vars);
    auto &block_label = s_block_label;
    bool collision_now = had_any_collision;
    for (int iter = 2; iter <= ctx.wl_iterations && collision_now; iter++) {
      s_sig_k.assign(n_vars, 0);
      auto &sig_k = s_sig_k;
      for (const auto &ref : s_clause_refs) {
        uint64_t clause_h = 0;
        if (!ref.is_binary) {
          // Single walk of the clause: gather (idx, mix64(block_label[idx]))
          // into a small scratch buffer while accumulating clause_h. Then
          // iterate the buffer to apply sig_k[idx] += clause_h - h_idx.
          s_wl_clause_contribs.clear();
          for (auto lt = pool.begin() + ref.ofs; *lt != SENTINEL_LIT; lt++) {
            if (values[*lt] != X_TRI) continue;
            int idx = s_var_idx[lt->var()];
            if (idx < 0 || (s_var_flags[idx] & 1)) continue;
            uint64_t h = mix64(block_label[idx]);
            s_wl_clause_contribs.push_back({idx, h});
            clause_h += h;
          }
          for (const auto &c : s_wl_clause_contribs)
            sig_k[c.first] += clause_h - c.second;
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
      // Rebuild s_labels_collision as sorted (block_label, var_idx)
      // pairs over all non-singleton vars. Used both for collision
      // detection and (post-loop) by the static-WL and anchored-ID
      // steps. Sorted ascending by (block_label, var_idx).
      s_labels_collision.clear();
      s_labels_collision.reserve(s_sig_pairs.size());
      for (auto &p : s_sig_pairs)
        s_labels_collision.push_back({block_label[p.second], p.second});
      std::sort(s_labels_collision.begin(), s_labels_collision.end());
      collision_now = false;
      for (size_t k = 1; k < s_labels_collision.size(); k++)
        if (s_labels_collision[k].first == s_labels_collision[k - 1].first) {
          collision_now = true; break;
        }
    }
    // Step 3: combine with static (preprocessing-time) WL labels for
    // vars still in collision blocks. Refines those blocks using
    // global structural info that pure dynamic WL doesn't see.
    // Anchored vars are untouched (their labels are already unique).
    // Fires whenever collisions remain — independent of wl_iterations.
    if (collision_now && ctx.static_wl_labels != nullptr) {
      // s_labels_collision must hold sorted (block_label, var_idx)
      // pairs for the CURRENT block_label. The WL loop's last
      // iteration leaves it that way; rebuild defensively to cover
      // the case where the WL loop didn't run (wl_iterations < 2
      // but had_any_collision was true).
      if (s_labels_collision.size() != s_sig_pairs.size()) {
        s_labels_collision.clear();
        s_labels_collision.reserve(s_sig_pairs.size());
        for (auto &p : s_sig_pairs)
          s_labels_collision.push_back({block_label[p.second], p.second});
        std::sort(s_labels_collision.begin(), s_labels_collision.end());
      }
      // Two-pointer scan: groups with size > 1 are collision blocks.
      // Refine each var in such a group with its static WL label.
      size_t k = 0;
      while (k < s_labels_collision.size()) {
        size_t j = k + 1;
        while (j < s_labels_collision.size()
               && s_labels_collision[j].first == s_labels_collision[k].first)
          j++;
        if (j - k > 1) {
          for (size_t m = k; m < j; m++) {
            unsigned i = s_labels_collision[m].second;
            unsigned raw_var = s_active_vars[i];
            uint64_t sl = (raw_var < ctx.static_wl_labels->size())
                            ? (*ctx.static_wl_labels)[raw_var] : 0;
            block_label[i] = mix64(block_label[i] ^ (sl * 0x9E3779B97F4A7C15ULL));
          }
        }
        k = j;
      }
      // Rebuild s_labels_collision against the refined block_label
      // for the collision check + the downstream anchored-ID step.
      s_labels_collision.clear();
      s_labels_collision.reserve(s_sig_pairs.size());
      for (auto &p : s_sig_pairs)
        s_labels_collision.push_back({block_label[p.second], p.second});
      std::sort(s_labels_collision.begin(), s_labels_collision.end());
      collision_now = false;
      for (size_t kk = 1; kk < s_labels_collision.size(); kk++)
        if (s_labels_collision[kk].first == s_labels_collision[kk - 1].first) {
          collision_now = true; break;
        }
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
      // s_labels_collision holds sorted (block_label, var_idx) pairs.
      // After the WL loop and/or static-WL step it's current; if
      // neither ran (wl_iterations < 2 and static_wl_labels == nullptr),
      // build it now.
      if (s_labels_collision.size() != s_sig_pairs.size()) {
        s_labels_collision.clear();
        s_labels_collision.reserve(s_sig_pairs.size());
        for (auto &p : s_sig_pairs)
          s_labels_collision.push_back({block_label[p.second], p.second});
        std::sort(s_labels_collision.begin(), s_labels_collision.end());
      }
      // Two-pointer scan: groups of size 1 are anchored (sequential
      // 1..k_anchored in sorted (block_label, idx) order). Groups of
      // size > 1 are residual (RESIDUAL_OFFSET + raw_var_id, flag bit 4).
      //
      // RESIDUAL_OFFSET must be FREE-VAR-INVARIANT: using max_var+1
      // (the old formula) shifts when a free var with a higher raw ID
      // is active, so two states with identical in-scope clauses but
      // differing free-var counts get different residual canonical_ids
      // -> different per-clause hashes -> cache miss. We use a fixed
      // sentinel above any plausible anchor count or raw var ID.
      const int RESIDUAL_OFFSET = 1 << 30;
      unsigned anchor_id = 1;
      size_t k = 0;
      while (k < s_labels_collision.size()) {
        size_t j = k + 1;
        while (j < s_labels_collision.size()
               && s_labels_collision[j].first == s_labels_collision[k].first)
          j++;
        if (j - k == 1) {
          unsigned i = s_labels_collision[k].second;
          s_canonical_id[i] = (int)(anchor_id++);
        } else {
          for (size_t m = k; m < j; m++) {
            unsigned i = s_labels_collision[m].second;
            s_canonical_id[i] = RESIDUAL_OFFSET + (int)s_active_vars[i];
            s_var_flags[i] |= 4;  // pass 2 will skip flip for residuals
          }
        }
        k = j;
      }
    }
  }

  // Pass 2: Compute component hash using canonical IDs. Two independent
  // FNV sums (different offset basis + different multiplier) fill hash
  // and hash_hi for a 128-bit identity.
  uint64_t component_hash = 0;
  uint64_t component_hash_hi = 0;
  unsigned total_clauses = 0;

  for (const auto &ref : s_clause_refs) {
    int canon_buf[256];
    unsigned canon_len = 0;

    if (!ref.is_binary) {
      for (auto lt = pool.begin() + ref.ofs; *lt != SENTINEL_LIT; lt++) {
        if (values[*lt] != X_TRI) continue;
        int idx = s_var_idx[lt->var()];
        if (idx < 0) continue;
        int canon;
        if (s_var_flags[idx] & 1) {
          canon = 0;
        } else {
          int cid = s_canonical_id[idx];
          // Encode I-membership DIRECTLY into the emitted key, not just the
          // signature. INDEP_SEED only perturbs canonical-id ORDERING and
          // vanishes when an I-var is the sole non-singleton — so an all-Y
          // component and an otherwise-identical 1-I component would collide
          // (different projected counts, same key). A high flag bit makes an
          // I-var's canon disjoint from a Y-var's. Safe for nVars < 2^28
          // (above anchored IDs and below RESIDUAL_OFFSET=1<<30).
          if (ctx.is_indep && (*ctx.is_indep)[s_active_vars[idx]]) cid |= (1 << 28);
          bool is_residual = (s_var_flags[idx] & 4);
          bool norm_pos = is_residual
                             ? lt->sign()
                             : (lt->sign() != bool(s_var_flags[idx] & 2));
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
          // Encode I-membership directly (see the long-clause branch above).
          if (ctx.is_indep && (*ctx.is_indep)[s_active_vars[idx]]) cid |= (1 << 28);
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
  }

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
  return key;
}

// ---------------------------------------------------------------
// Static (preprocessing-time) WL label computation.
//
// One-time pass over the post-preprocessing global formula. Uses the
// same dynamic-WL primitives as build_canonical_key: each var's seed
// label is a sum of clause-type-dictionary hashes for the original
// long + binary clauses it participates in, then n_iters of neighbor
// aggregation. Result: one uint64 per var index (1..num_vars; entry 0
// unused). Called once and stored on the Counter for the rest of the run.
//
// Port note (§4.1): the sharpsat-separator original walked a flat
// SENTINEL_LIT-delimited literal_pool to enumerate clauses; here we iterate
// `long_irred_cls` (genuine ClauseOfs values) directly and dereference each
// via alloc.ptr(). Unlike build_canonical_key above, these ARE real offsets,
// so this function is not affected by the clause-ID-vs-offset discrepancy.
// ---------------------------------------------------------------
std::vector<uint64_t> compute_static_wl_labels(
    uint32_t num_vars,
    const std::vector<ClauseOfs>& long_irred_cls,
    const LiteralIndexedVector<LitWatchList>& watches,
    const LiteralIndexedVector<TriValue>& values,
    const ClauseAllocator& alloc,
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

  // Long original clauses — iterate long_irred_cls directly (§4.1 rewrite).
  for (const ClauseOfs ofs : long_irred_cls) {
    const Clause& cl = *alloc.ptr(ofs);
    bool sat = false;
    unsigned active = 0;
    for (const Lit l : cl) {
      if (values[l] == T_TRI) { sat = true; break; }
      if (values[l] == X_TRI) active++;
    }
    if (!sat && active >= 2) {
      CRef r; r.is_binary = false; r.ofs = ofs;
      r.va = r.vb = 0; r.la = r.lb = 0;
      refs.push_back(r);
    }
  }

  // Binary clauses (each undirected pair once). Filter by BinCl::irred().
  for (unsigned v = 1; v <= num_vars; v++) {
    for (int sgn = 0; sgn <= 1; sgn++) {
      Lit lit(v, sgn == 0);
      if (values[lit] == T_TRI) continue;
      for (const auto& bincl : watches[lit].binaries) {
        if (!bincl.irred()) continue;
        const Lit other = bincl.lit();
        if (values[other] == T_TRI) continue;
        const unsigned other_var = other.var();
        if (other_var <= v) continue;  // each pair once
        CRef r; r.is_binary = true;
        r.va = v; r.vb = other_var;
        r.la = lit.val(); r.lb = other.val();
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
      const Clause& cl = *alloc.ptr(r.ofs);
      for (const Lit l : cl) {
        if (values[l] != X_TRI) continue;
        if (l.sign()) pos_count[l.var()]++;
        else neg_count[l.var()]++;
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
    auto contrib = [&](unsigned var, bool pos_lit) {
      if (values[Lit(var, true)] != X_TRI) return;
      len++;
      if (is_singleton[var]) { ns++; return; }
      bool norm_pos = pos_lit != is_flip[var];
      if (norm_pos) np++; else nn++;
    };
    if (!r.is_binary) {
      const Clause& cl = *alloc.ptr(r.ofs);
      for (const Lit l : cl) contrib(l.var(), l.sign());
    } else {
      contrib(r.va, r.la > 0);
      contrib(r.vb, r.lb > 0);
    }
    ClauseType ct = {len, np, nn, ns};
    auto hh = g_clause_type_dict.lookup(ct);
    auto add_for_var = [&](unsigned var, bool pos_lit) {
      if (is_singleton[var]) return;
      if (values[Lit(var, true)] != X_TRI) return;
      bool norm_pos = pos_lit != is_flip[var];
      labels[var] += norm_pos ? hh.first : hh.second;
    };
    if (!r.is_binary) {
      const Clause& cl = *alloc.ptr(r.ofs);
      for (const Lit l : cl) add_for_var(l.var(), l.sign());
    } else {
      add_for_var(r.va, r.la > 0);
      add_for_var(r.vb, r.lb > 0);
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
        if (values[Lit(var, true)] != X_TRI) return;
        clause_h += mix64(labels[var]);
      };
      auto add_sig = [&](unsigned var) {
        if (is_singleton[var]) return;
        if (values[Lit(var, true)] != X_TRI) return;
        sig[var] += clause_h - mix64(labels[var]);
      };
      if (!r.is_binary) {
        const Clause& cl = *alloc.ptr(r.ofs);
        for (const Lit l : cl) add_clause_h(l.var());
        for (const Lit l : cl) add_sig(l.var());
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

}  // namespace GanakInt
