// Test: canonical key order-invariance.
//
// Build a solver state on a CNF, compute the canonical key for the
// super-component. Permute the stored-literal order within each clause
// of a second solver instance loaded from the same file, compute the
// key there, and compare.
//
// Under a correct implementation, the canonical key must be invariant
// under stored-literal permutations — otherwise two logically
// equivalent sub-components can hash to different cache keys (missing
// hits) or the same key (collisions), either of which can produce
// wrong model counts.
//
// Usage: test_canonical_key_invariance <cnf-file>
// Exits 0 on pass, 1 on fail.

#include "solver.h"

#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <cnf-file>\n";
        return 2;
    }
    const std::string path = argv[1];

    Solver h1;
    h1.config().quiet = true;
    h1._prepareForKeyComputation(path);
    CanonicalKey k_baseline = h1._computeRootCanonicalKey();

    Solver h2;
    h2.config().quiet = true;
    h2._prepareForKeyComputation(path);
    h2._permuteClauseLiteralsForTest(/*seed=*/42);
    CanonicalKey k_permuted = h2._computeRootCanonicalKey();

    bool match_hash    = (k_baseline.hash        == k_permuted.hash);
    bool match_nvars   = (k_baseline.num_vars    == k_permuted.num_vars);
    bool match_ncls    = (k_baseline.num_clauses == k_permuted.num_clauses);
    bool match_clauses = (k_baseline.clauses     == k_permuted.clauses);

    std::cout << "baseline: hash=" << k_baseline.hash
              << " nvars=" << k_baseline.num_vars
              << " nclauses=" << k_baseline.num_clauses << "\n";
    std::cout << "permuted: hash=" << k_permuted.hash
              << " nvars=" << k_permuted.num_vars
              << " nclauses=" << k_permuted.num_clauses << "\n";

    if (match_hash && match_nvars && match_ncls && match_clauses) {
        std::cout << "PASS: canonical key invariant under clause-literal permutation\n";
        return 0;
    }

    std::cerr << "FAIL: canonical key DIFFERS across clause-literal permutations\n";
    std::cerr << "  hash match:    " << (match_hash    ? "yes" : "NO") << "\n";
    std::cerr << "  nvars match:   " << (match_nvars   ? "yes" : "NO") << "\n";
    std::cerr << "  nclauses match:" << (match_ncls    ? "yes" : "NO") << "\n";
    std::cerr << "  clauses match: " << (match_clauses ? "yes" : "NO") << "\n";
    if (!match_clauses) {
        std::cerr << "  first differing clauses:\n";
        size_t n = std::min(k_baseline.clauses.size(), k_permuted.clauses.size());
        int diffs_shown = 0;
        for (size_t i = 0; i < n && diffs_shown < 5; i++) {
            if (k_baseline.clauses[i] == k_permuted.clauses[i]) continue;
            std::cerr << "    [" << i << "] baseline:";
            for (int l : k_baseline.clauses[i]) std::cerr << " " << l;
            std::cerr << "\n    [" << i << "] permuted:";
            for (int l : k_permuted.clauses[i]) std::cerr << " " << l;
            std::cerr << "\n";
            diffs_shown++;
        }
    }
    return 1;
}
