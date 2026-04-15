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
#include <iostream>
#include <gmpxx.h>

#include "canonical_key.h"

class ContentCache {
public:
  // Verify mode: when enabled, lookup() always reports a miss so the
  // solver recomputes each sub-component. store() then checks the
  // recomputed count against the previously stored value for the same
  // key; a mismatch is a cache-key collision bug.
  //
  // When a mismatch is detected, the caller-supplied on_mismatch_
  // callback (optional) is invoked with (key, stored_count, new_count).
  // Whether or not a callback runs, the process is aborted.
  bool verify_mode = false;

  // Returns: MISS if not cached; HIT if cached.
  // In verify_mode, HIT is converted to MISS after stashing the stored
  // count into pending_check_ so store() can compare.
  bool lookup(const CanonicalKey &key, mpz_class &count) {
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      if (verify_mode) {
        stats_verify_checks++;
        pending_has_ = true;
        pending_key_ = key;
        pending_stored_ = it->second;
        return false; // force recomputation
      }
      count = it->second;
      stats_hits++;
      return true;
    }
    stats_misses++;
    return false;
  }

  void store(const CanonicalKey &key, const mpz_class &count) {
    assert(count >= 0 && "Cached count must be non-negative");
    if (verify_mode) {
      auto it = cache_.find(key);
      if (it != cache_.end()) {
        if (it->second != count) {
          std::cerr << "\n*** CACHE_KEY_COLLISION DETECTED ***\n"
                    << "  key.hash    = 0x" << std::hex << key.hash << std::dec << "\n"
                    << "  key.num_vars    = " << key.num_vars << "\n"
                    << "  key.num_clauses = " << key.num_clauses << "\n"
                    << "  stored_count = " << it->second << "\n"
                    << "  new_count    = " << count << "\n"
                    << "  diff         = " << (count - it->second) << "\n";
          std::cerr.flush();
          std::abort();
        }
        stats_verify_ok++;
        return; // leave existing entry as-is
      }
      // First time for this key — store it.
    }
    if (max_entries > 0 && cache_.size() >= max_entries)
      evict();
    // Even outside verify_mode, flag a store-over-existing with a
    // different value: in non-verify mode this is silenced by the
    // cache hit path, so it only triggers if a caller forgets to
    // lookup before store. Kept here for belt-and-suspenders.
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
  }

  // Used by the solver's verify-mode store path to get the previously
  // stored count for a key (e.g., to dump component details alongside
  // the mismatch). Returns false if not cached.
  bool peek(const CanonicalKey &key, mpz_class &out) const {
    auto it = cache_.find(key);
    if (it == cache_.end()) return false;
    out = it->second;
    return true;
  }

  // Consumed by solver to know if a stored value was stashed at the
  // most recent lookup() call — helps the caller decide whether to
  // pre-dump the "first-occurrence" component.
  bool consumePending(CanonicalKey &key, mpz_class &stored) {
    if (!pending_has_) return false;
    key = pending_key_;
    stored = pending_stored_;
    pending_has_ = false;
    return true;
  }

  size_t size() const { return cache_.size(); }

  // Configuration
  size_t max_entries = 1000000;

  // Statistics
  unsigned long stats_hits = 0;
  unsigned long stats_misses = 0;
  unsigned long stats_stores = 0;
  unsigned long stats_verify_checks = 0;
  unsigned long stats_verify_ok = 0;

private:
  std::unordered_map<CanonicalKey, mpz_class, CanonicalKeyHash> cache_;
  bool pending_has_ = false;
  CanonicalKey pending_key_;
  mpz_class pending_stored_;

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
