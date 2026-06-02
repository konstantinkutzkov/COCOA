// SCC-based UNSAT detector for the binary-implication graph of a CNF.
//
// This is the Aspvall-Plass-Tarjan (1979) 2-SAT satisfiability test.
// On the *binary subset* of the residual formula it is complete
// (returns SAT iff the 2-CNF is satisfiable). On the *full* formula
// it is sound for UNSAT only — long clauses can only strengthen
// unsatisfiability, so a 2-CNF UNSAT verdict carries over. Conversely,
// a 2-CNF SAT verdict says nothing about the full formula.
//
// The module is deliberately decoupled from the solver: pure integer
// inputs, no Solver/Instance/CanonicalKey dependencies. Drive it from
// any caller via DIMACS-style signed literals. The solver-side wrapper
// that pulls active binary clauses out of solver state lives elsewhere
// (planned: instance.h helper, see docs/scc_unsat_prune_plan.md step 2).
//
// See docs/scc_unsat_prune_plan.md for the full design and integration
// roadmap.

#ifndef SCC_UNSAT_H_
#define SCC_UNSAT_H_

#include <utility>
#include <vector>

namespace SccUnsat {

// Iterative Tarjan SCC on an int-indexed adjacency list.
// adj[u] = vector of nodes reachable from u via one directed edge.
// Returns scc_id[v] in [0, num_sccs); v and w are strongly connected
// iff scc_id[v] == scc_id[w]. SCC IDs are assigned in reverse
// topological order of the condensation DAG (Tarjan's natural order),
// but callers should not rely on the specific numbering.
//
// Iterative — safe on graphs with millions of vertices; recursive
// Tarjan blows the stack at ~50k on macOS default 8 MiB.
std::vector<int> compute_sccs(const std::vector<std::vector<int> >& adj);

// 2-SAT UNSAT check via SCC on the binary-implication graph.
//
//   num_vars: number of variables. Variables are 1-indexed in `binaries`
//             and `units`; var 0 is not used.
//   binaries: list of (a, b) pairs encoding clause (a v b) where a, b
//             are signed nonzero DIMACS literals in
//             {-num_vars..-1, 1..num_vars}.
//   units:    list of signed nonzero literals encoding unit clauses (a).
//
// Returns true iff the 2-CNF is provably UNSAT (some variable v has
// both polarities in the same SCC of the implication graph). Returns
// false otherwise — including for satisfiable formulas AND for cases
// the 2-SAT test cannot rule out.
//
// Cost: O(num_vars + |binaries| + |units|). Pure integer scratch
// vectors are allocated and freed inside this call; the caller is
// expected to amortize calls by re-using the function rather than
// rebuilding the graph manually.
bool is_unsat_2sat(
    unsigned num_vars,
    const std::vector<std::pair<int, int> >& binaries,
    const std::vector<int>& units);

}  // namespace SccUnsat

#endif  // SCC_UNSAT_H_
