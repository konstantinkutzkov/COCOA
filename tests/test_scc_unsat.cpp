// Unit tests for src/scc_unsat.{h,cpp}.
//
// Tests the standalone SCC + 2-SAT UNSAT detector in isolation —
// no Solver, no preprocessor, no METIS. If these pass we can trust
// the SCC primitive when wiring it into the solver later.

#include "scc_unsat.h"

#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

static int fails = 0;
static int passes = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (cond) {                                                        \
            ++passes;                                                      \
        } else {                                                           \
            ++fails;                                                       \
            std::cout << "FAIL: " << msg << " at " << __FILE__ << ":"      \
                      << __LINE__ << "\n";                                 \
        }                                                                  \
    } while (0)

static int n_distinct_sccs(const std::vector<int>& scc_id) {
    int max_id = -1;
    for (size_t i = 0; i < scc_id.size(); ++i) {
        if (scc_id[i] > max_id) max_id = scc_id[i];
    }
    return max_id + 1;
}

static bool same_scc(const std::vector<int>& scc_id, int u, int v) {
    return scc_id[u] == scc_id[v];
}

// -- Graph-level SCC tests ---------------------------------------------

static void test_empty_graph() {
    std::vector<std::vector<int> > adj;
    std::vector<int> scc = SccUnsat::compute_sccs(adj);
    CHECK(scc.empty(), "empty graph yields empty SCC vector");
}

static void test_isolated_nodes() {
    std::vector<std::vector<int> > adj(3);
    std::vector<int> scc = SccUnsat::compute_sccs(adj);
    CHECK(scc.size() == 3, "3 nodes");
    CHECK(n_distinct_sccs(scc) == 3, "3 isolated nodes -> 3 SCCs");
}

static void test_single_cycle() {
    //  0 -> 1 -> 2 -> 0
    std::vector<std::vector<int> > adj(3);
    adj[0].push_back(1);
    adj[1].push_back(2);
    adj[2].push_back(0);
    std::vector<int> scc = SccUnsat::compute_sccs(adj);
    CHECK(n_distinct_sccs(scc) == 1, "3-cycle -> 1 SCC");
    CHECK(same_scc(scc, 0, 1) && same_scc(scc, 1, 2),
          "all 3 nodes in same SCC");
}

static void test_chain() {
    //  0 -> 1 -> 2 -> 3   (DAG, each node its own SCC)
    std::vector<std::vector<int> > adj(4);
    adj[0].push_back(1);
    adj[1].push_back(2);
    adj[2].push_back(3);
    std::vector<int> scc = SccUnsat::compute_sccs(adj);
    CHECK(n_distinct_sccs(scc) == 4, "4-chain -> 4 SCCs");
}

static void test_two_cycles_dag_link() {
    // {0,1,2} cycle, {3,4} cycle, 2 -> 3 connector (no back edge)
    std::vector<std::vector<int> > adj(5);
    adj[0].push_back(1);
    adj[1].push_back(2);
    adj[2].push_back(0);
    adj[2].push_back(3);
    adj[3].push_back(4);
    adj[4].push_back(3);
    std::vector<int> scc = SccUnsat::compute_sccs(adj);
    CHECK(n_distinct_sccs(scc) == 2, "two cycles + connector -> 2 SCCs");
    CHECK(same_scc(scc, 0, 1) && same_scc(scc, 1, 2),
          "cycle 1 all in one SCC");
    CHECK(same_scc(scc, 3, 4), "cycle 2 all in one SCC");
    CHECK(!same_scc(scc, 0, 3), "the two cycles are separate SCCs");
}

static void test_self_loop() {
    //  0 -> 0 (self loop), 1 isolated
    std::vector<std::vector<int> > adj(2);
    adj[0].push_back(0);
    std::vector<int> scc = SccUnsat::compute_sccs(adj);
    CHECK(n_distinct_sccs(scc) == 2, "self-loop + isolated -> 2 SCCs");
}

static void test_large_chain_no_stack_overflow() {
    // Long chain would blow a recursive Tarjan's 8 MiB call stack.
    const int N = 200000;
    std::vector<std::vector<int> > adj(N);
    for (int i = 0; i + 1 < N; ++i) adj[i].push_back(i + 1);
    std::vector<int> scc = SccUnsat::compute_sccs(adj);
    CHECK(static_cast<int>(scc.size()) == N, "200k chain returns N entries");
    CHECK(n_distinct_sccs(scc) == N,
          "200k chain -> N SCCs (iterative, no stack overflow)");
}

