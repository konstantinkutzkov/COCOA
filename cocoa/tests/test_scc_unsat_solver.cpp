// Integration test for the solver-side SCC UNSAT wrapper
// (Solver::sccCheckComponentUnsat, defined in src/scc_unsat_solver.cpp).
//
// The standalone SCC algorithm is verified by test_scc_unsat. This test
// covers the bridge layer: writing a small CNF to disk, loading it via
// _prepareForKeyComputationNoPP (no preprocessing, free vars survive),
// and confirming the wrapper extracts the right edges from
// binary_links_ + literal_pool_ to reach the correct UNSAT/SAT verdict.

#include "solver.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

static int fails = 0;
static int passes = 0;

static void write_cnf(const std::string &path, const std::string &body) {
    std::ofstream f(path.c_str());
    f << body;
}

static void run_one(const std::string &label,
                    const std::string &cnf_body,
                    bool expect_unsat) {
    const std::string path = std::string("/tmp/test_scc_unsat_solver_") +
                              label + ".cnf";
    write_cnf(path, cnf_body);

    Solver s;
    s.config().quiet = true;
    s._prepareForKeyComputationNoPP(path);
    const bool got = s._sccCheckRootForTest();

    if (got == expect_unsat) {
        ++passes;
        std::cout << "PASS [" << label << "] expected="
                  << (expect_unsat ? "UNSAT" : "SAT")
                  << " got=" << (got ? "UNSAT" : "SAT") << "\n";
    } else {
        ++fails;
        std::cout << "FAIL [" << label << "] expected="
                  << (expect_unsat ? "UNSAT" : "SAT")
                  << " got=" << (got ? "UNSAT" : "SAT") << "\n";
    }
}

int main() {
    // (x1 v x2) -- SAT
    run_one("sat_one_binary",
            "p cnf 2 1\n"
            "1 2 0\n",
            /*expect_unsat=*/false);

    // (x1 v x2) ^ (~x1 v x2) ^ (x1 v ~x2) ^ (~x1 v ~x2) -- UNSAT
    run_one("unsat_four_binaries",
            "p cnf 2 4\n"
            "1 2 0\n"
            "-1 2 0\n"
            "1 -2 0\n"
            "-1 -2 0\n",
            /*expect_unsat=*/true);

    // Equivalence chain: (~x1 v x2), (~x2 v x3), (~x3 v x1) -- SAT
    // (all-true and all-false both satisfy)
    run_one("sat_equiv_chain",
            "p cnf 3 3\n"
            "-1 2 0\n"
            "-2 3 0\n"
            "-3 1 0\n",
            /*expect_unsat=*/false);

    // Pure-binary UNSAT cycle:
    //   (~x1 v x2): x1 -> x2
    //   (~x2 v ~x1): x2 -> ~x1
    //   (x1 v x3): ~x1 -> x3
    //   (~x3 v x1): x3 -> x1
    // x1 reaches ~x1 (via x2); ~x1 reaches x1 (via x3). UNSAT.
    run_one("unsat_pure_binary_cycle",
            "p cnf 3 4\n"
            "-1 2 0\n"
            "-2 -1 0\n"
            "1 3 0\n"
            "-3 1 0\n",
            /*expect_unsat=*/true);

    // Long clause shortened-to-binary path. (x1 v x2 v x3) on its own
    // is SAT. But the wrapper should not falsely report UNSAT just
    // because it sees a long clause it can't reduce.
    run_one("sat_long_clause_alone",
            "p cnf 3 1\n"
            "1 2 3 0\n",
            /*expect_unsat=*/false);

    // Even with no preprocessing, a CNF that is 2-SAT-UNSAT only via
    // long-clause reasoning should NOT be flagged here (incomplete
    // for long clauses): (x1 v x2 v x3) ^ ... -- skipping since we
    // can only test the SOUND direction here.

    std::cout << passes << " passed, " << fails << " failed\n";
    if (fails > 0) {
        std::cout << "FAIL\n";
        return EXIT_FAILURE;
    }
    std::cout << "PASS\n";
    return EXIT_SUCCESS;
}
