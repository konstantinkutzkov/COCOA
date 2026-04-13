// Adaptive balanced separator: incrementally increase distance threshold
// until a balanced cut is found.
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <queue>
#include <string>
#include "../src/dinic.h"
#include "../src/separator.h"

struct BalancedCutResult {
    int flow;
    int sep_size;
    int n_var, n_cls;
    int side_a, side_b;
    int threshold;
    std::vector<CutNode> separator;
};

BalancedCutResult find_balanced_cut(
    const FormulaInfo &info,
    int s_var, int t_var,
    int n_vars, int n_cls,
    int max_threshold,
    int target_balance_ratio = 4)  // stop when small side >= n_vars / ratio
{
    int s_idx = info.var_id_to_idx.at(s_var);
    int t_idx = info.var_id_to_idx.at(t_var);
    int n_nodes = n_vars + n_cls;

    // BFS distances from s and t
    auto bfs_dist = [&](int start_idx) -> std::vector<int> {
        std::vector<int> dist_var(n_vars, -1);
        std::vector<int> dist_cls(n_cls, -1);
        std::queue<std::pair<bool, int>> q;
        dist_var[start_idx] = 0;
        q.push({false, start_idx});
        while (!q.empty()) {
            bool is_cls = q.front().first;
            int idx = q.front().second;
            q.pop();
            if (is_cls) {
                int d = dist_cls[idx];
                for (unsigned vid : info.clause_variables[idx]) {
                    auto it = info.var_id_to_idx.find(vid);
                    if (it == info.var_id_to_idx.end()) continue;
                    int vi = it->second;
                    if (dist_var[vi] < 0) { dist_var[vi] = d + 1; q.push({false, vi}); }
                }
            } else {
                int d = dist_var[idx];
                for (int ci : info.var_to_clause_indices[idx]) {
                    if (dist_cls[ci] < 0) { dist_cls[ci] = d + 1; q.push({true, ci}); }
                }
            }
        }
        std::vector<int> dist(n_nodes);
        for (int i = 0; i < n_vars; i++) dist[i] = dist_var[i] >= 0 ? dist_var[i] : 999;
        for (int i = 0; i < n_cls; i++) dist[n_vars + i] = dist_cls[i] >= 0 ? dist_cls[i] : 999;
        return dist;
    };

    auto dist_s = bfs_dist(s_idx);
    auto dist_t = bfs_dist(t_idx);

    // Compute diameter (for reference)
    int diameter = 0;
    for (int i = 0; i < n_nodes; i++) {
        int d = std::min(dist_s[i], dist_t[i]);
        if (d < 999 && d > diameter) diameter = d;
    }

    BalancedCutResult best;
    best.flow = 0;
    best.sep_size = 0;
    best.side_a = 0;
    best.side_b = 0;
    best.threshold = -1;

    int min_balance = std::max(2, n_vars / target_balance_ratio);

    auto n_in = [](int i) { return 2 * i; };
    auto n_out = [](int i) { return 2 * i + 1; };

    for (int thresh = 1; thresh <= max_threshold; thresh++) {
        Dinic dinic(2 * n_nodes);

        // Set node capacities
        for (int i = 0; i < n_nodes; i++) {
            int cap;
            if (i == s_idx || i == t_idx) {
                cap = INF_CAP;
            } else {
                int min_dist = std::min(dist_s[i], dist_t[i]);
                cap = (min_dist <= thresh) ? INF_CAP : 1;
            }
            dinic.add_edge(n_in(i), n_out(i), cap);
        }

        // Incidence edges
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

        int source = n_out(s_idx);
        int sink = n_in(t_idx);
        int flow = dinic.max_flow(source, sink, n_vars);  // generous flow cap
        auto reach = dinic.reachable_from(source);

        // Extract separator
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

        // Verify
        auto sizes = verify_separator(info, separator, 1);
        int balance = (sizes.size() >= 2) ? sizes[1] : 0;

        int nv = 0, nc = 0;
        for (const auto &nd : separator)
            if (nd.kind == CutNode::VAR) nv++; else nc++;

        if (balance > best.side_b ||
            (balance == best.side_b && (int)separator.size() < best.sep_size)) {
            best.flow = flow;
            best.sep_size = separator.size();
            best.n_var = nv;
            best.n_cls = nc;
            best.side_a = sizes.empty() ? 0 : sizes[0];
            best.side_b = balance;
            best.threshold = thresh;
            best.separator = separator;
        }

        if (balance >= min_balance)
            break;  // balanced enough, stop
    }

    return best;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <cnf_file> [tries]" << std::endl;
        return 1;
    }

    int tries = argc > 2 ? atoi(argv[2]) : 20;

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
    int n_cls = clauses.size();

    std::cout << "Variables: " << n_vars << ", Clauses: " << n_cls << std::endl;

    // Try multiple terminal pairs
    BalancedCutResult overall_best;
    overall_best.side_b = 0;

    for (int i = 0; i < tries; i++) {
        auto terminals = pick_terminals_two_sweep(info, i);
        int s = terminals.first.id;
        int t = terminals.second.id;

        int max_thresh = 5;  // adaptive, up to 5 distance levels
        auto result = find_balanced_cut(info, s, t, n_vars, n_cls, max_thresh);

        std::cout << "  Try " << i << ": s=V" << s << " t=V" << t
                  << " thresh=" << result.threshold
                  << " sep=" << result.sep_size
                  << " (" << result.n_var << "V+" << result.n_cls << "C)"
                  << " sides=" << result.side_a << "/" << result.side_b
                  << std::endl;

        if (result.side_b > overall_best.side_b ||
            (result.side_b == overall_best.side_b && result.sep_size < overall_best.sep_size)) {
            overall_best = result;
        }
    }

    std::cout << "\nBest overall:" << std::endl;
    std::cout << "  Separator: " << overall_best.sep_size
              << " (" << overall_best.n_var << "V+" << overall_best.n_cls << "C)"
              << " sides=" << overall_best.side_a << "/" << overall_best.side_b
              << " threshold=" << overall_best.threshold
              << std::endl;

    return 0;
}
