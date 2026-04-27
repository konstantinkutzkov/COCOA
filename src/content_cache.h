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
  unsigned long stats_verify_checks = 0;
  unsigned long stats_verify_ok = 0;
  unsigned long stats_l1_hits = 0;
  unsigned long stats_l1_misses = 0;
  unsigned long stats_l1_stores = 0;

private:
  std::unordered_map<CanonicalKey, mpz_class, CanonicalKeyHash> cache_;
  std::unordered_map<IdKey, mpz_class, IdKeyHasher> l1_cache_;
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
