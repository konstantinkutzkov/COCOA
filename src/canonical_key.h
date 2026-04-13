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
#include <climits>
#include <unordered_map>
#include <cstdint>

#include "primitive_types.h"
#include "structures.h"
#include "containers.h"

class Component;  // forward declaration

static const int SINGLETON_MARKER = 0;

struct CanonicalKey {
  size_t hash = 0;
  // Flat sorted representation: clauses sorted, separated by INT_MIN
  std::vector<int> data;

  bool operator==(const CanonicalKey &other) const {
    return hash == other.hash && data == other.data;
  }

  // Add one clause (literals must already be sorted).
  // Clauses are accumulated unsorted; call finalize() when done.
  void addClause(const int *lits, unsigned len) {
    for (unsigned i = 0; i < len; i++)
      clause_buf.push_back(lits[i]);
    clause_buf.push_back(INT_MIN);  // separator
  }

  // Sort clauses and compute hash. Must be called after all addClause calls.
  void finalize() {
    // Parse clause_buf into individual clauses, sort them, flatten
    std::vector<std::vector<int>> clauses;
    std::vector<int> current;
    for (int v : clause_buf) {
      if (v == INT_MIN) {
        if (!current.empty()) {
          clauses.push_back(std::move(current));
          current.clear();
        }
      } else {
        current.push_back(v);
      }
    }
    std::sort(clauses.begin(), clauses.end());

    // Flatten into data
    data.clear();
    data.reserve(clause_buf.size());
    for (const auto &cl : clauses) {
      for (int v : cl) data.push_back(v);
      data.push_back(INT_MIN);
    }

    // Hash the flat data with FNV-1a
    hash = 14695981039346656037ULL;
    for (int v : data) {
      hash ^= (size_t)(unsigned)v;
      hash *= 1099511628211ULL;
    }

    clause_buf.clear();
    clause_buf.shrink_to_fit();
  }

private:
  std::vector<int> clause_buf;  // temporary buffer before finalize
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
