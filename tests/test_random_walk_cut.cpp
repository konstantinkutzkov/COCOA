// Balanced separator using random walk augmented Dinic's.
//
// Problem: plain Dinic's always finds trivial low-degree cuts (3 clauses).
// Fix: add virtual edges from random walks to bypass low-degree bottlenecks.
// The minimum cut now must be a more global, structural separator.

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <queue>
#include <string>
#include <random>
#include <set>
#include "../src/dinic.h"
#include "../src/separator.h"

struct AugmentedCutResult {
    std::vector<CutNode> separator;
    int n_var_sep = 0, n_cls_sep = 0;
    int side_a = 0, side_b = 0;
    int flow = 0;
};

AugmentedCutResult find_augmented_balanced_cut(
    const FormulaInfo &info,
    int s_var, int t_var,
    int n_walks = 50,        // number of random walks
    int walk_length = 6,     // steps per walk
    int walk_edge_cap = 1,   // capacity of virtual edges
    unsigned seed = 42)
{
    int n_vars = info.active_vars.size();
    int n_cls = info.active_clause_ids.size();
    int n_nodes = n_vars + n_cls;

    int s_idx = info.var_id_to_idx.at(s_var);
    int t_idx = info.var_id_to_idx.at(t_var);

    auto n_in = [](int i) { return 2 * i; };
    auto n_out = [](int i) { return 2 * i + 1; };

    // Build adjacency list for random walks on the incidence graph
    std::vector<std::vector<int>> adj(n_nodes);
    for (int ci = 0; ci < n_cls; ci++) {
        int c_node = n_vars + ci;
        for (unsigned vid : info.clause_variables[ci]) {
            auto it = info.var_id_to_idx.find(vid);
            if (it == info.var_id_to_idx.end()) continue;
            int v_node = it->second;
            adj[v_node].push_back(c_node);
            adj[c_node].push_back(v_node);
        }
    }

    // Collect random walk endpoints
    std::mt19937 rng(seed);
    std::vector<std::pair<int, int>> virtual_edges;

    for (int w = 0; w < n_walks; w++) {
        // Start from a random variable node
        int start = rng() % n_vars;
        int current = start;

        for (int step = 0; step < walk_length; step++) {
            if (adj[current].empty()) break;
            current = adj[current][rng() % adj[current].size()];
        }

        // Add virtual edge between start and end (if different)
        if (current != start) {
            virtual_edges.push_back({start, current});
        }
    }

    // Build Dinic's graph with original edges + virtual edges
    Dinic dinic(2 * n_nodes);

    // Node capacities: all nodes get cap 1, terminals get INF
    for (int i = 0; i < n_nodes; i++) {
        int cap = (i == s_idx || i == t_idx) ? INF_CAP : 1;
        dinic.add_edge(n_in(i), n_out(i), cap);
    }

    // Original incidence edges (INF cap)
    for (int ci = 0; ci < n_cls; ci++) {
        int c_node = n_vars + ci;
        for (unsigned vid : info.clause_variables[ci]) {
            auto it = info.var_id_to_idx.find(vid);
            if (it == info.var_id_to_idx.end()) continue;
            int v_node = it->second;
            dinic.add_edge(n_out(v_node), n_in(c_node), INF_CAP);
            dinic.add_edge(n_out(c_node), n_in(v_node), INF_CAP);
        }
    }

    // Virtual edges from random walks (low capacity)
    for (const auto &ve : virtual_edges) {
        dinic.add_edge(n_out(ve.first), n_in(ve.second), walk_edge_cap);
        dinic.add_edge(n_out(ve.second), n_in(ve.first), walk_edge_cap);
    }

    int source = n_out(s_idx);
    int sink = n_in(t_idx);

    int flow = dinic.max_flow(source, sink, n_nodes);
    auto reach = dinic.reachable_from(source);

    // Extract separator (only original nodes, not virtual)
    std::vector<CutNode> separator;
    for (int i = 0; i < n_vars; i++) {
        if (reach[n_in(i)] && !reach[n_out(i)])
            separator.push_back(CutNode(CutNode::VAR, info.active_vars[i]));
    }
    for (int j = 0; j < n_cls; j++) {
        int nj = n_vars + j;
        if (reach[n_in(nj)] && !reach[n_out(nj)])
            separator.push_back(CutNode(CutNode::CLAUSE, info.active_clause_ids[j]));
    }

    // Count variable balance
    int source_vars = 0, sink_vars = 0;
    for (int i = 0; i < n_vars; i++) {
        if (reach[n_in(i)] && !reach[n_out(i)]) continue;  // separator
        if (reach[n_out(i)]) source_vars++;
        else sink_vars++;
    }

    AugmentedCutResult result;
    result.separator = separator;
    result.flow = flow;
    result.side_a = std::max(source_vars, sink_vars);
    result.side_b = std::min(source_vars, sink_vars);
    for (const auto &nd : separator) {
        if (nd.kind == CutNode::VAR) result.n_var_sep++;
        else result.n_cls_sep++;
    }

    return result;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <cnf_file> [tries] [n_walks] [walk_len]" << std::endl;
        return 1;
    }

    int tries = argc > 2 ? atoi(argv[2]) : 10;
    int n_walks = argc > 3 ? atoi(argv[3]) : 50;
    int walk_len = argc > 4 ? atoi(argv[4]) : 6;

    std::ifstream fin(argv[1]);
    std::string line;
    int n_vars = 0;
    std::vector<std::vector<int>> clauses;

    while (std::getline(fin, line)) {
        if (line.empty() || line[0] == 'c') continue;
        if (line[0] == 'p') { sscanf(line.c_str(), "p cnf %d", &n_vars); continue; }
        std::istringstream iss(line);
        std::vector<int> clause;
        int lit;
        while (iss >> lit) { if (lit == 0) break; clause.push_back(lit); }
        if (!clause.empty()) clauses.push_back(clause);
    }

    FormulaInfo info;
    for (int v = 1; v <= n_vars; v++)
        info.active_vars.push_back(v);
    for (size_t i = 0; i < clauses.size(); i++) {
        info.active_clause_ids.push_back(i);
        std::vector<unsigned> vars;
        for (int lit : clauses[i]) vars.push_back(abs(lit));
        info.clause_variables.push_back(vars);
    }
    info.buildIndex();

    std::cout << "Variables: " << n_vars << ", Clauses: " << clauses.size()
              << ", Walks: " << n_walks << ", WalkLen: " << walk_len << std::endl;

    AugmentedCutResult overall_best;
    overall_best.side_b = 0;

    for (int i = 0; i < tries; i++) {
        auto terminals = pick_terminals_two_sweep(info, i);
        int s = terminals.first.id;
        int t = terminals.second.id;

        auto result = find_augmented_balanced_cut(info, s, t,
            n_walks, walk_len, 1, i * 137 + 42);

        std::cout << "  Try " << i << ": s=V" << s << " t=V" << t
                  << " flow=" << result.flow
                  << " sep=" << (result.n_var_sep + result.n_cls_sep)
                  << " (" << result.n_var_sep << "V+" << result.n_cls_sep << "C)"
                  << " sides=" << result.side_a << "/" << result.side_b
                  << std::endl;

        if (result.side_b > overall_best.side_b ||
            (result.side_b == overall_best.side_b &&
             (result.n_var_sep + result.n_cls_sep) < (overall_best.n_var_sep + overall_best.n_cls_sep))) {
            overall_best = result;
        }
    }

    std::cout << "\nBest: sep=" << (overall_best.n_var_sep + overall_best.n_cls_sep)
              << " (" << overall_best.n_var_sep << "V+" << overall_best.n_cls_sep << "C)"
              << " sides=" << overall_best.side_a << "/" << overall_best.side_b
              << std::endl;

    return 0;
}
