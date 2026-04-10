/*
 * content_cache.h
 *
 * Content-based component cache that hashes the actual formula content
 * (active literals in each clause), not just clause/variable IDs.
 *
 * The canonical form of a sub-formula:
 *   - Active variables sorted ascending
 *   - For each clause (long and binary): active literals sorted
 *     (by variable ID, positive before negative)
 *   - Clauses sorted lexicographically
 *   - Hash computed from this canonical representation
 *   - Full equality check on collision
 */

#ifndef CONTENT_CACHE_H_
#define CONTENT_CACHE_H_

#include <vector>
#include <algorithm>
#include <unordered_map>
#include <gmpxx.h>

#include "primitive_types.h"
#include "structures.h"
#include "containers.h"
#include "component_types/component.h"

struct ContentCacheKey {
  std::vector<unsigned> vars;           // sorted active variable IDs
  std::vector<std::vector<int>> clauses; // sorted clauses of sorted literals

  bool operator==(const ContentCacheKey &other) const {
    return vars == other.vars && clauses == other.clauses;
  }

  size_t computeHash() const {
    // FNV-1a style hash
    size_t h = 14695981039346656037ULL;
    for (unsigned v : vars) {
      h ^= v;
      h *= 1099511628211ULL;
    }
    // separator between vars and clauses
    h ^= 0xFFFFFFFF;
    h *= 1099511628211ULL;
    for (const auto &cl : clauses) {
      for (int lit : cl) {
        h ^= (size_t)(unsigned)lit;
        h *= 1099511628211ULL;
      }
      // clause separator
      h ^= 0;
      h *= 1099511628211ULL;
    }
    return h;
  }
};

struct ContentCacheKeyHash {
  size_t operator()(const ContentCacheKey &k) const {
    return k.computeHash();
  }
};

class ContentCache {
public:
  // Build a cache key from a component and the current formula state.
  // Includes both long clauses (from literal_pool_) and binary clauses
  // (from binary_links_).
  static ContentCacheKey buildKey(
      Component &comp,
      const std::vector<LiteralID> &literal_pool,
      const LiteralIndexedVector<Literal> &literals,
      const LiteralIndexedVector<TriValue> &literal_values,
      const std::vector<ClauseOfs> &clause_id_to_ofs,
      const std::unordered_map<ClauseOfs, unsigned> &removed_clauses,
      unsigned original_lit_pool_size) {

    ContentCacheKey key;

    // Collect active variables (already sorted in component)
    for (auto it = comp.varsBegin(); *it != varsSENTINEL; it++) {
      if (literal_values[LiteralID(*it, true)] == X_TRI)
        key.vars.push_back(*it);
    }

    // Build set of active vars for quick lookup
    std::vector<bool> var_active(key.vars.empty() ? 1 : key.vars.back() + 1, false);
    for (unsigned v : key.vars)
      var_active[v] = true;

    // Collect long clauses with their active literals
    for (auto it = comp.clsBegin(); *it != clsSENTINEL; it++) {
      ClauseOfs ofs = clause_id_to_ofs[*it];
      if (removed_clauses.count(ofs))
        continue;
      if (ofs >= original_lit_pool_size)
        continue;

      std::vector<int> clause_lits;
      bool satisfied = false;
      auto lt = literal_pool.begin() + ofs;
      for (; *lt != SENTINEL_LIT; lt++) {
        if (literal_values[*lt] == T_TRI) {
          satisfied = true;
          break;
        }
        if (literal_values[*lt] == X_TRI)
          clause_lits.push_back(lt->toInt());
      }
      if (satisfied || clause_lits.size() < 2)
        continue;

      // Sort literals: by variable ID, positive before negative
      std::sort(clause_lits.begin(), clause_lits.end(),
        [](int a, int b) {
          int va = a > 0 ? a : -a;
          int vb = b > 0 ? b : -b;
          if (va != vb) return va < vb;
          return a > b; // positive before negative
        });
      key.clauses.push_back(std::move(clause_lits));
    }

    // Collect binary clauses between active variables in the component
    for (unsigned v : key.vars) {
      for (int sign = 0; sign <= 1; sign++) {
        LiteralID lit(v, sign == 0);
        for (auto bt = literals[lit].binary_links_.begin();
             *bt != SENTINEL_LIT; bt++) {
          unsigned other_var = bt->var();
          // Only include each binary clause once (when v < other_var)
          // and only if other_var is in the component
          if (other_var <= v) continue;
          if (other_var >= var_active.size() || !var_active[other_var]) continue;
          if (literal_values[*bt] == T_TRI) continue; // clause satisfied

          int lit_int = lit.toInt();
          int other_int = bt->toInt();

          std::vector<int> bin_cl = {lit_int, other_int};
          std::sort(bin_cl.begin(), bin_cl.end(),
            [](int a, int b) {
              int va = a > 0 ? a : -a;
              int vb = b > 0 ? b : -b;
              if (va != vb) return va < vb;
              return a > b;
            });
          key.clauses.push_back(std::move(bin_cl));
        }
      }
    }

    // Sort all clauses lexicographically
    std::sort(key.clauses.begin(), key.clauses.end());

    return key;
  }

  bool lookup(const ContentCacheKey &key, mpz_class &count) {
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      count = it->second;
      stats_hits++;
      return true;
    }
    stats_misses++;
    return false;
  }

  void store(const ContentCacheKey &key, const mpz_class &count) {
    cache_[key] = count;
    stats_stores++;
  }

  // Stats
  unsigned long stats_hits = 0;
  unsigned long stats_misses = 0;
  unsigned long stats_stores = 0;

  size_t size() const { return cache_.size(); }

private:
  std::unordered_map<ContentCacheKey, mpz_class, ContentCacheKeyHash> cache_;
};

#endif /* CONTENT_CACHE_H_ */
