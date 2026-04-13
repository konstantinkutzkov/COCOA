// Weighted bipartite separator: clause nodes have lower capacity
// than variable nodes, reflecting the lower cost of clause branching.
// Balance measured by variable count only.
//
// Uses iterative piercing (flowcutter-style) with:
// - Variable nodes: capacity 1
// - Clause nodes: capacity alpha (< 1, e.g. 0.3)
// - Balance = min(vars_on_side_A, vars_on_side_B)
// - Score = vars_in_separator + alpha * clauses_in_separator

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <random>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include "../src/dinic.h"
#include "../src/separator.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <cnf_file> [tries] [alpha]" << std::endl;
        return 1;
    }

    int tries = argc > 2 ? atoi(argv[2]) : 10;
    // alpha: relative cost of clause branching vs variable branching
    // alpha=1.0 means equal cost, alpha=0.0 means clauses are free
    float alpha = argc > 3 ? atof(argv[3]) : 0.3;

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

    int n_cls = clauses.size();
    int n_nodes = n_vars + n_cls;

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

    // Adjacency for random walks
    std::vector<std::vector<int>> adj(n_nodes);
    for (int ci = 0; ci < n_cls; ci++) {
        int c_node = n_vars + ci;
        for (unsigned vid : info.clause_variables[ci]) {
            auto it = info.var_id_to_idx.find(vid);
            if (it == info.var_id_to_idx.end()) continue;
            adj[it->second].push_back(c_node);
            adj[c_node].push_back(it->second);
        }
    }

    auto n_in = [](int i) { return 2 * i; };
    auto n_out = [](int i) { return 2 * i + 1; };

    // Variable capacity is fixed. Clause capacity depends on clause length:
    // alpha(clause) = 3.0 / clause_length
    // So length-3 clause has same cost as a variable (cap=var_cap),
    // longer clauses are cheaper.
    // We use per-clause capacities, so var_cap is the reference.
    int var_cap = 100;  // higher precision for varying clause caps

    std::cout << "Variables: " << n_vars << ", Clauses: " << n_cls
              << ", alpha=" << alpha
              << " (var_cap=" << var_cap << " cls_cap=3/len)"
              << std::endl;

    // Clause length stats
    int max_cl_len = 0;
    for (const auto &cl : clauses)
        if ((int)cl.size() > max_cl_len) max_cl_len = cl.size();
    std::cout << "Max clause length: " << max_cl_len << std::endl;

    struct BestResult {
        float score = 1e9;
        int nv = 0, nc = 0;
        int side_a = 0, side_b = 0;
    } overall_best;

    for (int tri = 0; tri < tries; tri++) {
        auto terminals = pick_terminals_two_sweep(info, tri);
        int s_idx = info.var_id_to_idx.at(terminals.first.id);
        int t_idx = info.var_id_to_idx.at(terminals.second.id);

        std::vector<bool> source_set(n_nodes, false);
        std::vector<bool> sink_set(n_nodes, false);
        source_set[s_idx] = true;
        sink_set[t_idx] = true;

        std::mt19937 rng(tri * 1000 + 42);

        BestResult try_best;
        int max_iters = 30;

        for (int iter = 0; iter < max_iters; iter++) {
            Dinic dinic(2 * n_nodes);

            // Node capacities:
            // Variables: cap = var_cap (cost 1.0)
            // Clauses of length k: cap = var_cap * log(3)/log(k) * (1 - k/n_vars)
            //   - log(3)/log(k): branching depth reduction
            //   - (1 - k/n_vars): discount for high variable coverage
            //   - Binary clauses (k=2): cap > var_cap (worse than variable branching)
            for (int i = 0; i < n_vars; i++) {
                int cap = (source_set[i] || sink_set[i]) ? INF_CAP : var_cap;
                dinic.add_edge(n_in(i), n_out(i), cap);
            }
            for (int j = 0; j < n_cls; j++) {
                int nj = n_vars + j;
                int cap;
                if (source_set[nj] || sink_set[nj]) {
                    cap = INF_CAP;
                } else {
                    int cl_len = clauses[j].size();
                    if (cl_len <= 2) {
                        // Binary clauses: more expensive than variables
                        cap = var_cap * 3 / 2;  // cost 1.5
                    } else {
                        double cost = (log(3.0) / log((double)cl_len))
                                    * (1.0 - (double)cl_len / n_vars);
                        cap = std::max(1, (int)(var_cap * cost));
                    }
                }
                dinic.add_edge(n_in(nj), n_out(nj), cap);
            }

            // Incidence edges
            for (int ci = 0; ci < n_cls; ci++) {
                int c_node = n_vars + ci;
                for (unsigned vid : info.clause_variables[ci]) {
                    auto it = info.var_id_to_idx.find(vid);
                    if (it == info.var_id_to_idx.end()) continue;
                    dinic.add_edge(n_out(it->second), n_in(c_node), INF_CAP);
                    dinic.add_edge(n_out(c_node), n_in(it->second), INF_CAP);
                }
            }

            // Random walk edges
            int walk_len = 4 + (iter % 4) * 2;
            for (int w = 0; w < 30; w++) {
                int start = rng() % n_nodes;
                int current = start;
                for (int step = 0; step < walk_len; step++) {
                    if (adj[current].empty()) break;
                    current = adj[current][rng() % adj[current].size()];
                }
                if (current != start) {
                    dinic.add_edge(n_out(start), n_in(current), 1);
                    dinic.add_edge(n_out(current), n_in(start), 1);
                }
            }

            int flow = dinic.max_flow(n_out(s_idx), n_in(t_idx), n_nodes * var_cap);
            auto reach = dinic.reachable_from(n_out(s_idx));

            int source_vars = 0, sink_vars = 0, sep_v = 0, sep_c = 0;
            float sep_clause_cost = 0;
            for (int i = 0; i < n_vars; i++) {
                if (reach[n_in(i)] && !reach[n_out(i)]) sep_v++;
                else if (reach[n_out(i)]) source_vars++;
                else sink_vars++;
            }
            for (int j = 0; j < n_cls; j++) {
                int nj = n_vars + j;
                if (reach[n_in(nj)] && !reach[n_out(nj)]) {
                    sep_c++;
                    int cl_len = clauses[j].size();
                    if (cl_len <= 2) {
                        sep_clause_cost += 1.5f;
                    } else {
                        sep_clause_cost += (float)(log(3.0) / log((double)cl_len)
                                          * (1.0 - (double)cl_len / n_vars));
                    }
                }
            }

            int balance = std::min(source_vars, sink_vars);
            float score = sep_v + sep_clause_cost;

            if (balance >= n_vars / 5 && score < try_best.score) {
                try_best.score = score;
                try_best.nv = sep_v;
                try_best.nc = sep_c;
                try_best.side_a = std::max(source_vars, sink_vars);
                try_best.side_b = balance;
            }

            if (balance >= n_vars / 4) break;

            // Absorb small side + separator into smaller terminal set
            bool source_is_small = (source_vars <= sink_vars);
            for (int i = 0; i < n_vars; i++) {
                bool in_sep = reach[n_in(i)] && !reach[n_out(i)];
                bool in_source = reach[n_out(i)];
                if (source_is_small) {
                    if (in_source || in_sep) source_set[i] = true;
                } else {
                    if (!in_source && !in_sep) sink_set[i] = true;
                    if (in_sep) sink_set[i] = true;
                }
            }
            for (int j = 0; j < n_cls; j++) {
                int nj = n_vars + j;
                if (reach[n_in(nj)] && !reach[n_out(nj)]) {
                    if (source_is_small) source_set[nj] = true;
                    else sink_set[nj] = true;
                }
            }
        }

        if (try_best.score < 1e8) {
            std::cout << "  Try " << tri
                      << ": sep=" << (try_best.nv + try_best.nc)
                      << " (" << try_best.nv << "V+" << try_best.nc << "C)"
                      << " score=" << std::fixed << std::setprecision(1) << try_best.score
                      << " sides=" << try_best.side_a << "/" << try_best.side_b
                      << std::endl;

            if (try_best.score < overall_best.score)
                overall_best = try_best;
        }
    }

    std::cout << "\nBest: sep=" << (overall_best.nv + overall_best.nc)
              << " (" << overall_best.nv << "V+" << overall_best.nc << "C)"
              << " score=" << overall_best.score
              << " sides=" << overall_best.side_a << "/" << overall_best.side_b
              << std::endl;

    std::cout << "\nReference (flowcutter primal): sep=19 (19V+0C) score=19.0 sides=130/73" << std::endl;
    std::cout << "Reference (flowcutter incidence): sep=18 (14V+4C) score=" << (14 + alpha*4) << " sides=25/24" << std::endl;

    return 0;
}
