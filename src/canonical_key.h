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
  size_t hash1 = 14695981039346656037ULL;
  size_t hash2 = 6364136223846793005ULL;
  std::vector<std::vector<int>> clauses;  // TODO: remove after debugging

  bool operator==(const CanonicalKey &other) const {
    return hash1 == other.hash1 && hash2 == other.hash2
           && clauses == other.clauses;
  }

  void computeHash() {
    hash1 = 14695981039346656037ULL;
    hash2 = 6364136223846793005ULL;
    // XOR per-clause hashes for order independence
    for (const auto &cl : clauses) {
      size_t ch1 = 0, ch2 = 0;
      for (int lit : cl) {
        ch1 = ch1 * 1099511628211ULL ^ (size_t)(unsigned)lit;
        ch2 = ch2 * 14695981039346656037ULL ^ (size_t)(unsigned)lit;
      }
      hash1 ^= ch1;
      hash2 ^= ch2;
    }
  }
};

struct CanonicalKeyHash {
  size_t operator()(const CanonicalKey &k) const { return k.hash1; }
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
