// See scc_unsat.h for API contract and design rationale.

#include "scc_unsat.h"

#include <cassert>
#include <vector>

namespace SccUnsat {

namespace {

const int kUnvisited = -1;

struct Frame {
    int v;
    int child_iter;  // index into adj[v] of the next child to visit
};

// Literal encoding (internal to this file).
// Var v in [1, num_vars] with positive polarity ->  2 * (v - 1)
// Var v in [1, num_vars] with negative polarity ->  2 * (v - 1) + 1
// Negation flips the LSB.
inline int lit_to_idx(int lit) {
    const int v = lit > 0 ? lit : -lit;
    return 2 * (v - 1) + (lit < 0 ? 1 : 0);
}

inline int neg_idx(int idx) { return idx ^ 1; }

}  // namespace

std::vector<int> compute_sccs(const std::vector<std::vector<int> >& adj) {
    const int n = static_cast<int>(adj.size());
    std::vector<int> index(n, kUnvisited);
    std::vector<int> lowlink(n, 0);
    std::vector<char> on_stack(n, 0);
    std::vector<int> sccs_stack;
    sccs_stack.reserve(n);
    std::vector<int> scc_id(n, 0);
    std::vector<Frame> call_stack;
    call_stack.reserve(n);

    int disc = 0;
    int scc_count = 0;

    for (int start = 0; start < n; ++start) {
        if (index[start] != kUnvisited) continue;

        // First-visit bookkeeping for `start`.
        index[start] = disc;
        lowlink[start] = disc;
        ++disc;
        sccs_stack.push_back(start);
        on_stack[start] = 1;
        Frame f0;
        f0.v = start;
        f0.child_iter = 0;
        call_stack.push_back(f0);

        while (!call_stack.empty()) {
            Frame& top = call_stack.back();
            const int v = top.v;
            const int n_children = static_cast<int>(adj[v].size());

            if (top.child_iter < n_children) {
                const int w = adj[v][top.child_iter];
                ++top.child_iter;
                if (index[w] == kUnvisited) {
                    // Push w; first-visit bookkeeping inlined.
                    index[w] = disc;
                    lowlink[w] = disc;
                    ++disc;
                    sccs_stack.push_back(w);
                    on_stack[w] = 1;
                    Frame fw;
                    fw.v = w;
                    fw.child_iter = 0;
                    call_stack.push_back(fw);
                    // `top` reference is now dangling — do not touch it.
                } else if (on_stack[w]) {
                    if (index[w] < lowlink[v]) lowlink[v] = index[w];
                }
            } else {
                // Post-visit: v has no more children.
                if (lowlink[v] == index[v]) {
                    // v is the root of its SCC; pop until we see v.
                    while (true) {
                        const int w = sccs_stack.back();
                        sccs_stack.pop_back();
                        on_stack[w] = 0;
                        scc_id[w] = scc_count;
                        if (w == v) break;
                    }
                    ++scc_count;
                }
                const int v_low = lowlink[v];
                call_stack.pop_back();
                if (!call_stack.empty()) {
                    Frame& parent = call_stack.back();
                    if (v_low < lowlink[parent.v]) lowlink[parent.v] = v_low;
                }
            }
        }
    }
    return scc_id;
}

bool is_unsat_2sat(
    unsigned num_vars,
    const std::vector<std::pair<int, int> >& binaries,
    const std::vector<int>& units) {
    if (num_vars == 0) return false;
    const int n_lits = static_cast<int>(2u * num_vars);
    std::vector<std::vector<int> > adj(n_lits);

    for (size_t i = 0; i < binaries.size(); ++i) {
        const int a_lit = binaries[i].first;
        const int b_lit = binaries[i].second;
        assert(a_lit != 0 && b_lit != 0);
        const int a = lit_to_idx(a_lit);
        const int b = lit_to_idx(b_lit);
        // (a v b) <=> (~a -> b) and (~b -> a).
        adj[neg_idx(a)].push_back(b);
        adj[neg_idx(b)].push_back(a);
    }
    for (size_t i = 0; i < units.size(); ++i) {
        const int u_lit = units[i];
        assert(u_lit != 0);
        const int a = lit_to_idx(u_lit);
        // Unit (a) <=> (~a -> a). Forces a in any satisfying assignment.
        adj[neg_idx(a)].push_back(a);
    }

    const std::vector<int> scc = compute_sccs(adj);
    for (unsigned v = 1; v <= num_vars; ++v) {
        const int pos = 2 * static_cast<int>(v - 1);
        const int neg = pos + 1;
        if (scc[pos] == scc[neg]) return true;
    }
    return false;
}

}  // namespace SccUnsat
