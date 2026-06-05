// Soundness test for ContentCache memory-bounded eviction (the -cs cap).
// Verifies: (1) the byte cap bounds the cache; (2) eviction NEVER corrupts a
// surviving entry's value (the property that makes eviction count-preserving);
// (3) eviction actually fires under a tiny cap; (4) max_bytes_=0 => unbounded.
// Build: g++ -std=c++17 -I ../src test_cache_evict.cpp -lgmpxx -lgmp
#include "content_cache.h"
#include <iostream>
#include <cstdint>

static mpz_class l2val(int i) { return mpz_class(i) * 1000003 + 7; }
static uint64_t  kh(int i)    { return (uint64_t)i * 0x9E3779B97F4A7C15ULL; }

int main() {
  int fail = 0;
  const int N = 20000;

  // ---- L2 (canonical) eviction under a tiny cap ----
  {
    ContentCache cc; cc.max_bytes_ = 8192;
    for (int i = 1; i <= N; i++) {
      CanonicalKey k; k.hash = kh(i); k.hash_hi = (uint64_t)i;
      k.num_vars = i % 500; k.n_in_clauses = i % 300; k.num_clauses = i % 400;
      mpz_class tmp;
      if (!cc.lookup(k, tmp)) cc.store(k, l2val(i));   // mimic solver: lookup-then-store
    }
    if (cc.cur_bytes_ > cc.max_bytes_ + 4096) { std::cerr << "FAIL L2 bound cur=" << cc.cur_bytes_ << "\n"; fail++; }
    if (cc.size() >= (size_t)N)               { std::cerr << "FAIL L2 no eviction size=" << cc.size() << "\n"; fail++; }
    if (cc.stats_evictions == 0)              { std::cerr << "FAIL L2 evictions=0\n"; fail++; }
    int verified = 0;
    for (int i = 1; i <= N; i++) {
      CanonicalKey k; k.hash = kh(i); k.hash_hi = (uint64_t)i; k.num_vars = i % 500;
      mpz_class got;
      if (cc.peek(k, got)) {
        if (got != l2val(i)) { std::cerr << "FAIL L2 CORRUPT survivor i=" << i << " got=" << got << " want=" << l2val(i) << "\n"; fail++; break; }
        verified++;
      }
    }
    std::cout << "L2: size=" << cc.size() << " cur_bytes=" << cc.cur_bytes_ << "/" << cc.max_bytes_
              << " evictions=" << cc.stats_evictions << " survivors_ok=" << verified << "\n";
  }

  // ---- L1 (identity) eviction under a tiny cap ----
  {
    ContentCache cc; cc.max_bytes_ = 8192;
    for (int i = 1; i <= N; i++) { IdKey k; k.hash_lo = kh(i); k.hash_hi = (uint64_t)i; cc.l1_store(k, mpz_class(i)); }
    if (cc.cur_bytes_ > cc.max_bytes_ + 4096) { std::cerr << "FAIL L1 bound cur=" << cc.cur_bytes_ << "\n"; fail++; }
    int verified = 0;
    for (int i = 1; i <= N; i++) {
      IdKey k; k.hash_lo = kh(i); k.hash_hi = (uint64_t)i; mpz_class got;
      if (cc.l1_lookup(k, got)) { if (got != mpz_class(i)) { std::cerr << "FAIL L1 CORRUPT i=" << i << "\n"; fail++; break; } verified++; }
    }
    std::cout << "L1: size=" << cc.l1_size() << " cur_bytes=" << cc.cur_bytes_ << "/" << cc.max_bytes_
              << " survivors_ok=" << verified << "\n";
  }

  // ---- max_bytes_=0 must DISABLE eviction (unbounded) ----
  {
    ContentCache cc; cc.max_bytes_ = 0;
    for (int i = 1; i <= 5000; i++) { CanonicalKey k; k.hash = kh(i); k.hash_hi = (uint64_t)i; mpz_class tmp; if (!cc.lookup(k, tmp)) cc.store(k, l2val(i)); }
    if (cc.size() != 5000)       { std::cerr << "FAIL unbounded evicted size=" << cc.size() << "\n"; fail++; }
    if (cc.stats_evictions != 0) { std::cerr << "FAIL unbounded evictions=" << cc.stats_evictions << "\n"; fail++; }
    std::cout << "UNBOUNDED(max_bytes=0): size=" << cc.size() << " evictions=" << cc.stats_evictions << " (expect 5000, 0)\n";
  }

  std::cout << (fail ? "*** FAIL ***\n" : "ALL PASS - eviction bounds memory, never corrupts survivors, off when max_bytes_=0\n");
  return fail ? 1 : 0;
}
