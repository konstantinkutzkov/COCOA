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
#include <unordered_set>
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

// Cache entry: the structural count plus the -cachePurge TAINT bit.
// tainted = a non-local ("phantom") learned/redundant clause fired during
// the subtree that computed this count, OR the count consumed a tainted
// cache entry — i.e., the value is certified equal to the pure component
// count only while its storing context's siblings are all satisfiable.
// Tainted entries are journaled (purgeable); pure entries never need
// purging (restriction-of-resolution + component-connectivity argument).
// Taint must propagate THROUGH hits: a consumer that multiplies in a
// tainted value inherits the context-dependence.
struct CacheVal {
  mpz_class count;
  bool tainted = false;
  CacheVal() = default;
  CacheVal(const mpz_class &c, bool t) : count(c), tainted(t) {}
};

class ContentCache {
public:
  // Returns: MISS if not cached; HIT if cached.
  // NOTE: dead code in production (zero call sites; both live L2 hit
  // paths use peek()). Kept for ABI/diagnostics.
  bool lookup(const CanonicalKey &key, mpz_class &count) {
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      count = it->second.count;
      stats_hits++;
      stats_hit_vars_sum += key.num_vars;
      if (key.num_vars > stats_max_hit_size) stats_max_hit_size = key.num_vars;
      stats_hit_buckets[size_bucket(key.num_vars)]++;
      return true;
    }
    stats_misses++;
    return false;
  }

  void store(const CanonicalKey &key, const mpz_class &count,
             bool tainted = true) {
    assert(count >= 0 && "Cached count must be non-negative");
    if (max_bytes_ > 0 && cur_bytes_ >= max_bytes_)
      evict();
    // Flag a store-over-existing with a different value: this can only
    // trigger if a caller forgets to lookup before store. Belt-and-
    // suspenders.
    auto existing = cache_.find(key);
    if (existing != cache_.end() && existing->second.count != count) {
      std::cerr << "\n*** CACHE_STORE_COLLISION (overwrite with different value) ***\n"
                << "  key.hash        = 0x" << std::hex << key.hash << std::dec << "\n"
                << "  key.num_vars    = " << key.num_vars << "\n"
                << "  key.num_clauses = " << key.num_clauses << "\n"
                << "  old_count = " << existing->second.count << "\n"
                << "  new_count = " << count << "\n";
      std::cerr.flush();
      std::abort();
    }
    bool is_new = (existing == cache_.end());
    cache_[key] = CacheVal{count, tainted};
    if (is_new) cur_bytes_ += l2_entry_bytes(key, count);
    // -cachePurge journal (Level-1 taint refinement): journal ONLY
    // TAINTED stores made inside an open branch arm — pure counts are
    // context-free and may survive any failure. No mark open (root
    // decomposition spine) => nothing to journal: a zero there ends the
    // run with the always-correct live count.
    if (purge_mode_ != 0 && !marks_.empty()) {
      if (tainted) {
        stats_tainted_stores++;
        if (!journal_saturated_) {
          journal_l2_.push_back(key);
          noteJournalGrowth();
        }
      } else {
        stats_pure_stores++;
      }
    }
    stats_stores++;
    stats_store_vars_sum += key.num_vars;
    if (key.num_vars > stats_max_store_size) stats_max_store_size = key.num_vars;
    stats_store_buckets[size_bucket(key.num_vars)]++;
  }

  // Read a stored count without updating hit-rate stats. If tainted_out
  // is non-null it receives the entry's taint bit (consumers MUST inherit
  // taint into their own solve, or the purge can be bypassed via a
  // pure-looking parent absorbing a poisoned child value).
  bool peek(const CanonicalKey &key, mpz_class &out,
            bool *tainted_out = nullptr) const {
    auto it = cache_.find(key);
    if (it == cache_.end()) return false;
    // -cachePurge mode 2: count hits landing on entries a real purge
    // would have erased — the run's soundness exposure AND the upper
    // bound on memoization lost to mode 1. Var-weighted sum because
    // recompute cost scales with component size (the mean misleads;
    // see the hit-bucket comment below). Valid only with deriv/dive
    // features off (their diagnostic peeks would inflate the counts).
    if (purge_mode_ == 2 && !dead_l2_.empty() && dead_l2_.count(key)) {
      stats_dead_hits++;
      stats_dead_hit_vars_sum += key.num_vars;
    }
    out = it->second.count;
    if (tainted_out) *tainted_out = it->second.tainted;
    return true;
  }

  size_t size() const { return cache_.size(); }

  // L1 (identity-based) cache API. Fast-path: check L1 before
  // building the canonical key. On L1 hit the canonical build is
  // skipped entirely.
  bool l1_lookup(const IdKey &k, mpz_class &count,
                 bool *tainted_out = nullptr) {
    auto it = l1_cache_.find(k);
    if (it == l1_cache_.end()) { stats_l1_misses++; return false; }
    // -cachePurge mode 2: see peek().
    if (purge_mode_ == 2 && !dead_l1_.empty() && dead_l1_.count(k))
      stats_dead_hits_l1++;
    count = it->second.count;
    if (tainted_out) *tainted_out = it->second.tainted;
    stats_l1_hits++;
    return true;
  }

  void l1_store(const IdKey &k, const mpz_class &count, bool tainted = true) {
    assert(count >= 0 && "L1 count must be non-negative");
    // Byte-bounded: trigger the same combined L1+L2 eviction when over budget.
    if (max_bytes_ > 0 && cur_bytes_ >= max_bytes_)
      evict();
    bool is_new = (l1_cache_.find(k) == l1_cache_.end());
    l1_cache_[k] = CacheVal{count, tainted};
    if (is_new) cur_bytes_ += l1_entry_bytes(k, count);
    // -cachePurge journal: see store() — tainted stores only.
    if (purge_mode_ != 0 && !marks_.empty()) {
      if (tainted) {
        stats_tainted_stores++;
        if (!journal_saturated_) {
          journal_l1_.push_back(k);
          noteJournalGrowth();
        }
      } else {
        stats_pure_stores++;
      }
    }
    stats_l1_stores++;
  }

  size_t l1_size() const { return l1_cache_.size(); }

  // ---------------------------------------------------------------
  // Learned-clause cache-pollution defense (-cachePurge).
  //
  // Reinstates the protection removed in commit e9f8df6 (the ancestor
  // 9ff3ea4 had `removeAllCachePollutionsOf` in solver.cpp:325). A
  // component count computed while a globally-learned clause pruned
  // inside its solve equals the pure local count ONLY while every
  // sibling component in the residual is satisfiable (containment
  // theorem); in a context where a sibling is UNSAT the node product
  // is 0 anyway, but the too-small count must not OUTLIVE that
  // context via the cache. Therefore: every store made inside an open
  // branch arm is journaled, and when an arm resolves to 0 (genuine
  // failure, not timeout) the journaled range is purged.
  //
  //   purge_mode_ 0 = off (journal fully inert; today's behavior)
  //               1 = purge stores made under failed branch arms
  //               2 = diagnostic: mark would-be-purged entries dead and
  //                   count hits on them; results unchanged
  //
  // Discipline (load-bearing): popMarkSuccess() does NOT truncate the
  // journal — an ancestor arm may still fail and must purge entries
  // stored under its completed-successful descendants (the
  // C1-stored-then-sibling-C2-UNSAT case). Journals clear only when
  // the outermost mark pops. popMarkFailed() truncates to its mark, so
  // each journal slot is purged at most once (hard work ceiling).
  // ---------------------------------------------------------------
  void pushMark() {
    marks_.emplace_back(journal_l2_.size(), journal_l1_.size());
  }

  void popMarkSuccess() {
    assert(!marks_.empty());
    marks_.pop_back();
    if (marks_.empty()) {
      // OUTERMOST arm completed successfully: no enclosing failure exists,
      // so every component that was open during any firing under this arm
      // proved satisfiable -> all phantom prunings were vacuous (containment
      // theorem) -> every surviving journaled (tainted) entry's count equals
      // the pure count. PROMOTE them to pure so future consumers don't
      // inherit stale taint. (Entries stored under inner FAILED arms were
      // already purged/truncated and are not in the journal.)
      if (purge_mode_ == 1 && !journal_saturated_) {
        for (const auto &k : journal_l2_) {
          auto it = cache_.find(k);
          if (it != cache_.end() && it->second.tainted) {
            it->second.tainted = false;
            stats_promoted++;
          }
        }
        for (const auto &k : journal_l1_) {
          auto it = l1_cache_.find(k);
          if (it != l1_cache_.end() && it->second.tainted) {
            it->second.tainted = false;
            stats_promoted++;
          }
        }
      }
      journal_l2_.clear();
      journal_l1_.clear();
      journal_saturated_ = false;
    }
  }

  void popMarkFailed() {
    assert(!marks_.empty());
    auto mk = marks_.back();
    marks_.pop_back();
    stats_purge_events++;
    if (purge_mode_ == 1) {
      if (journal_saturated_) {
        // Saturated journal: some stores under this arm were NOT
        // journaled, so the only sound erase is everything. Pure
        // memoization loss (cache is memoization only).
        stats_purged_l2 += cache_.size();
        stats_purged_l1 += l1_cache_.size();
        cache_.clear();
        l1_cache_.clear();
        cur_bytes_ = 0;
        journal_l2_.clear();
        journal_l1_.clear();
        for (auto &m : marks_) m = {0, 0};
        journal_saturated_ = false;
        return;
      }
      for (size_t i = mk.first; i < journal_l2_.size(); ++i) {
        auto it = cache_.find(journal_l2_[i]);
        if (it != cache_.end()) {  // may already be evicted — fine
          cur_bytes_ -= l2_entry_bytes(it->first, it->second.count);
          cache_.erase(it);
          stats_purged_l2++;
        }
      }
      for (size_t i = mk.second; i < journal_l1_.size(); ++i) {
        auto it = l1_cache_.find(journal_l1_[i]);
        if (it != l1_cache_.end()) {
          cur_bytes_ -= l1_entry_bytes(it->first, it->second.count);
          l1_cache_.erase(it);
          stats_purged_l1++;
        }
      }
    } else if (purge_mode_ == 2) {
      for (size_t i = mk.first; i < journal_l2_.size(); ++i) {
        dead_l2_.insert(journal_l2_[i]);
        stats_dead_marked++;
      }
      for (size_t i = mk.second; i < journal_l1_.size(); ++i) {
        dead_l1_.insert(journal_l1_[i]);
        stats_dead_marked++;
      }
    }
    journal_l2_.resize(mk.first);
    journal_l1_.resize(mk.second);
    if (marks_.empty()) {
      journal_l2_.clear();
      journal_l1_.clear();
      journal_saturated_ = false;
    }
  }

  bool marksEmpty() const { return marks_.empty(); }
  size_t dead_l2_size() const { return dead_l2_.size(); }
  size_t dead_l1_size() const { return dead_l1_.size(); }

  // Configuration: cache memory budget in BYTES (0 = unbounded). Default
  // ~20 GB -- conservative headroom under the competition's 32 GB HARD
  // limit, since this byte count is an ESTIMATE and non-cache structures
  // also use RAM. Kept well above normal usage, so it only bites genuinely
  // heavy instances (and not so low it cripples cache-dependent ones). The
  // solver overrides it from the -cs flag (which writes statistics_
  // .maximum_cache_size_bytes_) when -cs is supplied. The cap covers BOTH
  // levels (L1 identity + L2 canonical) via cur_bytes_.
  uint64_t max_bytes_ = 20000ULL * 1000000ULL;
  uint64_t cur_bytes_ = 0;
  unsigned long stats_evictions = 0;

  // -cachePurge mode (see the pollution-defense block above). Wired from
  // config_.cache_purge_mode in solver.cpp next to the -cs cap.
  int purge_mode_ = 0;
  // Journal byte cap (keys only). The journal clears only when the
  // OUTERMOST open arm completes, so its peak tracks the store volume of
  // the largest outermost arm (~150 MB at probe169 scale). On overflow:
  // stop journaling (saturate); if a saturated scope later FAILS in mode
  // 1, flush both maps entirely (sound, pure memoization loss). 0 = uncapped.
  uint64_t journal_max_bytes_ = 2000ULL * 1000000ULL;

  // -cachePurge statistics (all inert at mode 0).
  unsigned long stats_purge_events = 0;
  unsigned long stats_purged_l2 = 0;
  unsigned long stats_purged_l1 = 0;
  uint64_t stats_journal_peak = 0;
  unsigned long stats_journal_overflows = 0;
  unsigned long stats_dead_marked = 0;
  // Taint refinement (Level 1): journal split + outermost-success promotions.
  unsigned long stats_tainted_stores = 0;
  unsigned long stats_pure_stores = 0;
  unsigned long stats_promoted = 0;
  // mutable: bumped inside const peek().
  mutable unsigned long stats_dead_hits = 0;
  mutable unsigned long stats_dead_hits_l1 = 0;
  mutable unsigned long long stats_dead_hit_vars_sum = 0;

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
  std::unordered_map<CanonicalKey, CacheVal, CanonicalKeyHash> cache_;
  std::unordered_map<IdKey, CacheVal, IdKeyHasher> l1_cache_;

  // -cachePurge state: store journals (keys by value), the per-arm
  // watermark stack, and (mode 2 only) the dead-entry marks.
  std::vector<CanonicalKey> journal_l2_;
  std::vector<IdKey> journal_l1_;
  std::vector<std::pair<size_t, size_t>> marks_;
  std::unordered_set<CanonicalKey, CanonicalKeyHash> dead_l2_;
  std::unordered_set<IdKey, IdKeyHasher> dead_l1_;
  bool journal_saturated_ = false;

  void noteJournalGrowth() {
    uint64_t jb = (uint64_t)journal_l2_.size() * sizeof(CanonicalKey)
                + (uint64_t)journal_l1_.size() * sizeof(IdKey);
    if (jb > stats_journal_peak) stats_journal_peak = jb;
    if (journal_max_bytes_ > 0 && jb > journal_max_bytes_) {
      journal_saturated_ = true;
      stats_journal_overflows++;
    }
  }

  // Estimated heap footprint of one cache entry: key + mpz limbs + a fixed
  // NODE_OVERHEAD approximating the unordered_map node pointer, malloc
  // header, and bucket slot. Rough by design (calibrate vs RSS before
  // trusting a high -cs); good enough to keep us off the OOM ceiling.
  static constexpr size_t NODE_OVERHEAD = 48;
  static size_t mpz_heap_bytes(const mpz_class &c) {
    return sizeof(mpz_class) + (size_t)mpz_size(c.get_mpz_t()) * sizeof(mp_limb_t);
  }
  static size_t l2_entry_bytes(const CanonicalKey &key, const mpz_class &c) {
    (void)key;
    return sizeof(CanonicalKey) + mpz_heap_bytes(c) + NODE_OVERHEAD;
  }
  static size_t l1_entry_bytes(const IdKey &k, const mpz_class &c) {
    (void)k;
    return sizeof(IdKey) + mpz_heap_bytes(c) + NODE_OVERHEAD;
  }

  // Memory-bounded eviction: drop ~50% of BOTH levels and decrement the
  // running byte estimate. SOUND — the cache is pure memoization, so an
  // eviction only forces recomputation, never a wrong count. Eviction is
  // by unordered_map iteration order (~arbitrary); a true LRU would need
  // per-entry generation counters (future refinement).
  void evict() {
    size_t l2rm = cache_.size() / 2;
    auto it = cache_.begin();
    for (size_t i = 0; i < l2rm && it != cache_.end(); i++) {
      cur_bytes_ -= l2_entry_bytes(it->first, it->second.count);
      it = cache_.erase(it);
    }
    size_t l1rm = l1_cache_.size() / 2;
    auto jt = l1_cache_.begin();
    for (size_t i = 0; i < l1rm && jt != l1_cache_.end(); i++) {
      cur_bytes_ -= l1_entry_bytes(jt->first, jt->second.count);
      jt = l1_cache_.erase(jt);
    }
    stats_evictions++;
  }
};

#endif /* CONTENT_CACHE_H_ */
