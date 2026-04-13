/*
 * content_cache.h
 *
 * Content-based component cache using canonical formula keys.
 * Replaces the ID-based ComponentCache.
 *
 * Keys are canonical representations of the actual formula content
 * (see canonical_key.h). Two structurally identical sub-formulas
 * produce the same key, regardless of internal variable/clause IDs.
 */

#ifndef CONTENT_CACHE_H_
#define CONTENT_CACHE_H_

#include <unordered_map>
#include <cassert>
#include <gmpxx.h>

#include "canonical_key.h"

class ContentCache {
public:
  bool lookup(const CanonicalKey &key, mpz_class &count) {
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      count = it->second;
      stats_hits++;
      return true;
    }
    stats_misses++;
    return false;
  }

  void store(const CanonicalKey &key, const mpz_class &count) {
    assert(count >= 0 && "Cached count must be non-negative");
    if (max_entries > 0 && cache_.size() >= max_entries)
      evict();
    cache_[key] = count;
    stats_stores++;
  }

  size_t size() const { return cache_.size(); }

  // Configuration
  size_t max_entries = 1000000;

  // Statistics
  unsigned long stats_hits = 0;
  unsigned long stats_misses = 0;
  unsigned long stats_stores = 0;

private:
  std::unordered_map<CanonicalKey, mpz_class, CanonicalKeyHash> cache_;

  void evict() {
    // Simple: clear half the cache when full
    // A more sophisticated LRU could be added later
    if (cache_.size() > 0) {
      auto it = cache_.begin();
      size_t to_remove = cache_.size() / 2;
      for (size_t i = 0; i < to_remove && it != cache_.end(); i++)
        it = cache_.erase(it);
    }
  }
};

#endif /* CONTENT_CACHE_H_ */