static void test_large_cycle_no_stack_overflow() {
    const int N = 200000;
    std::vector<std::vector<int> > adj(N);
    for (int i = 0; i < N; ++i) adj[i].push_back((i + 1) % N);
    std::vector<int> scc = SccUnsat::compute_sccs(adj);
    CHECK(n_distinct_sccs(scc) == 1, "200k cycle -> 1 SCC");
}

// -- 2-SAT UNSAT detection ---------------------------------------------

static void test_2sat_empty() {
    bool unsat = SccUnsat::is_unsat_2sat(0, std::vector<std::pair<int, int> >(),
                                         std::vector<int>());
    CHECK(!unsat, "empty formula is vacuously SAT");
    bool unsat2 = SccUnsat::is_unsat_2sat(5, std::vector<std::pair<int, int> >(),
                                          std::vector<int>());
    CHECK(!unsat2, "no clauses, 5 free vars -> SAT");
}

static void test_2sat_sat_unit() {
    // (x1) — satisfiable by x1 = true
    std::vector<int> units;
    units.push_back(1);
    bool unsat = SccUnsat::is_unsat_2sat(
        1, std::vector<std::pair<int, int> >(), units);
    CHECK(!unsat, "(x1) is SAT");
}

static void test_2sat_unsat_unit_unit() {
    // (x1) ^ (~x1) -> UNSAT.
    // Unit (x1) adds edge ~x1 -> x1; unit (~x1) adds edge x1 -> ~x1.
    // Combined: x1 and ~x1 are mutually reachable, same SCC.
    std::vector<int> units;
    units.push_back(1);
    units.push_back(-1);
    bool unsat = SccUnsat::is_unsat_2sat(
        1, std::vector<std::pair<int, int> >(), units);
    CHECK(unsat, "x1 ^ ~x1 detected UNSAT");
}

static void test_2sat_sat_one_binary() {
    // (x1 v x2) -- clearly SAT
    std::vector<std::pair<int, int> > bins;
    bins.push_back(std::make_pair(1, 2));
    bool unsat = SccUnsat::is_unsat_2sat(2, bins, std::vector<int>());
    CHECK(!unsat, "(x1 v x2) is SAT");
}

static void test_2sat_unsat_all_four_clauses() {
    // (x1 v x2) ^ (~x1 v x2) ^ (x1 v ~x2) ^ (~x1 v ~x2)
    // All 4 assignments of {x1, x2} are forbidden by exactly one clause.
    std::vector<std::pair<int, int> > bins;
    bins.push_back(std::make_pair(1, 2));
    bins.push_back(std::make_pair(-1, 2));
    bins.push_back(std::make_pair(1, -2));
    bins.push_back(std::make_pair(-1, -2));
    bool unsat = SccUnsat::is_unsat_2sat(2, bins, std::vector<int>());
    CHECK(unsat, "all four 2-clauses over {x1,x2} detected UNSAT");
}

static void test_2sat_sat_equivalence_chain() {
    // (~x1 v x2), (~x2 v x3), (~x3 v x1)
    // Implications: x1 -> x2 -> x3 -> x1 ; symmetric ~x1 <- ~x2 <- ~x3 <- ~x1
    // {x1,x2,x3} same SCC; {~x1,~x2,~x3} same SCC; the two are separate.
    // Satisfiable (all true or all false).
    std::vector<std::pair<int, int> > bins;
    bins.push_back(std::make_pair(-1, 2));
    bins.push_back(std::make_pair(-2, 3));
    bins.push_back(std::make_pair(-3, 1));
    bool unsat = SccUnsat::is_unsat_2sat(3, bins, std::vector<int>());
    CHECK(!unsat, "equivalence chain x1<->x2<->x3 is SAT");
}

