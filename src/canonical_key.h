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
// Canonical key: hash for fast bucket lookup + normalized clause
// multiset for structural equality.
//
// `clauses` stores, for each active clause, a sorted vector of
// canonical literals (positive = canonical_id, negative = -canonical_id,
// 0 = singleton marker). The outer vector is sorted lexicographically.
//
// Equality requires hash match AND exact structural match. This closes
// the systematic-collision hole: two non-isomorphic sub-components
// that happen to share a 64-bit hash will differ in `clauses` and be
// correctly distinguished.
// ---------------------------------------------------------------
struct CanonicalKey {
  uint64_t hash = 0;
  unsigned num_vars = 0;
  unsigned num_clauses = 0;
  std::vector<std::vector<int>> clauses;  // normalized multiset

  bool operator==(const CanonicalKey &other) const {
    if (hash != other.hash) return false;
    if (num_vars != other.num_vars) return false;
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
    unsigned original_lit_pool_size);

#endif /* CANONICAL_KEY_H_ */
