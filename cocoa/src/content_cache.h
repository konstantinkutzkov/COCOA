/*
 * content_cache.h
 *
 * Content-based component cache using canonical hash keys.
 * Cache entry is just hash + sanity counts + model count.
 * No stored clause vectors — relies on hash quality for correctness.
 */

#ifndef CONTENT_CACHE_H_
#define CONTENT_CACHE_H_

#include <unordered_map>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
#include <gmpxx.h>

#include "canonical_key.h"

// ---------------------------------------------------------------
// L1 (identity-based) cache key: sub-component fingerprint using
// the CURRENT run's variable and clause IDs (stable within a run).
//
// Stores a 128-bit hash (two independent 64-bit halves) of the
// {active var IDs, active clause IDs} multiset. The two halves are
// computed in the same iteration pass at Component construction
// time, using two different mixing constants so the halves are
// independent.
//
// Equality compares both halves. No stored clause content, no
// structural equality check. At 128 bits and < 10^9 entries the
// birthday-collision probability is ~10^-20, functionally zero —
// wrong-answer risk from L1 hash collisions is eliminated.
//
// Systematic bugs in the canonicalization that would make two
// genuinely different formulas produce the same hash are NOT
// caught by this scheme (both halves would collide together).
// Those are guarded against by regression tests and oracle
// cross-validation, not by the hash width.
// ---------------------------------------------------------------
struct IdKey {
  uint64_t hash_lo = 0;
  uint64_t hash_hi = 0;

  bool operator==(const IdKey &o) const {
    return hash_lo == o.hash_lo && hash_hi == o.hash_hi;
  }
};

struct IdKeyHasher {
  // unordered_map buckets by this hash; cheap to use the low half.
  size_t operator()(const IdKey &k) const { return k.hash_lo; }
};

class ContentCache {
public:
  // Returns: MISS if not cached; HIT if cached.
  bool lookup(const CanonicalKey &key, mpz_class &count) {
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      count = it->second;
      stats_hits++;
      stats_hit_vars_sum += key.num_vars;
      if (key.num_vars > stats_max_hit_size) stats_max_hit_size = key.num_vars;
      stats_hit_buckets[size_bucket(key.num_vars)]++;
      return true;
    }
    stats_misses++;
    return false;
  }

  void store(const CanonicalKey &key, const mpz_class &count) {
    assert(count >= 0 && "Cached count must be non-negative");
    if (max_entries > 0 && cache_.size() >= max_entries)
      evict();
    // Flag a store-over-existing with a different value: this can only
    // trigger if a caller forgets to lookup before store. Belt-and-
    // suspenders.
    auto existing = cache_.find(key);
    if (existing != cache_.end() && existing->second != count) {
      std::cerr << "\n*** CACHE_STORE_COLLISION (overwrite with different value) ***\n"
                << "  key.hash        = 0x" << std::hex << key.hash << std::dec << "\n"
                << "  key.num_vars    = " << key.num_vars << "\n"
                << "  key.num_clauses = " << key.num_clauses << "\n"
                << "  old_count = " << existing->second << "\n"
                << "  new_count = " << count << "\n";
      std::cerr.flush();
      std::abort();
    }
    cache_[key] = count;
    stats_stores++;
    stats_store_vars_sum += key.num_vars;
    if (key.num_vars > stats_max_store_size) stats_max_store_size = key.num_vars;
    stats_store_buckets[size_bucket(key.num_vars)]++;
  }

  // Read a stored count without updating hit-rate stats.
  bool peek(const CanonicalKey &key, mpz_class &out) const {
    auto it = cache_.find(key);
    if (it == cache_.end()) return false;
    out = it->second;
    return true;
  }

  size_t size() const { return cache_.size(); }

  // L1 (identity-based) cache API. Fast-path: check L1 before
  // building the canonical key. On L1 hit the canonical build is
  // skipped entirely.
  bool l1_lookup(const IdKey &k, mpz_class &count) {
    auto it = l1_cache_.find(k);
    if (it == l1_cache_.end()) { stats_l1_misses++; return false; }
    count = it->second;
    stats_l1_hits++;
    return true;
  }

  void l1_store(const IdKey &k, const mpz_class &count) {
    assert(count >= 0 && "L1 count must be non-negative");
    // Same eviction policy as L2 — if we're over capacity, drop half.
    if (l1_max_entries > 0 && l1_cache_.size() >= l1_max_entries) {
      size_t to_remove = l1_cache_.size() / 2;
      auto it = l1_cache_.begin();
      for (size_t i = 0; i < to_remove && it != l1_cache_.end(); i++)
        it = l1_cache_.erase(it);
    }
    l1_cache_[k] = count;
    stats_l1_stores++;
  }

  size_t l1_size() const { return l1_cache_.size(); }

  // Configuration
  size_t max_entries = 1000000;
  size_t l1_max_entries = 1000000;

  // Statistics
  unsigned long stats_hits = 0;
  unsigned long stats_misses = 0;
  unsigned long stats_stores = 0;
  unsigned long stats_l1_hits = 0;
  unsigned long stats_l1_misses = 0;
  unsigned long stats_l1_stores = 0;
  // Accumulators: sum of num_vars at hit/store events on the L2
  // (canonical-key) cache. avg = sum / count tells us how big the
  // components going through the cache are. Large avg-at-hit means
  // each cache hit saves work on a big sub-formula — a real
  // amplification signal beyond hit rate.
  unsigned long long stats_hit_vars_sum = 0;
  unsigned long long stats_store_vars_sum = 0;
  // Tracking exponential-cost-relevant distribution: max hit size +
  // bucketed histogram of hit sizes. A handful of very large hits
  // dominate runtime savings; the mean is misleading for exponential
  // search trees.
  unsigned stats_max_hit_size = 0;
  unsigned stats_max_store_size = 0;
  // buckets: [0-25, 25-50, 50-100, 100-200, 200-400, 400+]
  unsigned long stats_hit_buckets[6]   = {0,0,0,0,0,0};
  unsigned long stats_store_buckets[6] = {0,0,0,0,0,0};
  static int size_bucket(unsigned n) {
    if (n < 25)  return 0;
    if (n < 50)  return 1;
    if (n < 100) return 2;
    if (n < 200) return 3;
    if (n < 400) return 4;
    return 5;
  }

private:
  std::unordered_map<CanonicalKey, mpz_class, CanonicalKeyHash> cache_;
  std::unordered_map<IdKey, mpz_class, IdKeyHasher> l1_cache_;

  void evict() {
    if (cache_.size() > 0) {
      auto it = cache_.begin();
      size_t to_remove = cache_.size() / 2;
      for (size_t i = 0; i < to_remove && it != cache_.end(); i++)
        it = cache_.erase(it);
    }
  }
};

#endif /* CONTENT_CACHE_H_ */
