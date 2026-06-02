// Test: canonical_key invariance under free-var count differences
// arising mid-search.
//
// Constructs two states from the SAME CNF that have identical
// in-scope clauses (same long-clause content, same active lits in
// those clauses) but differ ONLY in the number of free variables
// (active vars not appearing in any in-scope clause). A correct
// canonical_key must produce the same hash for both states.
//
// Usage: test_canonical_key_free_var_midsearch <cnf>
//
// The CNF should have:
//   - Some clauses (C1, C2, ...) that stay in scope after we force
//     specific lits.
//   - Some vars (say V_extra) that are declared but not in any of
//     those in-scope clauses.
//
// We construct:
//   - State A: force lits to satisfy specific clauses, leaving
//     V_extra ACTIVE (free).
//   - State B: force the same lits PLUS one more to assign V_extra
//     (reducing free var count by 1).
//
// Compare the canonical_keys.

#include "solver.h"
#include <iostream>

int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr << "usage: " << argv[0]
                  << " <cnf> <state_A_lits> <state_B_lits>\n"
                  << "  state_A_lits, state_B_lits: comma-separated DIMACS lits to force\n";
        return 2;
    }
    const std::string path = argv[1];

    auto parse_lits = [](const std::string &s) {
        std::vector<int> out;
        std::string cur;
        for (char c : s) {
            if (c == ',') {
                if (!cur.empty()) { out.push_back(std::stoi(cur)); cur.clear(); }
            } else cur += c;
        }
        if (!cur.empty()) out.push_back(std::stoi(cur));
        return out;
    };

    auto lits_A = parse_lits(argv[2]);
    auto lits_B = parse_lits(argv[3]);

    Solver hA;
    hA.config().quiet = true;
    hA._prepareForKeyComputation(path);  // WITH preprocessing
    for (int l : lits_A) hA._forceLitForTest(l);
    CanonicalKey kA = hA._computeRootCanonicalKey();

    Solver hB;
    hB.config().quiet = true;
    hB._prepareForKeyComputation(path);  // WITH preprocessing
    for (int l : lits_B) hB._forceLitForTest(l);
    CanonicalKey kB = hB._computeRootCanonicalKey();

    std::cout << "state A (lits=" << argv[2] << "): hash=" << kA.hash
              << " hash_hi=" << kA.hash_hi
              << " nvars=" << kA.num_vars
              << " n_in_clauses=" << kA.n_in_clauses
              << " nclauses=" << kA.num_clauses << "\n";
    std::cout << "state B (lits=" << argv[3] << "): hash=" << kB.hash
              << " hash_hi=" << kB.hash_hi
              << " nvars=" << kB.num_vars
              << " n_in_clauses=" << kB.n_in_clauses
              << " nclauses=" << kB.num_clauses << "\n";

    bool same_hash = (kA.hash == kB.hash && kA.hash_hi == kB.hash_hi);
    if (same_hash) std::cout << "PASS: canonical_key invariant\n";
    else            std::cout << "FAIL: canonical_key DIFFERS\n";
    return same_hash ? 0 : 1;
}
