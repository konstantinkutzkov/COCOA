// Combined approach: iterative piercing + random walk augmentation
//
// Each iteration:
// 1. Build Dinic's graph with current source/sink sets (INF cap)
// 2. Add random walk virtual edges (cap 1) to prevent trivial cuts
// 3. Run Dinic's, record cut
// 4. Absorb small side + separator variables into smaller terminal set
// 5. Repeat with fresh random walks

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <random>
#include <algorithm>
#include "../src/dinic.h"
#include "../src/separator.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <cnf_file> [tries] [max_iters] [walks_per_iter]" << std::endl;
        return 1;
    }

    int tries = argc > 2 ? atoi(argv[2]) : 10;
    int max_iters = argc > 3 ? atoi(argv[3]) : 30;
    int walks_per_iter = argc > 4 ? atoi(argv[4]) : 30;

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

    int n_cls = info.active_clause_ids.size();
    int n_nodes = n_vars + n_cls;

    // Build adjacency for random walks
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

    std::cout << "Variables: " << n_vars << ", Clauses: " << n_cls << std::endl;

    // Best result across all tries
    int overall_best_balance = 0;
    int overall_best_sep = 0;
    int overall_best_nv = 0, overall_best_nc = 0;
    int overall_best_a = 0, overall_best_b = 0;

    for (int tri = 0; tri < tries; tri++) {
        auto terminals = pick_terminals_two_sweep(info, tri);
        int s_idx = info.var_id_to_idx.at(terminals.first.id);
        int t_idx = info.var_id_to_idx.at(terminals.second.id);

        // Terminal sets (nodes with INF capacity)
        std::vector<bool> source_set(n_nodes, false);
        std::vector<bool> sink_set(n_nodes, false);
        source_set[s_idx] = true;
        sink_set[t_idx] = true;

        std::mt19937 rng(tri * 1000 + 42);

        int best_balance = 0;
        int best_sep_size = n_nodes;
        int best_nv = 0, best_nc = 0;
        int best_a = 0, best_b = 0;
        int best_iter = -1;

        for (int iter = 0; iter < max_iters; iter++) {
            // Build Dinic's graph
            Dinic dinic(2 * n_nodes);

            for (int i = 0; i < n_nodes; i++) {
                int cap = (source_set[i] || sink_set[i]) ? INF_CAP : 1;
                dinic.add_edge(n_in(i), n_out(i), cap);
            }

            // Original incidence edges
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

            // Random walk virtual edges for this iteration
            int walk_len = 4 + (iter % 4) * 2;  // vary walk length
            for (int w = 0; w < walks_per_iter; w++) {
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

            int source = n_out(s_idx);
            int sink = n_in(t_idx);
            int flow = dinic.max_flow(source, sink, n_nodes);
            auto reach = dinic.reachable_from(source);

            // Extract separator and count variable balance
            int source_vars = 0, sink_vars = 0;
            int sep_v = 0, sep_c = 0;
            for (int i = 0; i < n_vars; i++) {
                if (reach[n_in(i)] && !reach[n_out(i)]) sep_v++;
                else if (reach[n_out(i)]) source_vars++;
                else sink_vars++;
            }
            for (int j = 0; j < n_cls; j++) {
                int nj = n_vars + j;
                if (reach[n_in(nj)] && !reach[n_out(nj)]) sep_c++;
            }

            int balance = std::min(source_vars, sink_vars);

            if (balance > best_balance ||
                (balance == best_balance && (sep_v + sep_c) < best_sep_size)) {
                best_balance = balance;
                best_sep_size = sep_v + sep_c;
                best_nv = sep_v;
                best_nc = sep_c;
                best_a = std::max(source_vars, sink_vars);
                best_b = balance;
                best_iter = iter;
            }

            // Stop if balanced
            if (balance >= n_vars / 4) break;

            // Grow the SMALLER side by absorbing the small component
            // + separator into it, then piercing into the big side.
            //
            // small side = min(source_vars, sink_vars)
            // If source_vars < sink_vars: grow source
            // If sink_vars < source_vars: grow sink
            //
            // "Grow sink" means: absorb sink-side nodes + separator
            // into sink_set, then take some source-side nodes and
            // add them to sink_set (piercing).

            bool source_is_small = (source_vars <= sink_vars);

            // Absorb the small side + separator into the small terminal set
            for (int i = 0; i < n_vars; i++) {
                bool in_sep = reach[n_in(i)] && !reach[n_out(i)];
                bool in_source = reach[n_out(i)];
                // bool in_sink = !reach[n_in(i)];

                if (source_is_small) {
                    // Absorb source-side vars + separator vars into source_set
                    if (in_source || in_sep)
                        source_set[i] = true;
                } else {
                    // Absorb sink-side vars + separator vars into sink_set
                    if (!in_source && !in_sep)  // sink side
                        sink_set[i] = true;
                    if (in_sep)
                        sink_set[i] = true;
                }
            }
            // Also absorb clause nodes in separator
            for (int j = 0; j < n_cls; j++) {
                int nj = n_vars + j;
                if (reach[n_in(nj)] && !reach[n_out(nj)]) {
                    if (source_is_small)
                        source_set[nj] = true;
                    else
                        sink_set[nj] = true;
                }
            }
        }

        std::cout << "  Try " << tri
                  << ": s=V" << terminals.first.id
                  << " t=V" << terminals.second.id
                  << " best_iter=" << best_iter
                  << " sep=" << best_sep_size
                  << " (" << best_nv << "V+" << best_nc << "C)"
                  << " sides=" << best_a << "/" << best_b
                  << std::endl;

        if (best_balance > overall_best_balance ||
            (best_balance == overall_best_balance &&
             best_sep_size < overall_best_sep)) {
            overall_best_balance = best_balance;
            overall_best_sep = best_sep_size;
            overall_best_nv = best_nv;
            overall_best_nc = best_nc;
            overall_best_a = best_a;
            overall_best_b = best_b;
        }
    }

    std::cout << "\nBest: sep=" << overall_best_sep
              << " (" << overall_best_nv << "V+" << overall_best_nc << "C)"
              << " sides=" << overall_best_a << "/" << overall_best_b
              << std::endl;

    // Compare with flowcutter reference
    std::cout << "\nFlowcutter reference (from TD):" << std::endl;
    std::cout << "  Instance 025: sep=20 sides=21/22 (vars only)" << std::endl;
    std::cout << "  Instance 027: sep=18 sides=23/25 (vars only)" << std::endl;

    return 0;
}
