/*
 * canonical_key.h
 *
 * Fast canonical key for component caching using tabulation hashing.
 *
 * A global dictionary maps canonical clause types to random hash values.
 * Variable signatures are computed via one-pass accumulation of
 * dictionary values. No heap allocation per component.
 */

#ifndef CANONICAL_KEY_H_
#define CANONICAL_KEY_H_

#include <vector>
#include <algorithm>
#include <unordered_map>
#include <cstdint>
#include <cassert>
#include <random>

#include "primitive_types.h"
#include "structures.h"
#include "containers.h"

// ---------------------------------------------------------------
// Canonical clause type: structural properties of a clause
// ---------------------------------------------------------------
struct ClauseType {
  unsigned length;
  unsigned num_pos;   // positive non-singleton literals
  unsigned num_neg;   // negative non-singleton literals
  unsigned num_sing;  // singleton literals

  bool operator==(const ClauseType &o) const {
    return length == o.length && num_pos == o.num_pos
        && num_neg == o.num_neg && num_sing == o.num_sing;
  }
};

struct ClauseTypeHash {
  size_t operator()(const ClauseType &t) const {
    size_t h = t.length;
    h = h * 31 + t.num_pos;
    h = h * 31 + t.num_neg;
    h = h * 31 + t.num_sing;
    return h;
  }
};

// ---------------------------------------------------------------
// Global dictionary: clause type → random hash values
// ---------------------------------------------------------------
class ClauseTypeDictionary {
public:
  ClauseTypeDictionary() : rng_(42) {}

  // Get hash values for a clause type (creates entry if new)
  std::pair<uint64_t, uint64_t> lookup(const ClauseType &type) {
    auto it = dict_.find(type);
    if (it != dict_.end())
      return it->second;

    // Generate two independent random values for this type
    uint64_t h_pos = rng_() | (uint64_t(rng_()) << 32);
    uint64_t h_neg = rng_() | (uint64_t(rng_()) << 32);
    // Ensure non-zero
    if (h_pos == 0) h_pos = 1;
    if (h_neg == 0) h_neg = 1;
    dict_[type] = {h_pos, h_neg};
    return {h_pos, h_neg};
  }

  size_t size() const { return dict_.size(); }

private:
  std::unordered_map<ClauseType, std::pair<uint64_t, uint64_t>, ClauseTypeHash> dict_;
  std::mt19937 rng_;
};

// Global instance (shared across all components)
extern ClauseTypeDictionary g_clause_type_dict;

// ---------------------------------------------------------------
// Diagnostic stats for the anonymization pass.
// ---------------------------------------------------------------
struct CanonStats {
  long long n_calls = 0;
  long long sum_anchored = 0;
  long long sum_collision_block_vars = 0;
  long long sum_orientation_ambiguous_in_blocks = 0;
  long long calls_with_any_collision = 0;
  unsigned  max_block_size = 0;
  // Histogram of largest-block-per-call: bucket = floor(log2(size+1))
  long long max_block_buckets[16] = {0};  // up to size 32k+
  // After-iter-2 (if enabled), for direct comparison
  long long sum_anchored_iter2 = 0;
  long long sum_collision_block_vars_iter2 = 0;
  long long calls_with_any_collision_iter2 = 0;
  unsigned  max_block_size_iter2 = 0;
  long long max_block_buckets_iter2[16] = {0};
};
extern CanonStats g_canon_stats;

// ---------------------------------------------------------------
// Canonical key: two modes of equality.
//
// COMPACT (default):
//   Identity is a 128-bit hash — `hash` (low half) + `hash_hi` (high
//   half), each a different FNV sum over the canonical clauses. The
//   `clauses` vector is left empty. `operator==` compares only the
//   two 64-bit halves. At ~10^-20 birthday-collision probability,
//   wrong-answer risk from hash collisions is functionally zero.
//
// STRICT (debug-only, opt-in via -l2Strict):
//   Adds the full normalized clause multiset. `operator==` requires
//   the hash halves, counts, AND the full multiset to match. Closes
//   the systematic-collision hole at the cost of larger keys and a
//   deeper equality check. Useful for forensic debugging: if we see
//   a miscount under compact mode, flipping to strict tells us
//   whether the cause was a canonical-form hash collision (strict
//   fixes it) or a canonicalization fault (strict stays broken).
//
// The `compact` flag carries the mode. `CanonicalKeyHash` always
// buckets on the low half; only equality differs.
// ---------------------------------------------------------------
struct CanonicalKey {
  uint64_t hash = 0;            // low 64 bits of the 128-bit L2 hash
  uint64_t hash_hi = 0;         // high 64 bits
  unsigned num_vars = 0;        // total active (X_TRI) vars in the sub-component
  unsigned n_in_clauses = 0;    // active vars actually appearing in at least one canonical clause
  unsigned num_clauses = 0;
  bool compact = true;          // true: 128-bit pure hash; false: + clauses
  std::vector<std::vector<int>> clauses;  // strict mode only

  bool operator==(const CanonicalKey &other) const {
    if (hash != other.hash) return false;
    if (hash_hi != other.hash_hi) return false;
    if (compact && other.compact) return true;
    // Strict mode: use structural fields only. num_vars (which includes
    // free vars) is intentionally NOT checked — under the
    // structural-count cache scheme, two sub-components that share the
    // same active connected structure but differ only in their count of
    // free vars (vars not in any clause) MUST collide on this key, so
    // they share a single cache entry; the per-caller free-var factor
    // is applied at store/retrieve via 2^(num_vars - n_in_clauses).
    if (compact != other.compact) return false;
    if (n_in_clauses != other.n_in_clauses) return false;
    if (num_clauses != other.num_clauses) return false;
    return clauses == other.clauses;
  }
};

struct CanonicalKeyHash {
  size_t operator()(const CanonicalKey &k) const { return k.hash; }
};

// ---------------------------------------------------------------
// Build canonical key (declaration — implemented in canonical_key.cpp)
// ---------------------------------------------------------------
class Component;  // forward declaration

CanonicalKey buildCanonicalKey(
    Component &comp,
    const std::vector<LiteralID> &literal_pool,
    const LiteralIndexedVector<Literal> &literals,
    const LiteralIndexedVector<TriValue> &literal_values,
    const std::vector<ClauseOfs> &clause_id_to_ofs,
    const std::unordered_map<ClauseOfs, unsigned> &removed_clauses,
    unsigned original_lit_pool_size,
    bool compact = true,
    bool no_anonymization = false,
    int wl_iterations = 1,
    const std::vector<uint64_t> *static_wl_labels = nullptr);

// One-time global WL computation. Runs at preprocessing finish on
// the post-preprocessed formula's incidence/primal structure. Yields
// one uint64 label per variable index (1..n_vars; entry 0 unused).
// Cheap: ~O(num_clauses) per WL iteration, run once for the whole solve.
std::vector<uint64_t> computeStaticWLLabels(
    unsigned num_vars,
    const std::vector<LiteralID> &literal_pool,
    const LiteralIndexedVector<Literal> &literals,
    const LiteralIndexedVector<TriValue> &literal_values,
    unsigned original_lit_pool_size,
    int n_iters);

#endif /* CANONICAL_KEY_H_ */
