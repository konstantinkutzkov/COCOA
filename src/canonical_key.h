/*
 * canonical_key.h
 *
 * Content-based canonical key for component caching.
 *
 * Builds a canonical representation of a CNF sub-formula:
 * 1. Collects active clauses (long + binary, excluding learned and removed)
 * 2. Identifies singleton variables (appear exactly once)
 * 3. Replaces singleton literals with marker SINGLETON_MARKER
 * 4. Normalizes non-singleton polarity (orient so positive >= negative)
 * 5. Sorts literals within clauses, sorts clauses lexicographically
 * 6. Computes hash for fast lookup
 *
 * The canonical key captures the actual formula content, not internal IDs.
 * Two structurally identical sub-formulas produce the same key.
 */

#ifndef CANONICAL_KEY_H_
#define CANONICAL_KEY_H_

#include <vector>
#include <algorithm>
#include <unordered_map>
#include <cstdint>

#include "primitive_types.h"
#include "structures.h"
#include "containers.h"

class Component;  // forward declaration

static const int SINGLETON_MARKER = 0;

struct CanonicalKey {
  std::vector<std::vector<int>> clauses;
  size_t hash = 0;

  bool operator==(const CanonicalKey &other) const {
    return hash == other.hash && clauses == other.clauses;
  }

  void computeHash() {
    // Hash the sorted clause multiset as a flat byte sequence (FNV-1a)
    // Clauses must already be sorted before calling this
    size_t h = 14695981039346656037ULL;
    for (const auto &cl : clauses) {
      for (int lit : cl) {
        h ^= (size_t)(unsigned)lit;
        h *= 1099511628211ULL;
      }
      // Clause separator
      h ^= 0xFFFFFFFFU;
      h *= 1099511628211ULL;
    }
    hash = h;
  }
};

struct CanonicalKeyHash {
  size_t operator()(const CanonicalKey &k) const { return k.hash; }
};

// Comparison for sorting literals: by variable ID, positive before negative
inline bool litLess(int a, int b) {
  int va = a > 0 ? a : -a;
  int vb = b > 0 ? b : -b;
  if (va != vb) return va < vb;
  return a > b;  // positive before negative
}

// Declaration only — implementation in canonical_key.cpp
CanonicalKey buildCanonicalKey(
    Component &comp,
    const std::vector<LiteralID> &literal_pool,
    const LiteralIndexedVector<Literal> &literals,
    const LiteralIndexedVector<TriValue> &literal_values,
    const std::vector<ClauseOfs> &clause_id_to_ofs,
    const std::unordered_map<ClauseOfs, unsigned> &removed_clauses,
    unsigned original_lit_pool_size);

#endif /* CANONICAL_KEY_H_ */
