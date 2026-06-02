// Regression test for the soundness fix to Solver::sccCheckComponentUnsat
// (see src/scc_unsat_solver.cpp).
//
// Bug found 2026-05-27 on t1_105 + triple_no_lockstep + -checkUnsat:
// the SCC bridge walked clauses in `comp.clsBegin()` without checking
// `removed_clauses_`, so consumed clauses (dropped via branchOnClause's
// consume arm) were still contributing edges to the binary-implication
// graph. This let SCC fabricate UNSAT certificates that don't apply to
// the residual formula, returning true on branches that are actually
// SAT. The wrong "branch counted as 0" then propagated through the
// component cache, eventually triggering CACHE_STORE_COLLISION when the
// same canonical_key reappeared via a different search path with the
// correct count.
//
// The fix (scc_unsat_solver.cpp): skip clauses where
// `isClauseRemoved(ofs)` returns true, matching canonical_key.cpp:145
// and brute-force (solver.cpp:1441).
//
// This test constructs the minimal CNF + state that triggers the bug:
// two binaries that on their own are satisfiable, plus a long clause
// that shortens to a forced unit if walked. With the long clause
// marked as removed, the SCC bridge must NOT walk it; if the filter is
// missing, it would fabricate UNSAT.
//
// Layout (4 vars, 3 clauses):
//   (~x1 v x2)        original binary
//   (~x2 v ~x1)       original binary
//   (x1 v x3 v x4)    original long  <-- marked as removed
//
// With x3=F and x4=F forced into the trail (no BCP run), the long
// clause walks (if not filtered) as a single active literal x1 = a
// fabricated unit. Combined with the two binaries, the implication
// graph has x1 -> x2 -> ~x1 -> x1, putting x1 and ~x1 in the same SCC
// -> spurious UNSAT.
//
// With the filter, the long clause is skipped, the SCC graph holds
// only the two binaries, x1 and ~x1 are in different SCCs, and the
// verdict is correctly SAT (false).

#include "solver.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

int main() {
    const std::string path = "/tmp/test_scc_removed_clauses.cnf";
    {
        std::ofstream f(path.c_str());
        f << "p cnf 4 3\n"
             "-1 2 0\n"      // (~x1 v x2)
             "-2 -1 0\n"     // (~x2 v ~x1)
             "1 3 4 0\n";    // (x1 v x3 v x4)  -- to be removed
    }

    Solver s;
    s.config().quiet = true;
    s._prepareForKeyComputationNoPP(path);

    // Force x3=F and x4=F so the long clause, IF walked, shortens to
    // (x1) -- a fabricated unit that would close x1 <-> ~x1 in the SCC.
    if (!s._forceLitForTest(-3) || !s._forceLitForTest(-4)) {
        std::cout << "FAIL: could not force x3=F / x4=F\n";
        return EXIT_FAILURE;
    }

    // Mark the long clause as removed. The residual is now just the
    // two binaries -- satisfiable by x1=F (gives x2 free, both clauses
    // hold).
    s._markAllLongClausesRemovedForTest();

    const bool got_unsat = s._sccCheckRootForTest();

    if (got_unsat) {
        std::cout << "FAIL: SCC returned UNSAT on a residual that is SAT.\n"
                     "  This means the bridge walked the removed clause and\n"
                     "  fabricated a 2-CNF UNSAT certificate. The fix in\n"
                     "  src/scc_unsat_solver.cpp must add isClauseRemoved(ofs)\n"
                     "  before walking the clause body.\n";
        return EXIT_FAILURE;
    }

    std::cout << "PASS: SCC correctly returned SAT (false) when the\n"
                 "      long clause is removed.\n";
    return EXIT_SUCCESS;
}
