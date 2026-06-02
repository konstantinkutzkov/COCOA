// Test: canonical key invariance under free-variable addition.
//
// Build canonical_keys for two CNFs that have IDENTICAL clauses but
// differ in their total variable count (cnf2 has an extra var that
// does not appear in any clause — a "free variable"). A correct
// canonical_key must be structurally invariant under adding free
// vars, since the cache stores structural counts (dividing by
// 2^free_vars at store, multiplying back at retrieval).
//
// If keys differ, the cache will store duplicate entries for
// structurally-equivalent sub-formulas with different free-var
// counts, missing what should be cache hits.
//
// Usage: test_canonical_key_free_var <cnf-no-free> <cnf-with-free>

#include "solver.h"
#include <iostream>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <cnf-no-free> <cnf-with-free>\n";
        return 2;
    }

    Solver h1;
    h1.config().quiet = true;
    h1._prepareForKeyComputationNoPP(argv[1]);
    CanonicalKey k1 = h1._computeRootCanonicalKey();

    Solver h2;
    h2.config().quiet = true;
    h2._prepareForKeyComputationNoPP(argv[2]);
    CanonicalKey k2 = h2._computeRootCanonicalKey();

    std::cout << "cnf1 (" << argv[1] << "): hash=" << k1.hash
              << " hash_hi=" << k1.hash_hi
              << " nvars=" << k1.num_vars
              << " n_in_clauses=" << k1.n_in_clauses
              << " nclauses=" << k1.num_clauses << "\n";
    std::cout << "cnf2 (" << argv[2] << "): hash=" << k2.hash
              << " hash_hi=" << k2.hash_hi
              << " nvars=" << k2.num_vars
              << " n_in_clauses=" << k2.n_in_clauses
              << " nclauses=" << k2.num_clauses << "\n";

    bool same_hash = (k1.hash == k2.hash && k1.hash_hi == k2.hash_hi);
    if (same_hash) {
        std::cout << "PASS: canonical_key invariant under free-var addition\n";
        return 0;
    } else {
        std::cout << "FAIL: canonical_key DIFFERS — free vars affect hash\n";
        return 1;
    }
}