static void test_2sat_sat_chain_plus_unit() {
    // Same equivalence chain plus unit (x1) forces all true. Still SAT.
    std::vector<std::pair<int, int> > bins;
    bins.push_back(std::make_pair(-1, 2));
    bins.push_back(std::make_pair(-2, 3));
    bins.push_back(std::make_pair(-3, 1));
    std::vector<int> units;
    units.push_back(1);
    bool unsat = SccUnsat::is_unsat_2sat(3, bins, units);
    CHECK(!unsat, "equivalence chain + unit(x1) is SAT");
}

static void test_2sat_unsat_implication_cycle() {
    // (~x1 v x2):  x1 -> x2
    // (~x2 v ~x1): x2 -> ~x1   (i.e., x1 and x2 cannot both be true)
    // (x1) [unit]: ~x1 -> x1
    // Combined: x1 -> x2 -> ~x1 -> x1, so x1 and ~x1 mutually reach.
    std::vector<std::pair<int, int> > bins;
    bins.push_back(std::make_pair(-1, 2));
    bins.push_back(std::make_pair(-2, -1));
    std::vector<int> units;
    units.push_back(1);
    bool unsat = SccUnsat::is_unsat_2sat(2, bins, units);
    CHECK(unsat, "x1 -> x2 -> ~x1 plus unit(x1) detected UNSAT");
}

static void test_2sat_unsat_pure_binary_cycle() {
    // 4 vars, build an SCC cycle of mixed polarities forcing
    // x1 -> ~x1 -> x1 via pure binary clauses (no units).
    //
    //   (~x1 v x2):  x1 -> x2     (-1, 2)
    //   (~x2 v ~x1): x2 -> ~x1    (-2, -1)
    //   (x1 v x3):   ~x1 -> x3    (1, 3)
    //   (~x3 v x1):  ~x3 -> ~x1? No: ~x3 -> ~x1 from (~x3 v x1)? Let's redo.
    //
    // Goal: ~x1 reaches x1. Use ~x1 -> x3 -> x1.
    //   (x1 v x3) gives ~x1 -> x3 and ~x3 -> x1.  Has ~x1 -> x3. Good.
    //   (~x3 v x1) gives x3 -> x1 and ~x1 -> ~x3. Has x3 -> x1. Good.
    //
    // So clauses: (~x1 v x2), (~x2 v ~x1), (x1 v x3), (~x3 v x1).
    // x1 -> x2 -> ~x1 (path A); ~x1 -> x3 -> x1 (path B).
    // x1 and ~x1 are in the same SCC. UNSAT.
    std::vector<std::pair<int, int> > bins;
    bins.push_back(std::make_pair(-1, 2));
    bins.push_back(std::make_pair(-2, -1));
    bins.push_back(std::make_pair(1, 3));
    bins.push_back(std::make_pair(-3, 1));
    bool unsat = SccUnsat::is_unsat_2sat(3, bins, std::vector<int>());
    CHECK(unsat, "pure-binary 2-SAT UNSAT via SCC closure");
}

static void test_2sat_long_implication_chain_sat() {
    // x1 -> x2 -> x3 -> ... -> xN  (SAT, all-true assignment)
    const int N = 50;
    std::vector<std::pair<int, int> > bins;
    for (int i = 1; i < N; ++i) {
        bins.push_back(std::make_pair(-i, i + 1));
    }
    bool unsat = SccUnsat::is_unsat_2sat(N, bins, std::vector<int>());
    CHECK(!unsat, "long implication chain (no contradiction) is SAT");
}

int main() {
    test_empty_graph();
    test_isolated_nodes();
    test_single_cycle();
    test_chain();
    test_two_cycles_dag_link();
    test_self_loop();
    test_large_chain_no_stack_overflow();
    test_large_cycle_no_stack_overflow();

    test_2sat_empty();
    test_2sat_sat_unit();
    test_2sat_unsat_unit_unit();
    test_2sat_sat_one_binary();
    test_2sat_unsat_all_four_clauses();
    test_2sat_sat_equivalence_chain();
    test_2sat_sat_chain_plus_unit();
    test_2sat_unsat_implication_cycle();
    test_2sat_unsat_pure_binary_cycle();
    test_2sat_long_implication_chain_sat();

    std::cout << passes << " passed, " << fails << " failed\n";
    if (fails > 0) {
        std::cout << "FAIL\n";
        return EXIT_FAILURE;
    }
    std::cout << "PASS\n";
    return EXIT_SUCCESS;
}
