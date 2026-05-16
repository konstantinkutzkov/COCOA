// Test: canonical key invariance under learned clauses.
//
// Every flavor of learned clause (long or binary, however obtained)
// is redundant with respect to the original formula. The canonical
// cache key must therefore be a function of the ORIGINAL content
// only — injecting learned clauses over the sub-component's active
// variables must leave the key bit-identical.
//
// Tested scenarios:
//   1. Baseline: compute key K0 on the raw (post-preprocess) formula.
//   2. Inject a learned 3-clause, compute key K1. Assert K0 == K1.
//   3. Inject a learned binary, compute key K2. Assert K0 == K2.
//   4. Cumulative: inject both (both are live in the pool).
//      Compute key K3. Assert K0 == K3.
//
// If any K_i differs from K0, the canonical-key machinery has
// stopped filtering learned clauses — a regression that would
// silently fragment cache entries by learning history.
//
// Usage: test_canonical_key_learned <cnf-file>
// Exits 0 on pass, 1 on fail.

#include "solver.h"

#include <iostream>
#include <string>
#include <vector>

static bool keys_equal(const CanonicalKey &a, const CanonicalKey &b) {
    return a.hash == b.hash
        && a.hash_hi == b.hash_hi
        && a.num_vars == b.num_vars
        && a.n_in_clauses == b.n_in_clauses
        && a.num_clauses == b.num_clauses;
}

static void dump(const char *label, const CanonicalKey &k) {
    std::cout << label
              << ": hash=" << k.hash
              << " hash_hi=" << k.hash_hi
              << " nvars=" << k.num_vars
              << " n_in_clauses=" << k.n_in_clauses
              << " nclauses=" << k.num_clauses << "\n";
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <cnf-file>\n";
        return 2;
    }
    const std::string path = argv[1];

    // --- K0: raw key, no learned clauses injected ---
    Solver h0;
    h0.config().quiet = true;
    h0._prepareForKeyComputation(path);
    CanonicalKey k0 = h0._computeRootCanonicalKey();
    dump("K0 (raw)           ", k0);

    if (k0.num_vars < 3) {
        std::cerr << "fixture error: CNF has fewer than 3 active vars after "
                  << "preprocessing, cannot inject a length-3 learned clause\n";
        return 2;
    }

    if (k0.num_clauses == 0) {
        std::cerr << "fixture error: canonical key has no clauses, cannot "
                  << "construct a learned clause over active variables\n";
        return 2;
    }
    // Pull three active variable IDs from the (post-preprocess,
    // post-compact) solver. Iterate variables directly.
    std::vector<int> active_vars;
    for (unsigned v = 1; v <= h0._numVariablesForTest() && active_vars.size() < 3; v++) {
        if (h0._isActiveForTest(v)) active_vars.push_back((int)v);
    }
    if (active_vars.size() < 3) {
        std::cerr << "fixture error: fewer than 3 active variables\n";
        return 2;
    }

    // --- K1: inject a learned 3-clause over active vars ---
    Solver h1;
    h1.config().quiet = true;
    h1._prepareForKeyComputation(path);
    h1._injectLearnedLongClauseForTest({active_vars[0], active_vars[1], active_vars[2]});
    CanonicalKey k1 = h1._computeRootCanonicalKey();
    dump("K1 (+learned long) ", k1);

    // --- K2: inject a learned binary over active vars ---
    Solver h2;
    h2.config().quiet = true;
    h2._prepareForKeyComputation(path);
    h2._injectLearnedBinaryForTest(active_vars[0], active_vars[1]);
    CanonicalKey k2 = h2._computeRootCanonicalKey();
    dump("K2 (+learned bin)  ", k2);

    // --- K3: both ---
    Solver h3;
    h3.config().quiet = true;
    h3._prepareForKeyComputation(path);
    h3._injectLearnedLongClauseForTest({active_vars[0], active_vars[1], active_vars[2]});
    h3._injectLearnedBinaryForTest(active_vars[0], active_vars[1]);
    CanonicalKey k3 = h3._computeRootCanonicalKey();
    dump("K3 (+both)         ", k3);

    // --- Assertions ---
    bool ok = true;
    if (!keys_equal(k0, k1)) {
        std::cerr << "FAIL: K0 != K1 (learned long clause affected the key)\n";
        ok = false;
    }
    if (!keys_equal(k0, k2)) {
        std::cerr << "FAIL: K0 != K2 (learned binary affected the key)\n";
        ok = false;
    }
    if (!keys_equal(k0, k3)) {
        std::cerr << "FAIL: K0 != K3 (combined learned content affected the key)\n";
        ok = false;
    }
    if (ok) {
        std::cout << "PASS: canonical key is invariant under learned-clause injection\n";
        return 0;
    }
    return 1;
}
